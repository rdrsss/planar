//! integration_tests/planar_agent_test.zig — black-box tests for the
//! planar-agent binary scaffold (plan 85 M1.d + M1.e concurrency test).
//!
//! Covered:
//!   - planar-agent --help surfaces the registered verbs and exits 0.
//!   - planar-agent version emits a leading "planar-agent " token and
//!     exits 0.
//!   - Two cross-process self-test-acquire invocations race against the
//!     same scratch SQLite DB; exactly one wins the claim, the other
//!     gets `no_work` (slug: claim-concurrency-tests, t#2536).
//!
//! Design note on the concurrency test: the spec offers two paths —
//! a temporary self-test verb or invoking the engine through a
//! test-harness verb. We picked the temporary `self-test-acquire`
//! verb route (registered in `src/cmd/planar-agent/handlers/`) and
//! marked it INTERNAL with a comment pointing at M2 for removal.
//! Rationale: it lets the test exercise the COMPILED binary's
//! BEGIN IMMEDIATE transaction inside the real cli.dispatch loop —
//! the exact code path M2's `pull` verb will use. A test-only Zig
//! function would skip the dispatch + handler argv parsing layer
//! and leave that contract untested at M1 close.

const std = @import("std");
const harness = @import("harness");

/// Resolve the planar-agent binary path from PLANAR_AGENT_BIN env var.
/// build.zig (and the Makefile) set this alongside PLANAR_BIN.
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

/// Run the planar-agent binary with `args`, returning stdout, stderr,
/// and exit term. Uses the same PLANAR_DB injection as the planar
/// harness so both binaries see the same scratch DB.
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

    // Inherit + inject PLANAR_DB the same way Suite.exec does.
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

test "planar-agent --help lists the M1 verb tree and exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runAgent(&suite, &.{"--help"});
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);
    // The verb table must include at least the M1 scaffold entries.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "version") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "self-test-acquire") != null);
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

test "planar-agent self-test-acquire on empty plan returns no_work" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed the DB via the operator binary so the schema is fully
    // migrated and the plan/task seeded the same way operators would.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "ag-empty", "--json", "empty" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");

    // No tasks → self-test-acquire must report no_work.
    const id_buf = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(id_buf);
    const res = runAgent(&suite, &.{ "self-test-acquire", id_buf });
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"no_work\":true") != null);
}

test "two planar-agent self-test-acquire processes race; exactly one wins" {
    // CONCURRENCY contract test (slug: claim-concurrency-tests, t#2536).
    //
    // Two child processes race the same atomic.pullNext through the
    // compiled binary against ONE scratch SQLite file. The BEGIN
    // IMMEDIATE writer lock inside store.acquireClaim must serialize
    // the "check no active claim, then insert" pair so exactly one
    // process succeeds. The other must see the task as already-claimed
    // and exit cleanly with `no_work`.
    //
    // No sentinel-file barrier is needed: `std.process.Child.spawn`
    // returns synchronously once the child is forked, and we launch
    // both children back-to-back before calling `wait` on either.
    // SQLite's writer lock dominates the resulting race; both processes
    // are inside `BEGIN IMMEDIATE` within microseconds of each other.

    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "ag-race", "--json", "race" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_arg = try std.fmt.allocPrint(gpa, "{d}", .{plan_id});
    defer gpa.free(plan_id_arg);
    const seed_out = suite.mustRun(&.{
        "task",      "add",
        "--plan",    plan_id_arg,
        "contended",
    });
    gpa.free(seed_out);

    // Build the agent argv as ["planar-agent", "self-test-acquire", "<plan_id>"].
    const agent_bin = resolveAgentBin();
    const id_buf = try std.fmt.allocPrint(gpa, "{d}", .{plan_id});
    defer gpa.free(id_buf);

    const argv = &[_][]const u8{ agent_bin, "self-test-acquire", id_buf, "--vendor", "race-test" };

    // Build the env map once and reuse for both children.
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    // Spawn both children before awaiting either. The launches are
    // synchronous (the spawn call returns once the fork completes), so
    // the race window inside acquireClaim is sub-millisecond — both
    // processes hit `BEGIN IMMEDIATE` within microseconds and SQLite's
    // writer lock serializes them.
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

    // Drain the pipes BEFORE waiting so the children don't block on
    // a full pipe buffer. Each output is small (one JSON line) so a
    // single read suffices in practice.
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

    // Both must exit 0 — neither path is an error (winner returns a
    // claim_token, loser returns no_work).
    if (term1 != .exited or term1.exited != 0) {
        std.debug.print("c1 stdout: {s}\nc1 stderr: {s}\n", .{ stdout1, stderr1 });
        return error.Child1Failed;
    }
    if (term2 != .exited or term2.exited != 0) {
        std.debug.print("c2 stdout: {s}\nc2 stderr: {s}\n", .{ stdout2, stderr2 });
        return error.Child2Failed;
    }

    const s1: []const u8 = stdout1;
    const s2: []const u8 = stdout2;

    const c1_no_work = std.mem.indexOf(u8, s1, "\"no_work\":true") != null;
    const c1_won = std.mem.indexOf(u8, s1, "\"claim_token\":\"") != null;
    const c2_no_work = std.mem.indexOf(u8, s2, "\"no_work\":true") != null;
    const c2_won = std.mem.indexOf(u8, s2, "\"claim_token\":\"") != null;

    // Exactly one winner. Both winning OR both no-working both break
    // the exclusivity contract.
    if (c1_won == c2_won) {
        std.debug.print(
            "concurrency contract broken: c1_won={any} c2_won={any}\nc1 stdout: {s}\nc2 stdout: {s}\n",
            .{ c1_won, c2_won, s1, s2 },
        );
        return error.ExactlyOneWinnerExpected;
    }
    try std.testing.expect(c1_no_work != c2_no_work);
    try std.testing.expect((c1_won and c2_no_work) or (c2_won and c1_no_work));

    // Report the wall-clock for the test log; the planner reads this
    // back in the report. ~ms for two child spawns + the claim
    // transaction is the working assumption; we don't enforce a hard
    // bound here, just print it.
    std.debug.print("[plan85-claim-concurrency] elapsed_ns={d} ({d:.3}ms) winner={s}\n", .{
        elapsed_ns,
        @as(f64, @floatFromInt(elapsed_ns)) / 1_000_000.0,
        if (c1_won) "c1" else "c2",
    });
}

/// monotonicNs returns a monotonic clock reading in nanoseconds via
/// `Io.Clock.awake.now(io).toNanos()`. Used by the concurrency test
/// to report the wall-clock window during which both child processes
/// raced against the same DB. Instrumentation only — no test
/// invariant depends on it being precise.
fn monotonicNs() u64 {
    const ts: std.Io.Timestamp = std.Io.Clock.awake.now(std.testing.io);
    return @intCast(ts.toNanoseconds());
}

/// Drain a child process's stdout/stderr pipe to an allocator-owned
/// byte slice. The pipe's writer end is held by the child; we read
/// until EOF (the child closes the pipe at exit). Cap at 1 MiB so a
/// runaway child can't OOM the test runner.
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

/// Extract an integer from a JSON blob by locating `key` (which must
/// include the surrounding double-quotes, e.g. `"\"id\""`), advancing
/// past the colon, and parsing decimal digits. Returns null when the
/// key is absent or the value is not an integer.
fn extractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}
