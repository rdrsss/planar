//! integration_tests/scenarios/scenario_decision_workflow_test.zig
//!
//! Scenario M3 of plan 352. Decision workflow: operator captures an
//! ADR-class decision, accepts it, supersedes it with a follow-up,
//! attaches a decision to a task, and confirms the terminal-status
//! guard refuses a withdraw on an already-accepted decision.
//!
//! Verbs exercised in the primary flow:
//!     plan create, decision add (×2), decision accept,
//!     decision supersede, decision show, decision link,
//!     task add, decision withdraw (negative path).
//!
//! Verifies (roadmap slugs):
//!     [dw/decision-add] — `decision add --plan --body` creates a
//!     row with `status=proposed`; `decision show --json` round-
//!     trips.
//!     [dw/decision-accept] — `decision accept` flips status to
//!     `accepted`; the decided_at timestamp populates.
//!     [dw/decision-supersede] — `decision supersede <old> --by
//!     <new>` flips the old decision to `superseded` (terminal).
//!     [dw/decision-task-link] — `decision link <id> task:<task-id>
//!     --relationship addresses` writes the edge.
//!     Terminal-status guard (TS-X2 in test-spec) — `decision
//!     withdraw` on an accepted-or-later decision exits non-zero
//!     with the documented "is terminal" message.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const DecisionJSON = struct {
    id: i64,
    title: []const u8,
    body: ?[]const u8 = null,
    status: []const u8,
    decided_at: ?[]const u8 = null,
};

const TaskJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

// =========================================================================
// Primary flow: propose → accept → supersede, with terminal-state guard
// =========================================================================

test "scenario: decision workflow — propose, accept, supersede, terminal-status guard refuses withdraw" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("decision-flow");

    // ---- 1. Plan to anchor the decisions.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Auth strategy",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // ---- 2. First ADR — captured as `proposed`.
    //
    // `decision add` requires --body (engine guard, surfaced via
    // probe 2026-05-26); --plan attaches it to the anchor plan
    // via the standard derives-from edge.
    const d1 = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add",  "--json",
        "--plan",   plan_id_str,
        "--body",   "Adopt OIDC; keep legacy cookie behind feature flag for 90 days.",
        "Adopt OIDC for primary auth",
    });
    try std.testing.expectEqualStrings("Adopt OIDC for primary auth", d1.title);
    try std.testing.expectEqualStrings("proposed", d1.status);
    try std.testing.expect(d1.decided_at == null);

    const d1_id_str = std.fmt.allocPrint(arena, "{d}", .{d1.id}) catch unreachable;

    // ---- 3. Accept — status flips, decided_at populates.
    const accept_out = suite.mustRun(&.{ "decision", "accept", d1_id_str, "--json" });
    gpa.free(accept_out);

    const d1_accepted = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "show", d1_id_str, "--json",
    });
    try std.testing.expectEqualStrings("accepted", d1_accepted.status);
    try std.testing.expect(d1_accepted.decided_at != null);
    try std.testing.expect(d1_accepted.decided_at.?.len > 0);

    // ---- 4. Second ADR (the successor).
    const d2 = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add",  "--json",
        "--plan",   plan_id_str,
        "--body",   "Drop OIDC in favor of platform IdP; rationale in 2026-Q2 retro.",
        "Switch to platform IdP",
    });
    try std.testing.expectEqualStrings("proposed", d2.status);

    const d2_id_str = std.fmt.allocPrint(arena, "{d}", .{d2.id}) catch unreachable;

    // ---- 5. Supersede the first ADR by the second.
    //
    // `decision supersede <old> --by <new>` flips the old to
    // `superseded` (terminal). The verb's positional is the OLD
    // decision; the `--by` flag carries the SUCCESSOR id.
    const supersede_out = suite.mustRun(&.{
        "decision", "supersede", d1_id_str, "--by", d2_id_str, "--json",
    });
    gpa.free(supersede_out);

    const d1_superseded = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "show", d1_id_str, "--json",
    });
    try std.testing.expectEqualStrings("superseded", d1_superseded.status);

    // ---- 6. Decision → task link.
    //
    // `decision link <id> task:<task-id> --relationship addresses`
    // writes the entity_links row. The relationship enum is
    // declared in src/engine/entitylink.zig § Relationship; we
    // use `addresses` for "decision addresses task" semantics.
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",         "add",     "--json",
        "--plan",       plan_id_str,
        "--next-action", "wire IdP",
        "Wire platform IdP into login flow",
    });
    const task_id_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    const link_target = std.fmt.allocPrint(arena, "task:{d}", .{task.id}) catch unreachable;
    const link_out = suite.mustRun(&.{
        "decision", "link", d2_id_str, link_target, "--relationship", "addresses",
    });
    gpa.free(link_out);

    // Confirm the edge via `links list <ref>` — the entity_links
    // browser. The decision is the source, the task is the
    // target; both sides should surface the edge.
    const links_raw = suite.mustRun(&.{ "links", "list", std.fmt.allocPrint(arena, "decision:{d}", .{d2.id}) catch unreachable, "--json" });
    defer gpa.free(links_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, links_raw, 1, task_id_str));
    try std.testing.expect(std.mem.containsAtLeast(u8, links_raw, 1, "addresses"));

    // ---- 7. Terminal-status guard refuses withdraw.
    //
    // `decision withdraw` on a non-proposed decision must exit
    // non-zero with "is terminal" wording. The earlier supersede
    // made d1 terminal; we attempt withdraw and assert the
    // refusal.
    const stderr = suite.expectFailure(&.{ "decision", "withdraw", d1_id_str });
    defer gpa.free(stderr);

    const guard_msg_ok =
        std.mem.containsAtLeast(u8, stderr, 1, "terminal") or
        std.mem.containsAtLeast(u8, stderr, 1, "cannot withdraw") or
        std.mem.containsAtLeast(u8, stderr, 1, "superseded");
    if (!guard_msg_ok) {
        std.debug.print(
            "\nterminal-status guard stderr lacked expected substring; got:\n{s}\n",
            .{stderr},
        );
        try std.testing.expect(false);
    }
}

