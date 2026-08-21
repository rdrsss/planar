//! integration_tests/closure_compute_show_test.zig — black-box tests for the
//! `planar closure {compute, show}` verb group (M2.5 of plan 636).
//!
//! The tests drive the full extractor pipeline through the compiled binary:
//! register a project rooted at the suite tmp dir, write a fixture `.zig`
//! corpus into that root, declare a task's seed via
//! `task touches add --path`, run `closure compute`, then assert the
//! persisted rows via `closure show --json`.
//!
//! Asserted contract:
//!   - rows are written with roles modify / reference / transitive.
//!   - the seed's own decls appear with role 'modify' and token_weight > 0.
//!   - a cross-file reference appears with role 'reference'.
//!   - a reference-of-a-reference appears with role 'transitive'.
//!   - every row carries the recorded extractor_version.
//!   - the path column is populated (the M2.2 caveat: path disambiguates
//!     same-stem files; without it the stem-only symbol would collide).
//!   - recompute is idempotent (row count is stable across two runs).
//!
//! Negative path: `closure compute` on a task with no declared touches
//! exits non-zero (NoSeeds).

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64 };
const TaskAddJSON = struct { id: i64 };

const Row = struct {
    id: i64 = 0,
    repo_id: i64 = 0,
    path: []const u8 = "",
    symbol: []const u8 = "",
    role: []const u8 = "",
    token_weight: i64 = 0,
    extractor_version: []const u8 = "",
    created_at: []const u8 = "",
};

const ShowJSON = struct {
    task_id: i64 = 0,
    rows: []const Row = &.{},
};

const ComputeJSON = struct {
    task_id: i64 = 0,
    seeds: usize = 0,
    modify: usize = 0,
    reference: usize = 0,
    transitive: usize = 0,
    rows_written: usize = 0,
    extractor_version: []const u8 = "",
};

/// Write `data` to `<tmp>/<rel>`, creating parent dirs.
fn writeFixture(suite: *harness.Suite, rel: []const u8, data: []const u8) void {
    const gpa = suite.allocator;
    const root = suite.tmpAbsPath();
    const abs = std.fs.path.join(gpa, &.{ root, rel }) catch @panic("OOM");
    defer gpa.free(abs);
    if (std.fs.path.dirname(abs)) |dir| {
        std.Io.Dir.cwd().createDirPath(std.testing.io, dir) catch |e|
            std.debug.panic("createDirPath {s}: {s}", .{ dir, @errorName(e) });
    }
    std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = abs, .data = data }) catch |e|
        std.debug.panic("writeFile {s}: {s}", .{ abs, @errorName(e) });
}

/// Register a project at the suite tmp + an org assoc; return the repo slug.
fn registerRepoSlug(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    _ = suite.registerProject("cl-repo");
    const cr = suite.mustRun(&.{ "assoc", "create", "cl-org", "--kind", "org" });
    suite.allocator.free(cr);
    const root = suite.tmpAbsPath();
    const ad = suite.mustRun(&.{ "assoc", "add", "cl-org", root });
    suite.allocator.free(ad);

    const Member = struct { id: i64, slug: []const u8, name: []const u8 };
    const members = suite.mustRunJSON([]Member, arena, &.{ "assoc", "members", "cl-org", "--json" });
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "cl-repo")) return m.slug;
    }
    std.debug.panic("registered repo slug not found", .{});
}

fn touchPath(suite: *harness.Suite, repo_slug: []const u8, task_id: i64, path: []const u8) void {
    const gpa = suite.allocator;
    const tid = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch unreachable;
    defer gpa.free(tid);
    const out = suite.mustRun(&.{ "task", "touches", "add", tid, repo_slug, "--path", path });
    gpa.free(out);
}

fn rowByRole(rows: []const Row, role: []const u8, symbol: []const u8) ?Row {
    for (rows) |r| {
        if (std.mem.eql(u8, r.role, role) and std.mem.eql(u8, r.symbol, symbol)) return r;
    }
    return null;
}

