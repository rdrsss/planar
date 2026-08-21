//! engine/local/link — symlink install/remove/list for sandbox sources.

const std = @import("std");
const manifest = @import("manifest.zig");

pub const Layout = enum {
    flat,
    @"dir-symlink",
};

pub const Target = struct {
    vendor: []const u8,
    layout: Layout,
    target_path: []const u8,
    link_target: []const u8,
};

pub const LinkOpts = struct {
    home_dir: []const u8,
    dry_run: bool = false,
    vendor_filter: ?[]const u8 = null,
    force_copy: bool = false,
};

pub const TargetRecord = struct {
    vendor: []const u8,
    target_path: []const u8,
    source_path: []const u8,
    mode: ?manifest.Mode = null,
    action: []const u8,
    warning: []const u8 = "",
    linked_at: []const u8 = "",
};

pub const LinkResult = struct {
    name: []const u8,
    kind: manifest.Kind,
    records: []const TargetRecord,
};

pub fn deinitLinkResult(res: LinkResult, allocator: std.mem.Allocator) void {
    allocator.free(res.name);
    for (res.records) |rec| deinitTargetRecord(rec, allocator);
    allocator.free(res.records);
}

pub fn targets(
    home_dir: []const u8,
    source_dir: []const u8,
    name: []const u8,
    kind: manifest.Kind,
    shadow: bool,
    vendors: []const []const u8,
    allocator: std.mem.Allocator,
) ![]const Target {
    if (home_dir.len == 0 or name.len == 0) return error.InvalidInput;
    const basename = if (shadow) try allocator.dupe(u8, name) else try std.fmt.allocPrint(allocator, "local-{s}", .{name});
    defer allocator.free(basename);
    const filename = try std.fmt.allocPrint(allocator, "{s}.md", .{basename});
    defer allocator.free(filename);

    switch (kind) {
        .skill => {
            if (source_dir.len == 0) return error.InvalidInput;
            var out: std.ArrayList(Target) = .empty;
            errdefer {
                for (out.items) |t| deinitTarget(t, allocator);
                out.deinit(allocator);
            }
            for (vendors) |vendor| {
                const install_dir = try skillDir(home_dir, vendor, allocator);
                defer allocator.free(install_dir);
                if (isDirSymlinkVendor(vendor)) {
                    try out.append(allocator, .{
                        .vendor = try allocator.dupe(u8, vendor),
                        .layout = .@"dir-symlink",
                        .target_path = try std.fs.path.join(allocator, &.{ install_dir, basename }),
                        .link_target = try allocator.dupe(u8, source_dir),
                    });
                } else {
                    try out.append(allocator, .{
                        .vendor = try allocator.dupe(u8, vendor),
                        .layout = .flat,
                        .target_path = try std.fs.path.join(allocator, &.{ install_dir, filename }),
                        .link_target = try std.fs.path.join(allocator, &.{ source_dir, "SKILL.md" }),
                    });
                }
            }
            std.mem.sort(Target, out.items, {}, struct {
                fn lt(_: void, a: Target, b: Target) bool {
                    return std.mem.order(u8, a.vendor, b.vendor) == .lt;
                }
            }.lt);
            return try out.toOwnedSlice(allocator);
        },
        .agent => {
            const target_path = try std.fs.path.join(allocator, &.{ home_dir, ".planar", "agents", filename });
            var out = try allocator.alloc(Target, 1);
            out[0] = .{
                .vendor = try allocator.dupe(u8, "agents"),
                .layout = .flat,
                .target_path = target_path,
                .link_target = try allocator.alloc(u8, 0),
            };
            return out;
        },
    }
}

pub fn deinitTargets(items: []const Target, allocator: std.mem.Allocator) void {
    for (items) |t| deinitTarget(t, allocator);
    allocator.free(items);
}

