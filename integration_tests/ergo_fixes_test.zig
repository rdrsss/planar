//! integration_tests/ergo_fixes_test.zig
//!
//! CLI-ergonomics regression tests for three findings from the
//! introspection pass (tasks 3878, 3879, 3880):
//!
//!   3878 — `task update` must accept `--editor` as a no-op (not UnknownFlag).
//!   3879 — SchemaVersionAhead error must carry a concrete recovery hint.
//!   3880 — cross-scope guard error must name the `--scope` override.

const std = @import("std");
const harness = @import("harness");

const IDJSON = struct { id: i64 };
const TaskJSON = struct {
    id: i64,
    status: []const u8 = "",
};

// ---------------------------------------------------------------------------
// Fix 3878: `task update --editor=false ...` must exit 0 (no UnknownFlag).
// ---------------------------------------------------------------------------

test "3878: task update accepts --editor flag as no-op and applies the change" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Create a plan and task.
    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "3878-plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",          "add",            "--json", "--plan",       plan_id,
        "--next-action", "initial action", "--body", "initial body", "3878-task",
    });
    const task_id = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    // `task update --editor=false --status doing` must succeed (exit 0).
    // Before fix: UnknownFlag -> non-zero exit.
    const out = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "update", "--json", "--editor=false", "--status", "doing", task_id,
    });
    try std.testing.expectEqualStrings("doing", out.status);
}

test "3878: task update --editor (bare, no value) is also accepted as no-op" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "3878b-plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",          "add",            "--json",     "--plan", plan_id,
        "--next-action", "initial action", "3878b-task",
    });
    const task_id = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    // --editor without a value (defaults to false via default=.{.bool=false}).
    // Must succeed and apply --next-action.
    const out = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "update", "--json", "--editor", "--next-action", "updated action", task_id,
    });
    try std.testing.expectEqual(task.id, out.id);
}

// ---------------------------------------------------------------------------
// Fix 3879: SchemaVersionAhead error must include a concrete recovery hint.
// ---------------------------------------------------------------------------

test "3879: SchemaVersionAhead error message contains rebuild/reinstall recovery hint" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Bootstrap the DB to a valid migrated state via `planar init`.
    const init_out = suite.mustRun(&.{ "init", "--allow-no-repo", "--name", "3879-hint-proj" });
    gpa.free(init_out);

    // Inject a synthetic future version row via sqlite3. Skip gracefully when
    // sqlite3 is not on PATH (mirrors the watch-test synthetic-chain pattern).
    const future_sql =
        "insert into schema_migrations (version, description) values (99999, 'synthetic-future');";
    const inject_res = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, future_sql },
    }) catch |e| {
        std.debug.print("3879: sqlite3 not available ({s}); skipping\n", .{@errorName(e)});
        return error.SkipZigTest;
    };
    defer gpa.free(inject_res.stdout);
    defer gpa.free(inject_res.stderr);
    if (inject_res.term != .exited or inject_res.term.exited != 0) {
        std.debug.print("3879: sqlite3 inject non-zero: {any}\n", .{inject_res.term});
        @panic("3879: sqlite3 failed to inject synthetic schema version");
    }

    // Now any planning verb must fail with the SchemaVersionAhead message.
    // The error text must include the recovery hint phrase.
    const res = suite.exec(&.{ "task", "list" });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // Must exit non-zero (exit code 7 for SchemaVersionAhead).
    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
    // Must include the recovery hint.
    try std.testing.expect(
        std.mem.containsAtLeast(u8, res.stderr, 1, "rebuild/reinstall"),
    );
    // Must still name the DB version so the operator can correlate.
    try std.testing.expect(
        std.mem.containsAtLeast(u8, res.stderr, 1, "99999"),
    );
}

// ---------------------------------------------------------------------------
// Fix 3880: cross-scope guard error must name the --scope override.
// ---------------------------------------------------------------------------

test "3880: cross-scope guard error names --scope override in the message" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Two projects, each bound to its own association.
    _ = suite.registerProject("scope-a");
    const proj_a = suite.tmpAbsPath();
    const proj_b = suite.freshSystemTmpDir();
    const init_b = suite.mustRunInDir(proj_b, &.{ "init", "--allow-no-repo", "--name", "guard-b-3880" });
    gpa.free(init_b);

    const cr_a = suite.mustRun(&.{ "assoc", "create", "guard-3880-a", "--kind", "org" });
    gpa.free(cr_a);
    const cr_b = suite.mustRun(&.{ "assoc", "create", "guard-3880-b", "--kind", "org" });
    gpa.free(cr_b);
    const add_a = suite.mustRun(&.{ "assoc", "add", "guard-3880-a", proj_a });
    gpa.free(add_a);
    const add_b = suite.mustRun(&.{ "assoc", "add", "guard-3880-b", proj_b });
    gpa.free(add_b);

    // Seed a task in assoc-b's scope.
    const plan_b = suite.mustRunJSON(IDJSON, arena, &.{
        "plan", "create", "--json", "--scope", "guard-3880-b", "3880-plan-b",
    });
    const plan_b_id = std.fmt.allocPrint(arena, "{d}", .{plan_b.id}) catch @panic("OOM");

    const task_b = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",         "add",           "--json",
        "--plan",       plan_b_id,       "--scope",
        "guard-3880-b", "--next-action", "trigger guard",
        "3880-task-b",
    });
    const task_b_id = std.fmt.allocPrint(arena, "{d}", .{task_b.id}) catch @panic("OOM");

    // From proj_a's cwd (resolves to guard-3880-a), update the assoc-b task.
    // Must fail with scope mismatch; the error must mention "--scope".
    const stderr = suite.expectFailureInDir(proj_a, &.{
        "task", "update", "--status", "doing", task_b_id,
    });
    defer gpa.free(stderr);

    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "scope mismatch"));
    // The hint must name the --scope flag explicitly.
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "--scope"));
    // The entity slug must appear so the operator knows what to pass.
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "guard-3880-b"));
}
