//! integration_tests/parallel_dispatch_workflow_test.zig — black-box tests for
//! `workflows/parallel-dispatch.lua` (plan 760 M1, tasks 4620–4624).
//!
//! These tests exercise the deterministic, spawn-free wave/lane computation
//! seam through the planar-execute engine against a seeded isolated PLANAR_DB.
//! The seam computes waves/paths/branch-names/merge-order and HANDS BACK; it
//! never spawns coders and never runs git worktree/branch/merge. The model does
//! that. So these tests assert on the seam's JSON `flow.result` payload only.
//!
//! Covered scenarios (test-spec artifact 429):
//!
//!   single-wave-compute / seam-plan-phase (Happy): a plan whose
//!     recommend-strategy returns two disjoint eligible tasks yields ONE wave of
//!     two lanes, each with a distinct worktree path + lane branch, and carries
//!     the serialized set through untouched.
//!
//!   worktree-create (naming): epic branch is `epic/p<plan>-<slug>`, each lane
//!     branch is `cycle/p<plan>/<task-slug>` and worktree path mirrors it.
//!
//!   single-wave-compute (Empty-null): a plan with fewer than two eligible
//!     tasks (fan_out_available:false) yields NO fan-out wave — an empty lane
//!     list with fan_out=false, not an error.
//!
//!   manual-fan-in: the fan_in phase emits the stable (task-id-ordered) merge
//!     order and teardown list even when the input lanes are out of order.
//!
//!   Determinism: the seam is byte-stable across runs — running the same phase
//!     against the same DB twice yields identical stdout (a resume must
//!     recompute identical waves). Guaranteed by array-ordered lanes + sorted
//!     object keys in planar-execute's JSON serializer.
//!
//!   Spawn-free contract: the seam source contains no model-spawning primitive.
//!
//! The tests isolate PLANAR_DB via the harness Suite tmp dir and prepend the
//! harness binary dirs to PATH so planar-execute's inner `cli.planar` shells hit
//! the same binary + DB.

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// resolveEnv — same pattern as dispatch_workflow_test.zig
// ---------------------------------------------------------------------------

fn resolveEnv(comptime key: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key ++ "=")) return s[(key ++ "=").len..];
    }
    @panic(key ++ " is not set. Run via: make test-integration");
}

// ---------------------------------------------------------------------------
// RunResult / runExecute / mustExecute — mirror dispatch_workflow_test.zig
// ---------------------------------------------------------------------------

const RunResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,
    fn deinit(self: RunResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }
};

fn runExecute(
    gpa: std.mem.Allocator,
    cwd: []const u8,
    db_path: []const u8,
    args: []const []const u8,
) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveEnv("PLANAR_EXECUTE_BIN"));
    for (args) |a| try argv.append(gpa, a);

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    try env_map.put("PLANAR_DB", db_path);
    try env_map.put("PLANAR_CONFIG_PATH", "/nonexistent-planar-config.toml");
    try env_map.put("PLANAR_DISABLE_WORKTREE_GATE", "1");

    const planar_bin = resolveEnv("PLANAR_BIN");
    const planar_dir = std.fs.path.dirname(planar_bin) orelse ".";
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ planar_dir, old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .cwd = .{ .path = cwd },
        .environ_map = &env_map,
    });
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr, .gpa = gpa };
}

fn mustExecute(
    gpa: std.mem.Allocator,
    cwd: []const u8,
    db_path: []const u8,
    args: []const []const u8,
) !RunResult {
    const res = try runExecute(gpa, cwd, db_path, args);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "planar-execute failed (term={any})\nstdout: {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("mustExecute: planar-execute exited non-zero");
    }
    return res;
}

/// repoRootFromBin — derive repo root from PLANAR_BIN (three dirname levels:
/// bin/planar → bin → repo-root; the harness build lays the binary at
/// <root>/bin/planar so two dirname levels reach the root, but the dispatch
/// test uses three because PLANAR_BIN there is under zig-out. We resolve the
/// workflow relative to the binary's grand-grandparent, matching the dispatch
/// test's proven path, then fall back if the workflow is not there.
fn repoRootFromBin(allocator: std.mem.Allocator) ![]const u8 {
    const bin_path = resolveEnv("PLANAR_BIN");
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    return allocator.dupe(u8, d3);
}

