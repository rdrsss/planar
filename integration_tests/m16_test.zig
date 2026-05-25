//! integration_tests/m16_test.zig — M16 Cycle B integration tests.
//!
//! Covers three test-spec scenarios (177-zig-parity-test-spec.md):
//!   § "Happy path — Plan steps under a milestone plan"        (L219) — task 2158
//!   § "Happy path — Promote a global plan to an association"  (L213) — task 2161
//!   § "Happy path — Plan recompute-status flips on aggregate change" (L261) — task 2159
//!
//! All tests are black-box: they invoke the compiled `planar` binary and assert
//! on JSON output. Each test uses an isolated ephemeral database via Suite.
//!
//! NOTE on string field lifetime: Zig 0.16 std.json.parseFromSlice uses
//! alloc_if_needed, which aliases string fields directly into the input buffer.
//! mustRunJSON frees the stdout buffer after parsing, invalidating those aliases.
//! To safely assert on string fields, we use mustRun + keep the stdout alive in
//! the arena, then parse directly from the live buffer.

const std = @import("std");
const harness = @import("harness");

// ---- shared JSON shapes -----------------------------------------------------

const PlanJSON = struct {
    id: i64,
    title: []const u8 = "",
    slug: []const u8 = "",
    status: []const u8 = "",
    scope_kind: []const u8 = "",
    scope_id: ?i64 = null,
};

const TaskJSON = struct {
    id: i64,
    title: []const u8 = "",
    status: []const u8 = "",
};

const StepJSON = struct {
    id: i64,
    plan_id: i64,
    ordinal: i64,
    body: []const u8 = "",
    status: []const u8 = "",
    task_id: ?i64 = null,
    created_at: []const u8 = "",
    updated_at: []const u8 = "",
};

