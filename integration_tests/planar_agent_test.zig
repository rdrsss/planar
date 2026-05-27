//! integration_tests/planar_agent_test.zig — black-box tests for the
//! planar-agent binary (plan 85 M2: full 13-verb surface).
//!
//! Covered scenarios (slugs in parens map to test-spec coverage):
//!
//!   - --help surfaces all 13 verbs and exits 0 (planar-agent-cmd-tree).
//!   - version emits planar-agent-prefixed line.
//!   - pull happy path: claim row, action row, task → doing (planar-agent-pull,
//!     planar-agent-json-shapes).
//!   - pull no_work on empty plan returns {ok,no_work:true} with no writes.
//!   - peek then pull returns the same task; peek writes nothing
//!     (planar-agent-peek).
//!   - complete: task → done, claim → completed (planar-agent-complete).
//!   - fail: task → todo, claim → aborted; next pull picks up same task
//!     (planar-agent-fail).
//!   - release: task → todo, claim → released (distinct from fail)
//!     (planar-agent-release).
//!   - block: blocker edge inserted, task → blocked, claim → released
//!     (planar-agent-block).
//!   - claim primitive does NOT auto-transition task status
//!     (planar-agent-claim).
//!   - heartbeat extends lease (planar-agent-heartbeat).
//!   - action start/end records nested action under claim
//!     (planar-agent-action-start, planar-agent-action-end).
//!   - reconcile --dry-run reports candidates without writing
//!     (planar-agent-reconcile).
//!   - abort releases claim from a different session and records audit row
//!     (planar-agent-abort).
//!   - --no-locality-probe leaves locality columns NULL/unknown
//!     (locality-flags-wiring).
//!   - cross-process pull contention: exactly one winner
//!     (claim-concurrency-tests).
//!
//! Several scenarios (e.g. locality probe inside a real git checkout)
//! are exercised by unit tests in src/engine/runtime/agentactivity/.

const std = @import("std");
const harness = @import("harness");

/// Resolve the planar-agent binary path from PLANAR_AGENT_BIN env var.
fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_AGENT_BIN=")) {
            return s["PLANAR_AGENT_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_AGENT_BIN is not set.
        \\Run integration tests via: make test-integration (which sets it).
    );
}

/// Run the planar-agent binary with `args`. Inherits the test harness's
/// PLANAR_DB so both binaries see the same scratch DB.
fn runAgent(
    suite: *const harness.Suite,
    args: []const []const u8,
) harness.Suite.RunResult {
    const gpa = suite.allocator;
    const agent_bin = resolveAgentBin();

    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, agent_bin) catch @panic("OOM");
    for (args) |a| argv_list.append(gpa, a) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM injecting PLANAR_DB");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runAgent spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

/// Assert exit 0 and return res.stdout (caller owns).
fn mustRunAgent(
    suite: *const harness.Suite,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const res = runAgent(suite, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("planar-agent failed (term={any}): {s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

/// Seed scratch DB with init + plan + one todo task. Returns the plan id
/// as a heap-allocated decimal string (caller frees via `gpa.free`).
fn seedPlanWithTask(suite: *const harness.Suite, plan_slug: []const u8, task_title: []const u8) []u8 {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", plan_slug, "--json", plan_slug });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    const task_out = suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, task_title });
    gpa.free(task_out);
    return plan_id_arg;
}

// =========================================================================
// Basics
// =========================================================================

test "planar-agent --help lists the M2 verb tree and exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runAgent(&suite, &.{"--help"});
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);
    // Spot-check the M2 verbs are listed.
    inline for ([_][]const u8{
        "version", "pull",      "peek",   "complete", "fail",      "release", "block",
        "claim",   "heartbeat", "action", "ingest",   "reconcile", "abort",
    }) |v| {
        if (std.mem.indexOf(u8, res.stdout, v) == null) {
            std.debug.print("missing verb '{s}' in --help:\n{s}\n", .{ v, res.stdout });
            return error.MissingVerb;
        }
    }
}

test "planar-agent version emits planar-agent-prefixed line" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runAgent(&suite, &.{"version"});
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);
    try std.testing.expect(std.mem.startsWith(u8, res.stdout, "planar-agent "));
}

