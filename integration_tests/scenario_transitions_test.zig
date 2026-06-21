//! integration_tests/scenario_transitions_test.zig
//!
//! Pins the scenario status-transition matrix (plan 692 / M3) at the
//! CLI layer.  The policy.status.check(.scenario, …) arm is fully
//! enforced after this milestone; this file provides the integration
//! surface required by test-spec artifact 382 § "Integration contract".
//!
//! Contract pinned:
//!   - Legal edges succeed: exit 0 + `scenario show --json` reflects
//!     the new status (exit 0 alone is insufficient — a silent no-op
//!     is still exit 0).
//!   - Illegal edges fail: non-zero exit + `scenario show --json`
//!     confirms the stored status is unchanged.
//!
//! Matrix under test:
//!   draft    → {ready, retired}          (ready via auto-transition in verify)
//!   ready    → {verified, failing, retired}
//!   verified → {failing, retired}
//!   failing  → {verified, retired}
//!   retired  → terminal (no outgoing edges)
//!   identity (from == to) → no-op (exit 0, status unchanged)
//!
//! Note: `ready` and `failing` are CLI-unreachable intermediate states —
//! there is no `scenario ready` verb (Non-Goal).  `verify --outcome pass`
//! on a draft scenario auto-walks draft → ready → verified internally
//! (two policy-checked hops).  `failing` is set only by internal engine
//! code; no CLI verb sets it.  Matrix edges into/out of these states are
//! covered by unit tests in src/engine/planning/scenario.zig.
//!
//! Verifies (test-spec slugs from artifact 382):
//!   scenario-arm-matrix, scenario-wire-paths, scenario-sweep

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64, status: []const u8 };
const ScenarioJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
    last_outcome: ?[]const u8 = null,
};

// ---------------------------------------------------------------------------
// Helper: create a plan + scenario, return the scenario id as a string.
// ---------------------------------------------------------------------------

fn setupScenario(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    project_slug: []const u8,
    title: []const u8,
) []const u8 {
    _ = suite.registerProject(project_slug);

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Transition test plan",
    });
    const plan_id_s = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const scen = suite.mustRunJSON(ScenarioJSON, arena, &.{
        "scenario", "add", "--json", "--plan", plan_id_s, title,
    });
    return std.fmt.allocPrint(arena, "{d}", .{scen.id}) catch unreachable;
}

// ---------------------------------------------------------------------------
// scenario-arm-matrix: legal edges accepted
// ---------------------------------------------------------------------------

test "scenario arm: draft → verified via auto-transition is legal (scenario-arm-matrix)" {
    // verify on a draft scenario walks draft → ready → verified internally.
    // This is the primary legal path from the CLI perspective.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const id = setupScenario(&suite, arena, "scen-draft-verified", "draft-verified scenario");

    const updated = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "verify", id, "--json" });
    try std.testing.expectEqualStrings("verified", updated.status);
    try std.testing.expectEqualStrings("pass", updated.last_outcome.?);

    // Post-state via show (not just the verb's echo).
    const shown = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    try std.testing.expectEqualStrings("verified", shown.status);
}

test "scenario arm: draft → retired is legal (scenario-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const id = setupScenario(&suite, arena, "scen-draft-retired", "draft-retired scenario");

    gpa.free(suite.mustRun(&.{ "scenario", "retire", id }));
    const shown = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", shown.status);
}

test "scenario arm: verified → retired is legal (scenario-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const id = setupScenario(&suite, arena, "scen-verified-retired", "verified-retired scenario");

    // draft → verified (auto-transition)
    gpa.free(suite.mustRun(&.{ "scenario", "verify", id }));
    gpa.free(suite.mustRun(&.{ "scenario", "retire", id, "--reason", "obsolete" }));

    const shown = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", shown.status);
}

// ---------------------------------------------------------------------------
// scenario-arm-matrix: illegal edges refused + post-state unchanged
// ---------------------------------------------------------------------------

test "scenario arm: retired is terminal — verify from retired fails (scenario-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const id = setupScenario(&suite, arena, "scen-terminal-retire", "terminal scenario");

    // Drive to retired.
    gpa.free(suite.mustRun(&.{ "scenario", "retire", id }));
    const after_retire = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", after_retire.status);

    // Attempting verify from retired must fail (retired → verified not in matrix).
    const stderr = suite.expectFailure(&.{ "scenario", "verify", id });
    gpa.free(stderr);
    const shown = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", shown.status);
}

// ---------------------------------------------------------------------------
// scenario-wire-paths: every status-mutation verb routes through the check
// ---------------------------------------------------------------------------

