const std = @import("std");
const db = @import("db");
const manifest = @import("manifest.zig");
const feature = @import("feature.zig");
const parse = @import("parse.zig");
const render = @import("render.zig");
const terminal_mod = @import("terminal.zig");
const c = @cImport({
    @cInclude("fcntl.h");
    @cInclude("unistd.h");
    @cInclude("sys/stat.h");
    @cInclude("dirent.h");
    @cInclude("stdio.h");
});

pub const Mode = enum { status, pull, push, sync };

pub const Summary = struct {
    applied: usize = 0,
    pending: usize = 0,
    conflicts: usize = 0,
    malformed: usize = 0,
    /// Count of terminal-status entities excluded from the FS write set under
    /// the active `filter_mode` (plan 439 M2). Always 0 on non-push runs.
    filtered: usize = 0,
    /// Count of pre-existing files on the FS for entities this push would
    /// have filtered. These are the "surprise files" the operator
    /// accumulated before the filter shipped (plan 439 M3). Reported but
    /// not removed unless `--apply-cleanup` was passed.
    pre_existing_terminal: usize = 0,
    /// Count of pre-existing terminal files this push REMOVED in the same
    /// pass (only when `--apply-cleanup` was passed). Subset of
    /// `pre_existing_terminal`.
    cleaned: usize = 0,
};

pub const MalformedFile = struct {
    path: []const u8,
    parse_error: []const u8,
};

pub const Entry = struct {
    class: Classification,
    file_path: []const u8,
    entity_kind: []const u8,
    entity_id: i64,
    conflict_id: i64 = 0,
    parse_error: []const u8 = "",
};

pub const Result = struct {
    applied: usize = 0,
    pending: usize = 0,
    conflicts: usize = 0,
    malformed: usize = 0,
    malformed_files: []const MalformedFile = &.{},
    /// Count of terminal-status entities excluded from the FS write set.
    filtered: usize = 0,
    /// Pre-existing terminal files visible on disk that fall inside this
    /// push's filter set (plan 439 M3). Reported in the summary; removed
    /// only when `apply_cleanup` was passed.
    pre_existing_terminal: usize = 0,
    /// Number of pre-existing terminal files actually removed in this pass.
    cleaned: usize = 0,
    /// Active filter mode label (`"failures"` or `"all"`), so the CLI summary
    /// can surface the operative policy. Always `"failures"` for non-push
    /// runs (the field exists; the value is not consulted there).
    filter_mode: []const u8 = "failures",
    entries: []const Entry,
};

pub const ConflictResolution = enum { fs, db };

const Classification = enum {
    no_op,
    fs_to_db,
    db_to_fs,
    conflict,
    new_on_fs,
    deleted_on_fs,
    malformed,
};

const Entity = struct {
    kind: []const u8,
    id: i64,
    updated_at: []const u8,
    /// Current entity status. Empty when the status column is unreachable for
    /// the kind (defensive — the filter step treats unknown as active).
    status: []const u8,
};

const Anchor = struct {
    id: i64,
    slug: []const u8,
    assoc_slug: []const u8,
    assoc_id: ?i64,
    plan_key: []const u8,
};

pub fn deinitResult(allocator: std.mem.Allocator, result: Result) void {
    for (result.entries) |entry| {
        allocator.free(entry.file_path);
        allocator.free(entry.entity_kind);
        if (entry.parse_error.len > 0) allocator.free(entry.parse_error);
    }
    if (result.entries.len > 0) allocator.free(result.entries);
    if (result.malformed_files.len > 0) allocator.free(result.malformed_files);
}

pub fn status(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) !Result {
    return run(d, allocator, anchor_plan_id, .status, .failures, false);
}

pub fn pull(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) !Result {
    return run(d, allocator, anchor_plan_id, .pull, .failures, false);
}

/// Push DB → FS for the given anchor plan's feature tree.
///
/// `filter_mode` selects which terminal-status entities are excluded from the
/// FS write set (plan 439 M2). `apply_cleanup` instructs the push to also
/// remove pre-existing FS files for entities that the filter would have
/// dropped (plan 439 M3). The cleanup is narrow-scope: it operates only on
/// files for entities this push enumerated, not on every terminal-backed
/// file in the workbench tree (use `planar workbench gc` for that).
pub fn push(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
    filter_mode: terminal_mod.Mode,
    apply_cleanup: bool,
) !Result {
    return run(d, allocator, anchor_plan_id, .push, filter_mode, apply_cleanup);
}

pub fn sync(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) !Result {
    return run(d, allocator, anchor_plan_id, .sync, .failures, false);
}

pub fn resolve(d: *db.sqlite.Db, event_id: i64, prefer: ConflictResolution) !void {
    var stmt = d.prepare("select coalesce(context_json,''), outcome from sync_events where id=? and scope='workbench'") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = event_id }}) catch return error.QueryFailed;
    const ctx_json, const outcome_now = switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => .{
            try stmt.columnTextAlloc(0, std.heap.page_allocator),
            try stmt.columnTextAlloc(1, std.heap.page_allocator),
        },
    };
    defer std.heap.page_allocator.free(ctx_json);
    defer std.heap.page_allocator.free(outcome_now);
    if (!std.mem.eql(u8, outcome_now, "conflict")) return error.InvalidInput;
    const ctx = parseConflictContext(ctx_json) catch return error.InvalidInput;
    defer std.heap.page_allocator.free(ctx.entity_kind);
    defer std.heap.page_allocator.free(ctx.file_path);

    const root = try resolveWorkbenchRoot(std.heap.page_allocator);
    defer std.heap.page_allocator.free(root);
    const abs = try std.fs.path.join(std.heap.page_allocator, &.{ root, ctx.file_path });
    defer std.heap.page_allocator.free(abs);
    switch (prefer) {
        .fs => {
            const fs_content = try readFileAlloc(std.heap.page_allocator, abs);
            defer std.heap.page_allocator.free(fs_content);
            const applied = try pullToDb(d, ctx.entity_kind, ctx.entity_id, fs_content);
            if (!applied) return error.InvalidInput;
            const db_updated = try fetchUpdatedAt(d, std.heap.page_allocator, ctx.entity_kind, ctx.entity_id);
            defer std.heap.page_allocator.free(db_updated);
            const fs_hash = try manifest.hashContent(std.heap.page_allocator, fs_content);
            defer std.heap.page_allocator.free(fs_hash);
            try manifest.upsert(d, .{
                .id = 0,
                .anchor_plan_id = ctx.anchor_plan_id,
                .entity_kind = ctx.entity_kind,
                .entity_id = ctx.entity_id,
                .file_path = ctx.file_path,
                .content_hash = fs_hash,
                .fs_mtime = "",
                .db_updated_at = db_updated,
                .last_synced_at = "",
            });
        },
        .db => {
            const rel, const db_content = try renderEntity(d, std.heap.page_allocator, ctx.anchor_plan_id, ctx.entity_kind, ctx.entity_id);
            defer std.heap.page_allocator.free(rel);
            defer std.heap.page_allocator.free(db_content);
            try writeFileAtomic(std.heap.page_allocator, abs, db_content);
            const db_updated = try fetchUpdatedAt(d, std.heap.page_allocator, ctx.entity_kind, ctx.entity_id);
            defer std.heap.page_allocator.free(db_updated);
            const db_hash = try manifest.hashContent(std.heap.page_allocator, db_content);
            defer std.heap.page_allocator.free(db_hash);
            try manifest.upsert(d, .{
                .id = 0,
                .anchor_plan_id = ctx.anchor_plan_id,
                .entity_kind = ctx.entity_kind,
                .entity_id = ctx.entity_id,
                .file_path = ctx.file_path,
                .content_hash = db_hash,
                .fs_mtime = "",
                .db_updated_at = db_updated,
                .last_synced_at = "",
            });
        },
    }
    const outcome: []const u8 = switch (prefer) {
        .fs => "resolved-fs",
        .db => "resolved-db",
    };
    _ = d.execParams(
        "update sync_events set outcome = ? where id = ? and scope = 'workbench'",
        &.{ .{ .text = outcome }, .{ .int = event_id } },
    ) catch return error.QueryFailed;
}

/// Archive the workbench tree for the given anchor plan.
///
/// `filter_mode` is accepted for API symmetry with push/restore. The current
/// archive implementation deletes the on-disk feature tree without packaging
/// it into a separate archive store, so there is no "write set" to filter.
/// If a future revision adds an archive store (tarball, git stash, etc.),
/// this is the parameter that selects which terminal-status entries are
/// packaged. For now, callers can pass `.failures` (the default) safely.
pub fn archive(d: *db.sqlite.Db, anchor_plan_id: i64, root: []const u8, allocator: std.mem.Allocator, filter_mode: terminal_mod.Mode) ![]const u8 {
    _ = filter_mode; // Reserved; archive currently has no write set to filter.
    const a = try fetchAnchor(d, allocator, anchor_plan_id);
    defer freeAnchor(allocator, a);
    const feature_dir = try feature.featureDir(allocator, root, a.assoc_slug, a.plan_key, a.slug);
    errdefer allocator.free(feature_dir);
    deleteTreePortable(allocator, feature_dir) catch |err| switch (err) {
        error.FileNotFound => {},
        else => return error.QueryFailed,
    };
    _ = d.execParams("delete from workbench_sync_state where anchor_plan_id = ?", &.{.{ .int = anchor_plan_id }}) catch return error.QueryFailed;
    return feature_dir;
}

