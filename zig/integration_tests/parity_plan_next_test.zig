//! integration_tests/parity_plan_next_test.zig
//!
//! Cluster F-plan-next parity test for plan 351. See parity-triage.md
//! §F-plan-next and question 236.
//!
//! Q236 operator decision 2026-05-26: reinstate `planar plan next
//! <plan>`. Go has it; zig dropped it. The agents/orchestrator.md
//! role spec and pl-orchestrator skill surface both reference this
//! verb today; the gap was hit by this very dispatch session.
//!
//! Today: `plan next` doesn't exist as a subcommand — zig errors
//! with UnknownSubcommand. Phase 4 ports the Go implementation
//! (github.com/rdrsss/planar-go-archive: src/cmd/planar/internal/
//! system/plan.go plan-next handler) and the test turns green.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
};
const TaskJSON = struct {
    id: i64,
};

test "parity: 'planar plan next <id>' returns highest-priority eligible task (Cluster F-plan-next, Q236; red until verb reinstated)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Fixture: plan with 3 tasks at different priorities. plan-next
    // should return the highest-priority todo task.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "PLAN_NEXT_INTEG_root",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Lower-priority task (priority 100 = default).
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--plan", plan_id_str, "--priority", "100", "low-priority-task",
    });
    // Higher-priority task. plan-next should pick this one.
    const expected = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--plan", plan_id_str, "--priority", "20", "HIGH_PRIORITY_TARGET",
    });

    // Run plan next. Today: errors with UnknownSubcommand. Phase 4
    // makes this return the high-priority task.
    const res = suite.exec(&.{ "plan", "next", plan_id_str });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    const exit_ok = res.term == .exited and res.term.exited == 0;
    const id_str = std.fmt.allocPrint(arena, "{d}", .{expected.id}) catch unreachable;
    const stdout_has_target = std.mem.containsAtLeast(u8, res.stdout, 1, "HIGH_PRIORITY_TARGET") or
        std.mem.containsAtLeast(u8, res.stdout, 1, id_str);

    if (!exit_ok or !stdout_has_target) {
        std.debug.print(
            "\nplan next contract failed: exit_ok={} stdout_has_target={}\nstdout: {s}\nstderr: {s}\n",
            .{ exit_ok, stdout_has_target, res.stdout, res.stderr },
        );
        try std.testing.expect(false);
    }
}