pub fn link(file: manifest.SandboxFile, opts: LinkOpts, allocator: std.mem.Allocator) !LinkResult {
    if (opts.home_dir.len == 0) return error.InvalidInput;
    const source_dir = std.fs.path.dirname(file.source_path) orelse "";
    const vendors = try manifest.resolvedVendors(file.frontmatter, allocator);
    defer {
        for (vendors) |v| allocator.free(v);
        allocator.free(vendors);
    }
    const tgs = try targets(
        opts.home_dir,
        source_dir,
        file.name,
        file.kind,
        file.frontmatter.shadow,
        vendors,
        allocator,
    );
    defer deinitTargets(tgs, allocator);

    const now = try nowTimestamp(allocator);
    defer allocator.free(now);

    var records: std.ArrayList(TargetRecord) = .empty;
    errdefer {
        for (records.items) |rec| deinitTargetRecord(rec, allocator);
        records.deinit(allocator);
    }

    for (tgs) |t| {
        var rec = TargetRecord{
            .vendor = try allocator.dupe(u8, t.vendor),
            .target_path = try allocator.dupe(u8, t.target_path),
            .source_path = try allocator.dupe(u8, if (t.link_target.len == 0) file.source_path else t.link_target),
            .mode = null,
            .action = "",
            .warning = try allocator.alloc(u8, 0),
            .linked_at = try allocator.alloc(u8, 0),
        };
        errdefer deinitTargetRecord(rec, allocator);

        if (opts.vendor_filter) |vf| {
            if (!std.mem.eql(u8, vf, rec.vendor)) {
                rec.action = try allocator.dupe(u8, "skipped");
                try records.append(allocator, rec);
                continue;
            }
        }

        if (file.frontmatter.shadow) {
            rec.warning = try std.fmt.allocPrint(
                allocator,
                "shadow: linked as \"{s}\" (no local- prefix); any canonical install with this name is replaced",
                .{std.fs.path.basename(rec.target_path)},
            );
        }

        if (opts.dry_run) {
            rec.action = try allocator.dupe(u8, "dry-run");
            rec.mode = if (opts.force_copy) .copy else .symlink;
            try records.append(allocator, rec);
            continue;
        }

        if (std.fs.path.dirname(rec.target_path)) |parent| {
            try std.Io.Dir.cwd().createDirPath(fsIo(), parent);
        }

        const installed = try installOne(rec.source_path, rec.target_path, t.layout, opts.force_copy, allocator);
        rec.mode = installed.mode;
        rec.action = installed.action;
        rec.linked_at = try allocator.dupe(u8, now);
        if (installed.mode == .copy and rec.warning.len == 0) {
            rec.warning = try allocator.dupe(u8, "copy fallback: edits to source require re-running `planar local link`");
        }

        try records.append(allocator, rec);
    }

    if (!opts.dry_run) {
        var m = try manifest.loadManifest(opts.home_dir, file.kind, allocator);
        defer manifest.deinitManifest(m, allocator);
        try putManifestEntry(&m, file.name, file.source_path, records.items, allocator);
        try manifest.saveManifest(opts.home_dir, file.kind, m, allocator);
    }

    return .{
        .name = try allocator.dupe(u8, file.name),
        .kind = file.kind,
        .records = try records.toOwnedSlice(allocator),
    };
}

pub const UnlinkOpts = struct {
    home_dir: []const u8,
    purge: bool = false,
};

pub const UnlinkResult = struct {
    name: []const u8,
    kind: manifest.Kind,
    removed: []const TargetRecord,
    purged_file: []const u8 = "",
};

pub fn deinitUnlinkResult(res: UnlinkResult, allocator: std.mem.Allocator) void {
    allocator.free(res.name);
    for (res.removed) |rec| deinitTargetRecord(rec, allocator);
    allocator.free(res.removed);
    if (res.purged_file.len > 0) allocator.free(res.purged_file);
}

pub fn unlink(name: []const u8, kind: manifest.Kind, opts: UnlinkOpts, allocator: std.mem.Allocator) !UnlinkResult {
    if (opts.home_dir.len == 0 or name.len == 0) return error.InvalidInput;

    var m = try manifest.loadManifest(opts.home_dir, kind, allocator);
    defer manifest.deinitManifest(m, allocator);

    var removed: std.ArrayList(TargetRecord) = .empty;
    errdefer {
        for (removed.items) |rec| deinitTargetRecord(rec, allocator);
        removed.deinit(allocator);
    }

    const idx = findManifestEntry(m, name);
    if (idx) |entry_idx| {
        const entry = m.entries[entry_idx];
        for (entry.links) |link_row| {
            removePathIfExists(link_row.target_path);
            try removed.append(allocator, .{
                .vendor = try allocator.dupe(u8, link_row.vendor),
                .target_path = try allocator.dupe(u8, link_row.target_path),
                .source_path = try allocator.dupe(u8, link_row.source_path),
                .mode = link_row.mode,
                .action = try allocator.dupe(u8, "removed"),
                .warning = try allocator.alloc(u8, 0),
                .linked_at = try allocator.dupe(u8, link_row.linked_at),
            });
        }
        try removeManifestEntry(&m, name, allocator);
        try manifest.saveManifest(opts.home_dir, kind, m, allocator);
    }

    var purged = try allocator.alloc(u8, 0);
    if (opts.purge) {
        switch (kind) {
            .skill => {
                const src_dir = try std.fs.path.join(allocator, &.{ opts.home_dir, ".planar", "local", "skills", name });
                defer allocator.free(src_dir);
                _ = std.Io.Dir.cwd().deleteTree(fsIo(), src_dir) catch {};
                purged = try allocator.dupe(u8, src_dir);
            },
            .agent => {
                const file_name = try std.fmt.allocPrint(allocator, "{s}.md", .{name});
                defer allocator.free(file_name);
                const src_path = try std.fs.path.join(allocator, &.{ opts.home_dir, ".planar", "local", "agents", file_name });
                defer allocator.free(src_path);
                _ = std.Io.Dir.cwd().deleteFile(fsIo(), src_path) catch {};
                purged = try allocator.dupe(u8, src_path);
            },
        }
    }

    return .{
        .name = try allocator.dupe(u8, name),
        .kind = kind,
        .removed = try removed.toOwnedSlice(allocator),
        .purged_file = purged,
    };
}

