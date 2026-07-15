//! integration_tests/run_lifecycle_test.zig — black-box integration tests
//! for the `planar run *` operational verb group (task 4106, plan 638).
//!
//! Tests walk the operational run lifecycle via the compiled binary and
//! assert on JSON output from `run show --json`. Exit code 0 alone is not
//! sufficient — JSON shape and content are verified.
//!
//! Covered scenarios:
//!
//!   - Full lifecycle: start → event × 2 → finish → show --json; asserts
//!     status, ordered events, and terminal status.
//!   - run event emits JSON with auto-incremented seq.
//!   - run finish emits JSON with run_uid + status.
//!   - run show text mode (no --json) contains run_uid and arm.
//!   - Invalid --status on `run finish` exits non-zero (enum guard).
//!   - Missing run_uid on `run event` exits non-zero (not found).
//!   - `run show` on a missing uid exits non-zero (not found).
//!   - `run start --workflow` stores workflow name as arm.

const std = @import("std");
const harness = @import("harness");

// -------------------------------------------------------------------------
// JSON shapes for run show output.
// -------------------------------------------------------------------------

const StartOut = struct {
    run_uid: []const u8 = "",
    plan_id: i64 = 0,
    arm: []const u8 = "",
};

const EventOut = struct {
    run_uid: []const u8 = "",
    seq: i64 = 0,
    kind: []const u8 = "",
};

const FinishOut = struct {
    run_uid: []const u8 = "",
    status: []const u8 = "",
};

const ShowRun = struct {
    id: i64 = 0,
    run_uid: []const u8 = "",
    plan_id: i64 = 0,
    arm: []const u8 = "",
    status: []const u8 = "",
    started_at: []const u8 = "",
    ended_at: ?[]const u8 = null,
    events: []const ShowEvent = &.{},
};

const ShowEvent = struct {
    id: i64 = 0,
    seq: i64 = 0,
    kind: []const u8 = "",
    payload: ?std.json.Value = null,
    created_at: []const u8 = "",
};

const PlanId = struct {
    id: i64,
    scope_kind: []const u8,
    scope_id: ?i64 = null,
};

// -------------------------------------------------------------------------
// Helper
// -------------------------------------------------------------------------

/// Seed a project + plan; return the plan id as a decimal string (arena-owned).
fn seedPlan(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    const root = suite.registerProject("run-test");
    const p = suite.mustRunInDir(root, &.{ "plan", "create", "--json", "--scope", "global", "Run test plan" });
    defer suite.allocator.free(p);
    const plan = std.json.parseFromSlice(PlanId, arena, p, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch @panic("parseFromSlice PlanId failed");
    std.testing.expectEqualStrings("global", plan.value.scope_kind) catch
        @panic("run lifecycle seed plan must be globally owned");
    std.testing.expect(plan.value.scope_id == null) catch
        @panic("globally owned run lifecycle seed plan must not have a scope id");
    return std.fmt.allocPrint(arena, "{d}", .{plan.value.id}) catch @panic("OOM");
}

// -------------------------------------------------------------------------
// Test 1: Full lifecycle start → event × 2 → finish → show --json
// -------------------------------------------------------------------------

test "run: full lifecycle start → event × 2 → finish → show --json" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan_id = seedPlan(&suite, arena);

    // Step 1: run start — emits JSON with run_uid.
    const start_out = suite.mustRunJSON(StartOut, arena, &.{
        "run", "start", "--plan", plan_id,
    });
    const run_uid = start_out.run_uid;
    try std.testing.expect(run_uid.len > 0);
    try std.testing.expectEqualStrings("op", start_out.arm);

    // Step 2: run event #1 (with payload).
    const ev1_out = suite.mustRunJSON(EventOut, arena, &.{
        "run",                 "event",      run_uid,
        "--kind",              "step_start", "--payload",
        "{\"step\":\"plan\"}",
    });
    try std.testing.expectEqual(@as(i64, 1), ev1_out.seq);
    try std.testing.expectEqualStrings("step_start", ev1_out.kind);

    // Step 3: run event #2 (without payload — seq must be 2).
    const ev2_out = suite.mustRunJSON(EventOut, arena, &.{
        "run",    "event",    run_uid,
        "--kind", "step_end",
    });
    try std.testing.expectEqual(@as(i64, 2), ev2_out.seq);
    try std.testing.expectEqualStrings("step_end", ev2_out.kind);

    // Step 4: run finish.
    const finish_out = suite.mustRunJSON(FinishOut, arena, &.{
        "run",      "finish",    run_uid,
        "--status", "completed",
    });
    try std.testing.expectEqualStrings(run_uid, finish_out.run_uid);
    try std.testing.expectEqualStrings("completed", finish_out.status);

    // Step 5: run show --json; assert full shape.
    const show = suite.mustRunJSON(ShowRun, arena, &.{
        "run", "show", run_uid, "--json",
    });
    try std.testing.expectEqualStrings(run_uid, show.run_uid);
    try std.testing.expectEqualStrings("op", show.arm);
    try std.testing.expectEqualStrings("completed", show.status);
    try std.testing.expect(show.ended_at != null);

    // Events ordered by seq.
    try std.testing.expectEqual(@as(usize, 2), show.events.len);
    try std.testing.expectEqual(@as(i64, 1), show.events[0].seq);
    try std.testing.expectEqualStrings("step_start", show.events[0].kind);
    try std.testing.expect(show.events[0].payload != null);
    try std.testing.expectEqual(@as(i64, 2), show.events[1].seq);
    try std.testing.expectEqualStrings("step_end", show.events[1].kind);
    try std.testing.expect(show.events[1].payload == null);
}

