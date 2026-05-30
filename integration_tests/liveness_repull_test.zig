//! integration_tests/liveness_repull_test.zig — plan 493 F1.
//!
//! Verifies the claim-ritual liveness contract: when a worker crashes
//! and its lease lapses, the task it was working on MUST become
//! re-pullable. The fix widens `pickNextEligible` to also select tasks
//! whose status is `doing` but have no unexpired active claim.
//!
//! Test-spec scenarios (artifact 266):
//!   S1 — `liveness-crashed-task-repullable`   (post-reconcile state)
//!   S2 — `liveness-repull-pre-reconcile`      (window before reconcile)
//!   S3 — `liveness-selector-viewer-agree`     (plan next ↔ pull selector)
//!
//! Pre-F1 these tests are RED: `peek` / `pull` return `no_work` even
//! though `plan next` reports the task in the `available` bucket. Post-F1
//! they go GREEN by construction (same predicate as `nextWork`).

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// planar-agent shell-out (mirrors planar_agent_test.zig conventions)
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

/// Sleep for the given seconds via the testing IO clock. Allows lease
/// expiry to elapse without depending on wall-clock vagaries.
fn sleepSecs(s: u64) void {
    std.testing.io.sleep(std.Io.Duration.fromSeconds(@intCast(s)), std.Io.Clock.awake) catch {};
}

// =========================================================================
// S1 — Crashed task is re-pullable.
// slug: liveness-crashed-task-repullable
// =========================================================================

test "S1 liveness-crashed-task-repullable: reconciled doing-no-claim task is re-pulled (plan 493 F1)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-liveness-s1", "crashed-task");
    defer gpa.free(pid_arg);

    // Pull with a 1-second TTL so the lease lapses quickly.
    const pull1 = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "1", "--json" });
    defer gpa.free(pull1);
    const first_token = extractStringField(gpa, pull1, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(first_token);
    const first_task_id = extractIntField(pull1, "\"task\":{\"id\"") orelse @panic("no task id");

    // Simulate the worker crashing: let the lease lapse, then reconcile
    // marks the active claim `stale`. The task stays `doing` (verified
    // by the assertion below); this is the documented B1 defect.
    sleepSecs(2);
    const recon = mustRunAgent(&suite, &.{ "reconcile", "--stale-after", "0", "--json" });
    defer gpa.free(recon);
    try std.testing.expect(std.mem.indexOf(u8, recon, "\"claims_marked_stale\":1") != null);

    // Confirm the documented stranding: task is `doing` after reconcile.
    const tid_arg = std.fmt.allocPrint(gpa, "{d}", .{first_task_id}) catch @panic("OOM");
    defer gpa.free(tid_arg);
    const task_after_recon = suite.mustRun(&.{ "task", "show", "--json", tid_arg });
    defer gpa.free(task_after_recon);
    try std.testing.expect(std.mem.indexOf(u8, task_after_recon, "\"status\":\"doing\"") != null);

    // CONTRACT: peek + pull MUST re-select the same task. Pre-F1 both
    // return no_work because pickNextEligible requires `status='todo'`.
    const peek_out = mustRunAgent(&suite, &.{ "peek", pid_arg, "--json" });
    defer gpa.free(peek_out);
    if (std.mem.indexOf(u8, peek_out, "\"no_work\":true") != null) {
        std.debug.print("S1 RED: peek returned no_work on a claim-less doing task. peek_out: {s}\n", .{peek_out});
        return error.PeekRefusedClaimlessDoing;
    }
    const peek_task_id = extractIntField(peek_out, "\"task\":{\"id\"") orelse @panic("no peek task.id");
    try std.testing.expectEqual(first_task_id, peek_task_id);

    const pull2 = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "60", "--json" });
    defer gpa.free(pull2);
    if (std.mem.indexOf(u8, pull2, "\"no_work\":true") != null) {
        std.debug.print("S1 RED: pull returned no_work on a claim-less doing task. pull2: {s}\n", .{pull2});
        return error.PullRefusedClaimlessDoing;
    }
    const second_task_id = extractIntField(pull2, "\"task\":{\"id\"") orelse @panic("no task.id on repull");
    try std.testing.expectEqual(first_task_id, second_task_id);
    // Fresh claim token is issued for the second pull.
    const second_token = extractStringField(gpa, pull2, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(second_token);
    try std.testing.expect(!std.mem.eql(u8, first_token, second_token));
}