/// Restore the workbench tree for the given anchor plan from DB state.
///
/// `filter_mode` selects which terminal-status entries are re-materialized
/// (plan 439 M5). M1 threads the parameter through without changing behavior;
/// the per-entity filter check lands in M5.
pub fn restore(d: *db.sqlite.Db, anchor_plan_id: i64, root: []const u8, allocator: std.mem.Allocator, filter_mode: terminal_mod.Mode) ![]const u8 {
    const a = try fetchAnchor(d, allocator, anchor_plan_id);
    defer freeAnchor(allocator, a);
    const feature_dir = try feature.featureDir(allocator, root, a.assoc_slug, a.plan_key, a.slug);
    errdefer allocator.free(feature_dir);
    try makePathAll(feature_dir);

    const entities = try enumerateEntities(d, allocator, anchor_plan_id);
    defer freeEntities(allocator, entities);
    for (entities) |e| {
        // Plan 439 M5: restore honors the same filter as push. Skip
        // terminal entities under the active filter mode so the restored
        // tree mirrors what a fresh push would write. The anchor plan is
        // exempt.
        if (e.id != anchor_plan_id) {
            if (terminal_mod.isFilteredStr(e.kind, e.status, filter_mode)) |drop| {
                if (drop) continue;
            }
        }
        const rel_path, const content = try renderEntity(d, allocator, anchor_plan_id, e.kind, e.id);
        defer allocator.free(rel_path);
        defer allocator.free(content);
        const abs = try std.fs.path.join(allocator, &.{ feature_dir, rel_path });
        defer allocator.free(abs);
        try writeFileAtomic(allocator, abs, content);
        const stored = try feature.storedPath(allocator, a.assoc_slug, a.plan_key, a.slug, rel_path);
        defer allocator.free(stored);
        const hash = try manifest.hashContent(allocator, content);
        defer allocator.free(hash);
        try manifest.upsert(d, .{
            .id = 0,
            .anchor_plan_id = anchor_plan_id,
            .entity_kind = e.kind,
            .entity_id = e.id,
            .file_path = stored,
            .content_hash = hash,
            .fs_mtime = "",
            .db_updated_at = e.updated_at,
            .last_synced_at = "",
        });
    }
    const rows = try manifest.load(d, allocator, anchor_plan_id);
    defer manifest.deinitRows(rows, allocator);
    try manifest.writeSyncFile(allocator, feature_dir, rows);
    return feature_dir;
}

pub const ActiveFeature = struct {
    plan_id: i64,
    slug: []const u8,
    status: []const u8,
    assoc_slug: []const u8,
    plan_key: []const u8,
    has_fs_tree: bool,
};

