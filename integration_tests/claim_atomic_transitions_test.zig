//! integration_tests/claim_atomic_transitions_test.zig
//!
//! Black-box CLI tests for the claim-atomic task-status transition guard
//! introduced by decision 533 / task 4165.
//!
//! Contract pinned:
//!   - `task done <id>` on a task with an active claim exits NON-ZERO and
//!     prints an "active work claim" refuse message; task status unchanged.
//!   - `task done <id> --force` on a claimed task succeeds and flips status.
//!   - `task block <id> --on <blocker>` on a claimed task exits NON-ZERO.
//!   - `task update <id> --status done` on a claimed task exits NON-ZERO;
//!     `--status done --force` succeeds.
//!   - An unclaimed task transitions normally (unchanged contract).
//!
//! Claims are seeded via `planar-agent pull` — the real agent path.
//!
//! Run via: make test-integration

const std = @import("std");
const harness = @import("harness");

// -------------------------------------------------------------------------
// Agent-binary helpers (mirrors planar_agent_test.zig conventions)
// -------------------------------------------------------------------------

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

fn mustRunAgent(
    suite: *const harness.Suite,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const res = runAgent(suite, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("planar-agent failed (term={any}):\n{s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

fn extractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

/// Seed: init + plan + one todo task. Returns plan_id as a heap string (caller frees).
fn seedPlan(suite: *const harness.Suite, slug: []const u8, task_title: []const u8) []u8 {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", slug, "--json", slug });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    const task_out = suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, task_title });
    gpa.free(task_out);
    return plan_id_arg;
}

/// Pull a task claim for `plan_id_arg`. Returns claim_token (caller frees) and task_id.
const PullResult = struct { claim_token: []u8, task_id: i64 };

fn pullClaim(suite: *const harness.Suite, plan_id_arg: []const u8) PullResult {
    const gpa = suite.allocator;
    const stdout = mustRunAgent(suite, &.{ "pull", plan_id_arg, "--no-locality-probe", "--json" });
    defer gpa.free(stdout);

    const task_id = extractIntField(stdout, "\"task\":{\"id\"") orelse @panic("no task.id in pull JSON");
    // Extract claim_token string field.
    const prefix = "\"claim_token\":\"";
    const tok_start = std.mem.indexOf(u8, stdout, prefix) orelse @panic("no claim_token in pull JSON");
    const body = stdout[tok_start + prefix.len ..];
    const tok_end = std.mem.indexOf(u8, body, "\"") orelse @panic("unterminated claim_token");
    const token = gpa.dupe(u8, body[0..tok_end]) catch @panic("OOM");
    return .{ .claim_token = token, .task_id = task_id };
}

// =========================================================================
// Tests
// =========================================================================

test "claim-atomic: task done on claimed task exits non-zero; status unchanged" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_id_arg = seedPlan(&suite, "ca-done-refuse", "CA done refuse task");
    defer gpa.free(plan_id_arg);

    const pull = pullClaim(&suite, plan_id_arg);
    defer gpa.free(pull.claim_token);
    const tid_arg = std.fmt.allocPrint(gpa, "{d}", .{pull.task_id}) catch @panic("OOM");
    defer gpa.free(tid_arg);

    // Post-pull: task should be "doing".
    const before = suite.mustRun(&.{ "task", "show", "--json", tid_arg });
    defer gpa.free(before);
    try std.testing.expect(std.mem.indexOf(u8, before, "\"status\":\"doing\"") != null);

    // task done — MUST be refused (claim-atomic guard).
    const stderr = suite.expectFailure(&.{ "task", "done", tid_arg });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "active work claim") != null);

    // Post-state: status MUST still be "doing" — no write happened.
    const after = suite.mustRun(&.{ "task", "show", "--json", tid_arg });
    defer gpa.free(after);
    try std.testing.expect(std.mem.indexOf(u8, after, "\"status\":\"doing\"") != null);
}

