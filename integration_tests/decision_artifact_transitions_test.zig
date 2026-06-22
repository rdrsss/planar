//! integration_tests/decision_artifact_transitions_test.zig
//!
//! Pins the decision and artifact status-transition matrices (plan 692 / M4)
//! at the CLI layer.  policy.status.check(.decision, …) and
//! policy.status.check(.artifact, …) are fully enforced after this
//! milestone, with the modules' local validateTransition functions
//! delegating to the central policy arm.
//!
//! Contract pinned:
//!   - Legal edges succeed: exit 0 + `*/show --json` reflects the new
//!     status (exit 0 alone is insufficient — a silent no-op is still
//!     exit 0; post-state is the contract).
//!   - Illegal edges fail: non-zero exit + `*/show --json` confirms the
//!     stored status is UNCHANGED.
//!   - Error spelling is preserved: the CLI surfaces the same
//!     "terminal" / "cannot" wording as before delegation.
//!
//! Decision matrix:
//!   proposed  → {accepted, superseded, withdrawn}
//!   accepted  → {superseded, withdrawn}
//!   superseded, withdrawn → terminal
//!   identity (from == to) → no-op (exit 0)
//!
//! Artifact matrix:
//!   draft   → active
//!   active  → {draft, superseded, retired}
//!   superseded, retired → terminal
//!   identity (from == to) → no-op (exit 0)
//!
//! Verifies (test-spec slugs from artifact 382):
//!   decision-arm-matrix, artifact-arm-matrix,
//!   decision-artifact-delegate, decision-artifact-sweep

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64, title: []const u8, status: []const u8 };
const DecisionJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
    decided_at: ?[]const u8 = null,
};
const ArtifactJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
    kind: []const u8,
};

// ============================================================================
// decision-arm-matrix: legal edges accepted
// ============================================================================

test "decision arm: proposed → accepted is legal (decision-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("dec-proposed-accepted");

    const d = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--body", "some rationale", "Accept me",
    });
    try std.testing.expectEqualStrings("proposed", d.status);
    const id = std.fmt.allocPrint(arena, "{d}", .{d.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "decision", "accept", id }));

    const after = suite.mustRunJSON(DecisionJSON, arena, &.{ "decision", "show", id, "--json" });
    try std.testing.expectEqualStrings("accepted", after.status);
    try std.testing.expect(after.decided_at != null);
}

test "decision arm: proposed → withdrawn is legal (decision-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("dec-proposed-withdrawn");

    const d = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--body", "some rationale", "Withdraw me",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{d.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "decision", "withdraw", id }));

    const after = suite.mustRunJSON(DecisionJSON, arena, &.{ "decision", "show", id, "--json" });
    try std.testing.expectEqualStrings("withdrawn", after.status);
}

test "decision arm: accepted → superseded is legal via supersede (decision-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("dec-accepted-superseded");

    const old = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--body", "old body", "Old ADR",
    });
    const new = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--body", "new body", "New ADR",
    });
    const old_id = std.fmt.allocPrint(arena, "{d}", .{old.id}) catch unreachable;
    const new_id = std.fmt.allocPrint(arena, "{d}", .{new.id}) catch unreachable;

    // Accept old first, then supersede.
    gpa.free(suite.mustRun(&.{ "decision", "accept", old_id }));
    gpa.free(suite.mustRun(&.{ "decision", "supersede", old_id, "--by", new_id }));

    const after = suite.mustRunJSON(DecisionJSON, arena, &.{ "decision", "show", old_id, "--json" });
    try std.testing.expectEqualStrings("superseded", after.status);
}

// ============================================================================
// decision-arm-matrix: illegal edges refused, post-state unchanged
// ============================================================================

test "decision arm: superseded → accepted is refused (terminal guard)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("dec-superseded-refused");

    const old = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--body", "old body", "Old decision",
    });
    const new = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--body", "new body", "New decision",
    });
    const old_id = std.fmt.allocPrint(arena, "{d}", .{old.id}) catch unreachable;
    const new_id = std.fmt.allocPrint(arena, "{d}", .{new.id}) catch unreachable;

    // Drive old to superseded.
    gpa.free(suite.mustRun(&.{ "decision", "supersede", old_id, "--by", new_id }));

    // Attempt to accept a superseded decision — must fail.
    const stderr = suite.expectFailure(&.{ "decision", "accept", old_id });
    defer gpa.free(stderr);

    // Post-state must be unchanged (still superseded).
    const after = suite.mustRunJSON(DecisionJSON, arena, &.{ "decision", "show", old_id, "--json" });
    try std.testing.expectEqualStrings("superseded", after.status);
}

