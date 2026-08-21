//! engine/runtime/audit_trail — read-only audit trail queries.
//!
//! Reads `audit_log` rows (recorded by every data-plane mutation via
//! `engine.policy.audit.record`) plus optional traversal through
//! `entity_links` to gather related entities' audit rows.
//!
//! Per the M7 spec (engine-audit-read): this is a READ verb. It never
//! inserts into audit_log; mutations go through `policy.audit.record`.
//!
//! The external_links / sync_events path (the Go binary's `audit trail
//! <link-id>`) lives in M8 alongside the external plane port — we don't
//! reach into it here because the table isn't queryable until M8 lands.

const std = @import("std");
const db = @import("db");

// =========================================================================
// Types
// =========================================================================

pub const AuditEntry = struct {
    id: i64,
    verb: []const u8,
    entity_kind: []const u8,
    entity_id: i64,
    actor: ?[]const u8,
    scope: ?[]const u8,
    summary: ?[]const u8,
    recorded_at: []const u8,
};

pub fn deinitEntry(e: AuditEntry, allocator: std.mem.Allocator) void {
    allocator.free(e.verb);
    allocator.free(e.entity_kind);
    if (e.actor) |a| allocator.free(a);
    if (e.scope) |s| allocator.free(s);
    if (e.summary) |s| allocator.free(s);
    allocator.free(e.recorded_at);
}

pub fn deinitEntries(items: []const AuditEntry, allocator: std.mem.Allocator) void {
    for (items) |e| deinitEntry(e, allocator);
    allocator.free(items);
}

pub const Error = error{
    QueryFailed,
} || std.mem.Allocator.Error;

// =========================================================================
// Queries
// =========================================================================

/// All audit_log rows for a single entity, oldest first.
pub fn forEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_kind: []const u8,
    entity_id: i64,
) Error![]AuditEntry {
    const sql: [:0]const u8 =
        \\select id, verb, entity_kind, entity_id, actor, scope, summary, recorded_at
        \\from audit_log
        \\where entity_kind = ? and entity_id = ?
        \\order by id
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .text = entity_kind }, .{ .int = entity_id } }) catch return Error.QueryFailed;
    return try readRows(&stmt, allocator);
}

/// Audit rows for the entity plus everything reachable from it through
/// `entity_links` (one-hop, either direction). Used by `audit trail
/// <task-id>` to surface decisions, questions, etc. linked to a task.
pub fn forEntityWithLinks(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_kind: []const u8,
    entity_id: i64,
) Error![]AuditEntry {
    const sql: [:0]const u8 =
        \\select id, verb, entity_kind, entity_id, actor, scope, summary, recorded_at
        \\from audit_log
        \\where (entity_kind = ? and entity_id = ?)
        \\   or (entity_kind, entity_id) in (
        \\        select to_kind, to_id from entity_links
        \\        where from_kind = ? and from_id = ?
        \\        union
        \\        select from_kind, from_id from entity_links
        \\        where to_kind = ? and to_id = ?
        \\      )
        \\order by id
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .text = entity_kind },
        .{ .int = entity_id },
        .{ .text = entity_kind },
        .{ .int = entity_id },
        .{ .text = entity_kind },
        .{ .int = entity_id },
    }) catch return Error.QueryFailed;
    return try readRows(&stmt, allocator);
}

/// Audit rows whose `summary` matches a substring (used to recover
/// sentinel-bodied notes like "plan_status:" without a dedicated table).
pub fn forEntityGrep(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_kind: []const u8,
    entity_id: i64,
    pattern: []const u8,
) Error![]AuditEntry {
    var like_buf: std.ArrayList(u8) = .empty;
    defer like_buf.deinit(allocator);
    try like_buf.append(allocator, '%');
    try like_buf.appendSlice(allocator, pattern);
    try like_buf.append(allocator, '%');

    const sql: [:0]const u8 =
        \\select id, verb, entity_kind, entity_id, actor, scope, summary, recorded_at
        \\from audit_log
        \\where entity_kind = ? and entity_id = ? and summary like ?
        \\order by id
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .text = entity_kind },
        .{ .int = entity_id },
        .{ .text = like_buf.items },
    }) catch return Error.QueryFailed;
    return try readRows(&stmt, allocator);
}

// =========================================================================
// Session timeline (cross-cutting read used by `audit session`)
// =========================================================================

pub const SessionTimeline = struct {
    session_id: i64,
    vendor: []const u8,
    task_id: ?i64,
    started_at: []const u8,
    ended_at: ?[]const u8,
    entries: []const SessionEntry,
};

pub const SessionEntry = struct {
    ordinal: i64,
    prefix: []const u8,
    body: []const u8,
};

pub fn deinitTimeline(t: SessionTimeline, allocator: std.mem.Allocator) void {
    allocator.free(t.vendor);
    allocator.free(t.started_at);
    if (t.ended_at) |s| allocator.free(s);
    for (t.entries) |e| {
        allocator.free(e.prefix);
        allocator.free(e.body);
    }
    allocator.free(t.entries);
}

pub const NotFoundError = error{NotFound};

