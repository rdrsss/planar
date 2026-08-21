const std = @import("std");
const db = @import("db");
const c = @cImport({
    // glibc's fortified open/openat wrappers in bits/fcntl2.h use
    // __attribute__((__error__(...))), which translate-c cannot represent —
    // disable fortification so translate-c sees the plain declarations.
    @cDefine("_FORTIFY_SOURCE", "0");
    @cInclude("fcntl.h");
    @cInclude("unistd.h");
    @cInclude("stdio.h");
});

pub const SyncState = struct {
    id: i64,
    anchor_plan_id: i64,
    entity_kind: []const u8,
    entity_id: i64,
    file_path: []const u8,
    content_hash: []const u8,
    fs_mtime: []const u8,
    db_updated_at: []const u8,
    last_synced_at: []const u8,
};

pub fn deinitRow(row: SyncState, allocator: std.mem.Allocator) void {
    allocator.free(row.entity_kind);
    allocator.free(row.file_path);
    allocator.free(row.content_hash);
    allocator.free(row.fs_mtime);
    allocator.free(row.db_updated_at);
    allocator.free(row.last_synced_at);
}

pub fn deinitRows(rows: []const SyncState, allocator: std.mem.Allocator) void {
    for (rows) |row| deinitRow(row, allocator);
    allocator.free(rows);
}

pub fn hashContent(allocator: std.mem.Allocator, content: []const u8) ![]u8 {
    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(content, &digest, .{});
    return std.fmt.allocPrint(allocator, "{x}", .{digest});
}

pub fn load(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) ![]SyncState {
    var stmt = d.prepare(
        \\select id, anchor_plan_id, entity_kind, entity_id, file_path, content_hash,
        \\       coalesce(fs_mtime, ''), coalesce(db_updated_at, ''), coalesce(last_synced_at, '')
        \\from workbench_sync_state
        \\where anchor_plan_id = ?
        \\order by file_path
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(SyncState) = .empty;
    errdefer {
        for (out.items) |row| deinitRow(row, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .anchor_plan_id = stmt.columnInt(1),
                .entity_kind = try stmt.columnTextAlloc(2, allocator),
                .entity_id = stmt.columnInt(3),
                .file_path = try stmt.columnTextAlloc(4, allocator),
                .content_hash = try stmt.columnTextAlloc(5, allocator),
                .fs_mtime = try stmt.columnTextAlloc(6, allocator),
                .db_updated_at = try stmt.columnTextAlloc(7, allocator),
                .last_synced_at = try stmt.columnTextAlloc(8, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn upsert(d: *db.sqlite.Db, row: SyncState) !void {
    _ = d.execParams(
        \\insert into workbench_sync_state
        \\(anchor_plan_id, entity_kind, entity_id, file_path, content_hash, fs_mtime, db_updated_at, last_synced_at)
        \\values (?, ?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
        \\on conflict(file_path) do update set
        \\  anchor_plan_id = excluded.anchor_plan_id,
        \\  entity_kind = excluded.entity_kind,
        \\  entity_id = excluded.entity_id,
        \\  content_hash = excluded.content_hash,
        \\  fs_mtime = excluded.fs_mtime,
        \\  db_updated_at = excluded.db_updated_at,
        \\  last_synced_at = excluded.last_synced_at
    , &.{
        .{ .int = row.anchor_plan_id },
        .{ .text = row.entity_kind },
        .{ .int = row.entity_id },
        .{ .text = row.file_path },
        .{ .text = row.content_hash },
        .{ .text = row.fs_mtime },
        .{ .text = row.db_updated_at },
    }) catch return error.QueryFailed;
}

pub fn deleteByFilePath(d: *db.sqlite.Db, file_path: []const u8) !void {
    _ = d.execParams("delete from workbench_sync_state where file_path = ?", &.{.{ .text = file_path }}) catch return error.QueryFailed;
}

pub fn writeSyncFile(
    allocator: std.mem.Allocator,
    feature_dir: []const u8,
    rows: []const SyncState,
) !void {
    const sync_path = try std.fs.path.join(allocator, &.{ feature_dir, ".sync" });
    defer allocator.free(sync_path);
    const tmp_path = try std.fmt.allocPrint(allocator, "{s}.tmp", .{sync_path});
    defer allocator.free(tmp_path);

    var body: std.ArrayList(u8) = .empty;
    defer body.deinit(allocator);
    for (rows) |row| {
        const rel = featureRelPath(row.file_path);
        const line = try std.fmt.allocPrint(allocator, "{s}\t{s}:{d}\t{s}\t{s}\n", .{
            rel,
            row.entity_kind,
            row.entity_id,
            row.content_hash,
            row.last_synced_at,
        });
        defer allocator.free(line);
        try body.appendSlice(allocator, line);
    }
    try writeFile(tmp_path, body.items);
    const tmp_z = try allocator.dupeZ(u8, tmp_path);
    defer allocator.free(tmp_z);
    const sync_z = try allocator.dupeZ(u8, sync_path);
    defer allocator.free(sync_z);
    if (c.rename(tmp_z.ptr, sync_z.ptr) != 0) return error.RenameFailed;
}

fn featureRelPath(file_path: []const u8) []const u8 {
    const p = std.mem.trim(u8, file_path, "/");
    const first_sep = std.mem.indexOfScalar(u8, p, '/') orelse return p;
    const second_sep = std.mem.indexOfScalarPos(u8, p, first_sep + 1, '/') orelse return p;
    return p[second_sep + 1 ..];
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
