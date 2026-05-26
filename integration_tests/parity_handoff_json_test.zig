//! integration_tests/parity_handoff_json_test.zig
//!
//! Cluster H-handoff-json-shape parity test for plan 351 (1 row).
//! See parity-triage.md §H-handoff-json-shape.
//!
//! Zig handoff --json returns {ok, snapshot_id, handoff_id, status,
//! resumable}. Go returns the same fields PLUS a `failures` array
//! that lists why a snapshot isn't resumable. The `failures` array
//! is load-bearing for the validate-handoff flow; oncall / scripts
//! that scrape handoff JSON depend on it. Pretty-print vs compact
//! JSON is subordinate Bucket-4 cosmetic and not asserted here.
//!
//! Today: zig's JSON lacks `failures`. Phase 4 fix in
//! src/cmd/planar/handlers/handoff.zig restores the field.

const std = @import("std");
const harness = @import("harness");

const TaskJSON = struct {
    id: i64,
};

test "parity: 'handoff --json' includes 'failures' array field (Cluster H-handoff-json-shape; red until field restored)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Fixture: need a current task for handoff to capture.
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--next-action", "verify", "HANDOFF_JSON_TEST_TASK",
    });

    // Run handoff with --json (default subcommand path).
    const stdout = suite.mustRun(&.{ "handoff", "--json" });
    defer gpa.free(stdout);

    const trimmed = std.mem.trim(u8, stdout, " \n");
    const parsed = std.json.parseFromSlice(std.json.Value, arena, trimmed, .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print(
            "\nhandoff --json: failed to parse output: {s}\nraw: {s}\n",
            .{ @errorName(e), stdout },
        );
        try std.testing.expect(false);
        unreachable;
    };

    try std.testing.expect(parsed.value == .object);
    const obj = parsed.value.object;

    // The contract: `failures` field present, regardless of whether
    // the snapshot is resumable or not (Go emits `[]` when none).
    if (obj.get("failures") == null) {
        std.debug.print(
            "\nhandoff --json missing 'failures' key; raw output:\n{s}\n",
            .{stdout},
        );
        try std.testing.expect(false);
    }
}