pub fn listActive(d: *db.sqlite.Db, allocator: std.mem.Allocator, root: []const u8) ![]ActiveFeature {
    var stmt = d.prepare(
        \\select p.id, coalesce(p.slug,''), coalesce(p.status,''), coalesce(a.slug,'global')
        \\from plans p
        \\left join associations a on (p.scope_kind='association' and a.id=p.scope_id)
        \\where p.parent_plan_id is null
        \\order by p.id
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    var out: std.ArrayList(ActiveFeature) = .empty;
    errdefer {
        for (out.items) |f| freeActive(allocator, f);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const slug = try stmt.columnTextAlloc(1, allocator);
                const status_txt = try stmt.columnTextAlloc(2, allocator);
                const assoc = try stmt.columnTextAlloc(3, allocator);
                const key = try resolvePlanKey(d, allocator, id);
                const dir = try feature.featureDir(allocator, root, assoc, key, slug);
                defer allocator.free(dir);
                const has_tree = pathExists(dir);
                try out.append(allocator, .{
                    .plan_id = id,
                    .slug = slug,
                    .status = status_txt,
                    .assoc_slug = assoc,
                    .plan_key = key,
                    .has_fs_tree = has_tree,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn freeActive(allocator: std.mem.Allocator, v: ActiveFeature) void {
    allocator.free(v.slug);
    allocator.free(v.status);
    allocator.free(v.assoc_slug);
    allocator.free(v.plan_key);
}

pub fn freeActiveMany(allocator: std.mem.Allocator, values: []const ActiveFeature) void {
    for (values) |v| freeActive(allocator, v);
    allocator.free(values);
}

fn run(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
    mode: Mode,
    filter_mode: terminal_mod.Mode,
    apply_cleanup: bool,
) !Result {
    const a = try fetchAnchor(d, allocator, anchor_plan_id);
    defer freeAnchor(allocator, a);
    const root = try resolveWorkbenchRoot(allocator);
    defer allocator.free(root);
    const feature_dir = try feature.featureDir(allocator, root, a.assoc_slug, a.plan_key, a.slug);
    defer allocator.free(feature_dir);
    if (mode != .status) try makePathAll(feature_dir);

    const manifest_rows = try manifest.load(d, allocator, anchor_plan_id);
    defer manifest.deinitRows(manifest_rows, allocator);

    const entities = try enumerateEntities(d, allocator, anchor_plan_id);
    defer freeEntities(allocator, entities);

    var summary = Summary{};
    var entries: std.ArrayList(Entry) = .empty;
    errdefer {
        for (entries.items) |entry| {
            allocator.free(entry.file_path);
            allocator.free(entry.entity_kind);
            if (entry.parse_error.len > 0) allocator.free(entry.parse_error);
        }
        entries.deinit(allocator);
    }
    var seen_files: std.StringHashMap(bool) = .init(allocator);
    var seen_file_keys: std.ArrayList([]const u8) = .empty;
    defer {
        for (seen_file_keys.items) |key| allocator.free(key);
        seen_file_keys.deinit(allocator);
        seen_files.deinit();
    }
    for (entities) |e| {
        // Plan 439 M2 filter step. On push runs, drop entities whose status
        // is in the active filter set. Other I/O modes (status/pull/sync)
        // are untouched — pull explicitly bypasses the filter (Q336), and
        // status/sync show the full set so the operator can reason about
        // what would have been written. The anchor plan itself is exempt:
        // dropping it would break feature-tree navigation.
        const is_anchor = std.mem.eql(u8, e.kind, "plan") and e.id == anchor_plan_id;
        if (mode == .push and !is_anchor) {
            if (terminal_mod.isFilteredStr(e.kind, e.status, filter_mode)) |drop| {
                if (drop) {
                    summary.filtered += 1;
                    // Plan 439 M3: surprise-free upgrade path. If the
                    // filtered entity already has a file on disk, that's a
                    // pre-existing terminal artifact. It still belongs to
                    // this push's input corpus, so parse and aggregate it
                    // before applying the output filter or optional cleanup.
                    const pre_rel_path, const pre_db_content = try renderEntity(d, allocator, anchor_plan_id, e.kind, e.id);
                    defer allocator.free(pre_rel_path);
                    defer allocator.free(pre_db_content);
                    const pre_stored = try feature.storedPath(allocator, a.assoc_slug, a.plan_key, a.slug, pre_rel_path);
                    defer allocator.free(pre_stored);
                    const pre_stored_key = try allocator.dupe(u8, pre_stored);
                    errdefer allocator.free(pre_stored_key);
                    try seen_files.put(pre_stored_key, true);
                    try seen_file_keys.append(allocator, pre_stored_key);
                    const pre_abs = try std.fs.path.join(allocator, &.{ feature_dir, pre_rel_path });
                    defer allocator.free(pre_abs);
                    if (pathExists(pre_abs)) {
                        summary.pre_existing_terminal += 1;
                        const pre_fs_content = try readFileAlloc(allocator, pre_abs);
                        defer allocator.free(pre_fs_content);
                        const parsed = parse.parse(allocator, pre_fs_content) catch |parse_err| {
                            summary.malformed += 1;
                            try entries.append(allocator, .{
                                .class = .malformed,
                                .file_path = try allocator.dupe(u8, pre_stored),
                                .entity_kind = try allocator.dupe(u8, e.kind),
                                .entity_id = e.id,
                                .parse_error = try allocator.dupe(u8, @errorName(parse_err)),
                            });
                            if (apply_cleanup) {
                                const pre_abs_z = try allocator.dupeZ(u8, pre_abs);
                                defer allocator.free(pre_abs_z);
                                _ = c.unlink(pre_abs_z.ptr);
                                summary.cleaned += 1;
                                _ = d.execParams(
                                    "delete from workbench_sync_state where anchor_plan_id = ? and entity_kind = ? and entity_id = ?",
                                    &.{
                                        .{ .int = anchor_plan_id },
                                        .{ .text = e.kind },
                                        .{ .int = e.id },
                                    },
                                ) catch {};
                            }
                            continue;
                        };
                        parse.deinit(parsed, allocator);
                        if (apply_cleanup) {
                            const pre_abs_z = try allocator.dupeZ(u8, pre_abs);
                            defer allocator.free(pre_abs_z);
                            _ = c.unlink(pre_abs_z.ptr);
                            summary.cleaned += 1;
                            // Drop the manifest row so subsequent pushes
                            // do not treat the missing file as drift.
                            _ = d.execParams(
                                "delete from workbench_sync_state where anchor_plan_id = ? and entity_kind = ? and entity_id = ?",
                                &.{
                                    .{ .int = anchor_plan_id },
                                    .{ .text = e.kind },
                                    .{ .int = e.id },
                                },
                            ) catch {};
                        }
                    }
                    continue;
                }
            }
        }
        const rel_path, const db_content = try renderEntity(d, allocator, anchor_plan_id, e.kind, e.id);
        defer allocator.free(rel_path);
        defer allocator.free(db_content);
        const stored = try feature.storedPath(allocator, a.assoc_slug, a.plan_key, a.slug, rel_path);
        defer allocator.free(stored);
        const stored_key = try allocator.dupe(u8, stored);
        errdefer allocator.free(stored_key);
        try seen_files.put(stored_key, true);
        try seen_file_keys.append(allocator, stored_key);
        const abs = try std.fs.path.join(allocator, &.{ feature_dir, rel_path });
        defer allocator.free(abs);

        const fs_content = readFileAlloc(allocator, abs) catch |read_err| switch (read_err) {
            error.FileNotFound => null,
            else => return read_err,
        };
        defer if (fs_content) |fc| allocator.free(fc);
        const db_hash = try manifest.hashContent(allocator, db_content);
        defer allocator.free(db_hash);

        const parse_error: ?[]const u8 = if (fs_content) |content| check: {
            const parsed = parse.parse(allocator, content) catch |err| break :check @errorName(err);
            parse.deinit(parsed, allocator);
            break :check null;
        } else null;

        const cls: Classification = blk: {
            if (parse_error != null) break :blk .malformed;
            if (findState(manifest_rows, e.kind, e.id)) |state| {
                if (fs_content == null) break :blk .deleted_on_fs;
                const fs_hash = try manifest.hashContent(allocator, fs_content.?);
                defer allocator.free(fs_hash);
                const fs_changed = !std.mem.eql(u8, fs_hash, state.content_hash);
                const db_changed = !std.mem.eql(u8, e.updated_at, state.db_updated_at);
                if (!fs_changed and !db_changed) break :blk .no_op;
                if (fs_changed and !db_changed) break :blk .fs_to_db;
                if (!fs_changed and db_changed) break :blk .db_to_fs;
                if (std.mem.eql(u8, fs_hash, db_hash)) break :blk .no_op;
                break :blk .conflict;
            }
            if (fs_content == null) break :blk .db_to_fs;
            const fs_hash = try manifest.hashContent(allocator, fs_content.?);
            defer allocator.free(fs_hash);
            if (std.mem.eql(u8, fs_hash, db_hash)) break :blk .no_op;
            break :blk .fs_to_db;
        };

        var conflict_id: i64 = 0;
        switch (cls) {
            .no_op => {},
            .db_to_fs => {
                if (mode == .push or mode == .sync) {
                    try writeFileAtomic(allocator, abs, db_content);
                    try manifest.upsert(d, .{
                        .id = 0,
                        .anchor_plan_id = anchor_plan_id,
                        .entity_kind = e.kind,
                        .entity_id = e.id,
                        .file_path = stored,
                        .content_hash = db_hash,
                        .fs_mtime = "",
                        .db_updated_at = e.updated_at,
                        .last_synced_at = "",
                    });
                    summary.applied += 1;
                } else summary.pending += 1;
            },
            .fs_to_db => {
                if (mode == .pull or mode == .sync) {
                    if (fs_content) |fc| {
                        if (pullToDb(d, e.kind, e.id, fc) catch false) {
                            const new_updated = try fetchUpdatedAt(d, allocator, e.kind, e.id);
                            defer allocator.free(new_updated);
                            const fs_hash = try manifest.hashContent(allocator, fc);
                            defer allocator.free(fs_hash);
                            try manifest.upsert(d, .{
                                .id = 0,
                                .anchor_plan_id = anchor_plan_id,
                                .entity_kind = e.kind,
                                .entity_id = e.id,
                                .file_path = stored,
                                .content_hash = fs_hash,
                                .fs_mtime = "",
                                .db_updated_at = new_updated,
                                .last_synced_at = "",
                            });
                            summary.applied += 1;
                        } else summary.pending += 1;
                    } else summary.pending += 1;
                } else summary.pending += 1;
            },
            .conflict => {
                summary.conflicts += 1;
                summary.pending += 1;
                const fs_hash = if (fs_content) |fc| try manifest.hashContent(allocator, fc) else try allocator.dupe(u8, "");
                defer allocator.free(fs_hash);
                const fs_mtime = fileMtime(abs, allocator) catch try allocator.dupe(u8, "");
                defer allocator.free(fs_mtime);
                conflict_id = try ensureConflictEventID(
                    d,
                    allocator,
                    mode,
                    anchor_plan_id,
                    e.kind,
                    e.id,
                    stored,
                    fs_hash,
                    db_hash,
                    fs_mtime,
                    e.updated_at,
                );
            },
            .deleted_on_fs => {
                if (mode == .pull or mode == .sync) {
                    try softDeleteEntity(d, e.kind, e.id);
                    try manifest.deleteByFilePath(d, stored);
                    summary.applied += 1;
                } else summary.pending += 1;
            },
            .new_on_fs => summary.pending += 1,
            .malformed => summary.malformed += 1,
        }

        try entries.append(allocator, .{
            .class = cls,
            .file_path = try allocator.dupe(u8, stored),
            .entity_kind = try allocator.dupe(u8, e.kind),
            .entity_id = e.id,
            .conflict_id = conflict_id,
            .parse_error = if (parse_error) |err| try allocator.dupe(u8, err) else "",
        });
    }

    // Process manifest rows for entities no longer returned by DB enumeration.
    for (manifest_rows) |state| {
        if (seen_files.contains(state.file_path)) continue;
        const abs = try std.fs.path.join(allocator, &.{ root, state.file_path });
        defer allocator.free(abs);
        const fs_content = readFileAlloc(allocator, abs) catch |e| switch (e) {
            error.FileNotFound => null,
            else => return e,
        };
        defer if (fs_content) |fc| allocator.free(fc);
        if (fs_content != null) continue;
        if (mode == .pull or mode == .sync) {
            try softDeleteEntity(d, state.entity_kind, state.entity_id);
            try manifest.deleteByFilePath(d, state.file_path);
            summary.applied += 1;
        } else summary.pending += 1;
        try entries.append(allocator, .{
            .class = .deleted_on_fs,
            .file_path = try allocator.dupe(u8, state.file_path),
            .entity_kind = try allocator.dupe(u8, state.entity_kind),
            .entity_id = state.entity_id,
        });
    }

    // New-on-FS detection for pull/sync: only task files are auto-created.
    const fs_files = listMarkdownFiles(allocator, feature_dir) catch |e| switch (e) {
        error.FileNotFound => &[_][]const u8{},
        else => return e,
    };
    defer {
        for (fs_files) |p| allocator.free(p);
        if (fs_files.len > 0) allocator.free(fs_files);
    }
    for (fs_files) |abs_path| {
        const stored = try toStoredPath(allocator, abs_path, root);
        defer allocator.free(stored);
        if (seen_files.contains(stored)) continue;
        if (findStateByFilePath(manifest_rows, stored) != null) continue;
        const content = readFileAlloc(allocator, abs_path) catch |e| switch (e) {
            error.FileNotFound => continue,
            else => return e,
        };
        defer allocator.free(content);
        const parsed = parse.parse(allocator, content) catch |parse_err| {
            summary.malformed += 1;
            try entries.append(allocator, .{
                .class = .malformed,
                .file_path = try allocator.dupe(u8, stored),
                .entity_kind = try allocator.dupe(u8, ""),
                .entity_id = 0,
                .parse_error = try allocator.dupe(u8, @errorName(parse_err)),
            });
            continue;
        };
        defer parse.deinit(parsed, allocator);
        if (!std.mem.eql(u8, parsed.frontmatter.entity_kind, "task")) {
            summary.pending += 1;
            try entries.append(allocator, .{
                .class = .new_on_fs,
                .file_path = try allocator.dupe(u8, stored),
                .entity_kind = try allocator.dupe(u8, parsed.frontmatter.entity_kind),
                .entity_id = 0,
            });
            continue;
        }
        if (mode == .pull or mode == .sync) {
            const new_id = try insertTaskFromFrontmatter(d, parsed.frontmatter, parsed.body, a.assoc_id);
            _ = d.execParams(
                \\insert into entity_links (from_kind, from_id, to_kind, to_id, relationship)
                \\values ('task', ?, 'plan', ?, 'derives-from')
            , &.{ .{ .int = new_id }, .{ .int = anchor_plan_id } }) catch return error.QueryFailed;
            const new_updated = try fetchUpdatedAt(d, allocator, "task", new_id);
            defer allocator.free(new_updated);
            const h = try manifest.hashContent(allocator, content);
            defer allocator.free(h);
            try manifest.upsert(d, .{
                .id = 0,
                .anchor_plan_id = anchor_plan_id,
                .entity_kind = "task",
                .entity_id = new_id,
                .file_path = stored,
                .content_hash = h,
                .fs_mtime = "",
                .db_updated_at = new_updated,
                .last_synced_at = "",
            });
            summary.applied += 1;
            try entries.append(allocator, .{
                .class = .new_on_fs,
                .file_path = try allocator.dupe(u8, stored),
                .entity_kind = try allocator.dupe(u8, "task"),
                .entity_id = new_id,
            });
        } else {
            summary.pending += 1;
            try entries.append(allocator, .{
                .class = .new_on_fs,
                .file_path = try allocator.dupe(u8, stored),
                .entity_kind = try allocator.dupe(u8, "task"),
                .entity_id = 0,
            });
        }
    }

    if (mode != .status) {
        const rows = try manifest.load(d, allocator, anchor_plan_id);
        defer manifest.deinitRows(rows, allocator);
        try manifest.writeSyncFile(allocator, feature_dir, rows);
    }
    const owned_entries = try entries.toOwnedSlice(allocator);
    errdefer {
        for (owned_entries) |entry| {
            allocator.free(entry.file_path);
            allocator.free(entry.entity_kind);
            if (entry.parse_error.len > 0) allocator.free(entry.parse_error);
        }
        allocator.free(owned_entries);
    }
    const malformed_files = if (summary.malformed == 0)
        &[_]MalformedFile{}
    else blk: {
        const files = try allocator.alloc(MalformedFile, summary.malformed);
        var index: usize = 0;
        for (owned_entries) |entry| {
            if (entry.class != .malformed) continue;
            files[index] = .{ .path = entry.file_path, .parse_error = entry.parse_error };
            index += 1;
        }
        break :blk files;
    };
    return .{
        .applied = summary.applied,
        .pending = summary.pending,
        .conflicts = summary.conflicts,
        .malformed = summary.malformed,
        .malformed_files = malformed_files,
        .filtered = summary.filtered,
        .pre_existing_terminal = summary.pre_existing_terminal,
        .cleaned = summary.cleaned,
        .filter_mode = terminal_mod.Mode.toString(filter_mode),
        .entries = owned_entries,
    };
}

fn ensureConflictEventID(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    mode: Mode,
    anchor_plan_id: i64,
    entity_kind: []const u8,
    entity_id: i64,
    file_path: []const u8,
    fs_hash: []const u8,
    db_hash: []const u8,
    fs_mtime: []const u8,
    db_updated_at: []const u8,
) !i64 {
    const existing_id = try findPendingConflictEventID(d, allocator, anchor_plan_id, entity_kind, entity_id, file_path);
    if (existing_id > 0) return existing_id;

    const direction: []const u8 = switch (mode) {
        .push => "push",
        else => "pull",
    };
    const ctx = try std.fmt.allocPrint(allocator, "{{\"anchor_plan_id\":{d},\"entity_kind\":\"{s}\",\"entity_id\":{d},\"file_path\":\"{s}\",\"fs_hash\":\"{s}\",\"db_hash\":\"{s}\",\"fs_mtime\":\"{s}\",\"db_updated_at\":\"{s}\"}}", .{
        anchor_plan_id, entity_kind, entity_id, file_path, fs_hash, db_hash, fs_mtime, db_updated_at,
    });
    defer allocator.free(ctx);

    const inserted_id = d.execParams(
        \\insert into sync_events (link_id, scope, direction, outcome, context_json)
        \\values (null, 'workbench', ?, 'conflict', ?)
    , &.{ .{ .text = direction }, .{ .text = ctx } }) catch return error.QueryFailed;
    if (inserted_id <= 0) return error.ConflictEventIDUnavailable;
    return inserted_id;
}

fn findPendingConflictEventID(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
    entity_kind: []const u8,
    entity_id: i64,
    file_path: []const u8,
) !i64 {
    var stmt = d.prepare(
        \\select id, coalesce(context_json, '')
        \\from sync_events
        \\where scope='workbench' and outcome='conflict'
        \\order by id desc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => return 0,
            .row => {
                const id = stmt.columnInt(0);
                const context_json = try stmt.columnTextAlloc(1, allocator);
                defer allocator.free(context_json);
                const parsed = parseConflictContextOwned(allocator, context_json) catch continue;
                defer {
                    allocator.free(parsed.entity_kind);
                    allocator.free(parsed.file_path);
                }
                if (parsed.anchor_plan_id == anchor_plan_id and
                    parsed.entity_id == entity_id and
                    std.mem.eql(u8, parsed.entity_kind, entity_kind) and
                    std.mem.eql(u8, parsed.file_path, file_path))
                {
                    return id;
                }
            },
        }
    }
}

const ConflictContext = struct {
    anchor_plan_id: i64,
    entity_kind: []const u8,
    entity_id: i64,
    file_path: []const u8,
};

fn parseConflictContextOwned(allocator: std.mem.Allocator, raw: []const u8) !ConflictContext {
    const Parsed = struct {
        anchor_plan_id: i64,
        entity_kind: []const u8,
        entity_id: i64,
        file_path: []const u8,
    };
    const parsed = try std.json.parseFromSlice(Parsed, allocator, raw, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();
    return .{
        .anchor_plan_id = parsed.value.anchor_plan_id,
        .entity_kind = try allocator.dupe(u8, parsed.value.entity_kind),
        .entity_id = parsed.value.entity_id,
        .file_path = try allocator.dupe(u8, parsed.value.file_path),
    };
}

fn parseConflictContext(raw: []const u8) !ConflictContext {
    const Parsed = struct {
        anchor_plan_id: i64,
        entity_kind: []const u8,
        entity_id: i64,
        file_path: []const u8,
    };
    const parsed = try std.json.parseFromSlice(Parsed, std.heap.page_allocator, raw, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();
    return .{
        .anchor_plan_id = parsed.value.anchor_plan_id,
        .entity_kind = try std.heap.page_allocator.dupe(u8, parsed.value.entity_kind),
        .entity_id = parsed.value.entity_id,
        .file_path = try std.heap.page_allocator.dupe(u8, parsed.value.file_path),
    };
}

fn pullToDb(d: *db.sqlite.Db, kind: []const u8, id: i64, file_content: []const u8) !bool {
    const a = std.heap.page_allocator;
    d.savepoint(a, "workbench_pull_entity") catch return false;
    var committed = false;
    defer if (!committed) {
        d.rollbackToSavepoint(a, "workbench_pull_entity") catch {};
        d.releaseSavepoint(a, "workbench_pull_entity") catch {};
    };
    const parsed = parse.parse(std.heap.page_allocator, file_content) catch return false;
    defer parse.deinit(parsed, std.heap.page_allocator);
    const body = extractBodyText(parsed.body);
    if (std.mem.eql(u8, kind, "task")) {
        if (parsed.frontmatter.status.len == 0) {
            _ = d.execParams(
                "update tasks set body=?, updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?",
                &.{ .{ .text = body }, .{ .int = id } },
            ) catch return false;
        } else {
            _ = d.execParams(
                "update tasks set body=?, status=?, updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?",
                &.{ .{ .text = body }, .{ .text = parsed.frontmatter.status }, .{ .int = id } },
            ) catch return false;
        }
        reconcileTouchesFromFM(d, id, parsed.frontmatter.touches) catch return false;
        d.releaseSavepoint(a, "workbench_pull_entity") catch return false;
        committed = true;
        return true;
    }
    if (std.mem.eql(u8, kind, "plan")) {
        if (parsed.frontmatter.status.len == 0) {
            _ = d.execParams(
                "update plans set summary=?, updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?",
                &.{ .{ .text = body }, .{ .int = id } },
            ) catch return false;
        } else {
            _ = d.execParams(
                "update plans set summary=?, status=?, updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?",
                &.{ .{ .text = body }, .{ .text = parsed.frontmatter.status }, .{ .int = id } },
            ) catch return false;
        }
        d.releaseSavepoint(a, "workbench_pull_entity") catch return false;
        committed = true;
        return true;
    }
    if (std.mem.eql(u8, kind, "artifact")) {
        _ = d.execParams("update artifacts set body=?, updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?", &.{ .{ .text = body }, .{ .int = id } }) catch return false;
        d.releaseSavepoint(a, "workbench_pull_entity") catch return false;
        committed = true;
        return true;
    }
    if (std.mem.eql(u8, kind, "decision")) {
        _ = d.execParams("update decisions set body=?, updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?", &.{ .{ .text = body }, .{ .int = id } }) catch return false;
        d.releaseSavepoint(a, "workbench_pull_entity") catch return false;
        committed = true;
        return true;
    }
    if (std.mem.eql(u8, kind, "question")) {
        _ = d.execParams("update questions set body=?, updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?", &.{ .{ .text = body }, .{ .int = id } }) catch return false;
        d.releaseSavepoint(a, "workbench_pull_entity") catch return false;
        committed = true;
        return true;
    }
    if (std.mem.eql(u8, kind, "scenario")) {
        _ = d.execParams("update test_scenarios set body=?, updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?", &.{ .{ .text = body }, .{ .int = id } }) catch return false;
        d.releaseSavepoint(a, "workbench_pull_entity") catch return false;
        committed = true;
        return true;
    }
    return false;
}

fn insertTaskFromFrontmatter(d: *db.sqlite.Db, fm: parse.FrontMatter, body: []const u8, assoc_id: ?i64) !i64 {
    const title = if (fm.title.len == 0) "Task from workbench" else fm.title;
    const status_txt = if (fm.status.len == 0) "todo" else fm.status;
    const body_text = extractBodyText(body);
    const priority = if (fm.priority == 0) 100 else fm.priority;
    const new_id = if (assoc_id) |sid|
        d.execParams(
            "insert into tasks (scope_kind, scope_id, title, body, status, priority) values ('association', ?, ?, ?, ?, ?)",
            &.{ .{ .int = sid }, .{ .text = title }, .{ .text = body_text }, .{ .text = status_txt }, .{ .int = priority } },
        ) catch return error.QueryFailed
    else
        d.execParams(
            "insert into tasks (scope_kind, scope_id, title, body, status, priority) values ('global', null, ?, ?, ?, ?)",
            &.{ .{ .text = title }, .{ .text = body_text }, .{ .text = status_txt }, .{ .int = priority } },
        ) catch return error.QueryFailed;
    try reconcileTouchesFromFM(d, new_id, fm.touches);
    return new_id;
}

fn reconcileTouchesFromFM(d: *db.sqlite.Db, task_id: i64, slugs: []const []const u8) !void {
    var want_ids: std.AutoHashMap(i64, bool) = .init(std.heap.page_allocator);
    defer want_ids.deinit();
    for (slugs) |slug| {
        const repo_id = resolveProjectIDBySlug(d, slug) catch |err| switch (err) {
            error.NotFound => continue,
            else => return err,
        };
        try want_ids.put(repo_id, true);
    }

    var have_ids: std.AutoHashMap(i64, bool) = .init(std.heap.page_allocator);
    defer have_ids.deinit();
    var stmt = d.prepare(
        \\select to_id from entity_links
        \\where from_kind='task' and from_id=? and to_kind='repo' and relationship='touches'
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try have_ids.put(stmt.columnInt(0), true),
        }
    }

    var want_it = want_ids.iterator();
    while (want_it.next()) |entry| {
        const repo_id = entry.key_ptr.*;
        if (!have_ids.contains(repo_id)) {
            _ = d.execParams(
                \\insert or ignore into entity_links (from_kind, from_id, to_kind, to_id, relationship)
                \\values ('task', ?, 'repo', ?, 'touches')
            , &.{ .{ .int = task_id }, .{ .int = repo_id } }) catch return error.QueryFailed;
        }
    }

    var have_it = have_ids.iterator();
    while (have_it.next()) |entry| {
        const repo_id = entry.key_ptr.*;
        if (!want_ids.contains(repo_id)) {
            _ = d.execParams(
                \\delete from entity_links
                \\where from_kind='task' and from_id=? and to_kind='repo' and to_id=? and relationship='touches'
            , &.{ .{ .int = task_id }, .{ .int = repo_id } }) catch return error.QueryFailed;
        }
    }
}

fn resolveProjectIDBySlug(d: *db.sqlite.Db, slug: []const u8) !i64 {
    var stmt = d.prepare("select id from projects where slug=?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => return stmt.columnInt(0),
    }
}

fn tableForKind(kind: []const u8) ?[]const u8 {
    if (std.mem.eql(u8, kind, "plan")) return "plans";
    if (std.mem.eql(u8, kind, "task")) return "tasks";
    if (std.mem.eql(u8, kind, "artifact")) return "artifacts";
    if (std.mem.eql(u8, kind, "decision")) return "decisions";
    if (std.mem.eql(u8, kind, "question")) return "questions";
    if (std.mem.eql(u8, kind, "scenario")) return "test_scenarios";
    return null;
}

fn findStateByFilePath(rows: []const manifest.SyncState, file_path: []const u8) ?manifest.SyncState {
    for (rows) |r| {
        if (std.mem.eql(u8, r.file_path, file_path)) return r;
    }
    return null;
}

fn softDeleteEntity(d: *db.sqlite.Db, kind: []const u8, id: i64) !void {
    const q: []const u8 = if (std.mem.eql(u8, kind, "task"))
        "update tasks set status='cancelled', updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?"
    else if (std.mem.eql(u8, kind, "plan"))
        "update plans set status='abandoned', updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?"
    else if (std.mem.eql(u8, kind, "artifact"))
        "update artifacts set status='retired', updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?"
    else if (std.mem.eql(u8, kind, "scenario"))
        // INTENTIONAL BYPASS of policy.status.check: softDeleteEntity can only
        // ever target `retired`, which is a legal destination from every
        // non-retired scenario status, and an identity (no-op) from retired
        // itself. Adding the check here would require an allocator parameter
        // propagating to all callers. The bypass is safe by construction.
        "update test_scenarios set status='retired', updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?"
    else if (std.mem.eql(u8, kind, "question"))
        "update questions set status='wontfix', updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?"
    else if (std.mem.eql(u8, kind, "decision"))
        "update decisions set status='withdrawn', updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?"
    else
        return;
    const qz = try std.heap.page_allocator.dupeZ(u8, q);
    defer std.heap.page_allocator.free(qz);
    _ = d.execParams(qz, &.{.{ .int = id }}) catch return error.QueryFailed;
}

pub const RenderedEntity = struct {
    rel_path: []const u8,
    content: []const u8,

    pub fn deinit(self: RenderedEntity, allocator: std.mem.Allocator) void {
        allocator.free(self.rel_path);
        allocator.free(self.content);
    }
};

pub fn renderEntityCanonical(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
    kind: []const u8,
    id: i64,
) !RenderedEntity {
    const rel_path, const content = try renderEntity(d, allocator, anchor_plan_id, kind, id);
    return .{
        .rel_path = rel_path,
        .content = content,
    };
}

fn renderEntity(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64, kind: []const u8, id: i64) !struct { []const u8, []const u8 } {
    if (std.mem.eql(u8, kind, "plan")) return renderPlan(d, allocator, anchor_plan_id, id);
    if (std.mem.eql(u8, kind, "task")) return renderTask(d, allocator, anchor_plan_id, id);
    if (std.mem.eql(u8, kind, "artifact")) return renderArtifact(d, allocator, anchor_plan_id, id);
    if (std.mem.eql(u8, kind, "question")) return renderQuestion(d, allocator, anchor_plan_id, id);
    if (std.mem.eql(u8, kind, "decision")) return renderDecision(d, allocator, anchor_plan_id, id);
    if (std.mem.eql(u8, kind, "scenario")) return renderScenario(d, allocator, anchor_plan_id, id);
    return error.NotFound;
}

fn renderPlan(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64, id: i64) !struct { []const u8, []const u8 } {
    var stmt = d.prepare("select coalesce(title,''), coalesce(slug,''), coalesce(summary,''), coalesce(status,''), coalesce(created_at,''), coalesce(updated_at,'') from plans where id=?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const slug = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(slug);
            const summary_txt = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(summary_txt);
            const status_txt = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(status_txt);
            const created_at = try stmt.columnTextAlloc(4, allocator);
            defer allocator.free(created_at);
            const updated_at = try stmt.columnTextAlloc(5, allocator);
            defer allocator.free(updated_at);
            const fm = render.FrontMatter{ .entity_kind = "plan", .entity_id = id, .anchor_plan_id = anchor_plan_id, .title = title, .status = status_txt };
            const body = try std.fmt.allocPrint(
                allocator,
                "# Plan {d}: {s}\n\n**Status:** {s}  \n**Created:** {s}  \n**Updated:** {s}\n{s}{s}\n",
                .{
                    id,
                    title,
                    status_txt,
                    created_at,
                    updated_at,
                    if (summary_txt.len > 0) "\n" else "",
                    summary_txt,
                },
            );
            defer allocator.free(body);
            const content = try render.render(allocator, fm, body);
            const rel = if (id == anchor_plan_id) try allocator.dupe(u8, "README.md") else try std.fmt.allocPrint(allocator, "plans/{s}.md", .{slug});
            return .{ rel, content };
        },
    }
}

fn renderTask(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64, id: i64) !struct { []const u8, []const u8 } {
    var stmt = d.prepare("select scope_kind, scope_id, coalesce(title,''), coalesce(body,''), coalesce(status,''), priority, coalesce(next_action,''), coalesce(due_at,''), coalesce(created_at,''), coalesce(updated_at,'') from tasks where id=?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const scope_kind = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(scope_kind);
            const scope_id = stmt.columnIntOpt(1);
            const title = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(title);
            const body_text = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(body_text);
            const status_txt = try stmt.columnTextAlloc(4, allocator);
            defer allocator.free(status_txt);
            const priority = stmt.columnInt(5);
            const next_action = try stmt.columnTextAlloc(6, allocator);
            defer allocator.free(next_action);
            const due_at = try stmt.columnTextAlloc(7, allocator);
            defer allocator.free(due_at);
            const created_at = try stmt.columnTextAlloc(8, allocator);
            defer allocator.free(created_at);
            const updated_at = try stmt.columnTextAlloc(9, allocator);
            defer allocator.free(updated_at);
            const slug = try feature.slugify(allocator, title);
            defer allocator.free(slug);
            const touches = try loadTaskTouches(d, allocator, id);
            defer {
                for (touches) |touch| allocator.free(touch);
                allocator.free(touches);
            }
            const task_dir = try resolveTaskDir(d, allocator, scope_kind, scope_id, touches);
            defer allocator.free(task_dir);
            const fm = render.FrontMatter{
                .entity_kind = "task",
                .entity_id = id,
                .anchor_plan_id = anchor_plan_id,
                .title = title,
                .status = status_txt,
                .priority = priority,
                .touches = touches,
            };
            const body = try std.fmt.allocPrint(
                allocator,
                "# Task {d}: {s}\n\n**Status:** {s}  \n**Priority:** {d}  \n**Created:** {s}  \n**Updated:** {s}\n{s}{s}{s}{s}{s}{s}{s}\n",
                .{
                    id,
                    title,
                    status_txt,
                    priority,
                    created_at,
                    updated_at,
                    if (due_at.len > 0) "**Due:** " else "",
                    if (due_at.len > 0) due_at else "",
                    if (due_at.len > 0) "\n" else "",
                    if (body_text.len > 0) "\n" else "",
                    body_text,
                    if (next_action.len > 0) "\n**Next action:** " else "",
                    if (next_action.len > 0) next_action else "",
                },
            );
            defer allocator.free(body);
            return .{
                try std.fmt.allocPrint(allocator, "{s}/{d}-{s}.md", .{ task_dir, id, slug }),
                try render.render(allocator, fm, body),
            };
        },
    }
}

fn renderArtifact(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64, id: i64) !struct { []const u8, []const u8 } {
    var stmt = d.prepare("select coalesce(title,''), coalesce(status,''), coalesce(kind,''), coalesce(body,'') from artifacts where id=?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const status_txt = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(status_txt);
            const kind_txt = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(kind_txt);
            const body_txt = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(body_txt);
            const file_name = try feature.artifactFilename(allocator, id, title, kind_txt);
            defer allocator.free(file_name);
            const fm = render.FrontMatter{
                .entity_kind = "artifact",
                .entity_id = id,
                .anchor_plan_id = anchor_plan_id,
                .title = title,
                .status = status_txt,
                .artifact_kind = kind_txt,
            };
            const body = try std.fmt.allocPrint(allocator, "# Artifact {d}: {s}\n\n**Kind:** {s}  \n**Status:** {s}\n\n## Content\n\n{s}\n", .{ id, title, kind_txt, status_txt, body_txt });
            defer allocator.free(body);
            // Plan 314 task 2319: write artifacts at the feature-dir root
            // (matching Go's `internal/workbench/render.renderArtifact`).
            // Previously zig wrote them under `artifacts/<file>`; the two
            // binaries are converged here so the spec/ingest readers no
            // longer need the subdir-then-root fallback.
            return .{
                try allocator.dupe(u8, file_name),
                try render.render(allocator, fm, body),
            };
        },
    }
}

fn renderDecision(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64, id: i64) !struct { []const u8, []const u8 } {
    var stmt = d.prepare("select coalesce(title,''), coalesce(status,''), coalesce(body,''), coalesce(rationale,''), coalesce(created_at,''), coalesce(updated_at,'') from decisions where id=?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const status_txt = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(status_txt);
            const body_txt = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(body_txt);
            const rationale = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(rationale);
            const slug = try feature.slugify(allocator, title);
            defer allocator.free(slug);
            const fm = render.FrontMatter{ .entity_kind = "decision", .entity_id = id, .anchor_plan_id = anchor_plan_id, .title = title, .status = status_txt };
            const created_at = try stmt.columnTextAlloc(4, allocator);
            defer allocator.free(created_at);
            const updated_at = try stmt.columnTextAlloc(5, allocator);
            defer allocator.free(updated_at);
            const body = try std.fmt.allocPrint(allocator, "# Decision {d}: {s}\n\n**Status:** {s}  \n**Created:** {s}  \n**Updated:** {s}\n\n## Body\n\n{s}\n\n## Rationale\n\n{s}\n", .{ id, title, status_txt, created_at, updated_at, body_txt, rationale });
            defer allocator.free(body);
            return .{
                try std.fmt.allocPrint(allocator, "decisions/{d}-{s}.md", .{ id, slug }),
                try render.render(allocator, fm, body),
            };
        },
    }
}

fn renderQuestion(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64, id: i64) !struct { []const u8, []const u8 } {
    var stmt = d.prepare("select coalesce(title,''), coalesce(status,''), coalesce(body,''), coalesce(answer_body,''), coalesce(answered_at,''), coalesce(created_at,''), coalesce(updated_at,'') from questions where id=?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const status_txt = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(status_txt);
            const body_txt = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(body_txt);
            const answer_body = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(answer_body);
            const answered_at = try stmt.columnTextAlloc(4, allocator);
            defer allocator.free(answered_at);
            const created_at = try stmt.columnTextAlloc(5, allocator);
            defer allocator.free(created_at);
            const updated_at = try stmt.columnTextAlloc(6, allocator);
            defer allocator.free(updated_at);
            const slug = try feature.slugify(allocator, title);
            defer allocator.free(slug);
            const fm = render.FrontMatter{ .entity_kind = "question", .entity_id = id, .anchor_plan_id = anchor_plan_id, .title = title, .status = status_txt };
            var body_builder: std.ArrayList(u8) = .empty;
            defer body_builder.deinit(allocator);
            const q_head = try std.fmt.allocPrint(allocator, "# Question {d}: {s}\n\n**Status:** {s}  \n**Created:** {s}  \n**Updated:** {s}\n", .{ id, title, status_txt, created_at, updated_at });
            defer allocator.free(q_head);
            try body_builder.appendSlice(allocator, q_head);
            if (body_txt.len > 0) {
                const q_body = try std.fmt.allocPrint(allocator, "\n{s}\n", .{body_txt});
                defer allocator.free(q_body);
                try body_builder.appendSlice(allocator, q_body);
            }
            if (answer_body.len > 0) {
                const q_ans = try std.fmt.allocPrint(allocator, "\n**Answer:** {s}\n", .{answer_body});
                defer allocator.free(q_ans);
                try body_builder.appendSlice(allocator, q_ans);
                if (answered_at.len > 0) {
                    const q_at = try std.fmt.allocPrint(allocator, "\n**Answered at:** {s}\n", .{answered_at});
                    defer allocator.free(q_at);
                    try body_builder.appendSlice(allocator, q_at);
                }
            }
            const body = try body_builder.toOwnedSlice(allocator);
            defer allocator.free(body);
            return .{
                try std.fmt.allocPrint(allocator, "questions/{d}-{s}.md", .{ id, slug }),
                try render.render(allocator, fm, body),
            };
        },
    }
}