/// Parse a single JSON object from a buffer that must remain alive as long as the
/// returned struct's string fields are in use (Zig 0.16 alloc_if_needed aliases).
fn parseJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) T {
    const trimmed = std.mem.trim(u8, buf, " \n");
    const parsed = std.json.parseFromSlice(T, arena, trimmed, .{
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nparseJSON {s} failed: {s}\nbuf: {s}\n", .{ @typeName(T), @errorName(e), buf });
        std.testing.expect(false) catch {};
        unreachable;
    };
    return parsed.value;
}

/// Parse a JSON array from a buffer that must remain alive as long as the returned
/// slice's string fields are in use.
fn parseJSONArray(comptime Elem: type, arena: std.mem.Allocator, buf: []const u8) []const Elem {
    const trimmed = std.mem.trim(u8, buf, " \n");
    const parsed = std.json.parseFromSlice([]const Elem, arena, trimmed, .{
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nparseJSONArray failed: {s}\nbuf: {s}\n", .{ @errorName(e), buf });
        std.testing.expect(false) catch {};
        unreachable;
    };
    return parsed.value;
}

// ---- Test 1: Plan step lifecycle (task 2158) ---------------------------------

test "M16 plan step lifecycle: add × 3, list (ordinal order), done, skip, link" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Create a plan — we only need the id (integer, safe via mustRunJSON).
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "M16_STEP_INTEG_Plan",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Add three steps. Keep each stdout buffer alive in the arena so we can
    // parse string fields (status, body) safely.
    const s1_buf = suite.mustRun(&.{ "plan", "step", "add", "--json", plan_id_str, "Step A" });
    defer gpa.free(s1_buf);
    const s1 = parseJSON(StepJSON, arena, s1_buf);

    const s2_buf = suite.mustRun(&.{ "plan", "step", "add", "--json", plan_id_str, "Step B" });
    defer gpa.free(s2_buf);
    const s2 = parseJSON(StepJSON, arena, s2_buf);

    const s3_buf = suite.mustRun(&.{ "plan", "step", "add", "--json", plan_id_str, "Step C" });
    defer gpa.free(s3_buf);
    const s3 = parseJSON(StepJSON, arena, s3_buf);

    // Ordinals are auto-assigned 1, 2, 3.
    try std.testing.expectEqual(@as(i64, 1), s1.ordinal);
    try std.testing.expectEqual(@as(i64, 2), s2.ordinal);
    try std.testing.expectEqual(@as(i64, 3), s3.ordinal);
    try std.testing.expectEqualStrings("pending", s1.status);

    // list: three steps in ordinal order (JSON array output).
    const list1_buf = suite.mustRun(&.{ "plan", "step", "list", "--json", plan_id_str });
    defer gpa.free(list1_buf);
    const steps1 = parseJSONArray(StepJSON, arena, list1_buf);
    try std.testing.expectEqual(@as(usize, 3), steps1.len);
    try std.testing.expectEqual(@as(i64, 1), steps1[0].ordinal);
    try std.testing.expectEqual(@as(i64, 2), steps1[1].ordinal);
    try std.testing.expectEqual(@as(i64, 3), steps1[2].ordinal);

    // done: mark step 1 done.
    const s1_id_str = std.fmt.allocPrint(arena, "{d}", .{s1.id}) catch unreachable;
    const done_buf = suite.mustRun(&.{ "plan", "step", "done", "--json", s1_id_str });
    defer gpa.free(done_buf);
    const done_step = parseJSON(StepJSON, arena, done_buf);
    try std.testing.expectEqualStrings("done", done_step.status);

    // list again: step at ordinal 1 is done, step at ordinal 2 is still pending.
    const list2_buf = suite.mustRun(&.{ "plan", "step", "list", "--json", plan_id_str });
    defer gpa.free(list2_buf);
    const steps2 = parseJSONArray(StepJSON, arena, list2_buf);
    try std.testing.expectEqualStrings("done", steps2[0].status);
    try std.testing.expectEqualStrings("pending", steps2[1].status);

    // skip: mark step 2 skipped.
    const s2_id_str = std.fmt.allocPrint(arena, "{d}", .{s2.id}) catch unreachable;
    const skip_buf = suite.mustRun(&.{ "plan", "step", "skip", "--json", s2_id_str });
    defer gpa.free(skip_buf);
    const skip_step = parseJSON(StepJSON, arena, skip_buf);
    try std.testing.expectEqualStrings("skipped", skip_step.status);

    // Create a task to link to step 3.
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "M16_STEP_LINK_Task",
    });
    const s3_id_str = std.fmt.allocPrint(arena, "{d}", .{s3.id}) catch unreachable;
    const task_id_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    // link: associate step 3 with the task.
    const link_buf = suite.mustRun(&.{ "plan", "step", "link", "--json", s3_id_str, task_id_str });
    defer gpa.free(link_buf);
    const linked = parseJSON(StepJSON, arena, link_buf);
    try std.testing.expectEqual(task.id, linked.task_id.?);

    // list once more: link is visible on step at ordinal 3.
    const list3_buf = suite.mustRun(&.{ "plan", "step", "list", "--json", plan_id_str });
    defer gpa.free(list3_buf);
    const steps3 = parseJSONArray(StepJSON, arena, list3_buf);
    try std.testing.expectEqual(task.id, steps3[2].task_id.?);
}

// ---- Test 2: Promote and demote (task 2161) ----------------------------------

