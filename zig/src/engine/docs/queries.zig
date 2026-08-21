//! engine/docs/queries - backlinks, orphan artifacts, and coverage reports.

const std = @import("std");
const db = @import("db");
const manifest = @import("manifest.zig");

pub const Backlink = struct {
    path: []const u8,
    source_hash: []const u8,
};

pub const Orphan = struct {
    id: i64,
    kind: []const u8,
    title: []const u8,
    updated_at: []const u8,
};

pub const Coverage = struct {
    total_done: i64,
    covered: i64,
    uncovered: []const PlanRow,
};

pub const PlanRow = struct {
    id: i64,
    title: []const u8,
};

pub fn deinitBacklinks(rows: []const Backlink, allocator: std.mem.Allocator) void {
    for (rows) |row| {
        allocator.free(row.path);
        allocator.free(row.source_hash);
    }
    allocator.free(rows);
}

pub fn deinitOrphans(rows: []const Orphan, allocator: std.mem.Allocator) void {
    for (rows) |row| {
        allocator.free(row.kind);
        allocator.free(row.title);
        allocator.free(row.updated_at);
    }
    allocator.free(rows);
}

pub fn deinitCoverage(c: Coverage, allocator: std.mem.Allocator) void {
    for (c.uncovered) |row| allocator.free(row.title);
    allocator.free(c.uncovered);
}

pub fn backlinks(allocator: std.mem.Allocator, entity_ref: []const u8) ![]Backlink {
    const m = try manifest.load(manifest.file_name, allocator);
    defer manifest.deinitManifest(m, allocator);
    var out: std.ArrayList(Backlink) = .empty;
    errdefer deinitBacklinks(out.items, allocator);
    for (m.entries) |entry| {
        for (entry.entry.sources) |source| {
            if (!std.mem.eql(u8, source.ref, entity_ref)) continue;
            try out.append(allocator, .{
                .path = try allocator.dupe(u8, entry.path),
                .source_hash = try allocator.dupe(u8, source.hash),
            });
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn orphans(d: *db.sqlite.Db, allocator: std.mem.Allocator, kind: []const u8, since_days: i64) ![]Orphan {
    const m = manifest.load(manifest.file_name, allocator) catch |e| switch (e) {
        error.FileNotFound => null,
        else => return e,
    };
    defer if (m) |mm| manifest.deinitManifest(mm, allocator);

    var stmt = d.prepare(
        "select id, kind, title, updated_at from artifacts where kind = ? " ++
            "and updated_at <= datetime('now', '-' || ? || ' days') order by updated_at, id",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .text = kind }, .{ .int = since_days } }) catch return error.QueryFailed;

    var out: std.ArrayList(Orphan) = .empty;
    errdefer deinitOrphans(out.items, allocator);
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const ref = try std.fmt.allocPrint(allocator, "artifact:{d}", .{id});
                defer allocator.free(ref);
                if (m) |mm| {
                    if (hasBacklink(mm, ref)) continue;
                }
                try out.append(allocator, .{
                    .id = id,
                    .kind = try stmt.columnTextAlloc(1, allocator),
                    .title = try stmt.columnTextAlloc(2, allocator),
                    .updated_at = try stmt.columnTextAlloc(3, allocator),
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn coverage(d: *db.sqlite.Db, allocator: std.mem.Allocator) !Coverage {
    const m = manifest.load(manifest.file_name, allocator) catch |e| switch (e) {
        error.FileNotFound => null,
        else => return e,
    };
    defer if (m) |mm| manifest.deinitManifest(mm, allocator);

    var stmt = d.prepare("select id, title from plans where status = 'done' order by id") catch return error.QueryFailed;
    defer stmt.finalize();
    var total: i64 = 0;
    var covered: i64 = 0;
    var uncovered: std.ArrayList(PlanRow) = .empty;
    errdefer {
        for (uncovered.items) |row| allocator.free(row.title);
        uncovered.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                total += 1;
                const id = stmt.columnInt(0);
                const ref = try std.fmt.allocPrint(allocator, "plan:{d}", .{id});
                defer allocator.free(ref);
                if (m != null and hasBacklink(m.?, ref)) {
                    covered += 1;
                    continue;
                }
                try uncovered.append(allocator, .{
                    .id = id,
                    .title = try stmt.columnTextAlloc(1, allocator),
                });
            },
        }
    }
    return .{ .total_done = total, .covered = covered, .uncovered = try uncovered.toOwnedSlice(allocator) };
}

fn hasBacklink(m: manifest.Manifest, entity_ref: []const u8) bool {
    for (m.entries) |entry| {
        for (entry.entry.sources) |source| {
            if (std.mem.eql(u8, source.ref, entity_ref)) return true;
        }
    }
    return false;
}