test "decision arm: withdrawn → accepted is refused (terminal guard)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("dec-withdrawn-refused");

    const d = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--body", "rationale", "Withdrawn decision",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{d.id}) catch unreachable;

    // Drive to withdrawn.
    gpa.free(suite.mustRun(&.{ "decision", "withdraw", id }));

    // Attempt to accept a withdrawn decision — must fail.
    const stderr = suite.expectFailure(&.{ "decision", "accept", id });
    defer gpa.free(stderr);

    // Post-state must be unchanged (still withdrawn).
    const after = suite.mustRunJSON(DecisionJSON, arena, &.{ "decision", "show", id, "--json" });
    try std.testing.expectEqualStrings("withdrawn", after.status);
}

test "decision arm: withdraw from superseded is refused (terminal guard)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("dec-superseded-withdraw-refused");

    const old = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--body", "old body", "Another old ADR",
    });
    const new = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--body", "new body", "Successor ADR",
    });
    const old_id = std.fmt.allocPrint(arena, "{d}", .{old.id}) catch unreachable;
    const new_id = std.fmt.allocPrint(arena, "{d}", .{new.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "decision", "supersede", old_id, "--by", new_id }));

    // withdraw from superseded must fail.
    const stderr = suite.expectFailure(&.{ "decision", "withdraw", old_id });
    defer gpa.free(stderr);

    const after = suite.mustRunJSON(DecisionJSON, arena, &.{ "decision", "show", old_id, "--json" });
    try std.testing.expectEqualStrings("superseded", after.status);
}

// ============================================================================
// decision-artifact-delegate: error-spelling preservation under delegation
// ============================================================================

test "decision delegate: terminal guard error contains expected wording" {
    // Confirms the error message (TerminalStatus path) is unchanged after
    // validateTransition delegates to policy.status.check.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("dec-delegate-error-spelling");

    const d = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--body", "body", "Spelling test decision",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{d.id}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "decision", "withdraw", id }));

    // Attempting accept from withdrawn (terminal) should fail with "terminal"
    // or equivalent wording — same as before the policy arm delegated.
    const stderr = suite.expectFailure(&.{ "decision", "accept", id });
    defer gpa.free(stderr);

    const guard_ok =
        std.mem.containsAtLeast(u8, stderr, 1, "terminal") or
        std.mem.containsAtLeast(u8, stderr, 1, "withdrawn") or
        std.mem.containsAtLeast(u8, stderr, 1, "cannot");
    if (!guard_ok) {
        std.debug.print(
            "\ndecision terminal-guard stderr lacked expected substring; got:\n{s}\n",
            .{stderr},
        );
        try std.testing.expect(false);
    }
}

// ============================================================================
// artifact-arm-matrix: legal edges accepted
// ============================================================================

test "artifact arm: active → draft is legal (artifact-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("art-active-draft");

    // Create with explicit --status active (CLI default is draft).
    const art = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--kind", "other", "--status", "active", "Active artifact",
    });
    try std.testing.expectEqualStrings("active", art.status);
    const id = std.fmt.allocPrint(arena, "{d}", .{art.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "artifact", "update", id, "--status", "draft" }));

    const after = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("draft", after.status);
}

test "artifact arm: draft → active is legal (artifact-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("art-draft-active");

    const art = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--kind", "other", "--status", "draft", "Draft artifact",
    });
    try std.testing.expectEqualStrings("draft", art.status);
    const id = std.fmt.allocPrint(arena, "{d}", .{art.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "artifact", "update", id, "--status", "active" }));

    const after = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("active", after.status);
}

test "artifact arm: active → superseded is legal (artifact-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("art-active-superseded");

    // Create with --status active so we can test active → superseded.
    const art = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--kind", "tech_spec", "--status", "active", "Tech spec v1",
    });
    try std.testing.expectEqualStrings("active", art.status);
    const id = std.fmt.allocPrint(arena, "{d}", .{art.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "artifact", "update", id, "--status", "superseded" }));

    const after = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("superseded", after.status);
}

test "artifact arm: active → retired is legal (artifact-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("art-active-retired");

    // Create with --status active so we can test active → retired.
    const art = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--kind", "roadmap", "--status", "active", "Old roadmap",
    });
    try std.testing.expectEqualStrings("active", art.status);
    const id = std.fmt.allocPrint(arena, "{d}", .{art.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "artifact", "update", id, "--status", "retired" }));

    const after = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", after.status);
}

// ============================================================================
// artifact-arm-matrix: illegal edges refused, post-state unchanged
// ============================================================================

test "artifact arm: retired → active is refused (terminal guard)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("art-retired-refused");

    // Create active so we can drive to retired (active → retired is legal).
    const art = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--kind", "other", "--status", "active", "Artifact to retire",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{art.id}) catch unreachable;

    // Drive to retired.
    gpa.free(suite.mustRun(&.{ "artifact", "update", id, "--status", "retired" }));

    // Attempt to revive — must fail.
    const stderr = suite.expectFailure(&.{ "artifact", "update", id, "--status", "active" });
    defer gpa.free(stderr);

    // Post-state must be unchanged (still retired).
    const after = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", after.status);
}

