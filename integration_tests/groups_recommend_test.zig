//! integration_tests/groups_recommend_test.zig — black-box tests for the
//! `planar groups recommend <plan-id> [--budget] [--json]` verb (M3.2,
//! plan 637).
//!
//! The verb is the read-only grouping arm: it loads a plan's open tasks, each
//! task's effective derived closure (the `closures` rows, role
//! modify/reference), and the task dependency DAG (`entity_links` `blocks`
//! edges), then forms slices that minimize closure replication under a window
//! budget via the greedy heuristic. The tests drive the full pipeline through
//! the compiled binary: register a project, write a fixture `.zig` corpus,
//! declare each task's seed via `task touches add --path`, run
//! `closure compute` per task to populate real `closures` rows, then call
//! `groups recommend --json` and assert the slice contract.
//!
//! Asserted contract:
//!   - slice membership: heavily-overlapping tasks co-locate; disjoint tasks
//!     stay in separate slices.
//!   - budget compliance: no slice's unioned cost exceeds the budget; a tight
//!     budget splits a pair that a generous budget merges.
//!   - per-slice cost + total_cost are reported and the union is deduped.
//!   - dependency edges: a plan with a `blocks` edge produces a SCHEDULABLE
//!     grouping (the edge-direction caveat guard — a flipped mapping would
//!     surface as a non-schedulable slice-DAG).

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64 };
const TaskAddJSON = struct { id: i64 };
const ComputeJSON = struct {
    task_id: i64 = 0,
    rows_written: usize = 0,
};

const Slice = struct {
    task_ids: []const i64 = &.{},
    union_symbols: []const []const u8 = &.{},
    cost: i64 = 0,
};
const Summary = struct {
    slices: usize = 0,
    total_cost: i64 = 0,
};
const GroupsJSON = struct {
    plan_id: i64 = 0,
    budget: i64 = 0,
    open_tasks: usize = 0,
    solver: []const u8 = "",
    optimal_available: bool = false,
    selected_greedy: bool = false,
    slices: []const Slice = &.{},
    summary: Summary = .{},
};

/// True iff the optional external `mtkahypar` binary is runnable on this
/// machine (mirrors `engine.grouping.mtkahypar.solverAvailable` and the
/// `gitAvailable` probe in the worktree scenario). When false, the
/// `--solver=mtkahypar` path degrades to greedy; when true, the optimal arm
/// actually runs and the cost-≤-greedy guard (task 4247) is exercised.
fn mtkahyparAvailable(gpa: std.mem.Allocator) bool {
    const r = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "mtkahypar", "--probe" },
    }) catch return false;
    defer gpa.free(r.stdout);
    defer gpa.free(r.stderr);
    return switch (r.term) {
        .exited => |code| code == 0,
        else => false,
    };
}

/// Write `data` to `<tmp>/<rel>`, creating parent dirs.
fn writeFixture(suite: *harness.Suite, rel: []const u8, data: []const u8) void {
    const gpa = suite.allocator;
    const root = suite.tmpAbsPath();
    const abs = std.fs.path.join(gpa, &.{ root, rel }) catch @panic("OOM");
    defer gpa.free(abs);
    if (std.fs.path.dirname(abs)) |dir| {
        std.Io.Dir.cwd().createDirPath(std.testing.io, dir) catch |e|
            std.debug.panic("createDirPath {s}: {s}", .{ dir, @errorName(e) });
    }
    std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = abs, .data = data }) catch |e|
        std.debug.panic("writeFile {s}: {s}", .{ abs, @errorName(e) });
}

/// Register a project at the suite tmp + an org assoc; return the repo slug.
fn registerRepoSlug(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    _ = suite.registerProject("gr-repo");
    const cr = suite.mustRun(&.{ "assoc", "create", "gr-org", "--kind", "org" });
    suite.allocator.free(cr);
    const root = suite.tmpAbsPath();
    const ad = suite.mustRun(&.{ "assoc", "add", "gr-org", root });
    suite.allocator.free(ad);

    const Member = struct { id: i64, slug: []const u8, name: []const u8 };
    const members = suite.mustRunJSON([]Member, arena, &.{ "assoc", "members", "gr-org", "--json" });
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "gr-repo")) return m.slug;
    }
    std.debug.panic("registered repo slug not found", .{});
}

