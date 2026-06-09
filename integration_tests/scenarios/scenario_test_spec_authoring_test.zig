//! integration_tests/scenarios/scenario_test_spec_authoring_test.zig
//!
//! Scenario M5 of plan 352. Test-spec scenario authoring: operator
//! drafts a test-spec scenario, links it to the task it verifies,
//! records a successful verification, then retires an obsolete
//! scenario from a different plan.
//!
//! File name uses `test_spec_authoring` (not `scenario_authoring`)
//! to break the naming collision with the scenario-test concept —
//! this file tests the `planar scenario` verbs, which exist to
//! capture test-spec scenarios, NOT the integration-test scenario
//! harness itself. See Q243 on plan 352.
//!
//! Verbs exercised:
//!     plan create, scenario add, scenario list, scenario show,
//!     scenario verify, scenario link, scenario retire,
//!     task add.
//!
//! Verifies (roadmap slugs):
//!     [ts/scenario-add] — `scenario add --plan --body` creates
//!     a `draft` row; `scenario list` returns it.
//!     [ts/scenario-task-link] — `scenario link <id> task:<id>
//!     --relationship verifies` writes the entity_links edge.
//!     [ts/scenario-verify-passing] — `scenario verify <id>
//!     --summary "<text>"` flips status to `verified` and
//!     populates last_outcome="pass" + last_run_at.
//!
//! Known gap: TS-Z2 (scenario verify --outcome failing) deferred
//! to the long-tail audit. `scenario verify` as implemented today
//! only records successful runs (no --outcome flag); the failing-
//! run authoring flow is captured as an open question against the
//! engine, not this scenario file.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const ScenarioJSON = struct {
    id: i64,
    title: []const u8,
    body: ?[]const u8 = null,
    status: []const u8,
    last_outcome: ?[]const u8 = null,
    last_run_at: ?[]const u8 = null,
};

const TaskJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

// =========================================================================
// Primary flow: add → link to task → verify, status transitions captured
// =========================================================================

test "scenario: test-spec authoring — add, link to task, verify (passing)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ts-author");

    // ---- 1. Plan + task to anchor the scenario.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Test-spec smoke",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",      "add",                         "--json",
        "--plan",    plan_id_str,                   "--next-action",
        "implement", "Implement /reports endpoint",
    });
    const task_id_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    // ---- 2. Scenario add — starts as draft.
    const scen = suite.mustRunJSON(ScenarioJSON, arena, &.{
        "scenario",                                                                     "add",                         "--json",
        "--plan",                                                                       plan_id_str,                   "--body",
        "GIVEN /reports endpoint, WHEN GET with valid token, THEN 200 + JSON payload.", "Reports endpoint happy path",
    });
    try std.testing.expectEqualStrings("draft", scen.status);
    try std.testing.expect(scen.last_run_at == null);
    try std.testing.expect(scen.last_outcome == null);

    const scen_id_str = std.fmt.allocPrint(arena, "{d}", .{scen.id}) catch unreachable;

    // ---- 3. Scenario list returns the draft row.
    const list_raw = suite.mustRun(&.{ "scenario", "list", "--scope", "global", "--json" });
    defer gpa.free(list_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, list_raw, 1, "Reports endpoint happy path"));

    // ---- 4. Link scenario → task via `verifies` relationship.
    //
    // The `verifies` value matches the entity_links Relationship
    // enum (src/engine/entitylink.zig § Relationship). The link
    // is the test-spec "this scenario verifies that task" edge.
    const link_target = std.fmt.allocPrint(arena, "task:{d}", .{task.id}) catch unreachable;
    const link_out = suite.mustRun(&.{
        "scenario", "link", scen_id_str, link_target, "--relationship", "verifies",
    });
    gpa.free(link_out);

    const scen_ref = std.fmt.allocPrint(arena, "test_scenario:{d}", .{scen.id}) catch unreachable;
    const links_raw = suite.mustRun(&.{ "links", "list", scen_ref, "--json" });
    defer gpa.free(links_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, links_raw, 1, "verifies"));
    try std.testing.expect(std.mem.containsAtLeast(u8, links_raw, 1, task_id_str));

    // ---- 5. Verify the scenario.
    //
    // `scenario verify <id> --summary "<text>"` flips status to
    // `verified` and populates last_outcome="pass" + last_run_at.
    // The current implementation only records successful runs;
    // a failing-run flow is open (see TS-Z2 in the test spec).
    const ver_out = suite.mustRun(&.{
        "scenario",  "verify",                                          scen_id_str,
        "--summary", "Ran 2026-05-26: clean pass, no allocator leaks.",
    });
    gpa.free(ver_out);

    const scen_after = suite.mustRunJSON(ScenarioJSON, arena, &.{
        "scenario", "show", scen_id_str, "--json",
    });
    try std.testing.expectEqualStrings("verified", scen_after.status);
    try std.testing.expect(scen_after.last_outcome != null);
    try std.testing.expectEqualStrings("pass", scen_after.last_outcome.?);
    try std.testing.expect(scen_after.last_run_at != null);
    try std.testing.expect(scen_after.last_run_at.?.len > 0);
}

