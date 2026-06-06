//! integration_tests/task_transitions_test.zig
//!
//! Pins the `task block` and `task reopen` status-transition contract at
//! the CLI layer. Both verbs had engine unit tests but no black-box
//! integration coverage, so a handler-level regression (arg parsing,
//! scope resolution, JSON shape, status flip) would pass
//! `make test-integration` silently.
//!
//! Contract pinned:
//!   - `task block <id> --on <blocker> --json` flips status to "blocked"
//!     and returns the full task object.
//!   - `task reopen <id> --json` on a done task flips status back to
//!     "todo" (the default reopen target).
//!   - `task show <id> --json` reflects each transition (exit 0 alone is
//!     not sufficient — a silent no-op is still exit 0).
//!
//! NOTE: `cli-reference.md` claims `task reopen` writes a `task_reopens`
//! row, but the engine does not yet do so — this test deliberately
//! asserts only the status contract, not a `task_reopens` insert.
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64, title: []const u8, status: []const u8 };
const TaskJSON = struct { id: i64, title: []const u8, status: []const u8 };

test "scenario: task block then reopen — status transitions round-trip" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("task-transitions");

    // ---- 1. Plan + two tasks (one to block, one to be the blocker).
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Transitions plan",
    });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--plan", plan_id, "--next-action", "do the work", "Primary task",
    });
    const blocker = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--plan", plan_id, "--next-action", "unblock", "Blocking task",
    });
    try std.testing.expectEqualStrings("todo", task.status);

    const task_id = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;
    const blocker_id = std.fmt.allocPrint(arena, "{d}", .{blocker.id}) catch unreachable;

    // ---- 2. Block the primary task on the blocker.
    const blocked = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "block", task_id, "--on", blocker_id, "--reason", "waiting on blocker", "--json",
    });
    try std.testing.expectEqualStrings("blocked", blocked.status);

    // Post-state must reflect the block (not just the verb's own echo).
    const after_block = suite.mustRunJSON(TaskJSON, arena, &.{ "task", "show", task_id, "--json" });
    try std.testing.expectEqualStrings("blocked", after_block.status);

    // ---- 3. Drive the blocker to done, then reopen it.
    gpa.free(suite.mustRun(&.{ "task", "done", blocker_id }));
    const after_done = suite.mustRunJSON(TaskJSON, arena, &.{ "task", "show", blocker_id, "--json" });
    try std.testing.expectEqualStrings("done", after_done.status);

    const reopened = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "reopen", blocker_id, "--reason", "needs rework", "--json",
    });
    try std.testing.expectEqualStrings("todo", reopened.status);

    const after_reopen = suite.mustRunJSON(TaskJSON, arena, &.{ "task", "show", blocker_id, "--json" });
    try std.testing.expectEqualStrings("todo", after_reopen.status);
}