pub const ListRecord = struct {
    name: []const u8,
    kind: manifest.Kind,
    record: TargetRecord,
};

pub fn deinitListRecords(rows: []const ListRecord, allocator: std.mem.Allocator) void {
    for (rows) |r| {
        allocator.free(r.name);
        deinitTargetRecord(r.record, allocator);
    }
    allocator.free(rows);
}

pub fn list(home_dir: []const u8, allocator: std.mem.Allocator) ![]const ListRecord {
    if (home_dir.len == 0) return error.InvalidInput;
    var out: std.ArrayList(ListRecord) = .empty;
    errdefer {
        for (out.items) |r| {
            allocator.free(r.name);
            deinitTargetRecord(r.record, allocator);
        }
        out.deinit(allocator);
    }

    for ([_]manifest.Kind{ .skill, .agent }) |kind| {
        const m = try manifest.loadManifest(home_dir, kind, allocator);
        defer manifest.deinitManifest(m, allocator);
        for (m.entries) |entry| {
            for (entry.links) |l| {
                const action = try allocator.dupe(u8, classifyExisting(l));
                errdefer allocator.free(action);
                try out.append(allocator, .{
                    .name = try allocator.dupe(u8, entry.name),
                    .kind = kind,
                    .record = .{
                        .vendor = try allocator.dupe(u8, l.vendor),
                        .target_path = try allocator.dupe(u8, l.target_path),
                        .source_path = try allocator.dupe(u8, l.source_path),
                        .mode = l.mode,
                        .action = action,
                        .warning = try allocator.alloc(u8, 0),
                        .linked_at = try allocator.dupe(u8, l.linked_at),
                    },
                });
            }
        }
    }

    std.mem.sort(ListRecord, out.items, {}, struct {
        fn lt(_: void, a: ListRecord, b: ListRecord) bool {
            if (a.kind != b.kind) return @intFromEnum(a.kind) < @intFromEnum(b.kind);
            const ord = std.mem.order(u8, a.name, b.name);
            if (ord != .eq) return ord == .lt;
            return std.mem.order(u8, a.record.vendor, b.record.vendor) == .lt;
        }
    }.lt);

    return try out.toOwnedSlice(allocator);
}

pub const ReconcileOpts = struct {
    home_dir: []const u8,
    dry_run: bool = false,
};

pub const ReconcileAction = struct {
    name: []const u8,
    kind: manifest.Kind,
    reason: []const u8,
    source_path: []const u8,
    removed_targets: []const []const u8,
};

pub const ReconcileResult = struct {
    actions: []const ReconcileAction,
};

pub fn deinitReconcileResult(res: ReconcileResult, allocator: std.mem.Allocator) void {
    for (res.actions) |a| {
        allocator.free(a.name);
        allocator.free(a.reason);
        allocator.free(a.source_path);
        for (a.removed_targets) |rt| allocator.free(rt);
        allocator.free(a.removed_targets);
    }
    allocator.free(res.actions);
}

