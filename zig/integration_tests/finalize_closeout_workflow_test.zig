//! integration_tests/finalize_closeout_workflow_test.zig
//!
//! Black-box integration tests for `workflows/finalize_closeout.lua` (plan 638, M1).
//!
//! These tests exercise the full workflow end-to-end via the compiled
//! `planar-execute` binary, a real isolated `PLANAR_DB`, and the harness
//! `planar` binary on PATH. They prove the rule-7 structural never-force-close
//! invariant and the ready-path happy path.
//!
//! ## Test fixtures
//!
//! 1. **Ready plan** — all tasks terminal (done). The workflow must:
//!      - return flow.result with ready=true, closed=true.
//!      - advance the plan status to "done".
//!      - record a run that finishes with status "completed" and includes
//!        a "closeout-eval" event followed by a "closed" event.
//!
//! 2. **Not-ready plan** — one task left open (todo). The workflow must:
//!      - return flow.result with ready=false and a non-empty blocked_by.
//!      - leave the plan status UNCHANGED (not done).
//!      - record a run that finishes with status "aborted" and includes
//!        a "closeout-eval" event and a "blocked" event (NO "closed" event).
//!      - NEVER call `plan closeout` (apply): proven by the plan status
//!        assertion above and by the absence of a "closed" event.
//!
//! Rule 7 is verified structurally: the "closed" event and "done" plan status
//! are the only observable outputs of the apply call; if neither appears on the
//! not-ready path, the apply branch was never reached.

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// JSON shapes
// =========================================================================

const PlanJSON = struct {
    id: i64 = 0,
    status: []const u8 = "",
};

const TaskJSON = struct {
    id: i64 = 0,
};

const StartOut = struct {
    run_uid: []const u8 = "",
};

const ShowRun = struct {
    run_uid: []const u8 = "",
    status: []const u8 = "",
    events: []const ShowEvent = &.{},
};

const ShowEvent = struct {
    seq: i64 = 0,
    kind: []const u8 = "",
    payload: ?std.json.Value = null,
};

/// Flow result from the workflow's stdout.
const FlowResult = struct {
    ready: bool = false,
    closed: bool = false,
    run_uid: []const u8 = "",
    blocked_by: []const []const u8 = &.{},
};

// =========================================================================
// Harness helpers
// =========================================================================

/// resolveEnv reads an environment variable or panics.
fn resolveEnv(comptime key: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key ++ "=")) return s[(key ++ "=").len..];
    }
    @panic(key ++ " is not set. Run via: make test-integration");
}

/// RunResult from runExecute.
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

/// runExecute spawns `planar-execute <args...>` with the isolated DB and PATH.
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

/// repoRootFromBin derives the repo root from PLANAR_BIN (bin is 4 levels
/// deep since the M0 relocation to zig/: zig/zig-out/bin/planar).
fn repoRootFromBin(allocator: std.mem.Allocator) ![]const u8 {
    const bin_path = resolveEnv("PLANAR_BIN");
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    const d4 = std.fs.path.dirname(d3) orelse return error.FileNotFound;
    return allocator.dupe(u8, d4);
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

// =========================================================================
// Fixture helpers
// =========================================================================

/// seedActiveAnchorPlan creates an anchor plan (no parent) with N tasks, all
/// in status "todo". Returns (plan_id_str, task_id_strs) arena-owned.
fn seedActiveAnchorPlan(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    slug: []const u8,
    task_count: usize,
) struct { plan_id: []u8, task_ids: [][]u8 } {
    const gpa = suite.allocator;

    const plan_buf = suite.mustRun(&.{
        "plan", "create", "--json", "--status", "active", "--slug", slug, slug,
    });
    defer gpa.free(plan_buf);
    const plan = parseJSON(PlanJSON, arena, plan_buf);
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    var task_ids = arena.alloc([]u8, task_count) catch @panic("OOM");
    for (0..task_count) |idx| {
        const title = std.fmt.allocPrint(arena, "task-{d}", .{idx + 1}) catch @panic("OOM");
        const t_buf = suite.mustRun(&.{ "task", "add", "--json", "--plan", plan_id, title });
        defer gpa.free(t_buf);
        const t = parseJSON(TaskJSON, arena, t_buf);
        task_ids[idx] = std.fmt.allocPrint(arena, "{d}", .{t.id}) catch @panic("OOM");
        // Advance to doing so task done is legal (todo → done is not in the matrix).
        gpa.free(suite.mustRun(&.{ "task", "update", task_ids[idx], "--status", "doing" }));
    }
    return .{ .plan_id = plan_id, .task_ids = task_ids };
}

// =========================================================================
// Test 1: Ready plan — workflow closes it
// =========================================================================

test "finalize_closeout: ready plan → workflow closes it, run completed, events in order" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("fc-ready");
    suite.addAssoc("fc-ready", null);

    // Seed an anchor plan with two tasks.
    const seeded = seedActiveAnchorPlan(&suite, arena, "fc-ready-plan", 2);

    // Mark both tasks done so the plan passes the closeout hard gate.
    // After this the plan remains "active" (anchor auto-recompute caps at active).
    gpa.free(suite.mustRun(&.{ "task", "done", seeded.task_ids[0] }));
    gpa.free(suite.mustRun(&.{ "task", "done", seeded.task_ids[1] }));

    // Confirm plan is still "active" (anchor cap in effect; closeout not yet applied).
    const before = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", seeded.plan_id });
    try std.testing.expectEqualStrings("active", before.status);

    // Resolve the workflow path from the repo root.
    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "finalize_closeout.lua" });
    defer gpa.free(wf_path);

    // Build --args JSON.
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{seeded.plan_id});
    defer gpa.free(args_json);

    // Run the workflow.
    const res = try runExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "closeout", "--args", args_json,
    });
    defer res.deinit();

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("workflow stderr:\n{s}\nstdout:\n{s}\n", .{ res.stderr, res.stdout });
    }
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    // Parse flow.result from stdout.
    const result = parseJSON(FlowResult, arena, res.stdout);
    try std.testing.expect(result.ready);
    try std.testing.expect(result.closed);
    try std.testing.expect(result.run_uid.len > 0);

    // Plan must now be "done".
    const after = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", seeded.plan_id });
    try std.testing.expectEqualStrings("done", after.status);

    // Verify the run record: status=completed, events contain closeout-eval and closed (in order).
    const run_show = suite.mustRunJSON(ShowRun, arena, &.{
        "run", "show", result.run_uid, "--json",
    });
    try std.testing.expectEqualStrings("completed", run_show.status);

    // Find the closeout-eval and closed events.
    var found_eval = false;
    var found_closed = false;
    var eval_seq: i64 = 0;
    var closed_seq: i64 = 0;
    for (run_show.events) |ev| {
        if (std.mem.eql(u8, ev.kind, "closeout-eval")) {
            found_eval = true;
            eval_seq = ev.seq;
        }
        if (std.mem.eql(u8, ev.kind, "closed")) {
            found_closed = true;
            closed_seq = ev.seq;
        }
    }
    try std.testing.expect(found_eval);
    try std.testing.expect(found_closed);
    // closeout-eval must precede closed in the event sequence.
    try std.testing.expect(eval_seq < closed_seq);
}