// =========================================================================
// pull / peek
// =========================================================================

test "planar-agent pull on empty plan returns no_work" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "ag-empty", "--json", "empty" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const id_buf = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(id_buf);

    const stdout = mustRunAgent(&suite, &.{ "pull", id_buf, "--no-locality-probe", "--json" });
    defer gpa.free(stdout);
    try std.testing.expect(std.mem.indexOf(u8, stdout, "\"no_work\":true") != null);
}

test "planar-agent pull claims a task, flips it to doing, returns claim+task+action_id" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-pull", "first-task");
    defer gpa.free(pid_arg);

    const stdout = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--role", "coder", "--json" });
    defer gpa.free(stdout);

    // Shape assertions.
    try std.testing.expect(std.mem.indexOf(u8, stdout, "\"ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, stdout, "\"claim_token\":\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, stdout, "\"claim\":{") != null);
    try std.testing.expect(std.mem.indexOf(u8, stdout, "\"task\":{") != null);
    try std.testing.expect(std.mem.indexOf(u8, stdout, "\"action_id\":") != null);
    // Locality probe was skipped → all locality columns NULL.
    try std.testing.expect(std.mem.indexOf(u8, stdout, "\"repo_root\":null") != null);
    try std.testing.expect(std.mem.indexOf(u8, stdout, "\"dirty_at_claim\":null") != null);

    // Post-state: the task is in `doing` per task show.
    const task_id = extractIntField(stdout, "\"task\":{\"id\"") orelse @panic("no task.id");
    const tid_arg = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(tid_arg);
    const task_json = suite.mustRun(&.{ "task", "show", "--json", tid_arg });
    defer gpa.free(task_json);
    try std.testing.expect(std.mem.indexOf(u8, task_json, "\"status\":\"doing\"") != null);
}

test "planar-agent peek returns same task pull would acquire; peek writes nothing" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-peek", "peek-task");
    defer gpa.free(pid_arg);

    const peek_out = mustRunAgent(&suite, &.{ "peek", pid_arg, "--json" });
    defer gpa.free(peek_out);
    try std.testing.expect(std.mem.indexOf(u8, peek_out, "\"no_work\":false") != null);
    const peek_task_id = extractIntField(peek_out, "\"task\":{\"id\"") orelse @panic("no peek task.id");

    // Pull should claim the same task (peek wrote nothing).
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const pull_task_id = extractIntField(pull_out, "\"task\":{\"id\"") orelse @panic("no pull task.id");
    try std.testing.expectEqual(peek_task_id, pull_task_id);
}

// =========================================================================
// complete / fail / release / block
// =========================================================================

test "planar-agent complete: task → done, claim → completed" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-complete", "to-finish");
    defer gpa.free(pid_arg);

    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);
    const task_id = extractIntField(pull_out, "\"task\":{\"id\"") orelse @panic("no task.id");

    const complete_out = mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "shipped", "--json" });
    defer gpa.free(complete_out);
    try std.testing.expect(std.mem.indexOf(u8, complete_out, "\"status\":\"completed\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, complete_out, "\"status\":\"done\"") != null);

    const tid_arg = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(tid_arg);
    const task_json = suite.mustRun(&.{ "task", "show", "--json", tid_arg });
    defer gpa.free(task_json);
    try std.testing.expect(std.mem.indexOf(u8, task_json, "\"status\":\"done\"") != null);
}

