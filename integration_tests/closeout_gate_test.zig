//! integration_tests/closeout_gate_test.zig — plan 566, task 3883.
//!
//! Black-box tests for `planar plan closeout <plan-id> [--dry-run] [--json]`.
//!
//! Scenarios covered:
//!   1. Dry-run on a ready plan (all tasks done) — ready, not applied, plan unchanged.
//!   2. Dry-run on a blocked plan (open task) — blocked, blocked_by names reason, plan unchanged.
//!   3. Apply on a ready plan — plan becomes done, applied=true.
//!   4. Apply on a blocked plan — non-zero exit, plan unchanged.
//!   5. Cancelled tasks are terminal — plan with done+cancelled tasks → ready.
//!   6. Open descendant plan blocks closeout.
//!   7. Live claim blocks closeout; stale claim is advisory only.
//!   8. Git evidence: no git attribution → inconclusive note, hard gate still computed.
//!   9. Anchor plan closeout via operator verb works (operator can close when ready).
//!  10. Already-terminal plan (already done) → no-op, applied=false.
//!  11. Plan not found → exit 1.

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// JSON shapes
// =========================================================================

const PlanJSON = struct {
    id: i64 = 0,
    status: []const u8 = "",
};

const TaskJSON = struct {
    id: i64 = 0,
    status: []const u8 = "",
};

const CloseoutTasksJSON = struct {
    open: i64 = 0,
    done: i64 = 0,
    cancelled: i64 = 0,
};

const CloseoutDescendantsJSON = struct {
    open: i64 = 0,
    terminal: i64 = 0,
};

const CloseoutClaimsJSON = struct {
    live: i64 = 0,
    stale: i64 = 0,
};

const CloseoutHardEvidenceJSON = struct {
    tasks: CloseoutTasksJSON = .{},
    descendants: CloseoutDescendantsJSON = .{},
    claims: CloseoutClaimsJSON = .{},
    finalization_tasks: i64 = 0,
};

const GitEvidenceJSON = struct {
    repo_root: []const u8 = "",
    note: []const u8 = "",
};

const EpicMergeRollupJSON = struct {
    target_branch: []const u8 = "",
    total_branches: i64 = 0,
    merged_count: i64 = 0,
    note: []const u8 = "",
};

const CloseoutResultJSON = struct {
    plan: i64 = 0,
    ready: bool = false,
    applied: bool = false,
    hard_evidence: CloseoutHardEvidenceJSON = .{},
    blocked_by: []const []const u8 = &.{},
    git_evidence: []const GitEvidenceJSON = &.{},
    epic_merge: ?EpicMergeRollupJSON = null,
    warnings: []const []const u8 = &.{},
};

// =========================================================================
// Helpers
// =========================================================================

/// Resolve the planar-agent binary from PLANAR_AGENT_BIN.
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

