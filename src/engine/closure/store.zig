//! engine/closure/store — M2.5 closure pipeline integration + persistence.
//!
//! Wires the M2 extractor pipeline together and persists its output to the
//! `closures` table (migration 00026):
//!
//!   task_touch_paths (seeds)
//!     → symbols.resolve   (per seed: the modify-set, with byte spans)
//!     → walk.walk         (role-partitioned closure: modify/reference/transitive)
//!     → weight.tokenCount (per unit, over its source span)
//!     → closures rows     (path, symbol, role, token_weight, extractor_version)
//!
//! ## Corpus
//!
//! The corpus the reference walk resolves against is every `.zig` file under
//! the repo's `projects.root_path`. Each seed's closure is computed against
//! that whole-repo corpus so cross-file references resolve. Files that cannot
//! be read or parsed are skipped (the walk simply leaves their references
//! `unresolved`); a corpus is best-effort, never fatal.
//!
//! ## Span resolution (the M2.4 SpanMap-population residual)
//!
//! Token weighting needs each unit's source span. Modify units carry byte
//! spans directly from `symbols.resolve`. Reference and transitive units
//! carry only a `qualified` name — to weight them we must resolve that name
//! back to its DEFINING file + byte span. `SpanIndex` does this: it parses
//! every corpus file once and records, for each qualified name (top-level
//! decl AND container member), the file path it lives in and the source
//! slice of its declaration. A unit whose qualified name is not in the
//! index (stdlib, builtins, unresolved) gets `path` = its stem and
//! `token_weight` = 0 — its context cost is unknown and never fabricated.
//!
//! ## Persistence
//!
//! Owns the DB writes. `compute` deletes any prior rows for the task at the
//! current `extractor_version` (so a recompute is idempotent rather than
//! UNIQUE-colliding) and inserts the fresh closure inside one savepoint.
//! Both `path` and `symbol` are stored: the qualified scheme is stem-only,
//! so `path` is what disambiguates two same-stem files (the M2.2 caveat).

const std = @import("std");
const db = @import("db");
const ts = @import("treesitter");
const symbols = @import("symbols.zig");
const walk = @import("walk.zig");
const weight = @import("weight.zig");

/// The extractor version stamped on every row this store writes. Bump when
/// the pipeline's symbol/walk/weight semantics change so a re-extraction is
/// comparable to (not silently overwriting) an earlier one.
pub const extractor_version = "m2-closure-0.1";

/// Max source-file size the corpus/seed reader will load (24 MiB). Larger
/// files are skipped (best-effort corpus).
const max_file_bytes: usize = 1 << 24;

/// The single-threaded blocking IO the filesystem reads run against
/// (mirrors engine/import.zig's `fsIo`). The engine holds no IO writers,
/// but disk reads need an `std.Io`; this is the canonical blocking one.
fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

pub const Error = error{
    NotFound,
    QueryFailed,
    NoSeeds,
} || std.mem.Allocator.Error;

/// One persisted closure row, as read back by `show`. Owned strings are
/// freed by `deinitRows`.
pub const Row = struct {
    id: i64,
    task_id: i64,
    repo_id: i64,
    path: []const u8,
    symbol: []const u8,
    role: []const u8,
    token_weight: i64,
    extractor_version: []const u8,
    created_at: []const u8,

    pub fn deinit(self: Row, a: std.mem.Allocator) void {
        a.free(self.path);
        a.free(self.symbol);
        a.free(self.role);
        a.free(self.extractor_version);
        a.free(self.created_at);
    }
};

pub fn deinitRows(rows: []const Row, a: std.mem.Allocator) void {
    for (rows) |r| r.deinit(a);
    a.free(rows);
}

/// Summary of a `compute` run, returned to the caller for reporting.
pub const ComputeResult = struct {
    task_id: i64,
    seeds: usize,
    modify: usize,
    reference: usize,
    transitive: usize,
    rows_written: usize,
};

// =========================================================================
// Internal: corpus + span index
// =========================================================================

/// A repo-relative seed path plus the repo it belongs to.
const Seed = struct {
    repo_id: i64,
    /// Repo-relative path as declared in task_touch_paths.
    rel_path: []const u8,
    /// Absolute on-disk path (root_path ++ rel_path).
    abs_path: []const u8,
};

/// Where a qualified symbol is defined: the repo-relative path of its file
/// and the source slice of its declaration (for token weighting).
const SpanEntry = struct {
    rel_path: []const u8,
    span: []const u8,
};