// -------------------------------------------------------------------------
// Test 2: --workflow stores as arm
// -------------------------------------------------------------------------

test "run: start --workflow stores workflow name as arm" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan_id = seedPlan(&suite, arena);

    const start_out = suite.mustRunJSON(StartOut, arena, &.{
        "run", "start", "--plan", plan_id, "--workflow", "my-workflow",
    });
    try std.testing.expectEqualStrings("my-workflow", start_out.arm);

    // Confirm via show --json.
    const show = suite.mustRunJSON(ShowRun, arena, &.{
        "run", "show", start_out.run_uid, "--json",
    });
    try std.testing.expectEqualStrings("my-workflow", show.arm);
}

// -------------------------------------------------------------------------
// Test 3: Text output (no --json) — ensure it doesn't crash.
// -------------------------------------------------------------------------

test "run: show without --json emits human-readable text" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan_id = seedPlan(&suite, arena);

    const start_out = suite.mustRunJSON(StartOut, arena, &.{
        "run", "start", "--plan", plan_id,
    });
    const run_uid = start_out.run_uid;

    const text = suite.mustRun(&.{ "run", "show", run_uid });
    defer gpa.free(text);
    try std.testing.expect(std.mem.containsAtLeast(u8, text, 1, run_uid));
    try std.testing.expect(std.mem.containsAtLeast(u8, text, 1, "op"));
    try std.testing.expect(std.mem.containsAtLeast(u8, text, 1, "running"));
}

// -------------------------------------------------------------------------
// Test 4: Invalid --status on run finish exits non-zero (enum guard).
// -------------------------------------------------------------------------

test "run: invalid finish --status exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan_id = seedPlan(&suite, arena);

    const start_out = suite.mustRunJSON(StartOut, arena, &.{
        "run", "start", "--plan", plan_id,
    });

    const stderr = suite.expectFailure(&.{
        "run",      "finish",         start_out.run_uid,
        "--status", "invalid-status",
    });
    defer gpa.free(stderr);
    try std.testing.expect(stderr.len > 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "invalid-status"));
}

// -------------------------------------------------------------------------
// Test 5: run event on a missing uid exits non-zero.
// -------------------------------------------------------------------------

test "run: event on missing uid exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    _ = suite.registerProject("run-event-missing");

    const stderr = suite.expectFailure(&.{
        "run",    "event",      "no-such-run-uid-aaaa",
        "--kind", "step_start",
    });
    defer gpa.free(stderr);
    try std.testing.expect(stderr.len > 0);
}

// -------------------------------------------------------------------------
// Test 6: run show on a missing uid exits non-zero.
// -------------------------------------------------------------------------

test "run: show on missing uid exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    _ = suite.registerProject("run-show-missing");

    const stderr = suite.expectFailure(&.{ "run", "show", "no-such-run-uid-bbbb" });
    defer gpa.free(stderr);
    try std.testing.expect(stderr.len > 0);
}