fn addTask(suite: *harness.Suite, arena: std.mem.Allocator, pid: []const u8, title: []const u8) i64 {
    const t = suite.mustRunJSON(TaskAddJSON, arena, &.{ "task", "add", "--plan", pid, "--json", title });
    return t.id;
}

fn touchPath(suite: *harness.Suite, repo_slug: []const u8, task_id: i64, path: []const u8) void {
    const gpa = suite.allocator;
    const tid = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch unreachable;
    defer gpa.free(tid);
    const out = suite.mustRun(&.{ "task", "touches", "add", tid, repo_slug, "--path", path });
    gpa.free(out);
}

fn computeClosure(suite: *harness.Suite, arena: std.mem.Allocator, task_id: i64) void {
    const tid = std.fmt.allocPrint(arena, "{d}", .{task_id}) catch unreachable;
    const comp = suite.mustRunJSON(ComputeJSON, arena, &.{ "closure", "compute", tid, "--json" });
    if (comp.rows_written == 0) std.debug.panic("expected closure rows for task {d}", .{task_id});
}

fn sliceWith(g: GroupsJSON, task_id: i64) ?Slice {
    for (g.slices) |s| {
        for (s.task_ids) |t| if (t == task_id) return s;
    }
    return null;
}

fn sliceHas(s: Slice, task_id: i64) bool {
    for (s.task_ids) |t| if (t == task_id) return true;
    return false;
}

test "groups recommend: overlapping tasks co-locate, disjoint stay apart, budget honored" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);

    // Shared dependency file referenced by tasks A and B (overlap).
    writeFixture(&suite,
        \\src/core.zig
    ,
        \\pub fn api(n: u32) u32 {
        \\    return n + 1;
        \\}
        \\
    );
    // a.zig: modifies its own run(); references core.api (shared with b).
    writeFixture(&suite,
        \\src/a.zig
    ,
        \\const core = @import("core.zig");
        \\pub fn run() u32 {
        \\    return core.api(1);
        \\}
        \\
    );
    // b.zig: modifies its own run(); references core.api (shared with a).
    writeFixture(&suite,
        \\src/b.zig
    ,
        \\const core = @import("core.zig");
        \\pub fn run() u32 {
        \\    return core.api(2);
        \\}
        \\
    );
    // c.zig: disjoint — references nothing the others touch.
    writeFixture(&suite,
        \\src/c.zig
    ,
        \\pub fn lonely() u32 {
        \\    return 42;
        \\}
        \\
    );

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "gr-plan", "--json", "GR_PLAN",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const ta = addTask(&suite, arena, pid, "task A");
    const tb = addTask(&suite, arena, pid, "task B");
    const tc = addTask(&suite, arena, pid, "task C");
    touchPath(&suite, repo, ta, "src/a.zig");
    touchPath(&suite, repo, tb, "src/b.zig");
    touchPath(&suite, repo, tc, "src/c.zig");
    computeClosure(&suite, arena, ta);
    computeClosure(&suite, arena, tb);
    computeClosure(&suite, arena, tc);

    // Generous budget: A and B (sharing core.api) co-locate; C stays alone.
    const g = suite.mustRunJSON(GroupsJSON, arena, &.{
        "groups", "recommend", pid, "--budget", "100000", "--json",
    });
    try std.testing.expectEqual(plan.id, g.plan_id);
    try std.testing.expectEqual(@as(i64, 100000), g.budget);
    try std.testing.expectEqual(@as(usize, 3), g.open_tasks);
    try std.testing.expectEqual(@as(usize, 2), g.slices.len);
    try std.testing.expectEqual(@as(usize, 2), g.summary.slices);
    // Default solver is greedy; greedy is the baseline (not the optimal arm).
    try std.testing.expectEqualStrings("greedy", g.solver);
    try std.testing.expect(!g.optimal_available);

    // A's slice holds B but not C.
    const sa = sliceWith(g, ta) orelse std.debug.panic("no slice for A", .{});
    try std.testing.expect(sliceHas(sa, tb));
    try std.testing.expect(!sliceHas(sa, tc));

    // C is a singleton.
    const sc = sliceWith(g, tc) orelse std.debug.panic("no slice for C", .{});
    try std.testing.expectEqual(@as(usize, 1), sc.task_ids.len);

    // The shared symbol is deduped: A+B slice union contains core.api ONCE.
    var core_api_count: usize = 0;
    for (sa.union_symbols) |sym| {
        if (std.mem.eql(u8, sym, "core.api")) core_api_count += 1;
    }
    try std.testing.expectEqual(@as(usize, 1), core_api_count);

    // Budget compliance: no slice exceeds the budget.
    for (g.slices) |s| try std.testing.expect(s.cost <= 100000);

    // total_cost equals the sum of slice costs.
    var sum: i64 = 0;
    for (g.slices) |s| sum += s.cost;
    try std.testing.expectEqual(g.summary.total_cost, sum);
}