/// Maps a qualified symbol name to its defining file + source span. Backed
/// by the caller's arena; borrows source slices from the corpus buffers
/// (which the arena also owns), so it needs no separate deinit.
const SpanIndex = std.StringHashMapUnmanaged(SpanEntry);

// =========================================================================
// Public surface
// =========================================================================

/// Computes the derived closure for `task_id` and persists it to `closures`.
///
/// Reads the task's seeds from `task_touch_paths`, builds the repo corpus,
/// runs symbols→walk→weight per seed, resolves spans for every unit, and
/// writes one `closures` row per (path, symbol, role). Idempotent: prior
/// rows for the task at this `extractor_version` are deleted first.
///
/// Errors: `NoSeeds` when the task declares no path-level touches (nothing
/// to compute); `QueryFailed` on a DB error.
pub fn compute(
    d: *db.sqlite.Db,
    gpa: std.mem.Allocator,
    task_id: i64,
) Error!ComputeResult {
    var arena = std.heap.ArenaAllocator.init(gpa);
    defer arena.deinit();
    const a = arena.allocator();

    // 1. Read seeds (task_touch_paths joined to projects for root_path).
    const seeds = try readSeeds(d, a, task_id);
    if (seeds.len == 0) return Error.NoSeeds;

    // 2. Build the corpus: every readable .zig file under the seeds' repos.
    //    The corpus drives reference resolution AND the span index.
    var corpus: std.ArrayList(walk.File) = .empty;
    var repo_roots = try repoRootMap(d, a, seeds);
    {
        var it = repo_roots.iterator();
        while (it.next()) |e| {
            try collectCorpus(a, e.value_ptr.*, &corpus);
        }
    }

    // 3. Build the span index over the corpus (qualified → file + span).
    const span_index = try buildSpanIndex(a, corpus.items, &repo_roots);

    // 4. Per seed: resolve modify-set, walk for the role-partitioned closure.
    //    Accumulate persisted rows keyed by (path, symbol, role) so a symbol
    //    appearing for two seeds is not double-written.
    var pending: std.StringHashMapUnmanaged(PendingRow) = .empty;
    var counts = RoleCounts{};

    for (seeds) |seed| {
        const source = std.Io.Dir.cwd().readFileAlloc(
            fsIo(),
            seed.abs_path,
            a,
            std.Io.Limit.limited(max_file_bytes),
        ) catch continue;
        const seed_file = walk.File{ .path = seed.rel_path, .source = source };

        var closure = walk.walk(gpa, seed_file, corpus.items) catch continue;
        defer closure.deinit();

        // Modify-set spans come straight from symbols.resolve (authoritative
        // for the seed file's own decls).
        var mods = symbols.resolve(gpa, seed.rel_path, source) catch {
            // Even if symbol resolution fails we still record walk units.
            try recordUnits(a, &pending, &counts, closure.units, seed.repo_id, span_index, null, gpa);
            continue;
        };
        defer mods.deinit();

        try recordUnits(a, &pending, &counts, closure.units, seed.repo_id, span_index, &mods, gpa);
    }

    // 5. Persist: delete prior rows for this task at this version, then insert.
    const written = try persist(d, gpa, task_id, &pending);

    return .{
        .task_id = task_id,
        .seeds = seeds.len,
        .modify = counts.modify,
        .reference = counts.reference,
        .transitive = counts.transitive,
        .rows_written = written,
    };
}

/// Reads back the persisted closure rows for a task, ordered by role then
/// path then symbol. Caller frees via `deinitRows`.
pub fn show(d: *db.sqlite.Db, gpa: std.mem.Allocator, task_id: i64) Error![]Row {
    var stmt = d.prepare(
        \\select id, task_id, repo_id, path, symbol, role, token_weight,
        \\       extractor_version, created_at
        \\from closures
        \\where task_id = ?
        \\order by
        \\  case role when 'modify' then 0 when 'reference' then 1 else 2 end,
        \\  path, symbol
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(Row) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(gpa);
        out.deinit(gpa);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(gpa, .{
                .id = stmt.columnInt(0),
                .task_id = stmt.columnInt(1),
                .repo_id = stmt.columnInt(2),
                .path = try stmt.columnTextAlloc(3, gpa),
                .symbol = try stmt.columnTextAlloc(4, gpa),
                .role = try stmt.columnTextAlloc(5, gpa),
                .token_weight = stmt.columnInt(6),
                .extractor_version = try stmt.columnTextAlloc(7, gpa),
                .created_at = try stmt.columnTextAlloc(8, gpa),
            }),
        }
    }
    return out.toOwnedSlice(gpa);
}

