//! engine.grouping.mtkahypar — M3.3a Mt-KaHyPar serialization seam (pure).
//!
//! The **encoding half** of the hypergraph-solver seam (decision D-HG4 /
//! plan-634 decision 520). It turns the SAME input the greedy heuristic
//! consumes — the plan's tasks with their effective closures (`greedy.Task` /
//! `greedy.Unit`) — into the weighted **hMETIS** hypergraph file an external
//! `mtkahypar` binary reads on stdin. This module does NOT spawn the solver,
//! does NOT parse its partition, does NOT repair: those are M3.3b/M3.3c. Its
//! entire deliverable is a deterministic, byte-stable encoder plus its golden
//! test.
//!
//! ## The hypergraph (spec §2 / D-HG1 / D-HG4)
//!
//! We INVERT the per-task closures into a hypergraph whose:
//!
//!   - **Vertices are tasks.** Vertex `i` (1-based, ordered by ascending
//!     `task.id`) has weight = the task's own effective-closure token cost, i.e.
//!     `Σ w(u)` over the task's distinct units. (This mirrors `weight.cost`'s
//!     dedup-by-qualified-symbol rule; the `greedy.Unit` slice is already
//!     folded to one entry per symbol by `load.loadUnits`, so the vertex weight
//!     is a straight sum over the task's units. A symbol's weight is a fixed
//!     property of the source, so a duplicate symbol is summed once defensively.)
//!
//!   - **Hyperedges are context units (qualified symbols).** For each distinct
//!     symbol `u`, the **connectivity hyperedge** pins every task whose
//!     effective closure contains `u`, weighted `w(u)`. Co-locating those tasks
//!     in one block makes the λ−1 connectivity term zero for `u` — exactly the
//!     replication cost spec §2 minimizes.
//!
//! ## D-HG1 (decision 519) — explicit per-role + write-conflict encoding
//!
//! The M2 walk showed write-vs-read on a shared unit is the DOMINANT cross-task
//! sharing shape, not a rarity, so role asymmetry is encoded explicitly rather
//! than flattened. For each distinct symbol `u` we may emit TWO hyperedges:
//!
//!   1. **Connectivity edge** — pins = ALL tasks holding `u` (any role), weight
//!      `w(u)`. The plain replication-minimization signal.
//!   2. **Write-conflict penalty edge** — pins = ONLY the tasks that hold `u`
//!      as role `modify`, weight `penalty_mult * w(u)`. Two tasks that both
//!      WRITE `u` co-located is a merge-collision hazard; this extra edge makes
//!      the solver pay a (low, tunable) surcharge for cutting — i.e. it gently
//!      *prefers* keeping co-modifiers apart relative to a pure read overlap of
//!      the same weight. D-HG4 specifies the capacity is present but the default
//!      is LOW (empirical tuning from the M2 corpus; flattening is the fallback).
//!
//! The penalty multiplier is the named constant `default_write_conflict_mult`
//! and is also a parameter on `EncodeOptions` so M3.3c tuning can sweep it.
//!
//! ## 1-pin hyperedges are dropped
//!
//! A hyperedge with a single pin contributes nothing to λ−1 connectivity (it
//! never crosses a block boundary), so a symbol held by exactly one task emits
//! NO connectivity edge. Likewise a write-conflict edge is emitted only when
//! TWO OR MORE tasks modify the symbol (a lone writer is not a conflict). This
//! keeps the file minimal and the header counts honest.
//!
//! ## hMETIS weighted format
//!
//! Header: `<num_hyperedges> <num_vertices> <fmt>` with `fmt = 11` (the two
//! low bits: bit0 = edge weights present, bit1 = vertex weights present). Then
//! one line per hyperedge: `<edge_weight> <pin> <pin> …` with **1-based** vertex
//! ids (hMETIS convention). Then one line per vertex: `<vertex_weight>`, in
//! vertex-id order. See the Mt-KaHyPar / hMETIS hgr format.
//!
//! ### Deterministic ordering (byte-stable)
//!
//!   - Vertices are numbered 1..N by ascending `task.id`. The numbering map is
//!     fixed before any edge is written.
//!   - Hyperedges are emitted in ascending **symbol** order. For a symbol that
//!     yields both a connectivity and a penalty edge, the connectivity edge is
//!     written first, the penalty edge immediately after.
//!   - Pins within an edge are written in ascending vertex-id (task-id) order.
//!
//! With a fixed input the emitted bytes are identical on every run.
//!
//! ## Pure module
//!
//! No DB, no subprocess, no file I/O. `encode` returns an allocator-owned byte
//! slice; `encodeTo` writes to a caller-supplied writer. Every function is
//! allocator-explicit and has no global mutable state.

