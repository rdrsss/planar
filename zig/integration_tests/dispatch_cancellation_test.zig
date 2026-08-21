//! Cancellation and failure must never strand work (plan 950 task 5531).
//!
//! Every code-writing dispatch takes an exclusive claim. If a failure or a
//! cancellation left that claim active, the task would be invisible to the
//! next `pull` until its lease expired — the work would look like it was
//! still being done by a process that is gone. These tests prove both
//! terminal verbs release the claim AND return the task, in one step, so the
//! work is immediately re-dispatchable.
//!
//! The redaction half of the contract (diagnostics record flag NAMES and
//! positional ARITY, never values) is pinned by `cli_log_test.zig`, including
//! against hostile input.

const std = @import("std");
const harness = @import("harness");

fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_AGENT_BIN=")) return s["PLANAR_AGENT_BIN=".len..];
    }
    @panic("PLANAR_AGENT_BIN is not set; run via make test-integration");
}

fn runAgent(suite: *const harness.Suite, args: []const []const u8) harness.Suite.RunResult {
    const gpa = suite.allocator;
    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(gpa);
    argv.append(gpa, resolveAgentBin()) catch @panic("OOM");
    for (args) |a| argv.append(gpa, a) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var count: usize = 0;
    while (raw[count] != null) : (count += 1) {}
    const block: std.process.Environ.PosixBlock = .{ .slice = @ptrCast(raw[0..count :null]) };
    var env = (std.process.Environ{ .block = block }).createMap(gpa) catch @panic("OOM");
    defer env.deinit();
    env.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    const r = std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .environ_map = &env,
    }) catch |e| std.debug.panic("spawn failed: {s}", .{@errorName(e)});
    return .{ .stdout = r.stdout, .stderr = r.stderr, .term = r.term };
}

fn mustRunAgent(suite: *const harness.Suite, args: []const []const u8) []u8 {
    const r = runAgent(suite, args);
    defer suite.allocator.free(r.stderr);
    if (r.term != .exited or r.term.exited != 0) {
        std.debug.print("planar-agent failed: {any}\n{s}\n{s}\n", .{ r.term, r.stdout, r.stderr });
        @panic("planar-agent must-run failed");
    }
    return r.stdout;
}

fn extractString(json: []const u8, key: []const u8) ?[]const u8 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    const start = idx + key.len;
    const end = std.mem.indexOfScalarPos(u8, json, start, '"') orelse return null;
    return json[start..end];
}

fn extractInt(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

/// Seed a plan with one todo task and return the plan id as a string.
fn seedPlan(suite: *harness.Suite, slug: []const u8) []u8 {
    const gpa = suite.allocator;
    const root = suite.registerProject(slug);
    suite.addAssoc(slug, null);

    const plan_json = suite.mustRunInDir(root, &.{ "plan", "create", "Cancellation plan", "--json" });
    defer gpa.free(plan_json);
    const plan_id = extractInt(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");

    const task_json = suite.mustRunInDir(root, &.{
        "task", "add", "--plan", plan_arg, "--json", "work that will not finish",
    });
    gpa.free(task_json);
    return plan_arg;
}

/// Pull the next task and return its claim token (caller frees).
fn pullClaim(suite: *harness.Suite, plan_arg: []const u8) []u8 {
    const gpa = suite.allocator;
    const out = mustRunAgent(suite, &.{ "pull", plan_arg, "--role", "coder", "--json" });
    defer gpa.free(out);
    const token = extractString(out, "\"claim_token\":\"") orelse {
        std.debug.print("pull output had no claim_token:\n{s}\n", .{out});
        @panic("no claim token");
    };
    return gpa.dupe(u8, token) catch @panic("OOM");
}

test "a failed dispatch releases its claim and the work is immediately re-dispatchable" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const gpa = suite.allocator;

    const plan_arg = seedPlan(&suite, "cancel-fail");
    defer gpa.free(plan_arg);

    const token = pullClaim(&suite, plan_arg);
    defer gpa.free(token);

    // While claimed, the work must NOT be handed to a second agent.
    const contended = runAgent(&suite, &.{ "pull", plan_arg, "--role", "coder", "--json" });
    defer contended.deinit(gpa);
    try std.testing.expect(std.mem.indexOf(u8, contended.stdout, "\"claim_token\"") == null);

    const failed = mustRunAgent(&suite, &.{
        "fail",              "--claim",
        token,               "--reason",
        "ran out of budget", "--category",
        "usage_limit",       "--no-locality-probe",
        "--json",
    });
    gpa.free(failed);

    // The claim is gone AND the task is back — one step, not two. Splitting
    // them is what strands a claim when the process dies in between.
    const reclaim = pullClaim(&suite, plan_arg);
    defer gpa.free(reclaim);
    try std.testing.expect(!std.mem.eql(u8, token, reclaim));
}

test "a released dispatch also returns the work, and is distinguishable from a failure" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const gpa = suite.allocator;

    const plan_arg = seedPlan(&suite, "cancel-release");
    defer gpa.free(plan_arg);

    const token = pullClaim(&suite, plan_arg);
    defer gpa.free(token);

    const released = mustRunAgent(&suite, &.{
        "release", "--claim", token, "--reason", "operator cancelled", "--json",
    });
    defer gpa.free(released);

    // `release` and `fail` must not collapse into one state: a graceful
    // give-up and an aborted run mean different things when an operator is
    // later reading why a task bounced.
    try std.testing.expect(std.mem.indexOf(u8, released, "aborted") == null);

    const reclaim = pullClaim(&suite, plan_arg);
    defer gpa.free(reclaim);
    try std.testing.expect(!std.mem.eql(u8, token, reclaim));
}

test "a terminal verb cannot be replayed against a spent claim" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const gpa = suite.allocator;

    const plan_arg = seedPlan(&suite, "cancel-replay");
    defer gpa.free(plan_arg);

    const token = pullClaim(&suite, plan_arg);
    defer gpa.free(token);

    const out = mustRunAgent(&suite, &.{
        "release", "--claim", token, "--reason", "first", "--json",
    });
    gpa.free(out);

    // Replaying a terminal verb would release a claim someone else may now
    // hold on the same task.
    const replay = runAgent(&suite, &.{
        "release", "--claim", token, "--reason", "second", "--json",
    });
    defer replay.deinit(gpa);
    try std.testing.expect(replay.term.exited != 0);
}