fn renderScenario(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64, id: i64) !struct { []const u8, []const u8 } {
    var stmt = d.prepare("select coalesce(title,''), coalesce(status,''), coalesce(body,''), coalesce(last_run_at,''), coalesce(last_outcome,''), coalesce(created_at,''), coalesce(updated_at,'') from test_scenarios where id=?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(title);
            const status_txt = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(status_txt);
            const body_txt = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(body_txt);
            const last_run_at = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(last_run_at);
            const last_outcome = try stmt.columnTextAlloc(4, allocator);
            defer allocator.free(last_outcome);
            const created_at = try stmt.columnTextAlloc(5, allocator);
            defer allocator.free(created_at);
            const updated_at = try stmt.columnTextAlloc(6, allocator);
            defer allocator.free(updated_at);
            const slug = try feature.slugify(allocator, title);
            defer allocator.free(slug);
            const fm = render.FrontMatter{ .entity_kind = "scenario", .entity_id = id, .anchor_plan_id = anchor_plan_id, .title = title, .status = status_txt };
            var body_builder: std.ArrayList(u8) = .empty;
            defer body_builder.deinit(allocator);
            const s_head = try std.fmt.allocPrint(allocator, "# Scenario {d}: {s}\n\n**Status:** {s}  \n**Created:** {s}  \n**Updated:** {s}\n", .{ id, title, status_txt, created_at, updated_at });
            defer allocator.free(s_head);
            try body_builder.appendSlice(allocator, s_head);
            if (last_run_at.len > 0) {
                const s_run = try std.fmt.allocPrint(allocator, "\n**Last run:** {s}", .{last_run_at});
                defer allocator.free(s_run);
                try body_builder.appendSlice(allocator, s_run);
            }
            if (last_outcome.len > 0) {
                const s_outcome = try std.fmt.allocPrint(allocator, "  \n**Last outcome:** {s}", .{last_outcome});
                defer allocator.free(s_outcome);
                try body_builder.appendSlice(allocator, s_outcome);
            }
            if (body_txt.len > 0) {
                const s_body = try std.fmt.allocPrint(allocator, "\n\n{s}\n", .{body_txt});
                defer allocator.free(s_body);
                try body_builder.appendSlice(allocator, s_body);
            }
            const body = try body_builder.toOwnedSlice(allocator);
            defer allocator.free(body);
            return .{
                try std.fmt.allocPrint(allocator, "scenarios/{d}-{s}.md", .{ id, slug }),
                try render.render(allocator, fm, body),
            };
        },
    }
}

