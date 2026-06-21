//! integration_tests/plan_update_test.zig — plan update parity checks.

const std = @import("std");
const harness = @import("harness");

test "plan update changes slug without changing status" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const CreateJSON = struct {
        id: i64,
        slug: []const u8,
        status: []const u8,
    };
    const ShowJSON = struct {
        slug: []const u8,
        status: []const u8,
    };

    const created = suite.mustRunJSON(CreateJSON, arena, &.{
        "plan", "create", "--json", "--slug", "old-plan-slug", "Slug Update Plan",
    });
    try std.testing.expectEqualStrings("old-plan-slug", created.slug);

    const id_s = try std.fmt.allocPrint(arena, "{d}", .{created.id});
    const update_out = suite.mustRun(&.{ "plan", "update", id_s, "--slug", "new-plan-slug" });
    defer gpa.free(update_out);

    const shown = suite.mustRunJSON(ShowJSON, arena, &.{ "plan", "show", "--json", id_s });
    try std.testing.expectEqualStrings("new-plan-slug", shown.slug);
    try std.testing.expectEqualStrings("draft", shown.status);
}

// ---------------------------------------------------------------------------
// Plan-arm status-transition matrix (plan 692 / M2 / task plan-arm-matrix).
//
// Test-spec (artifact 382, §"plan arm") mandates an INTEGRATION surface
// assertion over the `.plan` arm of `policy.status.check`, which the existing
// slug-update test above does not exercise. The integration contract
// (test-spec §Integration): exit 0 alone is not the contract — assert the
// post-state via `plan show --json` on every move, and assert non-zero exit
// AND unchanged stored status on every refused move.
// ---------------------------------------------------------------------------

const PlanJSON = struct {
    id: i64,
    slug: ?[]const u8 = null,
    status: []const u8,
};

test "plan arm accepts the legal lifecycle: draft/active/paused/done/abandoned" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // ---- draft → active → paused → active → done on one plan. ----
    const created = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--slug", "legal-lifecycle", "Legal Lifecycle Plan",
    });
    try std.testing.expectEqualStrings("draft", created.status);
    const id_s = std.fmt.allocPrint(arena, "{d}", .{created.id}) catch unreachable;

    // Each legal move: `plan update --status <to>` exits 0 (mustRunJSON
    // panics on non-zero), and the returned object reflects the new status.
    // Re-read via `plan show --json` so we assert the STORED post-state, not
    // just the verb's own echo (a silent no-op would still echo exit 0).
    const Move = struct { to: []const u8 };
    const lifecycle = [_]Move{
        .{ .to = "active" },
        .{ .to = "paused" },
        .{ .to = "active" },
        .{ .to = "done" },
    };
    inline for (lifecycle) |m| {
        const updated = suite.mustRunJSON(PlanJSON, arena, &.{
            "plan", "update", id_s, "--status", m.to, "--json",
        });
        try std.testing.expectEqualStrings(m.to, updated.status);

        const shown = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", id_s });
        try std.testing.expectEqualStrings(m.to, shown.status);
    }

    // ---- active → abandoned on a second plan (the lifecycle plan above is
    // already terminal at `done`, and `done → abandoned` is illegal). ----
    const created2 = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--slug", "abandon-lifecycle", "Abandon Lifecycle Plan",
    });
    const id2_s = std.fmt.allocPrint(arena, "{d}", .{created2.id}) catch unreachable;

    const to_active = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "update", id2_s, "--status", "active", "--json",
    });
    try std.testing.expectEqualStrings("active", to_active.status);

    const to_abandoned = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "update", id2_s, "--status", "abandoned", "--json",
    });
    try std.testing.expectEqualStrings("abandoned", to_abandoned.status);

    const shown2 = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", id2_s });
    try std.testing.expectEqualStrings("abandoned", shown2.status);
}

test "plan arm refuses skip-ahead and terminal-revival moves" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // ---- Illegal move #1: draft → done (skipping active). ----
    const skip_plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--slug", "skip-ahead", "Skip Ahead Plan",
    });
    try std.testing.expectEqualStrings("draft", skip_plan.status);
    const skip_id = std.fmt.allocPrint(arena, "{d}", .{skip_plan.id}) catch unreachable;

    // Non-zero exit (expectFailure panics if the command succeeds).
    const skip_stderr = suite.expectFailure(&.{ "plan", "update", skip_id, "--status", "done" });
    gpa.free(skip_stderr);

    // Exit code is NOT sufficient: assert the stored status is unchanged.
    const after_skip = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", skip_id });
    try std.testing.expectEqualStrings("draft", after_skip.status);

    // ---- Illegal move #2: done → draft (reviving a terminal plan). ----
    // Drive a fresh plan draft → active → done legally first so the source
    // status is the terminal `done`, then attempt the illegal revival.
    const revive_plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--slug", "terminal-revival", "Terminal Revival Plan",
    });
    const revive_id = std.fmt.allocPrint(arena, "{d}", .{revive_plan.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "plan", "update", revive_id, "--status", "active" }));
    gpa.free(suite.mustRun(&.{ "plan", "update", revive_id, "--status", "done" }));
    const at_done = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", revive_id });
    try std.testing.expectEqualStrings("done", at_done.status);

    // Non-zero exit on the revival attempt.
    const revive_stderr = suite.expectFailure(&.{ "plan", "update", revive_id, "--status", "draft" });
    gpa.free(revive_stderr);

    // Stored status must still be the terminal `done`.
    const after_revive = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", revive_id });
    try std.testing.expectEqualStrings("done", after_revive.status);
}
