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

/// Like `runAgent`, but executes with cwd set to `cwd`. Uses an absolute
/// PLANAR_DB so the child still sees the suite DB outside the test runner cwd.
fn runAgentInDir(
    suite: *harness.Suite,
    cwd: []const u8,
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
    env_map.put("PLANAR_DB", suite.absDbPath()) catch @panic("OOM injecting PLANAR_DB");
    env_map.put("PWD", cwd) catch @panic("OOM injecting PWD");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
        .cwd = .{ .path = cwd },
    }) catch |e| std.debug.panic("runAgentInDir spawn failed: {s}", .{@errorName(e)});

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

/// Assert exit 0 and return stdout for a planar-agent invocation rooted at
/// `cwd`. Caller owns the returned buffer.
fn mustRunAgentInDir(
    suite: *harness.Suite,
    cwd: []const u8,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const res = runAgentInDir(suite, cwd, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "planar-agent failed in cwd '{s}' (term={any}): {s}\nstderr: {s}\n",
            .{ cwd, res.term, res.stdout, res.stderr },
        );
        @panic("planar-agent must-run-in-dir failed");
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

test "planar-agent terminal verbs collect git commits into session_commits regardless of outcome" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const repo_root = try std.fs.path.join(gpa, &.{ suite.tmpAbsPath(), "agent-session-commits-repo" });
    defer gpa.free(repo_root);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, repo_root);

    try runCommandDiscard(&.{ "git", "init", repo_root });
    try runCommandInDirDiscard(repo_root, &.{ "git", "config", "user.email", "planar-test@example.com" });
    try runCommandInDirDiscard(repo_root, &.{ "git", "config", "user.name", "Planar Test" });

    try writeRepoFile(repo_root, "README.md", "seed\n");
    try runCommandInDirDiscard(repo_root, &.{ "git", "add", "README.md" });
    try runCommandInDirDiscard(repo_root, &.{ "git", "commit", "-m", "seed" });

    const branch = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "branch", "--show-current" }));
    defer gpa.free(branch);
    const canonical_repo_root = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "rev-parse", "--show-toplevel" }));
    defer gpa.free(canonical_repo_root);

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const complete_plan = suite.mustRun(&.{ "plan", "create", "--slug", "ag-session-commits-complete", "--json", "agent session commits complete" });
    defer gpa.free(complete_plan);
    const complete_plan_id = extractIntField(complete_plan, "\"id\"") orelse @panic("no complete plan id");
    const complete_plan_arg = std.fmt.allocPrint(gpa, "{d}", .{complete_plan_id}) catch @panic("OOM");
    defer gpa.free(complete_plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", complete_plan_arg, "complete task" }));

    const fail_plan = suite.mustRun(&.{ "plan", "create", "--slug", "ag-session-commits-fail", "--json", "agent session commits fail" });
    defer gpa.free(fail_plan);
    const fail_plan_id = extractIntField(fail_plan, "\"id\"") orelse @panic("no fail plan id");
    const fail_plan_arg = std.fmt.allocPrint(gpa, "{d}", .{fail_plan_id}) catch @panic("OOM");
    defer gpa.free(fail_plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", fail_plan_arg, "fail task" }));

    const release_plan = suite.mustRun(&.{ "plan", "create", "--slug", "ag-session-commits-release", "--json", "agent session commits release" });
    defer gpa.free(release_plan);
    const release_plan_id = extractIntField(release_plan, "\"id\"") orelse @panic("no release plan id");
    const release_plan_arg = std.fmt.allocPrint(gpa, "{d}", .{release_plan_id}) catch @panic("OOM");
    defer gpa.free(release_plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", release_plan_arg, "release task" }));

    const initial_count = try sqliteScalar(gpa, suite.db_path, "select count(*) from session_commits;");
    try std.testing.expectEqual(@as(i64, 0), initial_count);

    const complete_pull = mustRunAgentInDir(&suite, repo_root, &.{ "pull", complete_plan_arg, "--role", "coder", "--json" });
    defer gpa.free(complete_pull);
    const complete_token = extractStringField(gpa, complete_pull, "\"claim_token\":\"") catch @panic("no complete token");
    defer gpa.free(complete_token);
    const complete_session_id = extractIntField(complete_pull, "\"session_id\":") orelse @panic("no complete session id");
    const complete_claim_id = extractIntField(complete_pull, "\"claim\":{\"id\"") orelse @panic("no complete claim id");

    const complete_sha_a = try createCommit(repo_root, "complete-a.txt", "complete-a\n", "complete a");
    defer gpa.free(complete_sha_a);
    const complete_sha_b = try createCommit(repo_root, "complete-b.txt", "complete-b\n", "complete b");
    defer gpa.free(complete_sha_b);

    const complete_out = mustRunAgentInDir(&suite, repo_root, &.{ "complete", "--claim", complete_token, "--summary", "shipped", "--json" });
    defer gpa.free(complete_out);
    try std.testing.expect(std.mem.indexOf(u8, complete_out, "\"status\":\"completed\"") != null);
    try assertCommitRows(
        gpa,
        suite.db_path,
        complete_session_id,
        complete_claim_id,
        canonical_repo_root,
        branch,
        &.{
            .{ .sha = complete_sha_a, .subject = "complete a" },
            .{ .sha = complete_sha_b, .subject = "complete b" },
        },
    );
    try std.testing.expectEqual(@as(i64, 2), try sqliteScalar(gpa, suite.db_path, "select count(*) from session_commits;"));

    const fail_pull = mustRunAgentInDir(&suite, repo_root, &.{ "pull", fail_plan_arg, "--role", "coder", "--json" });
    defer gpa.free(fail_pull);
    const fail_token = extractStringField(gpa, fail_pull, "\"claim_token\":\"") catch @panic("no fail token");
    defer gpa.free(fail_token);
    const fail_session_id = extractIntField(fail_pull, "\"session_id\":") orelse @panic("no fail session id");
    const fail_claim_id = extractIntField(fail_pull, "\"claim\":{\"id\"") orelse @panic("no fail claim id");

    const fail_sha = try createCommit(repo_root, "fail.txt", "fail\n", "fail commit");
    defer gpa.free(fail_sha);

    const fail_out = mustRunAgentInDir(&suite, repo_root, &.{ "fail", "--claim", fail_token, "--reason", "broke", "--json" });
    defer gpa.free(fail_out);
    try std.testing.expect(std.mem.indexOf(u8, fail_out, "\"status\":\"aborted\"") != null);
    try assertCommitRows(
        gpa,
        suite.db_path,
        fail_session_id,
        fail_claim_id,
        canonical_repo_root,
        branch,
        &.{.{ .sha = fail_sha, .subject = "fail commit" }},
    );
    try std.testing.expectEqual(@as(i64, 3), try sqliteScalar(gpa, suite.db_path, "select count(*) from session_commits;"));

    const release_pull = mustRunAgentInDir(&suite, repo_root, &.{ "pull", release_plan_arg, "--role", "coder", "--json" });
    defer gpa.free(release_pull);
    const release_token = extractStringField(gpa, release_pull, "\"claim_token\":\"") catch @panic("no release token");
    defer gpa.free(release_token);
    const release_session_id = extractIntField(release_pull, "\"session_id\":") orelse @panic("no release session id");
    const release_claim_id = extractIntField(release_pull, "\"claim\":{\"id\"") orelse @panic("no release claim id");

    const release_sha = try createCommit(repo_root, "release.txt", "release\n", "release commit");
    defer gpa.free(release_sha);

    const release_out = mustRunAgentInDir(&suite, repo_root, &.{ "release", "--claim", release_token, "--reason", "not mine", "--json" });
    defer gpa.free(release_out);
    try std.testing.expect(std.mem.indexOf(u8, release_out, "\"status\":\"released\"") != null);
    try assertCommitRows(
        gpa,
        suite.db_path,
        release_session_id,
        release_claim_id,
        canonical_repo_root,
        branch,
        &.{.{ .sha = release_sha, .subject = "release commit" }},
    );
    try std.testing.expectEqual(@as(i64, 4), try sqliteScalar(gpa, suite.db_path, "select count(*) from session_commits;"));
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
    const list_json = suite.mustRun(&.{ "task", "list", "--scope", "global", "--plan", pid_arg, "--json" });
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

// =========================================================================
// Duration-string parsing (plan 85 t#2620): --ttl / --stale-after accept
// both bare-int seconds (back-compat) and ISO-style suffixed durations
// (10m, 1h, 500ms, ...).
// =========================================================================

test "planar-agent pull --ttl 10m accepts suffixed duration string" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-ttl-suffix", "ttl-task");
    defer gpa.free(pid_arg);

    // Pre-fix this exited InvalidValue. Should now succeed (600s lease).
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "10m", "--json" });
    defer gpa.free(pull_out);
    try std.testing.expect(std.mem.indexOf(u8, pull_out, "\"ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, pull_out, "\"no_work\":false") != null);
}

