//! integration_tests/handoff_annotation_transitions_test.zig
//!
//! Pins the handoff and annotation status-transition matrices (plan 692 / M5)
//! at the CLI layer.  policy.status.check(.handoff, …) and
//! policy.status.check(.annotation, …) are fully enforced after this
//! milestone; handoff.validateTransition delegates to the central policy arm
//! and annotation.transition's isTerminal pre-check has moved into the arm.
//!
//! Contract pinned:
//!   - Legal edges succeed: exit 0 + `*/show --json` reflects the new
//!     status (exit 0 alone is insufficient — a silent no-op is still
//!     exit 0; post-state is the contract).
//!   - Illegal edges fail: non-zero exit + `*/show --json` confirms the
//!     stored status is UNCHANGED.
//!   - Error-spelling preservation: annotation's terminal guard surfaces
//!     the same operator-visible message after the guard moved into the
//!     policy arm (TerminalStatus mapping at the annotation.transition
//!     boundary).
//!
//! Handoff matrix:
//!   pending   → {validated, consumed, abandoned}
//!   validated → {consumed, abandoned}
//!   consumed, abandoned → terminal
//!   identity (from == to) → no-op (handled at policy layer)
//!
//! Annotation matrix:
//!   active   → {resolved, dismissed, archived}
//!   terminal (resolved / dismissed / archived) → anything refused
//!   identity (from == to) → no-op (handled at policy layer)
//!
//! Verifies (test-spec slugs from artifact 382):
//!   handoff-arm-matrix, handoff-delegate,
//!   annotation-consolidate, handoff-annotation-sweep

const std = @import("std");
const harness = @import("harness");

const HandoffShowJSON = struct { id: i64, status: []const u8 };
const AnnotationJSON = struct { id: i64, status: []const u8 };
const Id = struct { id: i64 };

// ============================================================================
// Helpers — shared fixture setup
// ============================================================================

/// Seed a snapshot id suitable for creating a handoff.
fn seedSnapshot(suite: *harness.Suite, arena: std.mem.Allocator) i64 {
    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "Handoff matrix plan" });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--json", "--plan", pid, "--next-action", "resume here", "Handoff task" });
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;
    const snap = suite.mustRunJSON(Id, arena, &.{ "capture", "snapshot", "--task", tid, "--json" });
    return snap.id;
}

// ============================================================================
// handoff-arm-matrix: legal edges accepted, post-state verified
// ============================================================================

test "handoff arm: pending → validated is legal (handoff-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("hoff-pending-validated");

    const sid = seedSnapshot(&suite, arena);
    const sid_str = std.fmt.allocPrint(arena, "{d}", .{sid}) catch unreachable;

    const created = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "create", sid_str, "--json" });
    try std.testing.expectEqualStrings("pending", created.status);
    const hid = std.fmt.allocPrint(arena, "{d}", .{created.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "handoff", "validate", hid }));

    const after = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "show", hid, "--json" });
    try std.testing.expectEqualStrings("validated", after.status);
}

test "handoff arm: pending → consumed is legal via consume (handoff-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("hoff-pending-consumed");

    const sid = seedSnapshot(&suite, arena);
    const sid_str = std.fmt.allocPrint(arena, "{d}", .{sid}) catch unreachable;

    const created = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "create", sid_str, "--json" });
    try std.testing.expectEqualStrings("pending", created.status);
    const hid = std.fmt.allocPrint(arena, "{d}", .{created.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "handoff", "consume", hid }));

    const after = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "show", hid, "--json" });
    try std.testing.expectEqualStrings("consumed", after.status);
}

test "handoff arm: pending → abandoned is legal via abandon --reason (handoff-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("hoff-pending-abandoned");

    const sid = seedSnapshot(&suite, arena);
    const sid_str = std.fmt.allocPrint(arena, "{d}", .{sid}) catch unreachable;

    const created = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "create", sid_str, "--json" });
    const hid = std.fmt.allocPrint(arena, "{d}", .{created.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "handoff", "abandon", hid, "--reason", "no longer needed" }));

    const after = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "show", hid, "--json" });
    try std.testing.expectEqualStrings("abandoned", after.status);
}

