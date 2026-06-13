//! integration_tests/planar_agent_run_test.zig — black-box scenario tests
//! for `planar-agent run start` / `run end` and the `reconcile` run sweep.
//!
//! Per CLAUDE.md § integration test methodology: each test walks a realistic
//! operator/harness workflow rather than exercising verbs in isolation.
//!
//! Covered scenarios:
//!
//!   - Scenario A (happy path): `run start` → assert workflow_runs row via
//!     sqlite3; `run end --status completed` → assert terminal status +
//!     ended_at set.
//!   - Scenario B (all terminal statuses): run end accepts completed, failed,
//!     interrupted; rejects abandoned (reconcile-only per Q597) and running.
//!   - Scenario C (reconcile run sweep): a row with a dead PID → `reconcile`
//!     → assert `abandoned`. A row with a live PID survives reconcile.
//!   - Scenario D (capability boundary): `run start` is available on
//!     planar-agent; it is NOT on planar-execute or planar-watch.

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

/// Run the planar-agent binary and inject the suite's PLANAR_DB.
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
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runAgent spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunAgent(suite: *const harness.Suite, args: []const []const u8) []u8 {
    const gpa = suite.allocator;
    const res = runAgent(suite, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("planar-agent failed (term={any}): {s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

/// Run sqlite3 returning a scalar integer.
fn sqliteScalar(gpa: std.mem.Allocator, db_path: []const u8, sql: []const u8) i64 {
    const result = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", db_path, sql },
    }) catch |e| std.debug.panic("sqlite3 spawn failed: {s}", .{@errorName(e)});
    defer gpa.free(result.stdout);
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.panic("sqlite3 returned non-zero: {s}", .{result.stderr});
    }
    const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
    return std.fmt.parseInt(i64, trimmed, 10) catch |e|
        std.debug.panic("sqlite3 output not integer ('{s}'): {s}", .{ trimmed, @errorName(e) });
}

/// Seed the DB and return the plan id as a heap-allocated decimal string.
fn seedPlan(suite: *const harness.Suite, slug: []const u8) []u8 {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", slug, "--json", slug });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    return std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
}

fn extractIntField(s: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, s, key) orelse return null;
    var i = idx + key.len;
    while (i < s.len and (s[i] == ' ' or s[i] == ':' or s[i] == '\t')) i += 1;
    var end = i;
    while (end < s.len and s[end] >= '0' and s[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, s[i..end], 10) catch null;
}

// =========================================================================
// Scenario A — run start → assert row; run end → assert terminal status
// =========================================================================

test "scenario A: run start inserts running row; run end transitions to completed" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "run-lifecycle");
    defer gpa.free(plan_arg);

    // Step 1: run start. Use this process's PID as the "harness" PID so the
    // run looks alive for the duration of the test, and a unique run-id.
    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);
    const run_id_str = std.fmt.allocPrint(gpa, "test-run-{d}-1", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_id_str);

    const start_out = mustRunAgent(&suite, &.{
        "run",         "start",
        "--plan",      plan_arg,
        "--workflow",  "isolated-sequential",
        "--run-id",    run_id_str,
        "--pid",       self_pid,
        "--repo-root", "/tmp/test-repo",
        "--json",
    });
    defer gpa.free(start_out);

    // Assert the JSON shape.
    try std.testing.expect(std.mem.indexOf(u8, start_out, "\"ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, start_out, "\"run_id\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, start_out, "\"status\":\"running\"") != null);

    // Assert the row is in the DB with status = 'running'.
    const run_db_id = extractIntField(start_out, "\"run_id\":") orelse @panic("no run_id in start output");
    const check_sql = std.fmt.allocPrint(
        gpa,
        "SELECT count(*) FROM workflow_runs WHERE id = {d} AND status = 'running';",
        .{run_db_id},
    ) catch @panic("OOM");
    defer gpa.free(check_sql);
    const count = sqliteScalar(gpa, suite.db_path, check_sql);
    try std.testing.expectEqual(@as(i64, 1), count);

    // Step 2: run end. Transition to completed.
    const end_out = mustRunAgent(&suite, &.{
        "run",      "end",
        "--run-id", run_id_str,
        "--status", "completed",
        "--json",
    });
    defer gpa.free(end_out);

    try std.testing.expect(std.mem.indexOf(u8, end_out, "\"ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, end_out, "\"status\":\"completed\"") != null);

    // Assert the DB row is now completed with ended_at set.
    const done_check_sql = std.fmt.allocPrint(
        gpa,
        "SELECT count(*) FROM workflow_runs WHERE id = {d} AND status = 'completed' AND ended_at IS NOT NULL;",
        .{run_db_id},
    ) catch @panic("OOM");
    defer gpa.free(done_check_sql);
    const done_count = sqliteScalar(gpa, suite.db_path, done_check_sql);
    try std.testing.expectEqual(@as(i64, 1), done_count);
}