const std = @import("std");
const greedy = @import("greedy.zig");

/// Default write-conflict penalty multiplier (D-HG4: "penalty default LOW").
/// The write-conflict hyperedge for a symbol carries weight
/// `default_write_conflict_mult * w(u)`. Kept LOW so the conflict surcharge
/// nudges the partition without dominating the genuine replication objective;
/// M3.3c may sweep this against the M2 corpus.
pub const default_write_conflict_mult: u32 = 1;

/// Knobs for `encode` / `encodeTo`. Defaults reproduce the canonical encoding
/// the golden fixture pins.
pub const EncodeOptions = struct {
    /// Multiplier applied to `w(u)` for a symbol's write-conflict penalty edge.
    /// `0` disables penalty edges entirely (pure connectivity encoding).
    write_conflict_mult: u32 = default_write_conflict_mult,
};

pub const Error = std.mem.Allocator.Error || std.Io.Writer.Error;

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

/// Encodes `tasks` (each carrying its effective-closure `greedy.Unit`s) into a
/// weighted hMETIS hypergraph string and returns it as an allocator-owned byte
/// slice. Caller frees with `gpa.free`.
///
/// The `deps` the greedy solver also consumes are intentionally NOT an argument:
/// hMETIS partitioning is undirected and has no precedence concept. Dependency
/// ordering (D-HG2) is enforced post-partition in M3.3b/M3.3c, not in the
/// encoding.
pub fn encode(
    gpa: std.mem.Allocator,
    tasks: []const greedy.Task,
    opts: EncodeOptions,
) Error![]u8 {
    var aw: std.Io.Writer.Allocating = .init(gpa);
    errdefer aw.deinit();
    try encodeTo(gpa, &aw.writer, tasks, opts);
    return aw.toOwnedSlice();
}