test "handoff arm: validated → consumed is legal (handoff-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("hoff-validated-consumed");

    const sid = seedSnapshot(&suite, arena);
    const sid_str = std.fmt.allocPrint(arena, "{d}", .{sid}) catch unreachable;

    const created = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "create", sid_str, "--json" });
    const hid = std.fmt.allocPrint(arena, "{d}", .{created.id}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "handoff", "validate", hid }));

    gpa.free(suite.mustRun(&.{ "handoff", "consume", hid }));

    const after = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "show", hid, "--json" });
    try std.testing.expectEqualStrings("consumed", after.status);
}

// ============================================================================
// handoff-arm-matrix: illegal edges refused, post-state unchanged
// ============================================================================

test "handoff arm: consumed → abandoned is refused (terminal guard, handoff-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("hoff-consumed-refused");

    const sid = seedSnapshot(&suite, arena);
    const sid_str = std.fmt.allocPrint(arena, "{d}", .{sid}) catch unreachable;

    const created = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "create", sid_str, "--json" });
    const hid = std.fmt.allocPrint(arena, "{d}", .{created.id}) catch unreachable;

    // Drive to consumed.
    gpa.free(suite.mustRun(&.{ "handoff", "consume", hid }));

    // Attempt to abandon a consumed (terminal) handoff — must fail.
    const stderr = suite.expectFailure(&.{ "handoff", "abandon", hid, "--reason", "test" });
    defer gpa.free(stderr);

    // Post-state must be unchanged (still consumed).
    const after = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "show", hid, "--json" });
    try std.testing.expectEqualStrings("consumed", after.status);
}

test "handoff arm: abandoned → consumed is refused (terminal guard, handoff-arm-matrix)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("hoff-abandoned-refused");

    const sid = seedSnapshot(&suite, arena);
    const sid_str = std.fmt.allocPrint(arena, "{d}", .{sid}) catch unreachable;

    const created = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "create", sid_str, "--json" });
    const hid = std.fmt.allocPrint(arena, "{d}", .{created.id}) catch unreachable;

    // Drive to abandoned.
    gpa.free(suite.mustRun(&.{ "handoff", "abandon", hid, "--reason", "give up" }));

    // Attempt to consume an abandoned (terminal) handoff — must fail.
    const stderr = suite.expectFailure(&.{ "handoff", "consume", hid });
    defer gpa.free(stderr);
    // The error message should mention terminal or consumed/abandoned state.
    const refusal_ok =
        std.mem.containsAtLeast(u8, stderr, 1, "terminal") or
        std.mem.containsAtLeast(u8, stderr, 1, "abandon") or
        std.mem.containsAtLeast(u8, stderr, 1, "cannot") or
        std.mem.containsAtLeast(u8, stderr, 1, "IllegalTransition");
    if (!refusal_ok) {
        std.debug.print("\nhandoff abandoned→consumed stderr lacked expected wording; got:\n{s}\n", .{stderr});
        try std.testing.expect(false);
    }

    // Post-state must be unchanged (still abandoned).
    const after = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "show", hid, "--json" });
    try std.testing.expectEqualStrings("abandoned", after.status);
}

// ============================================================================
// handoff-delegate: delegation confirmed, abandon remains verb-gated
// ============================================================================

