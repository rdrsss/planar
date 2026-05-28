//! integration_tests/scenarios/scenario_worktree_scope_test.zig
//!
//! Plan 297 M3 scenario: engine enforcement of the worktree scope
//! rule (refer to `docs/.../142-tech-spec § Scope handling for
//! worktrees` and the methodology section just landed by M2). The
//! contract being pinned here:
//!
//!  - From inside a worktree, reads ARE allowed and resolve to the
//!    parent repo's scope.
//!  - From inside a worktree, planning verbs are REFUSED with exit
//!    code 8 and a stderr block that names the cwd, the parent repo
//!    root, the reason, and the suggested remediation.
//!  - `--scope <slug>` does NOT override the refusal.
//!  - The authoritative-fallback detection path (worktree at a
//!    non-`.worktrees/` location) still triggers the refusal.
//!  - `task done` is planning-class (coders must use
//!    `planar-agent complete`).

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

// Helper: skip the test if `git` is not on PATH. Returns true when
// git is available.
fn gitAvailable(gpa: std.mem.Allocator) bool {
    const r = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "git", "--version" },
    }) catch return false;
    defer gpa.free(r.stdout);
    defer gpa.free(r.stderr);
    return switch (r.term) {
        .exited => |code| code == 0,
        else => false,
    };
}

// Helper: run a git command, panicking on failure (tests treat git as
// a hard dependency once `gitAvailable` has cleared).
fn mustGit(gpa: std.mem.Allocator, cwd: []const u8, args: []const []const u8) void {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    argv.append(gpa, "git") catch @panic("OOM");
    argv.append(gpa, "-C") catch @panic("OOM");
    argv.append(gpa, cwd) catch @panic("OOM");
    for (args) |a| argv.append(gpa, a) catch @panic("OOM");

    const r = std.process.run(gpa, std.testing.io, .{ .argv = argv.items }) catch
        @panic("mustGit: spawn failed");
    defer gpa.free(r.stdout);
    defer gpa.free(r.stderr);
    switch (r.term) {
        .exited => |code| if (code != 0) {
            std.debug.print(
                "\nmustGit failed (exit {d}) in {s}\nargs: {s}\nstderr: {s}\n",
                .{ code, cwd, args[0], r.stderr },
            );
            @panic("mustGit: non-zero exit");
        },
        else => @panic("mustGit: abnormal termination"),
    }
}

// ============================================================================
// Scenario 1: convention path `.worktrees/<plan>/<task>/` — fast path
// ============================================================================

