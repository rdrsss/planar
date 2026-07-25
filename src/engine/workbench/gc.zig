//! engine/workbench/gc — garbage-collect terminal-backed FS files.
//!
//! Implements `planar workbench gc <plan>` (plan 439 M4): walk the workbench
//! tree, classify each `.md` file by its backing entity's status, and remove
//! files for entities that fall inside the active filter mode. Defaults to
//! apply (matching Planar's destructive-verb convention) but refuses with
//! exit 1 when any to-be-removed file has FS-content drift from its
//! DB-stored hash; `--yes` overrides the refusal.
//!
//! The verb is FS-only: zero DB writes outside the manifest cleanup that
//! mirrors successful file removals. No entity row is mutated.

const std = @import("std");
const db = @import("db");
const parse = @import("parse.zig");
const manifest = @import("manifest.zig");
const feature = @import("feature.zig");
const terminal_mod = @import("terminal.zig");

const c = @cImport({
    // glibc's fortified open/openat wrappers in bits/fcntl2.h use
    // __attribute__((__error__(...))), which translate-c cannot represent —
    // disable fortification so translate-c sees the plain declarations.
    @cDefine("_FORTIFY_SOURCE", "0");
    @cInclude("dirent.h");
    @cInclude("unistd.h");
    @cInclude("sys/stat.h");
    @cInclude("fcntl.h");
});

pub const Options = struct {
    dry_run: bool = false,
    yes: bool = false,
    filter_mode: terminal_mod.Mode = .failures,
    all_scopes: bool = false,
};

pub const DriftedEntry = struct {
    path: []const u8,
};

pub const Summary = struct {
    removed: usize = 0,
    kept: usize = 0,
    drifted_skipped: usize = 0,
    errors: usize = 0,
    drifted_paths: []const []const u8 = &.{},

    pub fn deinit(self: Summary, allocator: std.mem.Allocator) void {
        for (self.drifted_paths) |p| allocator.free(p);
        allocator.free(self.drifted_paths);
    }
};

/// Run gc for a single anchor plan. The caller supplies the resolved
/// workbench root; `gc.run` will resolve the per-plan feature directory.
pub fn runForPlan(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
    workbench_root: []const u8,
    opts: Options,
) !Summary {
    var drifted: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (drifted.items) |p| allocator.free(p);
        drifted.deinit(allocator);
    }
    var summary = Summary{};
    try runForPlanInto(d, allocator, anchor_plan_id, workbench_root, opts, &summary, &drifted);
    summary.drifted_paths = try drifted.toOwnedSlice(allocator);
    return summary;
}

/// Run gc across every anchor plan that has a workbench tree. Aggregates
/// the per-plan summaries into one.
pub fn runAllScopes(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    workbench_root: []const u8,
    opts: Options,
) !Summary {
    var drifted: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (drifted.items) |p| allocator.free(p);
        drifted.deinit(allocator);
    }
    var summary = Summary{};

    var stmt = d.prepare(
        \\select id from plans
        \\where parent_plan_id is null
        \\order by id
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                runForPlanInto(d, allocator, id, workbench_root, opts, &summary, &drifted) catch {
                    summary.errors += 1;
                };
            },
        }
    }
    summary.drifted_paths = try drifted.toOwnedSlice(allocator);
    return summary;
}

fn runForPlanInto(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
    workbench_root: []const u8,
    opts: Options,
    summary: *Summary,
    drifted: *std.ArrayList([]const u8),
) !void {
    const a = fetchAnchor(d, allocator, anchor_plan_id) catch return;
    defer freeAnchor(allocator, a);
    const feature_dir = try feature.featureDir(allocator, workbench_root, a.assoc_slug, a.plan_key, a.slug);
    defer allocator.free(feature_dir);
    if (!pathExists(feature_dir)) return; // no tree → nothing to gc.

    var files: std.ArrayList([]const u8) = .empty;
    defer {
        for (files.items) |p| allocator.free(p);
        files.deinit(allocator);
    }
    try collectMarkdown(allocator, feature_dir, &files);

    for (files.items) |path| {
        const content = readFileAlloc(allocator, path) catch {
            summary.errors += 1;
            continue;
        };
        defer allocator.free(content);

        const parsed = parse.parse(allocator, content) catch {
            // Unparseable frontmatter: leave the file alone.
            summary.kept += 1;
            continue;
        };
        defer parse.deinit(parsed, allocator);

        const status_txt = fetchStatus(d, allocator, parsed.frontmatter.entity_kind, parsed.frontmatter.entity_id) catch "";
        defer if (status_txt.len > 0) allocator.free(status_txt);

        const filtered = terminal_mod.isFilteredStr(
            parsed.frontmatter.entity_kind,
            status_txt,
            opts.filter_mode,
        ) orelse {
            // Unknown kind / status — keep.
            summary.kept += 1;
            continue;
        };
        if (!filtered) {
            summary.kept += 1;
            continue;
        }

        // Filtered entity. Check drift.
        const fs_hash = try manifest.hashContent(allocator, content);
        defer allocator.free(fs_hash);
        const db_hash = fetchManifestHash(d, allocator, anchor_plan_id, parsed.frontmatter.entity_kind, parsed.frontmatter.entity_id) catch "";
        defer if (db_hash.len > 0) allocator.free(db_hash);

        const drift = db_hash.len > 0 and !std.mem.eql(u8, fs_hash, db_hash);
        if (drift and !opts.yes) {
            const dup = try allocator.dupe(u8, path);
            try drifted.append(allocator, dup);
            summary.drifted_skipped += 1;
            continue;
        }

        if (opts.dry_run) {
            summary.removed += 1;
            continue;
        }

        const path_z = try allocator.dupeZ(u8, path);
        defer allocator.free(path_z);
        if (c.unlink(path_z.ptr) != 0) {
            summary.errors += 1;
            continue;
        }
        summary.removed += 1;
        _ = d.execParams(
            "delete from workbench_sync_state where anchor_plan_id = ? and entity_kind = ? and entity_id = ?",
            &.{
                .{ .int = anchor_plan_id },
                .{ .text = parsed.frontmatter.entity_kind },
                .{ .int = parsed.frontmatter.entity_id },
            },
        ) catch {};
    }
}