pub fn reconcile(opts: ReconcileOpts, allocator: std.mem.Allocator) !ReconcileResult {
    if (opts.home_dir.len == 0) return error.InvalidInput;
    var actions: std.ArrayList(ReconcileAction) = .empty;
    var maybe_now: ?[]u8 = null;
    defer if (maybe_now) |ts| allocator.free(ts);
    errdefer {
        for (actions.items) |a| {
            allocator.free(a.name);
            allocator.free(a.reason);
            allocator.free(a.source_path);
            for (a.removed_targets) |rt| allocator.free(rt);
            allocator.free(a.removed_targets);
        }
        actions.deinit(allocator);
    }

    for ([_]manifest.Kind{ .skill, .agent }) |kind| {
        var m = try manifest.loadManifest(opts.home_dir, kind, allocator);
        defer manifest.deinitManifest(m, allocator);

        var keep: std.ArrayList(manifest.ManifestEntry) = .empty;
        errdefer {
            for (keep.items) |entry| freeManifestEntry(entry, allocator);
            keep.deinit(allocator);
        }

        for (m.entries) |entry| {
            if (pathExists(entry.source_path)) {
                var links: std.ArrayList(manifest.ManifestRecord) = .empty;
                errdefer {
                    for (links.items) |r| freeManifestRecord(r, allocator);
                    links.deinit(allocator);
                }
                var repaired: std.ArrayList([]const u8) = .empty;
                errdefer {
                    for (repaired.items) |p| allocator.free(p);
                    repaired.deinit(allocator);
                }

                for (entry.links) |src_rec| {
                    var next_mode = src_rec.mode;
                    var next_linked_at = try allocator.dupe(u8, src_rec.linked_at);
                    var next_record = manifest.ManifestRecord{
                        .vendor = try allocator.dupe(u8, src_rec.vendor),
                        .target_path = try allocator.dupe(u8, src_rec.target_path),
                        .source_path = try allocator.dupe(u8, src_rec.source_path),
                        .mode = next_mode,
                        .linked_at = next_linked_at,
                    };

                    const status = classifyExisting(src_rec);
                    if (!std.mem.eql(u8, status, "live")) {
                        try repaired.append(allocator, try allocator.dupe(u8, src_rec.target_path));
                        if (!opts.dry_run) {
                            if (std.fs.path.dirname(src_rec.target_path)) |parent| {
                                try std.Io.Dir.cwd().createDirPath(fsIo(), parent);
                            }
                            const installed = try installOne(
                                src_rec.source_path,
                                src_rec.target_path,
                                layoutForRecord(kind, src_rec.vendor),
                                false,
                                allocator,
                            );
                            next_mode = installed.mode;
                            allocator.free(installed.action);
                            if (maybe_now == null) maybe_now = try nowTimestamp(allocator);
                            allocator.free(next_linked_at);
                            next_linked_at = try allocator.dupe(u8, maybe_now.?);
                            next_record.mode = next_mode;
                            next_record.linked_at = next_linked_at;
                        }
                    }
                    try links.append(allocator, next_record);
                }

                if (repaired.items.len > 0) {
                    try actions.append(allocator, .{
                        .name = try allocator.dupe(u8, entry.name),
                        .kind = kind,
                        .reason = try allocator.dupe(u8, "target-missing"),
                        .source_path = try allocator.dupe(u8, entry.source_path),
                        .removed_targets = try repaired.toOwnedSlice(allocator),
                    });
                } else {
                    repaired.deinit(allocator);
                }
                try keep.append(allocator, .{
                    .name = try allocator.dupe(u8, entry.name),
                    .source_path = try allocator.dupe(u8, entry.source_path),
                    .links = try links.toOwnedSlice(allocator),
                });
                continue;
            }

            var removed: std.ArrayList([]const u8) = .empty;
            errdefer {
                for (removed.items) |rt| allocator.free(rt);
                removed.deinit(allocator);
            }
            for (entry.links) |rec| {
                if (!opts.dry_run) removePathIfExists(rec.target_path);
                try removed.append(allocator, try allocator.dupe(u8, rec.target_path));
            }

            try actions.append(allocator, .{
                .name = try allocator.dupe(u8, entry.name),
                .kind = kind,
                .reason = try allocator.dupe(u8, "source-missing"),
                .source_path = try allocator.dupe(u8, entry.source_path),
                .removed_targets = try removed.toOwnedSlice(allocator),
            });
        }

        if (!opts.dry_run) {
            const next_entries = try keep.toOwnedSlice(allocator);
            keep = .empty;
            manifest.deinitManifest(m, allocator);
            m = .{
                .version = 1,
                .entries = next_entries,
            };
            try manifest.saveManifest(opts.home_dir, kind, m, allocator);
        }
    }

    return .{
        .actions = try actions.toOwnedSlice(allocator),
    };
}

fn layoutForRecord(kind: manifest.Kind, vendor: []const u8) Layout {
    if (kind == .skill and isDirSymlinkVendor(vendor)) return .@"dir-symlink";
    return .flat;
}

pub const InstallResult = struct {
    mode: manifest.Mode,
    action: []const u8,
};