// =========================================================================
// S2 — Recovery without a prior reconcile (the pre-reconcile window).
// slug: liveness-repull-pre-reconcile
// =========================================================================

test "S2 liveness-repull-pre-reconcile: expired-but-active claim does not block re-pull (plan 493 F1)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-liveness-s2", "pre-recon-task");
    defer gpa.free(pid_arg);

    // Pull with a 1-second TTL and intentionally DO NOT reconcile.
    const pull1 = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "1", "--json" });
    defer gpa.free(pull1);
    const first_task_id = extractIntField(pull1, "\"task\":{\"id\"") orelse @panic("no task id");

    // Let the lease lapse. The claim row is still status='active' (no
    // reconcile has flipped it to 'stale'), just expired.
    sleepSecs(2);

    // CONTRACT: peek + pull MUST still re-select the task — the widened
    // predicate uses the `lease_expires_at >= now` half of "unexpired
    // active claim", matching the existing `nextWork` predicate. Pre-F1
    // both return no_work for two reasons: status='doing' fails the
    // first predicate AND the claim row exists.
    const peek_out = mustRunAgent(&suite, &.{ "peek", pid_arg, "--json" });
    defer gpa.free(peek_out);
    if (std.mem.indexOf(u8, peek_out, "\"no_work\":true") != null) {
        std.debug.print("S2 RED: peek returned no_work pre-reconcile. peek_out: {s}\n", .{peek_out});
        return error.PeekRefusedExpiredActiveClaim;
    }
    const peek_task_id = extractIntField(peek_out, "\"task\":{\"id\"") orelse @panic("no peek task.id");
    try std.testing.expectEqual(first_task_id, peek_task_id);

    const pull2 = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "60", "--json" });
    defer gpa.free(pull2);
    if (std.mem.indexOf(u8, pull2, "\"no_work\":true") != null) {
        std.debug.print("S2 RED: pull returned no_work pre-reconcile. pull2: {s}\n", .{pull2});
        return error.PullRefusedExpiredActiveClaim;
    }
    const second_task_id = extractIntField(pull2, "\"task\":{\"id\"") orelse @panic("no repull task.id");
    try std.testing.expectEqual(first_task_id, second_task_id);
}

// =========================================================================
// S3 — `plan next` available bucket agrees with the pull selector.
// slug: liveness-selector-viewer-agree
// =========================================================================

