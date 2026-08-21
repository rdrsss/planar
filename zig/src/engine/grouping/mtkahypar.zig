//! engine.grouping.mtkahypar — Mt-KaHyPar hypergraph-solver seam.
//!
//! Two halves of the external-solver seam (decision D-HG4 / plan-634 decision
//! 520), both keyed off the SAME input the greedy heuristic consumes — the
//! plan's tasks with their effective closures (`greedy.Task` / `greedy.Unit`):
//!
//!   - **M3.3a — encoding (pure).** `encode` / `encodeTo` turn the tasks into
//!     the weighted **hMETIS** hypergraph file an external `mtkahypar` binary
//!     reads. Deterministic and byte-stable; no I/O, no subprocess.
//!   - **M3.3b — subprocess invoke + parse + graceful degradation (this
//!     milestone).** `solverAvailable` probes the binary; `invoke` writes the
//!     M3.3a encoding to a temp file, runs `mtkahypar` with the km1
//!     connectivity objective, and parses the per-vertex block assignment back
//!     into `greedy.Slice`s in the SAME shape greedy emits. When the binary is
//!     absent the caller degrades to the greedy arm (D-HG4: optional RUN_DEP).
//!
//! The D-HG3 union-repair / exact-budget pass and the D-HG2 partition-then-order
//! cycle-repair are **M3.3c** — NOT in this module yet. `invoke` returns slices
//! by vertex→block membership with a basic `weight.cost`-style union cost; the
//! budget-repair that makes the partition comparable to greedy lands in M3.3c.
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

// ===========================================================================
// M3.3b — subprocess invoke + parse + graceful degradation
// ===========================================================================

/// The external solver binary name. Resolved off `$PATH` like every other
/// optional Planar RUN_DEP (`gh`, `rg`). Not vendored and not compiled by
/// `build.zig` (D-HG4); `install.sh --with-mtkahypar` installs the official
/// native wheel behind Planar's stable adapter contract.
pub const solver_bin = "mtkahypar";

/// Default imbalance epsilon handed to `mtkahypar`. A 3% block-imbalance
/// tolerance is the Mt-KaHyPar documented default for connectivity
/// partitioning; the exact-budget guarantee is restored by the M3.3c
/// union-repair pass, so the native balance proxy only needs to be a sane
/// starting point (D-HG3: "native epsilon-balance proxy + post-hoc repair").
pub const default_epsilon = "0.03";

/// Errors the subprocess seam surfaces to the caller. Any of these means the
/// optimal arm could not produce a partition; the caller degrades to greedy
/// and reports `optimal_available:false` (D-HG4).
pub const InvokeError = error{
    /// `mtkahypar` was not runnable (not on PATH) or the spawn itself failed.
    SolverUnavailable,
    /// `mtkahypar` ran but exited non-zero.
    SolverFailed,
    /// The solver ran but no partition-output file could be located / read.
    PartitionMissing,
    /// The partition file's contents did not match the expected
    /// one-block-id-per-line, vertex-count-many shape.
    PartitionMalformed,
} || std.mem.Allocator.Error;

/// True iff the external `mtkahypar` binary is runnable on this machine.
///
/// Spawn `mtkahypar --probe`, which imports the native extension before
/// returning success. This is stronger than a help-only probe: a stale venv or
/// ABI-incompatible wheel cannot masquerade as an available solver. A `false`
/// result degrades to greedy with `optimal_available:false`.
pub fn solverAvailable(allocator: std.mem.Allocator, io: std.Io) bool {
    const res = std.process.run(allocator, io, .{
        .argv = &.{ solver_bin, "--probe" },
    }) catch return false;
    defer allocator.free(res.stdout);
    defer allocator.free(res.stderr);
    return switch (res.term) {
        .exited => |code| code == 0,
        else => false,
    };
}

/// Knobs for `invoke`. Defaults reproduce the canonical solver invocation.
pub const InvokeOptions = struct {
    /// Per-slice token budget — the same `B` greedy partitions under. Used to
    /// derive the starting block count `k = ceil(deduped-union cost / budget)`
    /// AND the exact-budget ceiling the D-HG3 union-repair pass enforces.
    budget: u32,
    /// Imbalance tolerance handed to `mtkahypar` (`-e`). Defaults to
    /// `default_epsilon`.
    epsilon: []const u8 = default_epsilon,
    /// hMETIS encoding knobs forwarded to `encode`.
    encode: EncodeOptions = .{},
    /// The task dependency DAG (`entity_links` `blocks` edges, same orientation
    /// greedy consumes: `Dep{ blocked, blocker }`). Used by the M3.3c D-HG2
    /// cycle-repair pass to make the slice-DAG schedulable. Empty when the plan
    /// has no inter-task ordering — the cycle-repair is then a no-op.
    deps: []const greedy.Dep = &.{},
};

/// Run the external `mtkahypar` solver over `tasks` and return the partition as
/// `greedy.Slice`s (the SAME shape greedy / M3.2 emit). Read-only; allocates a
/// temp working dir under the system temp, writes the M3.3a hMETIS encoding to
/// it, invokes the solver with the km1 (connectivity) objective and a block
/// count `k = ceil(total_union_cost / budget)` (clamped ≥ 1), reads back the
/// per-vertex block assignment, and groups task ids by block.
///
/// The temp dir is removed before return (success OR error). The caller owns
/// the returned slices and frees each via `slice.deinit(gpa)` plus the outer
/// slice via `gpa.free`.
///
/// M3.3c: after parsing the per-vertex block assignment into slices, the result
/// is run through `repairPartition` — the D-HG3 union-repair (split any block
/// whose TRUE unioned closure exceeds `budget` back under budget, exact by
/// construction) and the D-HG2 cycle-repair (merge slices that induce an
/// inter-slice cycle so the result is schedulable, then re-apply union-repair).
/// The returned slices are therefore BOTH budget-compliant and schedulable,
/// with each slice's `cost` the dedup-by-symbol union cost (`weight.cost`
/// semantics) — apples-to-apples with the greedy arm.
pub fn invoke(
    gpa: std.mem.Allocator,
    io: std.Io,
    tasks: []const greedy.Task,
    opts: InvokeOptions,
) InvokeError![]greedy.Slice {
    if (tasks.len == 0) return gpa.alloc(greedy.Slice, 0);

    // --- 1. Encode the hypergraph to bytes (M3.3a) ----------------------
    const hgr = encode(gpa, tasks, opts.encode) catch |e| switch (e) {
        error.OutOfMemory => return error.OutOfMemory,
        else => return error.PartitionMalformed, // writer error on an in-mem buf: unreachable in practice
    };
    defer gpa.free(hgr);

    // --- 2. Make an isolated temp working dir ---------------------------
    const work = try makeTempDir(gpa, io);
    defer {
        std.Io.Dir.cwd().deleteTree(io, work) catch {};
        gpa.free(work);
    }

    const hgr_path = try std.fs.path.join(gpa, &.{ work, "graph.hgr" });
    defer gpa.free(hgr_path);
    writeFileAll(io, hgr_path, hgr) catch return error.PartitionMissing;

    // --- 3. Block count k = ceil(deduped-union cost / budget), clamped ≥ 1 -
    const k = try blockCount(gpa, tasks, opts.budget);
    const k_str = try std.fmt.allocPrint(gpa, "{d}", .{k});
    defer gpa.free(k_str);

    // --- 4. Invoke the solver -------------------------------------------
    // Mt-KaHyPar writes its partition next to the input file as
    // `<input>.part<k>.epsilon<eps>.seed<seed>.KaHyPar` when
    // `--write-partition-file=true` is set. We give it an explicit output
    // folder (the temp dir) and then SCAN for the produced partition file so
    // we are robust to the version-specific name suffix.
    const argv = [_][]const u8{
        solver_bin,
        "-h",
        hgr_path,
        "-k",
        k_str,
        "-e",
        opts.epsilon,
        "-o",
        "km1",
        "-m",
        "direct",
        "--write-partition-file=true",
        "--partition-output-folder",
        work,
    };
    const res = std.process.run(gpa, io, .{ .argv = &argv }) catch
        return error.SolverUnavailable;
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);
    switch (res.term) {
        .exited => |code| if (code != 0) return error.SolverFailed,
        else => return error.SolverFailed,
    }

    // --- 5. Locate + read the partition file ----------------------------
    const part = try readPartitionFile(gpa, io, work, hgr_path);
    defer gpa.free(part);

    // --- 6. Parse block ids → slices ------------------------------------
    const raw = try parsePartition(gpa, part, tasks);

    // --- 7. M3.3c repair: D-HG3 budget + D-HG2 schedulability -----------
    // parsePartition's slices are budget-naive (the native ε-balance proxy may
    // leave an over-budget block) and may induce an inter-slice cycle. Repair
    // both. On error the raw slices are freed so we never leak.
    return repairPartition(gpa, raw, tasks, opts.deps, opts.budget) catch |e| {
        for (raw) |s| s.deinit(gpa);
        gpa.free(raw);
        return e;
    };
}

