//! engine.grouping.greedy — M3.1 greedy overlap-merge first pass.
//!
//! A sanity-check grouping heuristic that runs *before* reaching for a
//! hypergraph solver (M3.3). It forms **slices** (groups of tasks that share a
//! context window) by repeatedly merging the task-pair with the highest closure
//! **overlap**, stopping a slice's growth when its **unioned closure** would
//! exceed the window budget `B`.
//!
//! It is deliberately a heuristic with no optimality guarantee — its job is to
//! unblock the grouped measurement arm (build-spec M3) and to be the baseline
//! the solver must beat. What it *does* guarantee:
//!
//!   - **Budget (D-HG3).** No slice's unioned closure ever exceeds `B`. The
//!     union is computed DIRECTLY — shared units are deduplicated and paid for
//!     once (spec v0.1 §2; hypergraph-spec §6) — so the budget is exact by
//!     construction, with no balance-metric mismatch.
//!   - **Ordering (D-HG2).** A merge that would make the resulting slice-DAG
//!     unschedulable (induce an inter-slice cycle) is refused. Where ordering
//!     and replication-minimization conflict, **ordering wins** (spec v0.1 §2
//!     constraint 2): a high-overlap merge is rejected rather than produce a
//!     cyclic slice-DAG.
//!
//! ## D-HG1 — role asymmetry (the one decision greedy does NOT skip)
//!
//! hypergraph-spec §6 / decision 519 (D-HG1): a unit one task *writes*
//! (`modify`) is a different risk than a unit two tasks merely *read*
//! (`reference`). Two tasks co-located in one slice that both **modify** the
//! same symbol are a merge-collision hazard — co-locating them does not save
//! replication, it concentrates write risk. The greedy pass encodes the
//! asymmetry in the **overlap score**, not as a hard merge ban:
//!
//!   - A unit shared as `reference`/`reference`, or `modify`/`reference`, earns
//!     its full token weight toward the pair's overlap — co-locating genuinely
//!     saves resident context (the replication win the objective is after).
//!   - A unit BOTH tasks list as `modify` (a **write-write** conflict) earns
//!     ZERO overlap credit — co-locating two writers of the same symbol is a
//!     hazard, not a saving, so it must not pull the pair to the front of the
//!     merge order. The merge is still *permitted* if some other overlap (or a
//!     dependency) justifies it; it is just never *rewarded* for the conflict.
//!
//! This keeps the heuristic simple — overlap stays a single scalar score — while
//! preserving the role partition that spec v0.1 §1.2 makes central. The solver
//! (M3.3) refines this into a first-class write-conflict penalty on the
//! connectivity objective.
//!
//! ## Determinism
//!
//! The merge order is fully deterministic. Pairs are scored once; the
//! highest-overlap eligible pair is merged each round; ties break on the
//! lexicographically-smallest `(min task id, max task id)` pair. With a fixed
//! input the output slices, their unioned closures, and their costs are
//! identical on every run.
//!
//! ## Pure module
//!
//! No DB access, no I/O, no global mutable state. The caller loads each task's
//! effective closure (via `engine/closure/store.show` over the `closures` table
//! / the M2 walk+weight surface) and the task dependency edges (the
//! `entity_links` `blocks` edges as `strategy.zig` reads them), then hands them
//! in as plain slices. Every function is allocator-explicit.

const std = @import("std");

// ---------------------------------------------------------------------------
// Input model
// ---------------------------------------------------------------------------

/// The role a unit plays in a task's closure (mirrors `closure.walk.Role`,
/// restated here so the grouping module does not depend on the extractor).
/// Only the effective roles (`modify`, `reference`) are relevant to grouping;
/// transitive units are excluded from the effective closure upstream.
pub const Role = enum { modify, reference };

/// One unit of a task's effective closure: a qualified symbol, the role the
/// task plays on it, and its token weight `w(u)` (spec v0.1 §2).
///
/// `qualified` and the unit's identity is the symbol name; two units with the
/// same `qualified` in different tasks denote the *same* context unit (the
/// thing co-location lets a slice pay for once).
pub const Unit = struct {
    /// Qualified symbol name (e.g. `widget.Foo.bar`). Borrowed; must outlive
    /// the `group` call.
    qualified: []const u8,
    /// The role the owning task plays on this unit.
    role: Role,
    /// Token weight of the unit (`w(u)`), counted once per distinct symbol in
    /// a unioned closure.
    weight: u32,
};

