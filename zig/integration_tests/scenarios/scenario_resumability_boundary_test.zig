//! Durable-boundary scenarios for zero-context orchestration recovery.
//!
//! The tests exercise the CLI composition required by classic,
//! barrel-deferred, and parallel-fanout boundaries. They use task state,
//! context snapshots, active worktree claims, and validated handoffs only;
//! fixture state is never written through SQL.

const std = @import("std");
const harness = @import("harness");

const Id = struct { id: i64 };
const TaskState = struct {
    id: i64,
    status: []const u8,
    next_action: ?[]const u8 = null,
};
const ValidationFailure = struct {
    check: []const u8,
    message: []const u8,
    remediation: []const u8,
};
const ResumeValidation = struct {
    task_id: i64,
    resumable: bool,
    failures: ?[]const ValidationFailure = null,
};
const ResumePacket = struct {
    identity: struct { task_id: i64 },
    state: struct {
        status: []const u8,
        next_action: []const u8,
        last_action_body: []const u8,
    },
    active_claim: ?struct { worktree_path: []const u8 = "" } = null,
    from_handoff: ?struct { worktree_path: []const u8 = "" } = null,
};
const Pull = struct {
    claim_token: []const u8,
    task: ?struct { id: i64 } = null,
};
const Handoff = struct {
    handoff_id: i64,
    resumable: bool,
};

fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const value: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, value, "PLANAR_AGENT_BIN=")) {
            return value["PLANAR_AGENT_BIN=".len..];
        }
    }
    @panic("PLANAR_AGENT_BIN not set; run via make test-integration");
}

fn mustRunAgent(
    suite: *const harness.Suite,
    arena: std.mem.Allocator,
    comptime T: type,
    args: []const []const u8,
) T {
    const gpa = suite.allocator;
    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(gpa);
    argv.append(gpa, resolveAgentBin()) catch @panic("OOM");
    for (args) |arg| argv.append(gpa, arg) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var count: usize = 0;
    while (raw[count] != null) : (count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .environ_map = &env_map,
    }) catch |err| std.debug.panic("planar-agent spawn failed: {s}", .{@errorName(err)});
    defer gpa.free(result.stdout);
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("planar-agent failed: stdout={s} stderr={s}\n", .{ result.stdout, result.stderr });
        @panic("planar-agent failed");
    }
    const parsed = std.json.parseFromSlice(T, arena, result.stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |err| {
        std.debug.print("planar-agent JSON decode failed: {s}\nraw={s}\n", .{ @errorName(err), result.stdout });
        @panic("planar-agent JSON decode failed");
    };
    return parsed.value;
}

fn checkpoint(
    suite: *const harness.Suite,
    arena: std.mem.Allocator,
    task_id: []const u8,
    next_action: []const u8,
    note: []const u8,
) void {
    _ = suite.mustRunJSON(Id, arena, &.{
        "task", "update", task_id, "--next-action", next_action, "--json",
    });
    _ = suite.mustRunJSON(Id, arena, &.{
        "capture", "snapshot", "--task", task_id, "--next-action", next_action, "--note", note, "--json",
    });
}

fn expectValidationFailure(
    suite: *const harness.Suite,
    arena: std.mem.Allocator,
    task_id: []const u8,
    expected_check: []const u8,
) !ValidationFailure {
    const result = suite.exec(&.{ "resume", "validate", task_id, "--json" });
    defer result.deinit(suite.allocator);
    try std.testing.expect(result.term == .exited and result.term.exited != 0);
    const parsed = try std.json.parseFromSlice(ResumeValidation, arena, result.stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    });
    try std.testing.expect(!parsed.value.resumable);
    const failures = parsed.value.failures orelse return error.TestUnexpectedResult;
    for (failures) |failure| {
        if (std.mem.eql(u8, failure.check, expected_check)) return failure;
    }
    return error.TestUnexpectedResult;
}

