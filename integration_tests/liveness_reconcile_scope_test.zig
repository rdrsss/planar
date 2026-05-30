//! integration_tests/liveness_reconcile_scope_test.zig — plan 493 F2.
//!
//! Verifies the scope-able-reconcile contract (S4 in test-spec #266):
//!
//!   S4 — `liveness-reconcile-session-scope`
//!
//! Two sessions A and B each pull a task on their own plan with a short
//! TTL. Both leases lapse so each session owns one expired-active claim.
//!
//!   1. `planar-agent reconcile --session <A>` MUST flip ONLY A's claim
//!      to `stale`; B's claim MUST remain `active`-but-expired.
//!   2. Then a bare `planar-agent reconcile` (no `--session`) MUST sweep
//!      B's still-expired-active claim — the global default is preserved
//!      bit-for-bit.
//!
//! Pre-F2 the binary has no `--session` flag at all, so the first call
//! fails with a CLI parse error. That is the documented RED failure.
//! Post-F2 the filtered SQL touches only the matching session's row;
//! the global path is unchanged. The "still-stale" observability check
//! uses `reconcile --dry-run --stale-after 0` (global) — a claim that
//! is still expired-and-active will appear as a candidate; a freshly
//! marked-stale claim will not.

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// planar-agent shell-out (mirrors planar_agent_test.zig / liveness_repull_test.zig)
// =========================================================================

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
        std.debug.print("planar-agent failed (term={any}): {s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

// =========================================================================
// Fixture helpers
// =========================================================================

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

fn seedAdditionalPlanWithTask(suite: *const harness.Suite, plan_slug: []const u8, task_title: []const u8) []u8 {
    const gpa = suite.allocator;
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", plan_slug, "--json", plan_slug });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    const task_out = suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, task_title });
    gpa.free(task_out);
    return plan_id_arg;
}

fn sleepSecs(s: u64) void {
    std.testing.io.sleep(std.Io.Duration.fromSeconds(@intCast(s)), std.Io.Clock.awake) catch {};
}

// =========================================================================
// S4 — Scoped reconcile touches only its own session.
// slug: liveness-reconcile-session-scope
// =========================================================================

test "S4 liveness-reconcile-session-scope: --session sweeps only matching claims; bare is global (plan 493 F2)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Two distinct plans so the two tasks don't contend with each other
    // on selection. The decision boundary the test pins is session-scope,
    // not plan-scope — but using different plans keeps the pulls clean.
    const pidA = seedPlanWithTask(&suite, "ag-f2-A", "task-A");
    defer gpa.free(pidA);
    const pidB = seedAdditionalPlanWithTask(&suite, "ag-f2-B", "task-B");
    defer gpa.free(pidB);

    // Session A pulls plan A with TTL=1s. `--vendor-session` distinguishes
    // the operating session so `session_mod.ensureActive` materialises a
    // dedicated session row for each driver.
    const pullA = mustRunAgent(&suite, &.{
        "pull",                pidA,
        "--no-locality-probe", "--ttl",
        "1",                   "--vendor",
        "driver-A",            "--vendor-session",
        "A:1",                 "--json",
    });
    defer gpa.free(pullA);
    const tokenA = extractStringField(gpa, pullA, "\"claim_token\":\"") catch @panic("no token A");
    defer gpa.free(tokenA);
    const sessionA = extractIntField(pullA, "\"session_id\"") orelse @panic("no session_id A");

    // Session B pulls plan B with TTL=1s.
    const pullB = mustRunAgent(&suite, &.{
        "pull",                pidB,
        "--no-locality-probe", "--ttl",
        "1",                   "--vendor",
        "driver-B",            "--vendor-session",
        "B:1",                 "--json",
    });
    defer gpa.free(pullB);
    const tokenB = extractStringField(gpa, pullB, "\"claim_token\":\"") catch @panic("no token B");
    defer gpa.free(tokenB);
    const sessionB = extractIntField(pullB, "\"session_id\"") orelse @panic("no session_id B");

    // Sanity: the two pulls landed on distinct sessions. If not, the test
    // can't differentiate A from B and the rest is meaningless.
    if (sessionA == sessionB) {
        std.debug.print(
            "S4 setup-check: --vendor-session did not split sessions (A={d}, B={d})\n",
            .{ sessionA, sessionB },
        );
        return error.SessionsDidNotSeparate;
    }

    // Let both leases lapse so each claim is expired-but-still-`active`.
    sleepSecs(2);

    // CONTRACT pre-fix: this call FAILS — `--session` is not a recognised
    // flag on `planar-agent reconcile`. The harness's `mustRunAgent` panics
    // on non-zero exit. Post-F2 it succeeds and reports 1 claim marked stale.
    const sessionA_arg = std.fmt.allocPrint(gpa, "{d}", .{sessionA}) catch @panic("OOM");
    defer gpa.free(sessionA_arg);
    const scoped = mustRunAgent(&suite, &.{
        "reconcile", "--session", sessionA_arg, "--stale-after", "0", "--json",
    });
    defer gpa.free(scoped);

    // Post-fix expectations: A's claim was marked stale; B's claim was not.
    try std.testing.expect(std.mem.indexOf(u8, scoped, "\"claims_marked_stale\":1") != null);

    // CONTRACT: a global dry-run candidates list lists every claim that
    // is still `active` with an expired lease. After the scoped reconcile,
    // ONLY B's token should appear — A's row is now `stale`. Pre-fix this
    // would be unreachable (the previous call would have panic'd already);
    // post-fix it pins the bit-for-bit contract that --session A leaves B
    // untouched.
    const dry_after_scoped = mustRunAgent(&suite, &.{
        "reconcile", "--dry-run", "--stale-after", "0", "--json",
    });
    defer gpa.free(dry_after_scoped);
    if (std.mem.indexOf(u8, dry_after_scoped, tokenA) != null) {
        std.debug.print(
            "S4 RED: scoped reconcile did not retire A's claim, or the global dry-run still sees it active-expired.\ndry_after_scoped: {s}\n",
            .{dry_after_scoped},
        );
        return error.ScopedReconcileFailedToFlipA;
    }
    if (std.mem.indexOf(u8, dry_after_scoped, tokenB) == null) {
        std.debug.print(
            "S4 RED: scoped reconcile flipped B's claim (or it never landed). tokenB={s}\ndry_after_scoped: {s}\n",
            .{ tokenB, dry_after_scoped },
        );
        return error.ScopedReconcileTouchedB;
    }

    // CONTRACT: bare `reconcile` (no --session) preserves today's
    // behaviour — it sweeps every remaining expired-active claim,
    // which here is B's. After this run, B's token must no longer
    // appear as a candidate.
    const bare = mustRunAgent(&suite, &.{
        "reconcile", "--stale-after", "0", "--json",
    });
    defer gpa.free(bare);
    try std.testing.expect(std.mem.indexOf(u8, bare, "\"claims_marked_stale\":1") != null);

    const dry_after_bare = mustRunAgent(&suite, &.{
        "reconcile", "--dry-run", "--stale-after", "0", "--json",
    });
    defer gpa.free(dry_after_bare);
    try std.testing.expect(std.mem.indexOf(u8, dry_after_bare, "\"claims_marked_stale\":0") != null);
    if (std.mem.indexOf(u8, dry_after_bare, tokenB) != null) {
        std.debug.print(
            "S4 RED: bare reconcile did not sweep B; dry_after_bare: {s}\n",
            .{dry_after_bare},
        );
        return error.BareReconcileDidNotSweepB;
    }
}

// =========================================================================
// JSON helpers (mirrored from planar_agent_test.zig / liveness_repull_test.zig).
// =========================================================================

fn extractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

fn extractStringField(gpa: std.mem.Allocator, json: []const u8, prefix: []const u8) ![]u8 {
    const idx = std.mem.indexOf(u8, json, prefix) orelse return error.FieldNotFound;
    const i = idx + prefix.len;
    var end = i;
    while (end < json.len and json[end] != '"') end += 1;
    if (end == json.len) return error.UnterminatedString;
    return try gpa.dupe(u8, json[i..end]);
}