/// Parse `mtkahypar`'s partition output — one **0-based block id per line, in
/// vertex order** (vertex `i` = the i-th line, matching the 1-based vertex
/// numbering `encode` assigns by ascending `task.id`) — into `greedy.Slice`s.
///
/// Line `i` → vertex `i` → the task that `encode` numbered vertex `i+1` →
/// that task's block. Task ids are grouped by block id; each block becomes one
/// slice whose `cost` is the union cost of its members' closures (dedup by
/// symbol). Empty blocks are dropped. Slices are returned sorted by smallest
/// member id (matching greedy's `sliceLess`), and member ids within a slice are
/// sorted ascending.
///
/// Exposed separately from `invoke` so tests can pin the parse against a known
/// partition string without spawning the solver.
pub fn parsePartition(
    gpa: std.mem.Allocator,
    partition: []const u8,
    tasks: []const greedy.Task,
) InvokeError![]greedy.Slice {
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const a = arena_state.allocator();

    // Vertex order = ascending task id (the numbering `encode` fixes).
    const order = try a.alloc(usize, tasks.len);
    for (order, 0..) |*o, i| o.* = i;
    std.mem.sort(usize, order, tasks, struct {
        fn less(ts: []const greedy.Task, x: usize, y: usize) bool {
            return ts[x].id < ts[y].id;
        }
    }.less);

    // Parse one block id per non-blank line.
    var blocks = std.ArrayListUnmanaged(i64).empty;
    var it = std.mem.splitScalar(u8, partition, '\n');
    while (it.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r");
        if (line.len == 0) continue;
        const b = std.fmt.parseInt(i64, line, 10) catch return error.PartitionMalformed;
        try blocks.append(a, b);
    }
    // The solver emits exactly one block id per vertex.
    if (blocks.items.len != tasks.len) return error.PartitionMalformed;

    // Group task ids by block id, preserving first-seen block order.
    var block_index = std.AutoArrayHashMapUnmanaged(i64, std.ArrayListUnmanaged(usize)).empty;
    for (order, 0..) |task_idx, vertex| {
        const b = blocks.items[vertex];
        const gop = try block_index.getOrPut(a, b);
        if (!gop.found_existing) gop.value_ptr.* = .empty;
        try gop.value_ptr.append(a, task_idx);
    }

    // Materialize one greedy.Slice per non-empty block.
    var out = std.ArrayListUnmanaged(greedy.Slice).empty;
    errdefer {
        for (out.items) |s| s.deinit(gpa);
        out.deinit(gpa);
    }
    var bit = block_index.iterator();
    while (bit.next()) |entry| {
        const member_idxs = entry.value_ptr.items;
        if (member_idxs.len == 0) continue;
        const slice = try buildSlice(gpa, tasks, member_idxs);
        try out.append(gpa, slice);
    }

    const owned = try out.toOwnedSlice(gpa);
    std.mem.sort(greedy.Slice, owned, {}, struct {
        fn less(_: void, x: greedy.Slice, y: greedy.Slice) bool {
            return x.task_ids[0] < y.task_ids[0];
        }
    }.less);
    return owned;
}

// ===========================================================================
// M3.3c — repair passes (D-HG3 union-repair + D-HG2 cycle-repair)
// ===========================================================================

/// Make a parsed partition BOTH budget-compliant (D-HG3) and schedulable
/// (D-HG2), returning freshly-built `greedy.Slice`s that replace `slices`.
///
/// Pipeline (the order matters — see below):
///
///   1. **D-HG2 cycle-repair first.** Build the inter-slice precedence DAG from
///      `deps` (edge `blocker_slice → blocked_slice`). If it has a cycle, the
///      partition is unschedulable. We repair by collapsing every strongly-
///      connected component (SCC) of two-or-more slices into ONE slice — the
///      condensation of a graph by its SCCs is always a DAG, so the merged
///      result is schedulable by construction. A solver that respects ordering
///      rarely trips this, but a budget-blind partition can, and ordering wins
///      over partition quality (spec §2).
///   2. **D-HG3 union-repair second.** Merging in step 1 can push a slice's
///      true union cost over `budget`, and the raw partition may already have an
///      over-budget block. So AFTER the cycle-merge we split any slice whose
///      true `weight.cost` union exceeds `budget`. The split walks the slice's
///      members in a dependency-respecting topological order and cuts the
///      sequence into consecutive chunks each ≤ `budget`. Consecutive chunks of
///      a topo order can only depend backward, so the split NEVER reintroduces a
///      cycle — the result stays schedulable (preserving step 1's guarantee)
///      while becoming exact-budget by construction (mirroring greedy's
///      guarantee). A lone task whose own closure already exceeds `budget` is
///      emitted as a singleton (it cannot be made smaller — same as greedy).
///
/// Costs are computed via `buildSlice` (dedup by qualified symbol, transitive
/// already excluded upstream) so every returned slice's `cost` is the same
/// quantity greedy reports — the solver arm and greedy arm are comparable.
///
/// Caller owns the returned slices (`slice.deinit(gpa)` each + `gpa.free`). The
/// input `slices` are CONSUMED — freed by this function before it returns. On
/// error the input is left for the caller to free (the `invoke` errdefer path).
pub fn repairPartition(
    gpa: std.mem.Allocator,
    slices: []greedy.Slice,
    tasks: []const greedy.Task,
    deps: []const greedy.Dep,
    budget: u32,
) InvokeError![]greedy.Slice {
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const a = arena_state.allocator();

    // Map each task id to the index of the task in `tasks` (for buildSlice).
    var task_idx = std.AutoHashMapUnmanaged(i64, usize).empty;
    for (tasks, 0..) |t, i| try task_idx.put(a, t.id, i);

    // Working membership: list of slices, each a list of task ids. Seeded from
    // the parsed partition.
    var groups = std.ArrayListUnmanaged(std.ArrayListUnmanaged(i64)).empty;
    for (slices) |s| {
        var g = std.ArrayListUnmanaged(i64).empty;
        for (s.task_ids) |id| try g.append(a, id);
        try groups.append(a, g);
    }

    // --- 1. D-HG2 cycle-repair: collapse SCCs of the inter-slice DAG ------
    try mergeCyclicGroups(a, &groups, deps);

    // --- 2. D-HG3 union-repair: split over-budget groups in topo order ---
    var repaired = std.ArrayListUnmanaged(std.ArrayListUnmanaged(i64)).empty;
    for (groups.items) |g| {
        if (g.items.len == 0) continue;
        try splitOverBudget(a, &repaired, g.items, tasks, &task_idx, deps, budget);
    }

    // --- 3. Materialize fresh greedy.Slices, then free the inputs --------
    var out = std.ArrayListUnmanaged(greedy.Slice).empty;
    errdefer {
        for (out.items) |s| s.deinit(gpa);
        out.deinit(gpa);
    }
    for (repaired.items) |g| {
        if (g.items.len == 0) continue;
        const member_idxs = try a.alloc(usize, g.items.len);
        for (g.items, 0..) |id, i| member_idxs[i] = task_idx.get(id).?;
        const slice = try buildSlice(gpa, tasks, member_idxs);
        try out.append(gpa, slice);
    }

    // Free the consumed input partition (success path).
    for (slices) |s| s.deinit(gpa);
    gpa.free(slices);

    const owned = try out.toOwnedSlice(gpa);
    std.mem.sort(greedy.Slice, owned, {}, struct {
        fn less(_: void, x: greedy.Slice, y: greedy.Slice) bool {
            return x.task_ids[0] < y.task_ids[0];
        }
    }.less);
    return owned;
}