test "scenario: classic barrel and fanout checkpoints resume from zero context" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("durable-strategy-resume");
    const mappings = [_]struct {
        slug: []const u8,
        stage: []const u8,
        result: []const u8,
        worktree: []const u8,
    }{
        .{ .slug = "classic", .stage = "reviewer-decision", .result = "request-changes", .worktree = "/tmp/planar-durable/classic" },
        .{ .slug = "barrel", .stage = "verified-slice", .result = "slice-verified", .worktree = "/tmp/planar-durable/barrel" },
    };

    for (mappings) |mapping| {
        const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--slug", mapping.slug, "--json", mapping.slug });
        const plan_id = try std.fmt.allocPrint(arena, "{d}", .{plan.id});
        const exact_action = try std.fmt.allocPrint(arena, "run {s} verification and observe reviewer approval", .{mapping.slug});
        const task = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--plan", plan_id, "--next-action", exact_action, "--json", mapping.slug });
        const task_id = try std.fmt.allocPrint(arena, "{d}", .{task.id});
        const pull = mustRunAgent(&suite, arena, Pull, &.{ "pull", plan_id, "--no-locality-probe", "--worktree", mapping.worktree, "--json" });
        try std.testing.expect(pull.claim_token.len > 0);
        const doing = suite.mustRunJSON(TaskState, arena, &.{ "task", "show", task_id, "--json" });
        try std.testing.expectEqualStrings("doing", doing.status);

        const note = try std.fmt.allocPrint(
            arena,
            "orchestration_checkpoint: v1\nstage: {s}\niteration_scope: {s}\niteration: {d}\nresult: {s}",
            .{ mapping.stage, if (std.mem.eql(u8, mapping.slug, "classic")) "coder-review" else "none", if (std.mem.eql(u8, mapping.slug, "classic")) @as(u8, 1) else 0, mapping.result },
        );
        checkpoint(&suite, arena, task_id, exact_action, note);
        const validation = suite.mustRunJSON(ResumeValidation, arena, &.{ "resume", "validate", task_id, "--json" });
        try std.testing.expect(validation.resumable);
        const packet = suite.mustRunJSON(ResumePacket, arena, &.{ "resume", task_id, "--json" });
        try std.testing.expectEqualStrings("doing", packet.state.status);
        try std.testing.expectEqualStrings(exact_action, packet.state.next_action);
        try std.testing.expectEqualStrings(note, packet.state.last_action_body);
        try std.testing.expectEqualStrings(mapping.worktree, packet.active_claim.?.worktree_path);
    }

    const fanout_plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--slug", "fanout", "--json", "fanout" });
    const fanout_plan_id = try std.fmt.allocPrint(arena, "{d}", .{fanout_plan.id});
    const fanout_action = "run fanout reconcile for the remaining lane and observe a green barrier";
    const fanout_task = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--plan", fanout_plan_id, "--next-action", fanout_action, "--json", "fanout" });
    const fanout_task_id = try std.fmt.allocPrint(arena, "{d}", .{fanout_task.id});
    const fanout_worktree = "/tmp/planar-durable/fanout";
    const fanout_pull = mustRunAgent(&suite, arena, Pull, &.{ "pull", fanout_plan_id, "--no-locality-probe", "--worktree", fanout_worktree, "--json" });
    const fanout_note = "orchestration_checkpoint: v1\nstage: wave-barrier\niteration_scope: none\niteration: 0\nresult: wave-partial";
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_VENDOR", .value = "fanout-fixture" },
        .{ .key = "PLANAR_VENDOR_SESSION_ID", .value = "fanout-zero-context" },
    };
    gpa.free(suite.mustRunWith(&.{ "capture", "session", "--task", fanout_task_id }, &env));
    const handoff_raw = suite.mustRunWith(&.{ "handoff", fanout_task_id, "--note", fanout_note, "--json" }, &env);
    defer gpa.free(handoff_raw);
    const handoff_parsed = try std.json.parseFromSlice(Handoff, arena, handoff_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    });
    const handoff = handoff_parsed.value;
    try std.testing.expect(handoff.resumable);
    _ = mustRunAgent(&suite, arena, std.json.Value, &.{ "release", "--claim", fanout_pull.claim_token, "--json" });
    const released = suite.mustRunJSON(TaskState, arena, &.{ "task", "show", fanout_task_id, "--json" });
    try std.testing.expectEqualStrings("todo", released.status);

    const fanout_validation = suite.mustRunJSON(ResumeValidation, arena, &.{ "resume", "validate", fanout_task_id, "--json" });
    try std.testing.expect(fanout_validation.resumable);
    const fanout_packet = suite.mustRunJSON(ResumePacket, arena, &.{ "resume", fanout_task_id, "--json" });
    try std.testing.expect(fanout_packet.active_claim == null);
    try std.testing.expectEqualStrings(fanout_worktree, fanout_packet.from_handoff.?.worktree_path);
    try std.testing.expectEqualStrings(fanout_action, fanout_packet.state.next_action);
    try std.testing.expectEqualStrings(fanout_note, fanout_packet.state.last_action_body);
}