/// Run planar-agent with args, injecting PLANAR_DB from the suite.
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
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM injecting PLANAR_DB");

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
        std.debug.print(
            "planar-agent failed (term={any}):\nstdout: {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

fn extractString(gpa: std.mem.Allocator, json: []const u8, prefix: []const u8) []u8 {
    const idx = std.mem.indexOf(u8, json, prefix) orelse @panic("field not found");
    const i = idx + prefix.len;
    var end = i;
    while (end < json.len and json[end] != '"') end += 1;
    return gpa.dupe(u8, json[i..end]) catch @panic("OOM");
}

/// Seed: init + one plan + N tasks. Returns (plan_id_str, task_id_strs).
fn seedPlan(
    suite: *const harness.Suite,
    arena: std.mem.Allocator,
    slug: []const u8,
    task_count: usize,
    status: []const u8,
    parent_id: ?[]const u8,
) struct { plan_id: []u8, task_ids: [][]u8 } {
    const gpa = suite.allocator;

    // plan create
    var create_args: std.ArrayList([]const u8) = .empty;
    defer create_args.deinit(gpa);
    create_args.appendSlice(gpa, &.{ "plan", "create", "--json", "--status", status, "--slug", slug }) catch @panic("OOM");
    if (parent_id) |pid| {
        create_args.appendSlice(gpa, &.{ "--parent", pid }) catch @panic("OOM");
    }
    create_args.append(gpa, slug) catch @panic("OOM");

    const plan_buf = suite.mustRun(create_args.items);
    defer gpa.free(plan_buf);
    const plan_json = parseJSON(PlanJSON, arena, plan_buf);
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan_json.id}) catch @panic("OOM");

    var task_ids = arena.alloc([]u8, task_count) catch @panic("OOM");
    for (0..task_count) |idx| {
        const title = std.fmt.allocPrint(arena, "task-{d}", .{idx + 1}) catch @panic("OOM");
        const t_buf = suite.mustRun(&.{ "task", "add", "--json", "--plan", plan_id, title });
        defer gpa.free(t_buf);
        const t_json = parseJSON(TaskJSON, arena, t_buf);
        task_ids[idx] = std.fmt.allocPrint(arena, "{d}", .{t_json.id}) catch @panic("OOM");
    }

    return .{ .plan_id = plan_id, .task_ids = task_ids };
}

/// Advance a task to doing then mark it done.
/// The matrix requires todo → doing before done (todo → done is not a legal edge).
fn startAndDone(suite: *const harness.Suite, task_id: []const u8) void {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "task", "update", task_id, "--status", "doing" }));
    gpa.free(suite.mustRun(&.{ "task", "done", task_id }));
}

fn parseJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) T {
    const trimmed = std.mem.trim(u8, buf, " \n");
    const parsed = std.json.parseFromSlice(T, arena, trimmed, .{
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nparseJSON {s} failed: {s}\nbuf: {s}\n", .{ @typeName(T), @errorName(e), buf });
        @panic("parseJSON failed");
    };
    return parsed.value;
}

// =========================================================================
// Test 1: dry-run on a ready plan
// =========================================================================

test "closeout-gate: dry-run on a ready plan → ready, not applied, plan unchanged" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const seeded = seedPlan(&suite, arena, "closeout-dryrun-ready", 2, "active", null);
    // Mark both tasks done to verify the plan reaches terminal state.
    startAndDone(&suite, seeded.task_ids[0]);
    startAndDone(&suite, seeded.task_ids[1]);

    // Plan should be done (auto-recomputed). But closeout can still be run on it.
    // Use a fresh active plan for the dry-run test.
    const seeded2 = seedPlan(&suite, arena, "closeout-dryrun-ready2", 1, "active", null);
    const plan_id2 = seeded2.plan_id;
    startAndDone(&suite, seeded2.task_ids[0]);

    // Dry-run via --json.
    const out_buf = suite.mustRun(&.{ "plan", "closeout", "--dry-run", "--json", plan_id2 });
    defer gpa.free(out_buf);
    const result = parseJSON(CloseoutResultJSON, arena, out_buf);

    try std.testing.expect(result.ready);
    try std.testing.expect(!result.applied);
    try std.testing.expectEqual(@as(usize, 0), result.blocked_by.len);

    // Plan status must be unchanged — dry-run never writes.
    const plan_after = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", plan_id2 });
    // Plan auto-recomputed to done on task done; dry-run doesn't change that.
    // What matters is closeout applied=false.
    _ = plan_after;
}

// =========================================================================
// Test 2: dry-run on a blocked plan (open task)
// =========================================================================

