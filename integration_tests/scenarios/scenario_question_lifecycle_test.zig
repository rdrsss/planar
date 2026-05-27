//! integration_tests/scenarios/scenario_question_lifecycle_test.zig
//!
//! Scenario M4 of plan 352. Question lifecycle: operator captures
//! open uncertainties as they encounter them, lists them by status
//! filter, resolves some via `answer` and others via `wontfix`,
//! and links a question to the decision that closes it.
//!
//! Verbs exercised:
//!     plan create, question add (×4), question list (with status
//!     filters), question show, question answer, question wontfix,
//!     decision add, question link.
//!
//! Verifies (roadmap slugs):
//!     [ql/question-add-with-plan] — `question add --plan --body`
//!     creates an open question linked to the plan via entity_links.
//!     [ql/question-list-filter] — `question list --status open`,
//!     `--status answered`, `--status wontfix` each filter
//!     correctly.
//!     [ql/question-answer] — `question answer <id> --answer
//!     "<text>"` flips status to `answered` and populates
//!     answer_body + answered_at.
//!     [ql/question-wontfix] — `question wontfix <id> --reason
//!     "<text>"` from `open` transitions to `wontfix`.
//!     [ql/question-decision-link] — `question link <id>
//!     decision:<id> --relationship addresses` writes the edge;
//!     `links list` confirms.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const QuestionJSON = struct {
    id: i64,
    title: []const u8,
    body: ?[]const u8 = null,
    status: []const u8,
    answer_body: ?[]const u8 = null,
    answered_at: ?[]const u8 = null,
};

const DecisionJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

// =========================================================================
// Primary flow: add → answer / wontfix → list filters
// =========================================================================

test "scenario: question lifecycle — add, list-by-status, answer, wontfix" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ql-flow");

    // ---- 1. Plan to anchor the questions.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Migration spike",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // ---- 2. Three questions, all open initially. `question add
    // --plan` (Q237 follow-up, landed in plan 351 cycle D)
    // attaches each to the anchor plan via entity_links.
    const q_open = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question",                      "add",                       "--json",
        "--plan",                        plan_id_str,                 "--body",
        "Do we keep the legacy cookie?", "Legacy cookie disposition",
    });
    try std.testing.expectEqualStrings("open", q_open.status);
    try std.testing.expect(q_open.answered_at == null);

    const q_to_answer = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question",                             "add",            "--json",
        "--plan",                               plan_id_str,      "--body",
        "What's the migration cutover window?", "Cutover window",
    });

    const q_to_wontfix = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question",                                      "add",                        "--json",
        "--plan",                                        plan_id_str,                  "--body",
        "Should we audit pre-migration session tokens?", "Audit pre-migration tokens",
    });

    // ---- 3. `question list --status open` returns all three.
    const open_list_raw = suite.mustRun(&.{ "question", "list", "--status", "open", "--json" });
    defer gpa.free(open_list_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, open_list_raw, 1, "Legacy cookie disposition"));
    try std.testing.expect(std.mem.containsAtLeast(u8, open_list_raw, 1, "Cutover window"));
    try std.testing.expect(std.mem.containsAtLeast(u8, open_list_raw, 1, "Audit pre-migration tokens"));

    // ---- 4. Answer the second question.
    //
    // `question answer <id> --answer "<text>"` flips status to
    // `answered` and populates both answer_body and answered_at
    // atomically per the schema CHECK constraint documented in
    // src/engine/planning/question.zig § Status.
    const q2_id_str = std.fmt.allocPrint(arena, "{d}", .{q_to_answer.id}) catch unreachable;
    const ans_out = suite.mustRun(&.{
        "question", "answer",                                                 q2_id_str,
        "--answer", "T+24h after primary migration; rollback through T+72h.",
    });
    gpa.free(ans_out);

    const q_answered = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "show", q2_id_str, "--json",
    });
    try std.testing.expectEqualStrings("answered", q_answered.status);
    try std.testing.expect(q_answered.answer_body != null);
    try std.testing.expect(q_answered.answered_at != null);
    try std.testing.expect(q_answered.answer_body.?.len > 0);

    // ---- 5. wontfix the third question.
    //
    // `question wontfix <id> --reason "<text>"` from `open`
    // transitions to `wontfix`. The reason text is captured but
    // not asserted on per-field (it lives in audit history; the
    // show JSON doesn't surface a dedicated wontfix_reason
    // column as of 2026-05-26).
    const q3_id_str = std.fmt.allocPrint(arena, "{d}", .{q_to_wontfix.id}) catch unreachable;
    const wf_out = suite.mustRun(&.{
        "question", "wontfix",                                                   q3_id_str,
        "--reason", "Out of scope; audit lives on the platform team's roadmap.",
    });
    gpa.free(wf_out);

    const q_wontfix = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "show", q3_id_str, "--json",
    });
    try std.testing.expectEqualStrings("wontfix", q_wontfix.status);

    // ---- 6. Status filters after the transitions.
    //
    // `question list --status open` should now return just the
    // first question; `--status answered` returns the second;
    // `--status wontfix` returns the third.
    const open_after_raw = suite.mustRun(&.{ "question", "list", "--status", "open", "--json" });
    defer gpa.free(open_after_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, open_after_raw, 1, "Legacy cookie disposition"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, open_after_raw, 1, "Cutover window"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, open_after_raw, 1, "Audit pre-migration tokens"));

    const answered_raw = suite.mustRun(&.{ "question", "list", "--status", "answered", "--json" });
    defer gpa.free(answered_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, answered_raw, 1, "Cutover window"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, answered_raw, 1, "Legacy cookie disposition"));

    const wontfix_raw = suite.mustRun(&.{ "question", "list", "--status", "wontfix", "--json" });
    defer gpa.free(wontfix_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, wontfix_raw, 1, "Audit pre-migration tokens"));
}