test "groups recommend: a tight budget splits a pair a generous budget merges" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);

    writeFixture(&suite,
        \\src/core.zig
    ,
        \\pub fn api(n: u32) u32 {
        \\    return n + 1;
        \\}
        \\
    );
    writeFixture(&suite,
        \\src/a.zig
    ,
        \\const core = @import("core.zig");
        \\pub fn run() u32 {
        \\    return core.api(1);
        \\}
        \\
    );
    writeFixture(&suite,
        \\src/b.zig
    ,
        \\const core = @import("core.zig");
        \\pub fn run() u32 {
        \\    return core.api(2);
        \\}
        \\
    );

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "gr-budget", "--json", "GR_BUDGET",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const ta = addTask(&suite, arena, pid, "task A");
    const tb = addTask(&suite, arena, pid, "task B");
    touchPath(&suite, repo, ta, "src/a.zig");
    touchPath(&suite, repo, tb, "src/b.zig");
    computeClosure(&suite, arena, ta);
    computeClosure(&suite, arena, tb);

    // Generous budget: they merge into one slice.
    const merged = suite.mustRunJSON(GroupsJSON, arena, &.{
        "groups", "recommend", pid, "--budget", "100000", "--json",
    });
    try std.testing.expectEqual(@as(usize, 1), merged.slices.len);

    // Tight budget (1 token): no merge fits; each task stays its own slice
    // (a lone over-budget task is reported as-is, never dropped).
    const split = suite.mustRunJSON(GroupsJSON, arena, &.{
        "groups", "recommend", pid, "--budget", "1", "--json",
    });
    try std.testing.expectEqual(@as(usize, 2), split.slices.len);
}