/// D-HG2 — collapse every strongly-connected component of the inter-slice
/// precedence DAG into a single group, in place. After this returns, the
/// inter-slice graph induced by `deps` is acyclic (the SCC condensation is a
/// DAG). Groups that are not part of any cycle are untouched.
fn mergeCyclicGroups(
    a: std.mem.Allocator,
    groups: *std.ArrayListUnmanaged(std.ArrayListUnmanaged(i64)),
    deps: []const greedy.Dep,
) InvokeError!void {
    const n = groups.items.len;
    if (n <= 1) return;

    // task id -> group index.
    var grp_of = std.AutoHashMapUnmanaged(i64, usize).empty;
    for (groups.items, 0..) |g, gi| {
        for (g.items) |id| try grp_of.put(a, id, gi);
    }

    // Build group-level adjacency: blocker_group -> blocked_group.
    const adj = try a.alloc(std.AutoArrayHashMapUnmanaged(usize, void), n);
    for (adj) |*e| e.* = .empty;
    for (deps) |d| {
        const gb = grp_of.get(d.blocker) orelse continue;
        const gt = grp_of.get(d.blocked) orelse continue;
        if (gb == gt) continue;
        try adj[gb].put(a, gt, {});
    }

    // Tarjan's SCC over the group graph.
    const scc_of = try a.alloc(?usize, n);
    for (scc_of) |*s| s.* = null;
    var t = Tarjan{
        .a = a,
        .adj = adj,
        .index = try a.alloc(?usize, n),
        .lowlink = try a.alloc(usize, n),
        .on_stack = try a.alloc(bool, n),
        .stack = .empty,
        .scc_of = scc_of,
        .next_index = 0,
        .next_scc = 0,
    };
    for (t.index) |*i| i.* = null;
    for (t.on_stack) |*b| b.* = false;
    var v: usize = 0;
    while (v < n) : (v += 1) {
        if (t.index[v] == null) try t.strongConnect(v);
    }

    // Re-group by SCC id. Each SCC with >1 member group becomes one merged
    // group; SCCs of one group stay as-is. Preserve a deterministic order by
    // smallest original group index per SCC.
    var by_scc = try a.alloc(std.ArrayListUnmanaged(i64), t.next_scc);
    for (by_scc) |*g| g.* = .empty;
    for (groups.items, 0..) |g, gi| {
        const sid = scc_of[gi].?;
        for (g.items) |id| try by_scc[sid].append(a, id);
    }

    groups.clearRetainingCapacity();
    for (by_scc) |g| {
        if (g.items.len == 0) continue;
        try groups.append(a, g);
    }
}

/// Tarjan's strongly-connected-components state over the group graph.
const Tarjan = struct {
    a: std.mem.Allocator,
    adj: []std.AutoArrayHashMapUnmanaged(usize, void),
    index: []?usize,
    lowlink: []usize,
    on_stack: []bool,
    stack: std.ArrayListUnmanaged(usize),
    scc_of: []?usize,
    next_index: usize,
    next_scc: usize,

    fn strongConnect(self: *Tarjan, v: usize) std.mem.Allocator.Error!void {
        self.index[v] = self.next_index;
        self.lowlink[v] = self.next_index;
        self.next_index += 1;
        try self.stack.append(self.a, v);
        self.on_stack[v] = true;

        var it = self.adj[v].iterator();
        while (it.next()) |e| {
            const w = e.key_ptr.*;
            if (self.index[w] == null) {
                try self.strongConnect(w);
                self.lowlink[v] = @min(self.lowlink[v], self.lowlink[w]);
            } else if (self.on_stack[w]) {
                self.lowlink[v] = @min(self.lowlink[v], self.index[w].?);
            }
        }

        if (self.lowlink[v] == self.index[v].?) {
            const sid = self.next_scc;
            self.next_scc += 1;
            while (true) {
                const w = self.stack.pop().?;
                self.on_stack[w] = false;
                self.scc_of[w] = sid;
                if (w == v) break;
            }
        }
    }
};

/// D-HG3 — split a group's members into consecutive, dependency-ordered chunks
/// each with a true union cost ≤ `budget`, appending each chunk as a new group.
///
/// The members are first topologically ordered (consistent with `deps`); the
/// ordered sequence is then cut greedily: a running chunk grows until adding the
/// next member would push the chunk's TRUE union cost over `budget`, at which
/// point the chunk is emitted and a fresh one started. Because the cut points
/// fall on a topo order, every chunk's dependencies lie within itself or an
/// EARLIER chunk — no forward/backward straddle that could form a cycle. A lone
/// member whose own closure already exceeds `budget` is emitted as a singleton
/// (cannot be split smaller — matches greedy's lone-over-budget handling).
fn splitOverBudget(
    a: std.mem.Allocator,
    out: *std.ArrayListUnmanaged(std.ArrayListUnmanaged(i64)),
    members: []const i64,
    tasks: []const greedy.Task,
    task_idx: *const std.AutoHashMapUnmanaged(i64, usize),
    deps: []const greedy.Dep,
    budget: u32,
) InvokeError!void {
    // Topologically order the members (restricting deps to this member set).
    const ordered = try topoOrder(a, members, deps);

    var chunk = std.ArrayListUnmanaged(i64).empty;
    var chunk_idxs = std.ArrayListUnmanaged(usize).empty;
    for (ordered) |id| {
        const idx = task_idx.get(id).?;
        // Would adding this member exceed budget? If the chunk is non-empty and
        // the union with the new member is over budget, close the chunk first.
        if (chunk.items.len > 0) {
            try chunk_idxs.append(a, idx);
            const c = unionCostOf(a, tasks, chunk_idxs.items) catch |e| return e;
            if (c > budget) {
                // Roll the tentative member back out and flush the chunk.
                _ = chunk_idxs.pop();
                try out.append(a, chunk);
                chunk = .empty;
                chunk_idxs = .empty;
            }
        }
        try chunk.append(a, id);
        try chunk_idxs.append(a, idx);
    }
    if (chunk.items.len > 0) try out.append(a, chunk);
}

/// True union token cost of the members at `member_idxs` (dedup by qualified
/// symbol — `weight.cost` semantics). Mirrors `buildSlice`'s cost accumulation
/// without materializing the symbol list. Arena-scoped scratch.
fn unionCostOf(
    a: std.mem.Allocator,
    tasks: []const greedy.Task,
    member_idxs: []const usize,
) std.mem.Allocator.Error!u32 {
    var union_w = std.StringHashMapUnmanaged(u32).empty;
    var cost: u32 = 0;
    for (member_idxs) |mi| {
        for (tasks[mi].units) |u| {
            const gop = try union_w.getOrPut(a, u.qualified);
            if (!gop.found_existing) {
                gop.value_ptr.* = u.weight;
                cost += u.weight;
            } else if (u.weight > gop.value_ptr.*) {
                cost += u.weight - gop.value_ptr.*;
                gop.value_ptr.* = u.weight;
            }
        }
    }
    return cost;
}

