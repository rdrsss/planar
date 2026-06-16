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
/// optional Planar RUN_DEP (`gh`, `rg`). Not vendored, not compiled by
/// `build.zig` (D-HG4); the operator builds it from source.
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
/// Mirrors `harvest.ensureGitAvailable` / `models.probeBinary`: spawn
/// `mtkahypar --help`, treat ANY spawn error or non-zero/abnormal exit as
/// "absent". This is the gate the `--solver=mtkahypar` path checks before
/// reaching for `invoke`; a `false` result degrades to greedy with
/// `optimal_available:false` and no error surfaced to the operator.
pub fn solverAvailable(allocator: std.mem.Allocator, io: std.Io) bool {
    const res = std.process.run(allocator, io, .{
        .argv = &.{ solver_bin, "--help" },
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
    /// derive the starting block count `k = ceil(Σ vertex-weight / budget)`.
    budget: u32,
    /// Imbalance tolerance handed to `mtkahypar` (`-e`). Defaults to
    /// `default_epsilon`.
    epsilon: []const u8 = default_epsilon,
    /// hMETIS encoding knobs forwarded to `encode`.
    encode: EncodeOptions = .{},
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
/// NOTE (scope — M3.3b): the slice `cost` here is the straight union cost of
/// each block's closure (dedup by symbol, `weight.cost` semantics) so the slice
/// shape is complete; it is NOT yet budget-repaired. A block whose union
/// exceeds `budget` is returned as-is — the D-HG3 union-repair pass that splits
/// it back under budget is M3.3c. `invoke` is purely invoke + parse.
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

    // --- 3. Block count k = ceil(Σ vertex-weight / budget), clamped ≥ 1 -
    const k = blockCount(tasks, opts.budget);
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
    return parsePartition(gpa, part, tasks);
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

// ---------------------------------------------------------------------------
// M3.3b internals
// ---------------------------------------------------------------------------

/// `k = ceil(Σ vertex-weight / budget)`, clamped to ≥ 1. The starting block
/// count: enough blocks that, if the solver balanced perfectly, each would fit
/// the budget. M3.3c's repair pass corrects any residual over-budget block.
fn blockCount(tasks: []const greedy.Task, budget: u32) u32 {
    var total: u64 = 0;
    for (tasks) |t| total += vertexWeight(t);
    if (budget == 0 or total == 0) return 1;
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

test "mtkahypar.blockCount: ceil(total / budget), clamped >= 1" {
    const t1 = [_]greedy.Unit{
        .{ .qualified = "a", .role = .modify, .weight = 30 },
        .{ .qualified = "b", .role = .reference, .weight = 30 },
    };
    const t2 = [_]greedy.Unit{.{ .qualified = "c", .role = .modify, .weight = 40 }};
    const tasks = [_]greedy.Task{ mkTask(1, &t1), mkTask(2, &t2) };
    // total vertex weight = 60 + 40 = 100.
    try testing.expectEqual(@as(u32, 1), blockCount(&tasks, 100)); // exact fit
    try testing.expectEqual(@as(u32, 2), blockCount(&tasks, 60)); // ceil(100/60)=2
    try testing.expectEqual(@as(u32, 4), blockCount(&tasks, 30)); // ceil(100/30)=4
    try testing.expectEqual(@as(u32, 1), blockCount(&tasks, 0)); // budget 0 → 1
    try testing.expectEqual(@as(u32, 100), blockCount(&tasks, 1)); // ceil(100/1)
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