// =========================================================================
// Test 2: Not-ready plan — rule-7 red test (never force-closes)
// =========================================================================

test "finalize_closeout: not-ready plan → workflow does NOT close it, run aborted" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("fc-blocked");
    suite.addAssoc("fc-blocked", null);

    // Seed an anchor plan with two tasks; leave one open (todo) so the
    // hard gate is blocked.
    const seeded = seedActiveAnchorPlan(&suite, arena, "fc-blocked-plan", 2);
    // Only mark the first task done; the second remains "todo".
    gpa.free(suite.mustRun(&.{ "task", "done", seeded.task_ids[0] }));

    // Confirm plan is "active" and the second task is still open.
    const before = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", seeded.plan_id });
    try std.testing.expectEqualStrings("active", before.status);

    // Resolve the workflow path.
    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "finalize_closeout.lua" });
    defer gpa.free(wf_path);

    // Build --args JSON.
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{seeded.plan_id});
    defer gpa.free(args_json);

    // Run the workflow.
    const res = try runExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "closeout", "--args", args_json,
    });
    defer res.deinit();

    // The workflow itself exits 0 (it handled the blocked case gracefully and
    // called flow.result; the engine exits 0 when the phase returns normally).
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("workflow stderr:\n{s}\nstdout:\n{s}\n", .{ res.stderr, res.stdout });
    }
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    // flow.result must have ready=false and non-empty blocked_by.
    const result = parseJSON(FlowResult, arena, res.stdout);
    try std.testing.expect(!result.ready);
    try std.testing.expect(result.blocked_by.len > 0);
    try std.testing.expect(result.run_uid.len > 0);

    // Rule 7 proof 1: Plan status must be UNCHANGED ("active", not "done").
    const after = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", seeded.plan_id });
    try std.testing.expectEqualStrings("active", after.status);

    // Rule 7 proof 2: The run must be "aborted" (not "completed").
    const run_show = suite.mustRunJSON(ShowRun, arena, &.{
        "run", "show", result.run_uid, "--json",
    });
    try std.testing.expectEqualStrings("aborted", run_show.status);

    // Rule 7 proof 3: The event journal must contain "closeout-eval" and "blocked"
    // but MUST NOT contain "closed" (the apply event).
    var found_eval = false;
    var found_blocked = false;
    var found_closed = false;
    for (run_show.events) |ev| {
        if (std.mem.eql(u8, ev.kind, "closeout-eval")) found_eval = true;
        if (std.mem.eql(u8, ev.kind, "blocked")) found_blocked = true;
        if (std.mem.eql(u8, ev.kind, "closed")) found_closed = true;
    }
    try std.testing.expect(found_eval);
    try std.testing.expect(found_blocked);
    // The "closed" event must be absent — proving apply was never called.
    try std.testing.expect(!found_closed);
}