// ----------------------------------------------------------------------
// Helpers — duplicated narrowly from sync.zig to keep gc self-contained.

const Anchor = struct {
    id: i64,
    slug: []const u8,
    assoc_slug: []const u8,
    plan_key: []const u8,
};

fn fetchAnchor(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) !Anchor {
    var stmt = d.prepare(
        \\select coalesce(p.slug, ''), coalesce(a.slug, '')
        \\from plans p
        \\left join associations a on (p.scope_kind = 'association' and a.id = p.scope_id)
        \\where p.id = ? and p.parent_plan_id is null
        \\limit 1
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return error.QueryFailed;
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => error.NotFound,
        .row => blk: {
            const slug = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(slug);
            const assoc_slug = try stmt.columnTextAlloc(1, allocator);
            errdefer allocator.free(assoc_slug);
            // Mirror sync.zig: the feature-dir key is the plan's
            // external_id when it has an external link, falling back to
            // `p{d}` only when unlinked. Hardcoding `p{d}` here made GC
            // compute the wrong feature_dir for externally-linked plans
            // and silently skip their trees.
            const plan_key = try resolvePlanKey(d, allocator, anchor_plan_id);
            errdefer allocator.free(plan_key);
            break :blk .{
                .id = anchor_plan_id,
                .slug = slug,
                .assoc_slug = assoc_slug,
                .plan_key = plan_key,
            };
        },
    };
}

fn freeAnchor(allocator: std.mem.Allocator, a: Anchor) void {
    allocator.free(a.slug);
    allocator.free(a.assoc_slug);
    allocator.free(a.plan_key);
}

/// Resolve a plan's feature-dir key: its `external_links.external_id`
/// when linked, else the `p{d}` fallback. Mirrors
/// `sync.zig:resolvePlanKey` so gc and sync agree on the feature path.
fn resolvePlanKey(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) ![]const u8 {
    var stmt = d.prepare("select external_id from external_links where entity_kind='plan' and entity_id=? order by id limit 1") catch return std.fmt.allocPrint(allocator, "p{d}", .{plan_id});
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return std.fmt.allocPrint(allocator, "p{d}", .{plan_id});
    switch (stmt.step() catch .done) {
        .done => return std.fmt.allocPrint(allocator, "p{d}", .{plan_id}),
        .row => return stmt.columnTextAlloc(0, allocator),
    }
}

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

fn fetchManifestHash(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64, kind: []const u8, id: i64) ![]const u8 {
    var stmt = d.prepare(
        \\select coalesce(content_hash, '') from workbench_sync_state
        \\where anchor_plan_id = ? and entity_kind = ? and entity_id = ?
        \\limit 1
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .int = anchor_plan_id },
        .{ .text = kind },
        .{ .int = id },
    }) catch return error.QueryFailed;
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => allocator.dupe(u8, ""),
        .row => try stmt.columnTextAlloc(0, allocator),
    };
}

fn pathExists(path: []const u8) bool {
    var buf: [std.fs.max_path_bytes:0]u8 = undefined;
    if (path.len >= buf.len) return false;
    @memcpy(buf[0..path.len], path);
    buf[path.len] = 0;
    var st: c.struct_stat = undefined;
    return c.stat(@ptrCast(&buf), &st) == 0;
}

fn readFileAlloc(allocator: std.mem.Allocator, path: []const u8) ![]const u8 {
    var buf: [std.fs.max_path_bytes:0]u8 = undefined;
    if (path.len >= buf.len) return error.PathTooLong;
    @memcpy(buf[0..path.len], path);
    buf[path.len] = 0;
    const fd = c.open(@ptrCast(&buf), 0);
    if (fd < 0) return error.FileNotFound;
    defer _ = c.close(fd);
    var st: c.struct_stat = undefined;
    if (c.fstat(fd, &st) != 0) return error.QueryFailed;
    const size: usize = @intCast(st.st_size);
    const out = try allocator.alloc(u8, size);
    errdefer allocator.free(out);
    var off: usize = 0;
    while (off < size) {
        const n = c.read(fd, @ptrCast(&out[off]), size - off);
        if (n <= 0) break;
        off += @intCast(n);
    }
    return out[0..off];
}

fn collectMarkdown(allocator: std.mem.Allocator, dir: []const u8, out: *std.ArrayList([]const u8)) !void {
    const dir_z = try allocator.dupeZ(u8, dir);
    defer allocator.free(dir_z);
    const dp = c.opendir(dir_z.ptr) orelse return;
    defer _ = c.closedir(dp);
    while (c.readdir(dp)) |ent| {
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&ent.*.d_name)));
        if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;
        const child = try std.fs.path.join(allocator, &.{ dir, name });
        errdefer allocator.free(child);
        const child_z = try allocator.dupeZ(u8, child);
        defer allocator.free(child_z);
        var st: c.struct_stat = undefined;
        if (c.stat(child_z.ptr, &st) != 0) {
            allocator.free(child);
            continue;
        }
        if ((st.st_mode & c.S_IFMT) == c.S_IFDIR) {
            try collectMarkdown(allocator, child, out);
            allocator.free(child);
        } else if (std.mem.endsWith(u8, name, ".md")) {
            try out.append(allocator, child);
        } else {
            allocator.free(child);
        }
    }
}
