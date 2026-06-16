//! engine.grouping.load — M3.2 data-loading layer for `groups recommend`.
//!
//! The DB-facing bridge between the plan's persisted state (the `closures`
//! table from M2, the `entity_links` `blocks` edges, the open `tasks`) and the
//! PURE `greedy.group` solver (M3.1). It loads three things for a plan and
//! hands them to greedy as plain slices:
//!
//!   1. The plan's OPEN (todo) tasks — the grouping candidate set. Mirrors
//!      `strategy.zig`'s `status = 'todo'` candidate selection so the groups
//!      arm and the eligibility arm agree on which tasks are in play.
//!   2. Each task's EFFECTIVE closure from the `closures` table: rows with
//!      role `modify` / `reference`. `transitive` rows are EXCLUDED (the
//!      effective-closure default, migration 00026 / spec v0.1 §1.2). Each
//!      row becomes a `greedy.Unit{ qualified = symbol, role, weight }`.
//!   3. The task dependency DAG from `entity_links` `blocks` edges.
//!
//! ## Edge-direction mapping (the M3.1 reviewer caveat — load-bearing)
//!
//! `strategy.zig`'s convention is `task -[blocks]-> task` meaning **from_id is
//! blocked BY to_id** (see `task.markBlocked` / strategy rule 1). greedy's
//! `Dep` contract is the OPPOSITE orientation: `Dep{ blocked, blocker }` where
//! `blocker` must complete before `blocked`. So an `entity_links` row
//! `(from_id, to_id, relationship='blocks')` maps to:
//!
//!     Dep{ .blocked = from_id, .blocker = to_id }
//!
//! i.e. **from_id → blocked, to_id → blocker**. Getting this backwards would
//! let greedy's D-HG2 cycle check reason about a flipped DAG and produce an
//! UNSCHEDULABLE grouping. The mapping is verified by a direct unit test
//! (`load.loadDeps: edge-direction — from_id=blocked, to_id=blocker`) that
//! seeds known `entity_links` rows and asserts each produced `Dep` field
//! by value — a flipped mapping would produce `Dep{blocked=to_id,
//! blocker=from_id}` and fail the equality assertion immediately.
//!
//! ## Read-only
//!
//! This module only SELECTs. No writes, no migration. The verb that drives it
//! (`groups recommend`) is read-only, a sibling to `recommend-strategy`.

const std = @import("std");
const db = @import("db");
const greedy = @import("greedy.zig");

pub const Error = error{
    NotFound,
    QueryFailed,
} || std.mem.Allocator.Error;

/// The grouping recommendation for a plan: the formed slices plus the loading
/// stats the caller reports. The arena backing all borrowed data inside the
/// slices (`union_symbols`, etc.) is owned here — call `deinit`.
pub const Recommendation = struct {
    plan_id: i64,
    /// Window budget the grouping was computed under.
    budget: u32,
    /// Open (todo) tasks considered.
    open_tasks: usize,
    /// The greedy result. Borrows nothing from `arena`; it is `gpa`-owned.
    grouping: greedy.Grouping,
    /// Backs every `greedy.Unit`/`Task`/`Dep` slice fed to the solver; freed
    /// on `deinit`. (The greedy result itself is `gpa`-owned and copied out.)
    arena: *std.heap.ArenaAllocator,

    pub fn deinit(self: Recommendation, gpa: std.mem.Allocator) void {
        self.grouping.deinit(gpa);
        self.arena.deinit();
        gpa.destroy(self.arena);
    }
};

