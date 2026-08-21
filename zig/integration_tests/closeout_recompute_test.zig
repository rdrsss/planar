//! integration_tests/closeout_recompute_test.zig — plan 566 task 3882.
//!
//! Verifies that agent terminal verbs (complete/fail/release/block) recompute
//! the plan's roll-up status to match the behavior of operator task verbs
//! (`task done`, `task cancel`, `task block`). This closes the parity gap
//! where `planar-agent complete` on the last task of a plan left the plan's
//! roll-up status stale.
//!
//! Scenarios:
//!   1. Agent complete — all tasks done via planar-agent → plan flips to done.
//!   2. Agent mid-plan complete — partial completion updates plan consistently.
//!   3. Operator parity baseline — same plan shape via `task done` flips to done.
//!   4. Agent fail — fail sends task back to todo; plan stays active (not done).
//!   5. Agent release — release sends task back to todo; plan stays active.
//!   6. Agent block — block marks task blocked; plan stays active.

const std = @import("std");
const harness = @import("harness");

// ---- JSON shapes ------------------------------------------------------------

const PlanJSON = struct {
    id: i64 = 0,
    status: []const u8 = "",
};

const TaskJSON = struct {
    id: i64 = 0,
    status: []const u8 = "",
};

/// Parse a single JSON object from a buffer. The buffer must remain alive as
/// long as the returned struct's string fields are used (alloc_if_needed
/// aliases into the input buffer).
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

// ---- Agent helpers ----------------------------------------------------------

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

/// Assert exit 0 and return stdout. Caller owns the returned buffer.
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

/// Extract a quoted JSON string field from a JSON blob. Prefix must include
/// the opening `"` of the value. Returns a heap-allocated copy; caller frees.
fn extractString(gpa: std.mem.Allocator, json: []const u8, prefix: []const u8) []u8 {
    const idx = std.mem.indexOf(u8, json, prefix) orelse @panic("field not found");
    const i = idx + prefix.len;
    var end = i;
    while (end < json.len and json[end] != '"') end += 1;
    return gpa.dupe(u8, json[i..end]) catch @panic("OOM");
}

/// Advance a task to doing then mark it done.
/// The matrix requires todo → doing before done (todo → done is not a legal edge).
fn startAndDone(suite: *const harness.Suite, task_id: []const u8) void {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "task", "update", task_id, "--status", "doing" }));
    gpa.free(suite.mustRun(&.{ "task", "done", task_id }));
}

// ---- Seed helpers -----------------------------------------------------------

/// Seed an init + anchor plan + child milestone plan + N todo tasks on the
/// child. Returns (anchor_id_str, child_id_str) as arena-allocated strings.
fn seedAnchorChildPlan(
    suite: *const harness.Suite,
    arena: std.mem.Allocator,
    child_slug: []const u8,
    task_count: usize,
) struct { anchor_id: []u8, child_id: []u8, task_ids: [][]u8 } {
    const gpa = suite.allocator;

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    // Anchor plan (parent_plan_id IS NULL) — cannot auto-advance to done.
    const anchor_json = suite.mustRun(&.{ "plan", "create", "--json", "--status", "active", "anchor-for-closeout" });
    defer gpa.free(anchor_json);
    const anchor_id_raw = std.fmt.allocPrint(arena, "{d}", .{parseJSON(PlanJSON, arena, anchor_json).id}) catch @panic("OOM");

    // Child milestone plan — has parent_plan_id so it CAN auto-advance to done.
    const child_json_buf = suite.mustRun(&.{
        "plan",        "create", "--json",
        "--status",    "active", "--parent",
        anchor_id_raw, "--slug", child_slug,
        child_slug,
    });
    defer gpa.free(child_json_buf);
    const child_id_raw = std.fmt.allocPrint(arena, "{d}", .{parseJSON(PlanJSON, arena, child_json_buf).id}) catch @panic("OOM");

    // Add tasks.
    var task_ids = arena.alloc([]u8, task_count) catch @panic("OOM");
    for (0..task_count) |idx| {
        const title = std.fmt.allocPrint(arena, "task-{d}", .{idx + 1}) catch @panic("OOM");
        const t_json = suite.mustRun(&.{ "task", "add", "--json", "--plan", child_id_raw, title });
        defer gpa.free(t_json);
        const t_id = parseJSON(TaskJSON, arena, t_json).id;
        task_ids[idx] = std.fmt.allocPrint(arena, "{d}", .{t_id}) catch @panic("OOM");
    }

    return .{
        .anchor_id = anchor_id_raw,
        .child_id = child_id_raw,
        .task_ids = task_ids,
    };
}