test "groups recommend: a blocks edge produces a schedulable grouping (caveat guard)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);

    // core.api shared by t1 and t3 (heavy overlap); t2 references mid.thing.
    writeFixture(&suite,
        \\src/core.zig
    ,
        \\pub fn api(n: u32) u32 {
        \\    return n + 1;
        \\}
        \\
    );
    writeFixture(&suite,
        \\src/mid.zig
    ,
        \\pub fn thing(n: u32) u32 {
        \\    return n * 3;
        \\}
        \\
    );
    writeFixture(&suite,
        \\src/t1.zig
    ,
        \\const core = @import("core.zig");
        \\pub fn run() u32 {
        \\    return core.api(1);
        \\}
        \\
    );
    writeFixture(&suite,
        \\src/t2.zig
    ,
        \\const mid = @import("mid.zig");
        \\pub fn run() u32 {
        \\    return mid.thing(2);
        \\}
        \\
    );
    writeFixture(&suite,
        \\src/t3.zig
    ,
        \\const core = @import("core.zig");
        \\pub fn run() u32 {
        \\    return core.api(3);
        \\}
        \\
    );

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "gr-dag", "--json", "GR_DAG",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const t1 = addTask(&suite, arena, pid, "task 1");
    const t2 = addTask(&suite, arena, pid, "task 2");
    const t3 = addTask(&suite, arena, pid, "task 3");
    touchPath(&suite, repo, t1, "src/t1.zig");
    touchPath(&suite, repo, t2, "src/t2.zig");
    touchPath(&suite, repo, t3, "src/t3.zig");
    computeClosure(&suite, arena, t1);
    computeClosure(&suite, arena, t2);
    computeClosure(&suite, arena, t3);

    // Dependency chain via entity_links `blocks` edges (from is blocked BY to):
    //   t2 blocked by t1; t3 blocked by t2; t3 blocked by t1.
    // `links add <from> <to> --relationship blocks` writes the edge while
    // keeping both tasks 'todo' (so they stay in the open candidate set).
    const refs = [_][2]i64{ .{ t2, t1 }, .{ t3, t2 }, .{ t3, t1 } };
    for (refs) |pair| {
        const from = std.fmt.allocPrint(arena, "task:{d}", .{pair[0]}) catch unreachable;
        const to = std.fmt.allocPrint(arena, "task:{d}", .{pair[1]}) catch unreachable;
        const out = suite.mustRun(&.{ "links", "add", from, to, "--relationship", "blocks" });
        suite.allocator.free(out);
    }

    const g = suite.mustRunJSON(GroupsJSON, arena, &.{
        "groups", "recommend", pid, "--budget", "100000", "--json",
    });
    try std.testing.expectEqual(@as(usize, 3), g.open_tasks);

    // Verify the slice-DAG is schedulable under the known-correct dep orientation.
    // NOTE: this schedulability check uses hardcoded-correct dep values and
    // does NOT catch a loadDeps orientation flip on its own — the chain
    // t1→t2→t3 with t1/t3 overlapping produces the same "straddling" cycle for
    // both orientations. Orientation is directly guarded by the unit test
    // "load.loadDeps: edge-direction — from_id=blocked, to_id=blocker" in
    // src/engine/grouping/load.zig.
    try std.testing.expect(schedulableJSON(arena, g));
}

test "groups recommend: --solver=mtkahypar degrades to greedy when the binary is absent" {
    // M3.3b graceful-degradation path (D-HG4). On a machine WITHOUT the
    // optional `mtkahypar` binary — the CI / dev default — requesting the
    // optimal arm must NOT error: it falls back to the greedy arm, exits 0,
    // and reports `solver:"greedy"` + `optimal_available:false`. This is the
    // path that actually runs in this suite (the binary is not installed here);
    // the live optimal arm is exercised by the skip-if-absent unit test in
    // src/engine/grouping/mtkahypar.zig wherever the binary exists.
    const gpa = std.testing.allocator;
    // This test pins the binary-ABSENT degradation contract. When the optional
    // binary IS installed on the host the optimal arm actually runs, so the
    // `solver:"greedy"` + `optimal_available:false` assertions below would not
    // hold — the present-path contract is covered by the sibling
    // "...cost is never worse than greedy" test. Skip here when present.
    if (mtkahyparAvailable(gpa)) return error.SkipZigTest;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);

    writeFixture(&suite,
        \\src/core.zig
    ,
        \\pub fn api(n: u32) u32 {
        \\    return n + 1;
        \\}
        \\
    );
    writeFixture(&suite,
        \\src/a.zig
    ,
        \\const core = @import("core.zig");
        \\pub fn run() u32 {
        \\    return core.api(1);
        \\}
        \\
    );
    writeFixture(&suite,
        \\src/b.zig
    ,
        \\const core = @import("core.zig");
        \\pub fn run() u32 {
        \\    return core.api(2);
        \\}
        \\
    );

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "gr-solver", "--json", "GR_SOLVER",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const ta = addTask(&suite, arena, pid, "task A");
    const tb = addTask(&suite, arena, pid, "task B");
    touchPath(&suite, repo, ta, "src/a.zig");
    touchPath(&suite, repo, tb, "src/b.zig");
    computeClosure(&suite, arena, ta);
    computeClosure(&suite, arena, tb);

    // Request the optimal arm explicitly. Binary absent → degrade to greedy.
    const g = suite.mustRunJSON(GroupsJSON, arena, &.{
        "groups", "recommend", pid, "--budget", "100000", "--solver", "mtkahypar", "--json",
    });
    // Degraded, not errored: solver is greedy and optimal_available is false.
    try std.testing.expectEqualStrings("greedy", g.solver);
    try std.testing.expect(!g.optimal_available);
    try std.testing.expect(!g.selected_greedy);
    // The grouping is still produced (greedy result): A+B co-locate.
    try std.testing.expectEqual(@as(usize, 2), g.open_tasks);
    const sa = sliceWith(g, ta) orelse std.debug.panic("no slice for A", .{});
    try std.testing.expect(sliceHas(sa, tb));
}