/// Load a plan's open tasks + their effective closures + dependency DAG and
/// group them under `budget` via the greedy heuristic (M3.1). Read-only.
///
/// Returns `Error.NotFound` when the plan does not exist. Caller owns the
/// returned `Recommendation` and must call `deinit(gpa)`.
pub fn recommend(
    d: *db.sqlite.Db,
    gpa: std.mem.Allocator,
    plan_id: i64,
    budget: u32,
) Error!Recommendation {
    try ensurePlanExists(d, plan_id);

    const arena_ptr = try gpa.create(std.heap.ArenaAllocator);
    errdefer gpa.destroy(arena_ptr);
    arena_ptr.* = std.heap.ArenaAllocator.init(gpa);
    errdefer arena_ptr.deinit();
    const a = arena_ptr.allocator();

    // 1. Open (todo) task ids — the candidate set (mirrors strategy.zig).
    const task_ids = try loadOpenTaskIds(d, a, plan_id);

    // 2. Per task: its effective closure (modify/reference) as greedy Units.
    var tasks = try a.alloc(greedy.Task, task_ids.len);
    for (task_ids, 0..) |id, i| {
        tasks[i] = .{ .id = id, .units = try loadUnits(d, a, id) };
    }

    // 3. Dependency DAG from entity_links `blocks` edges, restricted to edges
    //    BETWEEN the plan's open tasks (an edge to a done/foreign task does
    //    not constrain the open-task grouping). Edge direction mapped per the
    //    caveat: from_id -> blocked, to_id -> blocker.
    const deps = try loadDeps(d, a, task_ids);

    const grouping = try greedy.group(gpa, tasks, deps, budget);

    return .{
        .plan_id = plan_id,
        .budget = budget,
        .open_tasks = task_ids.len,
        .grouping = grouping,
        .arena = arena_ptr,
    };
}

// =========================================================================
// DB helpers (read-only)
// =========================================================================

fn ensurePlanExists(d: *db.sqlite.Db, plan_id: i64) Error!void {
    var stmt = d.prepare("select count(*) from plans where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
    const n = switch (stmt.step() catch return Error.QueryFailed) {
        .done => @as(i64, 0),
        .row => stmt.columnInt(0),
    };
    if (n == 0) return Error.NotFound;
}

/// The plan's open (todo) task ids, ordered by priority then id — the same
/// candidate selection `strategy.zig` uses for `recommend-strategy`.
fn loadOpenTaskIds(
    d: *db.sqlite.Db,
    a: std.mem.Allocator,
    plan_id: i64,
) Error![]i64 {
    var stmt = d.prepare(
        "select id from tasks where plan_id = ? and status = 'todo' order by priority, id",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(i64) = .empty;
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(a, stmt.columnInt(0)),
        }
    }
    return out.toOwnedSlice(a);
}

/// A task's EFFECTIVE-closure units (role `modify`/`reference`) from the
/// `closures` table. `transitive` rows are excluded (the effective-closure
/// default). Each row → one `greedy.Unit{ qualified = symbol, role, weight }`.
///
/// The `qualified` symbol name is the unit identity greedy dedups on: two
/// tasks listing the same `symbol` denote the same context unit (paid for
/// once in a unioned slice closure). A symbol listed under BOTH roles for a
/// task is folded to a single `modify` unit (modify dominates for the
/// write-conflict detection greedy's D-HG1 does).
fn loadUnits(
    d: *db.sqlite.Db,
    a: std.mem.Allocator,
    task_id: i64,
) Error![]greedy.Unit {
    var stmt = d.prepare(
        \\select symbol, role, token_weight from closures
        \\where task_id = ? and role in ('modify', 'reference')
        \\order by symbol
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;

    // symbol -> index into `out`, so a symbol seen under two roles folds to
    // one unit (modify wins).
    var seen = std.StringHashMapUnmanaged(usize).empty;
    var out: std.ArrayList(greedy.Unit) = .empty;
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const symbol = stmt.columnTextAlloc(0, a) catch return Error.QueryFailed;
                const role_txt = try stmt.columnTextAlloc(1, a);
                const weight: u32 = @intCast(@max(@as(i64, 0), stmt.columnInt(2)));
                const role: greedy.Role = if (std.mem.eql(u8, role_txt, "modify"))
                    .modify
                else
                    .reference;

                if (seen.get(symbol)) |idx| {
                    // Fold: modify dominates; keep the larger weight defensively.
                    if (role == .modify) out.items[idx].role = .modify;
                    if (weight > out.items[idx].weight) out.items[idx].weight = weight;
                } else {
                    try seen.put(a, symbol, out.items.len);
                    try out.append(a, .{ .qualified = symbol, .role = role, .weight = weight });
                }
            },
        }
    }
    return out.toOwnedSlice(a);
}