test "planar-agent fail: task → todo, claim → aborted; subsequent pull re-picks same task" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-fail", "will-fail");
    defer gpa.free(pid_arg);

    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);
    const first_task_id = extractIntField(pull_out, "\"task\":{\"id\"") orelse @panic("no task.id");

    const fail_out = mustRunAgent(&suite, &.{ "fail", "--claim", token, "--reason", "broke", "--json" });
    defer gpa.free(fail_out);
    try std.testing.expect(std.mem.indexOf(u8, fail_out, "\"status\":\"aborted\"") != null);

    // Re-pull picks up the same task.
    const repull = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--json" });
    defer gpa.free(repull);
    const second_task_id = extractIntField(repull, "\"task\":{\"id\"") orelse @panic("no repull task.id");
    try std.testing.expectEqual(first_task_id, second_task_id);
}

test "planar-agent release: claim → released (distinct from fail's aborted)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-release", "to-give-up");
    defer gpa.free(pid_arg);

    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);

    const release_out = mustRunAgent(&suite, &.{ "release", "--claim", token, "--reason", "not mine", "--json" });
    defer gpa.free(release_out);
    try std.testing.expect(std.mem.indexOf(u8, release_out, "\"status\":\"released\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, release_out, "\"status\":\"todo\"") != null);
}

test "planar-agent block: blocker edge, task → blocked, claim → released" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-block", "the-task");
    defer gpa.free(pid_arg);
    // Seed a blocker task on the same plan.
    const blocker_out = suite.mustRun(&.{ "task", "add", "--plan", pid_arg, "--json", "blocker-task" });
    defer gpa.free(blocker_out);
    const blocker_id = extractIntField(blocker_out, "\"id\"") orelse @panic("no blocker id");

    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);

    const blocker_arg = std.fmt.allocPrint(gpa, "{d}", .{blocker_id}) catch @panic("OOM");
    defer gpa.free(blocker_arg);
    const block_out = mustRunAgent(&suite, &.{ "block", "--claim", token, "--blocker", blocker_arg, "--reason", "depends on B", "--json" });
    defer gpa.free(block_out);
    try std.testing.expect(std.mem.indexOf(u8, block_out, "\"status\":\"released\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, block_out, "\"status\":\"blocked\"") != null);
}

// =========================================================================
// claim / heartbeat / action start/end
// =========================================================================

test "planar-agent claim primitive does NOT auto-transition task status" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-claim", "direct-target");
    defer gpa.free(pid_arg);
    // Find the task id via task list.
    const list_json = suite.mustRun(&.{ "task", "list", "--plan", pid_arg, "--json" });
    defer gpa.free(list_json);
    const task_id = extractIntField(list_json, "\"id\"") orelse @panic("no task id");
    const ref = std.fmt.allocPrint(gpa, "task:{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(ref);

    const claim_out = mustRunAgent(&suite, &.{ "claim", "--entity", ref, "--role", "coder", "--no-locality-probe", "--json" });
    defer gpa.free(claim_out);
    try std.testing.expect(std.mem.indexOf(u8, claim_out, "\"status\":\"active\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, claim_out, "\"claim_token\":\"") != null);

    // Task status is UNCHANGED (still todo).
    const tid_arg = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(tid_arg);
    const task_json = suite.mustRun(&.{ "task", "show", "--json", tid_arg });
    defer gpa.free(task_json);
    try std.testing.expect(std.mem.indexOf(u8, task_json, "\"status\":\"todo\"") != null);
}

test "planar-agent claim refuses unsupported entity prefixes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const res = runAgent(&suite, &.{ "claim", "--entity", "question:1", "--no-locality-probe" });
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
}

test "planar-agent heartbeat extends lease, returns ok+claim" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-heartbeat", "hb-task");
    defer gpa.free(pid_arg);
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "60", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);

    const hb_out = mustRunAgent(&suite, &.{ "heartbeat", "--claim", token, "--ttl", "600", "--json" });
    defer gpa.free(hb_out);
    try std.testing.expect(std.mem.indexOf(u8, hb_out, "\"ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, hb_out, "\"claim_token\":\"") != null);
}