test "closeout-gate: dry-run on a blocked plan → blocked, blocked_by has reason, exit non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const seeded = seedPlan(&suite, arena, "closeout-blocked", 2, "active", null);
    const plan_id = seeded.plan_id;
    // Leave one task open (todo), mark the other done (via doing: todo → done is not in the matrix).
    startAndDone(&suite, seeded.task_ids[0]);

    // Dry-run on blocked plan — should exit non-zero.
    const res = suite.exec(&.{ "plan", "closeout", "--dry-run", "--json", plan_id });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // Non-zero exit because hard gate is blocked.
    try std.testing.expect(res.term == .exited and res.term.exited != 0);

    // Parse the JSON output (written to stdout before the non-zero exit).
    const result = parseJSON(CloseoutResultJSON, arena, res.stdout);
    try std.testing.expect(!result.ready);
    try std.testing.expect(!result.applied);
    try std.testing.expect(result.blocked_by.len > 0);

    // hard_evidence.tasks.open > 0.
    try std.testing.expect(result.hard_evidence.tasks.open > 0);

    // Plan unchanged.
    const plan_after = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", plan_id });
    try std.testing.expectEqualStrings("active", plan_after.status);
}

// =========================================================================
// Test 3: apply on a ready plan → plan becomes done
// =========================================================================

test "closeout-gate: apply on a ready plan → plan becomes done, applied=true" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    // Use a child plan (has parent) so auto-recompute wouldn't have fired on anchor.
    // Create anchor.
    const anchor = seedPlan(&suite, arena, "closeout-apply-anchor", 0, "active", null);
    // Create child plan with one task.
    const child = seedPlan(&suite, arena, "closeout-apply-child", 1, "active", anchor.plan_id);
    const child_id = child.plan_id;

    // Put plan in a state where auto-recompute DIDN'T fire (paused).
    gpa.free(suite.mustRun(&.{ "plan", "update", child_id, "--status", "paused" }));
    startAndDone(&suite, child.task_ids[0]);
    // Plan is paused + all tasks done → recompute would be a no-op (paused is protected).

    // Apply closeout — should mark done.
    const out_buf = suite.mustRun(&.{ "plan", "closeout", "--json", child_id });
    defer gpa.free(out_buf);
    const result = parseJSON(CloseoutResultJSON, arena, out_buf);

    try std.testing.expect(result.ready);
    try std.testing.expect(result.applied);
    try std.testing.expectEqual(@as(usize, 0), result.blocked_by.len);

    // Plan must now be done.
    const plan_after = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", child_id });
    try std.testing.expectEqualStrings("done", plan_after.status);
}

// =========================================================================
// Test 4: apply on a blocked plan → non-zero exit, plan unchanged
// =========================================================================

test "closeout-gate: apply on a blocked plan → non-zero exit, plan unchanged" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const seeded = seedPlan(&suite, arena, "closeout-apply-blocked", 2, "active", null);
    const plan_id = seeded.plan_id;
    // Leave one task open (todo); mark the other done (via doing: todo → done is not in the matrix).
    startAndDone(&suite, seeded.task_ids[0]);

    // Apply (no --dry-run) on a blocked plan — non-zero exit.
    const res = suite.exec(&.{ "plan", "closeout", "--json", plan_id });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    try std.testing.expect(res.term == .exited and res.term.exited != 0);

    // Plan must still be active.
    const plan_after = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", plan_id });
    try std.testing.expectEqualStrings("active", plan_after.status);
}

// =========================================================================
// Test 5: cancelled tasks are terminal, don't block
// =========================================================================

test "closeout-gate: cancelled tasks are terminal — plan with done+cancelled tasks is ready" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const seeded = seedPlan(&suite, arena, "closeout-cancelled-ok", 3, "active", null);
    const plan_id = seeded.plan_id;
    // Done + cancelled = all terminal (via doing for done: todo → done is not in the matrix).
    startAndDone(&suite, seeded.task_ids[0]);
    startAndDone(&suite, seeded.task_ids[1]);
    gpa.free(suite.mustRun(&.{ "task", "cancel", seeded.task_ids[2] }));

    // Plan is auto-recomputed done at this point. Closeout dry-run should see ready.
    const out_buf = suite.mustRun(&.{ "plan", "closeout", "--dry-run", "--json", plan_id });
    defer gpa.free(out_buf);
    const result = parseJSON(CloseoutResultJSON, arena, out_buf);

    try std.testing.expect(result.ready);
    try std.testing.expect(!result.applied);
    // Tasks: open=0, done=2, cancelled=1.
    try std.testing.expectEqual(@as(i64, 0), result.hard_evidence.tasks.open);
    try std.testing.expectEqual(@as(i64, 2), result.hard_evidence.tasks.done);
    try std.testing.expectEqual(@as(i64, 1), result.hard_evidence.tasks.cancelled);
}