/// One task to be grouped: its id and its effective closure.
pub const Task = struct {
    /// Stable task id (the `tasks.id` the caller pulled from the plan).
    id: i64,
    /// The task's effective-closure units (modify ∪ reference). Borrowed; must
    /// outlive the `group` call. A unit appearing under both roles SHOULD be
    /// presented once with role `modify` (modify dominates for conflict
    /// detection); duplicates of the same `qualified` are tolerated and folded.
    units: []const Unit,
};

/// A dependency edge in the task DAG, in the `entity_links` `blocks` sense:
/// `blocked` is blocked BY `blocker` — i.e. `blocker` must complete before
/// `blocked` (scheduling order `blocker → blocked`). This matches
/// `strategy.zig`'s reading of a `task -[blocks]-> task` edge (from_id is
/// blocked by to_id).
pub const Dep = struct {
    /// The task that cannot start until `blocker` is done.
    blocked: i64,
    /// The task that must complete first.
    blocker: i64,
};

// ---------------------------------------------------------------------------
// Output model
// ---------------------------------------------------------------------------

/// A formed slice: the set of task ids co-located in one context window, plus
/// its unioned effective closure and the total token cost of that union.
pub const Slice = struct {
    /// Member task ids, sorted ascending for deterministic output.
    task_ids: []const i64,
    /// The distinct qualified symbols of the slice's unioned closure, sorted.
    union_symbols: []const []const u8,
    /// `Cost = Σ_u w(u)` over the unioned closure (each distinct unit once).
    cost: u32,

    pub fn deinit(self: Slice, a: std.mem.Allocator) void {
        a.free(self.task_ids);
        for (self.union_symbols) |s| a.free(s);
        a.free(self.union_symbols);
    }
};

/// The grouping result: the formed slices. Caller owns it; call `deinit`.
pub const Grouping = struct {
    slices: []const Slice,

    pub fn deinit(self: Grouping, a: std.mem.Allocator) void {
        for (self.slices) |s| s.deinit(a);
        a.free(self.slices);
    }

    /// Total replication-inclusive cost across all slices (Σ over slices of the
    /// slice's unioned-closure cost). The metric the grouped arm reports.
    pub fn totalCost(self: Grouping) u64 {
        var total: u64 = 0;
        for (self.slices) |s| total += s.cost;
        return total;
    }
};

pub const Error = std.mem.Allocator.Error;

// ---------------------------------------------------------------------------
// Internal working state
// ---------------------------------------------------------------------------

/// A slice under construction. `members` are task ids; `union_*` is the
/// running unioned closure, kept current after every merge so the budget check
/// is exact. All storage is arena-backed.
const WorkSlice = struct {
    members: std.ArrayListUnmanaged(i64),
    /// qualified symbol → its (max-seen) weight. The union dedupes by symbol;
    /// a symbol's weight is a fixed property of the source, so any task's copy
    /// agrees, but we keep the max defensively.
    union_w: std.StringHashMapUnmanaged(u32),
    /// qualified symbol → true iff some member task lists it as `modify`. Used
    /// for write-write conflict detection in the overlap score.
    writers: std.StringHashMapUnmanaged(void),
    /// Running cost = Σ over union_w values. Maintained incrementally.
    cost: u32,
    /// True once this slice has been merged into another (tombstone).
    dead: bool,
};

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

/// Greedily groups `tasks` into slices under window budget `budget`, honoring
/// the dependency `deps` (D-HG2 ordering) and the role asymmetry (D-HG1).
///
/// Algorithm:
///   1. Each task starts as its own slice (its closure is its union).
///      A task whose own closure already exceeds `budget` stays a singleton
///      (it cannot be made smaller — the budget is a constraint on the slice,
///      and a lone over-budget task is reported as-is rather than dropped).
///   2. Repeatedly: score every pair of live slices by overlap (shared union
///      symbols, weighted, write-write conflicts earning zero — see D-HG1).
///      Pick the highest-scoring pair that (a) keeps the merged union ≤ budget
///      and (b) does not induce an inter-slice cycle (D-HG2). Merge it.
///   3. Stop when no eligible merge remains.
///
/// The caller owns the returned `Grouping` and must `deinit` it.
pub fn group(
    gpa: std.mem.Allocator,
    tasks: []const Task,
    deps: []const Dep,
    budget: u32,
) Error!Grouping {
    var arena = std.heap.ArenaAllocator.init(gpa);
    defer arena.deinit();
    const a = arena.allocator();

    // --- 1. Seed one slice per task -------------------------------------
    var slices = try a.alloc(WorkSlice, tasks.len);
    for (tasks, 0..) |t, i| {
        var ws: WorkSlice = .{
            .members = .empty,
            .union_w = .empty,
            .writers = .empty,
            .cost = 0,
            .dead = false,
        };
        try ws.members.append(a, t.id);
        for (t.units) |u| {
            try addUnit(a, &ws, u);
        }
        slices[i] = ws;
    }

    // --- 2. Greedy merge rounds -----------------------------------------
    // The dependency relation is materialized as a per-task adjacency we can
    // query for "does merging these two slices create a cycle among slices?"
    // We track, per live slice, the set of slices it must precede (out) so a
    // merge that would close a precedence loop is refused.
    while (true) {
        const best = try bestMerge(a, slices, tasks, deps, budget);
        if (best == null) break;
        const m = best.?;
        try mergeInto(a, &slices[m.lo], &slices[m.hi]);
        slices[m.hi].dead = true;
    }

    // --- 3. Materialize the result -------------------------------------
    var out: std.ArrayListUnmanaged(Slice) = .empty;
    errdefer {
        for (out.items) |s| s.deinit(gpa);
        out.deinit(gpa);
    }
    for (slices) |ws| {
        if (ws.dead) continue;
        const slice = try finalizeSlice(gpa, ws);
        try out.append(gpa, slice);
    }

    // Deterministic slice ordering: by smallest member id.
    const owned = try out.toOwnedSlice(gpa);
    std.mem.sort(Slice, owned, {}, sliceLess);
    return .{ .slices = owned };
}