/// workflowPath resolves the seam path relative to the repo root, trying the
/// three-dirname root first (matches dispatch_workflow_test.zig) and the
/// two-dirname root as a fallback (when PLANAR_BIN is <root>/bin/planar).
fn workflowPath(gpa: std.mem.Allocator) ![]const u8 {
    const bin_path = resolveEnv("PLANAR_BIN");
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;

    // Candidate 1: three-dirname root (zig-out/bin/planar layout).
    if (std.fs.path.dirname(d2)) |d3| {
        const p = try std.fs.path.join(gpa, &.{ d3, "workflows", "parallel-dispatch.lua" });
        if (std.Io.Dir.cwd().access(std.testing.io, p, .{})) |_| {
            return p;
        } else |_| {
            gpa.free(p);
        }
    }
    // Candidate 2: two-dirname root (bin/planar layout).
    const p2 = try std.fs.path.join(gpa, &.{ d2, "workflows", "parallel-dispatch.lua" });
    return p2;
}

// ---------------------------------------------------------------------------
// JSON shapes for the seam result payloads.
// ---------------------------------------------------------------------------

const Lane = struct {
    task_id: i64,
    slug: []const u8,
    title: []const u8,
    branch: []const u8,
    worktree: []const u8,
};

const PlanResult = struct {
    plan_id: i64,
    fan_out: bool,
    epic_branch: []const u8,
    base: []const u8,
    lane_count: i64,
    lanes: []Lane = &.{},
};

const EmptyPlanResult = struct {
    plan_id: i64,
    fan_out: bool,
    lane_count: i64,
    reason: []const u8 = "",
};

const FanInResult = struct {
    plan_id: i64,
    lane_count: i64,
    merge_order: []const []const u8,
    teardown_worktrees: []const []const u8,
};

// ---------------------------------------------------------------------------
// Seeding: register a project + org assoc, read the repo slug back, and add
// disjoint-touch tasks. Mirrors plan_recommend_strategy_test.zig's approach so
// the tasks flow through the SAME eligibility engine the seam consumes.
// ---------------------------------------------------------------------------

const PlanJSON = struct { id: i64 };
const TaskAddJSON = struct { id: i64 };

fn registerRepoSlug(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    _ = suite.registerProject("pd-repo");
    const assoc_slug = "pd-org";
    suite.allocator.free(suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "org" }));
    const root = suite.tmpAbsPath();
    suite.allocator.free(suite.mustRun(&.{ "assoc", "add", assoc_slug, root }));

    const Member = struct { id: i64, slug: []const u8, name: []const u8 };
    const members = suite.mustRunJSON([]Member, arena, &.{ "assoc", "members", assoc_slug, "--json" });
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "pd-repo")) return m.slug;
    }
    std.debug.panic("registered repo slug not found in assoc members", .{});
}

fn addTaskSlug(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    pid: []const u8,
    slug: []const u8,
    title: []const u8,
) i64 {
    const t = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--slug", slug, "--json", title,
    });
    return t.id;
}

fn touchPath(suite: *harness.Suite, repo_slug: []const u8, task_id: i64, path: []const u8) void {
    const gpa = suite.allocator;
    const tid = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch unreachable;
    defer gpa.free(tid);
    gpa.free(suite.mustRun(&.{ "task", "touches", "add", tid, repo_slug, "--path", path }));
}

/// laneByTaskId finds a lane by task id in a PlanResult (panics if absent).
fn laneByTaskId(res: PlanResult, id: i64) Lane {
    for (res.lanes) |l| {
        if (l.task_id == id) return l;
    }
    std.debug.panic("lane for task {d} not present in seam result", .{id});
}

// ---------------------------------------------------------------------------
// 4620/4621/4622: seam plan phase — two disjoint eligible tasks compute one
// wave of two lanes with the locked naming convention.
// ---------------------------------------------------------------------------