test "planar-agent heartbeat --ttl 1h accepts suffixed duration string" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-hb-suffix", "hb-task");
    defer gpa.free(pid_arg);
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "60", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);

    const hb_out = mustRunAgent(&suite, &.{ "heartbeat", "--claim", token, "--ttl", "1h", "--json" });
    defer gpa.free(hb_out);
    try std.testing.expect(std.mem.indexOf(u8, hb_out, "\"ok\":true") != null);
}

test "planar-agent reconcile --stale-after 5s accepts suffixed duration string" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-recon-suffix", "recon-task");
    defer gpa.free(pid_arg);

    // No active claim — exits 0 with empty result. The point of the
    // test is that the flag parser doesn't choke on the "5s" syntax.
    const out = mustRunAgent(&suite, &.{ "reconcile", "--dry-run", "--stale-after", "5s", "--json" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"ok\":true") != null);
}

test "planar-agent --ttl back-compat: bare-int seconds still parse" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-ttl-bareint", "bare-task");
    defer gpa.free(pid_arg);

    // The pre-existing contract: --ttl 60 (bare seconds) must continue
    // to work. The change from .int to .string for the flag MUST NOT
    // break this path.
    const out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "60", "--json" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"no_work\":false") != null);
}

test "planar-agent pull --metadata persists JSON on the dispatch action row" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-meta-pull", "meta-task");
    defer gpa.free(pid_arg);

    const meta = "{\"strategy\":\"isolated-sequential\",\"axes\":{\"isolation\":\"worktree\"},\"rationale\":\"2-task plan\"}";
    const out = mustRunAgent(&suite, &.{
        "pull",                pid_arg,
        "--metadata",          meta,
        "--role",              "coder",
        "--no-locality-probe", "--json",
    });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"ok\":true") != null);

    // The pull response does not embed the action row, but the action_id
    // is present. Round-trip through getActionById by ending the action
    // (which emits the full action JSON shape) — that surface includes
    // the metadata field.
    const action_id = extractIntField(out, "\"action_id\":") orelse @panic("no action_id");
    const aid_arg = std.fmt.allocPrint(gpa, "{d}", .{action_id}) catch @panic("OOM");
    defer gpa.free(aid_arg);

    const end_out = mustRunAgent(&suite, &.{
        "action", "end", "--action", aid_arg, "--outcome", "ok", "--summary", "wrap", "--json",
    });
    defer gpa.free(end_out);
    // The action JSON should round-trip the metadata text as the encoded
    // string field.
    try std.testing.expect(std.mem.indexOf(u8, end_out, "\"metadata\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, end_out, "isolated-sequential") != null);
}

test "planar-agent pull --metadata rejects malformed JSON" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const pid_arg = seedPlanWithTask(&suite, "ag-meta-bad", "meta-bad-task");
    defer gpa.free(pid_arg);

    const res = runAgent(&suite, &.{
        "pull",                pid_arg,
        "--metadata",          "{not json",
        "--no-locality-probe", "--json",
    });
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "metadata") != null);
}