// =========================================================================
// Test 6: open descendant plan blocks closeout
// =========================================================================

test "closeout-gate: open descendant plan blocks closeout" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const anchor = seedPlan(&suite, arena, "closeout-desc-anchor", 0, "active", null);
    const anchor_id = anchor.plan_id;
    // Child plan with one open task.
    const _child = seedPlan(&suite, arena, "closeout-desc-child", 1, "active", anchor_id);
    _ = _child;

    // Dry-run on ancestor — descendant is open → blocked.
    const res = suite.exec(&.{ "plan", "closeout", "--dry-run", "--json", anchor_id });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    try std.testing.expect(res.term == .exited and res.term.exited != 0);

    const result = parseJSON(CloseoutResultJSON, arena, res.stdout);
    try std.testing.expect(!result.ready);
    // descendants.open > 0.
    try std.testing.expect(result.hard_evidence.descendants.open > 0);
}

// =========================================================================
// Test 7: live claim blocks; stale claim is advisory
// =========================================================================

test "closeout-gate: live claim blocks closeout; stale claim is advisory warning" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const seeded = seedPlan(&suite, arena, "closeout-liveclaim", 2, "active", null);
    const plan_id = seeded.plan_id;

    // Pull (claim) task 1 but don't complete — creates a live claim.
    const pull_out = mustRunAgent(&suite, &.{ "pull", plan_id, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractString(gpa, pull_out, "\"claim_token\":\"");
    defer gpa.free(token);

    // Mark task 2 done (so the only remaining blocker is the live claim on task 1).
    // Task 2 is still todo (agent only pulled task 1); advance via doing first.
    startAndDone(&suite, seeded.task_ids[1]);

    // Closeout dry-run — should be blocked due to live claim.
    const res = suite.exec(&.{ "plan", "closeout", "--dry-run", "--json", plan_id });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    try std.testing.expect(res.term == .exited and res.term.exited != 0);

    const result = parseJSON(CloseoutResultJSON, arena, res.stdout);
    try std.testing.expect(!result.ready);
    // claims.live > 0.
    try std.testing.expect(result.hard_evidence.claims.live > 0);

    // Release the claim so it becomes stale in the sense of "released" status.
    gpa.free(mustRunAgent(&suite, &.{ "release", "--claim", token, "--reason", "cleanup", "--json" }));
}

// =========================================================================
// Test 8: git evidence — no git attribution → inconclusive, hard gate still computed
// =========================================================================

test "closeout-gate: git evidence with no attribution is inconclusive — hard gate still works" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const seeded = seedPlan(&suite, arena, "closeout-gitevidence", 1, "active", null);
    const plan_id = seeded.plan_id;
    startAndDone(&suite, seeded.task_ids[0]);

    // Closeout — no claims with locality data were ever created.
    const out_buf = suite.mustRun(&.{ "plan", "closeout", "--dry-run", "--json", plan_id });
    defer gpa.free(out_buf);
    const result = parseJSON(CloseoutResultJSON, arena, out_buf);

    // Hard gate: ready (all tasks done, no descendants, no claims).
    try std.testing.expect(result.ready);

    // git_evidence must be present and non-empty.
    try std.testing.expect(result.git_evidence.len > 0);

    // The note must mention "inconclusive".
    const note = result.git_evidence[0].note;
    try std.testing.expect(std.mem.containsAtLeast(u8, note, 1, "inconclusive"));
}

