//! integration_tests/list_plan_filter_test.zig
//!
//! Plan 85 t#2660. Pins the `--plan <id>` filter on the three
//! planning-entity list verbs (artifact list, decision list, question
//! list). Each verb already accepted `--scope`; this test pins the
//! parallel plan-scoped variant.
//!
//! Contract: with two plans P1 and P2 each carrying an artifact, a
//! decision, and a question linked via entity_links (relationship
//! 'derives-from'), `<entity> list --plan P1.id` returns only the
//! P1-linked row and excludes the P2-linked row. The filter composes
//! with `--scope` (intersect, not union) and is silently ignored —
//! producing an empty list — when the plan_id matches nothing.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const ArtifactJSON = struct {
    id: i64,
    title: []const u8,
    kind: []const u8,
    status: []const u8,
};

const DecisionJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const QuestionJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

test "list verbs: --plan <id> filters artifact/decision/question by entity-links edge" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("list-plan-filter");

    // Two plans in the same scope.
    const p1 = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Plan one",
    });
    const p2 = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Plan two",
    });
    const p1_id = std.fmt.allocPrint(arena, "{d}", .{p1.id}) catch unreachable;
    const p2_id = std.fmt.allocPrint(arena, "{d}", .{p2.id}) catch unreachable;

    // Each plan gets exactly one artifact, one decision, one
    // question — all attached via --plan, which writes the
    // entity_links derives-from edge the filter joins on.
    const a1 = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact",  "add",    "--json",
        "--plan",    p1_id,    "--kind",
        "tech_spec", "--body", "p1 spec body",
        "P1 spec",
    });
    const a2 = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact",  "add",    "--json",
        "--plan",    p2_id,    "--kind",
        "tech_spec", "--body", "p2 spec body",
        "P2 spec",
    });
    const d1 = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision",    "add",         "--json",
        "--plan",      p1_id,         "--body",
        "p1 dec body", "P1 decision",
    });
    const d2 = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision",    "add",         "--json",
        "--plan",      p2_id,         "--body",
        "p2 dec body", "P2 decision",
    });
    const q1 = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question",  "add",         "--json",
        "--plan",    p1_id,         "--body",
        "p1 q body", "P1 question",
    });
    const q2 = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question",  "add",         "--json",
        "--plan",    p2_id,         "--body",
        "p2 q body", "P2 question",
    });

    // --- artifact list --plan ---
    const arts_p1 = suite.mustRunJSON([]ArtifactJSON, arena, &.{
        "artifact", "list", "--scope", "global", "--plan", p1_id, "--json",
    });
    try expectExactlyOneId(ArtifactJSON, arts_p1, a1.id, a2.id);

    // --- decision list --plan ---
    const decs_p1 = suite.mustRunJSON([]DecisionJSON, arena, &.{
        "decision", "list", "--scope", "global", "--plan", p1_id, "--json",
    });
    try expectExactlyOneId(DecisionJSON, decs_p1, d1.id, d2.id);

    // --- question list --plan ---
    const qs_p1 = suite.mustRunJSON([]QuestionJSON, arena, &.{
        "question", "list", "--scope", "global", "--plan", p1_id, "--json",
    });
    try expectExactlyOneId(QuestionJSON, qs_p1, q1.id, q2.id);

    // --- Negative: a plan id that exists but has nothing linked
    // returns an empty list. Use a plan with no children.
    const p3 = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Plan three (empty)",
    });
    const p3_id = std.fmt.allocPrint(arena, "{d}", .{p3.id}) catch unreachable;
    const empty_arts = suite.mustRunJSON([]ArtifactJSON, arena, &.{
        "artifact", "list", "--scope", "global", "--plan", p3_id, "--json",
    });
    try std.testing.expectEqual(@as(usize, 0), empty_arts.len);
    const empty_decs = suite.mustRunJSON([]DecisionJSON, arena, &.{
        "decision", "list", "--scope", "global", "--plan", p3_id, "--json",
    });
    try std.testing.expectEqual(@as(usize, 0), empty_decs.len);
    const empty_qs = suite.mustRunJSON([]QuestionJSON, arena, &.{
        "question", "list", "--scope", "global", "--plan", p3_id, "--json",
    });
    try std.testing.expectEqual(@as(usize, 0), empty_qs.len);
}

fn expectExactlyOneId(
    comptime T: type,
    rows: []const T,
    want_id: i64,
    forbidden_id: i64,
) !void {
    try std.testing.expectEqual(@as(usize, 1), rows.len);
    try std.testing.expectEqual(want_id, rows[0].id);
    for (rows) |r| try std.testing.expect(r.id != forbidden_id);
}
