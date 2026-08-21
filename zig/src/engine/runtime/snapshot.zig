//! engine/runtime/snapshot — context_snapshots table.
//!
//! A context snapshot is the resume packet payload produced at terminate
//! time or on demand. It carries a narrative body, an exact next_action,
//! and the vendor identity needed for cross-vendor handoff. Consumed by
//! `planar resume`; produced by `capture snapshot` and the `handoff`
//! combined ritual.
//!
//! Per plan 144 M4: snapshots are operator vendor-session state and
//! carry no scope columns; mutations are NOT subject to the cross-scope
//! guard. The audit policy IS invoked.

const std = @import("std");
const db = @import("db");
const policy = @import("../policy.zig");

// =========================================================================
// Types
// =========================================================================

pub const Snapshot = struct {
    id: i64,
    session_id: i64,
    task_id: ?i64,
    vendor: []const u8,
    vendor_session_id: ?[]const u8,
    body: []const u8,
    next_action: []const u8,
    created_at: []const u8,
};

pub fn deinit(s: Snapshot, allocator: std.mem.Allocator) void {
    allocator.free(s.vendor);
    if (s.vendor_session_id) |v| allocator.free(v);
    allocator.free(s.body);
    allocator.free(s.next_action);
    allocator.free(s.created_at);
}

pub fn deinitMany(items: []const Snapshot, allocator: std.mem.Allocator) void {
    for (items) |s| deinit(s, allocator);
    allocator.free(items);
}

pub const CreateArgs = struct {
    session_id: i64,
    task_id: ?i64 = null,
    vendor: []const u8,
    vendor_session_id: ?[]const u8 = null,
    body: ?[]const u8 = null,
    next_action: ?[]const u8 = null,
};

pub const Error = error{
    NotFound,
    QueryFailed,
} || std.mem.Allocator.Error || policy.audit.Error;

// =========================================================================
// CRUD
// =========================================================================

pub fn create(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: CreateArgs,
) Error!Snapshot {
    const id = d.execParams(
        \\insert into context_snapshots
        \\  (session_id, task_id, vendor, vendor_session_id, body, next_action)
        \\values (?, ?, ?, ?, ?, ?)
    , &.{
        .{ .int = args.session_id },
        if (args.task_id) |t| .{ .int = t } else .{ .null = {} },
        .{ .text = args.vendor },
        if (args.vendor_session_id) |v| .{ .text = v } else .{ .null = {} },
        if (args.body) |b| (if (b.len > 0) db.sqlite.Param{ .text = b } else .{ .null = {} }) else .{ .null = {} },
        if (args.next_action) |n| (if (n.len > 0) db.sqlite.Param{ .text = n } else .{ .null = {} }) else .{ .null = {} },
    }) catch return Error.QueryFailed;

    const summary = try std.fmt.allocPrint(
        allocator,
        "create snapshot session={d} vendor={s}",
        .{ args.session_id, args.vendor },
    );
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "context_snapshot", .id = id },
        .summary = summary,
    });
    return try show(d, allocator, id);
}

pub fn show(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
) Error!Snapshot {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

/// Return the most recent snapshot for the given task, or null when no
/// snapshot has been captured. Ordered by (created_at DESC, id DESC) to
/// stably break ties within the same millisecond.
pub fn getLatestForTask(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error!?Snapshot {
    const sql: [:0]const u8 =
        "select " ++ select_columns ++
        " from context_snapshots where task_id = ? order by created_at desc, id desc limit 1";
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => null,
        .row => try readRow(&stmt, allocator),
    };
}

pub fn listForTask(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error![]Snapshot {
    const sql: [:0]const u8 =
        "select " ++ select_columns ++
        " from context_snapshots where task_id = ? order by created_at desc, id desc";
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(Snapshot) = .empty;
    errdefer {
        for (out.items) |s| deinit(s, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Internals
// =========================================================================

const select_columns =
    "id, session_id, task_id, vendor, vendor_session_id, " ++
    "coalesce(body, ''), coalesce(next_action, ''), created_at";

const select_one_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from context_snapshots where id = ?";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Snapshot {
    return .{
        .id = stmt.columnInt(0),
        .session_id = stmt.columnInt(1),
        .task_id = stmt.columnIntOpt(2),
        .vendor = try stmt.columnTextAlloc(3, allocator),
        .vendor_session_id = try stmt.columnTextOpt(4, allocator),
        .body = try stmt.columnTextAlloc(5, allocator),
        .next_action = try stmt.columnTextAlloc(6, allocator),
        .created_at = try stmt.columnTextAlloc(7, allocator),
    };
}

// =========================================================================
// Tests
// =========================================================================

const session = @import("session.zig");

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "create + show round-trip with optional task" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sess = try session.startSession(&d, a, .{ .vendor = "v" });
    defer session.deinit(sess, a);
    const snap = try create(&d, a, .{
        .session_id = sess.id,
        .vendor = "v",
        .body = "context body",
        .next_action = "do the thing",
    });
    defer deinit(snap, a);
    try std.testing.expect(snap.id > 0);
    try std.testing.expectEqualStrings("context body", snap.body);
    try std.testing.expectEqualStrings("do the thing", snap.next_action);
    try std.testing.expect(snap.task_id == null);
}

test "create with empty body/next_action stores empty strings via coalesce" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sess = try session.startSession(&d, a, .{ .vendor = "v" });
    defer session.deinit(sess, a);
    const snap = try create(&d, a, .{
        .session_id = sess.id,
        .vendor = "v",
    });
    defer deinit(snap, a);
    try std.testing.expectEqualStrings("", snap.body);
    try std.testing.expectEqualStrings("", snap.next_action);
}

test "getLatestForTask returns null when none, snapshot when present" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const t = try d.execParams("insert into tasks (scope_kind, title, status) values ('global', 't', 'todo')", &.{});
    try std.testing.expect((try getLatestForTask(&d, a, t)) == null);
    const sess = try session.startSession(&d, a, .{ .vendor = "v", .task_id = t });
    defer session.deinit(sess, a);
    const s1 = try create(&d, a, .{
        .session_id = sess.id,
        .task_id = t,
        .vendor = "v",
        .body = "first",
        .next_action = "n1",
    });
    defer deinit(s1, a);
    const latest = (try getLatestForTask(&d, a, t)).?;
    defer deinit(latest, a);
    try std.testing.expectEqual(s1.id, latest.id);
}

test "show returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
}