test "scenario wire-paths: verify from draft auto-transitions to verified (check is wired)" {
    // Proves policy.status.check is called by scenario.verify through each
    // hop (draft→ready, ready→verified) and that the auto-transition lands
    // at verified, not an error.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const id = setupScenario(&suite, arena, "scen-wire-verify", "wire-verify scenario");

    const result = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "verify", id, "--json" });
    try std.testing.expectEqualStrings("verified", result.status);

    const shown = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    try std.testing.expectEqualStrings("verified", shown.status);
}

test "scenario wire-paths: retire from draft is legal (check wired on retire)" {
    // Proves policy.status.check is called by scenario.retire and
    // accepts the draft → retired legal edge.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const id = setupScenario(&suite, arena, "scen-wire-retire-draft", "wire-retire-draft scenario");

    gpa.free(suite.mustRun(&.{ "scenario", "retire", id, "--reason", "test wire" }));
    const shown = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", shown.status);
}

test "scenario wire-paths: retire from retired is identity (check wired on retire)" {
    // retired → retired is the identity no-op (from == to, early return
    // before IllegalTransition).  Must succeed with exit 0.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const id = setupScenario(&suite, arena, "scen-wire-retire-terminal", "wire-retire-terminal scenario");

    // First retire: draft → retired (legal).
    gpa.free(suite.mustRun(&.{ "scenario", "retire", id }));

    // Second retire: retired → retired is identity (exit 0).
    gpa.free(suite.mustRun(&.{ "scenario", "retire", id }));
    const shown = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", shown.status);
}

test "scenario wire-paths: verify from retired is refused (check wired on verify)" {
    // retired → verified is not in the matrix; verify must refuse.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const id = setupScenario(&suite, arena, "scen-wire-verify-retired", "wire-verify-retired scenario");

    // Drive to retired via auto-transition to verified then retire.
    gpa.free(suite.mustRun(&.{ "scenario", "verify", id }));
    gpa.free(suite.mustRun(&.{ "scenario", "retire", id }));

    // verify from retired should fail.
    const stderr = suite.expectFailure(&.{ "scenario", "verify", id });
    gpa.free(stderr);
    const shown = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", shown.status);
}

// ---------------------------------------------------------------------------
// scenario-sweep: full lifecycle round-trip
// ---------------------------------------------------------------------------

test "scenario sweep: full lifecycle draft → verified → retired (add-verify-retire)" {
    // The canonical operator workflow: scenario add → scenario verify → scenario retire.
    // No explicit `scenario ready` step — verify auto-transitions draft → ready → verified.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("scen-sweep-full");
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "create", "--json", "Full lifecycle plan" });
    const plan_id_s = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const scen = suite.mustRunJSON(ScenarioJSON, arena, &.{
        "scenario", "add", "--json", "--plan", plan_id_s, "Full lifecycle scenario",
    });
    try std.testing.expectEqualStrings("draft", scen.status);
    const id = std.fmt.allocPrint(arena, "{d}", .{scen.id}) catch unreachable;

    // draft → verified (auto-transition: draft → ready → verified internally)
    const at_verified = suite.mustRunJSON(ScenarioJSON, arena, &.{
        "scenario", "verify", id, "--summary", "clean CI pass", "--json",
    });
    try std.testing.expectEqualStrings("verified", at_verified.status);
    try std.testing.expectEqualStrings("pass", at_verified.last_outcome.?);

    // verified → retired
    const at_retired = suite.mustRunJSON(ScenarioJSON, arena, &.{
        "scenario", "retire", id, "--reason", "feature removed", "--json",
    });
    try std.testing.expectEqualStrings("retired", at_retired.status);

    // Post-state confirm
    const final = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", final.status);
}

test "scenario sweep: non-passing verify from draft does not change status" {
    // verify --outcome fail on a draft scenario: records last_outcome but
    // does NOT auto-transition (only pass triggers auto-walk draft→ready→verified).
    // Status must remain draft.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const id = setupScenario(&suite, arena, "scen-sweep-fail-outcome", "failing run scenario");

    // verify --outcome fail: records the outcome but leaves status at draft.
    const res = suite.execWith(&.{ "scenario", "verify", id, "--outcome", "fail", "--json" }, &.{});
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    const shown = suite.mustRunJSON(ScenarioJSON, arena, &.{ "scenario", "show", id, "--json" });
    // Status must still be draft (non-passing run does not trigger auto-transition).
    try std.testing.expectEqualStrings("draft", shown.status);
    try std.testing.expect(shown.last_outcome != null);
    try std.testing.expectEqualStrings("fail", shown.last_outcome.?);
}