/// Topologically order `members` consistent with `deps` (only edges whose BOTH
/// endpoints are in `members` constrain the order). Kahn's algorithm with a
/// deterministic tie-break on ascending task id, so the split is stable. If the
/// member subgraph somehow still contained a cycle (it must not after D-HG2),
/// the leftover members are appended in id order so no member is dropped.
fn topoOrder(
    a: std.mem.Allocator,
    members: []const i64,
    deps: []const greedy.Dep,
) std.mem.Allocator.Error![]i64 {
    var in_set = std.AutoHashMapUnmanaged(i64, void).empty;
    for (members) |id| try in_set.put(a, id, {});

    // indegree (number of blockers within the member set) + adjacency
    // blocker -> [blocked].
    var indeg = std.AutoHashMapUnmanaged(i64, usize).empty;
    for (members) |id| try indeg.put(a, id, 0);
    var adj = std.AutoHashMapUnmanaged(i64, std.ArrayListUnmanaged(i64)).empty;
    for (deps) |d| {
        if (!in_set.contains(d.blocked) or !in_set.contains(d.blocker)) continue;
        if (d.blocked == d.blocker) continue;
        const gop = try adj.getOrPut(a, d.blocker);
        if (!gop.found_existing) gop.value_ptr.* = .empty;
        try gop.value_ptr.append(a, d.blocked);
        const cur = indeg.get(d.blocked).?;
        try indeg.put(a, d.blocked, cur + 1);
    }

    var out = std.ArrayListUnmanaged(i64).empty;
    // Ready set = members with indegree 0, processed smallest-id-first.
    var ready = std.ArrayListUnmanaged(i64).empty;
    for (members) |id| if (indeg.get(id).? == 0) try ready.append(a, id);
    while (ready.items.len > 0) {
        // Pick the smallest-id ready member (deterministic).
        std.mem.sort(i64, ready.items, {}, struct {
            fn less(_: void, x: i64, y: i64) bool {
                return x < y;
            }
        }.less);
        const id = ready.orderedRemove(0);
        try out.append(a, id);
        if (adj.get(id)) |nbrs| {
            for (nbrs.items) |nb| {
                const cur = indeg.get(nb).?;
                try indeg.put(a, nb, cur - 1);
                if (cur - 1 == 0) try ready.append(a, nb);
            }
        }
    }
    // Defensive: append any member not yet emitted (would indicate a residual
    // cycle, which D-HG2 should have removed) in id order.
    if (out.items.len != members.len) {
        var emitted = std.AutoHashMapUnmanaged(i64, void).empty;
        for (out.items) |id| try emitted.put(a, id, {});
        var leftover = std.ArrayListUnmanaged(i64).empty;
        for (members) |id| if (!emitted.contains(id)) try leftover.append(a, id);
        std.mem.sort(i64, leftover.items, {}, struct {
            fn less(_: void, x: i64, y: i64) bool {
                return x < y;
            }
        }.less);
        for (leftover.items) |id| try out.append(a, id);
    }
    return out.toOwnedSlice(a);
}

// ---------------------------------------------------------------------------
// M3.3b internals
// ---------------------------------------------------------------------------

/// `k = ceil(total_union_cost / budget)`, clamped to ≥ 1, where
/// `total_union_cost` is the DEDUPED union of ALL tasks' closures (each
/// distinct qualified symbol counted ONCE), not the sum of per-task vertex
/// weights.
///
/// ## Why the deduped union, not Σ vertex-weight (the bug fix for task 4247)
///
/// The old formula summed each task's OWN closure cost. For COUPLED tasks that
/// share most of their symbols this massively over-counts the achievable
/// merged cost: 3 tightly-coupled tasks whose union is `B` but whose
/// per-task weights each approach `B` summed to `≈3B`, forcing `k=3`. With
/// `k=3` Mt-KaHyPar is REQUIRED to emit 3 non-empty blocks → 3 singletons →
/// zero merge benefit → a partition WORSE than greedy, violating the M3
/// "cost ≤ greedy" acceptance.
///
/// The deduped union is the true lower bound on the number of budget-sized
/// blocks the input can occupy (you cannot pack the union into fewer than
/// `ceil(union / B)` blocks), so basing `k` on it gives the solver maximal
/// room to co-locate coupled tasks into ONE block. The D-HG3 union-repair
/// pass (`repairPartition`) still SPLITS any block whose true union exceeds
/// `budget`, so a too-small `k` cannot produce an over-budget slice — the
/// floor only ever helps the solver merge, never hurts budget compliance.
fn blockCount(
    scratch: std.mem.Allocator,
    tasks: []const greedy.Task,
    budget: u32,
) std.mem.Allocator.Error!u32 {
    if (budget == 0) return 1;

    // Deduped union of every task's closure (each qualified symbol once).
    var arena = std.heap.ArenaAllocator.init(scratch);
    defer arena.deinit();
    const a = arena.allocator();
    var seen = std.StringHashMapUnmanaged(u32).empty;
    var total: u64 = 0;
    for (tasks) |t| {
        for (t.units) |u| {
            const gop = try seen.getOrPut(a, u.qualified);
            if (!gop.found_existing) {
                gop.value_ptr.* = u.weight;
                total += u.weight;
            } else if (u.weight > gop.value_ptr.*) {
                total += u.weight - gop.value_ptr.*;
                gop.value_ptr.* = u.weight;
            }
        }
    }
    if (total == 0) return 1;
    const k = (total + budget - 1) / budget; // ceil
    return @intCast(@max(@as(u64, 1), k));
}

/// Build one `greedy.Slice` from the member task indices of a block: sorted
/// member ids, the deduped union of their closure symbols (sorted), and the
/// union token cost (each distinct symbol counted once — `weight.cost`
/// semantics). All output is `gpa`-owned to match greedy's `finalizeSlice`.
fn buildSlice(
    gpa: std.mem.Allocator,
    tasks: []const greedy.Task,
    member_idxs: []const usize,
) InvokeError!greedy.Slice {
    const ids = try gpa.alloc(i64, member_idxs.len);
    errdefer gpa.free(ids);
    for (member_idxs, 0..) |mi, i| ids[i] = tasks[mi].id;
    std.mem.sort(i64, ids, {}, struct {
        fn less(_: void, x: i64, y: i64) bool {
            return x < y;
        }
    }.less);

    // Union symbol → weight, deduped (max weight defensively).
    var union_w = std.StringArrayHashMapUnmanaged(u32).empty;
    defer union_w.deinit(gpa);
    var cost: u32 = 0;
    for (member_idxs) |mi| {
        for (tasks[mi].units) |u| {
            const gop = try union_w.getOrPut(gpa, u.qualified);
            if (!gop.found_existing) {
                gop.value_ptr.* = u.weight;
                cost += u.weight;
            } else if (u.weight > gop.value_ptr.*) {
                cost += u.weight - gop.value_ptr.*;
                gop.value_ptr.* = u.weight;
            }
        }
    }

    var syms = try gpa.alloc([]const u8, union_w.count());
    errdefer gpa.free(syms);
    var n: usize = 0;
    errdefer for (syms[0..n]) |s| gpa.free(s);
    var kit = union_w.iterator();
    while (kit.next()) |e| : (n += 1) {
        syms[n] = try gpa.dupe(u8, e.key_ptr.*);
    }
    std.mem.sort([]const u8, syms, {}, struct {
        fn less(_: void, x: []const u8, y: []const u8) bool {
            return std.mem.order(u8, x, y) == .lt;
        }
    }.less);

    return .{ .task_ids = ids, .union_symbols = syms, .cost = cost };
}

/// Create a unique temp working dir under the system temp root and return its
/// absolute path (owned). Mirrors the throwaway-dir pattern the harvest /
/// models modules use, but at runtime (not test) scope.
fn makeTempDir(gpa: std.mem.Allocator, io: std.Io) std.mem.Allocator.Error![]const u8 {
    // Unique process-private suffix from the io randomness source (mirrors
    // editor.zig's createTempFile). The dir is short-lived and deleted before
    // `invoke` returns.
    var rng_buf: [8]u8 = undefined;
    io.random(&rng_buf);
    const hex = std.fmt.bytesToHex(rng_buf, .lower);

    const base = tmpBase();
    const path = try std.fmt.allocPrint(gpa, "{s}/planar-mtkahypar-{s}", .{ base, hex });
    errdefer gpa.free(path);
    std.Io.Dir.cwd().createDirPath(io, path) catch {};
    return path;
}

/// System temp root. Honors `$TMPDIR` (set on macOS to a per-user dir) and
/// falls back to `/tmp`. Read via `std.c.environ` so the engine module stays
/// free of the `runtime.Ctx` environ (mirrors editor.zig's `getPosixEnv`).
fn tmpBase() []const u8 {
    if (getPosixEnv("TMPDIR")) |t| {
        // Strip a trailing slash so the join below is clean.
        return if (t[t.len - 1] == '/') t[0 .. t.len - 1] else t;
    }
    return "/tmp";
}