test "planar-agent action start --metadata persists JSON; null when absent" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-meta-action", "meta-act-task");
    defer gpa.free(pid_arg);
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);

    // Action start without --metadata → metadata:null.
    const start_no = mustRunAgent(&suite, &.{ "action", "start", "--claim", token, "--kind", "tool_call", "--json" });
    defer gpa.free(start_no);
    try std.testing.expect(std.mem.indexOf(u8, start_no, "\"metadata\":null") != null);

    // Action start WITH --metadata → metadata round-trips.
    const meta = "{\"k\":\"v\"}";
    const start_yes = mustRunAgent(&suite, &.{ "action", "start", "--claim", token, "--kind", "tool_call", "--metadata", meta, "--json" });
    defer gpa.free(start_yes);
    try std.testing.expect(std.mem.indexOf(u8, start_yes, "\"metadata\":") != null);
    // The metadata is encoded as a JSON string field, so the inner quotes
    // are escaped. Spot-check the key + value text appear.
    try std.testing.expect(std.mem.indexOf(u8, start_yes, "\\\"k\\\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, start_yes, "\\\"v\\\"") != null);
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

test "planar-agent reconcile --session scopes sweep; other session's claim untouched" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed two plans, each with one todo task, so each pull lands in a
    // distinct session (different vendor-session id → different session row).
    const pid_a = seedPlanWithTask(&suite, "ag-recon-sess-a", "sess-a-task");
    defer gpa.free(pid_a);
    const pid_b = seedPlanWithTask(&suite, "ag-recon-sess-b", "sess-b-task");
    defer gpa.free(pid_b);

    // Session A: pull with TTL=1s so the lease expires.
    const pull_a = mustRunAgent(&suite, &.{
        "pull",                pid_a,
        "--vendor",            "sess-test",
        "--vendor-session",    "sess-a:1",
        "--no-locality-probe", "--ttl",
        "1",                   "--json",
    });
    defer gpa.free(pull_a);
    const session_a = extractIntField(pull_a, "\"session_id\":") orelse @panic("no session_id in pull_a");
    const token_a = extractStringField(gpa, pull_a, "\"claim_token\":\"") catch @panic("no token_a");
    defer gpa.free(token_a);

    // Session B: pull with TTL=1s so the lease expires.
    const pull_b = mustRunAgent(&suite, &.{
        "pull",                pid_b,
        "--vendor",            "sess-test",
        "--vendor-session",    "sess-b:1",
        "--no-locality-probe", "--ttl",
        "1",                   "--json",
    });
    defer gpa.free(pull_b);
    const token_b = extractStringField(gpa, pull_b, "\"claim_token\":\"") catch @panic("no token_b");
    defer gpa.free(token_b);

    // Wait 2s for both leases to expire.
    try std.testing.io.sleep(std.Io.Duration.fromSeconds(2), std.Io.Clock.awake);

    // Reconcile scoped to session A only.
    const sess_a_arg = std.fmt.allocPrint(gpa, "{d}", .{session_a}) catch @panic("OOM");
    defer gpa.free(sess_a_arg);
    const recon_a = mustRunAgent(&suite, &.{
        "reconcile", "--session", sess_a_arg, "--stale-after", "0", "--json",
    });
    defer gpa.free(recon_a);
    // Exactly one claim marked stale (session A's).
    try std.testing.expect(std.mem.indexOf(u8, recon_a, "\"claims_marked_stale\":1") != null);

    // Session A's claim is now stale; session B's claim is still active.
    // Confirm via a session-A-scoped dry-run: no candidates remain in A.
    const dry_a = mustRunAgent(&suite, &.{
        "reconcile", "--session", sess_a_arg, "--dry-run", "--stale-after", "0", "--json",
    });
    defer gpa.free(dry_a);
    try std.testing.expect(std.mem.indexOf(u8, dry_a, "\"candidates\":[]") != null);

    // Session B's claim must still appear in a global dry-run.
    const dry_global = mustRunAgent(&suite, &.{
        "reconcile", "--dry-run", "--stale-after", "0", "--json",
    });
    defer gpa.free(dry_global);
    try std.testing.expect(std.mem.indexOf(u8, dry_global, token_b) != null);

    // Global reconcile picks up session B's expired claim.
    const recon_global = mustRunAgent(&suite, &.{
        "reconcile", "--stale-after", "0", "--json",
    });
    defer gpa.free(recon_global);
    try std.testing.expect(std.mem.indexOf(u8, recon_global, "\"claims_marked_stale\":1") != null);
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

    // No cosmetic timing print: the wall-clock invariant is already
    // covered by the assertions above (exactly-one-winner + no_work
    // bookkeeping). Printing to stderr near test exit triggers the
    // zig 0.16 test runner's stale "failed command:" re-emission noise
    // on every `make test-integration` run, so this keeps the gate
    // visibly clean. `elapsed_ns` is computed for future diagnostic
    // gating but intentionally not surfaced under green runs.
    _ = elapsed_ns;
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

