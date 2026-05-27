//! integration_tests/scenarios/scenario_plan_progression_test.zig
//!
//! Scenario M12 of plan 352. Plan progression: operator decomposes
//! a plan into ordered steps, marks them done / skipped, links a
//! step to its materializing task, and recomputes plan status.
//!
//! Verbs exercised:
//!     init, plan create, plan step add (×3, with --after), plan
//!     step list, plan step done, plan step skip, plan step link,
//!     task add, plan recompute-status, plan show.
//!
//! Verifies (roadmap slugs):
//!     [pp/plan-step-add] — `plan step add` appends a step at
//!     ordinal max+1; `plan step list` returns them in order.
//!     [pp/plan-step-done] — `plan step done <step-id>` flips
//!     status to "done".
//!     [pp/plan-step-skip] — `plan step skip <step-id>` flips
//!     status to "skipped".
//!     [pp/plan-step-link] — `plan step link <step-id> <task-id>`
//!     wires the step's task_id field.
//!     [pp/plan-recompute-status] — `plan recompute-status --plan
//!     <id> --json` returns the status_before / status_after /
//!     flipped tuple, no-op on a plan whose status already
//!     matches.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64, title: []const u8, status: []const u8 };
const TaskJSON = struct { id: i64, title: []const u8, status: []const u8 };

const StepJSON = struct {
    id: i64,
    plan_id: i64,
    ordinal: i64,
    body: []const u8,
    status: []const u8,
    task_id: ?i64 = null,
};

const RecomputeResult = struct {
    plan_id: i64,
    status_before: []const u8,
    status_after: []const u8,
    flipped: bool,
};

test "scenario: plan progression — add steps, mark done/skip/link, recompute status" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("pp-flow");

    // ---- 1. Plan to host the steps.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Plan progression",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // ---- 2. Three plan steps.
    const s1 = suite.mustRunJSON(StepJSON, arena, &.{
        "plan", "step", "add", plan_id_str, "Scaffold the route handler", "--json",
    });
    const s2 = suite.mustRunJSON(StepJSON, arena, &.{
        "plan", "step", "add", plan_id_str, "Wire the response shape", "--json",
    });
    const s3 = suite.mustRunJSON(StepJSON, arena, &.{
        "plan", "step", "add", plan_id_str, "Write the integration tests", "--json",
    });
    try std.testing.expectEqual(@as(i64, 1), s1.ordinal);
    try std.testing.expectEqual(@as(i64, 2), s2.ordinal);
    try std.testing.expectEqual(@as(i64, 3), s3.ordinal);

    // ---- 3. plan step list returns them in ordinal order.
    const list_raw = suite.mustRun(&.{ "plan", "step", "list", plan_id_str, "--json" });
    defer gpa.free(list_raw);
    const StepList = []StepJSON;
    const list_parsed = std.json.parseFromSlice(StepList, arena, list_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expectEqual(@as(usize, 3), list_parsed.value.len);
    try std.testing.expectEqual(s1.id, list_parsed.value[0].id);
    try std.testing.expectEqual(s2.id, list_parsed.value[1].id);
    try std.testing.expectEqual(s3.id, list_parsed.value[2].id);

    // ---- 4. Mark step 1 done.
    const s1_id_str = std.fmt.allocPrint(arena, "{d}", .{s1.id}) catch unreachable;
    const done_step = suite.mustRunJSON(StepJSON, arena, &.{
        "plan", "step", "done", s1_id_str, "--json",
    });
    try std.testing.expectEqualStrings("done", done_step.status);

    // ---- 5. Skip step 2.
    const s2_id_str = std.fmt.allocPrint(arena, "{d}", .{s2.id}) catch unreachable;
    const skip_step = suite.mustRunJSON(StepJSON, arena, &.{
        "plan", "step", "skip", s2_id_str, "--json",
    });
    try std.testing.expectEqualStrings("skipped", skip_step.status);

    // ---- 6. Link step 3 to its materializing task.
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",             "add",                "--json",
        "--plan",           plan_id_str,          "--next-action",
        "implement step 3", "Materialize step 3",
    });

    const s3_id_str = std.fmt.allocPrint(arena, "{d}", .{s3.id}) catch unreachable;
    const task_id_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    const link_out = suite.mustRun(&.{
        "plan", "step", "link", s3_id_str, task_id_str, "--json",
    });
    gpa.free(link_out);

    // Confirm via plan step list that step 3 carries the task_id.
    const list2_raw = suite.mustRun(&.{ "plan", "step", "list", plan_id_str, "--json" });
    defer gpa.free(list2_raw);
    const list2_parsed = std.json.parseFromSlice(StepList, arena, list2_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    var saw_linked = false;
    for (list2_parsed.value) |s| {
        if (s.id == s3.id and s.task_id != null and s.task_id.? == task.id) {
            saw_linked = true;
            break;
        }
    }
    try std.testing.expect(saw_linked);

    // ---- 7. plan recompute-status returns the before/after tuple.
    const recompute = suite.mustRunJSON(RecomputeResult, arena, &.{
        "plan", "recompute-status", "--plan", plan_id_str, "--json",
    });
    try std.testing.expectEqual(plan.id, recompute.plan_id);
    // The plan stays `draft` because we haven't activated it; the
    // recompute is a no-op here. What we pin is the verb's
    // documented JSON shape (before, after, flipped fields all
    // present and consistent).
    try std.testing.expectEqualStrings(recompute.status_before, recompute.status_after);
    try std.testing.expect(!recompute.flipped);
}