test "artifact arm: superseded → draft is refused (terminal guard)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("art-superseded-refused");

    // Create active so we can drive to superseded (active → superseded is legal).
    const art = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--kind", "adr", "--status", "active", "Superseded spec",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{art.id}) catch unreachable;

    // Drive to superseded.
    gpa.free(suite.mustRun(&.{ "artifact", "update", id, "--status", "superseded" }));

    // Attempt to move to draft — must fail.
    const stderr = suite.expectFailure(&.{ "artifact", "update", id, "--status", "draft" });
    defer gpa.free(stderr);

    // Post-state must be unchanged (still superseded).
    const after = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("superseded", after.status);
}

test "artifact arm: draft → superseded is refused (skip-over active)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("art-draft-superseded-refused");

    const art = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--kind", "other", "--status", "draft", "Draft skip test",
    });
    try std.testing.expectEqualStrings("draft", art.status);
    const id = std.fmt.allocPrint(arena, "{d}", .{art.id}) catch unreachable;

    // draft → superseded is not in the matrix (must go through active first).
    const stderr = suite.expectFailure(&.{ "artifact", "update", id, "--status", "superseded" });
    defer gpa.free(stderr);

    // Post-state must be unchanged (still draft).
    const after = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("draft", after.status);
}

test "artifact arm: draft → retired is refused (skip-over active)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("art-draft-retired-refused");

    const art = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--kind", "summary", "--status", "draft", "Draft retire test",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{art.id}) catch unreachable;

    // draft → retired is not in the matrix.
    const stderr = suite.expectFailure(&.{ "artifact", "update", id, "--status", "retired" });
    defer gpa.free(stderr);

    const after = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("draft", after.status);
}

// ============================================================================
// decision-artifact-sweep: existing flows survive consolidation
// ============================================================================

test "decision sweep: existing accept → supersede flow survives delegation" {
    // This is the canonical decision lifecycle that was working before M4.
    // After delegation it must still work end-to-end — no double-enforcement
    // divergence and no behavior change on legal paths.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("dec-sweep-full");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Sweep anchor plan",
    });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // proposed → accepted
    const d1 = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--plan", plan_id, "--body", "rationale", "ADR 001",
    });
    const d1_id = std.fmt.allocPrint(arena, "{d}", .{d1.id}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "decision", "accept", d1_id }));
    const d1_accepted = suite.mustRunJSON(DecisionJSON, arena, &.{ "decision", "show", d1_id, "--json" });
    try std.testing.expectEqualStrings("accepted", d1_accepted.status);
    try std.testing.expect(d1_accepted.decided_at != null);

    // accepted → superseded (via supersede verb)
    const d2 = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--json", "--plan", plan_id, "--body", "revised rationale", "ADR 002",
    });
    const d2_id = std.fmt.allocPrint(arena, "{d}", .{d2.id}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "decision", "supersede", d1_id, "--by", d2_id }));
    const d1_superseded = suite.mustRunJSON(DecisionJSON, arena, &.{ "decision", "show", d1_id, "--json" });
    try std.testing.expectEqualStrings("superseded", d1_superseded.status);

    // proposed → withdrawn (on d2 which is still proposed)
    gpa.free(suite.mustRun(&.{ "decision", "withdraw", d2_id }));
    const d2_withdrawn = suite.mustRunJSON(DecisionJSON, arena, &.{ "decision", "show", d2_id, "--json" });
    try std.testing.expectEqualStrings("withdrawn", d2_withdrawn.status);
}

test "artifact sweep: existing active → retired flow survives delegation" {
    // Verifies that the previously-working artifact update path is identical
    // after validateTransition delegates to the policy arm.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("art-sweep-full");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Artifact sweep plan",
    });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Create with --status active so we can exercise active → draft → active → retired.
    const art = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--plan", plan_id, "--kind", "tech_spec", "--status", "active", "Sweep spec",
    });
    try std.testing.expectEqualStrings("active", art.status);
    const id = std.fmt.allocPrint(arena, "{d}", .{art.id}) catch unreachable;

    // active → draft (round-trip)
    gpa.free(suite.mustRun(&.{ "artifact", "update", id, "--status", "draft" }));
    const at_draft = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("draft", at_draft.status);

    // draft → active
    gpa.free(suite.mustRun(&.{ "artifact", "update", id, "--status", "active" }));
    const at_active = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("active", at_active.status);

    // active → retired
    gpa.free(suite.mustRun(&.{ "artifact", "update", id, "--status", "retired" }));
    const at_retired = suite.mustRunJSON(ArtifactJSON, arena, &.{ "artifact", "show", id, "--json" });
    try std.testing.expectEqualStrings("retired", at_retired.status);
}
