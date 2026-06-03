//! integration_tests/handoff_resume_validate_test.zig
//!
//! Pins the CLI contract of the handoff create/validate/show/abandon
//! subverbs and `resume validate` — the readiness/handoff entry points the
//! pl-handoff and pl-resume skills call, none of which had black-box
//! coverage (only the combined `handoff --json` ritual and `resume`
//! packet were exercised).
//!
//! Contract pinned:
//!   - `capture snapshot --task <id> --json` produces a snapshot id.
//!   - `handoff create <snapshot-id> --json` opens a `pending` handoff.
//!   - `handoff validate <id> --json` flips it to `validated`.
//!   - `handoff show <id> --json` reflects the validated state.
//!   - `handoff abandon <id> --json` flips a second handoff to `abandoned`.
//!   - `resume validate <task-id> --json` returns resumable=true on a ready
//!     task and exits non-zero on a missing task.
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

const Id = struct { id: i64 };
const Handoff = struct { id: i64, status: []const u8 };
const ResumeValidate = struct { task_id: i64, resumable: bool };

test "scenario: handoff create -> validate -> show, abandon a second; resume validate" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("handoff-resume");

    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "Handoff plan" });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--json", "--plan", pid, "--next-action", "resume here", "Handoff task" });
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    // ---- snapshot to anchor the handoff.
    const snap = suite.mustRunJSON(Id, arena, &.{ "capture", "snapshot", "--task", tid, "--json" });
    const sid = std.fmt.allocPrint(arena, "{d}", .{snap.id}) catch unreachable;

    // ---- create -> validate -> show.
    const created = suite.mustRunJSON(Handoff, arena, &.{ "handoff", "create", sid, "--note", "first", "--json" });
    try std.testing.expectEqualStrings("pending", created.status);
    const hid = std.fmt.allocPrint(arena, "{d}", .{created.id}) catch unreachable;

    const validated = suite.mustRunJSON(Handoff, arena, &.{ "handoff", "validate", hid, "--json" });
    try std.testing.expectEqualStrings("validated", validated.status);

    const shown = suite.mustRunJSON(Handoff, arena, &.{ "handoff", "show", hid, "--json" });
    try std.testing.expectEqual(created.id, shown.id);
    try std.testing.expectEqualStrings("validated", shown.status);

    // ---- abandon a second (still-pending) handoff.
    const created2 = suite.mustRunJSON(Handoff, arena, &.{ "handoff", "create", sid, "--note", "second", "--json" });
    const hid2 = std.fmt.allocPrint(arena, "{d}", .{created2.id}) catch unreachable;
    const abandoned = suite.mustRunJSON(Handoff, arena, &.{ "handoff", "abandon", hid2, "--reason", "superseded", "--json" });
    try std.testing.expectEqualStrings("abandoned", abandoned.status);

    // ---- resume validate: ready task is resumable; missing task fails.
    const rv = suite.mustRunJSON(ResumeValidate, arena, &.{ "resume", "validate", tid, "--json" });
    try std.testing.expectEqual(task.id, rv.task_id);
    try std.testing.expect(rv.resumable);

    const miss = suite.expectFailure(&.{ "resume", "validate", "999999", "--json" });
    gpa.free(miss);
}