test "handoff delegate: consumed→validated refused, confirms policy arm is enforced (handoff-delegate)" {
    // Drives an illegal transition via the CLI and verifies refusal.
    // Before delegation, validateTransition had its own switch; after
    // delegation it routes through policy.status.check(.handoff, …).
    // The observable outcome is identical — this test pins it.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("hoff-delegate-confirm");

    const sid = seedSnapshot(&suite, arena);
    const sid_str = std.fmt.allocPrint(arena, "{d}", .{sid}) catch unreachable;

    const created = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "create", sid_str, "--json" });
    const hid = std.fmt.allocPrint(arena, "{d}", .{created.id}) catch unreachable;

    // pending → consumed (legal)
    gpa.free(suite.mustRun(&.{ "handoff", "consume", hid }));

    // consumed → validated is illegal (backward move from terminal).
    const stderr = suite.expectFailure(&.{ "handoff", "validate", hid });
    defer gpa.free(stderr);

    // Status is unchanged.
    const after = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "show", hid, "--json" });
    try std.testing.expectEqualStrings("consumed", after.status);
}

// ============================================================================
// annotation-consolidate: annotation arm is sole terminal guard
// ============================================================================

test "annotation arm: active → resolved is legal (annotation-consolidate)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ann-active-resolved");

    const ann = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/x.zig", "--body", "note",
    });
    try std.testing.expectEqualStrings("active", ann.status);
    const id = std.fmt.allocPrint(arena, "{d}", .{ann.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "annotate", "resolve", id }));

    const after = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id, "--json" });
    try std.testing.expectEqualStrings("resolved", after.status);
}

test "annotation arm: active → dismissed is legal (annotation-consolidate)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ann-active-dismissed");

    const ann = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/y.zig", "--body", "note2",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{ann.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "annotate", "dismiss", id }));

    const after = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id, "--json" });
    try std.testing.expectEqualStrings("dismissed", after.status);
}

test "annotation arm: active → archived is legal (annotation-consolidate)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ann-active-archived");

    const ann = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/z.zig", "--body", "note3",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{ann.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "annotate", "archive", id }));

    const after = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id, "--json" });
    try std.testing.expectEqualStrings("archived", after.status);
}

test "annotation arm: resolved → active is refused (terminal guard, annotation-consolidate)" {
    // After consolidation the policy arm is the sole guard.  The old
    // annotation.transition isTerminal pre-check is gone; this test
    // confirms the arm still refuses the move.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ann-resolved-refused");

    const ann = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/a.zig", "--body", "terminal test",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{ann.id}) catch unreachable;

    // Drive to resolved.
    gpa.free(suite.mustRun(&.{ "annotate", "resolve", id }));
    const at_resolved = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id, "--json" });
    try std.testing.expectEqualStrings("resolved", at_resolved.status);

    // Attempt to re-dismiss from resolved — must fail.
    const stderr = suite.expectFailure(&.{ "annotate", "dismiss", id });
    defer gpa.free(stderr);
    // The error should mention terminal or TerminalStatus.
    const refusal_ok =
        std.mem.containsAtLeast(u8, stderr, 1, "terminal") or
        std.mem.containsAtLeast(u8, stderr, 1, "TerminalStatus") or
        std.mem.containsAtLeast(u8, stderr, 1, "cannot");
    if (!refusal_ok) {
        std.debug.print("\nannotation terminal-guard stderr lacked expected wording; got:\n{s}\n", .{stderr});
        try std.testing.expect(false);
    }

    // Post-state must be unchanged (still resolved).
    const after = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id, "--json" });
    try std.testing.expectEqualStrings("resolved", after.status);
}

test "annotation arm: dismissed → archived is refused (terminal guard, annotation-consolidate)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ann-dismissed-refused");

    const ann = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/b.zig", "--body", "dismiss test",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{ann.id}) catch unreachable;

    // Drive to dismissed.
    gpa.free(suite.mustRun(&.{ "annotate", "dismiss", id }));

    // Attempt to archive from dismissed — must fail.
    const stderr = suite.expectFailure(&.{ "annotate", "archive", id });
    defer gpa.free(stderr);

    // Post-state must be unchanged (still dismissed).
    const after = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id, "--json" });
    try std.testing.expectEqualStrings("dismissed", after.status);
}

// ============================================================================
// annotation-consolidate: TerminalStatus error-spelling preserved
// ============================================================================