fn sliceLess(_: void, lhs: Slice, rhs: Slice) bool {
    return lhs.task_ids[0] < rhs.task_ids[0];
}

// ---------------------------------------------------------------------------
// Merge selection
// ---------------------------------------------------------------------------

const Candidate = struct {
    /// Index into the slices array of the slice that survives the merge.
    lo: usize,
    /// Index of the slice that is absorbed (tombstoned).
    hi: usize,
    /// Overlap score (weighted shared symbols, write-write conflicts excluded).
    score: u64,
    /// Tie-break key: the smallest member id across the two slices.
    tie_min: i64,
    /// Secondary tie-break: the larger of the two slices' smallest member ids.
    tie_max: i64,
};

/// Scans every live pair and returns the best eligible merge, or null when no
/// merge is eligible. Eligibility: merged union ≤ budget AND no induced
/// inter-slice cycle. Selection: highest overlap score, then deterministic
/// tie-break on member ids.
fn bestMerge(
    a: std.mem.Allocator,
    slices: []WorkSlice,
    tasks: []const Task,
    deps: []const Dep,
    budget: u32,
) Error!?Candidate {
    var best: ?Candidate = null;
    var i: usize = 0;
    while (i < slices.len) : (i += 1) {
        if (slices[i].dead) continue;
        var j: usize = i + 1;
        while (j < slices.len) : (j += 1) {
            if (slices[j].dead) continue;

            // Budget: compute the merged union cost directly (dedup shared).
            const merged_cost = unionCost(slices[i], slices[j]);
            if (merged_cost > budget) continue;

            // Ordering (D-HG2): refuse if merging closes a precedence cycle.
            if (try mergeInducesCycle(a, slices, tasks, deps, i, j)) continue;

            const score = overlapScore(slices[i], slices[j]);

            // A merge with zero overlap is still allowed (it may be a
            // dependency co-location), but only taken if nothing better
            // exists; score 0 candidates are eligible and compete on
            // tie-break. We require an actual overlap OR a direct dependency
            // to avoid arbitrarily fusing unrelated tasks (which would only
            // inflate a slice's cost with no benefit).
            const has_dep = pairHasDep(slices[i], slices[j], deps);
            if (score == 0 and !has_dep) continue;

            const lo_min = minMember(slices[i]);
            const hi_min = minMember(slices[j]);
            const tie_min = @min(lo_min, hi_min);
            const tie_max = @max(lo_min, hi_min);

            const cand: Candidate = .{
                .lo = i,
                .hi = j,
                .score = score,
                .tie_min = tie_min,
                .tie_max = tie_max,
            };
            if (best == null or candidateBetter(cand, best.?)) {
                best = cand;
            }
        }
    }
    return best;
}

/// Strict "is `x` a better pick than `y`" — higher score wins; ties break on
/// the smaller `tie_min`, then the smaller `tie_max`. Total order ⇒ stable.
fn candidateBetter(x: Candidate, y: Candidate) bool {
    if (x.score != y.score) return x.score > y.score;
    if (x.tie_min != y.tie_min) return x.tie_min < y.tie_min;
    return x.tie_max < y.tie_max;
}