test "scenario: worktree scope (convention path) — reads pass, planning verbs refused" {
    const gpa = std.testing.allocator;
    if (!gitAvailable(gpa)) return error.SkipZigTest;

    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // The fast-path detection in `engine.identity.scope` requires the
    // cwd to contain `.worktrees/<segment>/` literally. The harness's
    // own tmp_dir lives under `.zig-cache/tmp/...` which (in this
    // worktree-management checkout) is itself nested under
    // `.worktrees/...`. To exercise the fast-path against a clean
    // fixture, build the project root at a system tmp via
    // `freshSystemTmpDir`.
    const proj_root = suite.freshSystemTmpDir();

    // Initialize the project root as a git repo so `git worktree
    // add` works against it.
    mustGit(gpa, proj_root, &.{"init"});
    mustGit(gpa, proj_root, &.{ "config", "user.email", "test@example.com" });
    mustGit(gpa, proj_root, &.{ "config", "user.name", "Test" });
    mustGit(gpa, proj_root, &.{ "commit", "--allow-empty", "-m", "init" });

    // Register the proj_root as a Planar project + bind it to an
    // association so cwd-derive from inside it (and inside its
    // worktrees) resolves a real scope.
    const init_out = suite.mustRunInDir(proj_root, &.{
        "init", "--allow-no-repo", "--name", "wt-scope-fixture",
    });
    gpa.free(init_out);

    const assoc_slug = "wt-fixture-scope";
    const create_out = suite.mustRun(&.{
        "assoc", "create", assoc_slug, "--kind", "project",
    });
    gpa.free(create_out);
    const add_out = suite.mustRun(&.{ "assoc", "add", assoc_slug, proj_root });
    gpa.free(add_out);

    // Create a plan so we have something to `plan show` later.
    const plan_out = suite.mustRunInDir(proj_root, &.{
        "plan", "create", "Anchor plan", "--summary", "anchor",
    });
    gpa.free(plan_out);

    // Fetch the plan via list --json so we have its id without
    // depending on `plan create` output format.
    const list_raw = suite.mustRunInDir(proj_root, &.{ "plan", "list", "--json" });
    defer gpa.free(list_raw);
    const plans_parsed = std.json.parseFromSlice([]PlanJSON, arena, list_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nplan list parse failed: {s}\nraw: {s}\n", .{ @errorName(e), list_raw });
        @panic("plan list JSON decode");
    };
    try std.testing.expect(plans_parsed.value.len >= 1);
    const plan_id = plans_parsed.value[0].id;

    // Make the worktree at the convention path:
    // <proj_root>/.worktrees/<plan>/<task>/
    const wt_root = std.fs.path.join(arena, &.{
        proj_root, ".worktrees", "297-worktree-scope", "scope-resolver-tests",
    }) catch @panic("OOM");
    // git worktree add needs the parent dir to exist for nested paths;
    // mkdir -p the path's parent via shell. We accept whatever exit
    // status mkdir returns — git's failure mode will be clearer if
    // the path is actually wrong.
    const wt_parent = std.fs.path.dirname(wt_root).?;
    const mk = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "mkdir", "-p", wt_parent },
    }) catch @panic("mkdir -p spawn");
    gpa.free(mk.stdout);
    gpa.free(mk.stderr);
    mustGit(gpa, proj_root, &.{
        "worktree", "add", "-b", "297/scope-resolver-tests", wt_root,
    });

    // ---- READ from worktree: succeeds, returns parent scope.
    //
    // Reads are NOT gated. The harness's default PLANAR_DISABLE_WORKTREE_GATE=1
    // makes this work for the harness's own tmp paths too, but here
    // the cwd IS a real worktree — the read still resolves because
    // the gate's classification table marks `plan list / show` as
    // execution_or_read.
    const wt_list_raw = suite.mustRunInDir(wt_root, &.{ "plan", "list", "--json" });
    defer gpa.free(wt_list_raw);
    const wt_plans = std.json.parseFromSlice([]PlanJSON, arena, wt_list_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nwt plan list parse: {s}\nraw: {s}\n", .{ @errorName(e), wt_list_raw });
        @panic("wt plan list JSON");
    };
    try std.testing.expect(wt_plans.value.len >= 1);

    // `plan show <id> --json` from worktree should return the parent's plan.
    const id_str = try std.fmt.allocPrint(arena, "{d}", .{plan_id});
    const wt_show = suite.mustRunInDir(wt_root, &.{ "plan", "show", id_str, "--json" });
    defer gpa.free(wt_show);
    const wt_plan = std.json.parseFromSlice(PlanJSON, arena, wt_show, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nwt plan show parse: {s}\nraw: {s}\n", .{ @errorName(e), wt_show });
        @panic("wt plan show JSON");
    };
    try std.testing.expectEqual(plan_id, wt_plan.value.id);

    // ---- WRITE from worktree: refused with exit 8 + 4-line block.
    //
    // For the refusal-path assertions we explicitly UN-set
    // PLANAR_DISABLE_WORKTREE_GATE (the harness's default suppresses
    // the gate to avoid breaking unrelated scenarios). The gate
    // treats an empty value as "not set."

    const ungate = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DISABLE_WORKTREE_GATE", .value = "" },
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = wt_root },
    };

    const refusals = [_][]const []const u8{
        &.{ "plan", "create", "should-be-refused" },
        &.{ "task", "add", "should-be-refused", "--plan", id_str },
        &.{ "question", "add", "should-be-refused" },
        &.{ "artifact", "update", "1", "--status", "active" },
        // task done is planning-class (coders use planar-agent complete).
        &.{ "task", "done", "1" },
        // --scope does NOT override the refusal.
        &.{ "plan", "create", "should-be-refused", "--scope", assoc_slug },
    };

    for (refusals) |args| {
        const res = suite.execWithInDir(wt_root, args, &ungate);
        defer gpa.free(res.stdout);
        defer gpa.free(res.stderr);
        if (res.term == .exited and res.term.exited == 0) {
            std.debug.print(
                "\nrefusal expected for args[0]={s} but exited 0\nstdout:{s}\nstderr:{s}\n",
                .{ args[0], res.stdout, res.stderr },
            );
            try std.testing.expect(false);
        }
        // Exit-code contract: 8 is the worktree-refusal sentinel.
        try std.testing.expectEqual(@as(u8, 8), res.term.exited);
        // The refusal block has four labelled lines. Assert on label
        // words; the exact whitespace is the contract but the labels
        // matter most.
        try expectContains(res.stderr, "may not run from inside a worktree");
        try expectContains(res.stderr, "cwd:");
        try expectContains(res.stderr, "parent:");
        try expectContains(res.stderr, "reason:");
        try expectContains(res.stderr, "suggestion:");
        try expectContains(res.stderr, proj_root);
    }

    // Reads pass even with the gate enabled.
    const scope_show = suite.execWithInDir(wt_root, &.{ "scope", "show", "--json" }, &ungate);
    defer gpa.free(scope_show.stdout);
    defer gpa.free(scope_show.stderr);
    if (scope_show.term != .exited or scope_show.term.exited != 0) {
        std.debug.print("\nscope show from worktree failed\nstdout:{s}\nstderr:{s}\n", .{ scope_show.stdout, scope_show.stderr });
        try std.testing.expect(false);
    }
}

// ============================================================================
// Scenario 2: authoritative-fallback path (worktree at non-convention
// location) — fast path misses, `git rev-parse` detection still kicks in.
// ============================================================================