test "groups recommend: --solver=mtkahypar cost is never worse than greedy (task 4247)" {
    // The end-to-end cost-≤-greedy guard. Runs only where the optional binary
    // is installed (skip-if-absent). On a COUPLED plan — three tasks whose
    // closures all share one heavy seed — the pre-fix solver arm could return a
    // partition WORSE than greedy (forced into singletons by an over-counted k).
    // The min-of-{solver,greedy} guard + the deduped-union k-selection make the
    // returned grouping's total_cost ≤ the greedy arm's on the same input.
    const gpa = std.testing.allocator;
    if (!mtkahyparAvailable(gpa)) return error.SkipZigTest;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);

    // A heavy shared core all three tasks reference → tight coupling.
    writeFixture(&suite,
        \\src/core.zig
    ,
        \\pub fn api(n: u32) u32 {
        \\    return n + 1;
        \\}
        \\pub fn helper(n: u32) u32 {
        \\    return api(n) + 2;
        \\}
        \\
    );
    inline for (.{ "a", "b", "c" }) |name| {
        writeFixture(&suite, "src/" ++ name ++ ".zig",
            \\const core = @import("core.zig");
            \\pub fn run() u32 {
            \\    return core.api(1) + core.helper(2);
            \\}
            \\
        );
    }

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "gr-coupled", "--json", "GR_COUPLED",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const ta = addTask(&suite, arena, pid, "task A");
    const tb = addTask(&suite, arena, pid, "task B");
    const tc = addTask(&suite, arena, pid, "task C");
    touchPath(&suite, repo, ta, "src/a.zig");
    touchPath(&suite, repo, tb, "src/b.zig");
    touchPath(&suite, repo, tc, "src/c.zig");
    computeClosure(&suite, arena, ta);
    computeClosure(&suite, arena, tb);
    computeClosure(&suite, arena, tc);

    // Greedy baseline on the same input + budget.
    const budget = "100000";
    const greedy_g = suite.mustRunJSON(GroupsJSON, arena, &.{
        "groups", "recommend", pid, "--budget", budget, "--solver", "greedy", "--json",
    });
    // Optimal arm (binary present → actually runs).
    const solver_g = suite.mustRunJSON(GroupsJSON, arena, &.{
        "groups", "recommend", pid, "--budget", budget, "--solver", "mtkahypar", "--json",
    });

    // The availability probe imported the native wheel successfully. Any
    // invocation failure from here is a live integration failure, not an
    // acceptable skip/degradation path.
    try std.testing.expect(solver_g.optimal_available);
    try std.testing.expectEqualStrings("mtkahypar", solver_g.solver);

    // THE GUARANTEE: the solver arm's total cost is ≤ greedy's on this coupled
    // input. Before the fix the solver could report a strictly higher cost.
    try std.testing.expect(solver_g.summary.total_cost <= greedy_g.summary.total_cost);
}

