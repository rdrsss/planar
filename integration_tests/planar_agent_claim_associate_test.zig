//! integration_tests/planar_agent_claim_associate_test.zig — black-box
//! integration tests for `planar-agent claim-associate` (plan 586 task 3948).
//!
//! Covered scenarios:
//!
//!   Scenario A — basic association: acquire a claim WITHOUT run_id/stage,
//!     run `claim-associate --claim <tok> --run <id> --stage <s>`, then verify
//!     via a `planar-agent context add --claim` call (which needs run_id to
//!     work) that the live loop is closed.
//!
//!   Scenario B — null stage: `claim-associate --claim <tok> --run <id>` (no
//!     --stage) succeeds; `updated=1` in JSON output.
//!
//!   Scenario C — no-op on missing claim: a made-up token exits 0 with
//!     `{"ok":true,"updated":0}`.
//!
//!   Scenario D — --run required: omitting --run must fail with non-zero exit.
//!
//! Per CLAUDE.md § integration test methodology: each test walks a realistic
//! workflow and asserts post-state via JSON output, not just exit code.

const std = @import("std");
const harness = @import("harness");

/// Resolve the planar-agent binary from PLANAR_AGENT_BIN env var.
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

/// Run planar-agent with the suite's PLANAR_DB injected; return RunResult.
fn runAgent(suite: *const harness.Suite, args: []const []const u8) harness.Suite.RunResult {
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

/// Assert exit 0 and return stdout (caller owns).
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

/// Seed init + plan + task; return plan id string (caller frees).
fn seedPlanAndTask(suite: *const harness.Suite, slug: []const u8) struct { plan_id: []u8, task_id: i64 } {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", slug, "--json", slug });
    defer gpa.free(plan_json);
    const plan_id_int = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id = std.fmt.allocPrint(gpa, "{d}", .{plan_id_int}) catch @panic("OOM");

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_id, "--json", "associate test task",
    });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");

    return .{ .plan_id = plan_id, .task_id = task_id };
}

/// Open a workflow_runs row via `run start`; return the DB row id.
fn openRunRow(suite: *const harness.Suite, plan_id: []const u8, run_identifier: []const u8) i64 {
    const gpa = suite.allocator;
    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);

    const out = mustRunAgent(suite, &.{
        "run",         "start",
        "--plan",      plan_id,
        "--workflow",  "test-wf",
        "--run-id",    run_identifier,
        "--pid",       self_pid,
        "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(out);
    return extractIntField(out, "\"run_id\"") orelse @panic("no run_id in run start output");
}

/// Extract first integer value for `key` from a JSON string.
fn extractIntField(s: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, s, key) orelse return null;
    var i = idx + key.len;
    while (i < s.len and (s[i] == ' ' or s[i] == ':' or s[i] == '\t')) i += 1;
    var end = i;
    while (end < s.len and ((s[end] >= '0' and s[end] <= '9') or s[end] == '-')) end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, s[i..end], 10) catch null;
}

fn contains(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

// =========================================================================
// Scenario A — basic association then context add succeeds
// =========================================================================

test "scenario A: claim-associate stamps run_id+stage; subsequent context add --claim succeeds" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedPlanAndTask(&suite, "ca-assoc-a");
    defer gpa.free(seed.plan_id);
    const task_ref = std.fmt.allocPrint(gpa, "task:{d}", .{seed.task_id}) catch @panic("OOM");
    defer gpa.free(task_ref);

    // Acquire claim WITHOUT run_id/stage.
    const claim_out = mustRunAgent(&suite, &.{
        "claim", "--entity", task_ref, "--no-locality-probe", "--json",
    });
    defer gpa.free(claim_out);
    try std.testing.expect(contains(claim_out, "\"ok\":true"));
    // Extract the claim token from JSON output.
    const token_start = (std.mem.indexOf(u8, claim_out, "\"claim_token\":\"") orelse @panic("no claim_token")) + "\"claim_token\":\"".len;
    const token_end = std.mem.indexOf(u8, claim_out[token_start..], "\"") orelse @panic("no closing quote");
    const claim_token = claim_out[token_start .. token_start + token_end];

    // Open a workflow_runs row.
    const run_id_int = openRunRow(&suite, seed.plan_id, "ca-assoc-a-run-1");
    const run_id_str = std.fmt.allocPrint(gpa, "{d}", .{run_id_int}) catch @panic("OOM");
    defer gpa.free(run_id_str);

    // Associate the claim with the run + stage.
    const assoc_out = mustRunAgent(&suite, &.{
        "claim-associate", "--claim", claim_token, "--run", run_id_str, "--stage", "code", "--json",
    });
    defer gpa.free(assoc_out);
    try std.testing.expect(contains(assoc_out, "\"ok\":true"));
    try std.testing.expect(contains(assoc_out, "\"updated\":1"));

    // Now `context add --claim <token>` should succeed (it resolves run_id from the claim).
    // A success (exit 0) is the proof the live loop is closed — the engine reads run_id
    // from the claim row (decision 447/450) and context add requires it.
    const ctx_out = mustRunAgent(&suite, &.{
        "context", "add",
        "--claim", claim_token,
        "--kind",  "finding",
        "--body",  "test-finding from claim-associate integration test",
        "--json",
    });
    defer gpa.free(ctx_out);
    // context add exits 0 and returns JSON with "ok":true when the claim carries run_id.
    try std.testing.expect(contains(ctx_out, "\"ok\":true"));
}

