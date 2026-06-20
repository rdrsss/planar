//! integration_tests/plan_divergence_test.zig —
//!   `planar plan divergence <plan> [--json]`
//!
//! Validates the JSON shape and semantics of the divergence surface
//! (decision D4, plan 636 M2.6). Seeds a minimal plan with two open tasks
//! and asserts the all-zero base case (no touches, no closures seeded via
//! the CLI) to lock the JSON shape. The engine-level computation is covered
//! by the unit tests in strategy.zig.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64 };

const DivergenceJSON = struct {
    plan_id: i64,
    open_tasks: i64,
    pairs: i64,
    declared_overlaps: i64,
    derived_overlaps: i64,
    flips: i64,
    jaccard: f64,
};

test "plan divergence: JSON shape and zero base case" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject(null);

    const plan_out = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Divergence Test Plan", "--json",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan_out.id}) catch unreachable;

    // Add two open tasks; no touches or closures seeded (base case).
    const ta = suite.mustRun(&.{ "task", "add", "Task A", "--plan", pid });
    defer gpa.free(ta);
    const tb = suite.mustRun(&.{ "task", "add", "Task B", "--plan", pid });
    defer gpa.free(tb);

    // All-zero: no touches → no overlaps under either source → no flips.
    const div = suite.mustRunJSON(DivergenceJSON, arena, &.{
        "plan", "divergence", pid, "--json",
    });
    try std.testing.expectEqual(plan_out.id, div.plan_id);
    try std.testing.expectEqual(@as(i64, 2), div.open_tasks);
    try std.testing.expectEqual(@as(i64, 1), div.pairs);
    try std.testing.expectEqual(@as(i64, 0), div.declared_overlaps);
    try std.testing.expectEqual(@as(i64, 0), div.derived_overlaps);
    try std.testing.expectEqual(@as(i64, 0), div.flips);
    try std.testing.expectApproxEqAbs(@as(f64, 0.0), div.jaccard, 1e-9);
}

test "plan divergence: text mode exits 0 with field labels" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject(null);

    const plan_out = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Div Text Plan", "--json",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan_out.id}) catch unreachable;
    const t = suite.mustRun(&.{ "task", "add", "T", "--plan", pid });
    defer gpa.free(t);

    // Text mode: assert exit 0 and output contains key field names.
    const out = suite.mustRun(&.{ "plan", "divergence", pid });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "flips:") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "jaccard:") != null);
}

test "plan divergence: missing plan exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    _ = suite.registerProject(null);

    const err = suite.expectFailure(&.{ "plan", "divergence", "99999", "--json" });
    defer gpa.free(err);
}