// =========================================================================
// Internal helpers
// =========================================================================

const RoleCounts = struct {
    modify: usize = 0,
    reference: usize = 0,
    transitive: usize = 0,
};

/// A row staged for insertion. `path`/`symbol`/`role` are arena-owned.
const PendingRow = struct {
    repo_id: i64,
    path: []const u8,
    symbol: []const u8,
    role: walk.Role,
    token_weight: u32,
};

fn roleName(r: walk.Role) []const u8 {
    return switch (r) {
        .modify => "modify",
        .reference => "reference",
        .transitive => "transitive",
    };
}

/// Records a closure's units into the pending set, resolving each unit's
/// defining path + span (modify units prefer the seed's own symbol spans).
fn recordUnits(
    a: std.mem.Allocator,
    pending: *std.StringHashMapUnmanaged(PendingRow),
    counts: *RoleCounts,
    units: []const walk.Unit,
    repo_id: i64,
    span_index: SpanIndex,
    mods: ?*const symbols.Symbols,
    gpa: std.mem.Allocator,
) !void {
    // `mods` is accepted so a future refinement can prefer the seed's own
    // symbol spans; for MVP the span index (built over the same corpus,
    // which includes the seed) is the single authoritative span source.
    _ = mods;

    for (units) |u| {
        // Resolve path + span for this unit from the span index. A unit not
        // in the index (stdlib, builtin, unresolved) keeps its stem as the
        // path and weight 0 — its context cost is unknown, never fabricated.
        var rel_path: []const u8 = stemOf(u.qualified);
        var span: ?[]const u8 = null;
        if (span_index.get(u.qualified)) |entry| {
            rel_path = entry.rel_path;
            span = entry.span;
        }

        const tw: u32 = if (span) |sp| (weight.tokenCount(gpa, sp) catch 0) else 0;

        switch (u.role) {
            .modify => counts.modify += 1,
            .reference => counts.reference += 1,
            .transitive => counts.transitive += 1,
        }

        // Dedup key: (path, symbol, role). A symbol shared across seeds is
        // written once; the same symbol under two roles is two rows.
        const key = try std.fmt.allocPrint(a, "{s}\x00{s}\x00{s}", .{ rel_path, u.qualified, roleName(u.role) });
        if (pending.contains(key)) continue;
        try pending.put(a, key, .{
            .repo_id = repo_id,
            .path = try a.dupe(u8, rel_path),
            .symbol = try a.dupe(u8, u.qualified),
            .role = u.role,
            .token_weight = tw,
        });
    }
}

/// Deletes prior rows for `task_id` at this extractor version, then inserts
/// every pending row inside one savepoint. Returns the count inserted.
fn persist(
    d: *db.sqlite.Db,
    gpa: std.mem.Allocator,
    task_id: i64,
    pending: *std.StringHashMapUnmanaged(PendingRow),
) Error!usize {
    const sp = "closure_compute";
    d.savepoint(gpa, sp) catch return Error.QueryFailed;
    var released = false;
    errdefer if (!released) {
        d.rollbackToSavepoint(gpa, sp) catch {};
        d.releaseSavepoint(gpa, sp) catch {};
    };

    _ = d.execParams(
        "delete from closures where task_id = ? and extractor_version = ?",
        &.{ .{ .int = task_id }, .{ .text = extractor_version } },
    ) catch return Error.QueryFailed;

    var written: usize = 0;
    var it = pending.iterator();
    while (it.next()) |e| {
        const row = e.value_ptr.*;
        _ = d.execParams(
            \\insert into closures
            \\  (task_id, repo_id, path, symbol, role, token_weight, extractor_version)
            \\values (?, ?, ?, ?, ?, ?, ?)
        , &.{
            .{ .int = task_id },
            .{ .int = row.repo_id },
            .{ .text = row.path },
            .{ .text = row.symbol },
            .{ .text = roleName(row.role) },
            .{ .int = @as(i64, row.token_weight) },
            .{ .text = extractor_version },
        }) catch return Error.QueryFailed;
        written += 1;
    }

    d.releaseSavepoint(gpa, sp) catch return Error.QueryFailed;
    released = true;
    return written;
}