/// The dependency DAG over the plan's open tasks, read from `entity_links`
/// `blocks` edges. ONLY edges whose BOTH endpoints are in the open-task set
/// are returned (an edge to a done/foreign task does not constrain how the
/// open tasks group). Edge direction mapped per the caveat:
///
///     entity_links(from_id, to_id, 'blocks')  ⇒  Dep{ blocked = from_id,
///                                                      blocker = to_id }
///
/// because `strategy.zig`'s convention is `from_id is blocked BY to_id`,
/// while greedy's `Dep` is `blocker precedes blocked`.
fn loadDeps(
    d: *db.sqlite.Db,
    a: std.mem.Allocator,
    open_ids: []const i64,
) Error![]greedy.Dep {
    // Set membership for the open-task restriction.
    var open = std.AutoHashMapUnmanaged(i64, void).empty;
    for (open_ids) |id| try open.put(a, id, {});

    var stmt = d.prepare(
        \\select from_id, to_id from entity_links
        \\where from_kind = 'task' and to_kind = 'task' and relationship = 'blocks'
    ) catch return Error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(greedy.Dep) = .empty;
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const from_id = stmt.columnInt(0); // blocked
                const to_id = stmt.columnInt(1); // blocker
                if (!open.contains(from_id) or !open.contains(to_id)) continue;
                if (from_id == to_id) continue;
                try out.append(a, .{ .blocked = from_id, .blocker = to_id });
            },
        }
    }
    return out.toOwnedSlice(a);
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

fn seedPlan(d: *db.sqlite.Db) !i64 {
    return d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')",
        &.{},
    );
}

fn seedTask(d: *db.sqlite.Db, plan_id: i64) !i64 {
    return d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 'T', 'todo', 100)",
        &.{.{ .int = plan_id }},
    );
}

fn seedRepo(d: *db.sqlite.Db, slug: []const u8) !i64 {
    return d.execParams(
        "insert into projects (slug, name, root_path) values (?, ?, ?)",
        &.{ .{ .text = slug }, .{ .text = slug }, .{ .text = slug } },
    );
}

fn seedClosure(
    d: *db.sqlite.Db,
    task_id: i64,
    repo_id: i64,
    symbol: []const u8,
    role: []const u8,
    weight: i64,
) !void {
    _ = try d.execParams(
        \\insert into closures
        \\  (task_id, repo_id, path, symbol, role, token_weight, extractor_version)
        \\values (?, ?, ?, ?, ?, ?, 'test')
    , &.{
        .{ .int = task_id },
        .{ .int = repo_id },
        .{ .text = symbol },
        .{ .text = symbol },
        .{ .text = role },
        .{ .int = weight },
    });
}

fn linkBlocks(d: *db.sqlite.Db, from_id: i64, to_id: i64) !void {
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'blocks')",
        &.{ .{ .int = from_id }, .{ .int = to_id } },
    );
}