/// Overlap score of two slices: Σ over symbols present in BOTH slices' unions
/// of the symbol weight, EXCEPT a symbol that is a `modify` in both slices
/// (a write-write conflict) contributes ZERO (D-HG1).
fn overlapScore(x: WorkSlice, y: WorkSlice) u64 {
    var score: u64 = 0;
    var it = x.union_w.iterator();
    while (it.next()) |e| {
        const sym = e.key_ptr.*;
        if (y.union_w.get(sym)) |wy| {
            // write-write conflict: both slices write this symbol → no credit.
            if (x.writers.contains(sym) and y.writers.contains(sym)) continue;
            // shared read / read-write: credit the (agreed) weight once.
            score += @max(e.value_ptr.*, wy);
        }
    }
    return score;
}

/// Cost of the union of two slices' closures (dedup shared symbols).
fn unionCost(x: WorkSlice, y: WorkSlice) u32 {
    var total: u32 = x.cost;
    var it = y.union_w.iterator();
    while (it.next()) |e| {
        if (!x.union_w.contains(e.key_ptr.*)) total += e.value_ptr.*;
    }
    return total;
}

fn minMember(ws: WorkSlice) i64 {
    var m: i64 = ws.members.items[0];
    for (ws.members.items) |id| m = @min(m, id);
    return m;
}

/// True if any dependency edge connects a member of `x` to a member of `y`
/// (either direction).
fn pairHasDep(x: WorkSlice, y: WorkSlice, deps: []const Dep) bool {
    for (deps) |d| {
        const x_blocked = memberOf(x, d.blocked);
        const x_blocker = memberOf(x, d.blocker);
        const y_blocked = memberOf(y, d.blocked);
        const y_blocker = memberOf(y, d.blocker);
        if ((x_blocked and y_blocker) or (x_blocker and y_blocked)) return true;
    }
    return false;
}

fn memberOf(ws: WorkSlice, id: i64) bool {
    for (ws.members.items) |m| if (m == id) return true;
    return false;
}

// ---------------------------------------------------------------------------
// D-HG2 — schedulable slice-DAG guarantee
// ---------------------------------------------------------------------------

/// True iff merging slices `mi` and `mj` would induce a cycle in the
/// slice-level precedence DAG.
///
/// The slice DAG has an edge `S_a → S_b` when some member of `S_a` must
/// complete before some member of `S_b` (a `blocker → blocked` task edge that
/// crosses the two slices). Merging `mi` and `mj` collapses them into one node.
/// That is safe iff there is no *third* slice `S_k` such that the merged node
/// both precedes and follows `S_k` (which would be a 2-cycle through `S_k`),
/// and more generally iff the contracted slice graph stays acyclic.
///
/// We test it directly: build the post-merge slice precedence graph (treating
/// `mi`/`mj` as one node) and run a cycle check. This is O(slices · deps) per
/// candidate — fine for the plan sizes greedy targets, and exact (no
/// false-accept). Ordering wins over replication, so any doubt rejects.
fn mergeInducesCycle(
    a: std.mem.Allocator,
    slices: []WorkSlice,
    tasks: []const Task,
    deps: []const Dep,
    mi: usize,
    mj: usize,
) Error!bool {
    _ = tasks;
    // Assign each live slice a node id; mi and mj share a node (the merge).
    // node_of[k] = representative node index for slice k.
    var node_of = try a.alloc(usize, slices.len);
    defer a.free(node_of);
    for (slices, 0..) |ws, k| {
        if (ws.dead) {
            node_of[k] = std.math.maxInt(usize);
            continue;
        }
        node_of[k] = if (k == mj) mi else k;
    }

    // Map a task id to the slice index owning it.
    // (Linear scan; member sets are small.)
    const sliceOfTask = struct {
        fn f(sl: []WorkSlice, id: i64) ?usize {
            for (sl, 0..) |ws, k| {
                if (ws.dead) continue;
                for (ws.members.items) |m| if (m == id) return k;
            }
            return null;
        }
    }.f;

    // Build adjacency over node ids from the dependency edges.
    // Edge blocker → blocked (blocker precedes blocked).
    var adj: std.AutoHashMapUnmanaged(usize, std.AutoHashMapUnmanaged(usize, void)) = .empty;
    for (deps) |d| {
        const sb = sliceOfTask(slices, d.blocker) orelse continue;
        const st = sliceOfTask(slices, d.blocked) orelse continue;
        const nb = node_of[sb];
        const nt = node_of[st];
        if (nb == nt) continue; // intra-node edge: no slice-DAG edge.
        const gop = try adj.getOrPut(a, nb);
        if (!gop.found_existing) gop.value_ptr.* = .empty;
        try gop.value_ptr.put(a, nt, {});
    }

    // Cycle detection via DFS three-color marking over the node set.
    var color: std.AutoHashMapUnmanaged(usize, u8) = .empty; // 0=white,1=gray,2=black
    var it = adj.iterator();
    var roots: std.ArrayListUnmanaged(usize) = .empty;
    while (it.next()) |e| try roots.append(a, e.key_ptr.*);
    for (roots.items) |r| {
        if ((color.get(r) orelse 0) == 0) {
            if (try hasCycleFrom(a, &adj, &color, r)) return true;
        }
    }
    return false;
}