/// Reads the task's seeds from task_touch_paths joined to projects.
fn readSeeds(d: *db.sqlite.Db, a: std.mem.Allocator, task_id: i64) Error![]Seed {
    var stmt = d.prepare(
        \\select ttp.repo_id, ttp.path, p.root_path
        \\from task_touch_paths ttp
        \\join projects p on p.id = ttp.repo_id
        \\where ttp.task_id = ?
        \\order by ttp.repo_id, ttp.path
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(Seed) = .empty;
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const repo_id = stmt.columnInt(0);
                const rel = try stmt.columnTextAlloc(1, a);
                const root = try stmt.columnTextOpt(2, a);
                const abs = if (root) |r|
                    try std.fs.path.join(a, &.{ r, rel })
                else
                    try a.dupe(u8, rel);
                try out.append(a, .{ .repo_id = repo_id, .rel_path = rel, .abs_path = abs });
            },
        }
    }
    return out.toOwnedSlice(a);
}

/// Maps repo_id → root_path for every distinct repo among the seeds.
fn repoRootMap(
    d: *db.sqlite.Db,
    a: std.mem.Allocator,
    seeds: []const Seed,
) Error!std.AutoHashMapUnmanaged(i64, []const u8) {
    var roots: std.AutoHashMapUnmanaged(i64, []const u8) = .empty;
    for (seeds) |seed| {
        if (roots.contains(seed.repo_id)) continue;
        var stmt = d.prepare("select root_path from projects where id = ?") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = seed.repo_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => {},
            .row => {
                if (try stmt.columnTextOpt(0, a)) |root| {
                    try roots.put(a, seed.repo_id, root);
                }
            },
        }
    }
    return roots;
}

/// Walks `root` recursively, reading every `.zig` file into the corpus.
/// `File.path` is stored repo-relative so the qualified-name stem scheme
/// (basename-derived) and the span index line up with the seed paths.
fn collectCorpus(
    a: std.mem.Allocator,
    root: []const u8,
    corpus: *std.ArrayList(walk.File),
) !void {
    try collectCorpusRel(a, root, "", corpus);
}

fn collectCorpusRel(
    a: std.mem.Allocator,
    root: []const u8,
    rel: []const u8,
    corpus: *std.ArrayList(walk.File),
) !void {
    const dir_path = if (rel.len == 0) root else std.fs.path.join(a, &.{ root, rel }) catch return;
    var dir = std.Io.Dir.cwd().openDir(fsIo(), dir_path, .{ .iterate = true }) catch return;
    defer dir.close(fsIo());

    var it = dir.iterate();
    while (it.next(fsIo()) catch null) |entry| {
        // Skip hidden dirs/files (.git, .zig-cache, …) — never part of corpus.
        if (entry.name.len > 0 and entry.name[0] == '.') continue;
        const child_rel = if (rel.len == 0)
            a.dupe(u8, entry.name) catch continue
        else
            std.fs.path.join(a, &.{ rel, entry.name }) catch continue;
        switch (entry.kind) {
            .directory => try collectCorpusRel(a, root, child_rel, corpus),
            .file => {
                if (!std.mem.endsWith(u8, entry.name, ".zig")) continue;
                const abs = std.fs.path.join(a, &.{ root, child_rel }) catch continue;
                const src = std.Io.Dir.cwd().readFileAlloc(
                    fsIo(),
                    abs,
                    a,
                    std.Io.Limit.limited(max_file_bytes),
                ) catch continue;
                try corpus.append(a, .{ .path = child_rel, .source = src });
            },
            else => {},
        }
    }
}