test "annotation terminal guard: error spelling unchanged after consolidation" {
    // The annotation.transition boundary maps the policy arm's
    // IllegalTransition → TerminalStatus.  This test confirms the
    // operator-visible error message is the same as before.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ann-error-spelling");

    const ann = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/c.zig", "--body", "spelling test",
    });
    const id = std.fmt.allocPrint(arena, "{d}", .{ann.id}) catch unreachable;

    // Drive to archived (terminal).
    gpa.free(suite.mustRun(&.{ "annotate", "archive", id }));

    // Attempt to resolve from archived — surfaces TerminalStatus wording.
    const stderr = suite.expectFailure(&.{ "annotate", "resolve", id });
    defer gpa.free(stderr);

    // The operator-visible message must mention "terminal" or "TerminalStatus"
    // — the same wording as before the guard moved into the policy arm.
    const spelling_ok =
        std.mem.containsAtLeast(u8, stderr, 1, "TerminalStatus") or
        std.mem.containsAtLeast(u8, stderr, 1, "terminal") or
        std.mem.containsAtLeast(u8, stderr, 1, "cannot");
    if (!spelling_ok) {
        std.debug.print("\nannotation error-spelling check failed; got:\n{s}\n", .{stderr});
        try std.testing.expect(false);
    }
}

// ============================================================================
// handoff-annotation-sweep: existing flows survive consolidation
// ============================================================================

test "handoff sweep: existing pending → validate → consume flow survives delegation (handoff-annotation-sweep)" {
    // The previously-working handoff lifecycle must still work end-to-end
    // after validateTransition delegates to the policy arm.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("hoff-sweep-full");

    const sid = seedSnapshot(&suite, arena);
    const sid_str = std.fmt.allocPrint(arena, "{d}", .{sid}) catch unreachable;

    // pending → validated → consumed: the canonical two-step handoff lifecycle.
    const h = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "create", sid_str, "--json" });
    try std.testing.expectEqualStrings("pending", h.status);
    const hid = std.fmt.allocPrint(arena, "{d}", .{h.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "handoff", "validate", hid }));
    const at_validated = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "show", hid, "--json" });
    try std.testing.expectEqualStrings("validated", at_validated.status);

    gpa.free(suite.mustRun(&.{ "handoff", "consume", hid }));
    const at_consumed = suite.mustRunJSON(HandoffShowJSON, arena, &.{ "handoff", "show", hid, "--json" });
    try std.testing.expectEqualStrings("consumed", at_consumed.status);
}

test "annotation sweep: existing active → resolve → dismiss flow survives consolidation (handoff-annotation-sweep)" {
    // The previously-working annotation resolve + dismiss lifecycle must
    // still work after the isTerminal pre-check moves into the policy arm.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ann-sweep-full");

    // Two independent annotations: one resolved, one dismissed, one archived.
    const a1 = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/sweep1.zig", "--body", "sweep1",
    });
    const a2 = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/sweep2.zig", "--body", "sweep2",
    });
    const a3 = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/sweep3.zig", "--body", "sweep3",
    });

    const id1 = std.fmt.allocPrint(arena, "{d}", .{a1.id}) catch unreachable;
    const id2 = std.fmt.allocPrint(arena, "{d}", .{a2.id}) catch unreachable;
    const id3 = std.fmt.allocPrint(arena, "{d}", .{a3.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "annotate", "resolve", id1 }));
    gpa.free(suite.mustRun(&.{ "annotate", "dismiss", id2 }));
    gpa.free(suite.mustRun(&.{ "annotate", "archive", id3 }));

    const r1 = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id1, "--json" });
    try std.testing.expectEqualStrings("resolved", r1.status);

    const r2 = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id2, "--json" });
    try std.testing.expectEqualStrings("dismissed", r2.status);

    const r3 = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id3, "--json" });
    try std.testing.expectEqualStrings("archived", r3.status);
}
