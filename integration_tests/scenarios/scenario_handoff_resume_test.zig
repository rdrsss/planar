//! integration_tests/scenarios/scenario_handoff_resume_test.zig
//!
//! Scenario M13 of plan 352. Handoff / resume two-session
//! workflow: source operator creates handoff from one
//! (vendor, session-id) tuple; resumer operator from a different
//! tuple consumes it; `handoff list / show / abandon` are
//! exercised across the lifecycle.
//!
//! The flagship covered handoff happy-path within a single
//! session. M13 walks the realistic two-session shape — source
//! and destination vendor cookies are distinct.
//!
//! Verbs exercised:
//!     init, plan create, task add, capture session, capture
//!     snapshot, handoff (top-level → creates from active state),
//!     handoff list, handoff show, handoff consume, handoff
//!     abandon (refused after consume).
//!
//! Verifies (roadmap slugs):
//!     [hr/handoff-create-from-session-a] — `planar handoff
//!     --json` from a session with an active task produces a
//!     handoff row with status=validated (or pending) +
//!     resumable=true.
//!     [hr/handoff-list] — `handoff list` surfaces the row.
//!     [hr/handoff-show] — `handoff show <id>` round-trips.
//!     [hr/handoff-consume-resume] — `handoff consume` from a
//!     different vendor session transfers ownership.
//!     [hr/handoff-abandon-refused] — `handoff abandon` after
//!     consume exits non-zero with a documented refusal.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64, title: []const u8, status: []const u8 };
const TaskJSON = struct { id: i64, title: []const u8, status: []const u8 };
const SessionJSON = struct { id: i64 };

const HandoffJSON = struct {
    ok: bool,
    snapshot_id: i64,
    handoff_id: i64,
    status: []const u8,
    resumable: bool,
    failures: []const std.json.Value = &.{},
};

test "scenario: handoff/resume — source creates, resumer consumes, abandon refused after consume" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("hr-flow");

    const src_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_VENDOR", .value = "claude-source" },
        .{ .key = "PLANAR_VENDOR_SESSION_ID", .value = "session-A" },
    };
    const dst_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_VENDOR", .value = "claude-resumer" },
        .{ .key = "PLANAR_VENDOR_SESSION_ID", .value = "session-B" },
    };

    // ---- 1. Plan + task with next_action (required for resume-
    // readiness).
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Handoff target",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",          "add", "--json",
        "--plan",        plan_id_str,
        "--next-action", "finish migration",
        "Finish migration",
    });
    const task_id_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    // ---- 2. Source session: capture session + snapshot.
    const src_sess_raw = suite.mustRunWith(&.{
        "capture", "session", "--json", "--task", task_id_str,
    }, &src_env);
    _ = std.json.parseFromSlice(SessionJSON, arena, src_sess_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(src_sess_raw);

    const src_snap_raw = suite.mustRunWith(&.{
        "capture", "snapshot", "--json", "--task", task_id_str,
    }, &src_env);
    gpa.free(src_snap_raw);

    // ---- 3. Source session: create handoff via `planar handoff
    // --json` (top-level invocation; finds active session for the
    // env-pinned vendor tuple).
    const handoff_raw = suite.mustRunWith(&.{ "handoff", "--json" }, &src_env);
    const handoff = std.json.parseFromSlice(HandoffJSON, arena, handoff_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(handoff_raw);
    try std.testing.expect(handoff.value.ok);
    try std.testing.expect(handoff.value.handoff_id > 0);
    try std.testing.expect(handoff.value.resumable);
    try std.testing.expectEqual(@as(usize, 0), handoff.value.failures.len);

    const handoff_id_str = std.fmt.allocPrint(arena, "{d}", .{handoff.value.handoff_id}) catch unreachable;

    // ---- 4. `handoff list` defaults to status="pending"; we
    // pass --status validated explicitly because handoff create
    // immediately calls validate, leaving the row in `validated`
    // (and out of the default list). The verb is text-only;
    // assert substring.
    const list_out = suite.mustRun(&.{ "handoff", "list", "--status", "validated" });
    defer gpa.free(list_out);
    try std.testing.expect(std.mem.containsAtLeast(u8, list_out, 1, handoff_id_str));

    // ---- 5. `handoff show <id>` round-trips with the snapshot
    // and from-vendor fields.
    const show_out = suite.mustRun(&.{ "handoff", "show", handoff_id_str });
    defer gpa.free(show_out);
    try std.testing.expect(std.mem.containsAtLeast(u8, show_out, 1, handoff_id_str));
    try std.testing.expect(std.mem.containsAtLeast(u8, show_out, 1, "claude-source"));

    // ---- 6. Resumer session: open a different (vendor,
    // session-id) tuple.
    const dst_sess_raw = suite.mustRunWith(&.{
        "capture", "session", "--json", "--task", task_id_str,
    }, &dst_env);
    gpa.free(dst_sess_raw);

    // Consume from the resumer side.
    const consume_out = suite.mustRunWith(&.{
        "handoff", "consume", handoff_id_str,
    }, &dst_env);
    gpa.free(consume_out);

    // ---- 7. After consume, `handoff abandon` must refuse with
    // a non-zero exit and a documented "consumed / terminal /
    // already" wording.
    const abandon_stderr = suite.expectFailure(&.{
        "handoff", "abandon", handoff_id_str, "--reason", "no-op",
    });
    defer gpa.free(abandon_stderr);
    const refusal_ok =
        std.mem.containsAtLeast(u8, abandon_stderr, 1, "consume") or
        std.mem.containsAtLeast(u8, abandon_stderr, 1, "terminal") or
        std.mem.containsAtLeast(u8, abandon_stderr, 1, "already") or
        std.mem.containsAtLeast(u8, abandon_stderr, 1, "non-terminal");
    if (!refusal_ok) {
        std.debug.print("\nhandoff abandon stderr lacked expected wording; got:\n{s}\n", .{abandon_stderr});
        try std.testing.expect(false);
    }
}
