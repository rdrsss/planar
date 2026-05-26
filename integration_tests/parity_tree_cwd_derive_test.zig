//! integration_tests/parity_tree_cwd_derive_test.zig
//!
//! Cluster E anchor tests for plan 351 (parity-gap analysis +
//! regression tests). See scripts/parity-data/parity-triage.md
//! §E-tree-cwd-derive (commit 018a190) for the triage record and
//! task 2375 for the root-cause analysis.
//!
//! These tests pin the INTENDED behavior (matches Go binary; operator
//! decision 2026-05-26 selected option a: `init` stores the literal
//! cwd, no realPath). They will FAIL RED until task 2375's fix lands;
//! the failure is the contract that Phase 4 turns green.
//!
//! Why they need `freshSystemTmpDir` rather than the suite's standard
//! tmp_dir: the bug is in `init`'s realPath() call vs `assoc add`'s
//! literal-path storage. On macOS `/var/folders/...` symlinks to
//! `/private/var/folders/...`; the resulting projects-table row pair
//! breaks cwd-derive. The harness's standard `tmpAbsPath` already calls
//! realPath() (see harness.zig:98-106) so a test using it would not
//! reproduce the operator scenario — and the existing
//! cwd_scope_test.zig passes today for exactly that reason. Tests
//! here use `freshSystemTmpDir()` which returns a literal mktemp path.

const std = @import("std");
const harness = @import("harness");

test "parity: tree from literal-path registered project resolves to assoc (Cluster E text, gap-report row 148; red until task 2375)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = suite.freshSystemTmpDir();

    // Fixture: register the literal-path tmp dir as a Planar project,
    // create the association, bind the literal path to it. Matches the
    // operator sequence from scripts/parity-audit.sh and the task 2375
    // reproduction.
    const init_out = suite.mustRunInDir(root, &.{
        "init", "--allow-no-repo", "--name", "parity-e-text-proj",
    });
    gpa.free(init_out);

    const create_out = suite.mustRun(&.{
        "assoc", "create", "parity-e-text-fixture", "--kind", "project",
    });
    gpa.free(create_out);

    const add_out = suite.mustRun(&.{
        "assoc", "add", "parity-e-text-fixture", root,
    });
    gpa.free(add_out);

    // Act: run `planar tree` from inside the literal-path tmp dir.
    const stdout = suite.mustRunInDir(root, &.{"tree"});
    defer gpa.free(stdout);

    // Assert: stdout's first line is the assoc label, matching Go.
    // Today this fails because zig's init realpaths cwd → two project
    // rows → assoc binds to wrong row → cwd-derive returns "global".
    try std.testing.expect(std.mem.containsAtLeast(u8, stdout, 1, "assoc:parity-e-text-fixture"));

    // Negative assertion: guard against false positives where the slug
    // leaks via some other render path but the root label remains global.
    const first_line_end = std.mem.indexOfScalar(u8, stdout, '\n') orelse stdout.len;
    const first_line = std.mem.trim(u8, stdout[0..first_line_end], " \t\r");
    try std.testing.expect(!std.mem.eql(u8, first_line, "global"));
}

test "parity: tree --json from literal-path registered project emits scope-clean node (Cluster E json, gap-report row 149; red until task 2375)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const root = suite.freshSystemTmpDir();

    const init_out = suite.mustRunInDir(root, &.{
        "init", "--allow-no-repo", "--name", "parity-e-json-proj",
    });
    gpa.free(init_out);

    const create_out = suite.mustRun(&.{
        "assoc", "create", "parity-e-json-fixture", "--kind", "project",
    });
    gpa.free(create_out);

    const add_out = suite.mustRun(&.{
        "assoc", "add", "parity-e-json-fixture", root,
    });
    gpa.free(add_out);

    const stdout = suite.mustRunInDir(root, &.{ "tree", "--json" });
    defer gpa.free(stdout);

    const trimmed = std.mem.trim(u8, stdout, " \n");
    const parsed = std.json.parseFromSlice(std.json.Value, arena, trimmed, .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print(
            "\ntree --json: failed to parse output: {s}\nraw: {s}\n",
            .{ @errorName(e), stdout },
        );
        try std.testing.expect(false);
        unreachable;
    };

    try std.testing.expect(parsed.value == .object);
    const obj = parsed.value.object;

    // Positive: scope_label matches the assoc (Go behavior; option a).
    // Today fails because cwd-derive returns global → scope_label="global".
    const scope_label = obj.get("scope_label") orelse {
        std.debug.print("\nscope-node missing 'scope_label' field; raw: {s}\n", .{stdout});
        try std.testing.expect(false);
        unreachable;
    };
    try std.testing.expect(scope_label == .string);
    try std.testing.expectEqualStrings("assoc:parity-e-json-fixture", scope_label.string);

    // Negative: scope nodes must not carry entity-only fields. Go's
    // shape is { kind, title, scope_kind, scope_id, scope_label,
    // children }. Zig today emits id:0 plus slug/status/priority/
    // artifact_kind/created_at/updated_at as zero-valued — pollution.
    const polluted_keys = [_][]const u8{
        "id",
        "slug",
        "status",
        "priority",
        "artifact_kind",
        "created_at",
        "updated_at",
    };
    for (polluted_keys) |key| {
        if (obj.get(key) != null) {
            std.debug.print(
                "\nscope-node has unexpected entity-only field '{s}'; raw: {s}\n",
                .{ key, stdout },
            );
            try std.testing.expect(false);
        }
    }
}
