const std = @import("std");
const harness = @import("harness");

const Plan = struct { id: i64 };
const Task = struct { id: i64 };
const Question = struct { id: i64 };
const Triage = struct {
    id: i64,
    finding: []const u8,
    plan_id: ?i64,
    severity: []const u8,
    disposition: []const u8,
    reproduction_status: []const u8,
    duplicate_of: ?[]const u8,
    evidence_summary: ?[]const u8,
    created_at: []const u8,
    updated_at: []const u8,
};

test "feedback triage list show set round-trip task and retained question" {
    const a = std.testing.allocator;
    var suite = harness.Suite.init(a);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(a);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    _ = suite.registerProject("feedback-triage");

    const plan = suite.mustRunJSON(Plan, arena, &.{ "plan", "create", "--json", "Feedback findings" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(Task, arena, &.{ "task", "add", "--plan", plan_id, "--json", "Retry loop" });
    const question = suite.mustRunJSON(Question, arena, &.{ "question", "add", "--plan", plan_id, "--json", "Is this intended?" });
    const task_ref = std.fmt.allocPrint(arena, "task:{d}", .{task.id}) catch unreachable;
    const question_ref = std.fmt.allocPrint(arena, "question:{d}", .{question.id}) catch unreachable;

    const task_triage = suite.mustRunJSON(Triage, arena, &.{ "feedback", "triage", "set", task_ref, "--severity", "high", "--disposition", "accepted", "--reproduction", "reproduced", "--evidence", "redacted retry trace", "--json" });
    try std.testing.expectEqualStrings(task_ref, task_triage.finding);
    try std.testing.expectEqualStrings("redacted retry trace", task_triage.evidence_summary.?);

    const question_triage = suite.mustRunJSON(Triage, arena, &.{ "feedback", "triage", "set", question_ref, "--severity", "medium", "--disposition", "retained-question", "--reproduction", "inconclusive", "--json" });
    try std.testing.expectEqualStrings("retained-question", question_triage.disposition);
    try std.testing.expectEqualStrings("inconclusive", question_triage.reproduction_status);

    const shown = suite.mustRunJSON(Triage, arena, &.{ "feedback", "triage", "show", question_ref, "--json" });
    try std.testing.expectEqualStrings(question_ref, shown.finding);
    const listed = suite.mustRunJSON([]Triage, arena, &.{ "feedback", "triage", "list", "--plan", plan_id, "--severity", "medium", "--json" });
    try std.testing.expectEqual(@as(usize, 1), listed.len);
    try std.testing.expectEqualStrings(question_ref, listed[0].finding);
}