fn hasCycleFrom(
    a: std.mem.Allocator,
    adj: *std.AutoHashMapUnmanaged(usize, std.AutoHashMapUnmanaged(usize, void)),
    color: *std.AutoHashMapUnmanaged(usize, u8),
    node: usize,
) Error!bool {
    try color.put(a, node, 1); // gray
    if (adj.get(node)) |neighbors| {
        var nit = neighbors.iterator();
        while (nit.next()) |ne| {
            const nb = ne.key_ptr.*;
            const c = color.get(nb) orelse 0;
            if (c == 1) return true; // back edge → cycle
            if (c == 0) {
                if (try hasCycleFrom(a, adj, color, nb)) return true;
            }
        }
    }
    try color.put(a, node, 2); // black
    return false;
}

// ---------------------------------------------------------------------------
// Slice mutation
// ---------------------------------------------------------------------------

/// Adds one unit to a work slice's running union, updating cost + writer set.
fn addUnit(a: std.mem.Allocator, ws: *WorkSlice, u: Unit) Error!void {
    const gop = try ws.union_w.getOrPut(a, u.qualified);
    if (!gop.found_existing) {
        gop.value_ptr.* = u.weight;
        ws.cost += u.weight;
    } else if (u.weight > gop.value_ptr.*) {
        // Same symbol, larger weight seen (defensive): adjust cost.
        ws.cost += u.weight - gop.value_ptr.*;
        gop.value_ptr.* = u.weight;
    }
    if (u.role == .modify) try ws.writers.put(a, u.qualified, {});
}

/// Merges `src` into `dst`: unions closures, members, and writer sets, keeping
/// `dst.cost` exact.
fn mergeInto(a: std.mem.Allocator, dst: *WorkSlice, src: *WorkSlice) Error!void {
    for (src.members.items) |id| try dst.members.append(a, id);
    var it = src.union_w.iterator();
    while (it.next()) |e| {
        const gop = try dst.union_w.getOrPut(a, e.key_ptr.*);
        if (!gop.found_existing) {
            gop.value_ptr.* = e.value_ptr.*;
            dst.cost += e.value_ptr.*;
        } else if (e.value_ptr.* > gop.value_ptr.*) {
            dst.cost += e.value_ptr.* - gop.value_ptr.*;
            gop.value_ptr.* = e.value_ptr.*;
        }
    }
    var wit = src.writers.iterator();
    while (wit.next()) |e| try dst.writers.put(a, e.key_ptr.*, {});
}

/// Produces the public `Slice` from a finished work slice, copying its
/// owned data out of the arena into `gpa`.
fn finalizeSlice(gpa: std.mem.Allocator, ws: WorkSlice) Error!Slice {
    const ids = try gpa.dupe(i64, ws.members.items);
    errdefer gpa.free(ids);
    std.mem.sort(i64, ids, {}, lessI64);

    var syms: std.ArrayListUnmanaged([]const u8) = .empty;
    errdefer {
        for (syms.items) |s| gpa.free(s);
        syms.deinit(gpa);
    }
    var it = ws.union_w.iterator();
    while (it.next()) |e| try syms.append(gpa, try gpa.dupe(u8, e.key_ptr.*));
    const sym_slice = try syms.toOwnedSlice(gpa);
    std.mem.sort([]const u8, sym_slice, {}, lessStr);

    return .{ .task_ids = ids, .union_symbols = sym_slice, .cost = ws.cost };
}

fn lessI64(_: void, lhs: i64, rhs: i64) bool {
    return lhs < rhs;
}

fn lessStr(_: void, lhs: []const u8, rhs: []const u8) bool {
    return std.mem.order(u8, lhs, rhs) == .lt;
}

// ===========================================================================
// Tests
// ===========================================================================

const testing = std.testing;

/// Builds a Task from a set of (symbol, role, weight) literals.
fn mkTask(id: i64, units: []const Unit) Task {
    return .{ .id = id, .units = units };
}

fn findSliceWith(g: Grouping, task_id: i64) ?Slice {
    for (g.slices) |s| {
        for (s.task_ids) |t| if (t == task_id) return s;
    }
    return null;
}

fn sliceContainsTask(s: Slice, id: i64) bool {
    for (s.task_ids) |t| if (t == id) return true;
    return false;
}