test "scenario: mixed boundary repairs invalid lanes without rolling back healthy or terminal lanes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("durable-mixed-boundary");
    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "mixed boundary" });
    const plan_id = try std.fmt.allocPrint(arena, "{d}", .{plan.id});
    const terminal_plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "terminal boundary lane" });
    const terminal_plan_id = try std.fmt.allocPrint(arena, "{d}", .{terminal_plan.id});
    const healthy = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--plan", plan_id, "--next-action", "run healthy lane gate and observe success", "--json", "healthy" });
    const missing_action = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--plan", plan_id, "--json", "missing action" });
    const missing_snapshot = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--plan", plan_id, "--next-action", "capture the missing snapshot and observe resume validation", "--json", "missing snapshot" });
    const done = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--plan", terminal_plan_id, "--next-action", "terminal", "--json", "done lane" });
    const cancelled = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--plan", plan_id, "--next-action", "terminal", "--json", "cancelled lane" });
    const healthy_id = try std.fmt.allocPrint(arena, "{d}", .{healthy.id});
    const missing_action_id = try std.fmt.allocPrint(arena, "{d}", .{missing_action.id});
    const missing_snapshot_id = try std.fmt.allocPrint(arena, "{d}", .{missing_snapshot.id});
    const done_id = try std.fmt.allocPrint(arena, "{d}", .{done.id});
    const cancelled_id = try std.fmt.allocPrint(arena, "{d}", .{cancelled.id});

    const healthy_note = "orchestration_checkpoint: v1\nstage: wave-barrier\niteration_scope: none\niteration: 0\nresult: wave-partial";
    checkpoint(&suite, arena, healthy_id, "run healthy lane gate and observe success", healthy_note);
    _ = suite.mustRunJSON(Id, arena, &.{ "capture", "snapshot", "--task", missing_action_id, "--note", "missing next action", "--json" });
    const done_pull = mustRunAgent(&suite, arena, Pull, &.{ "pull", terminal_plan_id, "--no-locality-probe", "--json" });
    try std.testing.expectEqual(done.id, done_pull.task.?.id);
    _ = mustRunAgent(&suite, arena, std.json.Value, &.{ "complete", "--claim", done_pull.claim_token, "--summary", "terminal fixture completed atomically", "--json" });
    try std.testing.expectEqualStrings("done", (suite.mustRunJSON(TaskState, arena, &.{ "task", "show", done_id, "--json" })).status);
    _ = suite.mustRunJSON(TaskState, arena, &.{ "task", "cancel", cancelled_id, "--json" });

    const healthy_validation = suite.mustRunJSON(ResumeValidation, arena, &.{ "resume", "validate", healthy_id, "--json" });
    try std.testing.expect(healthy_validation.resumable);
    const action_failure = try expectValidationFailure(&suite, arena, missing_action_id, "next_action");
    try std.testing.expectEqualStrings("next_action is null", action_failure.message);
    try std.testing.expect(std.mem.indexOf(u8, action_failure.remediation, "--next-action \"<text>\"") != null);
    const snapshot_failure = try expectValidationFailure(&suite, arena, missing_snapshot_id, "snapshot");
    try std.testing.expectEqualStrings("no context snapshot found", snapshot_failure.message);
    try std.testing.expect(std.mem.indexOf(u8, snapshot_failure.remediation, "capture snapshot --task") != null);

    const healthy_packet = suite.mustRunJSON(ResumePacket, arena, &.{ "resume", healthy_id, "--json" });
    try std.testing.expectEqualStrings(healthy_note, healthy_packet.state.last_action_body);
    try std.testing.expectEqualStrings("todo", healthy_packet.state.status);
    try std.testing.expectEqualStrings("todo", (suite.mustRunJSON(TaskState, arena, &.{ "task", "show", missing_action_id, "--json" })).status);
    try std.testing.expectEqualStrings("todo", (suite.mustRunJSON(TaskState, arena, &.{ "task", "show", missing_snapshot_id, "--json" })).status);

    const done_packet = suite.mustRunJSON(ResumePacket, arena, &.{ "resume", done_id, "--json" });
    try std.testing.expectEqualStrings("done", done_packet.state.status);
    try std.testing.expectEqualStrings("", done_packet.state.last_action_body);
    const done_snapshot_failure = try expectValidationFailure(&suite, arena, done_id, "snapshot");
    try std.testing.expectEqualStrings("no context snapshot found", done_snapshot_failure.message);
    const cancelled_packet = suite.mustRunJSON(ResumePacket, arena, &.{ "resume", cancelled_id, "--json" });
    try std.testing.expectEqualStrings("cancelled", cancelled_packet.state.status);
    try std.testing.expectEqualStrings("", cancelled_packet.state.last_action_body);

    const repaired_action = "edit the missing-action lane and observe resume validation succeed";
    const repaired_action_note = "orchestration_checkpoint: v1\nstage: failure\niteration_scope: none\niteration: 0\nresult: repaired-next-action";
    checkpoint(&suite, arena, missing_action_id, repaired_action, repaired_action_note);
    _ = suite.mustRunJSON(Id, arena, &.{ "capture", "snapshot", "--task", missing_snapshot_id, "--note", "orchestration_checkpoint: v1\nstage: failure\niteration_scope: none\niteration: 0\nresult: repaired-snapshot", "--json" });
    try std.testing.expect((suite.mustRunJSON(ResumeValidation, arena, &.{ "resume", "validate", missing_action_id, "--json" })).resumable);
    try std.testing.expect((suite.mustRunJSON(ResumeValidation, arena, &.{ "resume", "validate", missing_snapshot_id, "--json" })).resumable);
    try std.testing.expect((suite.mustRunJSON(ResumeValidation, arena, &.{ "resume", "validate", healthy_id, "--json" })).resumable);

    const repaired_packet = suite.mustRunJSON(ResumePacket, arena, &.{ "resume", missing_action_id, "--json" });
    try std.testing.expectEqualStrings(repaired_action, repaired_packet.state.next_action);
    try std.testing.expectEqualStrings(repaired_action_note, repaired_packet.state.last_action_body);
}
