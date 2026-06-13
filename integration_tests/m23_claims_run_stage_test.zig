//! integration_tests/m23_claims_run_stage_test.zig — black-box integration
//! tests for migration 00023: agent_work_claims.run_id + .stage columns and
//! the --run/--stage flags on `planar-agent pull` and `planar-agent claim`.
//!
//! Per CLAUDE.md § integration test methodology: each test walks a realistic
//! workflow, asserts post-state via JSON output, and does NOT rely only on
//! exit code 0.
//!
//! Covered scenarios:
//!
//!   - Scenario A (pull with --run/--stage): pull acquires a claim with run_id
//!     and stage set; JSON output contains both fields.
//!   - Scenario B (claim with --run/--stage): direct claim primitive populates
//!     run_id and stage; JSON output contains both fields.
//!   - Scenario C (pull without --run/--stage): run_id and stage are null in
//!     JSON — additive-ness invariant holds.
//!   - Scenario D (claim without --run/--stage): same null invariant.

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

/// Run planar-agent with the suite's PLANAR_DB injected.
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

/// Assert exit 0, return stdout (caller owns).
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

/// Seed an init + plan, return the plan id string (caller frees).
fn seedPlan(suite: *const harness.Suite, slug: []const u8) []u8 {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", slug, "--json", slug });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    return std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
}

/// Extract the first integer value for `key` from a JSON string.
fn extractIntField(s: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, s, key) orelse return null;
    var i = idx + key.len;
    while (i < s.len and (s[i] == ' ' or s[i] == ':' or s[i] == '\t')) i += 1;
    var end = i;
    while (end < s.len and s[end] >= '0' and s[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, s[i..end], 10) catch null;
}

/// Return true if `haystack` contains `needle`.
fn contains(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

// =========================================================================
// Scenario A — pull with --run/--stage populates both columns on the claim
// =========================================================================

test "scenario A: pull --run --stage stamps run_id and stage on the returned claim" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "rs-pull");
    defer gpa.free(plan_arg);

    // Seed one todo task so pull has work.
    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_arg, "--json", "Run stage pull task",
    });
    defer gpa.free(task_json);

    // Start a workflow run to get a run_id.
    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);
    const run_identifier = std.fmt.allocPrint(gpa, "rs-pull-test-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_identifier);

    const run_start_out = mustRunAgent(&suite, &.{
        "run",         "start",
        "--plan",      plan_arg,
        "--workflow",  "test-wf",
        "--run-id",    run_identifier,
        "--pid",       self_pid,
        "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(run_start_out);

    // Extract the workflow run row id from the JSON.
    const wf_run_id = extractIntField(run_start_out, "\"run_id\"") orelse
        @panic("no run_id in run start output");
    const run_id_str = std.fmt.allocPrint(gpa, "{d}", .{wf_run_id}) catch @panic("OOM");
    defer gpa.free(run_id_str);

    // Pull with --run and --stage.
    const pull_out = mustRunAgent(&suite, &.{
        "pull",                plan_arg,
        "--no-locality-probe", "--run",
        run_id_str,            "--stage",
        "code",                "--json",
    });
    defer gpa.free(pull_out);

    // Assert: no_work=false, claim present.
    try std.testing.expect(contains(pull_out, "\"no_work\":false"));
    try std.testing.expect(contains(pull_out, "\"claim_token\""));

    // Assert: run_id and stage appear in the claim object.
    const run_id_key = std.fmt.allocPrint(gpa, "\"run_id\":{d}", .{wf_run_id}) catch @panic("OOM");
    defer gpa.free(run_id_key);
    try std.testing.expect(contains(pull_out, run_id_key));
    try std.testing.expect(contains(pull_out, "\"stage\":\"code\""));
}

// =========================================================================
// Scenario B — claim with --run/--stage populates both columns
// =========================================================================

test "scenario B: claim --entity --run --stage stamps run_id and stage on the returned claim" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "rs-claim");
    defer gpa.free(plan_arg);

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_arg, "--json", "Run stage claim task",
    });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");
    const task_ref = std.fmt.allocPrint(gpa, "task:{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(task_ref);

    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);
    const run_identifier = std.fmt.allocPrint(gpa, "rs-claim-test-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_identifier);

    const run_start_out = mustRunAgent(&suite, &.{
        "run",         "start",
        "--plan",      plan_arg,
        "--workflow",  "test-wf",
        "--run-id",    run_identifier,
        "--pid",       self_pid,
        "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(run_start_out);
    const wf_run_id = extractIntField(run_start_out, "\"run_id\"") orelse
        @panic("no run_id in run start output");
    const run_id_str = std.fmt.allocPrint(gpa, "{d}", .{wf_run_id}) catch @panic("OOM");
    defer gpa.free(run_id_str);

    const claim_out = mustRunAgent(&suite, &.{
        "claim",
        "--entity",
        task_ref,
        "--no-locality-probe",
        "--run",
        run_id_str,
        "--stage",
        "review",
        "--json",
    });
    defer gpa.free(claim_out);

    try std.testing.expect(contains(claim_out, "\"ok\":true"));
    try std.testing.expect(contains(claim_out, "\"claim_token\""));

    const run_id_key = std.fmt.allocPrint(gpa, "\"run_id\":{d}", .{wf_run_id}) catch @panic("OOM");
    defer gpa.free(run_id_key);
    try std.testing.expect(contains(claim_out, run_id_key));
    try std.testing.expect(contains(claim_out, "\"stage\":\"review\""));
}

// =========================================================================
// Scenario C — pull WITHOUT --run/--stage leaves both null (additive-ness)
// =========================================================================

test "scenario C: pull without --run/--stage leaves run_id and stage null in claim JSON" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "rs-null-pull");
    defer gpa.free(plan_arg);

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_arg, "--json", "Null run stage pull task",
    });
    defer gpa.free(task_json);

    const pull_out = mustRunAgent(&suite, &.{
        "pull", plan_arg, "--no-locality-probe", "--json",
    });
    defer gpa.free(pull_out);

    try std.testing.expect(contains(pull_out, "\"no_work\":false"));
    // Both columns must be null — not missing, not non-null.
    try std.testing.expect(contains(pull_out, "\"run_id\":null"));
    try std.testing.expect(contains(pull_out, "\"stage\":null"));
}