fn installOne(
    source_path: []const u8,
    target_path: []const u8,
    layout: Layout,
    force_copy: bool,
    allocator: std.mem.Allocator,
) !InstallResult {
    const current = classifyCurrent(target_path, source_path);
    if (current == .unchanged and !force_copy) {
        return .{ .mode = .symlink, .action = try allocator.dupe(u8, "unchanged") };
    }
    const previous_action = if (current == .absent) "created" else "updated";

    const tmp = try std.fmt.allocPrint(allocator, "{s}.planar-tmp", .{target_path});
    defer allocator.free(tmp);
    _ = std.Io.Dir.cwd().deleteFile(fsIo(), tmp) catch {};
    _ = std.Io.Dir.cwd().deleteTree(fsIo(), tmp) catch {};

    if (!force_copy) {
        if (std.Io.Dir.cwd().symLink(fsIo(), source_path, tmp, .{})) |_| {
            try renameIntoPlace(tmp, target_path);
            return .{ .mode = .symlink, .action = try allocator.dupe(u8, previous_action) };
        } else |_| {}
    }

    switch (layout) {
        .@"dir-symlink" => {
            try copyTree(source_path, tmp, allocator);
            try renameIntoPlace(tmp, target_path);
            return .{ .mode = .copy, .action = try allocator.dupe(u8, previous_action) };
        },
        .flat => {
            try copyFile(source_path, tmp, allocator);
            try renameIntoPlace(tmp, target_path);
            return .{ .mode = .copy, .action = try allocator.dupe(u8, previous_action) };
        },
    }
}

const ExistingState = enum { absent, unchanged, changed };

fn classifyCurrent(target_path: []const u8, source_path: []const u8) ExistingState {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    if (std.Io.Dir.cwd().readLink(fsIo(), target_path, &buf)) |n| {
        return if (std.mem.eql(u8, buf[0..n], source_path)) .unchanged else .changed;
    } else |_| {}
    return if (pathExists(target_path)) .changed else .absent;
}

fn classifyExisting(row: manifest.ManifestRecord) []const u8 {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const n = std.Io.Dir.cwd().readLink(fsIo(), row.target_path, &buf) catch {
        if (pathExists(row.target_path)) return "live";
        return "missing";
    };
    if (!std.mem.eql(u8, buf[0..n], row.source_path)) return "broken";
    if (!pathExists(row.source_path)) return "broken";
    return "live";
}

fn removePathIfExists(path: []const u8) void {
    std.Io.Dir.cwd().deleteFile(fsIo(), path) catch |e| switch (e) {
        error.FileNotFound, error.IsDir => {
            std.Io.Dir.cwd().deleteTree(fsIo(), path) catch {};
            return;
        },
        else => {
            std.Io.Dir.cwd().deleteTree(fsIo(), path) catch {};
            return;
        },
    };
}

fn copyTree(src: []const u8, dst: []const u8, allocator: std.mem.Allocator) !void {
    try std.Io.Dir.cwd().createDirPath(fsIo(), dst);
    var dir = openDirPath(src, .{ .iterate = true }) catch |e| return e;
    defer dir.close(fsIo());
    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        const child_src = try std.fs.path.join(allocator, &.{ src, entry.name });
        defer allocator.free(child_src);
        const child_dst = try std.fs.path.join(allocator, &.{ dst, entry.name });
        defer allocator.free(child_dst);
        switch (entry.kind) {
            .directory => try copyTree(child_src, child_dst, allocator),
            .sym_link => {
                var buf: [std.fs.max_path_bytes]u8 = undefined;
                const n = try std.Io.Dir.cwd().readLink(fsIo(), child_src, &buf);
                try std.Io.Dir.cwd().symLink(fsIo(), buf[0..n], child_dst, .{});
            },
            else => try copyFile(child_src, child_dst, allocator),
        }
    }
}

fn copyFile(src: []const u8, dst: []const u8, allocator: std.mem.Allocator) !void {
    const body = try readFile(src, allocator, 16 * 1024 * 1024);
    defer allocator.free(body);
    try std.Io.Dir.cwd().writeFile(fsIo(), .{ .sub_path = dst, .data = body });
}

fn renameIntoPlace(tmp_path: []const u8, target_path: []const u8) !void {
    std.Io.Dir.cwd().rename(tmp_path, std.Io.Dir.cwd(), target_path, fsIo()) catch |e| switch (e) {
        error.IsDir,
        error.NotDir,
        error.DirNotEmpty,
        error.AccessDenied,
        error.PermissionDenied,
        => {
            if (isRealDirectory(target_path)) {
                std.Io.Dir.cwd().deleteTree(fsIo(), target_path) catch {};
            } else {
                std.Io.Dir.cwd().deleteFile(fsIo(), target_path) catch {};
            }
            try std.Io.Dir.cwd().rename(tmp_path, std.Io.Dir.cwd(), target_path, fsIo());
        },
        else => return e,
    };
}

fn isRealDirectory(path: []const u8) bool {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    if (std.Io.Dir.cwd().readLink(fsIo(), path, &buf)) |_| return false else |_| {}
    if (openDirPath(path, .{})) |dir| {
        dir.close(fsIo());
        return true;
    } else |_| {
        return false;
    }
}