test "planar-agent action start/end emits ok+action shape under a claim" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-action", "act-task");
    defer gpa.free(pid_arg);
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);

    const start_out = mustRunAgent(&suite, &.{ "action", "start", "--claim", token, "--kind", "tool_call", "--json" });
    defer gpa.free(start_out);
    try std.testing.expect(std.mem.indexOf(u8, start_out, "\"ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, start_out, "\"action_id\":") != null);
    // tool_call probe-default = skip → head_sha null, dirty null (engine
    // stores NULL when the locality probe was fully skipped).
    try std.testing.expect(std.mem.indexOf(u8, start_out, "\"head_sha\":null") != null);
    try std.testing.expect(std.mem.indexOf(u8, start_out, "\"dirty\":null") != null);

    const action_id = extractIntField(start_out, "\"action_id\":") orelse @panic("no action_id");
    const aid_arg = std.fmt.allocPrint(gpa, "{d}", .{action_id}) catch @panic("OOM");
    defer gpa.free(aid_arg);

    const end_out = mustRunAgent(&suite, &.{ "action", "end", "--action", aid_arg, "--outcome", "ok", "--summary", "tool ran", "--json" });
    defer gpa.free(end_out);
    try std.testing.expect(std.mem.indexOf(u8, end_out, "\"outcome\":\"ok\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, end_out, "\"summary\":\"tool ran\"") != null);
}

// =========================================================================
// reconcile / abort
// =========================================================================

test "planar-agent reconcile --dry-run reports candidates without writing" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-recon", "rec-task");
    defer gpa.free(pid_arg);

    // Acquire with TTL=1s so it expires quickly.
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "1", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);

    // Sleep 2s so the lease expires.
    try std.testing.io.sleep(std.Io.Duration.fromSeconds(2), std.Io.Clock.awake);

    const dry_out = mustRunAgent(&suite, &.{ "reconcile", "--dry-run", "--stale-after", "0", "--json" });
    defer gpa.free(dry_out);
    try std.testing.expect(std.mem.indexOf(u8, dry_out, "\"claims_marked_stale\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, dry_out, "\"candidates\":[") != null);
    try std.testing.expect(std.mem.indexOf(u8, dry_out, token) != null);

    // Real reconcile marks it stale.
    const real_out = mustRunAgent(&suite, &.{ "reconcile", "--stale-after", "0", "--json" });
    defer gpa.free(real_out);
    try std.testing.expect(std.mem.indexOf(u8, real_out, "\"claims_marked_stale\":1") != null);
}

test "planar-agent abort releases a stuck claim from a different session and records audit row" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-abort", "stuck-task");
    defer gpa.free(pid_arg);
    // Owning session: vendor=sessionA, vendor-session=A:1.
    const pull_out = mustRunAgent(&suite, &.{
        "pull",                pid_arg,
        "--vendor",            "sessionA",
        "--vendor-session",    "A:1",
        "--no-locality-probe", "--json",
    });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);

    // Different session aborts.
    const abort_out = mustRunAgent(&suite, &.{
        "abort",    "--claim",  token,
        "--vendor", "operator", "--vendor-session",
        "ops:1",    "--reason", "force release",
        "--json",
    });
    defer gpa.free(abort_out);
    try std.testing.expect(std.mem.indexOf(u8, abort_out, "\"status\":\"aborted\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, abort_out, "\"aborting_session\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, abort_out, "\"release_reason\":\"force release\"") != null);
}

// =========================================================================
// Cross-process contention — exactly one winner under simultaneous pulls.
// =========================================================================