// =========================================================================
// Scenario D — claim WITHOUT --run/--stage leaves both null (additive-ness)
// =========================================================================

test "scenario D: claim without --run/--stage leaves run_id and stage null in claim JSON" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "rs-null-claim");
    defer gpa.free(plan_arg);

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_arg, "--json", "Null run stage claim task",
    });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");
    const task_ref = std.fmt.allocPrint(gpa, "task:{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(task_ref);

    const claim_out = mustRunAgent(&suite, &.{
        "claim", "--entity", task_ref, "--no-locality-probe", "--json",
    });
    defer gpa.free(claim_out);

    try std.testing.expect(contains(claim_out, "\"ok\":true"));
    try std.testing.expect(contains(claim_out, "\"run_id\":null"));
    try std.testing.expect(contains(claim_out, "\"stage\":null"));
}

// =========================================================================
// Scenario E — pull --stage without --run is rejected (guard)
// =========================================================================

test "scenario E: pull --stage without --run fails with non-zero exit" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "rs-guard-pull");
    defer gpa.free(plan_arg);

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_arg, "--json", "Guard pull task",
    });
    defer gpa.free(task_json);

    // Supplying --stage without --run must be rejected (exit non-zero).
    const res = runAgent(&suite, &.{
        "pull", plan_arg, "--no-locality-probe", "--stage", "code", "--json",
    });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // Must exit non-zero.
    if (res.term == .exited and res.term.exited == 0) {
        std.debug.print("expected non-zero exit for pull --stage without --run, got 0\nstdout: {s}\n", .{res.stdout});
        @panic("pull --stage without --run should have failed");
    }
    // Error message must mention --stage requires --run.
    const combined = std.mem.concat(gpa, u8, &.{ res.stdout, res.stderr }) catch @panic("OOM");
    defer gpa.free(combined);
    if (!contains(combined, "--stage requires --run")) {
        std.debug.print("expected '--stage requires --run' in output, got:\n{s}\n{s}\n", .{ res.stdout, res.stderr });
        @panic("missing expected error message");
    }
}

// =========================================================================
// Scenario F — claim --stage without --run is rejected (guard)
// =========================================================================

test "scenario F: claim --stage without --run fails with non-zero exit" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "rs-guard-claim");
    defer gpa.free(plan_arg);

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_arg, "--json", "Guard claim task",
    });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");
    const task_ref = std.fmt.allocPrint(gpa, "task:{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(task_ref);

    // Supplying --stage without --run must be rejected (exit non-zero).
    const res = runAgent(&suite, &.{
        "claim", "--entity", task_ref, "--no-locality-probe", "--stage", "review", "--json",
    });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // Must exit non-zero.
    if (res.term == .exited and res.term.exited == 0) {
        std.debug.print("expected non-zero exit for claim --stage without --run, got 0\nstdout: {s}\n", .{res.stdout});
        @panic("claim --stage without --run should have failed");
    }
    // Error message must mention --stage requires --run.
    const combined = std.mem.concat(gpa, u8, &.{ res.stdout, res.stderr }) catch @panic("OOM");
    defer gpa.free(combined);
    if (!contains(combined, "--stage requires --run")) {
        std.debug.print("expected '--stage requires --run' in output, got:\n{s}\n{s}\n", .{ res.stdout, res.stderr });
        @panic("missing expected error message");
    }
}