test "closure compute → show: role-partitioned rows with weights + extractor_version" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);

    // --- Fixture corpus under the project root ---------------------------
    // ee.zig: transform() calls helper() — a reference-of-a-reference for the
    // seed, so ee.helper must surface as role='transitive'.
    writeFixture(&suite,
        \\src/ee.zig
    ,
        \\pub fn transform(n: u32) u32 {
        \\    return helper(n) + 1;
        \\}
        \\fn helper(n: u32) u32 {
        \\    return n * 2;
        \\}
        \\
    );
    // seed.zig: the task's modify file. run() calls ee.transform → reference;
    // helper2 → reference (same-file). std.debug.print is unresolved (dropped).
    writeFixture(&suite,
        \\src/seed.zig
    ,
        \\const std = @import("std");
        \\const ee = @import("ee.zig");
        \\pub fn run() u32 {
        \\    const a = ee.transform(3);
        \\    const b = helper2(a);
        \\    std.debug.print("x", .{});
        \\    return a + b;
        \\}
        \\fn helper2(n: u32) u32 { return n; }
        \\
    );

    // --- Plan + task + seed declaration ---------------------------------
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "cl-plan", "--json", "CL_PLAN",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "closure seed task",
    });
    touchPath(&suite, repo, task.id, "src/seed.zig");

    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    // --- compute --------------------------------------------------------
    const comp = suite.mustRunJSON(ComputeJSON, arena, &.{ "closure", "compute", tid, "--json" });
    try std.testing.expectEqual(@as(usize, 1), comp.seeds);
    try std.testing.expect(comp.rows_written >= 3);
    try std.testing.expect(comp.modify >= 2); // seed.std, seed.ee, seed.run, seed.helper2
    try std.testing.expect(comp.reference >= 1);
    try std.testing.expect(comp.transitive >= 1);
    try std.testing.expectEqualStrings("m2-closure-0.1", comp.extractor_version);

    // --- show --json: assert the persisted rows --------------------------
    const shown = suite.mustRunJSON(ShowJSON, arena, &.{ "closure", "show", tid, "--json" });
    try std.testing.expectEqual(task.id, shown.task_id);
    try std.testing.expect(shown.rows.len >= 3);

    // modify: the seed's own run() — present, weight > 0, path populated.
    const run_row = rowByRole(shown.rows, "modify", "seed.run") orelse
        std.debug.panic("expected modify row seed.run", .{});
    try std.testing.expect(run_row.token_weight > 0);
    try std.testing.expectEqualStrings("src/seed.zig", run_row.path);
    try std.testing.expectEqualStrings("m2-closure-0.1", run_row.extractor_version);

    // reference: ee.transform resolved cross-file, weighted from ee.zig.
    const ref_row = rowByRole(shown.rows, "reference", "ee.transform") orelse
        std.debug.panic("expected reference row ee.transform", .{});
    try std.testing.expect(ref_row.token_weight > 0);
    try std.testing.expectEqualStrings("src/ee.zig", ref_row.path);

    // transitive: ee.helper (reference-of-a-reference) — stored, marked.
    const trans_row = rowByRole(shown.rows, "transitive", "ee.helper") orelse
        std.debug.panic("expected transitive row ee.helper", .{});
    try std.testing.expectEqualStrings("src/ee.zig", trans_row.path);

    // stdlib was NOT credited — no row's symbol starts with "std.".
    for (shown.rows) |r| {
        try std.testing.expect(!std.mem.startsWith(u8, r.symbol, "std."));
    }

    // --- recompute is idempotent: same row count, no UNIQUE collision ----
    const comp2 = suite.mustRunJSON(ComputeJSON, arena, &.{ "closure", "compute", tid, "--json" });
    try std.testing.expectEqual(comp.rows_written, comp2.rows_written);
    const shown2 = suite.mustRunJSON(ShowJSON, arena, &.{ "closure", "show", tid, "--json" });
    try std.testing.expectEqual(shown.rows.len, shown2.rows.len);
}

test "closure compute: task with no declared touches exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "cl-empty", "--json", "CL_EMPTY",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "no seeds",
    });
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    const err = suite.expectFailure(&.{ "closure", "compute", tid });
    gpa.free(err);
}

test "closure show: empty for an uncomputed task" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "cl-none", "--json", "CL_NONE",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "uncomputed",
    });
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    const shown = suite.mustRunJSON(ShowJSON, arena, &.{ "closure", "show", tid, "--json" });
    try std.testing.expectEqual(task.id, shown.task_id);
    try std.testing.expectEqual(@as(usize, 0), shown.rows.len);
}