fn fetchAnchor(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) !Anchor {
    var stmt = d.prepare(
        \\select p.id, coalesce(p.slug,''), coalesce(a.slug,''), p.scope_kind, p.scope_id
        \\from plans p
        \\left join associations a on (p.scope_kind='association' and a.id=p.scope_id)
        \\where p.id=? and p.parent_plan_id is null
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            return .{
                .id = stmt.columnInt(0),
                .slug = try stmt.columnTextAlloc(1, allocator),
                .assoc_slug = try stmt.columnTextAlloc(2, allocator),
                .assoc_id = blk: {
                    const scope_kind = try stmt.columnTextAlloc(3, allocator);
                    defer allocator.free(scope_kind);
                    if (!std.mem.eql(u8, scope_kind, "association")) break :blk null;
                    break :blk stmt.columnIntOpt(4);
                },
                .plan_key = try resolvePlanKey(d, allocator, anchor_plan_id),
            };
        },
    }
}

fn freeAnchor(allocator: std.mem.Allocator, a: Anchor) void {
    allocator.free(a.slug);
    allocator.free(a.assoc_slug);
    allocator.free(a.plan_key);
}

fn resolvePlanKey(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) ![]const u8 {
    var stmt = d.prepare("select external_id from external_links where entity_kind='plan' and entity_id=? order by id limit 1") catch return std.fmt.allocPrint(allocator, "p{d}", .{plan_id});
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return std.fmt.allocPrint(allocator, "p{d}", .{plan_id});
    switch (stmt.step() catch .done) {
        .done => return std.fmt.allocPrint(allocator, "p{d}", .{plan_id}),
        .row => return stmt.columnTextAlloc(0, allocator),
    }
}