test "two planar-agent pull processes race; exactly one wins" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-race", "contended");
    defer gpa.free(pid_arg);

    const agent_bin = resolveAgentBin();
    const argv = &[_][]const u8{
        agent_bin, "pull", pid_arg, "--no-locality-probe", "--vendor", "race-test", "--json",
    };

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    const start_ns = monotonicNs();

    var c1 = try std.process.spawn(std.testing.io, .{
        .argv = argv,
        .environ_map = &env_map,
        .stdout = .pipe,
        .stderr = .pipe,
    });
    var c2 = try std.process.spawn(std.testing.io, .{
        .argv = argv,
        .environ_map = &env_map,
        .stdout = .pipe,
        .stderr = .pipe,
    });

    const stdout1 = try drainPipe(gpa, &c1.stdout);
    defer gpa.free(stdout1);
    const stderr1 = try drainPipe(gpa, &c1.stderr);
    defer gpa.free(stderr1);
    const stdout2 = try drainPipe(gpa, &c2.stdout);
    defer gpa.free(stdout2);
    const stderr2 = try drainPipe(gpa, &c2.stderr);
    defer gpa.free(stderr2);

    const term1 = try c1.wait(std.testing.io);
    const term2 = try c2.wait(std.testing.io);

    const elapsed_ns: u64 = monotonicNs() - start_ns;

    if (term1 != .exited or term1.exited != 0) {
        std.debug.print("c1 stdout: {s}\nc1 stderr: {s}\n", .{ stdout1, stderr1 });
        return error.Child1Failed;
    }
    if (term2 != .exited or term2.exited != 0) {
        std.debug.print("c2 stdout: {s}\nc2 stderr: {s}\n", .{ stdout2, stderr2 });
        return error.Child2Failed;
    }

    const c1_no_work = std.mem.indexOf(u8, stdout1, "\"no_work\":true") != null;
    const c1_won = std.mem.indexOf(u8, stdout1, "\"claim_token\":\"") != null;
    const c2_no_work = std.mem.indexOf(u8, stdout2, "\"no_work\":true") != null;
    const c2_won = std.mem.indexOf(u8, stdout2, "\"claim_token\":\"") != null;

    if (c1_won == c2_won) {
        std.debug.print(
            "concurrency contract broken: c1_won={any} c2_won={any}\nc1 stdout: {s}\nc2 stdout: {s}\n",
            .{ c1_won, c2_won, stdout1, stdout2 },
        );
        return error.ExactlyOneWinnerExpected;
    }
    try std.testing.expect(c1_no_work != c2_no_work);
    try std.testing.expect((c1_won and c2_no_work) or (c2_won and c1_no_work));

    std.debug.print("[plan85-pull-concurrency] elapsed_ns={d} ({d:.3}ms) winner={s}\n", .{
        elapsed_ns,
        @as(f64, @floatFromInt(elapsed_ns)) / 1_000_000.0,
        if (c1_won) "c1" else "c2",
    });
}

// =========================================================================
// Helpers
// =========================================================================

fn monotonicNs() u64 {
    const ts: std.Io.Timestamp = std.Io.Clock.awake.now(std.testing.io);
    return @intCast(ts.toNanoseconds());
}

fn drainPipe(gpa: std.mem.Allocator, file: *?std.Io.File) ![]u8 {
    if (file.*) |*f| {
        defer {
            f.close(std.testing.io);
            file.* = null;
        }
        var reader = f.reader(std.testing.io, &.{});
        return reader.interface.allocRemaining(gpa, std.Io.Limit.limited(1 << 20)) catch |err| switch (err) {
            error.ReadFailed => if (reader.err) |e| return e else return err,
            else => return err,
        };
    }
    return try gpa.dupe(u8, "");
}

/// Locate `key` (which must include the surrounding double-quotes,
/// e.g. `"\"id\""`), advance past the colon, and parse decimal digits.
fn extractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

/// Locate a JSON string field whose prefix INCLUDES the opening quote
/// (e.g. `"\"claim_token\":\""`) and return the string up to the next
/// unescaped `"`. Caller owns the returned heap slice.
fn extractStringField(gpa: std.mem.Allocator, json: []const u8, prefix: []const u8) ![]u8 {
    const idx = std.mem.indexOf(u8, json, prefix) orelse return error.FieldNotFound;
    const i = idx + prefix.len;
    var end = i;
    while (end < json.len and json[end] != '"') end += 1;
    if (end == json.len) return error.UnterminatedString;
    return try gpa.dupe(u8, json[i..end]);
}