test "greedy: overlap-driven — heavily-overlapping pair merges, disjoint stay separate" {
    const a = testing.allocator;

    // Task 1 and Task 2 share symbols `shared.a`/`shared.b` (heavy overlap).
    // Task 3 is disjoint. With a generous budget, 1+2 merge; 3 stays alone.
    const t1_units = [_]Unit{
        .{ .qualified = "shared.a", .role = .reference, .weight = 50 },
        .{ .qualified = "shared.b", .role = .reference, .weight = 50 },
        .{ .qualified = "t1.own", .role = .modify, .weight = 10 },
    };
    const t2_units = [_]Unit{
        .{ .qualified = "shared.a", .role = .reference, .weight = 50 },
        .{ .qualified = "shared.b", .role = .reference, .weight = 50 },
        .{ .qualified = "t2.own", .role = .modify, .weight = 10 },
    };
    const t3_units = [_]Unit{
        .{ .qualified = "lonely.x", .role = .modify, .weight = 10 },
        .{ .qualified = "lonely.y", .role = .reference, .weight = 10 },
    };
    const tasks = [_]Task{
        mkTask(1, &t1_units),
        mkTask(2, &t2_units),
        mkTask(3, &t3_units),
    };

    var g = try group(a, &tasks, &.{}, 1000);
    defer g.deinit(a);

    // 1 and 2 co-located.
    const s12 = findSliceWith(g, 1).?;
    try testing.expect(sliceContainsTask(s12, 2));
    // 3 is alone.
    const s3 = findSliceWith(g, 3).?;
    try testing.expect(!sliceContainsTask(s3, 1));
    try testing.expect(!sliceContainsTask(s3, 2));
    try testing.expectEqual(@as(usize, 1), s3.task_ids.len);

    // Two slices total: {1,2} and {3}.
    try testing.expectEqual(@as(usize, 2), g.slices.len);

    // Unioned cost of {1,2}: shared.a(50)+shared.b(50)+t1.own(10)+t2.own(10)
    // = 120 (shared paid once).
    try testing.expectEqual(@as(u32, 120), s12.cost);
}

test "greedy: respects budget — a merge that would exceed B is split" {
    const a = testing.allocator;

    // Each task has a unique heavy symbol; only a small symbol is shared.
    // Merging any pair would push union past the budget, so each stays alone.
    const t1_units = [_]Unit{
        .{ .qualified = "shared.tiny", .role = .reference, .weight = 5 },
        .{ .qualified = "t1.heavy", .role = .modify, .weight = 60 },
    };
    const t2_units = [_]Unit{
        .{ .qualified = "shared.tiny", .role = .reference, .weight = 5 },
        .{ .qualified = "t2.heavy", .role = .modify, .weight = 60 },
    };
    const tasks = [_]Task{
        mkTask(1, &t1_units),
        mkTask(2, &t2_units),
    };

    // Budget 100: a merged union would be 5 + 60 + 60 = 125 > 100. Refuse.
    var g = try group(a, &tasks, &.{}, 100);
    defer g.deinit(a);

    try testing.expectEqual(@as(usize, 2), g.slices.len);
    for (g.slices) |s| {
        try testing.expectEqual(@as(usize, 1), s.task_ids.len);
        try testing.expect(s.cost <= 100);
    }

    // With a budget that fits the union (125), they DO merge — proving the
    // split above was the budget, not a missing overlap.
    var g2 = try group(a, &tasks, &.{}, 130);
    defer g2.deinit(a);
    try testing.expectEqual(@as(usize, 1), g2.slices.len);
    try testing.expectEqual(@as(u32, 125), g2.slices[0].cost);
}

test "greedy: never groups across a dependency violation (no inter-slice cycle)" {
    const a = testing.allocator;

    // A diamond with a cross dependency that, if two slices are formed the
    // wrong way, would be cyclic. Tasks: 1 → 2 → 3, and 1 → 3 (1 before 3).
    // Symbols are arranged so the highest overlap is between 1 and 3, but
    // 2 sits between them in the order. Whatever greedy does, the resulting
    // slice-DAG must stay acyclic.
    const t1_units = [_]Unit{
        .{ .qualified = "core.api", .role = .reference, .weight = 40 },
        .{ .qualified = "t1.own", .role = .modify, .weight = 10 },
    };
    const t2_units = [_]Unit{
        .{ .qualified = "mid.thing", .role = .modify, .weight = 40 },
        .{ .qualified = "t2.own", .role = .reference, .weight = 10 },
    };
    const t3_units = [_]Unit{
        .{ .qualified = "core.api", .role = .reference, .weight = 40 },
        .{ .qualified = "t3.own", .role = .modify, .weight = 10 },
    };
    const tasks = [_]Task{
        mkTask(1, &t1_units),
        mkTask(2, &t2_units),
        mkTask(3, &t3_units),
    };
    // 2 blocked by 1; 3 blocked by 2; 3 blocked by 1.
    const deps = [_]Dep{
        .{ .blocked = 2, .blocker = 1 },
        .{ .blocked = 3, .blocker = 2 },
        .{ .blocked = 3, .blocker = 1 },
    };

    var g = try group(a, &tasks, &deps, 1000);
    defer g.deinit(a);

    // Assert the produced slice-DAG is schedulable: build slice precedence
    // from deps and verify it is acyclic.
    try testing.expect(try schedulable(a, g, &deps));
}