test "scenario: worktree scope (non-convention path) — fallback detects, planning refused" {
    const gpa = std.testing.allocator;
    if (!gitAvailable(gpa)) return error.SkipZigTest;

    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const proj_root = suite.freshSystemTmpDir();
    mustGit(gpa, proj_root, &.{"init"});
    mustGit(gpa, proj_root, &.{ "config", "user.email", "test@example.com" });
    mustGit(gpa, proj_root, &.{ "config", "user.name", "Test" });
    mustGit(gpa, proj_root, &.{ "commit", "--allow-empty", "-m", "init" });

    const init_out = suite.mustRunInDir(proj_root, &.{
        "init", "--allow-no-repo", "--name", "wt-fallback-fixture",
    });
    gpa.free(init_out);

    const assoc_slug = "wt-fallback-scope";
    const create_out = suite.mustRun(&.{
        "assoc", "create", assoc_slug, "--kind", "project",
    });
    gpa.free(create_out);
    const add_out = suite.mustRun(&.{ "assoc", "add", assoc_slug, proj_root });
    gpa.free(add_out);

    // Worktree at a NON-`.worktrees/` path: a sibling system tmpdir.
    // The fast-path will miss; only the authoritative fallback can
    // classify this as a worktree.
    const wt_root = suite.freshSystemTmpDir();
    // freshSystemTmpDir already created the dir; `git worktree add`
    // refuses to add into an existing non-empty dir, but our mktemp
    // dir is empty — should work. If git complains about the dir
    // already existing, retry with `--force` semantics by removing
    // and recreating.
    const rm = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "rmdir", wt_root },
    }) catch @panic("rmdir spawn");
    gpa.free(rm.stdout);
    gpa.free(rm.stderr);

    mustGit(gpa, proj_root, &.{
        "worktree", "add", "-b", "feat-fallback", wt_root,
    });

    // A planning verb run from the non-convention worktree path must
    // still be refused (proves the fallback path fires). Same
    // un-gate-via-env approach as scenario 1.
    const ungate = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DISABLE_WORKTREE_GATE", .value = "" },
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = wt_root },
    };
    const res = suite.execWithInDir(wt_root, &.{
        "plan", "create", "should-be-refused-by-fallback",
    }, &ungate);
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);
    if (res.term == .exited and res.term.exited == 0) {
        std.debug.print(
            "\nfallback refusal expected but exited 0\nstdout:{s}\nstderr:{s}\n",
            .{ res.stdout, res.stderr },
        );
        try std.testing.expect(false);
    }
    try std.testing.expectEqual(@as(u8, 8), res.term.exited);
    try expectContains(res.stderr, "may not run from inside a worktree");
    try expectContains(res.stderr, "parent:");
}

// ============================================================================
// Scenario 3: main worktree (the canonical checkout root) is NOT a
// worktree — planning verbs succeed from it.
// ============================================================================

test "scenario: main worktree (canonical checkout) — planning verbs succeed" {
    const gpa = std.testing.allocator;
    if (!gitAvailable(gpa)) return error.SkipZigTest;

    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const proj_root = suite.freshSystemTmpDir();
    mustGit(gpa, proj_root, &.{"init"});
    mustGit(gpa, proj_root, &.{ "config", "user.email", "test@example.com" });
    mustGit(gpa, proj_root, &.{ "config", "user.name", "Test" });
    mustGit(gpa, proj_root, &.{ "commit", "--allow-empty", "-m", "init" });

    const init_out = suite.mustRunInDir(proj_root, &.{
        "init", "--allow-no-repo", "--name", "wt-main-fixture",
    });
    gpa.free(init_out);

    const assoc_slug = "wt-main-scope";
    const create_out = suite.mustRun(&.{
        "assoc", "create", assoc_slug, "--kind", "project",
    });
    gpa.free(create_out);
    const add_out = suite.mustRun(&.{ "assoc", "add", assoc_slug, proj_root });
    gpa.free(add_out);

    // From the canonical checkout root, `plan create` should succeed
    // even with the gate enabled. Explicitly un-gate via env so the
    // test exercises the gate's "not a worktree" classification path,
    // not the harness's default suppression.
    const ungate = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DISABLE_WORKTREE_GATE", .value = "" },
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = proj_root },
    };
    const res = suite.execWithInDir(proj_root, &.{
        "plan", "create", "Plan from main worktree",
    }, &ungate);
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "\nplan create from main worktree should succeed but didn't\nstdout:{s}\nstderr:{s}\n",
            .{ res.stdout, res.stderr },
        );
        try std.testing.expect(false);
    }
}

// ============================================================================
// Helpers
// ============================================================================

fn expectContains(haystack: []const u8, needle: []const u8) !void {
    if (std.mem.indexOf(u8, haystack, needle) == null) {
        std.debug.print(
            "\nexpected stderr to contain {s}\n----- stderr -----\n{s}\n------------------\n",
            .{ needle, haystack },
        );
        try std.testing.expect(false);
    }
}