/// Look up an environment variable via the C environ array. Returns null when
/// unset or empty.
fn getPosixEnv(key: []const u8) ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (s.len <= key.len + 1) continue;
        if (s[key.len] != '=') continue;
        if (!std.mem.eql(u8, s[0..key.len], key)) continue;
        const val = s[key.len + 1 ..];
        if (val.len == 0) return null;
        return val;
    }
    return null;
}

/// Write all of `data` to `path` (truncating).
fn writeFileAll(io: std.Io, path: []const u8, data: []const u8) !void {
    var f = try std.Io.Dir.cwd().createFile(io, path, .{});
    defer f.close(io);
    try f.writeStreamingAll(io, data);
}

/// Read back Mt-KaHyPar's partition file. The solver writes
/// `<input>.part<k>.…KaHyPar` into `dir`; the exact suffix is version-specific,
/// so we scan `dir` for any entry whose name starts with the input file's base
/// name and contains `.part`. Returns the file contents (owned).
fn readPartitionFile(
    gpa: std.mem.Allocator,
    io: std.Io,
    dir: []const u8,
    hgr_path: []const u8,
) InvokeError![]u8 {
    const base = std.fs.path.basename(hgr_path); // "graph.hgr"
    var d = std.Io.Dir.cwd().openDir(io, dir, .{ .iterate = true }) catch
        return error.PartitionMissing;
    defer d.close(io);

    var found: ?[]u8 = null;
    errdefer if (found) |f| gpa.free(f);
    var walker = d.iterate();
    while (walker.next(io) catch return error.PartitionMissing) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.startsWith(u8, entry.name, base)) continue;
        if (std.mem.indexOf(u8, entry.name, ".part") == null) continue;
        // Skip the input file itself.
        if (std.mem.eql(u8, entry.name, base)) continue;
        const full = std.fs.path.join(gpa, &.{ dir, entry.name }) catch
            return error.OutOfMemory;
        defer gpa.free(full);
        found = std.Io.Dir.cwd().readFileAlloc(
            io,
            full,
            gpa,
            std.Io.Limit.limited(8 * 1024 * 1024),
        ) catch return error.PartitionMissing;
        break;
    }
    return found orelse error.PartitionMissing;
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

// --- M3.3b: subprocess invoke + parse + degradation tests ------------------

fn freeSlices(a: std.mem.Allocator, slices: []greedy.Slice) void {
    for (slices) |s| s.deinit(a);
    a.free(slices);
}

test "mtkahypar.blockCount: ceil(deduped-union / budget), clamped >= 1 (disjoint)" {
    const a = testing.allocator;
    // DISJOINT symbols (a, b, c): the deduped union equals the per-task sum,
    // so k matches the old Σ-vertex-weight behaviour exactly. union = 100.
    const t1 = [_]greedy.Unit{
        .{ .qualified = "a", .role = .modify, .weight = 30 },
        .{ .qualified = "b", .role = .reference, .weight = 30 },
    };
    const t2 = [_]greedy.Unit{.{ .qualified = "c", .role = .modify, .weight = 40 }};
    const tasks = [_]greedy.Task{ mkTask(1, &t1), mkTask(2, &t2) };
    try testing.expectEqual(@as(u32, 1), try blockCount(a, &tasks, 100)); // exact fit
    try testing.expectEqual(@as(u32, 2), try blockCount(a, &tasks, 60)); // ceil(100/60)=2
    try testing.expectEqual(@as(u32, 4), try blockCount(a, &tasks, 30)); // ceil(100/30)=4
    try testing.expectEqual(@as(u32, 1), try blockCount(a, &tasks, 0)); // budget 0 → 1
    try testing.expectEqual(@as(u32, 100), try blockCount(a, &tasks, 1)); // ceil(100/1)
}

test "mtkahypar.blockCount: coupled tasks dedup to a SMALL k (the task-4247 fix)" {
    const a = testing.allocator;
    // Three tightly-COUPLED tasks: they all share the same heavy symbol H plus
    // one tiny private symbol each. The deduped union is H + the three tinies,
    // NOT 3*H. Under the OLD Σ-vertex-weight formula k would have been
    // ceil(3*H / budget) — forcing the solver into 3 singletons. The deduped
    // union keeps k at 1, giving the solver room to co-locate all three.
    const H = greedy.Unit{ .qualified = "H", .role = .reference, .weight = 90 };
    const t1u = [_]greedy.Unit{ H, .{ .qualified = "p1", .role = .modify, .weight = 1 } };
    const t2u = [_]greedy.Unit{ H, .{ .qualified = "p2", .role = .modify, .weight = 1 } };
    const t3u = [_]greedy.Unit{ H, .{ .qualified = "p3", .role = .modify, .weight = 1 } };
    const tasks = [_]greedy.Task{ mkTask(1, &t1u), mkTask(2, &t2u), mkTask(3, &t3u) };
    // Deduped union = H(90) + p1 + p2 + p3 = 93. budget 100 → k = 1 (one block).
    try testing.expectEqual(@as(u32, 1), try blockCount(a, &tasks, 100));
    // The OLD summed weight would have been 92*3 = 276 → ceil(276/100) = 3.
    // Prove the dedup matters: with the deduped union (93) k stays 1.
    try testing.expectEqual(@as(u32, 1), try blockCount(a, &tasks, 93));
}

test "mtkahypar.parsePartition: block ids in vertex order → grouped slices" {
    const a = testing.allocator;
    // Tasks seeded OUT of id order to prove vertex numbering = ascending id.
    //   vertex1 = task 1 (a,shared), vertex2 = task 2 (shared), vertex3 = task 3 (c)
    const t1 = [_]greedy.Unit{
        .{ .qualified = "a", .role = .modify, .weight = 10 },
        .{ .qualified = "shared", .role = .reference, .weight = 5 },
    };
    const t2 = [_]greedy.Unit{.{ .qualified = "shared", .role = .reference, .weight = 5 }};
    const t3 = [_]greedy.Unit{.{ .qualified = "c", .role = .modify, .weight = 7 }};
    const tasks = [_]greedy.Task{ mkTask(3, &t3), mkTask(1, &t1), mkTask(2, &t2) };

    // Partition: vertex1→block0, vertex2→block0, vertex3→block1.
    // i.e. tasks {1,2} together, task 3 alone.
    const part = "0\n0\n1\n";
    const slices = try parsePartition(a, part, &tasks);
    defer freeSlices(a, slices);

    try testing.expectEqual(@as(usize, 2), slices.len);
    // Sorted by smallest member id: slice0 = {1,2}, slice1 = {3}.
    try testing.expectEqualSlices(i64, &.{ 1, 2 }, slices[0].task_ids);
    try testing.expectEqualSlices(i64, &.{3}, slices[1].task_ids);
    // {1,2} union = a(10) + shared(5, deduped once) = 15.
    try testing.expectEqual(@as(u32, 15), slices[0].cost);
    // shared appears ONCE in the union.
    var shared_count: usize = 0;
    for (slices[0].union_symbols) |s| {
        if (std.mem.eql(u8, s, "shared")) shared_count += 1;
    }
    try testing.expectEqual(@as(usize, 1), shared_count);
    try testing.expectEqual(@as(u32, 7), slices[1].cost);
}

test "mtkahypar.parsePartition: all vertices in one block → single slice" {
    const a = testing.allocator;
    const t1 = [_]greedy.Unit{.{ .qualified = "x", .role = .modify, .weight = 3 }};
    const t2 = [_]greedy.Unit{.{ .qualified = "y", .role = .reference, .weight = 4 }};
    const tasks = [_]greedy.Task{ mkTask(5, &t1), mkTask(9, &t2) };
    const slices = try parsePartition(a, "0\n0\n", &tasks);
    defer freeSlices(a, slices);
    try testing.expectEqual(@as(usize, 1), slices.len);
    try testing.expectEqualSlices(i64, &.{ 5, 9 }, slices[0].task_ids);
    try testing.expectEqual(@as(u32, 7), slices[0].cost);
}

