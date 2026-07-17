//! integration_tests/health_hygiene_test.zig — lifecycle-drift reporter.
//!
//! Realistic operator workflow for task 744: create plans, tasks, and
//! questions through their public verbs, then inspect the read-only hygiene
//! report in JSON and text forms. Zero-day thresholds make freshly-created
//! rows deterministic fixtures without bypassing the CLI to rewrite clocks.

const std = @import("std");
const harness = @import("harness");

const Id = struct { id: i64 };

const TaskCounts = struct {
    todo: i64,
    doing: i64,
    blocked: i64,
    done: i64,
    cancelled: i64,
};

const StalePlan = struct {
    id: i64,
    title: []const u8,
    reason: []const u8,
    task_counts: TaskCounts,
    suggestion: []const u8,
};

const StaleTask = struct {
    id: i64,
    plan_id: i64,
    title: []const u8,
    age_days: i64,
    suggestion: []const u8,
};

const StaleQuestion = struct {
    id: i64,
    title: []const u8,
    age_days: i64,
    suggestion: []const u8,
};

const Thresholds = struct {
    stale_doing_days: i64,
    stale_open_days: i64,
};

const HygieneReport = struct {
    thresholds: Thresholds,
    stale_draft_plans: []StalePlan,
    stale_doing_tasks: []StaleTask,
    stale_open_questions: []StaleQuestion,
};

fn findPlan(rows: []const StalePlan, id: i64) ?StalePlan {
    for (rows) |row| if (row.id == id) return row;
    return null;
}

fn findTask(rows: []const StaleTask, id: i64) ?StaleTask {
    for (rows) |row| if (row.id == id) return row;
    return null;
}

fn findQuestion(rows: []const StaleQuestion, id: i64) ?StaleQuestion {
    for (rows) |row| if (row.id == id) return row;
    return null;
}

test "health hygiene reports scoped lifecycle drift with actionable JSON and text" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("hygiene-main");

    const wrapper = suite.mustRunJSON(Id, arena, &.{
        "plan", "create", "--json", "Empty draft wrapper",
    });

    const terminal = suite.mustRunJSON(Id, arena, &.{
        "plan", "create", "--json", "Finished draft plan",
    });
    const terminal_id = try std.fmt.allocPrint(arena, "{d}", .{terminal.id});
    const done_task = suite.mustRunJSON(Id, arena, &.{
        "task", "add", "--plan", terminal_id, "--no-auto-promote", "--json", "Already shipped",
    });
    const done_task_id = try std.fmt.allocPrint(arena, "{d}", .{done_task.id});
    gpa.free(suite.mustRun(&.{ "task", "update", done_task_id, "--status", "doing", "--no-auto-promote" }));
    gpa.free(suite.mustRun(&.{ "task", "update", done_task_id, "--status", "done", "--no-auto-promote" }));

    const active = suite.mustRunJSON(Id, arena, &.{
        "plan", "create", "--status", "active", "--json", "Active delivery",
    });
    const active_id = try std.fmt.allocPrint(arena, "{d}", .{active.id});
    const doing_task = suite.mustRunJSON(Id, arena, &.{
        "task", "add", "--plan", active_id, "--json", "Long-running implementation",
    });
    const doing_task_id = try std.fmt.allocPrint(arena, "{d}", .{doing_task.id});
    gpa.free(suite.mustRun(&.{ "task", "update", doing_task_id, "--status", "doing" }));

    const open_question = suite.mustRunJSON(Id, arena, &.{
        "question", "add", "--json", "Unresolved compatibility question",
    });
    const excluded_question = suite.mustRunJSON(Id, arena, &.{
        "question", "add", "--scope", "global", "--json", "Excluded global question",
    });

    const report = suite.mustRunJSON(HygieneReport, arena, &.{
        "health",        "hygiene", "--scope",      "project:hygiene-main",
        "--stale-doing", "0",       "--stale-open", "0",
        "--json",
    });
    try std.testing.expectEqual(@as(i64, 0), report.thresholds.stale_doing_days);
    try std.testing.expectEqual(@as(i64, 0), report.thresholds.stale_open_days);

    const wrapper_row = findPlan(report.stale_draft_plans, wrapper.id) orelse
        return error.TestExpectedEqual;
    try std.testing.expectEqualStrings("zero_tasks", wrapper_row.reason);
    try std.testing.expectEqual(@as(i64, 0), wrapper_row.task_counts.done);
    try std.testing.expect(std.mem.indexOf(u8, wrapper_row.suggestion, "--status abandoned") != null);

    const terminal_row = findPlan(report.stale_draft_plans, terminal.id) orelse
        return error.TestExpectedEqual;
    try std.testing.expectEqualStrings("all_tasks_terminal", terminal_row.reason);
    try std.testing.expectEqual(@as(i64, 1), terminal_row.task_counts.done);
    try std.testing.expect(std.mem.indexOf(u8, terminal_row.suggestion, "--status done") != null);

    const task_row = findTask(report.stale_doing_tasks, doing_task.id) orelse
        return error.TestExpectedEqual;
    try std.testing.expectEqual(active.id, task_row.plan_id);
    try std.testing.expect(task_row.age_days >= 0);
    try std.testing.expect(std.mem.indexOf(u8, task_row.suggestion, "--status blocked") != null);

    const question_row = findQuestion(report.stale_open_questions, open_question.id) orelse
        return error.TestExpectedEqual;
    try std.testing.expect(question_row.age_days >= 0);
    try std.testing.expect(std.mem.indexOf(u8, question_row.suggestion, "question answer") != null);
    try std.testing.expect(findQuestion(report.stale_open_questions, excluded_question.id) == null);

    const text_report = suite.mustRun(&.{
        "health",        "hygiene", "--scope",      "project:hygiene-main",
        "--stale-doing", "0",       "--stale-open", "0",
    });
    defer gpa.free(text_report);
    try std.testing.expect(std.mem.indexOf(u8, text_report, "Stale draft plans") != null);
    try std.testing.expect(std.mem.indexOf(u8, text_report, "planar plan update") != null);
    try std.testing.expect(std.mem.indexOf(u8, text_report, "Stale doing tasks") != null);
    try std.testing.expect(std.mem.indexOf(u8, text_report, "Stale open questions") != null);
}