test "parallel-dispatch plan: two disjoint eligible tasks compute one wave of two lanes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-happy", "--json", "PD_HAPPY",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_a = addTaskSlug(&suite, arena, pid, "lane-alpha", "Alpha lane");
    touchPath(&suite, repo, t_a, "src/alpha.zig");
    const t_b = addTaskSlug(&suite, arena, pid, "lane-bravo", "Bravo lane");
    touchPath(&suite, repo, t_b, "src/bravo.zig");
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer res.deinit();

    const out = std.mem.trim(u8, res.stdout, " \t\r\n");
    const parsed = try std.json.parseFromSlice(PlanResult, arena, out, .{ .ignore_unknown_fields = true });

    try std.testing.expectEqual(plan.id, parsed.value.plan_id);
    try std.testing.expect(parsed.value.fan_out);
    try std.testing.expectEqual(@as(i64, 2), parsed.value.lane_count);
    try std.testing.expectEqual(@as(usize, 2), parsed.value.lanes.len);

    // Epic branch: epic/p<plan>-<plan-slug>.
    const want_epic = try std.fmt.allocPrint(arena, "epic/p{d}-pd-happy", .{plan.id});
    try std.testing.expectEqualStrings(want_epic, parsed.value.epic_branch);
    try std.testing.expectEqualStrings("HEAD", parsed.value.base);

    // Lanes are sorted by task id (byte-stable ordering).
    try std.testing.expect(parsed.value.lanes[0].task_id < parsed.value.lanes[1].task_id);

    // Each lane's branch + worktree follow the locked naming convention.
    const la = laneByTaskId(parsed.value, t_a);
    const want_branch_a = try std.fmt.allocPrint(arena, "cycle/p{d}/lane-alpha", .{plan.id});
    const want_wt_a = try std.fmt.allocPrint(arena, ".worktrees/cycle/p{d}/lane-alpha", .{plan.id});
    try std.testing.expectEqualStrings(want_branch_a, la.branch);
    try std.testing.expectEqualStrings(want_wt_a, la.worktree);
    try std.testing.expectEqualStrings("lane-alpha", la.slug);

    const lb = laneByTaskId(parsed.value, t_b);
    const want_branch_b = try std.fmt.allocPrint(arena, "cycle/p{d}/lane-bravo", .{plan.id});
    try std.testing.expectEqualStrings(want_branch_b, lb.branch);

    // Worktree paths are DISTINCT per lane.
    try std.testing.expect(!std.mem.eql(u8, la.worktree, lb.worktree));
}

// ---------------------------------------------------------------------------
// seam-plan-phase: the serialized set is carried through untouched (a
// serialized task is never placed in a lane).
// ---------------------------------------------------------------------------

test "parallel-dispatch plan: serialized task is carried through, never placed in a lane" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-mixed", "--json", "PD_MIXED",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Two eligible + one serialized (touches a migration → rule 3).
    const t_a = addTaskSlug(&suite, arena, pid, "elig-a", "Eligible A");
    touchPath(&suite, repo, t_a, "src/a.zig");
    const t_b = addTaskSlug(&suite, arena, pid, "elig-b", "Eligible B");
    touchPath(&suite, repo, t_b, "src/b.zig");
    const t_mig = addTaskSlug(&suite, arena, pid, "mig", "Migrator");
    touchPath(&suite, repo, t_mig, "migrations/00099_widget.sql");
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    // The two eligible tasks are lanes; the migrator is NOT.
    const parsed = try std.json.parseFromSlice(PlanResult, arena, out, .{ .ignore_unknown_fields = true });
    try std.testing.expectEqual(@as(i64, 2), parsed.value.lane_count);
    for (parsed.value.lanes) |l| {
        try std.testing.expect(l.task_id != t_mig);
    }

    // The serialized task surfaces in the result (carried through). We assert on
    // the raw JSON so the excluded_by reason is visible in the payload.
    const mig_ref = try std.fmt.allocPrint(arena, "\"task_id\":{d}", .{t_mig});
    try std.testing.expect(std.mem.indexOf(u8, out, "\"serialized\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, mig_ref) != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"excluded_by\"") != null);
}

// ---------------------------------------------------------------------------
// single-wave-compute (Empty-null): fewer than two eligible tasks yields NO
// fan-out wave (empty lane list, fan_out=false), not an error.
// ---------------------------------------------------------------------------

test "parallel-dispatch plan: fewer than two eligible tasks yields no fan-out wave" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-empty", "--json", "PD_EMPTY",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Only one eligible task → fan_out_available:false.
    const t_only = addTaskSlug(&suite, arena, pid, "only-lane", "Only lane");
    touchPath(&suite, repo, t_only, "src/only.zig");
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(EmptyPlanResult, arena, out, .{ .ignore_unknown_fields = true });
    // Legitimately empty, not an error: exit 0 (asserted by mustExecute),
    // fan_out=false, lane_count=0, with a human reason.
    try std.testing.expect(!parsed.value.fan_out);
    try std.testing.expectEqual(@as(i64, 0), parsed.value.lane_count);
    try std.testing.expect(parsed.value.reason.len > 0);
}

// ---------------------------------------------------------------------------
// manual-fan-in: the fan_in phase emits a stable (task-id-ordered) merge order
// and teardown list even when the input lanes arrive out of order.
// ---------------------------------------------------------------------------

