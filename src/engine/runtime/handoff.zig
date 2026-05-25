//! engine/runtime/handoff — Handoffs entity + status transitions.
//!
//! Status lifecycle (CHECK-constrained):
//!
//!   pending   → {validated, consumed, abandoned}
//!   validated → {consumed, abandoned}
//!   consumed  → (terminal)
//!   abandoned → (terminal)
//!
//! Per plan 144 M4: handoffs are operator vendor-session state and
//! carry no scope columns. Mutations skip the cross-scope guard but
//! still emit an audit_log row.

const std = @import("std");
const db = @import("db");
const policy = @import("../policy.zig");

// =========================================================================
// Types
// =========================================================================

pub const Status = enum {
    pending,
    validated,
    consumed,
    abandoned,

    pub fn fromText(s: []const u8) ?Status {
        if (std.mem.eql(u8, s, "pending")) return .pending;
        if (std.mem.eql(u8, s, "validated")) return .validated;
        if (std.mem.eql(u8, s, "consumed")) return .consumed;
        if (std.mem.eql(u8, s, "abandoned")) return .abandoned;
        return null;
    }

    pub fn isTerminal(self: Status) bool {
        return self == .consumed or self == .abandoned;
    }
};

pub const Handoff = struct {
    id: i64,
    from_snapshot_id: i64,
    to_session_id: ?i64,
    from_vendor: []const u8,
    to_vendor: ?[]const u8,
    status: Status,
    validated_at: ?[]const u8,
    consumed_at: ?[]const u8,
    created_at: []const u8,
};

pub fn deinit(h: Handoff, allocator: std.mem.Allocator) void {
    allocator.free(h.from_vendor);
    if (h.to_vendor) |v| allocator.free(v);
    if (h.validated_at) |v| allocator.free(v);
    if (h.consumed_at) |v| allocator.free(v);
    allocator.free(h.created_at);
}

pub fn deinitMany(items: []const Handoff, allocator: std.mem.Allocator) void {
    for (items) |h| deinit(h, allocator);
    allocator.free(items);
}

pub const CreateArgs = struct {
    from_snapshot_id: i64,
    from_vendor: []const u8,
    to_vendor: ?[]const u8 = null,
};

pub const ListFilter = struct {
    /// Status filters. If empty, defaults to {pending} (Go parity).
    statuses: []const Status = &.{},
    task_id: ?i64 = null,
};

pub const Error = error{
    NotFound,
    IllegalTransition,
    QueryFailed,
} || std.mem.Allocator.Error || policy.audit.Error;

/// Validate a transition. Mirrors handoff.ValidateTransition in Go.
pub fn validateTransition(current: Status, next: Status) Error!void {
    if (current == next) return;
    if (current.isTerminal()) return Error.IllegalTransition;
    switch (current) {
        .pending => switch (next) {
            .validated, .consumed, .abandoned => return,
            else => return Error.IllegalTransition,
        },
        .validated => switch (next) {
            .consumed, .abandoned => return,
            else => return Error.IllegalTransition,
        },
        else => return Error.IllegalTransition,
    }
}

// =========================================================================
// CRUD
// =========================================================================

pub fn create(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: CreateArgs,
) Error!Handoff {
    const id = d.execParams(
        \\insert into handoffs (from_snapshot_id, from_vendor, to_vendor, status)
        \\values (?, ?, ?, 'pending')
    , &.{
        .{ .int = args.from_snapshot_id },
        .{ .text = args.from_vendor },
        if (args.to_vendor) |v| (if (v.len > 0) db.sqlite.Param{ .text = v } else .{ .null = {} }) else .{ .null = {} },
    }) catch return Error.QueryFailed;

    const summary = try std.fmt.allocPrint(
        allocator,
        "create handoff snapshot={d} from={s}",
        .{ args.from_snapshot_id, args.from_vendor },
    );
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "handoff", .id = id },
        .summary = summary,
    });

    return try show(d, allocator, id);
}

pub fn show(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
) Error!Handoff {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

/// Transition pending → validated, setting validated_at.
pub fn validate(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
) Error!Handoff {
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try validateTransition(current.status, .validated);

    _ = d.execParams(
        \\update handoffs
        \\set status = 'validated',
        \\    validated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\where id = ?
    , &.{.{ .int = id }}) catch return Error.QueryFailed;

    const summary = try std.fmt.allocPrint(allocator, "validate handoff id={d}", .{id});
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "handoff", .id = id },
        .summary = summary,
    });
    return try show(d, allocator, id);
}

pub const ConsumeArgs = struct {
    id: i64,
    session_id: ?i64 = null,
};