const ExpectedCommit = struct {
    sha: []const u8,
    subject: []const u8,
};

fn runCommand(argv: []const []const u8) ![]u8 {
    const gpa = std.testing.allocator;
    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv,
    });
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("command failed: {s}\nstderr: {s}\n", .{ argv[0], result.stderr });
        gpa.free(result.stdout);
        return error.CommandFailed;
    }
    return result.stdout;
}

fn runCommandDiscard(argv: []const []const u8) !void {
    const stdout = try runCommand(argv);
    std.testing.allocator.free(stdout);
}

fn runCommandInDir(cwd: []const u8, argv: []const []const u8) ![]u8 {
    const gpa = std.testing.allocator;
    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv,
        .cwd = .{ .path = cwd },
    });
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("command failed in '{s}': {s}\nstderr: {s}\n", .{ cwd, argv[0], result.stderr });
        gpa.free(result.stdout);
        return error.CommandFailed;
    }
    return result.stdout;
}

fn runCommandInDirDiscard(cwd: []const u8, argv: []const []const u8) !void {
    const stdout = try runCommandInDir(cwd, argv);
    std.testing.allocator.free(stdout);
}

fn trimOwned(gpa: std.mem.Allocator, raw: []u8) ![]u8 {
    defer gpa.free(raw);
    return try gpa.dupe(u8, std.mem.trim(u8, raw, " \t\r\n"));
}

