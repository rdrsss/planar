//! integration_tests/m5_readcomp_workflow_test.zig
//!
//! Black-box integration tests for the plan 638 M5 read-composition workflows:
//!   workflows/status.lua   — scope + plans + tasks + questions → composed summary
//!   workflows/health.lua   — planar health → forwarded health object
//!   workflows/resume.lua   — planar resume → task resume packet
//!   workflows/handoff.lua  — capture snapshot + handoff create/validate (write)
//!
//! Each test seeds a realistic fixture via the harness planar CLI, runs the
//! workflow via planar-execute, and asserts on the composed output shape +
//! post-state — NOT on exit code alone.
//!
//! Pattern: mirrors finalize_closeout_workflow_test.zig.
//! - Isolated PLANAR_DB per test via harness Suite.
//! - PATH prefixed with harness planar bin dir so engine's cli.planar resolves.
//! - PLANAR_EXECUTE_BIN env var points at the built planar-execute binary.

const std = @import("std");
const harness = @import("harness");

// =============================================================================
// Shared helpers (duplicated per-file by convention in this suite)
// =============================================================================

fn resolveEnv(comptime key: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key ++ "=")) return s[(key ++ "=").len..];
    }
    @panic(key ++ " is not set. Run via: make test-integration");
}

const RunResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,
    fn deinit(self: RunResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }
};

fn runExecute(
    gpa: std.mem.Allocator,
    cwd: []const u8,
    db_path: []const u8,
    args: []const []const u8,
) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveEnv("PLANAR_EXECUTE_BIN"));
    for (args) |a| try argv.append(gpa, a);

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    try env_map.put("PLANAR_DB", db_path);
    try env_map.put("PLANAR_CONFIG_PATH", "/nonexistent-planar-config.toml");
    try env_map.put("PLANAR_DISABLE_WORKTREE_GATE", "1");

    const planar_bin = resolveEnv("PLANAR_BIN");
    const planar_dir = std.fs.path.dirname(planar_bin) orelse ".";
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ planar_dir, old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .cwd = .{ .path = cwd },
        .environ_map = &env_map,
    });
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr, .gpa = gpa };
}

fn repoRootFromBin(allocator: std.mem.Allocator) ![]const u8 {
    const bin_path = resolveEnv("PLANAR_BIN");
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    return allocator.dupe(u8, d3);
}

fn parseJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) T {
    const trimmed = std.mem.trim(u8, buf, " \n\r\t");
    const parsed = std.json.parseFromSlice(T, arena, trimmed, .{
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nparseJSON failed: {s}\nbuf: {s}\n", .{ @errorName(e), buf });
        @panic("parseJSON failed");
    };
    return parsed.value;
}

// =============================================================================
// JSON shape structs
// =============================================================================

const PlanJSON = struct { id: i64 = 0, title: []const u8 = "", status: []const u8 = "" };
const TaskJSON = struct { id: i64 = 0, title: []const u8 = "", status: []const u8 = "" };
const RunShowJSON = struct {
    run_uid: []const u8 = "",
    status: []const u8 = "",
    events: []const RunEventJSON = &.{},
};
const RunEventJSON = struct { kind: []const u8 = "" };

// =============================================================================
// Test 1: status workflow — composed output mirrors seeded state
// =============================================================================

test "status workflow: seeded active plan + doing task + open question appear in composed result" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("m5-status");
    suite.addAssoc("m5-status", null);

    // Seed plan/task/question from `root` so they resolve to the m5-status scope.
    // (plan list inside the workflow runs with cwd=root; must seed from the same cwd.)
    const plan_buf = suite.mustRunInDir(root, &.{
        "plan",           "create", "--json", "--status", "active", "--slug", "m5-status-plan",
        "M5 Status Plan",
    });
    defer gpa.free(plan_buf);
    const plan = parseJSON(PlanJSON, arena, plan_buf);
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const task_buf = suite.mustRunInDir(root, &.{
        "task",           "add", "--json", "--plan", pid, "--next-action", "implement feature",
        "M5 Status Task",
    });
    defer gpa.free(task_buf);
    const task = parseJSON(TaskJSON, arena, task_buf);
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    // Advance task to "doing".
    gpa.free(suite.mustRunInDir(root, &.{ "task", "update", "--status", "doing", tid }));

    // Add an open question.
    gpa.free(suite.mustRunInDir(root, &.{ "question", "add", "--plan", pid, "Which API endpoint?" }));

    // Resolve the workflow path.
    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "status.lua" });
    defer gpa.free(wf_path);

    // Build --args: include plan_id to exercise the trace run path.
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    // Run the workflow from the project root so cwd-derived scope resolves.
    const res = try runExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "status", "--args", args_json,
    });
    defer res.deinit();

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("status workflow stderr:\n{s}\nstdout:\n{s}\n", .{ res.stderr, res.stdout });
    }
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    // Parse the flow.result. The status workflow returns scalar counts (no nested
    // object arrays — the engine panics on nested object tables in writeLuaTableJson).
    const StatusResult = struct {
        scope_slug: []const u8 = "",
        active_plan_count: i64 = 0,
        paused_plan_count: i64 = 0,
        todo_task_count: i64 = 0,
        doing_task_count: i64 = 0,
        blocked_task_count: i64 = 0,
        open_question_count: i64 = 0,
        summary: []const u8 = "",
        run_uid: []const u8 = "",
    };
    const status_result = parseJSON(StatusResult, arena, res.stdout);

    // scope_slug must be non-empty (cwd-derived from root).
    if (status_result.scope_slug.len == 0) {
        std.debug.print("status result scope_slug is empty\nraw: {s}\n", .{res.stdout});
    }
    try std.testing.expect(status_result.scope_slug.len > 0);

    // summary must be non-empty.
    try std.testing.expect(status_result.summary.len > 0);

    // We seeded 1 active plan → active_plan_count == 1.
    if (status_result.active_plan_count != 1) {
        std.debug.print("status result active_plan_count expected 1, got {d}\nraw: {s}\n", .{ status_result.active_plan_count, res.stdout });
    }
    try std.testing.expectEqual(@as(i64, 1), status_result.active_plan_count);

    // We seeded 1 doing task → doing_task_count == 1.
    if (status_result.doing_task_count != 1) {
        std.debug.print("status result doing_task_count expected 1, got {d}\nraw: {s}\n", .{ status_result.doing_task_count, res.stdout });
    }
    try std.testing.expectEqual(@as(i64, 1), status_result.doing_task_count);

    // We seeded 1 open question → open_question_count == 1.
    if (status_result.open_question_count != 1) {
        std.debug.print("status result open_question_count expected 1, got {d}\nraw: {s}\n", .{ status_result.open_question_count, res.stdout });
    }
    try std.testing.expectEqual(@as(i64, 1), status_result.open_question_count);

    // run_uid must be present (we passed plan_id → trace run opened).
    if (status_result.run_uid.len == 0) {
        std.debug.print("status result run_uid is empty\nraw: {s}\n", .{res.stdout});
    }
    try std.testing.expect(status_result.run_uid.len > 0);

    // Verify the trace run: status=completed, events contain "reads".
    const run_show = suite.mustRunJSON(RunShowJSON, arena, &.{
        "run", "show", status_result.run_uid, "--json",
    });
    try std.testing.expectEqualStrings("completed", run_show.status);
    var found_reads = false;
    for (run_show.events) |ev| {
        if (std.mem.eql(u8, ev.kind, "reads")) found_reads = true;
    }
    if (!found_reads) {
        std.debug.print("status trace run missing 'reads' event\n", .{});
    }
    try std.testing.expect(found_reads);
}