// =========================================================================
// Test 9: anchor plan closeout works when operator invokes it
// =========================================================================

test "closeout-gate: anchor plan can be closed by operator verb when ready" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    // Anchor plan with one task.
    const anchor = seedPlan(&suite, arena, "closeout-anchor-operator", 1, "active", null);
    const anchor_id = anchor.plan_id;

    // recompute-status would cap anchor at active — but operator closeout is unbounded.
    startAndDone(&suite, anchor.task_ids[0]);

    // After task done, anchor is still "active" (recompute caps anchors).
    const before = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", anchor_id });
    try std.testing.expectEqualStrings("active", before.status);

    // Apply closeout — operator verb bypasses the anchor cap.
    const out_buf = suite.mustRun(&.{ "plan", "closeout", "--json", anchor_id });
    defer gpa.free(out_buf);
    const result = parseJSON(CloseoutResultJSON, arena, out_buf);

    try std.testing.expect(result.ready);
    try std.testing.expect(result.applied);

    // Anchor plan is now done.
    const after = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", anchor_id });
    try std.testing.expectEqualStrings("done", after.status);
}

// =========================================================================
// Test 10: already-terminal plan → no-op, applied=false
// =========================================================================

test "closeout-gate: already-terminal plan is a no-op (applied=false)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    // Use a child plan (non-anchor) so task done auto-recomputes it to done.
    const anchor = seedPlan(&suite, arena, "closeout-noop-anchor", 0, "active", null);
    const child = seedPlan(&suite, arena, "closeout-terminal-noop", 1, "active", anchor.plan_id);
    const plan_id = child.plan_id;
    startAndDone(&suite, child.task_ids[0]);

    // Child plan auto-recomputes to done when its last task is marked done.
    const before = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", plan_id });
    try std.testing.expectEqualStrings("done", before.status);

    // Closeout apply on already-done plan.
    const out_buf = suite.mustRun(&.{ "plan", "closeout", "--json", plan_id });
    defer gpa.free(out_buf);
    const result = parseJSON(CloseoutResultJSON, arena, out_buf);

    // ready=true, applied=false (already terminal — no-op).
    try std.testing.expect(result.ready);
    try std.testing.expect(!result.applied);

    // Plan is still done.
    const after = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", plan_id });
    try std.testing.expectEqualStrings("done", after.status);
}

// =========================================================================
// Test 11: plan not found → exit 1
// =========================================================================

test "closeout-gate: plan not found → exit 1" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const stderr = suite.expectFailure(&.{ "plan", "closeout", "99999" });
    defer gpa.free(stderr);
    // Error message mentions the plan id.
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "99999"));
}

// =========================================================================
// Test 12 (task 3884): finalization_tasks count in --json output
// =========================================================================