/// Builds the qualified-name → {path, span} index over the corpus.
///
/// Each file is parsed once with tree-sitter. For every top-level
/// declaration the index records `stem.name → {path, decl-span}`. For
/// every container (struct/enum/union/opaque) it ALSO records each member
/// `stem.Container.member → {path, container-span}` — members are weighted
/// at container granularity (the whole container is resident), matching the
/// modify-set fold. This is what lets a reference resolved to a specific
/// member (the M2.2 caveat) be weighted instead of dropped.
fn buildSpanIndex(
    a: std.mem.Allocator,
    corpus: []const walk.File,
    repo_roots: *std.AutoHashMapUnmanaged(i64, []const u8),
) !SpanIndex {
    _ = repo_roots;
    var index: SpanIndex = .empty;
    for (corpus) |file| {
        const stem = fileStem(file.path);
        var parsed = ts.parse(file.source) catch continue;
        defer parsed.deinit();
        const root = parsed.root();
        const n = ts.c.ts_node_child_count(root);
        var i: u32 = 0;
        while (i < n) : (i += 1) {
            const node = ts.c.ts_node_child(root, i);
            if (!ts.c.ts_node_is_named(node)) continue;
            const ty = ts.nodeType(node);
            const is_fn = std.mem.eql(u8, ty, "function_declaration");
            const is_var = std.mem.eql(u8, ty, "variable_declaration");
            if (!is_fn and !is_var) continue;
            const name = childIdentifier(node, file.source) orelse continue;
            const span = nodeSpan(node, file.source) orelse continue;
            const qual = try std.fmt.allocPrint(a, "{s}.{s}", .{ stem, name });
            if (!index.contains(qual)) {
                try index.put(a, qual, .{ .rel_path = file.path, .span = span });
            }
            // Container members → container span.
            if (is_var) {
                if (containerDecl(node)) |cont| {
                    try indexContainerMembers(a, &index, file, stem, name, span, cont);
                }
            }
        }
    }
    return index;
}

/// Indexes each member of a container under `stem.Container.member`,
/// mapping it to the container's span (container-granularity weighting).
fn indexContainerMembers(
    a: std.mem.Allocator,
    index: *SpanIndex,
    file: walk.File,
    stem: []const u8,
    container: []const u8,
    container_span: []const u8,
    cont: ts.Node,
) !void {
    const n = ts.c.ts_node_child_count(cont);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(cont, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        const ty = ts.nodeType(child);
        const is_member = std.mem.eql(u8, ty, "function_declaration") or
            std.mem.eql(u8, ty, "container_field") or
            std.mem.eql(u8, ty, "variable_declaration");
        if (!is_member) continue;
        const mname = childIdentifier(child, file.source) orelse continue;
        const qual = try std.fmt.allocPrint(a, "{s}.{s}.{s}", .{ stem, container, mname });
        if (!index.contains(qual)) {
            try index.put(a, qual, .{ .rel_path = file.path, .span = container_span });
        }
    }
}

fn nodeSpan(node: ts.Node, source: []const u8) ?[]const u8 {
    const start = ts.c.ts_node_start_byte(node);
    const end = ts.c.ts_node_end_byte(node);
    if (end <= start or end > source.len) return null;
    return source[start..end];
}

fn childIdentifier(node: ts.Node, source: []const u8) ?[]const u8 {
    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        if (std.mem.eql(u8, ts.nodeType(child), "identifier")) {
            const s = ts.c.ts_node_start_byte(child);
            const e = ts.c.ts_node_end_byte(child);
            if (e > source.len) return null;
            return source[s..e];
        }
    }
    return null;
}

fn containerDecl(node: ts.Node) ?ts.Node {
    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        const ty = ts.nodeType(child);
        if (std.mem.eql(u8, ty, "struct_declaration") or
            std.mem.eql(u8, ty, "enum_declaration") or
            std.mem.eql(u8, ty, "union_declaration") or
            std.mem.eql(u8, ty, "opaque_declaration")) return child;
    }
    return null;
}

fn fileStem(path: []const u8) []const u8 {
    const base = std.fs.path.basename(path);
    if (std.mem.endsWith(u8, base, ".zig")) return base[0 .. base.len - ".zig".len];
    return base;
}

/// Returns the file-stem of a qualified name (text before the first dot).
fn stemOf(qualified: []const u8) []const u8 {
    const dot = std.mem.indexOfScalar(u8, qualified, '.') orelse return qualified;
    return qualified[0..dot];
}

// =========================================================================
// Tests
// =========================================================================

test "extractor_version is a stable constant" {
    try std.testing.expectEqualStrings("m2-closure-0.1", extractor_version);
}

test "stemOf derives the file stem from a qualified name" {
    try std.testing.expectEqualStrings("widget", stemOf("widget.Foo.bar"));
    try std.testing.expectEqualStrings("widget", stemOf("widget.run"));
    try std.testing.expectEqualStrings("nodots", stemOf("nodots"));
}

test "roleName maps roles to their schema text" {
    try std.testing.expectEqualStrings("modify", roleName(.modify));
    try std.testing.expectEqualStrings("reference", roleName(.reference));
    try std.testing.expectEqualStrings("transitive", roleName(.transitive));
}