test "load.loadDeps: edge-direction — from_id=blocked, to_id=blocker (orientation guard)" {
    // This test calls loadDeps DIRECTLY and asserts each Dep field by value.
    // It is the primary orientation guard: if loadDeps were flipped to produce
    // Dep{ .blocked = to_id, .blocker = from_id }, the expectEqual assertions
    // below fail immediately — no amount of merge-symmetry in a higher-level
    // fixture can paper over a wrong field value here.
    //
    // Fixture: two entity_links(from_id=A, to_id=B, 'blocks') rows (with
    // distinct from/to ids), covering both "from < to" and "from > to" orderings
    // so that a transposition cannot accidentally pass both.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan = try seedPlan(&d);
    // Four tasks so we get two dep edges with clearly distinct from/to ids.
    const tA = try seedTask(&d, plan);
    const tB = try seedTask(&d, plan);
    const tC = try seedTask(&d, plan);
    const tD = try seedTask(&d, plan);

    // Edge 1: from=tA, to=tB  → loadDeps must produce Dep{.blocked=tA,.blocker=tB}
    try linkBlocks(&d, tA, tB);
    // Edge 2: from=tD, to=tC  → loadDeps must produce Dep{.blocked=tD,.blocker=tC}
    // (Note from_id > to_id here, ruling out an accidental min/max transposition.)
    try linkBlocks(&d, tD, tC);

    const open = [_]i64{ tA, tB, tC, tD };
    var arena = std.heap.ArenaAllocator.init(a);
    defer arena.deinit();
    const deps = try loadDeps(&d, arena.allocator(), &open);

    // Sort by blocked id for deterministic assertion order.
    std.mem.sort(greedy.Dep, deps, {}, struct {
        fn lessThan(_: void, x: greedy.Dep, y: greedy.Dep) bool {
            return x.blocked < y.blocked;
        }
    }.lessThan);

    try testing.expectEqual(@as(usize, 2), deps.len);

    // Edge 1: from_id=tA is the blocked task; to_id=tB is the blocker.
    try testing.expectEqual(tA, deps[0].blocked);
    try testing.expectEqual(tB, deps[0].blocker);

    // Edge 2: from_id=tD is the blocked task; to_id=tC is the blocker.
    try testing.expectEqual(tD, deps[1].blocked);
    try testing.expectEqual(tC, deps[1].blocker);
}

test "load.recommend: NotFound on a missing plan" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try testing.expectError(Error.NotFound, recommend(&d, a, 99999, 1000));
}

test "load.recommend: overlapping closures co-locate, disjoint stay apart" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan = try seedPlan(&d);
    const r = try seedRepo(&d, "r");
    const t1 = try seedTask(&d, plan);
    const t2 = try seedTask(&d, plan);
    const t3 = try seedTask(&d, plan);

    // t1 & t2 share shared.a/shared.b (heavy overlap); t3 disjoint.
    try seedClosure(&d, t1, r, "shared.a", "reference", 50);
    try seedClosure(&d, t1, r, "shared.b", "reference", 50);
    try seedClosure(&d, t1, r, "t1.own", "modify", 10);
    try seedClosure(&d, t2, r, "shared.a", "reference", 50);
    try seedClosure(&d, t2, r, "shared.b", "reference", 50);
    try seedClosure(&d, t2, r, "t2.own", "modify", 10);
    try seedClosure(&d, t3, r, "lonely.x", "modify", 10);

    var rec = try recommend(&d, a, plan, 1000);
    defer rec.deinit(a);

    try testing.expectEqual(@as(usize, 3), rec.open_tasks);
    try testing.expectEqual(@as(usize, 2), rec.grouping.slices.len);

    // Find the slice holding t1: it must also hold t2, never t3.
    var found = false;
    for (rec.grouping.slices) |s| {
        var has1 = false;
        var has2 = false;
        var has3 = false;
        for (s.task_ids) |id| {
            if (id == t1) has1 = true;
            if (id == t2) has2 = true;
            if (id == t3) has3 = true;
        }
        if (has1) {
            found = true;
            try testing.expect(has2);
            try testing.expect(!has3);
            // Union cost: shared.a(50)+shared.b(50)+t1.own(10)+t2.own(10)=120.
            try testing.expectEqual(@as(u32, 120), s.cost);
        }
    }
    try testing.expect(found);
}

test "load.recommend: every slice respects the budget" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan = try seedPlan(&d);
    const r = try seedRepo(&d, "r");
    const t1 = try seedTask(&d, plan);
    const t2 = try seedTask(&d, plan);

    try seedClosure(&d, t1, r, "shared.tiny", "reference", 5);
    try seedClosure(&d, t1, r, "t1.heavy", "modify", 60);
    try seedClosure(&d, t2, r, "shared.tiny", "reference", 5);
    try seedClosure(&d, t2, r, "t2.heavy", "modify", 60);

    // Budget 100: merged union 125 > 100 → stay split.
    var rec = try recommend(&d, a, plan, 100);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 2), rec.grouping.slices.len);
    for (rec.grouping.slices) |s| try testing.expect(s.cost <= 100);
}