test "greedy: dependency-chain merge stays schedulable when co-located" {
    const a = testing.allocator;
    // 1 → 2 (2 blocked by 1) with heavy overlap; they merge into one slice.
    // A single slice holding both is trivially schedulable (intra-slice).
    const t1_units = [_]Unit{
        .{ .qualified = "shared.api", .role = .reference, .weight = 50 },
        .{ .qualified = "t1.own", .role = .modify, .weight = 10 },
    };
    const t2_units = [_]Unit{
        .{ .qualified = "shared.api", .role = .reference, .weight = 50 },
        .{ .qualified = "t2.own", .role = .modify, .weight = 10 },
    };
    const tasks = [_]Task{ mkTask(1, &t1_units), mkTask(2, &t2_units) };
    const deps = [_]Dep{.{ .blocked = 2, .blocker = 1 }};

    var g = try group(a, &tasks, &deps, 1000);
    defer g.deinit(a);

    try testing.expectEqual(@as(usize, 1), g.slices.len);
    try testing.expect(sliceContainsTask(g.slices[0], 1));
    try testing.expect(sliceContainsTask(g.slices[0], 2));
    try testing.expect(try schedulable(a, g, &deps));
}

test "greedy: D-HG1 — write-write conflict earns no overlap credit" {
    const a = testing.allocator;

    // Task 1 and 2 both MODIFY `hot.symbol` (write-write conflict) and share
    // nothing else. Task 1 and 3 share `cool.symbol` as references (read-read).
    // The read-read pair must merge first (the conflict pair earns 0 credit);
    // with a budget that only fits ONE merge, the read pair wins.
    const t1_units = [_]Unit{
        .{ .qualified = "hot.symbol", .role = .modify, .weight = 30 },
        .{ .qualified = "cool.symbol", .role = .reference, .weight = 30 },
        .{ .qualified = "t1.own", .role = .modify, .weight = 5 },
    };
    const t2_units = [_]Unit{
        .{ .qualified = "hot.symbol", .role = .modify, .weight = 30 },
        .{ .qualified = "t2.own", .role = .modify, .weight = 5 },
    };
    const t3_units = [_]Unit{
        .{ .qualified = "cool.symbol", .role = .reference, .weight = 30 },
        .{ .qualified = "t3.own", .role = .modify, .weight = 5 },
    };
    const tasks = [_]Task{
        mkTask(1, &t1_units),
        mkTask(2, &t2_units),
        mkTask(3, &t3_units),
    };

    // Budget tuned so only one merge fits: {1,3} union =
    //   hot(30)+cool(30)+t1.own(5)+t3.own(5) = 70.
    // {1,2} union = hot(30)+cool(30)+t1.own(5)+t2.own(5) = 70 too, but its
    // overlap SCORE is 0 (hot is write-write), so {1,3} (score 30) is picked.
    // After merging {1,3} (cost 70), merging 2 in would exceed budget 70.
    var g = try group(a, &tasks, &.{}, 70);
    defer g.deinit(a);

    const s1 = findSliceWith(g, 1).?;
    // 1 merged with 3 (read overlap), NOT with 2 (write-write).
    try testing.expect(sliceContainsTask(s1, 3));
    try testing.expect(!sliceContainsTask(s1, 2));

    const s2 = findSliceWith(g, 2).?;
    try testing.expectEqual(@as(usize, 1), s2.task_ids.len);
}