/// Transition pending|validated → consumed, optionally binding to_session_id.
pub fn consume(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: ConsumeArgs,
) Error!Handoff {
    const current = try show(d, allocator, args.id);
    defer deinit(current, allocator);
    try validateTransition(current.status, .consumed);

    if (args.session_id) |sid| {
        _ = d.execParams(
            \\update handoffs
            \\set status = 'consumed',
            \\    to_session_id = ?,
            \\    consumed_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
            \\where id = ?
        , &.{ .{ .int = sid }, .{ .int = args.id } }) catch return Error.QueryFailed;
    } else {
        _ = d.execParams(
            \\update handoffs
            \\set status = 'consumed',
            \\    consumed_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
            \\where id = ?
        , &.{.{ .int = args.id }}) catch return Error.QueryFailed;
    }

    const summary = try std.fmt.allocPrint(allocator, "consume handoff id={d}", .{args.id});
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "handoff", .id = args.id },
        .summary = summary,
    });
    return try show(d, allocator, args.id);
}

/// Transition any non-terminal → abandoned.
pub fn abandon(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    reason: ?[]const u8,
) Error!Handoff {
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try validateTransition(current.status, .abandoned);

    _ = d.execParams(
        "update handoffs set status = 'abandoned' where id = ?",
        &.{.{ .int = id }},
    ) catch return Error.QueryFailed;

    const summary = if (reason) |r|
        try std.fmt.allocPrint(allocator, "abandon handoff id={d} reason={s}", .{ id, r })
    else
        try std.fmt.allocPrint(allocator, "abandon handoff id={d}", .{id});
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "handoff", .id = id },
        .summary = summary,
    });
    return try show(d, allocator, id);
}

pub fn list(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: ListFilter,
) Error![]Handoff {
    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, "select h.");
    try sql_buf.appendSlice(allocator, select_columns_qualified);
    try sql_buf.appendSlice(allocator, " from handoffs h");

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    if (filter.task_id) |tid| {
        try sql_buf.appendSlice(allocator, " join context_snapshots cs on cs.id = h.from_snapshot_id where cs.task_id = ?");
        try params.append(allocator, .{ .int = tid });
    } else {
        try sql_buf.appendSlice(allocator, " where 1=1");
    }

    if (filter.statuses.len > 0) {
        try sql_buf.appendSlice(allocator, " and h.status in (");
        for (filter.statuses, 0..) |st, i| {
            if (i > 0) try sql_buf.appendSlice(allocator, ",");
            try sql_buf.appendSlice(allocator, "?");
            try params.append(allocator, .{ .text = @tagName(st) });
        }
        try sql_buf.appendSlice(allocator, ")");
    } else {
        try sql_buf.appendSlice(allocator, " and h.status = 'pending'");
    }

    try sql_buf.appendSlice(allocator, " order by h.created_at desc");

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var out: std.ArrayList(Handoff) = .empty;
    errdefer {
        for (out.items) |h| deinit(h, allocator);
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

/// Return the most-recent non-terminal handoff anchored at the snapshot,
/// or null if none.
pub fn getPendingForSnapshot(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    snapshot_id: i64,
) Error!?Handoff {
    const sql: [:0]const u8 =
        "select " ++ select_columns ++
        \\ from handoffs
        \\ where from_snapshot_id = ? and status in ('pending','validated')
        \\ order by created_at desc limit 1
        ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = snapshot_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => null,
        .row => try readRow(&stmt, allocator),
    };
}

// =========================================================================
// Internals
// =========================================================================

const select_columns =
    "id, from_snapshot_id, to_session_id, from_vendor, to_vendor, " ++
    "status, validated_at, consumed_at, created_at";

const select_columns_qualified =
    "id, from_snapshot_id, to_session_id, from_vendor, to_vendor, " ++
    "status, validated_at, consumed_at, created_at";

const select_one_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from handoffs where id = ?";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Handoff {
    const status_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(status_text);
    const status = Status.fromText(status_text) orelse return Error.QueryFailed;

    return .{
        .id = stmt.columnInt(0),
        .from_snapshot_id = stmt.columnInt(1),
        .to_session_id = stmt.columnIntOpt(2),
        .from_vendor = try stmt.columnTextAlloc(3, allocator),
        .to_vendor = try stmt.columnTextOpt(4, allocator),
        .status = status,
        .validated_at = try stmt.columnTextOpt(6, allocator),
        .consumed_at = try stmt.columnTextOpt(7, allocator),
        .created_at = try stmt.columnTextAlloc(8, allocator),
    };
}

// =========================================================================
// Tests
// =========================================================================

const session = @import("session.zig");
const snapshot = @import("snapshot.zig");

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

fn setupFixture(d: *db.sqlite.Db, allocator: std.mem.Allocator) !struct { sess: session.Session, snap: snapshot.Snapshot } {
    const s = try session.startSession(d, allocator, .{ .vendor = "claude" });
    errdefer session.deinit(s, allocator);
    const snap = try snapshot.create(d, allocator, .{
        .session_id = s.id,
        .vendor = "claude",
        .body = "ctx",
        .next_action = "do x",
    });
    return .{ .sess = s, .snap = snap };
}