test "M16 promote/demote: plan global → association → global" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Create an association.
    const assoc_out = suite.mustRun(&.{ "assoc", "create", "acme-test-org" });
    gpa.free(assoc_out);

    // Create a global plan.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "M16_PROMOTE_INTEG_Plan",
    });

    // Verify initial scope via plan show (keep buf alive for string access).
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const before_buf = suite.mustRun(&.{ "plan", "show", "--json", plan_id_str });
    defer gpa.free(before_buf);
    const before = parseJSON(PlanJSON, arena, before_buf);
    try std.testing.expectEqualStrings("global", before.scope_kind);
    try std.testing.expect(before.scope_id == null);

    // Promote the plan to the association.
    const plan_ref = std.fmt.allocPrint(arena, "plan:{d}", .{plan.id}) catch unreachable;
    const promote_out = suite.mustRun(&.{ "promote", plan_ref, "--to", "acme-test-org" });
    gpa.free(promote_out);

    // Verify scope changed to association via plan show.
    const promoted_buf = suite.mustRun(&.{ "plan", "show", "--json", plan_id_str });
    defer gpa.free(promoted_buf);
    const promoted = parseJSON(PlanJSON, arena, promoted_buf);
    try std.testing.expectEqualStrings("association", promoted.scope_kind);
    try std.testing.expect(promoted.scope_id != null);

    // Demote back to global.
    const demote_out = suite.mustRun(&.{ "demote", plan_ref });
    gpa.free(demote_out);

    // Verify scope is back to global with null scope_id.
    const demoted_buf = suite.mustRun(&.{ "plan", "show", "--json", plan_id_str });
    defer gpa.free(demoted_buf);
    const demoted = parseJSON(PlanJSON, arena, demoted_buf);
    try std.testing.expectEqualStrings("global", demoted.scope_kind);
    try std.testing.expect(demoted.scope_id == null);
}

// ---- Test 3: Recompute-status (task 2159) ------------------------------------

const RecomputeResultJSON = struct {
    plan_id: i64,
    status_before: []const u8 = "",
    status_after: []const u8 = "",
    flipped: bool,
};

test "M16 recompute-status: task writes auto-promote; explicit recompute remains idempotent" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Create a parent (anchor) plan.
    const anchor = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--status", "active", "M16_RECOMPUTE_INTEG_Anchor",
    });
    const anchor_id_str = std.fmt.allocPrint(arena, "{d}", .{anchor.id}) catch unreachable;

    // Create a child milestone plan (has a parent → not an anchor → can auto-promote to done).
    const child = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan",     "create",      "--json",                    "--status", "active",
        "--parent", anchor_id_str, "M16_RECOMPUTE_INTEG_Child",
    });
    const child_id_str = std.fmt.allocPrint(arena, "{d}", .{child.id}) catch unreachable;

    // Add two tasks to the child plan.
    const t1 = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--plan", child_id_str, "M16_RECOMPUTE_Task1",
    });
    const t2 = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--plan", child_id_str, "M16_RECOMPUTE_Task2",
    });

    // Confirm child plan status before recompute: should be active.
    const before_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id_str });
    defer gpa.free(before_buf);
    const before = parseJSON(PlanJSON, arena, before_buf);
    try std.testing.expectEqualStrings("active", before.status);

    // Mark both tasks done.
    const t1_id_str = std.fmt.allocPrint(arena, "{d}", .{t1.id}) catch unreachable;
    const t2_id_str = std.fmt.allocPrint(arena, "{d}", .{t2.id}) catch unreachable;
    const done1 = suite.mustRun(&.{ "task", "done", t1_id_str });
    gpa.free(done1);
    const done2 = suite.mustRun(&.{ "task", "done", t2_id_str });
    gpa.free(done2);

    // Task writes now auto-recompute plan status; explicit recompute should
    // observe done and report no flip.
    const r1_buf = suite.mustRun(&.{
        "plan", "recompute-status", "--json", "--plan", child_id_str,
    });
    defer gpa.free(r1_buf);
    const r1 = parseJSON(RecomputeResultJSON, arena, r1_buf);
    try std.testing.expectEqual(child.id, r1.plan_id);
    try std.testing.expectEqualStrings("done", r1.status_after);
    try std.testing.expect(!r1.flipped);

    // Verify via plan show.
    const after_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id_str });
    defer gpa.free(after_buf);
    const after = parseJSON(PlanJSON, arena, after_buf);
    try std.testing.expectEqualStrings("done", after.status);

    // Second call (idempotent): should report no flip.
    const r2_buf = suite.mustRun(&.{
        "plan", "recompute-status", "--json", "--plan", child_id_str,
    });
    defer gpa.free(r2_buf);
    const r2 = parseJSON(RecomputeResultJSON, arena, r2_buf);
    try std.testing.expectEqual(child.id, r2.plan_id);
    try std.testing.expect(!r2.flipped);
    try std.testing.expectEqualStrings("done", r2.status_after);
}