test "claim-atomic: task done --force on claimed task succeeds" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_id_arg = seedPlan(&suite, "ca-done-force", "CA done force task");
    defer gpa.free(plan_id_arg);

    const pull = pullClaim(&suite, plan_id_arg);
    defer gpa.free(pull.claim_token);
    const tid_arg = std.fmt.allocPrint(gpa, "{d}", .{pull.task_id}) catch @panic("OOM");
    defer gpa.free(tid_arg);

    // --force overrides the claim guard.
    const out = suite.mustRun(&.{ "task", "done", tid_arg, "--force", "--json" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"status\":\"done\"") != null);

    // Confirm via show.
    const show = suite.mustRun(&.{ "task", "show", "--json", tid_arg });
    defer gpa.free(show);
    try std.testing.expect(std.mem.indexOf(u8, show, "\"status\":\"done\"") != null);
}

test "claim-atomic: task block on claimed task exits non-zero; status unchanged" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_id_arg = seedPlan(&suite, "ca-block-refuse", "CA block refuse task");
    defer gpa.free(plan_id_arg);

    // Add a second task to serve as the blocker.
    const blocker_out = suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, "--json", "Blocker task" });
    defer gpa.free(blocker_out);
    const blocker_id = extractIntField(blocker_out, "\"id\"") orelse @panic("no blocker id");
    const blocker_arg = std.fmt.allocPrint(gpa, "{d}", .{blocker_id}) catch @panic("OOM");
    defer gpa.free(blocker_arg);

    const pull = pullClaim(&suite, plan_id_arg);
    defer gpa.free(pull.claim_token);
    const tid_arg = std.fmt.allocPrint(gpa, "{d}", .{pull.task_id}) catch @panic("OOM");
    defer gpa.free(tid_arg);

    // task block — MUST be refused.
    const stderr = suite.expectFailure(&.{ "task", "block", tid_arg, "--on", blocker_arg });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "active work claim") != null);

    // Status unchanged ("doing").
    const after = suite.mustRun(&.{ "task", "show", "--json", tid_arg });
    defer gpa.free(after);
    try std.testing.expect(std.mem.indexOf(u8, after, "\"status\":\"doing\"") != null);
}

test "claim-atomic: task update --status done on claimed task exits non-zero; --force succeeds" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_id_arg = seedPlan(&suite, "ca-update-force", "CA update force task");
    defer gpa.free(plan_id_arg);

    const pull = pullClaim(&suite, plan_id_arg);
    defer gpa.free(pull.claim_token);
    const tid_arg = std.fmt.allocPrint(gpa, "{d}", .{pull.task_id}) catch @panic("OOM");
    defer gpa.free(tid_arg);

    // task update --status done — MUST be refused.
    const stderr = suite.expectFailure(&.{ "task", "update", tid_arg, "--status", "done" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "active work claim") != null);

    // --force overrides.
    const out = suite.mustRun(&.{ "task", "update", tid_arg, "--status", "done", "--force", "--json" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"status\":\"done\"") != null);
}

test "claim-atomic: unclaimed task done transitions normally (unchanged contract)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed: init + plan.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "ca-unclaimed", "--json", "ca-unclaimed" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_id_arg);

    // Add a task with --json to capture the id directly, no scope-dependent list needed.
    const task_json = suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, "--json", "Unclaimed task" });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");
    const tid_arg = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(tid_arg);

    // Advance to doing first — the status matrix requires todo → doing → done.
    const doing_out = suite.mustRun(&.{ "task", "update", tid_arg, "--status", "doing", "--json" });
    defer gpa.free(doing_out);
    try std.testing.expect(std.mem.indexOf(u8, doing_out, "\"status\":\"doing\"") != null);

    // No claim → done should succeed without --force.
    const out = suite.mustRun(&.{ "task", "done", tid_arg, "--json" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"status\":\"done\"") != null);
}