fn putManifestEntry(
    m: *manifest.Manifest,
    name: []const u8,
    source_path: []const u8,
    records: []const TargetRecord,
    allocator: std.mem.Allocator,
) !void {
    var next: std.ArrayList(manifest.ManifestEntry) = .empty;
    errdefer {
        for (next.items) |entry| freeManifestEntry(entry, allocator);
        next.deinit(allocator);
    }
    var replaced = false;
    for (m.entries) |entry| {
        if (std.mem.eql(u8, entry.name, name)) {
            replaced = true;
            try next.append(allocator, try toManifestEntry(name, source_path, records, allocator));
        } else {
            try next.append(allocator, try cloneManifestEntry(entry, allocator));
        }
    }
    if (!replaced) try next.append(allocator, try toManifestEntry(name, source_path, records, allocator));

    manifest.deinitManifest(m.*, allocator);
    m.* = .{
        .version = 1,
        .entries = try next.toOwnedSlice(allocator),
    };
}

fn removeManifestEntry(m: *manifest.Manifest, name: []const u8, allocator: std.mem.Allocator) !void {
    var next: std.ArrayList(manifest.ManifestEntry) = .empty;
    errdefer {
        for (next.items) |entry| freeManifestEntry(entry, allocator);
        next.deinit(allocator);
    }
    for (m.entries) |entry| {
        if (std.mem.eql(u8, entry.name, name)) continue;
        try next.append(allocator, try cloneManifestEntry(entry, allocator));
    }
    manifest.deinitManifest(m.*, allocator);
    m.* = .{
        .version = 1,
        .entries = try next.toOwnedSlice(allocator),
    };
}

fn findManifestEntry(m: manifest.Manifest, name: []const u8) ?usize {
    for (m.entries, 0..) |entry, i| {
        if (std.mem.eql(u8, entry.name, name)) return i;
    }
    return null;
}

fn toManifestEntry(
    name: []const u8,
    source_path: []const u8,
    records: []const TargetRecord,
    allocator: std.mem.Allocator,
) !manifest.ManifestEntry {
    var links: std.ArrayList(manifest.ManifestRecord) = .empty;
    errdefer {
        for (links.items) |r| freeManifestRecord(r, allocator);
        links.deinit(allocator);
    }
    for (records) |rec| {
        if (std.mem.eql(u8, rec.action, "skipped") or std.mem.eql(u8, rec.action, "dry-run")) continue;
        try links.append(allocator, .{
            .vendor = try allocator.dupe(u8, rec.vendor),
            .target_path = try allocator.dupe(u8, rec.target_path),
            .source_path = try allocator.dupe(u8, rec.source_path),
            .mode = rec.mode.?,
            .linked_at = try allocator.dupe(u8, rec.linked_at),
        });
    }
    return .{
        .name = try allocator.dupe(u8, name),
        .source_path = try allocator.dupe(u8, source_path),
        .links = try links.toOwnedSlice(allocator),
    };
}

fn cloneManifestEntry(src: manifest.ManifestEntry, allocator: std.mem.Allocator) !manifest.ManifestEntry {
    var links: std.ArrayList(manifest.ManifestRecord) = .empty;
    errdefer {
        for (links.items) |r| freeManifestRecord(r, allocator);
        links.deinit(allocator);
    }
    for (src.links) |r| {
        try links.append(allocator, .{
            .vendor = try allocator.dupe(u8, r.vendor),
            .target_path = try allocator.dupe(u8, r.target_path),
            .source_path = try allocator.dupe(u8, r.source_path),
            .mode = r.mode,
            .linked_at = try allocator.dupe(u8, r.linked_at),
        });
    }
    return .{
        .name = try allocator.dupe(u8, src.name),
        .source_path = try allocator.dupe(u8, src.source_path),
        .links = try links.toOwnedSlice(allocator),
    };
}

fn skillDir(home_dir: []const u8, vendor: []const u8, allocator: std.mem.Allocator) ![]u8 {
    if (std.mem.eql(u8, vendor, "claude")) return std.fs.path.join(allocator, &.{ home_dir, ".claude", "commands" });
    if (std.mem.eql(u8, vendor, "codex")) return std.fs.path.join(allocator, &.{ home_dir, ".codex", "skills" });
    if (std.mem.eql(u8, vendor, "copilot")) return std.fs.path.join(allocator, &.{ home_dir, ".copilot", "skills" });
    return error.InvalidVendor;
}

fn isDirSymlinkVendor(vendor: []const u8) bool {
    return std.mem.eql(u8, vendor, "codex") or std.mem.eql(u8, vendor, "copilot");
}

fn pathExists(path: []const u8) bool {
    std.Io.Dir.cwd().access(fsIo(), path, .{}) catch return false;
    return true;
}

fn openDirPath(path: []const u8, opts: std.Io.Dir.OpenOptions) !std.Io.Dir {
    return std.Io.Dir.cwd().openDir(fsIo(), path, opts);
}