test "groups recommend: an unknown --solver value is rejected" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "gr-badsolver", "--json", "GR_BADSOLVER",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // 'optimal' is not a valid solver name → non-zero exit.
    const stderr = suite.expectFailure(&.{ "groups", "recommend", pid, "--solver", "optimal", "--json" });
    suite.allocator.free(stderr);
}

/// Build the slice-precedence DAG from the known dependency chain
/// (t2←t1, t3←t2, t3←t1, i.e. blocker→blocked: t1→t2, t2→t3, t1→t3) over the
/// produced slices and verify it is acyclic via Kahn's algorithm. Independent
/// of the engine's internal cycle check — a second opinion the grouping is
/// schedulable under the hardcoded-correct orientation. This helper does NOT
/// guard against a loadDeps orientation flip by itself (the chain is symmetric);
/// see the unit test "load.loadDeps: edge-direction" for the direct guard.
fn schedulableJSON(arena: std.mem.Allocator, g: GroupsJSON) bool {
    // Map task id -> slice index. We can't read task ids out of order, so scan.
    const SliceIdx = struct {
        fn of(grp: GroupsJSON, task_id: i64) ?usize {
            for (grp.slices, 0..) |s, k| {
                for (s.task_ids) |t| if (t == task_id) return k;
            }
            return null;
        }
    };
    // Reconstruct the task ids actually present (smallest-three open tasks).
    // Collect all task ids from slices.
    var ids = std.ArrayList(i64).empty;
    defer ids.deinit(arena);
    for (g.slices) |s| {
        for (s.task_ids) |t| ids.append(arena, t) catch return false;
    }
    if (ids.items.len != 3) return false;
    std.mem.sort(i64, ids.items, {}, std.sort.asc(i64));
    const t1 = ids.items[0];
    const t2 = ids.items[1];
    const t3 = ids.items[2];

    // blocker -> blocked edges: t1->t2, t2->t3, t1->t3.
    const edges = [_][2]i64{ .{ t1, t2 }, .{ t2, t3 }, .{ t1, t3 } };

    var adj = std.AutoHashMapUnmanaged(usize, std.AutoHashMapUnmanaged(usize, void)).empty;
    for (edges) |e| {
        const sb = SliceIdx.of(g, e[0]) orelse return false;
        const st = SliceIdx.of(g, e[1]) orelse return false;
        if (sb == st) continue;
        const gop = adj.getOrPut(arena, sb) catch return false;
        if (!gop.found_existing) gop.value_ptr.* = .empty;
        gop.value_ptr.put(arena, st, {}) catch return false;
    }

    var indeg = std.AutoHashMapUnmanaged(usize, usize).empty;
    var k: usize = 0;
    while (k < g.slices.len) : (k += 1) indeg.put(arena, k, 0) catch return false;
    var ait = adj.iterator();
    while (ait.next()) |e| {
        var nit = e.value_ptr.iterator();
        while (nit.next()) |ne| {
            const cur = indeg.get(ne.key_ptr.*).?;
            indeg.put(arena, ne.key_ptr.*, cur + 1) catch return false;
        }
    }
    var queue = std.ArrayList(usize).empty;
    var iit = indeg.iterator();
    while (iit.next()) |e| if (e.value_ptr.* == 0) {
        queue.append(arena, e.key_ptr.*) catch return false;
    };
    var visited: usize = 0;
    while (queue.pop()) |node| {
        visited += 1;
        if (adj.get(node)) |neighbors| {
            var nit = neighbors.iterator();
            while (nit.next()) |ne| {
                const nb = ne.key_ptr.*;
                const cur = indeg.get(nb).?;
                indeg.put(arena, nb, cur - 1) catch return false;
                if (cur - 1 == 0) queue.append(arena, nb) catch return false;
            }
        }
    }
    return visited == g.slices.len;
}