test "greedy: deterministic — same input yields identical slices across runs" {
    const a = testing.allocator;
    const t1_units = [_]Unit{
        .{ .qualified = "s.a", .role = .reference, .weight = 20 },
        .{ .qualified = "s.b", .role = .reference, .weight = 20 },
    };
    const t2_units = [_]Unit{
        .{ .qualified = "s.a", .role = .reference, .weight = 20 },
        .{ .qualified = "s.b", .role = .reference, .weight = 20 },
    };
    const t3_units = [_]Unit{
        .{ .qualified = "s.a", .role = .reference, .weight = 20 },
        .{ .qualified = "s.b", .role = .reference, .weight = 20 },
    };
    const tasks = [_]Task{
        mkTask(10, &t1_units),
        mkTask(20, &t2_units),
        mkTask(30, &t3_units),
    };

    // All three have identical closures; ties must resolve deterministically.
    var g1 = try group(a, &tasks, &.{}, 1000);
    defer g1.deinit(a);
    var g2 = try group(a, &tasks, &.{}, 1000);
    defer g2.deinit(a);

    try testing.expectEqual(g1.slices.len, g2.slices.len);
    for (g1.slices, g2.slices) |s1, s2| {
        try testing.expectEqualSlices(i64, s1.task_ids, s2.task_ids);
        try testing.expectEqual(s1.cost, s2.cost);
    }

    // With a budget fitting all three (union = 40), they form ONE slice.
    try testing.expectEqual(@as(usize, 1), g1.slices.len);
    try testing.expectEqual(@as(u32, 40), g1.slices[0].cost);
    try testing.expectEqualSlices(i64, &.{ 10, 20, 30 }, g1.slices[0].task_ids);
}

test "greedy: lone over-budget task is reported as its own slice" {
    const a = testing.allocator;
    const big = [_]Unit{
        .{ .qualified = "huge.symbol", .role = .modify, .weight = 500 },
    };
    const small = [_]Unit{
        .{ .qualified = "tiny.symbol", .role = .modify, .weight = 5 },
    };
    const tasks = [_]Task{ mkTask(1, &big), mkTask(2, &small) };
    var g = try group(a, &tasks, &.{}, 100);
    defer g.deinit(a);
    // Two slices; the over-budget task stands alone, not dropped.
    try testing.expectEqual(@as(usize, 2), g.slices.len);
    const s1 = findSliceWith(g, 1).?;
    try testing.expectEqual(@as(usize, 1), s1.task_ids.len);
    try testing.expectEqual(@as(u32, 500), s1.cost);
}

// --- test helper: independent schedulability check over the result ---------

/// Verifies the result's slice-precedence DAG (derived from `deps`) is acyclic.
/// Independent of the engine's internal cycle check — a second opinion that the
/// produced grouping is schedulable.
fn schedulable(a: std.mem.Allocator, g: Grouping, deps: []const Dep) !bool {
    // slice index per task.
    var task_slice: std.AutoHashMapUnmanaged(i64, usize) = .empty;
    defer task_slice.deinit(a);
    for (g.slices, 0..) |s, k| {
        for (s.task_ids) |t| try task_slice.put(a, t, k);
    }
    // adjacency: blocker_slice → blocked_slice.
    var adj: std.AutoHashMapUnmanaged(usize, std.AutoHashMapUnmanaged(usize, void)) = .empty;
    defer {
        var it = adj.iterator();
        while (it.next()) |e| e.value_ptr.deinit(a);
        adj.deinit(a);
    }
    for (deps) |d| {
        const sb = task_slice.get(d.blocker) orelse continue;
        const st = task_slice.get(d.blocked) orelse continue;
        if (sb == st) continue;
        const gop = try adj.getOrPut(a, sb);
        if (!gop.found_existing) gop.value_ptr.* = .empty;
        try gop.value_ptr.put(a, st, {});
    }
    // Kahn's algorithm: if we can topologically sort all slices, it's acyclic.
    var indeg: std.AutoHashMapUnmanaged(usize, usize) = .empty;
    defer indeg.deinit(a);
    var k: usize = 0;
    while (k < g.slices.len) : (k += 1) try indeg.put(a, k, 0);
    var ait = adj.iterator();
    while (ait.next()) |e| {
        var nit = e.value_ptr.iterator();
        while (nit.next()) |ne| {
            const cur = indeg.get(ne.key_ptr.*).?;
            try indeg.put(a, ne.key_ptr.*, cur + 1);
        }
    }
    var queue: std.ArrayListUnmanaged(usize) = .empty;
    defer queue.deinit(a);
    var iit = indeg.iterator();
    while (iit.next()) |e| if (e.value_ptr.* == 0) try queue.append(a, e.key_ptr.*);
    var visited: usize = 0;
    while (queue.pop()) |node| {
        visited += 1;
        if (adj.get(node)) |neighbors| {
            var nit = neighbors.iterator();
            while (nit.next()) |ne| {
                const nb = ne.key_ptr.*;
                const cur = indeg.get(nb).?;
                try indeg.put(a, nb, cur - 1);
                if (cur - 1 == 0) try queue.append(a, nb);
            }
        }
    }
    return visited == g.slices.len;
}