test "load.recommend: blocks edge maps to a schedulable grouping (caveat guard)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan = try seedPlan(&d);
    const r = try seedRepo(&d, "r");
    // Chain: t1 → t2 → t3 (t2 blocked by t1, t3 blocked by t2), plus t1 → t3.
    // High overlap between t1 and t3 (core.api); t2 sits between them.
    const t1 = try seedTask(&d, plan);
    const t2 = try seedTask(&d, plan);
    const t3 = try seedTask(&d, plan);

    try seedClosure(&d, t1, r, "core.api", "reference", 40);
    try seedClosure(&d, t1, r, "t1.own", "modify", 10);
    try seedClosure(&d, t2, r, "mid.thing", "modify", 40);
    try seedClosure(&d, t3, r, "core.api", "reference", 40);
    try seedClosure(&d, t3, r, "t3.own", "modify", 10);

    // entity_links: from is blocked BY to. t2 blocked by t1; t3 by t2; t3 by t1.
    try linkBlocks(&d, t2, t1);
    try linkBlocks(&d, t3, t2);
    try linkBlocks(&d, t3, t1);

    var rec = try recommend(&d, a, plan, 1000);
    defer rec.deinit(a);

    // Verify the produced slice-DAG is schedulable under the known-correct dep
    // orientation (hardcoded here to match the entity_links rows seeded above).
    // NOTE: this schedulability check uses hardcoded-correct dep values and does
    // NOT catch a loadDeps orientation flip on its own — the chain t1→t2→t3
    // produces the same "straddling" cycle for both orientations. The orientation
    // mapping is directly guarded by the
    // "load.loadDeps: edge-direction — from_id=blocked, to_id=blocker" test.
    const deps = [_]greedy.Dep{
        .{ .blocked = t2, .blocker = t1 },
        .{ .blocked = t3, .blocker = t2 },
        .{ .blocked = t3, .blocker = t1 },
    };
    try testing.expect(try schedulable(a, rec.grouping, &deps));
}

test "load.recommend: a dependency edge to a done/foreign task is ignored" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan = try seedPlan(&d);
    const r = try seedRepo(&d, "r");
    const t1 = try seedTask(&d, plan);
    // A done task (not in the open candidate set).
    const done = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 'D', 'done', 100)",
        &.{.{ .int = plan }},
    );
    try seedClosure(&d, t1, r, "t1.own", "modify", 10);
    // Edge from open t1 to a done task — must not appear in the loaded deps.
    try linkBlocks(&d, t1, done);

    var rec = try recommend(&d, a, plan, 1000);
    defer rec.deinit(a);
    // Only the one open task; one singleton slice.
    try testing.expectEqual(@as(usize, 1), rec.open_tasks);
    try testing.expectEqual(@as(usize, 1), rec.grouping.slices.len);
}

/// Independent schedulability check: the result's slice-precedence DAG
/// (derived from `deps`) must be acyclic (Kahn's algorithm).
fn schedulable(a: std.mem.Allocator, g: greedy.Grouping, deps: []const greedy.Dep) !bool {
    var task_slice = std.AutoHashMapUnmanaged(i64, usize).empty;
    defer task_slice.deinit(a);
    for (g.slices, 0..) |s, k| {
        for (s.task_ids) |t| try task_slice.put(a, t, k);
    }
    var adj = std.AutoHashMapUnmanaged(usize, std.AutoHashMapUnmanaged(usize, void)).empty;
    defer {
        var it = adj.iterator();
        while (it.next()) |e| e.value_ptr.deinit(a);
        adj.deinit(a);
    }
    for (deps) |dp| {
        const sb = task_slice.get(dp.blocker) orelse continue;
        const st = task_slice.get(dp.blocked) orelse continue;
        if (sb == st) continue;
        const gop = try adj.getOrPut(a, sb);
        if (!gop.found_existing) gop.value_ptr.* = .empty;
        try gop.value_ptr.put(a, st, {});
    }
    var indeg = std.AutoHashMapUnmanaged(usize, usize).empty;
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
    var queue: std.ArrayList(usize) = .empty;
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