fn enumerateEntities(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) ![]Entity {
    var out: std.ArrayList(Entity) = .empty;
    errdefer freeEntities(allocator, out.items);
    const anchor_updated = try fetchUpdatedAt(d, allocator, "plan", anchor_plan_id);
    try out.append(allocator, .{
        .kind = try allocator.dupe(u8, "plan"),
        .id = anchor_plan_id,
        .updated_at = anchor_updated,
        .status = try fetchStatus(d, allocator, "plan", anchor_plan_id),
    });

    try appendDerivedEntities(d, allocator, anchor_plan_id, &out);
    try appendPlanTasks(d, allocator, anchor_plan_id, &out);

    var child_ids: std.ArrayList(i64) = .empty;
    defer child_ids.deinit(allocator);
    var child_stmt = d.prepare("select id, updated_at from plans where parent_plan_id=? order by id") catch return error.QueryFailed;
    defer child_stmt.finalize();
    child_stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return error.QueryFailed;
    while (true) {
        switch (child_stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const child_id = child_stmt.columnInt(0);
                const child_updated = try child_stmt.columnTextAlloc(1, allocator);
                try child_ids.append(allocator, child_id);
                if (!containsEntity(out.items, "plan", child_id)) {
                    try out.append(allocator, .{
                        .kind = try allocator.dupe(u8, "plan"),
                        .id = child_id,
                        .updated_at = child_updated,
                        .status = try fetchStatus(d, allocator, "plan", child_id),
                    });
                } else allocator.free(child_updated);
            },
        }
    }
    for (child_ids.items) |child_id| {
        try appendDerivedEntities(d, allocator, child_id, &out);
        try appendPlanTasks(d, allocator, child_id, &out);
    }
    return try out.toOwnedSlice(allocator);
}

/// Append tasks attached to `plan_id` via the direct `tasks.plan_id` column.
/// Unlike artifacts / decisions / scenarios which reach plans through
/// `entity_links`, tasks carry their plan reference inline; without this
/// helper the workbench would never enumerate them, and the terminal-status
/// filter would silently ignore cancelled tasks (plan 439 follow-up).
fn appendPlanTasks(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64, out: *std.ArrayList(Entity)) !void {
    var stmt = d.prepare(
        \\select id, coalesce(updated_at, ''), coalesce(status, '')
        \\from tasks
        \\where plan_id = ?
        \\order by id
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                if (containsEntity(out.items, "task", id)) continue;
                const updated = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(updated);
                const status_txt = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(status_txt);
                try out.append(allocator, .{
                    .kind = try allocator.dupe(u8, "task"),
                    .id = id,
                    .updated_at = updated,
                    .status = status_txt,
                });
            },
        }
    }
}

