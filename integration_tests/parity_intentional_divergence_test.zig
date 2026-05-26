//! integration_tests/parity_intentional_divergence_test.zig
//!
//! Bucket-3 pinning tests for plan 351. These tests PASS today and
//! pin zig's intentional divergences from the Go archive binary,
//! ensuring Phase 4 / future work doesn't accidentally revert them.
//!
//! Covers:
//! - §D-zig-only-verb: `annotate` is a zig-only verb added after the
//!   Go archive cutoff. Test asserts `annotate --help` works.
//! - §F-q233-add-shape: `task add` / `question add` use positional
//!   <title> with body/scope/plan as flags (Q237 operator decision
//!   2026-05-26: confirm intentional). Test asserts the positional-
//!   title shape is accepted.
//!
//! Allowlist work for the audit rows is a Phase 5 concern (task 2362
//! adds scripts/parity-allowlist.txt). This file pins the contracts.

const std = @import("std");
const harness = @import("harness");

const TaskJSON = struct {
    id: i64,
};
const QuestionJSON = struct {
    id: i64,
};

test "Bucket 3 (D-zig-only-verb): 'annotate --help' exits 0 and renders zig-only verb help" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const stdout = suite.mustRun(&.{ "annotate", "--help" });
    defer gpa.free(stdout);

    // The zig annotate verb's help summary line.
    try std.testing.expect(std.mem.containsAtLeast(u8, stdout, 1, "Manage source annotations"));
}

test "Bucket 3 (F-q233-add-shape): 'task add <positional-title>' accepted (Q237 intentional zig shape)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Positional title (not --title flag) — pin the zig shape.
    const created = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--next-action", "pin", "BUCKET3_TASK_ADD_POSITIONAL",
    });
    try std.testing.expect(created.id > 0);
}

test "Bucket 3 (F-q233-add-shape): 'question add <positional-title>' accepted (Q237 intentional zig shape)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // question add: positional title + body flag, no --plan flag in zig.
    const created = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "add", "--json", "--body", "pin", "BUCKET3_QUESTION_ADD_POSITIONAL",
    });
    try std.testing.expect(created.id > 0);
}