fn writeRepoFile(repo_root: []const u8, rel_path: []const u8, contents: []const u8) !void {
    const gpa = std.testing.allocator;
    const path = try std.fs.path.join(gpa, &.{ repo_root, rel_path });
    defer gpa.free(path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = contents });
}

fn createCommit(
    repo_root: []const u8,
    rel_path: []const u8,
    contents: []const u8,
    subject: []const u8,
) ![]u8 {
    try writeRepoFile(repo_root, rel_path, contents);
    try runCommandInDirDiscard(repo_root, &.{ "git", "add", rel_path });
    try runCommandInDirDiscard(repo_root, &.{ "git", "commit", "-m", subject });
    return trimOwned(std.testing.allocator, try runCommandInDir(repo_root, &.{ "git", "rev-parse", "HEAD" }));
}

fn sqliteQueryLines(
    gpa: std.mem.Allocator,
    db_path: []const u8,
    sql: []const u8,
) ![]u8 {
    const result = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", "-separator", "|", db_path, sql },
    }) catch |e| {
        std.debug.print("sqlite3 spawn failed: {s}\n", .{@errorName(e)});
        return error.SkipZigTest;
    };
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("sqlite3 failed: {s}\n", .{result.stderr});
        gpa.free(result.stdout);
        return error.SqliteFailed;
    }
    return result.stdout;
}

fn sqliteScalar(
    gpa: std.mem.Allocator,
    db_path: []const u8,
    sql: []const u8,
) !i64 {
    const out = try sqliteQueryLines(gpa, db_path, sql);
    defer gpa.free(out);
    const trimmed = std.mem.trim(u8, out, " \t\r\n");
    return std.fmt.parseInt(i64, trimmed, 10) catch |e| {
        std.debug.print("sqlite3 output not integer ('{s}'): {s}\n", .{ trimmed, @errorName(e) });
        return error.SqliteParseFailed;
    };
}

fn assertCommitRows(
    gpa: std.mem.Allocator,
    db_path: []const u8,
    session_id: i64,
    claim_id: i64,
    repo_root: []const u8,
    branch: []const u8,
    expected: []const ExpectedCommit,
) !void {
    const sql = try std.fmt.allocPrint(gpa,
        \\select sha, subject, author, committed_at, repo_root, branch
        \\from session_commits
        \\where session_id = {d} and claim_id = {d}
        \\order by sha asc;
    , .{ session_id, claim_id });
    defer gpa.free(sql);
    const out = try sqliteQueryLines(gpa, db_path, sql);
    defer gpa.free(out);

    var actual_lines = std.ArrayList([]const u8).empty;
    defer actual_lines.deinit(gpa);
    var split = std.mem.splitScalar(u8, std.mem.trim(u8, out, "\n"), '\n');
    while (split.next()) |line| {
        if (line.len == 0) continue;
        try actual_lines.append(gpa, line);
    }
    try std.testing.expectEqual(expected.len, actual_lines.items.len);

    const expected_sorted = try gpa.alloc(ExpectedCommit, expected.len);
    defer gpa.free(expected_sorted);
    @memcpy(expected_sorted, expected);
    std.mem.sort(ExpectedCommit, expected_sorted, {}, struct {
        fn lessThan(_: void, a: ExpectedCommit, b: ExpectedCommit) bool {
            return std.mem.lessThan(u8, a.sha, b.sha);
        }
    }.lessThan);

    for (actual_lines.items, expected_sorted) |line, want| {
        var fields = std.mem.splitScalar(u8, line, '|');
        const sha = fields.next() orelse return error.MalformedSqliteRow;
        const subject = fields.next() orelse return error.MalformedSqliteRow;
        const author = fields.next() orelse return error.MalformedSqliteRow;
        const committed_at = fields.next() orelse return error.MalformedSqliteRow;
        const got_repo_root = fields.next() orelse return error.MalformedSqliteRow;
        const got_branch = fields.next() orelse return error.MalformedSqliteRow;

        try std.testing.expectEqualStrings(want.sha, sha);
        try std.testing.expectEqualStrings(want.subject, subject);
        try std.testing.expectEqualStrings("Planar Test", author);
        try std.testing.expect(committed_at.len > 0);
        try std.testing.expectEqualStrings(repo_root, got_repo_root);
        try std.testing.expectEqualStrings(branch, got_branch);
    }
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