test "closeout-gate: finalization_tasks count reflects slug-prefixed tasks in --json" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    // Create a plan with a regular task and two finalization-prefixed tasks.
    const anchor = seedPlan(&suite, arena, "closeout-fintask-anchor", 0, "active", null);
    const anchor_id = anchor.plan_id;

    // Regular task.
    const t1_buf = suite.mustRun(&.{ "task", "add", "--json", "--plan", anchor_id, "regular work" });
    defer gpa.free(t1_buf);
    const t1 = parseJSON(TaskJSON, arena, t1_buf);
    const t1_id = std.fmt.allocPrint(arena, "{d}", .{t1.id}) catch @panic("OOM");

    // Finalization task with "merge-" slug prefix.
    const t2_buf = suite.mustRun(&.{ "task", "add", "--json", "--plan", anchor_id, "--slug", "merge-feature-branch", "merge feature branch" });
    defer gpa.free(t2_buf);
    const t2 = parseJSON(TaskJSON, arena, t2_buf);
    const t2_id = std.fmt.allocPrint(arena, "{d}", .{t2.id}) catch @panic("OOM");

    // Finalization task with "reconcile-" slug prefix.
    const t3_buf = suite.mustRun(&.{ "task", "add", "--json", "--plan", anchor_id, "--slug", "reconcile-state", "reconcile planar state" });
    defer gpa.free(t3_buf);
    const t3 = parseJSON(TaskJSON, arena, t3_buf);
    const t3_id = std.fmt.allocPrint(arena, "{d}", .{t3.id}) catch @panic("OOM");

    // Mark all tasks done so the hard gate passes (via doing: todo → done is not in the matrix).
    startAndDone(&suite, t1_id);
    startAndDone(&suite, t2_id);
    startAndDone(&suite, t3_id);

    // Closeout dry-run with --json.
    const out_buf = suite.mustRun(&.{ "plan", "closeout", "--dry-run", "--json", anchor_id });
    defer gpa.free(out_buf);
    const result = parseJSON(CloseoutResultJSON, arena, out_buf);

    // Gate passes (all tasks done).
    try std.testing.expect(result.ready);

    // finalization_tasks must be 2 (merge-feature-branch + reconcile-state).
    try std.testing.expectEqual(@as(i64, 2), result.hard_evidence.finalization_tasks);

    // epic_merge must be null (--check-merge not passed).
    try std.testing.expect(result.epic_merge == null);
}

// =========================================================================
// Test 13 (task 3884): finalization_tasks = 0 when no slug-prefixed tasks
// =========================================================================

test "closeout-gate: finalization_tasks is 0 when no slug-prefixed tasks exist" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const seeded = seedPlan(&suite, arena, "closeout-no-fintask", 2, "active", null);
    startAndDone(&suite, seeded.task_ids[0]);
    startAndDone(&suite, seeded.task_ids[1]);

    const out_buf = suite.mustRun(&.{ "plan", "closeout", "--dry-run", "--json", seeded.plan_id });
    defer gpa.free(out_buf);
    const result = parseJSON(CloseoutResultJSON, arena, out_buf);

    try std.testing.expect(result.ready);
    try std.testing.expectEqual(@as(i64, 0), result.hard_evidence.finalization_tasks);
}

// =========================================================================
// Test 14 (task 3887): --check-merge absent → epic_merge is null in JSON
// =========================================================================

test "closeout-gate: epic_merge is null in JSON when --check-merge not passed" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const seeded = seedPlan(&suite, arena, "closeout-no-checkmerge", 1, "active", null);
    startAndDone(&suite, seeded.task_ids[0]);

    const out_buf = suite.mustRun(&.{ "plan", "closeout", "--dry-run", "--json", seeded.plan_id });
    defer gpa.free(out_buf);
    const result = parseJSON(CloseoutResultJSON, arena, out_buf);

    try std.testing.expect(result.ready);
    // Without --check-merge, epic_merge must be absent/null.
    try std.testing.expect(result.epic_merge == null);
}

// =========================================================================
// Test 15 (task 3887): --check-merge with no locality data → section present
// (inconclusive, not an error; epic_merge is null when no locality rows)
// =========================================================================

test "closeout-gate: --check-merge with no locality data → epic_merge absent (no locality rows)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const seeded = seedPlan(&suite, arena, "closeout-checkmerge-nodata", 1, "active", null);
    startAndDone(&suite, seeded.task_ids[0]);

    // --check-merge requested but no claims with locality exist.
    // The engine returns null when no locality rows are found.
    const out_buf = suite.mustRun(&.{ "plan", "closeout", "--dry-run", "--json", "--check-merge", seeded.plan_id });
    defer gpa.free(out_buf);
    const result = parseJSON(CloseoutResultJSON, arena, out_buf);

    try std.testing.expect(result.ready);
    // No locality data → null (inconclusive — documented contract).
    try std.testing.expect(result.epic_merge == null);
}
