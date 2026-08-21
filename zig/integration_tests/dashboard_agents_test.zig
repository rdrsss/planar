//! integration_tests/dashboard_agents_test.zig — `planar dashboard --agents`
//!
//! Black-box coverage for the `--agents` fold-in shipped in plan 85
//! M3. Asserts the JSON shape from tech-spec § "JSON shapes" →
//! `planar dashboard --json`:
//!   { active_plans: [Plan],
//!     claims: { active: [ClaimRow], stale: [ClaimRow] },
//!     next_available_by_plan: { "<plan_id>": [Task] } }
//!
//! Slugs covered:
//!   - dashboard-live-claims (t#2556)

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64, title: []const u8, status: []const u8 };
const TaskJSON = struct { id: i64, title: []const u8, status: []const u8 };

const ClaimJSON = struct {
    id: i64,
    claim_token: []const u8,
    status: []const u8,
    entity_kind: []const u8,
    entity_id: i64,
    vendor: []const u8,
    // Locality columns (load-bearing — the M3 brief calls this out).
    repo_root: ?[]const u8 = null,
    branch: ?[]const u8 = null,
    head_sha_at_claim: ?[]const u8 = null,
    dirty_at_claim: ?[]const u8 = null,
    worktree_id: ?i64 = null,
    worktree_path: ?[]const u8 = null,
};

const ClaimsBlock = struct {
    active: []ClaimJSON,
    stale: []ClaimJSON,
};

const DashboardJSON = struct {
    active_plans: []PlanJSON,
    // claims + next_available_by_plan only present when --agents.
    claims: ?ClaimsBlock = null,
};

const PlainDashboardJSON = struct {
    active_plans: []PlanJSON,
};

const PlanIdJSON = struct { id: i64 };
const TaskAddJSON = struct { id: i64 };

// Cross-binary helper to invoke planar-agent (for claim acquisition).
fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_AGENT_BIN=")) {
            return s["PLANAR_AGENT_BIN=".len..];
        }
    }
    @panic("PLANAR_AGENT_BIN not set; run via `make test-integration`");
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
    var env_map = environ.createMap(gpa) catch @panic("OOM env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM PLANAR_DB");

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
        std.debug.print(
            "planar-agent failed (term={any}): {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

// =========================================================================
// Tests
// =========================================================================

test "dashboard --json without --agents: active_plans only, no claims block" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    _ = suite.mustRunJSON(PlanIdJSON, arena, &.{
        "plan", "create", "--slug", "d-plain", "--json", "D_PLAIN",
    });

    const dash = suite.mustRunJSON(PlainDashboardJSON, arena, &.{ "dashboard", "--json" });
    try std.testing.expect(dash.active_plans.len >= 1);

    // Without --agents, the raw stdout must NOT contain the claims block
    // keys — proving that the agents fold-in is opt-in.
    const raw = suite.mustRun(&.{ "dashboard", "--json" });
    defer gpa.free(raw);
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"claims\"") == null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"next_available_by_plan\"") == null);
}

test "dashboard --agents --json: claims block + next_available_by_plan keys present" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanIdJSON, arena, &.{
        "plan", "create", "--slug", "d-agents", "--json", "D_AGENTS",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "available-one",
    });
    _ = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "to-claim",
    });

    // Seed a live claim via planar-agent pull.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    const raw = suite.mustRun(&.{ "dashboard", "--agents", "--json" });
    defer gpa.free(raw);

    try std.testing.expect(std.mem.indexOf(u8, raw, "\"active_plans\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"claims\":{\"active\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"stale\":[") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"next_available_by_plan\"") != null);

    // Per-plan next-available list: the plan id appears as a JSON
    // object key (stringified int) in the next_available_by_plan map.
    const pid_key = std.fmt.allocPrint(arena, "\"{d}\":[", .{plan.id}) catch unreachable;
    try std.testing.expect(std.mem.indexOf(u8, raw, pid_key) != null);

    // Decode a subset of the shape to assert the active claim has
    // the expected entity/status keys plus the locality fields.
    const dash = suite.mustRunJSON(DashboardJSON, arena, &.{ "dashboard", "--agents", "--json" });
    try std.testing.expect(dash.claims != null);
    const claims = dash.claims.?;
    try std.testing.expectEqual(@as(usize, 1), claims.active.len);
    const c = claims.active[0];
    try std.testing.expectEqualStrings("active", c.status);
    try std.testing.expectEqualStrings("task", c.entity_kind);
    // Locality columns present as keys even when --no-locality-probe
    // was used (values null in that case). The locality columns are
    // load-bearing per the M3 brief.
    try std.testing.expect(c.repo_root == null);
    try std.testing.expect(c.branch == null);
    try std.testing.expect(c.head_sha_at_claim == null);
    try std.testing.expect(c.dirty_at_claim == null);
}

test "dashboard --agents text: renders branch, head_sha, dirty, repo columns from a real git fixture" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanIdJSON, arena, &.{
        "plan", "create", "--slug", "d-locality", "--json", "D_LOC",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "loc-task",
    });

    // Seed a real git checkout so the locality probe records non-null
    // values. The probe shells out to `git symbolic-ref` and `git
    // rev-parse HEAD` against the supplied --repo-root.
    const repo_dir = suite.freshSystemTmpDir();
    gitInit(gpa, repo_dir);

    gpa.free(mustRunAgent(&suite, &.{
        "pull",        pid,
        "--repo-root", repo_dir,
        "--json",
    }));

    const text = suite.mustRun(&.{ "dashboard", "--agents" });
    defer gpa.free(text);
    // The text-mode renderer surfaces branch, sha (8-char prefix),
    // dirty, repo, and the claim token. Asserting on each individually
    // pins the column set for the operator-facing view.
    try std.testing.expect(std.mem.indexOf(u8, text, "branch:test-branch") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "sha:") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "dirty:clean") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "repo:") != null);

    // Also confirm the JSON shape carries the same locality columns.
    const json_raw = suite.mustRun(&.{ "dashboard", "--agents", "--json" });
    defer gpa.free(json_raw);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"branch\":\"test-branch\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"dirty_at_claim\":\"clean\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"head_sha_at_claim\":\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"repo_root\":\"") != null);
}

/// Initialize a git checkout at `dir` with one empty commit on branch
/// `test-branch`. Used by the locality-probe assertions.
fn gitInit(gpa: std.mem.Allocator, dir: []const u8) void {
    runGit(gpa, dir, &.{ "init", "-q", "-b", "test-branch", "." });
    runGit(gpa, dir, &.{ "config", "user.email", "test@example.com" });
    runGit(gpa, dir, &.{ "config", "user.name", "Test" });
    runGit(gpa, dir, &.{ "commit", "-q", "--allow-empty", "-m", "seed" });
}

fn runGit(gpa: std.mem.Allocator, dir: []const u8, args: []const []const u8) void {
    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(gpa);
    argv.append(gpa, "git") catch @panic("OOM");
    argv.append(gpa, "-C") catch @panic("OOM");
    argv.append(gpa, dir) catch @panic("OOM");
    for (args) |a| argv.append(gpa, a) catch @panic("OOM");
    const res = std.process.run(gpa, std.testing.io, .{ .argv = argv.items }) catch |e|
        std.debug.panic("git spawn failed: {s}", .{@errorName(e)});
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("git {s} failed: {s}\n", .{ args[0], res.stderr });
        @panic("git command failed");
    }
}