/// Fetch a session row + its full timeline. Returns NotFound when no
/// such session_id exists.
pub fn sessionTimeline(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
) (Error || NotFoundError)!SessionTimeline {
    var vendor: []const u8 = "";
    var task_id: ?i64 = null;
    var started_at: []const u8 = "";
    var ended_at: ?[]const u8 = null;

    {
        const sql: [:0]const u8 =
            "select vendor, task_id, started_at, ended_at from sessions where id = ?";
        var stmt = d.prepare(sql) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = session_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return error.NotFound,
            .row => {
                vendor = try stmt.columnTextAlloc(0, allocator);
                task_id = stmt.columnIntOpt(1);
                started_at = try stmt.columnTextAlloc(2, allocator);
                ended_at = try stmt.columnTextOpt(3, allocator);
            },
        }
    }
    errdefer {
        allocator.free(vendor);
        allocator.free(started_at);
        if (ended_at) |s| allocator.free(s);
    }

    var entries: std.ArrayList(SessionEntry) = .empty;
    errdefer {
        for (entries.items) |e| {
            allocator.free(e.prefix);
            allocator.free(e.body);
        }
        entries.deinit(allocator);
    }

    const sql: [:0]const u8 =
        "select ordinal, prefix, body from session_entries where session_id = ? order by ordinal";
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return Error.QueryFailed;
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try entries.append(allocator, .{
                .ordinal = stmt.columnInt(0),
                .prefix = try stmt.columnTextAlloc(1, allocator),
                .body = try stmt.columnTextAlloc(2, allocator),
            }),
        }
    }

    return .{
        .session_id = session_id,
        .vendor = vendor,
        .task_id = task_id,
        .started_at = started_at,
        .ended_at = ended_at,
        .entries = try entries.toOwnedSlice(allocator),
    };
}

// =========================================================================
// Internals
// =========================================================================

fn readRows(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error![]AuditEntry {
    var out: std.ArrayList(AuditEntry) = .empty;
    errdefer {
        for (out.items) |e| deinitEntry(e, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .verb = try stmt.columnTextAlloc(1, allocator),
                .entity_kind = try stmt.columnTextAlloc(2, allocator),
                .entity_id = stmt.columnInt(3),
                .actor = try stmt.columnTextOpt(4, allocator),
                .scope = try stmt.columnTextOpt(5, allocator),
                .summary = try stmt.columnTextOpt(6, allocator),
                .recorded_at = try stmt.columnTextAlloc(7, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Tests
// =========================================================================

const policy = @import("../policy.zig");

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "forEntity returns rows in id order" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try policy.audit.record(&d, .{
        .verb = .create,
        .entity = .{ .kind = "task", .id = 1 },
        .summary = "one",
    });
    try policy.audit.record(&d, .{
        .verb = .update,
        .entity = .{ .kind = "task", .id = 1 },
        .summary = "two",
    });
    const got = try forEntity(&d, a, "task", 1);
    defer deinitEntries(got, a);
    try std.testing.expectEqual(@as(usize, 2), got.len);
    try std.testing.expectEqualStrings("one", got[0].summary.?);
    try std.testing.expectEqualStrings("two", got[1].summary.?);
}

test "forEntityWithLinks pulls related entities via entity_links" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try policy.audit.record(&d, .{
        .verb = .create,
        .entity = .{ .kind = "task", .id = 5 },
        .summary = "task5",
    });
    try policy.audit.record(&d, .{
        .verb = .create,
        .entity = .{ .kind = "question", .id = 9 },
        .summary = "q9",
    });
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 5, 'question', 9, 'addresses')",
        &.{},
    );

    const got = try forEntityWithLinks(&d, a, "task", 5);
    defer deinitEntries(got, a);
    try std.testing.expectEqual(@as(usize, 2), got.len);
}

test "forEntityGrep filters by substring in summary" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try policy.audit.record(&d, .{
        .verb = .create,
        .entity = .{ .kind = "task", .id = 1 },
        .summary = "create task #1",
    });
    try policy.audit.record(&d, .{
        .verb = .update,
        .entity = .{ .kind = "task", .id = 1 },
        .summary = "plan_status: 5",
    });
    const got = try forEntityGrep(&d, a, "task", 1, "plan_status");
    defer deinitEntries(got, a);
    try std.testing.expectEqual(@as(usize, 1), got.len);
    try std.testing.expectEqualStrings("plan_status: 5", got[0].summary.?);
}

test "sessionTimeline returns ordered entries" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'note', 'first')",
        &.{.{ .int = sid }},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 2, 'action', 'second')",
        &.{.{ .int = sid }},
    );

    const t = try sessionTimeline(&d, a, sid);
    defer deinitTimeline(t, a);
    try std.testing.expectEqualStrings("claude", t.vendor);
    try std.testing.expectEqual(@as(usize, 2), t.entries.len);
    try std.testing.expectEqualStrings("first", t.entries[0].body);
    try std.testing.expectEqualStrings("second", t.entries[1].body);
}

test "sessionTimeline returns NotFound on missing session" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(error.NotFound, sessionTimeline(&d, a, 999));
}