/// Encodes `tasks` into weighted hMETIS form, writing the bytes to `w`. The
/// `scratch` allocator backs transient build state (the symbol index, pin
/// lists, vertex map); it is fully released before return.
pub fn encodeTo(
    scratch: std.mem.Allocator,
    w: *std.Io.Writer,
    tasks: []const greedy.Task,
    opts: EncodeOptions,
) Error!void {
    var arena = std.heap.ArenaAllocator.init(scratch);
    defer arena.deinit();
    const a = arena.allocator();

    // --- 1. Stable 1-based vertex numbering by ascending task id ---------
    const order = try a.alloc(usize, tasks.len);
    for (order, 0..) |*o, i| o.* = i;
    std.mem.sort(usize, order, tasks, struct {
        fn less(ts: []const greedy.Task, x: usize, y: usize) bool {
            return ts[x].id < ts[y].id;
        }
    }.less);
    // task id -> 1-based vertex number.
    var vnum = std.AutoHashMapUnmanaged(i64, u32).empty;
    // 1-based vertex number -> vertex weight (Σ w(u) over the task's units).
    var vweight = try a.alloc(u32, tasks.len);
    for (order, 0..) |ti, k| {
        const v: u32 = @intCast(k + 1);
        try vnum.put(a, tasks[ti].id, v);
        vweight[k] = vertexWeight(tasks[ti]);
    }

    // --- 2. Invert closures into per-symbol pin sets --------------------
    // symbol -> { connectivity pins (all roles), modify-only pins }.
    const PinSet = struct {
        all: std.AutoArrayHashMapUnmanaged(u32, void),
        mod: std.AutoArrayHashMapUnmanaged(u32, void),
        weight: u32,
    };
    var by_sym = std.StringArrayHashMapUnmanaged(PinSet).empty;
    for (tasks) |t| {
        const v = vnum.get(t.id).?;
        // Fold per-task duplicate symbols: a symbol may legitimately appear
        // once (load.loadUnits dedups), but be defensive.
        for (t.units) |u| {
            const gop = try by_sym.getOrPut(a, u.qualified);
            if (!gop.found_existing) {
                gop.value_ptr.* = .{ .all = .empty, .mod = .empty, .weight = u.weight };
            } else if (u.weight > gop.value_ptr.weight) {
                gop.value_ptr.weight = u.weight;
            }
            try gop.value_ptr.all.put(a, v, {});
            if (u.role == .modify) try gop.value_ptr.mod.put(a, v, {});
        }
    }

    // --- 3. Build the hyperedge list in deterministic symbol order ------
    // Sort symbols ascending.
    const syms = try a.dupe([]const u8, by_sym.keys());
    std.mem.sort([]const u8, syms, {}, struct {
        fn less(_: void, x: []const u8, y: []const u8) bool {
            return std.mem.order(u8, x, y) == .lt;
        }
    }.less);

    const Edge = struct { weight: u32, pins: []u32 };
    var edges = std.ArrayListUnmanaged(Edge).empty;
    for (syms) |sym| {
        const ps = by_sym.getPtr(sym).?;
        // (a) Connectivity edge — all pins, weight w(u). Drop 1-pin edges.
        if (ps.all.count() >= 2) {
            const pins = try sortedPins(a, ps.all.keys());
            try edges.append(a, .{ .weight = ps.weight, .pins = pins });
        }
        // (b) Write-conflict penalty edge — modify pins only, weight
        //     mult * w(u). Only when >=2 tasks modify the symbol and the
        //     multiplier is enabled. A lone writer is not a conflict.
        if (opts.write_conflict_mult > 0 and ps.mod.count() >= 2) {
            const pins = try sortedPins(a, ps.mod.keys());
            try edges.append(a, .{
                .weight = ps.weight * opts.write_conflict_mult,
                .pins = pins,
            });
        }
    }

    // --- 4. Serialize -----------------------------------------------------
    // Header: <num_hyperedges> <num_vertices> <fmt=11>.
    try w.print("{d} {d} 11\n", .{ edges.items.len, tasks.len });
    for (edges.items) |e| {
        try w.print("{d}", .{e.weight});
        for (e.pins) |p| try w.print(" {d}", .{p});
        try w.writeByte('\n');
    }
    // One vertex weight per line, in vertex-number order (1..N).
    for (vweight) |vw| try w.print("{d}\n", .{vw});
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

/// A task's own effective-closure token cost: `Σ w(u)` over its distinct units
/// (dedup by qualified symbol, mirroring `weight.cost`). The `greedy.Unit`
/// slice is normally already one-entry-per-symbol; duplicates are folded
/// (max weight) so the sum is stable regardless of input shape.
fn vertexWeight(t: greedy.Task) u32 {
    // Small linear dedup; per-task unit counts are tiny.
    var total: u32 = 0;
    var i: usize = 0;
    outer: while (i < t.units.len) : (i += 1) {
        // Skip if an earlier unit had the same symbol (keep the max weight).
        var j: usize = 0;
        var max_w = t.units[i].weight;
        while (j < i) : (j += 1) {
            if (std.mem.eql(u8, t.units[j].qualified, t.units[i].qualified)) {
                continue :outer; // already counted (at its own / later max)
            }
        }
        // Fold any later duplicates' larger weights into this first occurrence.
        var k: usize = i + 1;
        while (k < t.units.len) : (k += 1) {
            if (std.mem.eql(u8, t.units[k].qualified, t.units[i].qualified)) {
                max_w = @max(max_w, t.units[k].weight);
            }
        }
        total += max_w;
    }
    return total;
}

/// Copies the pin vertex-numbers into an owned slice sorted ascending.
fn sortedPins(a: std.mem.Allocator, keys: []const u32) Error![]u32 {
    const pins = try a.dupe(u32, keys);
    std.mem.sort(u32, pins, {}, struct {
        fn less(_: void, x: u32, y: u32) bool {
            return x < y;
        }
    }.less);
    return pins;
}

// ===========================================================================
// Tests
// ===========================================================================

const testing = std.testing;

fn mkTask(id: i64, units: []const greedy.Unit) greedy.Task {
    return .{ .id = id, .units = units };
}

test "mtkahypar.encode: golden hMETIS fixture (per-role + write-conflict + 1-pin)" {
    const a = testing.allocator;

    // Fixture exercising every encoding branch:
    //
    //   shared.write — task 1 MODIFIES it, task 2 REFERENCES it. A shared unit
    //     that is write-vs-read across tasks (the dominant D-HG1 shape). 2 pins
    //     → connectivity edge; only ONE writer (task 1) → NO penalty edge.
    //   hot.both    — tasks 1 and 3 BOTH MODIFY it. 2 pins → connectivity edge
    //     AND a write-conflict penalty edge over both writers.
    //   read.shared — tasks 2 and 3 both REFERENCE it (read/read). 2 pins →
    //     connectivity edge; zero writers → NO penalty edge.
    //   t1.only     — held only by task 1 (1-pin) → DROPPED, no edge.
    //
    // Vertex (task) weights = Σ w(u) over the task's own units:
    //   task 1: shared.write(10) + hot.both(20) + t1.only(5)      = 35
    //   task 2: shared.write(10) + read.shared(7)                 = 17
    //   task 3: hot.both(20)     + read.shared(7)                 = 27
    const t1 = [_]greedy.Unit{
        .{ .qualified = "shared.write", .role = .modify, .weight = 10 },
        .{ .qualified = "hot.both", .role = .modify, .weight = 20 },
        .{ .qualified = "t1.only", .role = .modify, .weight = 5 },
    };
    const t2 = [_]greedy.Unit{
        .{ .qualified = "shared.write", .role = .reference, .weight = 10 },
        .{ .qualified = "read.shared", .role = .reference, .weight = 7 },
    };
    const t3 = [_]greedy.Unit{
        .{ .qualified = "hot.both", .role = .modify, .weight = 20 },
        .{ .qualified = "read.shared", .role = .reference, .weight = 7 },
    };
    // Deliberately seed out of id order to prove the stable renumbering.
    const tasks = [_]greedy.Task{
        mkTask(3, &t3),
        mkTask(1, &t1),
        mkTask(2, &t2),
    };

    const out = try encode(a, &tasks, .{});
    defer a.free(out);

    // Vertex numbering (ascending id): task1=v1, task2=v2, task3=v3.
    //
    // Hyperedges, in ascending SYMBOL order:
    //   "hot.both"     conn  → weight 20, pins {v1,v3}=1 3
    //   "hot.both"     pen   → weight 20*1=20, pins {v1,v3}=1 3   (both modify)
    //   "read.shared"  conn  → weight 7,  pins {v2,v3}=2 3
    //   "shared.write" conn  → weight 10, pins {v1,v2}=1 2
    //   "t1.only"            → 1-pin, dropped.
    //
    // → num_hyperedges=4, num_vertices=3, fmt=11.
    // Vertex-weight lines (v1,v2,v3): 35, 17, 27.
    const golden =
        "4 3 11\n" ++
        "20 1 3\n" ++ // hot.both connectivity
        "20 1 3\n" ++ // hot.both write-conflict penalty (mult=1)
        "7 2 3\n" ++ // read.shared connectivity (read/read)
        "10 1 2\n" ++ // shared.write connectivity (write-vs-read)
        "35\n" ++ // v1 = task 1
        "17\n" ++ // v2 = task 2
        "27\n"; // v3 = task 3

    try testing.expectEqualStrings(golden, out);
}

test "mtkahypar.encode: deterministic across runs" {
    const a = testing.allocator;
    const t1 = [_]greedy.Unit{
        .{ .qualified = "s.a", .role = .reference, .weight = 9 },
        .{ .qualified = "s.b", .role = .modify, .weight = 4 },
    };
    const t2 = [_]greedy.Unit{
        .{ .qualified = "s.a", .role = .reference, .weight = 9 },
        .{ .qualified = "s.b", .role = .modify, .weight = 4 },
    };
    const tasks = [_]greedy.Task{ mkTask(7, &t1), mkTask(2, &t2) };

    const a1 = try encode(a, &tasks, .{});
    defer a.free(a1);
    const a2 = try encode(a, &tasks, .{});
    defer a.free(a2);
    try testing.expectEqualStrings(a1, a2);

    // Both modify s.b → connectivity + penalty edge; both ref s.a → conn only.
    // vertex numbering: task2=v1, task7=v2.
    const expected =
        "3 2 11\n" ++
        "9 1 2\n" ++ // s.a connectivity
        "4 1 2\n" ++ // s.b connectivity
        "4 1 2\n" ++ // s.b write-conflict penalty
        "13\n" ++ // v1=task2: 9+4
        "13\n"; // v2=task7: 9+4
    try testing.expectEqualStrings(expected, a1);
}

test "mtkahypar.encode: write_conflict_mult tunes / disables penalty edges" {
    const a = testing.allocator;
    const t1 = [_]greedy.Unit{.{ .qualified = "hot", .role = .modify, .weight = 6 }};
    const t2 = [_]greedy.Unit{.{ .qualified = "hot", .role = .modify, .weight = 6 }};
    const tasks = [_]greedy.Task{ mkTask(1, &t1), mkTask(2, &t2) };

    // mult=3 → penalty edge weight = 6*3 = 18.
    const hi = try encode(a, &tasks, .{ .write_conflict_mult = 3 });
    defer a.free(hi);
    try testing.expectEqualStrings(
        "2 2 11\n" ++ "6 1 2\n" ++ "18 1 2\n" ++ "6\n" ++ "6\n",
        hi,
    );

    // mult=0 → penalty edges disabled; only the connectivity edge remains.
    const off = try encode(a, &tasks, .{ .write_conflict_mult = 0 });
    defer a.free(off);
    try testing.expectEqualStrings(
        "1 2 11\n" ++ "6 1 2\n" ++ "6\n" ++ "6\n",
        off,
    );
}

test "mtkahypar.encode: no shared units → no hyperedges, only vertex weights" {
    const a = testing.allocator;
    const t1 = [_]greedy.Unit{.{ .qualified = "a.x", .role = .modify, .weight = 3 }};
    const t2 = [_]greedy.Unit{.{ .qualified = "b.y", .role = .reference, .weight = 4 }};
    const tasks = [_]greedy.Task{ mkTask(1, &t1), mkTask(2, &t2) };
    const out = try encode(a, &tasks, .{});
    defer a.free(out);
    // Both symbols are 1-pin → dropped. Header edges=0; vertex weights remain.
    try testing.expectEqualStrings("0 2 11\n" ++ "3\n" ++ "4\n", out);
}

test "mtkahypar.encodeTo: writer path matches encode" {
    const a = testing.allocator;
    const t1 = [_]greedy.Unit{.{ .qualified = "s", .role = .reference, .weight = 2 }};
    const t2 = [_]greedy.Unit{.{ .qualified = "s", .role = .reference, .weight = 2 }};
    const tasks = [_]greedy.Task{ mkTask(1, &t1), mkTask(2, &t2) };

    var aw: std.Io.Writer.Allocating = .init(a);
    defer aw.deinit();
    try encodeTo(a, &aw.writer, &tasks, .{});

    const via_encode = try encode(a, &tasks, .{});
    defer a.free(via_encode);
    try testing.expectEqualStrings(via_encode, aw.written());
}