// ============================================================================
// Test 1: agent complete path flips plan to done when all tasks are terminal
// ============================================================================

test "closeout-recompute: agent complete on all tasks flips child plan to done" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const seeded = seedAnchorChildPlan(&suite, arena, "ag-closeout-all", 2);
    const child_id = seeded.child_id;

    // Confirm plan starts active.
    {
        const before_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id });
        defer gpa.free(before_buf);
        const before = parseJSON(PlanJSON, arena, before_buf);
        try std.testing.expectEqualStrings("active", before.status);
    }

    // Pull + complete task 1. Plan should stay active (one task still todo).
    const pull1 = mustRunAgent(&suite, &.{ "pull", child_id, "--no-locality-probe", "--json" });
    defer gpa.free(pull1);
    const token1 = extractString(gpa, pull1, "\"claim_token\":\"");
    defer gpa.free(token1);

    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token1, "--summary", "done1", "--json" }));

    {
        const mid_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id });
        defer gpa.free(mid_buf);
        const mid = parseJSON(PlanJSON, arena, mid_buf);
        // One task still todo — plan must NOT be done yet.
        try std.testing.expect(!std.mem.eql(u8, "done", mid.status));
    }

    // Pull + complete task 2 (last). Plan should NOW flip to done.
    const pull2 = mustRunAgent(&suite, &.{ "pull", child_id, "--no-locality-probe", "--json" });
    defer gpa.free(pull2);
    const token2 = extractString(gpa, pull2, "\"claim_token\":\"");
    defer gpa.free(token2);

    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token2, "--summary", "done2", "--json" }));

    {
        const after_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id });
        defer gpa.free(after_buf);
        const after = parseJSON(PlanJSON, arena, after_buf);
        try std.testing.expectEqualStrings("done", after.status);
    }
}

// ============================================================================
// Test 2: operator parity baseline — same shape, task done verb, must match
// ============================================================================

test "closeout-recompute: operator task done baseline also flips plan to done" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const seeded = seedAnchorChildPlan(&suite, arena, "op-closeout-baseline", 2);
    const child_id = seeded.child_id;
    const task_ids = seeded.task_ids;

    // Mark both tasks done via operator verb (via doing: todo → done is not in the matrix).
    startAndDone(&suite, task_ids[0]);
    startAndDone(&suite, task_ids[1]);

    const after_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id });
    defer gpa.free(after_buf);
    const after = parseJSON(PlanJSON, arena, after_buf);
    try std.testing.expectEqualStrings("done", after.status);
}

// ============================================================================
// Test 3: agent fail resets task to todo; plan stays active
// ============================================================================

test "closeout-recompute: agent fail returns task to todo; plan stays active" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const seeded = seedAnchorChildPlan(&suite, arena, "ag-closeout-fail", 1);
    const child_id = seeded.child_id;

    const pull_out = mustRunAgent(&suite, &.{ "pull", child_id, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractString(gpa, pull_out, "\"claim_token\":\"");
    defer gpa.free(token);

    gpa.free(mustRunAgent(&suite, &.{ "fail", "--claim", token, "--reason", "broke", "--json" }));

    const after_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id });
    defer gpa.free(after_buf);
    const after = parseJSON(PlanJSON, arena, after_buf);
    // fail sends task → todo; plan must NOT be done (aggregate has an open task).
    try std.testing.expect(!std.mem.eql(u8, "done", after.status));
    // Plan should remain active (has tasks, none terminal).
    try std.testing.expectEqualStrings("active", after.status);
}

// ============================================================================
// Test 4: agent release returns task to todo; plan stays active
// ============================================================================

test "closeout-recompute: agent release returns task to todo; plan stays active" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const seeded = seedAnchorChildPlan(&suite, arena, "ag-closeout-release", 1);
    const child_id = seeded.child_id;

    const pull_out = mustRunAgent(&suite, &.{ "pull", child_id, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractString(gpa, pull_out, "\"claim_token\":\"");
    defer gpa.free(token);

    gpa.free(mustRunAgent(&suite, &.{ "release", "--claim", token, "--reason", "stepping back", "--json" }));

    const after_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id });
    defer gpa.free(after_buf);
    const after = parseJSON(PlanJSON, arena, after_buf);
    try std.testing.expectEqualStrings("active", after.status);
}

// ============================================================================
// Test 5: agent block marks task blocked; plan stays active
// ============================================================================