test "validateTransition: pending → validated|consumed|abandoned" {
    try validateTransition(.pending, .validated);
    try validateTransition(.pending, .consumed);
    try validateTransition(.pending, .abandoned);
}

test "validateTransition: terminal → anything is IllegalTransition" {
    try std.testing.expectError(Error.IllegalTransition, validateTransition(.consumed, .abandoned));
    try std.testing.expectError(Error.IllegalTransition, validateTransition(.abandoned, .consumed));
}

test "validateTransition: validated → pending is IllegalTransition" {
    try std.testing.expectError(Error.IllegalTransition, validateTransition(.validated, .pending));
}

test "create + show round-trip with status=pending" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const fx = try setupFixture(&d, a);
    defer session.deinit(fx.sess, a);
    defer snapshot.deinit(fx.snap, a);

    const h = try create(&d, a, .{
        .from_snapshot_id = fx.snap.id,
        .from_vendor = "claude",
        .to_vendor = "codex",
    });
    defer deinit(h, a);
    try std.testing.expectEqual(Status.pending, h.status);
    try std.testing.expectEqualStrings("claude", h.from_vendor);
    try std.testing.expectEqualStrings("codex", h.to_vendor.?);
    try std.testing.expect(h.validated_at == null);
    try std.testing.expect(h.consumed_at == null);
}

test "validate transitions pending → validated and sets validated_at" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const fx = try setupFixture(&d, a);
    defer session.deinit(fx.sess, a);
    defer snapshot.deinit(fx.snap, a);
    const h = try create(&d, a, .{ .from_snapshot_id = fx.snap.id, .from_vendor = "v" });
    defer deinit(h, a);

    const v = try validate(&d, a, h.id);
    defer deinit(v, a);
    try std.testing.expectEqual(Status.validated, v.status);
    try std.testing.expect(v.validated_at != null);
}

test "consume transitions to consumed and sets consumed_at + session" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const fx = try setupFixture(&d, a);
    defer session.deinit(fx.sess, a);
    defer snapshot.deinit(fx.snap, a);
    const h = try create(&d, a, .{ .from_snapshot_id = fx.snap.id, .from_vendor = "v" });
    defer deinit(h, a);

    const consumed = try consume(&d, a, .{ .id = h.id, .session_id = fx.sess.id });
    defer deinit(consumed, a);
    try std.testing.expectEqual(Status.consumed, consumed.status);
    try std.testing.expect(consumed.consumed_at != null);
    try std.testing.expectEqual(fx.sess.id, consumed.to_session_id.?);
}

test "abandon transitions any non-terminal to abandoned" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const fx = try setupFixture(&d, a);
    defer session.deinit(fx.sess, a);
    defer snapshot.deinit(fx.snap, a);
    const h = try create(&d, a, .{ .from_snapshot_id = fx.snap.id, .from_vendor = "v" });
    defer deinit(h, a);

    const abandoned = try abandon(&d, a, h.id, "stopped working");
    defer deinit(abandoned, a);
    try std.testing.expectEqual(Status.abandoned, abandoned.status);
}

test "abandon on terminal handoff returns IllegalTransition" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const fx = try setupFixture(&d, a);
    defer session.deinit(fx.sess, a);
    defer snapshot.deinit(fx.snap, a);
    const h = try create(&d, a, .{ .from_snapshot_id = fx.snap.id, .from_vendor = "v" });
    defer deinit(h, a);
    const consumed = try consume(&d, a, .{ .id = h.id });
    defer deinit(consumed, a);
    try std.testing.expectError(Error.IllegalTransition, abandon(&d, a, h.id, null));
}

test "list defaults to status=pending" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const fx = try setupFixture(&d, a);
    defer session.deinit(fx.sess, a);
    defer snapshot.deinit(fx.snap, a);
    const h1 = try create(&d, a, .{ .from_snapshot_id = fx.snap.id, .from_vendor = "v" });
    defer deinit(h1, a);
    const h2 = try create(&d, a, .{ .from_snapshot_id = fx.snap.id, .from_vendor = "v" });
    defer deinit(h2, a);
    const consumed = try consume(&d, a, .{ .id = h1.id });
    defer deinit(consumed, a);

    const got = try list(&d, a, .{});
    defer deinitMany(got, a);
    try std.testing.expectEqual(@as(usize, 1), got.len);
    try std.testing.expectEqual(h2.id, got[0].id);
}

test "list with explicit statuses filters" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const fx = try setupFixture(&d, a);
    defer session.deinit(fx.sess, a);
    defer snapshot.deinit(fx.snap, a);
    const h1 = try create(&d, a, .{ .from_snapshot_id = fx.snap.id, .from_vendor = "v" });
    defer deinit(h1, a);
    const consumed_h = try consume(&d, a, .{ .id = h1.id });
    deinit(consumed_h, a);

    const got = try list(&d, a, .{ .statuses = &.{.consumed} });
    defer deinitMany(got, a);
    try std.testing.expectEqual(@as(usize, 1), got.len);
}

test "show returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
}