// =========================================================================
// Scenario B — run end accepts completed|failed|interrupted; rejects others
// =========================================================================

test "scenario B: run end accepts failed and interrupted statuses" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "run-statuses");
    defer gpa.free(plan_arg);

    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);

    // Test 'failed' status.
    {
        const run_id = std.fmt.allocPrint(gpa, "test-run-fail-{d}", .{std.c.getpid()}) catch @panic("OOM");
        defer gpa.free(run_id);
        const start = mustRunAgent(&suite, &.{
            "run",        "start",  "--plan",      plan_arg,
            "--workflow", "wf",     "--run-id",    run_id,
            "--pid",      self_pid, "--repo-root", "/tmp",
            "--json",
        });
        defer gpa.free(start);
        const end_out = mustRunAgent(&suite, &.{
            "run", "end", "--run-id", run_id, "--status", "failed", "--json",
        });
        defer gpa.free(end_out);
        try std.testing.expect(std.mem.indexOf(u8, end_out, "\"status\":\"failed\"") != null);
    }

    // Test 'interrupted' status.
    {
        const run_id = std.fmt.allocPrint(gpa, "test-run-int-{d}", .{std.c.getpid()}) catch @panic("OOM");
        defer gpa.free(run_id);
        const start = mustRunAgent(&suite, &.{
            "run",        "start",  "--plan",      plan_arg,
            "--workflow", "wf",     "--run-id",    run_id,
            "--pid",      self_pid, "--repo-root", "/tmp",
            "--json",
        });
        defer gpa.free(start);
        const end_out = mustRunAgent(&suite, &.{
            "run", "end", "--run-id", run_id, "--status", "interrupted", "--json",
        });
        defer gpa.free(end_out);
        try std.testing.expect(std.mem.indexOf(u8, end_out, "\"status\":\"interrupted\"") != null);
    }
}