// =========================================================================
// Composition: retire an obsolete scenario; show round-trips terminal status
// =========================================================================

test "scenario: test-spec authoring — retire obsolete scenario lands terminal status" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ts-retire");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Retired flow plan",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const scen = suite.mustRunJSON(ScenarioJSON, arena, &.{
        "scenario",                      "add",                    "--json",
        "--plan",                        plan_id_str,              "--body",
        "Old flow no longer supported.", "Legacy upload pre-OIDC",
    });
    const scen_id_str = std.fmt.allocPrint(arena, "{d}", .{scen.id}) catch unreachable;

    const ret_out = suite.mustRun(&.{
        "scenario", "retire",                                     scen_id_str,
        "--reason", "Migration removed the underlying endpoint.",
    });
    gpa.free(ret_out);

    const after = suite.mustRunJSON(ScenarioJSON, arena, &.{
        "scenario", "show", scen_id_str, "--json",
    });
    try std.testing.expectEqualStrings("retired", after.status);
}

// =========================================================================
// Bug-fix red test: `scenario verify --outcome fail` records a failing
// run. Today the verb has no --outcome flag; it always records pass
// + flips status to verified. The test-spec's TS-Z2 (question 248)
// asserted the failing path. Pin the contract: --outcome fail records
// last_outcome="fail" and leaves status alone (not "verified" — a
// failing run does not constitute verification).
// =========================================================================

test "scenario: verify --outcome fail records failing run (red until engine + flag land)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ts-outcome-fail");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Failing-run target",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const scen = suite.mustRunJSON(ScenarioJSON, arena, &.{
        "scenario",                                           "add",            "--json",
        "--plan",                                             plan_id_str,      "--body",
        "GIVEN flaky service, WHEN x, THEN sometimes fails.", "Flaky scenario",
    });
    const scen_id_str = std.fmt.allocPrint(arena, "{d}", .{scen.id}) catch unreachable;

    // The contract: --outcome fail records last_outcome=fail,
    // last_run_at populates, status stays at draft (not verified).
    const res = suite.execWith(&.{
        "scenario",                                    "verify", scen_id_str,
        "--outcome",                                   "fail",   "--summary",
        "ran 2026-05-26: assertion at line 42 failed", "--json",
    }, &.{});
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "\nscenario verify --outcome fail should exit 0; got term={any} stderr={s}\n",
            .{ res.term, res.stderr },
        );
        try std.testing.expect(false);
    }

    const after = suite.mustRunJSON(ScenarioJSON, arena, &.{
        "scenario", "show", scen_id_str, "--json",
    });
    try std.testing.expect(after.last_outcome != null);
    try std.testing.expectEqualStrings("fail", after.last_outcome.?);
    try std.testing.expect(after.last_run_at != null);
    // A failing run does not constitute verification.
    try std.testing.expect(!std.mem.eql(u8, after.status, "verified"));
}