// =========================================================================
// Scenario B — --stage omitted: associate with run only (stage stays NULL)
// =========================================================================

test "scenario B: claim-associate without --stage stamps run_id only; updated=1" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedPlanAndTask(&suite, "ca-assoc-b");
    defer gpa.free(seed.plan_id);
    const task_ref = std.fmt.allocPrint(gpa, "task:{d}", .{seed.task_id}) catch @panic("OOM");
    defer gpa.free(task_ref);

    const claim_out = mustRunAgent(&suite, &.{
        "claim", "--entity", task_ref, "--no-locality-probe", "--json",
    });
    defer gpa.free(claim_out);
    const token_start = (std.mem.indexOf(u8, claim_out, "\"claim_token\":\"") orelse @panic("no claim_token")) + "\"claim_token\":\"".len;
    const token_end = std.mem.indexOf(u8, claim_out[token_start..], "\"") orelse @panic("no closing quote");
    const claim_token = claim_out[token_start .. token_start + token_end];

    const run_id_int = openRunRow(&suite, seed.plan_id, "ca-assoc-b-run-1");
    const run_id_str = std.fmt.allocPrint(gpa, "{d}", .{run_id_int}) catch @panic("OOM");
    defer gpa.free(run_id_str);

    // No --stage: associate with run only.
    const assoc_out = mustRunAgent(&suite, &.{
        "claim-associate", "--claim", claim_token, "--run", run_id_str, "--json",
    });
    defer gpa.free(assoc_out);
    try std.testing.expect(contains(assoc_out, "\"ok\":true"));
    try std.testing.expect(contains(assoc_out, "\"updated\":1"));
}

// =========================================================================
// Scenario C — no-op on nonexistent token: exits 0 with updated=0
// =========================================================================

test "scenario C: claim-associate with nonexistent token exits 0 with updated=0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Need at least init for the DB.
    const seed = seedPlanAndTask(&suite, "ca-assoc-c");
    defer gpa.free(seed.plan_id);

    const run_id_int = openRunRow(&suite, seed.plan_id, "ca-assoc-c-run-1");
    const run_id_str = std.fmt.allocPrint(gpa, "{d}", .{run_id_int}) catch @panic("OOM");
    defer gpa.free(run_id_str);

    // A made-up 32-char hex token — does not exist in the DB.
    const fake_token = "deadbeefdeadbeefdeadbeefdeadbeef";
    const assoc_out = mustRunAgent(&suite, &.{
        "claim-associate", "--claim", fake_token, "--run", run_id_str, "--json",
    });
    defer gpa.free(assoc_out);
    // Must exit 0 (best-effort; updated=0 is the no-op signal).
    try std.testing.expect(contains(assoc_out, "\"ok\":true"));
    try std.testing.expect(contains(assoc_out, "\"updated\":0"));
}

// =========================================================================
// Scenario D — --run is required: omitting it fails non-zero
// =========================================================================

test "scenario D: claim-associate without --run fails with non-zero exit" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedPlanAndTask(&suite, "ca-assoc-d");
    defer gpa.free(seed.plan_id);
    const task_ref = std.fmt.allocPrint(gpa, "task:{d}", .{seed.task_id}) catch @panic("OOM");
    defer gpa.free(task_ref);

    const claim_out = mustRunAgent(&suite, &.{
        "claim", "--entity", task_ref, "--no-locality-probe", "--json",
    });
    defer gpa.free(claim_out);
    const token_start = (std.mem.indexOf(u8, claim_out, "\"claim_token\":\"") orelse @panic("no claim_token")) + "\"claim_token\":\"".len;
    const token_end = std.mem.indexOf(u8, claim_out[token_start..], "\"") orelse @panic("no closing quote");
    const claim_token = claim_out[token_start .. token_start + token_end];

    // Omit --run: the parser must reject this.
    const res = runAgent(&suite, &.{
        "claim-associate", "--claim", claim_token, "--json",
    });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    if (res.term == .exited and res.term.exited == 0) {
        std.debug.print("expected non-zero exit for claim-associate without --run, got 0\nstdout: {s}\n", .{res.stdout});
        @panic("claim-associate without --run should have failed");
    }
}