fn appendDerivedEntities(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64, out: *std.ArrayList(Entity)) !void {
    var stmt = d.prepare(
        \\select case when from_kind='test_scenario' then 'scenario' else from_kind end, from_id
        \\from entity_links
        \\where to_kind='plan' and to_id=? and relationship='derives-from'
        \\order by from_kind, from_id
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const k = try stmt.columnTextAlloc(0, allocator);
                const id = stmt.columnInt(1);
                if (!containsEntity(out.items, k, id)) {
                    try out.append(allocator, .{
                        .kind = k,
                        .id = id,
                        .updated_at = try fetchUpdatedAt(d, allocator, k, id),
                        .status = try fetchStatus(d, allocator, k, id),
                    });
                } else allocator.free(k);
            },
        }
    }
}

fn containsEntity(items: []const Entity, kind: []const u8, id: i64) bool {
    for (items) |e| if (e.id == id and std.mem.eql(u8, e.kind, kind)) return true;
    return false;
}

fn loadTaskTouches(d: *db.sqlite.Db, allocator: std.mem.Allocator, task_id: i64) ![]const []const u8 {
    var stmt = d.prepare(
        \\select p.slug
        \\from entity_links el
        \\join projects p on p.id = el.to_id
        \\where el.from_kind='task' and el.from_id=? and el.to_kind='repo' and el.relationship='touches'
        \\order by el.id
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;
    var out: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try stmt.columnTextAlloc(0, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn resolveTaskDir(d: *db.sqlite.Db, allocator: std.mem.Allocator, scope_kind: []const u8, scope_id: ?i64, touches: []const []const u8) ![]const u8 {
    if (std.mem.eql(u8, scope_kind, "repo") and scope_id != null) {
        var stmt = d.prepare("select slug from projects where id=?") catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = scope_id.? }}) catch return error.QueryFailed;
        switch (stmt.step() catch return error.QueryFailed) {
            .done => {},
            .row => {
                const slug = try stmt.columnTextAlloc(0, allocator);
                defer allocator.free(slug);
                const safe = try repoSlugToFS(allocator, slug);
                defer allocator.free(safe);
                return std.fmt.allocPrint(allocator, "tasks/{s}", .{safe});
            },
        }
    }
    if (touches.len > 0) {
        const safe = try repoSlugToFS(allocator, touches[0]);
        defer allocator.free(safe);
        return std.fmt.allocPrint(allocator, "tasks/{s}", .{safe});
    }
    return allocator.dupe(u8, "tasks/cross");
}

fn repoSlugToFS(allocator: std.mem.Allocator, slug: []const u8) ![]const u8 {
    const out = try allocator.dupe(u8, slug);
    for (out) |*ch| {
        if (ch.* == '/') ch.* = '_';
    }
    return out;
}

fn freeEntities(allocator: std.mem.Allocator, entities: []const Entity) void {
    for (entities) |e| {
        allocator.free(e.kind);
        allocator.free(e.updated_at);
        allocator.free(e.status);
    }
    allocator.free(entities);
}

/// Fetch the current `status` column for an entity. Returns an empty string
/// when the kind has no status column or the row is missing.
fn fetchStatus(d: *db.sqlite.Db, allocator: std.mem.Allocator, kind: []const u8, id: i64) ![]const u8 {
    const sql = if (std.mem.eql(u8, kind, "plan"))
        "select coalesce(status, '') from plans where id = ?"
    else if (std.mem.eql(u8, kind, "task"))
        "select coalesce(status, '') from tasks where id = ?"
    else if (std.mem.eql(u8, kind, "decision"))
        "select coalesce(status, '') from decisions where id = ?"
    else if (std.mem.eql(u8, kind, "question"))
        "select coalesce(status, '') from questions where id = ?"
    else if (std.mem.eql(u8, kind, "scenario") or std.mem.eql(u8, kind, "test_scenario"))
        "select coalesce(status, '') from test_scenarios where id = ?"
    else if (std.mem.eql(u8, kind, "artifact"))
        "select coalesce(status, '') from artifacts where id = ?"
    else
        return allocator.dupe(u8, "");

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;

    return switch (stmt.step() catch return error.QueryFailed) {
        .done => allocator.dupe(u8, ""),
        .row => try stmt.columnTextAlloc(0, allocator),
    };
}

fn fetchUpdatedAt(d: *db.sqlite.Db, allocator: std.mem.Allocator, kind: []const u8, id: i64) ![]const u8 {
    const table = tableForKind(kind) orelse return error.NotFound;
    var sql_buf: [128]u8 = undefined;
    const sql = try std.fmt.bufPrint(&sql_buf, "select updated_at from {s} where id=?", .{table});
    const sql_z = try allocator.dupeZ(u8, sql);
    defer allocator.free(sql_z);
    var stmt = d.prepare(sql_z) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => return stmt.columnTextAlloc(0, allocator),
    }
}

fn writeFileAtomic(allocator: std.mem.Allocator, abs_path: []const u8, content: []const u8) !void {
    if (std.fs.path.dirname(abs_path)) |dir| try makePathAll(dir);
    const tmp = try std.fmt.allocPrint(allocator, "{s}.tmp", .{abs_path});
    defer allocator.free(tmp);
    try writeFile(tmp, content);
    const tmp_z = try allocator.dupeZ(u8, tmp);
    defer allocator.free(tmp_z);
    const abs_z = try allocator.dupeZ(u8, abs_path);
    defer allocator.free(abs_z);
    if (c.rename(tmp_z.ptr, abs_z.ptr) != 0) return error.RenameFailed;
}

fn resolveWorkbenchRoot(allocator: std.mem.Allocator) ![]const u8 {
    return resolveRoot(allocator, processEnviron());
}

/// Wrap std.c.environ into a std.process.Environ for use in engine-internal
/// callers that do not have an injected environ (e.g. sync.run()).
fn processEnviron() std.process.Environ {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var count: usize = 0;
    while (raw[count] != null) : (count += 1) {}
    const slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..count :null]);
    return .{ .block = .{ .slice = slice } };
}

/// resolveRoot resolves the workbench root directory using the canonical
/// three-layer precedence:
///   1. $PLANAR_WORKBENCH_ROOT env var (with `~/` expansion)
///   2. `workbench.root` in the config file at $PLANAR_CONFIG_PATH or
///      $HOME/.planar/config.toml (with `~/` expansion)
///   3. $HOME/.planar/workbench (built-in default)
///
/// Returns error.WorkbenchRootUnresolved when none of the three layers
/// can produce a path (no env var, no readable/non-empty config root,
/// and no HOME in the environment). Never returns "." or any cwd.
///
/// This is the single-source resolver; all call sites in the codebase
/// must use this function rather than maintaining local copies.
pub fn resolveRoot(allocator: std.mem.Allocator, environ: std.process.Environ) ![]const u8 {
    const config_parse = @import("../config/parse.zig");

    // Layer 1: env var.
    if (environ.getPosix("PLANAR_WORKBENCH_ROOT")) |raw| {
        if (raw.len > 0) return try expandTildeRoot(allocator, raw, environ);
    }

    // Layer 2: config file workbench.root.
    // Resolve the config file path ($PLANAR_CONFIG_PATH or $HOME/.planar/config.toml).
    const cfg_path_opt: ?[]const u8 = blk: {
        if (environ.getPosix("PLANAR_CONFIG_PATH")) |raw| {
            if (raw.len > 0) {
                break :blk try expandTildeRoot(allocator, raw, environ);
            }
        }
        if (environ.getPosix("HOME")) |home| {
            break :blk try std.fs.path.join(allocator, &.{ home, ".planar", "config.toml" });
        }
        break :blk null;
    };
    defer if (cfg_path_opt) |p| allocator.free(p);

    if (cfg_path_opt) |cfg_path| {
        // Attempt to read and parse the config file; silently skip if absent/unreadable.
        const content = readFileAlloc(allocator, cfg_path) catch null;
        if (content) |fc| {
            defer allocator.free(fc);
            var pe: config_parse.ParseError = undefined;
            var map = config_parse.parse(allocator, fc, &pe) catch null;
            if (map) |*m| {
                defer config_parse.deinitMap(m, allocator);
                if (m.get("workbench.root")) |val| {
                    const raw: []const u8 = switch (val) {
                        .string => |s| s,
                        else => "",
                    };
                    if (raw.len > 0) {
                        return try expandTildeRoot(allocator, raw, environ);
                    }
                }
            }
        }
    }

    // Layer 3: $HOME default.
    if (environ.getPosix("HOME")) |home| {
        return try std.fs.path.join(allocator, &.{ home, ".planar", "workbench" });
    }

    return error.WorkbenchRootUnresolved;
}

fn expandTildeRoot(allocator: std.mem.Allocator, path: []const u8, environ: std.process.Environ) ![]const u8 {
    if (std.mem.eql(u8, path, "~")) {
        const home = environ.getPosix("HOME") orelse return error.WorkbenchRootUnresolved;
        return allocator.dupe(u8, home);
    }
    if (std.mem.startsWith(u8, path, "~/")) {
        const home = environ.getPosix("HOME") orelse return error.WorkbenchRootUnresolved;
        return std.fs.path.join(allocator, &.{ home, path[2..] });
    }
    return allocator.dupe(u8, path);
}

// =========================================================================
// Tests for resolveRoot invariants (red before fix).
// =========================================================================