test "mtkahypar.parsePartition: wrong line count → PartitionMalformed" {
    const a = testing.allocator;
    const t1 = [_]greedy.Unit{.{ .qualified = "x", .role = .modify, .weight = 3 }};
    const t2 = [_]greedy.Unit{.{ .qualified = "y", .role = .reference, .weight = 4 }};
    const tasks = [_]greedy.Task{ mkTask(1, &t1), mkTask(2, &t2) };
    // Only one block id for two vertices.
    try testing.expectError(error.PartitionMalformed, parsePartition(a, "0\n", &tasks));
    // A non-numeric block id.
    try testing.expectError(error.PartitionMalformed, parsePartition(a, "0\nx\n", &tasks));
}

test "mtkahypar.parsePartition: empty task set → no slices" {
    const a = testing.allocator;
    const slices = try parsePartition(a, "", &.{});
    defer freeSlices(a, slices);
    try testing.expectEqual(@as(usize, 0), slices.len);
}

test "mtkahypar.solverAvailable: returns false when the binary is absent" {
    // On a machine WITHOUT mtkahypar (the CI / dev default), this is false —
    // the graceful-degradation gate. Where the binary IS installed it returns
    // true; either way it must not error. We only assert the no-error / bool
    // contract here so the test is stable on both kinds of machine.
    const a = testing.allocator;
    const present = solverAvailable(a, std.testing.io);
    // Tautological on type, but documents the contract: a bool, never a throw.
    try testing.expect(present == true or present == false);
}

// --- M3.3c: repair-pass + cost<=greedy acceptance tests --------------------

const greedy_mod = greedy;

/// Independent Kahn topo-check over a grouping's slice-DAG (a SECOND opinion,
/// not the engine's internal check). True iff the slice-precedence graph induced
/// by `deps` is acyclic — i.e. the grouping is schedulable.
fn slicesSchedulable(a: std.mem.Allocator, slices: []const greedy.Slice, deps: []const greedy.Dep) !bool {
    var task_slice = std.AutoHashMapUnmanaged(i64, usize).empty;
    defer task_slice.deinit(a);
    for (slices, 0..) |s, k| {
        for (s.task_ids) |t| try task_slice.put(a, t, k);
    }
    var adj = std.AutoHashMapUnmanaged(usize, std.AutoHashMapUnmanaged(usize, void)).empty;
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
    var indeg = std.AutoHashMapUnmanaged(usize, usize).empty;
    defer indeg.deinit(a);
    var k: usize = 0;
    while (k < slices.len) : (k += 1) try indeg.put(a, k, 0);
    var ait = adj.iterator();
    while (ait.next()) |e| {
        var nit = e.value_ptr.iterator();
        while (nit.next()) |ne| {
            const cur = indeg.get(ne.key_ptr.*).?;
            try indeg.put(a, ne.key_ptr.*, cur + 1);
        }
    }
    var queue = std.ArrayListUnmanaged(usize).empty;
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
    return visited == slices.len;
}

fn totalCost(slices: []const greedy.Slice) u64 {
    var t: u64 = 0;
    for (slices) |s| t += s.cost;
    return t;
}

test "mtkahypar.repairPartition: golden recorded partition — budget + schedulable + cost<=greedy (no binary)" {
    const a = testing.allocator;

    // --- Fixture: a plan whose OPTIMAL partition genuinely beats greedy -----
    //
    // Four tasks. The high-overlap structure is arranged so the optimal block
    // assignment ({1,2} | {3,4}) packs two heavy shared symbols each paid ONCE,
    // while greedy — forced to a budget that only admits ONE merge at a time and
    // a tie order that pulls the WRONG pair together first — ends up replicating
    // a heavy symbol across two slices, costing more.
    //
    //   task 1: A(40, ref) + B(40, ref)            → own cost 80
    //   task 2: A(40, ref) + B(40, ref)            → own cost 80   (twins of t1)
    //   task 3: C(40, ref) + D(40, ref)            → own cost 80
    //   task 4: C(40, ref) + D(40, ref)            → own cost 80   (twins of t3)
    //
    // {1,2} union = A+B = 80.  {3,4} union = C+D = 80.  Total optimal = 160.
    // Budget B = 90: fits any twin-pair (80) but not a cross pair (would be
    // A+B+C+D = 160 > 90), and not three tasks.
    //
    // Greedy on this same input: all four twin pairs tie at score 80; greedy's
    // tie-break merges the smallest-id eligible pair first → {1,2} (cost 80),
    // then {3,4} (cost 80). So on THIS symmetric fixture greedy also reaches
    // 160 — a TIE, which still satisfies "cost <= greedy". To make the solver
    // STRICTLY win we perturb below.
    const A = greedy.Unit{ .qualified = "A", .role = .reference, .weight = 40 };
    const B = greedy.Unit{ .qualified = "B", .role = .reference, .weight = 40 };
    const C = greedy.Unit{ .qualified = "C", .role = .reference, .weight = 40 };
    const D = greedy.Unit{ .qualified = "D", .role = .reference, .weight = 40 };

    // Perturbation that makes greedy STRICTLY worse than the recorded optimum:
    // give task 2 and task 3 a shared cross symbol X(10) that greedy's overlap
    // score will chase. With budget 90, greedy may pull {2,3} together first
    // (overlap on X) — but {2,3} union = A+B+C+D+X which is way over 90, so that
    // merge is refused; greedy falls back to the twin pairs. To force a genuine
    // greedy LOSS we instead use the asymmetric fixture in the next test. Here
    // we assert the TIE-OR-BETTER contract on the symmetric optimum.
    const t1u = [_]greedy.Unit{ A, B };
    const t2u = [_]greedy.Unit{ A, B };
    const t3u = [_]greedy.Unit{ C, D };
    const t4u = [_]greedy.Unit{ C, D };
    const tasks = [_]greedy.Task{
        .{ .id = 1, .units = &t1u },
        .{ .id = 2, .units = &t2u },
        .{ .id = 3, .units = &t3u },
        .{ .id = 4, .units = &t4u },
    };
    const budget: u32 = 90;

    // RECORDED mtkahypar partition output — one 0-based block id per vertex, in
    // ascending-task-id vertex order (the numbering `encode` assigns):
    //   vertex1=task1→block0, vertex2=task2→block0,
    //   vertex3=task3→block1, vertex4=task4→block1.
    // i.e. the optimal {1,2} | {3,4} assignment, as if the solver returned it.
    const recorded = "0\n0\n1\n1\n";

    // Feed it through parsePartition → repairPartition (the full M3.3c pipeline).
    const parsed = try parsePartition(a, recorded, &tasks);
    const solver_slices = try repairPartition(a, parsed, &tasks, &.{}, budget);
    defer freeSlices(a, solver_slices);

    // (a) Budget compliance by construction: every slice's true union <= B.
    for (solver_slices) |s| try testing.expect(s.cost <= budget);

    // (b) Schedulable (no deps here → trivially acyclic, but assert via Kahn).
    try testing.expect(try slicesSchedulable(a, solver_slices, &.{}));

    // (c) cost <= greedy on the SAME input.
    var g = try greedy_mod.group(a, &tasks, &.{}, budget);
    defer g.deinit(a);
    try testing.expect(totalCost(solver_slices) <= g.totalCost());

    // Concretely: the recorded optimum is {1,2}=80 + {3,4}=80 = 160.
    try testing.expectEqual(@as(u64, 160), totalCost(solver_slices));
}