test "closeout-recompute: agent block marks task blocked; plan stays active" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const seeded = seedAnchorChildPlan(&suite, arena, "ag-closeout-block", 2);
    const child_id = seeded.child_id;
    const blocker_id_str = seeded.task_ids[1]; // task 2 is the blocker

    // Pull task 1 (the one that will be blocked).
    const pull_out = mustRunAgent(&suite, &.{ "pull", child_id, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractString(gpa, pull_out, "\"claim_token\":\"");
    defer gpa.free(token);

    gpa.free(mustRunAgent(&suite, &.{
        "block", "--claim", token, "--blocker", blocker_id_str, "--reason", "waiting", "--json",
    }));

    const after_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id });
    defer gpa.free(after_buf);
    const after = parseJSON(PlanJSON, arena, after_buf);
    // plan has a blocked task and a todo task — not done.
    try std.testing.expect(!std.mem.eql(u8, "done", after.status));
}

// ============================================================================
// Test 7: capability boundary — agent completing ALL tasks of an ANCHOR plan
//         must NOT auto-close it (recomputeStatus caps anchors at active)
// ============================================================================

test "closeout-recompute: agent completing all tasks of an ANCHOR plan does NOT auto-close it (capability boundary)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    // Create a top-level anchor plan (NO --parent → parent_plan_id IS NULL).
    // recomputeStatus must cap its status at "active" regardless of task completion.
    const anchor_json = suite.mustRun(&.{ "plan", "create", "--json", "--status", "active", "anchor-cap-boundary" });
    defer gpa.free(anchor_json);
    const anchor_id = std.fmt.allocPrint(arena, "{d}", .{parseJSON(PlanJSON, arena, anchor_json).id}) catch @panic("OOM");

    // Add two tasks directly on the anchor (not on a child plan).
    for (0..2) |idx| {
        const title = std.fmt.allocPrint(arena, "anchor-task-{d}", .{idx + 1}) catch @panic("OOM");
        const t_json = suite.mustRun(&.{ "task", "add", "--json", "--plan", anchor_id, title });
        gpa.free(t_json);
    }

    // Confirm anchor starts active.
    {
        const before_buf = suite.mustRun(&.{ "plan", "show", "--json", anchor_id });
        defer gpa.free(before_buf);
        try std.testing.expectEqualStrings("active", parseJSON(PlanJSON, arena, before_buf).status);
    }

    // Complete both tasks via the agent path (pull → complete).
    for (0..2) |_| {
        const pull_out = mustRunAgent(&suite, &.{ "pull", anchor_id, "--no-locality-probe", "--json" });
        defer gpa.free(pull_out);
        const token = extractString(gpa, pull_out, "\"claim_token\":\"");
        defer gpa.free(token);
        gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--json" }));
    }

    // The anchor MUST still be "active" — the agent terminal verb must NOT
    // auto-close an operator-owned top-level plan.
    const after_buf = suite.mustRun(&.{ "plan", "show", "--json", anchor_id });
    defer gpa.free(after_buf);
    const after = parseJSON(PlanJSON, arena, after_buf);
    try std.testing.expectEqualStrings("active", after.status);
}

// ============================================================================
// Test 6: mid-plan partial completion updates plan consistently
// ============================================================================

test "closeout-recompute: mid-plan agent complete updates plan roll-up consistently" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const seeded = seedAnchorChildPlan(&suite, arena, "ag-closeout-mid", 3);
    const child_id = seeded.child_id;

    // Complete 2 of 3 tasks via agent.
    for (0..2) |_| {
        const pull_out = mustRunAgent(&suite, &.{ "pull", child_id, "--no-locality-probe", "--json" });
        defer gpa.free(pull_out);
        const token = extractString(gpa, pull_out, "\"claim_token\":\"");
        defer gpa.free(token);
        gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--json" }));
    }

    // Plan should be active (one task still todo).
    {
        const mid_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id });
        defer gpa.free(mid_buf);
        const mid = parseJSON(PlanJSON, arena, mid_buf);
        try std.testing.expect(!std.mem.eql(u8, "done", mid.status));
    }

    // Complete the last task.
    const pull_last = mustRunAgent(&suite, &.{ "pull", child_id, "--no-locality-probe", "--json" });
    defer gpa.free(pull_last);
    const token_last = extractString(gpa, pull_last, "\"claim_token\":\"");
    defer gpa.free(token_last);
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token_last, "--json" }));

    // Plan must now be done.
    {
        const final_buf = suite.mustRun(&.{ "plan", "show", "--json", child_id });
        defer gpa.free(final_buf);
        const final = parseJSON(PlanJSON, arena, final_buf);
        try std.testing.expectEqualStrings("done", final.status);
    }
}