// =============================================================================
// Test 2: status workflow — empty DB returns empty composition (no crash)
// =============================================================================

test "status workflow: empty scope returns scope_slug and summary with no crash" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("m5-status-empty");
    suite.addAssoc("m5-status-empty", null);

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "status.lua" });
    defer gpa.free(wf_path);

    // No plan_id → no trace run; empty scope → empty lists.
    const res = try runExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "status",
    });
    defer res.deinit();

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("status empty stderr:\n{s}\nstdout:\n{s}\n", .{ res.stderr, res.stdout });
    }
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    // scope_slug and summary must always be present; all counts must be 0.
    const EmptyStatusResult = struct {
        scope_slug: []const u8 = "",
        summary: []const u8 = "",
        active_plan_count: i64 = -1,
        doing_task_count: i64 = -1,
        open_question_count: i64 = -1,
    };
    const er = parseJSON(EmptyStatusResult, arena, res.stdout);
    try std.testing.expect(er.scope_slug.len > 0);
    try std.testing.expect(er.summary.len > 0);
    // Empty scope has no plans/tasks/questions.
    try std.testing.expectEqual(@as(i64, 0), er.active_plan_count);
    try std.testing.expectEqual(@as(i64, 0), er.doing_task_count);
    try std.testing.expectEqual(@as(i64, 0), er.open_question_count);
}

// =============================================================================
// Test 3: health workflow — returns the health verdict
// =============================================================================

const HealthResult = struct {
    db_ok: bool = false,
    schema_current: bool = false,
    integrity_ok: bool = false,
    overall: []const u8 = "",
};

test "health workflow: returns health object with overall=ok on a fresh DB" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("m5-health");
    suite.addAssoc("m5-health", null);

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "health.lua" });
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "health",
    });
    defer res.deinit();

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("health workflow stderr:\n{s}\nstdout:\n{s}\n", .{ res.stderr, res.stdout });
    }
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    const h = parseJSON(HealthResult, arena, res.stdout);

    // A fresh DB is always healthy.
    try std.testing.expect(h.db_ok);
    try std.testing.expect(h.schema_current);
    try std.testing.expect(h.integrity_ok);
    try std.testing.expectEqualStrings("ok", h.overall);
}

// =============================================================================
// Test 4: resume workflow — returns seeded task's resume packet
// =============================================================================

const ResumePacketJSON = struct {
    task_id: i64 = 0,
    title: []const u8 = "",
    status: []const u8 = "",
    next_action: []const u8 = "",
    plan_id: i64 = 0,
};