// =========================================================================
// Composition: question → decision link, links list surfaces the edge
// =========================================================================

test "scenario: question lifecycle — question link to the decision that resolves it" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ql-link");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Linked decision flow",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Operator opens a question.
    const q = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question",               "add",        "--json",
        "--plan",                 plan_id_str,  "--body",
        "Which IdP do we adopt?", "Choose IdP",
    });
    const q_id_str = std.fmt.allocPrint(arena, "{d}", .{q.id}) catch unreachable;

    // The answer is captured as a decision record.
    const d = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision",                               "add",                "--json",
        "--plan",                                 plan_id_str,          "--body",
        "Adopt platform IdP. See 2026-Q2 retro.", "Adopt platform IdP",
    });
    const d_id_str = std.fmt.allocPrint(arena, "{d}", .{d.id}) catch unreachable;

    // Link the question → decision (the decision addresses the
    // question). The `addresses` relationship comes from the
    // entity_links enum in src/engine/entitylink.zig.
    const link_target = std.fmt.allocPrint(arena, "decision:{d}", .{d.id}) catch unreachable;
    const link_out = suite.mustRun(&.{
        "question", "link", q_id_str, link_target, "--relationship", "addresses",
    });
    gpa.free(link_out);

    // `links list question:<id> --json` surfaces the edge.
    const ref = std.fmt.allocPrint(arena, "question:{d}", .{q.id}) catch unreachable;
    const links_raw = suite.mustRun(&.{ "links", "list", ref, "--json" });
    defer gpa.free(links_raw);

    try std.testing.expect(std.mem.containsAtLeast(u8, links_raw, 1, "addresses"));
    try std.testing.expect(std.mem.containsAtLeast(u8, links_raw, 1, d_id_str));

    // The decision side of the link also lists it.
    const d_ref = std.fmt.allocPrint(arena, "decision:{d}", .{d.id}) catch unreachable;
    const d_links_raw = suite.mustRun(&.{ "links", "list", d_ref, "--json" });
    defer gpa.free(d_links_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, d_links_raw, 1, "addresses"));
    try std.testing.expect(std.mem.containsAtLeast(u8, d_links_raw, 1, q_id_str));
}

// =========================================================================
// Bug-fix red test: question status policy refuses terminal → terminal
// transitions. `wontfix` from `answered` (or vice versa) is not a legal
// move — both are terminal states. The policy.status matrix currently
// stubs `.question` as permissive; the matrix needs the open-only rule.
// =========================================================================

test "scenario: question wontfix from answered refuses (red until status matrix wires open-only rule)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ql-terminal-guard");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Terminal guard target",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const q = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question",                                       "add",            "--json",
        "--plan",                                         plan_id_str,      "--body",
        "Drives the answered-then-wontfix-refusal probe", "Terminal probe",
    });
    const q_id_str = std.fmt.allocPrint(arena, "{d}", .{q.id}) catch unreachable;

    // Move to `answered` legally.
    const ans_out = suite.mustRun(&.{
        "question", "answer", q_id_str, "--answer", "yes, definitively",
    });
    gpa.free(ans_out);

    const q_answered = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "show", q_id_str, "--json",
    });
    try std.testing.expectEqualStrings("answered", q_answered.status);

    // Now attempt `wontfix` from the terminal answered state. The
    // documented contract: answered is terminal — wontfix must
    // refuse with non-zero exit. The current engine accepts the
    // transition (this assertion is red until the policy.status
    // matrix lands).
    const stderr = suite.expectFailure(&.{
        "question", "wontfix", q_id_str, "--reason", "should-refuse",
    });
    defer gpa.free(stderr);

    // Engine should surface an IllegalTransition or terminal-state
    // refusal in stderr.
    const refusal_ok =
        std.mem.containsAtLeast(u8, stderr, 1, "IllegalTransition") or
        std.mem.containsAtLeast(u8, stderr, 1, "terminal") or
        std.mem.containsAtLeast(u8, stderr, 1, "transition");
    if (!refusal_ok) {
        std.debug.print(
            "\nexpected terminal-state refusal in stderr; got:\n{s}\n",
            .{stderr},
        );
        try std.testing.expect(false);
    }

    // The status must NOT have flipped — still `answered`.
    const q_after = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "show", q_id_str, "--json",
    });
    try std.testing.expectEqualStrings("answered", q_after.status);
}