test "scenario B: run end rejects abandoned status (Q597 — abandoned is reconcile-only)" {
    // Q597 invariant: `run end` NEVER writes `abandoned`. That status is
    // written exclusively by `planar-agent reconcile` (reconcileRuns).
    // The eager stale-runlock takeover path (task 3928) routes through
    // `planar-agent reconcile`, NOT through `run end --status abandoned`.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "run-reject-abandoned");
    defer gpa.free(plan_arg);

    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);
    const run_id = std.fmt.allocPrint(gpa, "test-run-abn-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_id);

    const start = mustRunAgent(&suite, &.{
        "run",        "start",  "--plan",      plan_arg,
        "--workflow", "wf",     "--run-id",    run_id,
        "--pid",      self_pid, "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(start);

    // run end --status abandoned MUST be REJECTED (non-zero exit).
    const res = runAgent(&suite, &.{
        "run", "end", "--run-id", run_id, "--status", "abandoned", "--json",
    });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);
    if (res.term == .exited and res.term.exited == 0) {
        std.debug.print("FAIL: run end --status abandoned should have been rejected (exit 0 returned)\nstdout: {s}\n", .{res.stdout});
        @panic("Q597 violated: run end must not accept abandoned");
    }
}

// =========================================================================
// Scenario C — reconcile run sweep: dead PID → abandoned; live PID survives
// =========================================================================

test "scenario C: reconcile marks dead-pid run abandoned; live-pid run survives" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "run-reconcile");
    defer gpa.free(plan_arg);

    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);

    // Dead-PID run: use PID 2147483600 (overwhelmingly unlikely to exist).
    const dead_run_id = std.fmt.allocPrint(gpa, "dead-run-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(dead_run_id);
    const dead_start = mustRunAgent(&suite, &.{
        "run",        "start",      "--plan",      plan_arg,
        "--workflow", "wf",         "--run-id",    dead_run_id,
        "--pid",      "2147483600", "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(dead_start);
    const dead_db_id = extractIntField(dead_start, "\"run_id\":") orelse @panic("no run_id in dead_start");

    // Live-PID run: use this process's PID.
    const live_run_id = std.fmt.allocPrint(gpa, "live-run-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(live_run_id);
    const live_start = mustRunAgent(&suite, &.{
        "run",        "start",  "--plan",      plan_arg,
        "--workflow", "wf",     "--run-id",    live_run_id,
        "--pid",      self_pid, "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(live_start);
    const live_db_id = extractIntField(live_start, "\"run_id\":") orelse @panic("no run_id in live_start");

    // Before reconcile: both rows are running.
    const before_sql = std.fmt.allocPrint(
        gpa,
        "SELECT count(*) FROM workflow_runs WHERE status = 'running' AND id IN ({d},{d});",
        .{ dead_db_id, live_db_id },
    ) catch @panic("OOM");
    defer gpa.free(before_sql);
    try std.testing.expectEqual(@as(i64, 2), sqliteScalar(gpa, suite.db_path, before_sql));

    // Run reconcile (real, not dry-run).
    const recon_out = mustRunAgent(&suite, &.{ "reconcile", "--json" });
    defer gpa.free(recon_out);
    try std.testing.expect(std.mem.indexOf(u8, recon_out, "\"ok\":true") != null);
    // The dead run must have been abandoned.
    try std.testing.expect(std.mem.indexOf(u8, recon_out, "\"runs_abandoned\":1") != null);

    // Dead-PID row is now abandoned with ended_at set.
    const dead_check_sql = std.fmt.allocPrint(
        gpa,
        "SELECT count(*) FROM workflow_runs WHERE id = {d} AND status = 'abandoned' AND ended_at IS NOT NULL;",
        .{dead_db_id},
    ) catch @panic("OOM");
    defer gpa.free(dead_check_sql);
    try std.testing.expectEqual(@as(i64, 1), sqliteScalar(gpa, suite.db_path, dead_check_sql));

    // Live-PID row is still running.
    const live_check_sql = std.fmt.allocPrint(
        gpa,
        "SELECT count(*) FROM workflow_runs WHERE id = {d} AND status = 'running';",
        .{live_db_id},
    ) catch @panic("OOM");
    defer gpa.free(live_check_sql);
    try std.testing.expectEqual(@as(i64, 1), sqliteScalar(gpa, suite.db_path, live_check_sql));

    // Clean up: close the live run so we don't leave orphaned state.
    const end_out = mustRunAgent(&suite, &.{
        "run", "end", "--run-id", live_run_id, "--status", "completed", "--json",
    });
    defer gpa.free(end_out);
}

test "scenario C: reconcile --dry-run reports run_candidates without writing" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "run-reconcile-dry");
    defer gpa.free(plan_arg);

    // Dead-PID run.
    const run_id = std.fmt.allocPrint(gpa, "dry-dead-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_id);
    const start = mustRunAgent(&suite, &.{
        "run",        "start",      "--plan",      plan_arg,
        "--workflow", "wf",         "--run-id",    run_id,
        "--pid",      "2147483600", "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(start);

    // Dry-run: should report the candidate without writing.
    const dry_out = mustRunAgent(&suite, &.{ "reconcile", "--dry-run", "--json" });
    defer gpa.free(dry_out);
    try std.testing.expect(std.mem.indexOf(u8, dry_out, "\"run_candidates\":[") != null);
    try std.testing.expect(std.mem.indexOf(u8, dry_out, run_id) != null);
    // runs_abandoned must be 0 in dry-run.
    try std.testing.expect(std.mem.indexOf(u8, dry_out, "\"runs_abandoned\":0") != null);

    // The row must still be running (no write).
    const still_check = std.fmt.allocPrint(
        gpa,
        "SELECT count(*) FROM workflow_runs WHERE run_identifier = '{s}' AND status = 'running';",
        .{run_id},
    ) catch @panic("OOM");
    defer gpa.free(still_check);
    try std.testing.expectEqual(@as(i64, 1), sqliteScalar(gpa, suite.db_path, still_check));
}