test "S3 liveness-selector-viewer-agree: plan next runnable buckets == pull selector (plan 493 F1)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ag-liveness-s3", "viewer-agree-task");
    defer gpa.free(pid_arg);

    // Drive the plan into a state with a doing-no-active-claim task:
    // pull with a 1-second TTL, let it lapse, leave it stranded.
    const pull1 = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "1", "--json" });
    defer gpa.free(pull1);
    const target_task_id = extractIntField(pull1, "\"task\":{\"id\"") orelse @panic("no task id");
    sleepSecs(2);

    // Snapshot the `plan next` view. Per `store.nextWork` (~L835–924),
    // a doing-task with an expired-active OR stale claim row lands in
    // the `stale` bucket; a doing-task with NO claim row lands in
    // `available`. Both are "runnable" — the contract here is that
    // every task surfaced as runnable by the viewer is also selectable
    // by `pull`. Pre-F1 the selector returns no_work for both shapes,
    // so the viewer and selector diverge by construction.
    const plan_next = suite.mustRun(&.{ "plan", "next", "--json", pid_arg });
    defer gpa.free(plan_next);

    const runnable_ids = collectRunnableTaskIds(gpa, plan_next) catch @panic("parse plan next runnable");
    defer gpa.free(runnable_ids);

    // Sanity: the viewer surfaces the stranded task in available or stale.
    var viewer_lists_target = false;
    for (runnable_ids) |id| {
        if (id == target_task_id) viewer_lists_target = true;
    }
    if (!viewer_lists_target) {
        std.debug.print(
            "S3 setup-check: plan next did not list the doing-no-claim task in available/stale; plan_next: {s}\n",
            .{plan_next},
        );
        return error.ViewerDidNotListStrandedTask;
    }

    // CONTRACT: every runnable task (available ∪ stale) is selectable
    // by `pull`. Assert the load-bearing half: the next `peek` returns
    // a task that the viewer also surfaces as runnable. Pre-F1 the
    // selector returns no_work, so the two diverge.
    const peek_out = mustRunAgent(&suite, &.{ "peek", pid_arg, "--json" });
    defer gpa.free(peek_out);
    if (std.mem.indexOf(u8, peek_out, "\"no_work\":true") != null) {
        std.debug.print(
            "S3 RED: viewer's runnable buckets contain {any} but peek returned no_work — selector/viewer divergence.\npeek_out: {s}\n",
            .{ runnable_ids, peek_out },
        );
        return error.SelectorViewerDivergence;
    }
    const peek_task_id = extractIntField(peek_out, "\"task\":{\"id\"") orelse @panic("no peek task.id");
    var peek_in_runnable = false;
    for (runnable_ids) |id| {
        if (id == peek_task_id) peek_in_runnable = true;
    }
    if (!peek_in_runnable) {
        std.debug.print(
            "S3 RED: peek returned task {d} which is NOT in the viewer's runnable buckets {any}\n",
            .{ peek_task_id, runnable_ids },
        );
        return error.PeekOutsideRunnableBuckets;
    }
}

// =========================================================================
// JSON helpers (mirrored from planar_agent_test.zig)
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

/// Walk the `plan next --json` output and collect every task id from
/// the `available` and `stale` arrays — the two "runnable" buckets per
/// the F1 contract. The shape (per handlers/plan/next.zig § JSON
/// shape) is:
///
///   { ..., "available": [ Task, ... ],
///         "claimed":   [ {"task": Task, "claim": Claim }, ... ],
///         "stale":     [ {"task": Task, "claim": Claim }, ... ],
///         "blocked":   [ Task, ... ], ... }
///
/// `available` is a flat list of task objects; `stale` is a list of
/// `{task, claim}` wrappers. This helper handles both shapes.
fn collectRunnableTaskIds(gpa: std.mem.Allocator, body: []const u8) ![]i64 {
    var parsed = try std.json.parseFromSlice(std.json.Value, gpa, body, .{});
    defer parsed.deinit();
    const root = parsed.value;
    if (root != .object) return error.UnexpectedJsonRoot;
    const obj = root.object;

    var out: std.ArrayList(i64) = .empty;
    defer out.deinit(gpa);

    // `available` — flat array of Task objects.
    if (obj.get("available")) |available| {
        if (available != .array) return error.AvailableNotArray;
        for (available.array.items) |item| {
            if (item != .object) continue;
            const id_field = item.object.get("id") orelse continue;
            switch (id_field) {
                .integer => |n| try out.append(gpa, n),
                else => {},
            }
        }
    }

    // `stale` — array of `{task: Task, claim: Claim}` wrappers.
    if (obj.get("stale")) |stale| {
        if (stale != .array) return error.StaleNotArray;
        for (stale.array.items) |wrapper| {
            if (wrapper != .object) continue;
            const task_field = wrapper.object.get("task") orelse continue;
            if (task_field != .object) continue;
            const id_field = task_field.object.get("id") orelse continue;
            switch (id_field) {
                .integer => |n| try out.append(gpa, n),
                else => {},
            }
        }
    }

    return try out.toOwnedSlice(gpa);
}
