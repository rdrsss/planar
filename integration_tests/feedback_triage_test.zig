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

    const plan = suite.mustRunJSON(Plan, arena, &.{ "plan", "create", "--slug", "planar-feedback", "--json", "Feedback findings" });
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

test "deleting a duplicate target returns dependent findings to untriaged" {
    const a = std.testing.allocator;
    var suite = harness.Suite.init(a);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(a);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    _ = suite.registerProject("feedback-triage-delete-target");

    const plan = suite.mustRunJSON(Plan, arena, &.{ "plan", "create", "--slug", "planar-feedback", "--json", "Feedback findings" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const target = suite.mustRunJSON(Task, arena, &.{ "task", "add", "--plan", plan_id, "--json", "Original finding" });
    const dependent = suite.mustRunJSON(Task, arena, &.{ "task", "add", "--plan", plan_id, "--json", "Repeated finding" });
    const target_ref = std.fmt.allocPrint(arena, "task:{d}", .{target.id}) catch unreachable;
    const dependent_ref = std.fmt.allocPrint(arena, "task:{d}", .{dependent.id}) catch unreachable;
    _ = suite.mustRunJSON(Triage, arena, &.{ "feedback", "triage", "set", target_ref, "--severity", "medium", "--disposition", "accepted", "--reproduction", "reproduced", "--json" });
    _ = suite.mustRunJSON(Triage, arena, &.{ "feedback", "triage", "set", dependent_ref, "--severity", "low", "--disposition", "duplicate", "--reproduction", "not-run", "--duplicate-of", target_ref, "--json" });

    // The public CLI schema has no task/question deletion verb. Use a raw fixture
    // delete only to exercise the migration's foreign-key lifecycle semantics;
    // setup and the observable triage state remain public CLI contracts.
    const delete_sql = std.fmt.allocPrint(arena, "PRAGMA foreign_keys=ON; delete from tasks where id={d};", .{target.id}) catch unreachable;
    const deleted = std.process.run(a, std.testing.io, .{ .argv = &.{ "sqlite3", suite.db_path, delete_sql } }) catch @panic("failed to run sqlite3");
    defer a.free(deleted.stdout);
    defer a.free(deleted.stderr);
    try std.testing.expectEqual(@as(u8, 0), deleted.term.exited);

    const preserved = suite.mustRunJSON(Triage, arena, &.{ "feedback", "triage", "show", dependent_ref, "--json" });
    try std.testing.expectEqualStrings("untriaged", preserved.disposition);
    try std.testing.expect(preserved.duplicate_of == null);
    try std.testing.expectEqualStrings("low", preserved.severity);
    try std.testing.expectEqualStrings("not-run", preserved.reproduction_status);
    const listed = suite.mustRunJSON([]Triage, arena, &.{ "feedback", "triage", "list", "--plan", plan_id, "--json" });
    try std.testing.expectEqual(@as(usize, 1), listed.len);
    try std.testing.expectEqualStrings(dependent_ref, listed[0].finding);
    const still_present = suite.mustRunJSON(Task, arena, &.{ "task", "show", std.fmt.allocPrint(arena, "{d}", .{dependent.id}) catch unreachable, "--json" });
    try std.testing.expectEqual(dependent.id, still_present.id);
}

test "feedback triage rejects unplanned ambiguous and non-feedback findings" {
    const a = std.testing.allocator;
    var suite = harness.Suite.init(a);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(a);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    _ = suite.registerProject("feedback-triage-membership");

    const feedback = suite.mustRunJSON(Plan, arena, &.{ "plan", "create", "--slug", "planar-feedback", "--json", "Feedback findings" });
    const other = suite.mustRunJSON(Plan, arena, &.{ "plan", "create", "--slug", "ordinary-plan", "--json", "Ordinary work" });
    const unplanned = suite.mustRunJSON(Task, arena, &.{ "task", "add", "--json", "Loose task" });
    const other_id = std.fmt.allocPrint(arena, "{d}", .{other.id}) catch unreachable;
    const ordinary = suite.mustRunJSON(Task, arena, &.{ "task", "add", "--plan", other_id, "--json", "Ordinary task" });
    const feedback_id = std.fmt.allocPrint(arena, "{d}", .{feedback.id}) catch unreachable;
    const ambiguous = suite.mustRunJSON(Question, arena, &.{ "question", "add", "--plan", feedback_id, "--json", "Ambiguous question" });
    const ambiguous_id = std.fmt.allocPrint(arena, "{d}", .{ambiguous.id}) catch unreachable;
    const link_out = suite.mustRun(&.{ "question", "link", ambiguous_id, std.fmt.allocPrint(arena, "plan:{d}", .{other.id}) catch unreachable, "--relationship", "derives-from", "--json" });
    defer a.free(link_out);

    const common = [_][]const u8{ "--severity", "low", "--disposition", "accepted", "--reproduction", "not-run" };
    const unplanned_err = suite.expectFailure(&.{ "feedback", "triage", "set", std.fmt.allocPrint(arena, "task:{d}", .{unplanned.id}) catch unreachable, common[0], common[1], common[2], common[3], common[4], common[5] });
    defer a.free(unplanned_err);
    try std.testing.expect(std.mem.indexOf(u8, unplanned_err, "MissingFeedbackPlan") != null);
    const ordinary_err = suite.expectFailure(&.{ "feedback", "triage", "set", std.fmt.allocPrint(arena, "task:{d}", .{ordinary.id}) catch unreachable, common[0], common[1], common[2], common[3], common[4], common[5] });
    defer a.free(ordinary_err);
    try std.testing.expect(std.mem.indexOf(u8, ordinary_err, "DifferentFeedbackPlan") != null);
    const ambiguous_err = suite.expectFailure(&.{ "feedback", "triage", "set", std.fmt.allocPrint(arena, "question:{d}", .{ambiguous.id}) catch unreachable, common[0], common[1], common[2], common[3], common[4], common[5] });
    defer a.free(ambiguous_err);
    try std.testing.expect(std.mem.indexOf(u8, ambiguous_err, "AmbiguousFeedbackPlan") != null);
}
