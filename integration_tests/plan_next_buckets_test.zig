//! integration_tests/plan_next_buckets_test.zig — `planar plan next <plan>`
//!
//! Black-box coverage for the bucketed `plan next` view shipped in
//! plan 85 M3. Asserts the JSON shape from tech-spec § "JSON shapes"
//! → `planar plan next --json`:
//!   { plan_id, available, claimed, stale, blocked, summary }
//!
//! Slugs covered:
//!   - plan-next-selector (t#2555)

const std = @import("std");
const harness = @import("harness");

// Subset shapes — `ignore_unknown_fields = true` in `mustRunJSON` means
// we only have to model the fields we assert on.

const TaskJSON = struct { id: i64, title: []const u8, status: []const u8 };

const ClaimJSON = struct {
    id: i64,
    claim_token: []const u8,
    status: []const u8,
    entity_kind: []const u8,
    entity_id: i64,
};

const ClaimedEntry = struct { task: TaskJSON, claim: ClaimJSON };

const Summary = struct {
    available: i64,
    claimed: i64,
    stale: i64,
    blocked: i64,
    done: i64,
};

const PlanNextJSON = struct {
    plan_id: i64,
    available: []TaskJSON,
    claimed: []ClaimedEntry,
    stale: []ClaimedEntry,
    blocked: []TaskJSON,
    summary: Summary,
};

const PlanJSON = struct { id: i64 };
const TaskAddJSON = struct { id: i64 };

/// Resolve the planar-agent binary path for the cross-binary fixture
/// seeding (acquire a claim so the `claimed` bucket has rows). Mirrors
/// the helper in `planar_agent_test.zig`.
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

test "plan next --json on plan with one todo task: available[1], claimed[0], stale[0], blocked[0]" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pn-avail", "--json", "PN_AVAIL",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    _ = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--priority", "50", "--json", "first-task",
    });

    const next = suite.mustRunJSON(PlanNextJSON, arena, &.{ "plan", "next", pid, "--json" });
    try std.testing.expectEqual(plan.id, next.plan_id);
    try std.testing.expectEqual(@as(usize, 1), next.available.len);
    try std.testing.expectEqual(@as(usize, 0), next.claimed.len);
    try std.testing.expectEqual(@as(usize, 0), next.stale.len);
    try std.testing.expectEqual(@as(usize, 0), next.blocked.len);
    try std.testing.expectEqual(@as(i64, 1), next.summary.available);
    try std.testing.expectEqual(@as(i64, 0), next.summary.claimed);
    try std.testing.expectEqualStrings("first-task", next.available[0].title);
    try std.testing.expectEqualStrings("todo", next.available[0].status);
}

test "plan next --json buckets: available + claimed + blocked" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pn-mixed", "--json", "PN_MIXED",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Three tasks: one stays available, one will be claimed (via
    // planar-agent pull), one is moved to blocked status.
    _ = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--priority", "30", "--json", "available-task",
    });
    const claim_task = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--priority", "20", "--json", "to-be-claimed",
    });
    const block_task = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--priority", "40", "--json", "to-be-blocked",
    });
    _ = claim_task;

    // Block one task directly via `task update`.
    const block_id = std.fmt.allocPrint(arena, "{d}", .{block_task.id}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "task", "update", block_id, "--status", "blocked" }));

    // Claim the highest-priority remaining task via planar-agent pull.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    const next = suite.mustRunJSON(PlanNextJSON, arena, &.{ "plan", "next", pid, "--json" });
    try std.testing.expectEqual(@as(usize, 1), next.available.len);
    try std.testing.expectEqual(@as(usize, 1), next.claimed.len);
    try std.testing.expectEqual(@as(usize, 1), next.blocked.len);
    try std.testing.expectEqual(@as(usize, 0), next.stale.len);
    try std.testing.expectEqual(@as(i64, 1), next.summary.available);
    try std.testing.expectEqual(@as(i64, 1), next.summary.claimed);
    try std.testing.expectEqual(@as(i64, 1), next.summary.blocked);

    // The claimed bucket carries both the task AND the claim row, with
    // canonical ClaimRow keys (including the locality columns).
    const claimed = next.claimed[0];
    try std.testing.expectEqualStrings("to-be-claimed", claimed.task.title);
    try std.testing.expectEqualStrings("doing", claimed.task.status);
    try std.testing.expectEqualStrings("active", claimed.claim.status);
    try std.testing.expectEqualStrings("task", claimed.claim.entity_kind);
    try std.testing.expectEqual(claimed.task.id, claimed.claim.entity_id);
    // The text path defaults to NOT showing the claimed bucket; the
    // separate --include-claimed test below asserts the toggle.
}

test "plan next text mode hides claimed by default; --include-claimed surfaces it" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pn-text", "--json", "PN_TEXT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    _ = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--priority", "20", "--json", "CLAIMED_TASK_TITLE",
    });

    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    // Default text mode: claimed bucket is NOT in the per-row output,
    // but the summary header still shows claimed:1.
    const default_text = suite.mustRun(&.{ "plan", "next", pid });
    defer gpa.free(default_text);
    try std.testing.expect(std.mem.indexOf(u8, default_text, "claimed:1") != null);
    try std.testing.expect(std.mem.indexOf(u8, default_text, "CLAIMED_TASK_TITLE") == null);

    // --include-claimed: claimed task surfaces in the per-row text.
    const inc_text = suite.mustRun(&.{ "plan", "next", pid, "--include-claimed" });
    defer gpa.free(inc_text);
    try std.testing.expect(std.mem.indexOf(u8, inc_text, "claimed    task:") != null);
    try std.testing.expect(std.mem.indexOf(u8, inc_text, "CLAIMED_TASK_TITLE") != null);
}

test "plan next --json on missing plan exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const stderr = suite.expectFailure(&.{ "plan", "next", "9999", "--json" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "9999") != null);
}