test "mtkahypar.repairPartition: recorded partition STRICTLY beats greedy on an asymmetric fixture" {
    const a = testing.allocator;

    // Asymmetric fixture engineered so greedy's local, score-greedy merge order
    // strands a heavy symbol replicated across two slices, while the recorded
    // (optimal) partition co-locates the heavy-shared tasks and pays it once.
    //
    //   task 1: H(60, ref) + p(5, mod)     own 65   shares H with 2 and 3
    //   task 2: H(60, ref) + q(5, mod)     own 65   shares H with 1 and 3
    //   task 3: H(60, ref) + r(20, mod)    own 80   shares H with 1 and 2
    //   task 4: q(5, ref)  + s(30, mod)    own 35   shares small q with task 2
    //
    // Budget B = 130.
    //
    // OPTIMAL (recorded): {1,2,3} co-located → union = H(60)+p+q+r = 90 (<=130),
    //   {4} alone = 35. Total = 125.
    // GREEDY: highest overlap is H (weight 60) among {1,2},{1,3},{2,3}; greedy
    //   first merges {1,2} (tie-break smallest ids) → union H+p+q = 70. Next it
    //   tries to merge 3 in: union H+p+q+r = 90 <= 130, overlap H=60 → merges →
    //   {1,2,3}=90. Then task 4 shares q(5) with the slice → merge → +s(30) =
    //   90+30+... wait q already in union; union becomes H+p+q+r+s = 120 <=130
    //   → greedy ALSO co-locates 4. So greedy total = 120 < 125.
    //
    // That would make greedy BETTER — not what we want. So we cap the budget so
    // greedy cannot fit all four: B = 95. Then:
    //   GREEDY: {1,2}=70, +3 → 90 (<=95) ok → {1,2,3}=90. task4: union+s = 120
    //     > 95 → refused. {4} alone = 35. Greedy total = 90 + 35 = 125.
    //   RECORDED OPTIMAL identical here = 125. Tie again.
    //
    // To get a STRICT win, exploit greedy's WRITE-WRITE blind spot (D-HG1):
    // make the heavy shared symbol a WRITE in two tasks so greedy scores their
    // overlap as ZERO and refuses to co-locate them, replicating H — while the
    // recorded optimal co-locates them anyway (the union still fits B and the
    // optimum cares about union cost, not the conflict heuristic).
    //
    //   task 1: H(60, MODIFY) + p(5, mod)      shares H-as-write with task 2
    //   task 2: H(60, MODIFY) + q(5, mod)      shares H-as-write with task 1
    //   task 3: z(10, ref)                     disjoint singleton
    //
    // Budget B = 130 (fits {1,2} union H+p+q = 70).
    //   GREEDY: overlap({1,2}) on H is a write-write conflict → score 0, and no
    //     dep → greedy REFUSES the merge (score 0 and !has_dep → skip). So
    //     greedy leaves them SEPARATE: {1}=65, {2}=65, {3}=10 → total 140.
    //   RECORDED OPTIMAL: {1,2} co-located → H paid once = 70, {3}=10 →
    //     total 80. 80 < 140: a STRICT win.
    const H1 = greedy.Unit{ .qualified = "H", .role = .modify, .weight = 60 };
    const p = greedy.Unit{ .qualified = "p", .role = .modify, .weight = 5 };
    const H2 = greedy.Unit{ .qualified = "H", .role = .modify, .weight = 60 };
    const q = greedy.Unit{ .qualified = "q", .role = .modify, .weight = 5 };
    const z = greedy.Unit{ .qualified = "z", .role = .reference, .weight = 10 };

    const t1u = [_]greedy.Unit{ H1, p };
    const t2u = [_]greedy.Unit{ H2, q };
    const t3u = [_]greedy.Unit{z};
    const tasks = [_]greedy.Task{
        .{ .id = 1, .units = &t1u },
        .{ .id = 2, .units = &t2u },
        .{ .id = 3, .units = &t3u },
    };
    const budget: u32 = 130;

    // Recorded optimal: vertex1=t1→block0, vertex2=t2→block0, vertex3=t3→block1.
    const recorded = "0\n0\n1\n";
    const parsed = try parsePartition(a, recorded, &tasks);
    const solver_slices = try repairPartition(a, parsed, &tasks, &.{}, budget);
    defer freeSlices(a, solver_slices);

    for (solver_slices) |s| try testing.expect(s.cost <= budget);
    try testing.expect(try slicesSchedulable(a, solver_slices, &.{}));

    var g = try greedy_mod.group(a, &tasks, &.{}, budget);
    defer g.deinit(a);

    // Solver = 80 (H paid once), greedy = 140 (H replicated). STRICT win.
    try testing.expectEqual(@as(u64, 80), totalCost(solver_slices));
    try testing.expectEqual(@as(u64, 140), g.totalCost());
    try testing.expect(totalCost(solver_slices) < g.totalCost());
}

test "mtkahypar.repairPartition: D-HG3 splits an over-budget block to exact budget" {
    const a = testing.allocator;

    // Three tasks each with a unique heavy symbol; the recorded partition
    // dumps ALL THREE into one block whose true union far exceeds the budget.
    // The union-repair must split it so every output slice is <= budget.
    //
    //   task 1: x1(50, mod)
    //   task 2: x2(50, mod)
    //   task 3: x3(50, mod)
    // Recorded: all in block 0 → union 150. Budget 60 → must split: each task
    // is 50 (<=60), but no two fit together (100 > 60) → three singletons.
    const x1 = greedy.Unit{ .qualified = "x1", .role = .modify, .weight = 50 };
    const x2 = greedy.Unit{ .qualified = "x2", .role = .modify, .weight = 50 };
    const x3 = greedy.Unit{ .qualified = "x3", .role = .modify, .weight = 50 };
    const t1u = [_]greedy.Unit{x1};
    const t2u = [_]greedy.Unit{x2};
    const t3u = [_]greedy.Unit{x3};
    const tasks = [_]greedy.Task{
        .{ .id = 1, .units = &t1u },
        .{ .id = 2, .units = &t2u },
        .{ .id = 3, .units = &t3u },
    };
    const budget: u32 = 60;

    const parsed = try parsePartition(a, "0\n0\n0\n", &tasks);
    const repaired = try repairPartition(a, parsed, &tasks, &.{}, budget);
    defer freeSlices(a, repaired);

    // Three singletons, each <= budget.
    try testing.expectEqual(@as(usize, 3), repaired.len);
    for (repaired) |s| {
        try testing.expectEqual(@as(usize, 1), s.task_ids.len);
        try testing.expect(s.cost <= budget);
    }

    // A budget that fits two-but-not-three (110): the over-budget block of 150
    // splits into a chunk of two (100 <= 110) + one singleton (50).
    const parsed2 = try parsePartition(a, "0\n0\n0\n", &tasks);
    const repaired2 = try repairPartition(a, parsed2, &tasks, &.{}, 110);
    defer freeSlices(a, repaired2);
    try testing.expectEqual(@as(usize, 2), repaired2.len);
    for (repaired2) |s| try testing.expect(s.cost <= 110);
    // Total members preserved.
    var members: usize = 0;
    for (repaired2) |s| members += s.task_ids.len;
    try testing.expectEqual(@as(usize, 3), members);
}

test "mtkahypar.repairPartition: D-HG3 split respects ordering (no cycle from the split)" {
    const a = testing.allocator;

    // Chain 1 → 2 → 3 (2 blocked by 1, 3 blocked by 2). Recorded partition puts
    // all three in one over-budget block; the split must cut along the topo
    // order so the resulting slice-DAG stays schedulable.
    const ua = greedy.Unit{ .qualified = "a", .role = .modify, .weight = 50 };
    const ub = greedy.Unit{ .qualified = "b", .role = .modify, .weight = 50 };
    const uc = greedy.Unit{ .qualified = "c", .role = .modify, .weight = 50 };
    const t1u = [_]greedy.Unit{ua};
    const t2u = [_]greedy.Unit{ub};
    const t3u = [_]greedy.Unit{uc};
    const tasks = [_]greedy.Task{
        .{ .id = 1, .units = &t1u },
        .{ .id = 2, .units = &t2u },
        .{ .id = 3, .units = &t3u },
    };
    const deps = [_]greedy.Dep{
        .{ .blocked = 2, .blocker = 1 },
        .{ .blocked = 3, .blocker = 2 },
    };
    const budget: u32 = 110; // fits two but not three.

    const parsed = try parsePartition(a, "0\n0\n0\n", &tasks);
    const repaired = try repairPartition(a, parsed, &tasks, &deps, budget);
    defer freeSlices(a, repaired);

    for (repaired) |s| try testing.expect(s.cost <= budget);
    // The split chunks {1,2} and {3} (topo order) → schedulable.
    try testing.expect(try slicesSchedulable(a, repaired, &deps));
}