fn readFile(path: []const u8, allocator: std.mem.Allocator, max: usize) ![]u8 {
    return std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(max));
}

fn nowTimestamp(allocator: std.mem.Allocator) ![]u8 {
    const now_seconds = std.Io.Clock.now(.real, fsIo()).toSeconds();
    if (now_seconds < 0) return error.InvalidData;
    const epoch_seconds: std.time.epoch.EpochSeconds = .{ .secs = @as(u64, @intCast(now_seconds)) };
    const day = epoch_seconds.getEpochDay();
    const year_day = day.calculateYearDay();
    const month_day = year_day.calculateMonthDay();
    const day_seconds = epoch_seconds.getDaySeconds();
    return std.fmt.allocPrint(
        allocator,
        "{d:0>4}-{d:0>2}-{d:0>2}T{d:0>2}:{d:0>2}:{d:0>2}Z",
        .{
            year_day.year,
            month_day.month.numeric(),
            @as(u8, month_day.day_index) + 1,
            day_seconds.getHoursIntoDay(),
            day_seconds.getMinutesIntoHour(),
            day_seconds.getSecondsIntoMinute(),
        },
    );
}

fn freeManifestRecord(r: manifest.ManifestRecord, allocator: std.mem.Allocator) void {
    allocator.free(r.vendor);
    allocator.free(r.target_path);
    allocator.free(r.source_path);
    allocator.free(r.linked_at);
}

fn freeManifestEntry(entry: manifest.ManifestEntry, allocator: std.mem.Allocator) void {
    allocator.free(entry.name);
    allocator.free(entry.source_path);
    for (entry.links) |r| freeManifestRecord(r, allocator);
    allocator.free(entry.links);
}

fn deinitTarget(t: Target, allocator: std.mem.Allocator) void {
    allocator.free(t.vendor);
    allocator.free(t.target_path);
    allocator.free(t.link_target);
}

fn deinitTargetRecord(rec: TargetRecord, allocator: std.mem.Allocator) void {
    allocator.free(rec.vendor);
    allocator.free(rec.target_path);
    allocator.free(rec.source_path);
    allocator.free(rec.action);
    allocator.free(rec.warning);
    allocator.free(rec.linked_at);
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

test "link + list + unlink lifecycle for one skill" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "home" });
    defer gpa.free(home);

    const src_dir = try std.fs.path.join(gpa, &.{ home, ".planar", "local", "skills", "alpha" });
    defer gpa.free(src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);
    const src = try std.fs.path.join(gpa, &.{ src_dir, "SKILL.md" });
    defer gpa.free(src);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = src,
        .data =
        \\---
        \\description: alpha
        \\kind: skill
        \\vendors: [claude]
        \\---
        \\body
        ,
    });

    const file = try manifest.parseFile(src, .skill, gpa);
    defer manifest.deinitSandboxFile(file, gpa);

    const linked = try link(file, .{ .home_dir = home }, gpa);
    defer deinitLinkResult(linked, gpa);
    try std.testing.expectEqual(@as(usize, 1), linked.records.len);
    try std.testing.expect(std.mem.eql(u8, linked.records[0].vendor, "claude"));

    const rows = try list(home, gpa);
    defer deinitListRecords(rows, gpa);
    try std.testing.expectEqual(@as(usize, 1), rows.len);
    try std.testing.expect(!std.mem.eql(u8, rows[0].record.action, "missing"));

    const unlinked = try unlink("alpha", .skill, .{ .home_dir = home }, gpa);
    defer deinitUnlinkResult(unlinked, gpa);
    try std.testing.expectEqual(@as(usize, 1), unlinked.removed.len);
}

test "classifyExisting marks symlink with missing source as broken" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path });
    defer gpa.free(root);
    const source = try std.fs.path.join(gpa, &.{ root, "src.md" });
    defer gpa.free(source);
    const target = try std.fs.path.join(gpa, &.{ root, "dst.md" });
    defer gpa.free(target);

    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = source, .data = "x\n" });
    try std.Io.Dir.cwd().symLink(std.testing.io, source, target, .{});
    try std.Io.Dir.cwd().deleteFile(std.testing.io, source);

    const status = classifyExisting(.{
        .vendor = "claude",
        .target_path = target,
        .source_path = source,
        .mode = .symlink,
        .linked_at = "",
    });
    try std.testing.expectEqualStrings("broken", status);
}

fn looksLikeRfc3339UtcSecondPrecision(s: []const u8) bool {
    return s.len == 20 and
        s[4] == '-' and
        s[7] == '-' and
        s[10] == 'T' and
        s[13] == ':' and
        s[16] == ':' and
        s[19] == 'Z';
}