fn testEnvironFrom(allocator: std.mem.Allocator, entries: []const []const u8) !struct {
    environ: std.process.Environ,
    owned: [][:0]const u8,
    envp: [:null]?[*:0]u8,
} {
    const envp = try allocator.allocSentinel(?[*:0]u8, entries.len, null);
    errdefer allocator.free(envp);
    const owned = try allocator.alloc([:0]const u8, entries.len);
    errdefer allocator.free(owned);
    for (entries, 0..) |entry, i| {
        const z = try allocator.dupeZ(u8, entry);
        owned[i] = z;
        envp[i] = @constCast(z.ptr);
    }
    const block = std.process.Environ.PosixBlock{ .slice = envp };
    return .{ .environ = std.process.Environ{ .block = block }, .owned = owned, .envp = envp };
}

fn freeTestEnviron(allocator: std.mem.Allocator, owned: [][:0]const u8, envp: [:null]?[*:0]u8) void {
    for (owned) |s| allocator.free(s);
    allocator.free(owned);
    allocator.free(envp);
}

test "resolveRoot: no env, no HOME → error.WorkbenchRootUnresolved (not cwd)" {
    const a = std.testing.allocator;
    // Empty environment: no PLANAR_WORKBENCH_ROOT, no PLANAR_CONFIG_PATH, no HOME.
    const te = try testEnvironFrom(a, &.{});
    defer freeTestEnviron(a, te.owned, te.envp);

    const result = resolveRoot(a, te.environ);
    // Must error — never return ".".
    try std.testing.expectError(error.WorkbenchRootUnresolved, result);
}

test "resolveRoot: config file workbench.root wins when no env var set" {
    const a = std.testing.allocator;
    // Write a config file using the C write helper already available in this module.
    const cfg_path = "/tmp/planar-test-resolveRoot-config.toml";
    const cfg_content =
        \\[workbench]
        \\root = "/tmp/wb-from-config"
        \\
    ;
    try writeFile(cfg_path, cfg_content);

    // Inject PLANAR_CONFIG_PATH pointing at our file; no PLANAR_WORKBENCH_ROOT.
    const entry = try std.fmt.allocPrint(a, "PLANAR_CONFIG_PATH={s}", .{cfg_path});
    defer a.free(entry);
    const te = try testEnvironFrom(a, &.{entry});
    defer freeTestEnviron(a, te.owned, te.envp);

    const root = try resolveRoot(a, te.environ);
    defer a.free(root);
    // Must use the value from the config file.
    try std.testing.expectEqualStrings("/tmp/wb-from-config", root);
}

fn pathExists(path: []const u8) bool {
    const z = std.heap.page_allocator.dupeZ(u8, path) catch return false;
    defer std.heap.page_allocator.free(z);
    return c.access(z.ptr, 0) == 0;
}

fn writeFile(path: []const u8, data: []const u8) !void {
    const z = try std.heap.page_allocator.dupeZ(u8, path);
    defer std.heap.page_allocator.free(z);
    const fd = c.open(z.ptr, c.O_WRONLY | c.O_CREAT | c.O_TRUNC, @as(c_uint, 0o644));
    if (fd < 0) return error.OpenFailed;
    defer _ = c.close(fd);
    var off: usize = 0;
    while (off < data.len) {
        const n = c.write(fd, data[off..].ptr, data.len - off);
        if (n <= 0) return error.WriteFailed;
        off += @intCast(n);
    }
}

fn readFileAlloc(allocator: std.mem.Allocator, path: []const u8) ![]u8 {
    const z = try std.heap.page_allocator.dupeZ(u8, path);
    defer std.heap.page_allocator.free(z);
    const fd = c.open(z.ptr, c.O_RDONLY, @as(c_uint, 0));
    if (fd < 0) {
        return if (std.c.errno(fd) == .NOENT) error.FileNotFound else error.OpenFailed;
    }
    defer _ = c.close(fd);
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var buf: [4096]u8 = undefined;
    while (true) {
        const n = c.read(fd, &buf, buf.len);
        if (n < 0) return error.ReadFailed;
        if (n == 0) break;
        try out.appendSlice(allocator, buf[0..@intCast(n)]);
    }
    return try out.toOwnedSlice(allocator);
}

fn makePathAll(path: []const u8) !void {
    const normalized = try std.heap.page_allocator.dupe(u8, path);
    defer std.heap.page_allocator.free(normalized);
    var cur = std.ArrayList(u8).empty;
    defer cur.deinit(std.heap.page_allocator);
    var it = std.mem.splitScalar(u8, normalized, '/');
    if (std.mem.startsWith(u8, normalized, "/")) try cur.append(std.heap.page_allocator, '/');
    while (it.next()) |seg| {
        if (seg.len == 0) continue;
        if (cur.items.len > 1 or (cur.items.len == 1 and cur.items[0] != '/')) try cur.append(std.heap.page_allocator, '/');
        try cur.appendSlice(std.heap.page_allocator, seg);
        const z = try std.heap.page_allocator.dupeZ(u8, cur.items);
        defer std.heap.page_allocator.free(z);
        if (c.mkdir(z.ptr, 0o755) != 0 and !pathExists(cur.items)) return error.MakePathFailed;
    }
}

fn listMarkdownFiles(allocator: std.mem.Allocator, dir: []const u8) ![]const []const u8 {
    var out = std.ArrayList([]const u8).empty;
    errdefer {
        for (out.items) |p| allocator.free(p);
        out.deinit(allocator);
    }
    try collectMarkdownRecursive(allocator, dir, &out);
    return try out.toOwnedSlice(allocator);
}

fn toStoredPath(allocator: std.mem.Allocator, abs: []const u8, root: []const u8) ![]const u8 {
    if (std.mem.startsWith(u8, abs, root)) {
        var rel = abs[root.len..];
        if (rel.len > 0 and rel[0] == '/') rel = rel[1..];
        return allocator.dupe(u8, rel);
    }
    return allocator.dupe(u8, abs);
}

fn findState(rows: []const manifest.SyncState, kind: []const u8, id: i64) ?manifest.SyncState {
    for (rows) |r| {
        if (r.entity_id == id and std.mem.eql(u8, r.entity_kind, kind)) return r;
    }
    return null;
}

fn extractBodyText(body: []const u8) []const u8 {
    var lines = std.mem.splitScalar(u8, body, '\n');
    var started = false;
    var start_idx: usize = 0;
    var cursor: usize = 0;
    while (lines.next()) |line| {
        if (!started) {
            const trimmed = std.mem.trim(u8, line, " \r\t");
            if (std.mem.startsWith(u8, trimmed, "# ")) {
                cursor += line.len + 1;
                continue;
            }
            if (std.mem.startsWith(u8, trimmed, "**") and std.mem.indexOf(u8, trimmed, ":**") != null) {
                cursor += line.len + 1;
                continue;
            }
            if (trimmed.len == 0) {
                cursor += line.len + 1;
                continue;
            }
            started = true;
            start_idx = cursor;
        }
        cursor += line.len + 1;
    }
    if (!started) return "";
    return std.mem.trim(u8, body[start_idx..], " \r\n\t");
}

fn fileMtime(abs_path: []const u8, allocator: std.mem.Allocator) ![]const u8 {
    _ = abs_path;
    return allocator.dupe(u8, "");
}

fn deleteTreePortable(allocator: std.mem.Allocator, dir: []const u8) !void {
    if (!pathExists(dir)) return error.FileNotFound;
    const dir_z = try allocator.dupeZ(u8, dir);
    defer allocator.free(dir_z);
    var st: c.struct_stat = undefined;
    if (c.lstat(dir_z.ptr, &st) != 0) return error.FileNotFound;
    if ((st.st_mode & c.S_IFMT) != c.S_IFDIR) return error.UnsafeArchivePath;
    try deleteTreeRecursive(allocator, dir);
}

fn collectMarkdownRecursive(allocator: std.mem.Allocator, dir: []const u8, out: *std.ArrayList([]const u8)) !void {
    const dir_z = try allocator.dupeZ(u8, dir);
    defer allocator.free(dir_z);
    const dp = c.opendir(dir_z.ptr) orelse {
        return if (std.c.errno(-1) == .NOENT) error.FileNotFound else error.OpenFailed;
    };
    defer _ = c.closedir(dp);
    while (c.readdir(dp)) |ent| {
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&ent.*.d_name)));
        if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;
        const child = try std.fs.path.join(allocator, &.{ dir, name });
        defer allocator.free(child);
        var st: c.struct_stat = undefined;
        const child_z = try allocator.dupeZ(u8, child);
        defer allocator.free(child_z);
        if (c.lstat(child_z.ptr, &st) != 0) return error.ReadFailed;
        if ((st.st_mode & c.S_IFMT) == c.S_IFDIR) {
            try collectMarkdownRecursive(allocator, child, out);
        } else if (std.mem.endsWith(u8, name, ".md")) {
            try out.append(allocator, try allocator.dupe(u8, child));
        }
    }
}

fn deleteTreeRecursive(allocator: std.mem.Allocator, dir: []const u8) !void {
    const dir_z = try allocator.dupeZ(u8, dir);
    defer allocator.free(dir_z);
    const dp = c.opendir(dir_z.ptr) orelse return;
    defer _ = c.closedir(dp);
    while (c.readdir(dp)) |ent| {
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&ent.*.d_name)));
        if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;
        const child = try std.fs.path.join(allocator, &.{ dir, name });
        defer allocator.free(child);
        var st: c.struct_stat = undefined;
        const child_z = try allocator.dupeZ(u8, child);
        defer allocator.free(child_z);
        if (c.lstat(child_z.ptr, &st) != 0) continue;
        if ((st.st_mode & c.S_IFMT) == c.S_IFDIR) {
            try deleteTreeRecursive(allocator, child);
            _ = c.rmdir(child_z.ptr);
        } else {
            _ = c.unlink(child_z.ptr);
        }
    }
    _ = c.rmdir(dir_z.ptr);
}