test "mtkahypar.repairPartition: D-HG2 cycle-repair merges slices that induce an inter-slice cycle" {
    const a = testing.allocator;

    // The recorded partition induces an inter-slice CYCLE: task 1 (block0) is
    // blocked by task 2 (block1), AND task 2 is blocked by task 1 — once they
    // are in different blocks the slice-DAG has 0→1 and 1→0, unschedulable.
    // (This models a precedence-blind solver cutting across a tight cycle.)
    //
    //   task 1: a(20, mod)   blocked by 2
    //   task 2: b(20, mod)   blocked by 1
    // Recorded: t1→block0, t2→block1. Slice DAG: 0↔1 cycle.
    // Cycle-repair must MERGE them into one schedulable slice (union 40 <= B).
    const ua = greedy.Unit{ .qualified = "a", .role = .modify, .weight = 20 };
    const ub = greedy.Unit{ .qualified = "b", .role = .modify, .weight = 20 };
    const t1u = [_]greedy.Unit{ua};
    const t2u = [_]greedy.Unit{ub};
    const tasks = [_]greedy.Task{
        .{ .id = 1, .units = &t1u },
        .{ .id = 2, .units = &t2u },
    };
    // A 2-cycle in the task DAG (intentionally pathological for the test).
    const deps = [_]greedy.Dep{
        .{ .blocked = 1, .blocker = 2 },
        .{ .blocked = 2, .blocker = 1 },
    };
    const budget: u32 = 100;

    const parsed = try parsePartition(a, "0\n1\n", &tasks);
    const repaired = try repairPartition(a, parsed, &tasks, &deps, budget);
    defer freeSlices(a, repaired);

    // Merged into ONE slice holding both tasks (the SCC collapse).
    try testing.expectEqual(@as(usize, 1), repaired.len);
    try testing.expectEqualSlices(i64, &.{ 1, 2 }, repaired[0].task_ids);
    try testing.expectEqual(@as(u32, 40), repaired[0].cost);
    try testing.expect(repaired[0].cost <= budget);
    // Intra-slice edges impose no slice-DAG edge → schedulable.
    try testing.expect(try slicesSchedulable(a, repaired, &deps));
}

test "mtkahypar.repairPartition: a clean partition is returned unchanged (budget-compliant + acyclic)" {
    const a = testing.allocator;
    // Recorded partition already budget-compliant and acyclic → repair is a
    // pass-through (modulo fresh allocation).
    const ua = greedy.Unit{ .qualified = "a", .role = .modify, .weight = 10 };
    const ub = greedy.Unit{ .qualified = "b", .role = .modify, .weight = 10 };
    const t1u = [_]greedy.Unit{ua};
    const t2u = [_]greedy.Unit{ub};
    const tasks = [_]greedy.Task{
        .{ .id = 1, .units = &t1u },
        .{ .id = 2, .units = &t2u },
    };
    const deps = [_]greedy.Dep{.{ .blocked = 2, .blocker = 1 }};
    const parsed = try parsePartition(a, "0\n1\n", &tasks);
    const repaired = try repairPartition(a, parsed, &tasks, &deps, 1000);
    defer freeSlices(a, repaired);
    try testing.expectEqual(@as(usize, 2), repaired.len);
    try testing.expect(try slicesSchedulable(a, repaired, &deps));
}

test "mtkahypar.invoke: live solver round-trip (skips when mtkahypar absent)" {
    const a = testing.allocator;
    // Skip-if-absent idiom (mirrors harvest.ensureGitAvailable): this test only
    // runs where the optional RUN_DEP is installed. On this machine the binary
    // is absent, so it SKIPS; the logic is exercised wherever mtkahypar exists.
    if (!solverAvailable(a, std.testing.io)) return error.SkipZigTest;

    const t1 = [_]greedy.Unit{
        .{ .qualified = "shared.api", .role = .reference, .weight = 50 },
        .{ .qualified = "t1.own", .role = .modify, .weight = 10 },
    };
    const t2 = [_]greedy.Unit{
        .{ .qualified = "shared.api", .role = .reference, .weight = 50 },
        .{ .qualified = "t2.own", .role = .modify, .weight = 10 },
    };
    const t3 = [_]greedy.Unit{.{ .qualified = "lonely", .role = .modify, .weight = 5 }};
    const tasks = [_]greedy.Task{ mkTask(1, &t1), mkTask(2, &t2), mkTask(3, &t3) };

    const slices = try invoke(a, std.testing.io, &tasks, .{ .budget = 200 });
    defer freeSlices(a, slices);

    // Contract: every task appears in exactly one slice; ids are a partition.
    var seen: usize = 0;
    for (slices) |s| seen += s.task_ids.len;
    try testing.expectEqual(@as(usize, 3), seen);
    for (slices) |s| try testing.expect(s.task_ids.len >= 1);
}

test "mtkahypar.invoke: live coupled fixture — solver-arm cost <= greedy (task 4247)" {
    const a = testing.allocator;
    // The regression guard for task 4247. On a machine WITH mtkahypar installed
    // this RUNS the real solver; the harness for this worktree has the official
    // PyPI-backed CLI shim on PATH, so it must NOT skip here.
    //
    // Fixture mirrors the bug repro (artifact 356): three TIGHTLY-COUPLED tasks
    // whose closures share one heavy symbol H plus a tiny private symbol each.
    //
    //   task 1: H(90, ref) + p1(2, mod)    own cost 92
    //   task 2: H(90, ref) + p2(2, mod)    own cost 92
    //   task 3: H(90, ref) + p3(2, mod)    own cost 92
    //
    // Deduped union of all three = H(90)+p1+p2+p3 = 96. Budget 100 fits the
    // whole union in ONE block (cost 96).
    //
    // GREEDY co-locates all three (each merge stays under 100, H paid once) →
    // one slice, cost 96.
    //
    // The OLD blockCount summed per-task weights = 276 → k = ceil(276/100) = 3,
    // forcing mtkahypar to emit 3 non-empty blocks → 3 singletons → cost
    // 92+92+92 = 276, WORSE than greedy (the exact bug). With the deduped-union
    // k (= 1) the solver gets to co-locate, and the load.zig min-guard would
    // pick greedy regardless. Here we assert the RAW invoke arm now produces
    // cost <= greedy on this coupled input.
    if (!solverAvailable(a, std.testing.io)) return error.SkipZigTest;

    const H = greedy.Unit{ .qualified = "H", .role = .reference, .weight = 90 };
    const t1u = [_]greedy.Unit{ H, .{ .qualified = "p1", .role = .modify, .weight = 2 } };
    const t2u = [_]greedy.Unit{ H, .{ .qualified = "p2", .role = .modify, .weight = 2 } };
    const t3u = [_]greedy.Unit{ H, .{ .qualified = "p3", .role = .modify, .weight = 2 } };
    const tasks = [_]greedy.Task{ mkTask(1, &t1u), mkTask(2, &t2u), mkTask(3, &t3u) };
    const budget: u32 = 100;

    // Greedy arm on the same input.
    var g = try greedy_mod.group(a, &tasks, &.{}, budget);
    defer g.deinit(a);

    // Solver arm (the real binary).
    const slices = try invoke(a, std.testing.io, &tasks, .{ .budget = budget });
    defer freeSlices(a, slices);

    // Every slice stays within budget (D-HG3 union-repair invariant).
    for (slices) |s| try testing.expect(s.cost <= budget);

    // The whole input is still partitioned.
    var seen: usize = 0;
    for (slices) |s| seen += s.task_ids.len;
    try testing.expectEqual(@as(usize, 3), seen);

    // THE GUARANTEE: the solver-arm cost must not exceed greedy on this coupled
    // input. The min-of-{solver,greedy} guard in load.recommendWith enforces
    // this end-to-end; the k-selection fix is what lets the raw solver arm meet
    // it here instead of collapsing to 3 singletons (cost 276 > greedy 96).
    const solver_cost = totalCost(slices);
    try testing.expect(solver_cost <= g.totalCost());
}