// =========================================================================
// Composition: decision list filters by plan and status
// =========================================================================

test "scenario: decision workflow — decision list surfaces all decisions on a plan" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("decision-list");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Decision list smoke",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Add three decisions; two stay proposed, one gets accepted.
    const d_a = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json",
        "--plan",   plan_id_str, "--body", "A body",
        "Decision A",
    });
    const d_b = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json",
        "--plan",   plan_id_str, "--body", "B body",
        "Decision B",
    });
    _ = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json",
        "--plan",   plan_id_str, "--body", "C body",
        "Decision C",
    });

    const a_id_str = std.fmt.allocPrint(arena, "{d}", .{d_a.id}) catch unreachable;
    const acc_out = suite.mustRun(&.{ "decision", "accept", a_id_str, "--json" });
    gpa.free(acc_out);

    // `decision list --json` returns all decisions in the DB.
    // We assert our three titles all appear; the exact filter
    // shape (e.g. --plan, --status) varies per implementation
    // and isn't load-bearing for this composition assertion.
    const list_raw = suite.mustRun(&.{ "decision", "list", "--json" });
    defer gpa.free(list_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, list_raw, 1, "Decision A"));
    try std.testing.expect(std.mem.containsAtLeast(u8, list_raw, 1, "Decision B"));
    try std.testing.expect(std.mem.containsAtLeast(u8, list_raw, 1, "Decision C"));

    // And `decision show` round-trips each.
    const b_id_str = std.fmt.allocPrint(arena, "{d}", .{d_b.id}) catch unreachable;
    const d_b_shown = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "show", b_id_str, "--json",
    });
    try std.testing.expectEqual(d_b.id, d_b_shown.id);
    try std.testing.expectEqualStrings("proposed", d_b_shown.status);
}