test "link sets linked_at as RFC3339 UTC (not literal now)" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "home-ts" });
    defer gpa.free(home);

    const src_dir = try std.fs.path.join(gpa, &.{ home, ".planar", "local", "skills", "stamp" });
    defer gpa.free(src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);
    const src = try std.fs.path.join(gpa, &.{ src_dir, "SKILL.md" });
    defer gpa.free(src);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = src,
        .data =
        \\---
        \\description: stamp
        \\kind: skill
        \\vendors: [claude]
        \\---
        \\body
        ,
    });

    const file = try manifest.parseFile(src, .skill, gpa);
    defer manifest.deinitSandboxFile(file, gpa);

    const linked = try link(file, .{ .home_dir = home }, gpa);
    defer deinitLinkResult(linked, gpa);
    try std.testing.expectEqual(@as(usize, 1), linked.records.len);
    try std.testing.expect(!std.mem.eql(u8, linked.records[0].linked_at, "now"));
    try std.testing.expect(looksLikeRfc3339UtcSecondPrecision(linked.records[0].linked_at));

    const loaded = try manifest.loadManifest(home, .skill, gpa);
    defer manifest.deinitManifest(loaded, gpa);
    try std.testing.expectEqual(@as(usize, 1), loaded.entries.len);
    try std.testing.expect(looksLikeRfc3339UtcSecondPrecision(loaded.entries[0].links[0].linked_at));
}

test "reconcile removes stale manifest entry and target" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "home-reconcile" });
    defer gpa.free(home);

    const src_dir = try std.fs.path.join(gpa, &.{ home, ".planar", "local", "skills", "gone" });
    defer gpa.free(src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);
    const src = try std.fs.path.join(gpa, &.{ src_dir, "SKILL.md" });
    defer gpa.free(src);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = src,
        .data =
        \\---
        \\description: gone
        \\kind: skill
        \\vendors: [claude]
        \\---
        \\body
        ,
    });

    const file = try manifest.parseFile(src, .skill, gpa);
    defer manifest.deinitSandboxFile(file, gpa);
    const linked = try link(file, .{ .home_dir = home }, gpa);
    defer deinitLinkResult(linked, gpa);
    const target = try gpa.dupe(u8, linked.records[0].target_path);
    defer gpa.free(target);

    try std.Io.Dir.cwd().deleteFile(std.testing.io, src);

    const rec = try reconcile(.{ .home_dir = home }, gpa);
    defer deinitReconcileResult(rec, gpa);
    try std.testing.expectEqual(@as(usize, 1), rec.actions.len);
    try std.testing.expectEqualStrings("gone", rec.actions[0].name);
    try std.testing.expectEqualStrings("source-missing", rec.actions[0].reason);
    try std.testing.expectEqual(@as(usize, 1), rec.actions[0].removed_targets.len);
    try std.testing.expectEqualStrings(target, rec.actions[0].removed_targets[0]);

    try std.testing.expect(!pathExists(target));
    const rows = try list(home, gpa);
    defer deinitListRecords(rows, gpa);
    try std.testing.expectEqual(@as(usize, 0), rows.len);
}

test "reconcile repairs missing target for existing source" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "home-repair" });
    defer gpa.free(home);

    const src_dir = try std.fs.path.join(gpa, &.{ home, ".planar", "local", "skills", "repair" });
    defer gpa.free(src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);
    const src = try std.fs.path.join(gpa, &.{ src_dir, "SKILL.md" });
    defer gpa.free(src);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = src,
        .data =
        \\---
        \\description: repair
        \\kind: skill
        \\vendors: [claude]
        \\---
        \\body
        ,
    });

    const file = try manifest.parseFile(src, .skill, gpa);
    defer manifest.deinitSandboxFile(file, gpa);
    const linked = try link(file, .{ .home_dir = home }, gpa);
    defer deinitLinkResult(linked, gpa);
    const target = try gpa.dupe(u8, linked.records[0].target_path);
    defer gpa.free(target);
    try std.Io.Dir.cwd().deleteFile(std.testing.io, target);

    const rec = try reconcile(.{ .home_dir = home }, gpa);
    defer deinitReconcileResult(rec, gpa);
    try std.testing.expectEqual(@as(usize, 1), rec.actions.len);
    try std.testing.expectEqualStrings("target-missing", rec.actions[0].reason);
    try std.testing.expectEqual(@as(usize, 1), rec.actions[0].removed_targets.len);
    try std.testing.expectEqualStrings(target, rec.actions[0].removed_targets[0]);
    try std.Io.Dir.cwd().access(std.testing.io, target, .{});

    const rows = try list(home, gpa);
    defer deinitListRecords(rows, gpa);
    try std.testing.expectEqual(@as(usize, 1), rows.len);
    try std.testing.expectEqualStrings("live", rows[0].record.action);
}