test "resume workflow: returns the seeded task's resume packet" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("m5-resume");

    // Seed a plan + task with a next-action so the resume packet is populated.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "M5 Resume Plan",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",          "add",                   "--json",         "--plan", pid,
        "--next-action", "implement the feature", "M5 Resume Task",
    });
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "resume.lua" });
    defer gpa.free(wf_path);

    // Build --args.
    const args_json = try std.fmt.allocPrint(gpa, "{{\"task_id\":{s}}}", .{tid});
    defer gpa.free(args_json);

    const res = try runExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "resume", "--args", args_json,
    });
    defer res.deinit();

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("resume workflow stderr:\n{s}\nstdout:\n{s}\n", .{ res.stderr, res.stdout });
    }
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    // The result should be the resume packet: task_id, title, plan_id, next_action.
    const packet = parseJSON(ResumePacketJSON, arena, res.stdout);
    try std.testing.expectEqual(task.id, packet.task_id);
    try std.testing.expectEqualStrings("M5 Resume Task", packet.title);
    try std.testing.expectEqual(plan.id, packet.plan_id);
    try std.testing.expectEqualStrings("implement the feature", packet.next_action);
}

// =============================================================================
// Test 5: resume workflow — exits non-zero on missing task
// =============================================================================

test "resume workflow: exits non-zero when task does not exist" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    _ = suite.registerProject("m5-resume-miss");

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "resume.lua" });
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "resume", "--args", "{\"task_id\":999999}",
    });
    defer res.deinit();

    // Non-zero exit expected: cli.planar_json raises when planar exits non-zero.
    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
}

// =============================================================================
// Test 6: handoff workflow — creates + validates a handoff for a seeded task
// =============================================================================

const HandoffResult = struct {
    handoff_id: i64 = 0,
    snapshot_id: i64 = 0,
    status: []const u8 = "",
    task_id: i64 = 0,
    resumable: bool = false,
    run_uid: []const u8 = "",
};

const HandoffShowJSON = struct {
    id: i64 = 0,
    status: []const u8 = "",
};

test "handoff workflow: creates and validates a handoff, returns validated record + run trace" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("m5-handoff");

    // Seed a plan + task with next-action so resume validate can pass.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "M5 Handoff Plan",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",          "add",                     "--json",          "--plan", pid,
        "--next-action", "finish the handoff test", "M5 Handoff Task",
    });
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "handoff.lua" });
    defer gpa.free(wf_path);

    // Build --args with task_id and a note.
    const args_json = try std.fmt.allocPrint(gpa, "{{\"task_id\":{s},\"note\":\"workflow handoff test\"}}", .{tid});
    defer gpa.free(args_json);

    const res = try runExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "handoff", "--args", args_json,
    });
    defer res.deinit();

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("handoff workflow stderr:\n{s}\nstdout:\n{s}\n", .{ res.stderr, res.stdout });
    }
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    // Parse the flow.result.
    const hresult = parseJSON(HandoffResult, arena, res.stdout);
    try std.testing.expect(hresult.handoff_id > 0);
    try std.testing.expect(hresult.snapshot_id > 0);
    try std.testing.expectEqualStrings("validated", hresult.status);
    try std.testing.expectEqual(task.id, hresult.task_id);
    try std.testing.expect(hresult.run_uid.len > 0);

    // Verify the handoff actually exists in the DB and is validated.
    const hid_str = std.fmt.allocPrint(arena, "{d}", .{hresult.handoff_id}) catch @panic("OOM");
    const h_show = suite.mustRunJSON(HandoffShowJSON, arena, &.{
        "handoff", "show", hid_str, "--json",
    });
    try std.testing.expectEqual(hresult.handoff_id, h_show.id);
    try std.testing.expectEqualStrings("validated", h_show.status);

    // Verify the trace run: completed, events include snapshot-created, handoff-created,
    // handoff-validated, resume-validated.
    const run_show = suite.mustRunJSON(RunShowJSON, arena, &.{
        "run", "show", hresult.run_uid, "--json",
    });
    try std.testing.expectEqualStrings("completed", run_show.status);

    var found_snap = false;
    var found_create = false;
    var found_validate = false;
    var found_rv = false;
    for (run_show.events) |ev| {
        if (std.mem.eql(u8, ev.kind, "snapshot-created")) found_snap = true;
        if (std.mem.eql(u8, ev.kind, "handoff-created")) found_create = true;
        if (std.mem.eql(u8, ev.kind, "handoff-validated")) found_validate = true;
        if (std.mem.eql(u8, ev.kind, "resume-validated")) found_rv = true;
    }
    if (!found_snap) std.debug.print("handoff trace run missing 'snapshot-created' event\n", .{});
    if (!found_create) std.debug.print("handoff trace run missing 'handoff-created' event\n", .{});
    if (!found_validate) std.debug.print("handoff trace run missing 'handoff-validated' event\n", .{});
    if (!found_rv) std.debug.print("handoff trace run missing 'resume-validated' event\n", .{});

    try std.testing.expect(found_snap);
    try std.testing.expect(found_create);
    try std.testing.expect(found_validate);
    try std.testing.expect(found_rv);
}