test "parallel-dispatch fan_in: merge order is stable by task id regardless of input order" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    _ = suite.registerProject("pd-fanin");
    suite.addAssoc("pd-fanin", null);

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    // Input lanes deliberately OUT of task-id order (7 before 3).
    const args_json =
        \\{"plan_id":42,"lanes":[
        \\{"task_id":7,"branch":"cycle/p42/lane-g","worktree":".worktrees/cycle/p42/lane-g"},
        \\{"task_id":3,"branch":"cycle/p42/lane-c","worktree":".worktrees/cycle/p42/lane-c"}
        \\]}
    ;

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "fan_in", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(FanInResult, arena, out, .{ .ignore_unknown_fields = true });
    try std.testing.expectEqual(@as(i64, 42), parsed.value.plan_id);
    try std.testing.expectEqual(@as(i64, 2), parsed.value.lane_count);

    // Merge order sorted ascending by task id: lane-c (3) before lane-g (7).
    try std.testing.expectEqual(@as(usize, 2), parsed.value.merge_order.len);
    try std.testing.expectEqualStrings("cycle/p42/lane-c", parsed.value.merge_order[0]);
    try std.testing.expectEqualStrings("cycle/p42/lane-g", parsed.value.merge_order[1]);

    // Teardown list follows the same stable order.
    try std.testing.expectEqualStrings(".worktrees/cycle/p42/lane-c", parsed.value.teardown_worktrees[0]);
    try std.testing.expectEqualStrings(".worktrees/cycle/p42/lane-g", parsed.value.teardown_worktrees[1]);
}

// ---------------------------------------------------------------------------
// Determinism: the seam is byte-stable across runs. Running the plan phase
// against the same DB twice yields identical stdout (a resume must recompute
// identical waves). This is the load-bearing reason the computation lives in a
// deterministic seam and not in drift-prone skill prose.
// ---------------------------------------------------------------------------

test "parallel-dispatch plan: output is byte-stable across repeated runs" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-stable", "--json", "PD_STABLE",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Three eligible tasks so the lane array + several object keys exercise both
    // the array ordering and the sorted-object-key path.
    const t1 = addTaskSlug(&suite, arena, pid, "lane-one", "One");
    touchPath(&suite, repo, t1, "src/one.zig");
    const t2 = addTaskSlug(&suite, arena, pid, "lane-two", "Two");
    touchPath(&suite, repo, t2, "src/two.zig");
    const t3 = addTaskSlug(&suite, arena, pid, "lane-three", "Three");
    touchPath(&suite, repo, t3, "src/three.zig");
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const r1 = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer r1.deinit();
    const r2 = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer r2.deinit();

    // The whole stdout (including object key order) must be byte-identical.
    try std.testing.expectEqualStrings(r1.stdout, r2.stdout);
    // Sanity: it is a fan-out wave of three lanes (not a trivially-equal empty).
    try std.testing.expect(std.mem.indexOf(u8, r1.stdout, "\"fan_out\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, r1.stdout, "\"lane_count\":3") != null);
}

// ---------------------------------------------------------------------------
// Spawn-free contract: the seam source contains no model-spawning primitive and
// no git worktree/branch/merge (those are the model's job — the seam only
// computes and hands back).
// ---------------------------------------------------------------------------

test "parallel-dispatch source is spawn-free and computes only (no git worktree/merge)" {
    const gpa = std.testing.allocator;

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    const source = std.Io.Dir.cwd().readFileAlloc(std.testing.io, wf_path, gpa, .limited(256 * 1024)) catch
        @panic("failed to read parallel-dispatch.lua");
    defer gpa.free(source);

    // No model-spawning / general-exec primitives (mirrors host.zig DENIED set).
    const forbidden = [_][]const u8{
        "os.execute", "io.open", "spawn(", "agent(", "claude", "codex", "headless",
    };
    for (forbidden) |bad| {
        if (std.mem.indexOf(u8, source, bad) != null) {
            std.debug.print("parallel-dispatch.lua references a forbidden primitive: {s}\n", .{bad});
            return error.SpawnPrimitiveFound;
        }
    }

    // The seam does NOT itself run worktree/branch/merge git ops — those are the
    // model's job. It calls git host fns for none of these; assert the source
    // never names a git.* mutation the confined host surface does not even
    // expose (git worktree add / git merge). If a later milestone wires these,
    // that is a deliberate change and this assertion is the tripwire.
    if (std.mem.indexOf(u8, source, "git.worktree") != null or
        std.mem.indexOf(u8, source, "git.merge") != null or
        std.mem.indexOf(u8, source, "git.branch") != null)
    {
        std.debug.print("parallel-dispatch.lua runs a git op the seam must hand back to the model\n", .{});
        return error.SeamRunsGitOp;
    }
}
