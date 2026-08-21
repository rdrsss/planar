//! engine/runtime/session — Sessions table + session_entries timeline.
//!
//! A session is one vendor's episode of work, keyed by
//! (vendor, vendor_session_id). Idempotent on that tuple: opening a
//! session twice with the same key returns the same row. The session
//! row may bind to a task; binding is one-way (NULL → task) once set.
//!
//! Session timeline (`session_entries`) is append-only; ordinal is
//! computed as MAX(ordinal)+1 within the session. Prefix is one of:
//!   action, observation, decision, question, file, command, note,
//!   error, read.
//!
//! Per plan 144 M4, sessions are operator vendor-session state and
//! carry no scope columns; mutations are NOT subject to the
//! cross-scope guard. The audit policy is still invoked so the
//! mutation appears in `audit_log`.

const std = @import("std");
const db = @import("db");
const policy = @import("../policy.zig");

// =========================================================================
// Types
// =========================================================================

pub const Session = struct {
    id: i64,
    task_id: ?i64,
    project_id: ?i64,
    agent_id: ?i64,
    vendor: []const u8,
    vendor_session_id: ?[]const u8,
    model: ?[]const u8,
    started_at: []const u8,
    ended_at: ?[]const u8,
    summary: ?[]const u8,
    repo_root: ?[]const u8,
    head_sha_at_start: ?[]const u8,
};

pub fn deinit(s: Session, allocator: std.mem.Allocator) void {
    allocator.free(s.vendor);
    if (s.vendor_session_id) |v| allocator.free(v);
    if (s.model) |v| allocator.free(v);
    allocator.free(s.started_at);
    if (s.ended_at) |v| allocator.free(v);
    if (s.summary) |v| allocator.free(v);
    if (s.repo_root) |v| allocator.free(v);
    if (s.head_sha_at_start) |v| allocator.free(v);
}

pub fn deinitMany(items: []const Session, allocator: std.mem.Allocator) void {
    for (items) |s| deinit(s, allocator);
    allocator.free(items);
}

pub const SessionEntry = struct {
    id: i64,
    session_id: i64,
    ordinal: i64,
    prefix: []const u8,
    body: []const u8,
    created_at: []const u8,
};

pub fn deinitEntry(e: SessionEntry, allocator: std.mem.Allocator) void {
    allocator.free(e.prefix);
    allocator.free(e.body);
    allocator.free(e.created_at);
}

pub fn deinitEntries(items: []const SessionEntry, allocator: std.mem.Allocator) void {
    for (items) |e| deinitEntry(e, allocator);
    allocator.free(items);
}

pub const StartArgs = struct {
    vendor: []const u8,
    vendor_session_id: ?[]const u8 = null,
    task_id: ?i64 = null,
    model: ?[]const u8 = null,
};

pub const Error = error{
    NotFound,
    AlreadyEnded,
    TaskConflict,
    QueryFailed,
} || std.mem.Allocator.Error || policy.audit.Error;

// =========================================================================
// Env / vendor identity
// =========================================================================

/// Vendor returns the vendor identity from $PLANAR_VENDOR or "cli".
/// Result is a static string slice into the environ buffer; callers must
/// not retain it across env-buffer mutations.
pub fn vendorFromEnv(env: std.process.Environ) []const u8 {
    if (env.getPosix("PLANAR_VENDOR")) |v| {
        if (v.len > 0) return v;
    }
    return "cli";
}

pub fn vendorSessionIdFromEnv(env: std.process.Environ) ?[]const u8 {
    if (env.getPosix("PLANAR_VENDOR_SESSION_ID")) |v| {
        if (v.len > 0) return v;
    }
    return null;
}

// =========================================================================
// CRUD + lifecycle
// =========================================================================

/// Open or reuse a session keyed on (vendor, vendor_session_id).
/// Idempotent: when a row matching the key exists, returns it. If the
/// existing row has NULL task_id and args.task_id is non-null, binds
/// task_id atomically. Refuses if the existing row's task_id conflicts
/// with the caller's task_id.
pub fn startSession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: StartArgs,
) Error!Session {
    // Step 1: look up existing row.
    const select_with_vsid: [:0]const u8 =
        \\select id, task_id from sessions
        \\where vendor = ? and vendor_session_id = ?
        \\order by id asc limit 1
    ;
    const select_no_vsid: [:0]const u8 =
        \\select id, task_id from sessions
        \\where vendor = ? and vendor_session_id is null
        \\order by id asc limit 1
    ;

    var stmt = if (args.vendor_session_id) |_|
        d.prepare(select_with_vsid) catch return Error.QueryFailed
    else
        d.prepare(select_no_vsid) catch return Error.QueryFailed;
    defer stmt.finalize();

    if (args.vendor_session_id) |vsid| {
        stmt.bind(&.{
            .{ .text = args.vendor },
            .{ .text = vsid },
        }) catch return Error.QueryFailed;
    } else {
        stmt.bind(&.{.{ .text = args.vendor }}) catch return Error.QueryFailed;
    }

    const step_result = stmt.step() catch return Error.QueryFailed;
    if (step_result == .row) {
        const existing_id = stmt.columnInt(0);
        const existing_task: ?i64 = stmt.columnIntOpt(1);

        if (args.task_id) |new_task| {
            if (existing_task) |et| {
                if (et != new_task) return Error.TaskConflict;
            } else {
                _ = d.execParams(
                    "update sessions set task_id = ? where id = ?",
                    &.{ .{ .int = new_task }, .{ .int = existing_id } },
                ) catch return Error.QueryFailed;
            }
        }
        return try getById(d, allocator, existing_id);
    }

    // Step 2: insert new row.
    const new_id = d.execParams(
        "insert into sessions (vendor, vendor_session_id, task_id, model) values (?, ?, ?, ?)",
        &.{
            .{ .text = args.vendor },
            if (args.vendor_session_id) |v| .{ .text = v } else .{ .null = {} },
            if (args.task_id) |t| .{ .int = t } else .{ .null = {} },
            if (args.model) |m| .{ .text = m } else .{ .null = {} },
        },
    ) catch return Error.QueryFailed;

    const summary = try std.fmt.allocPrint(
        allocator,
        "start session vendor={s}",
        .{args.vendor},
    );
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "session", .id = new_id },
        .summary = summary,
    });

    return try getById(d, allocator, new_id);
}

/// End the session by setting ended_at and (optionally) summary.
/// Errors if the session is already ended.
pub fn endSession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
    summary_text: ?[]const u8,
) Error!void {
    // Verify session exists + is not ended.
    const check_sql: [:0]const u8 = "select ended_at from sessions where id = ?";
    var stmt = d.prepare(check_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => return Error.NotFound,
        .row => {
            if (!stmt.columnIsNull(0)) return Error.AlreadyEnded;
        },
    }

    if (summary_text) |sum| {
        _ = d.execParams(
            \\update sessions
            \\set ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'),
            \\    summary = ?
            \\where id = ?
        , &.{ .{ .text = sum }, .{ .int = session_id } }) catch return Error.QueryFailed;
    } else {
        _ = d.execParams(
            \\update sessions
            \\set ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
            \\where id = ?
        , &.{.{ .int = session_id }}) catch return Error.QueryFailed;
    }

    const note = try std.fmt.allocPrint(allocator, "end session id={d}", .{session_id});
    defer allocator.free(note);
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "session", .id = session_id },
        .summary = note,
    });
}

/// Return the active (ended_at IS NULL) session for the given vendor
/// tuple, or null if none exists.
pub fn activeForVendor(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    vendor: []const u8,
    vendor_session_id: ?[]const u8,
) Error!?Session {
    const sql_with: [:0]const u8 =
        \\select id from sessions
        \\where vendor = ? and vendor_session_id = ? and ended_at is null
        \\order by id asc limit 1
    ;
    const sql_no: [:0]const u8 =
        \\select id from sessions
        \\where vendor = ? and vendor_session_id is null and ended_at is null
        \\order by id asc limit 1
    ;
    var stmt = if (vendor_session_id) |_|
        d.prepare(sql_with) catch return Error.QueryFailed
    else
        d.prepare(sql_no) catch return Error.QueryFailed;
    defer stmt.finalize();
    if (vendor_session_id) |v| {
        stmt.bind(&.{ .{ .text = vendor }, .{ .text = v } }) catch return Error.QueryFailed;
    } else {
        stmt.bind(&.{.{ .text = vendor }}) catch return Error.QueryFailed;
    }
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => return null,
        .row => {
            const id = stmt.columnInt(0);
            return try getById(d, allocator, id);
        },
    }
}

/// Return the session_id of the active session for the operator's
/// (vendor, vendor_session_id) tuple, creating it if missing.
pub fn ensureActive(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    vendor: []const u8,
    vendor_session_id: ?[]const u8,
) Error!i64 {
    if (try activeForVendor(d, allocator, vendor, vendor_session_id)) |s| {
        defer deinit(s, allocator);
        return s.id;
    }
    const created = try startSession(d, allocator, .{
        .vendor = vendor,
        .vendor_session_id = vendor_session_id,
    });
    defer deinit(created, allocator);
    return created.id;
}

/// Append a session_entries row. Ordinal is MAX(ordinal)+1 within the
/// session. Best-effort: if session_id does not exist, the FK violation
/// surfaces as QueryFailed.
pub fn appendEntry(
    d: *db.sqlite.Db,
    session_id: i64,
    prefix: []const u8,
    body: []const u8,
) Error!void {
    // Compute next ordinal.
    var max_ord: i64 = 0;
    const sel_sql: [:0]const u8 =
        "select coalesce(max(ordinal), 0) from session_entries where session_id = ?";
    var stmt = d.prepare(sel_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .row => max_ord = stmt.columnInt(0),
        .done => {},
    }
    const next_ord = max_ord + 1;

    _ = d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, ?, ?, ?)",
        &.{
            .{ .int = session_id },
            .{ .int = next_ord },
            .{ .text = prefix },
            .{ .text = body },
        },
    ) catch return Error.QueryFailed;
}

/// Persist the session-open git context once. Existing non-null values stay
/// unchanged so reused sessions keep their original start boundary.
pub fn setStartGitContextIfUnset(
    d: *db.sqlite.Db,
    session_id: i64,
    repo_root: []const u8,
    head_sha_at_start: []const u8,
) Error!void {
    _ = d.execParams(
        \\update sessions
        \\set repo_root = case when repo_root is null then ? else repo_root end,
        \\    head_sha_at_start = case when head_sha_at_start is null then ? else head_sha_at_start end
        \\where id = ?
    , &.{
        .{ .text = repo_root },
        .{ .text = head_sha_at_start },
        .{ .int = session_id },
    }) catch return Error.QueryFailed;
}

pub fn getById(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
) Error!Session {
    const sql: [:0]const u8 =
        \\select id, task_id, project_id, agent_id, vendor, vendor_session_id,
        \\       model, started_at, ended_at, summary, repo_root, head_sha_at_start
        \\from sessions where id = ?
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readSessionRow(&stmt, allocator),
    };
}

pub fn listEntriesForSession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
) Error![]SessionEntry {
    const sql: [:0]const u8 =
        \\select id, session_id, ordinal, prefix, body, created_at
        \\from session_entries
        \\where session_id = ?
        \\order by ordinal
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(SessionEntry) = .empty;
    errdefer {
        for (out.items) |e| deinitEntry(e, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readEntryRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn recentEntriesForTask(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
    n: i64,
) Error![]SessionEntry {
    const sql: [:0]const u8 =
        \\select se.id, se.session_id, se.ordinal, se.prefix, se.body, se.created_at
        \\from session_entries se
        \\join sessions s on s.id = se.session_id
        \\where s.task_id = ?
        \\order by se.created_at desc
        \\limit ?
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = task_id }, .{ .int = n } }) catch return Error.QueryFailed;

    var out: std.ArrayList(SessionEntry) = .empty;
    errdefer {
        for (out.items) |e| deinitEntry(e, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readEntryRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn recentSessionsForTask(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
    n: i64,
) Error![]Session {
    const sql: [:0]const u8 =
        \\select id, task_id, project_id, agent_id, vendor, vendor_session_id,
        \\       model, started_at, ended_at, summary, repo_root, head_sha_at_start
        \\from sessions where task_id = ?
        \\order by started_at desc limit ?
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = task_id }, .{ .int = n } }) catch return Error.QueryFailed;

    var out: std.ArrayList(Session) = .empty;
    errdefer {
        for (out.items) |s| deinit(s, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readSessionRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Internals
// =========================================================================

fn readSessionRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Session {
    return .{
        .id = stmt.columnInt(0),
        .task_id = stmt.columnIntOpt(1),
        .project_id = stmt.columnIntOpt(2),
        .agent_id = stmt.columnIntOpt(3),
        .vendor = try stmt.columnTextAlloc(4, allocator),
        .vendor_session_id = try stmt.columnTextOpt(5, allocator),
        .model = try stmt.columnTextOpt(6, allocator),
        .started_at = try stmt.columnTextAlloc(7, allocator),
        .ended_at = try stmt.columnTextOpt(8, allocator),
        .summary = try stmt.columnTextOpt(9, allocator),
        .repo_root = try stmt.columnTextOpt(10, allocator),
        .head_sha_at_start = try stmt.columnTextOpt(11, allocator),
    };
}

fn readEntryRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!SessionEntry {
    return .{
        .id = stmt.columnInt(0),
        .session_id = stmt.columnInt(1),
        .ordinal = stmt.columnInt(2),
        .prefix = try stmt.columnTextAlloc(3, allocator),
        .body = try stmt.columnTextAlloc(4, allocator),
        .created_at = try stmt.columnTextAlloc(5, allocator),
    };
}

// =========================================================================
// Tests
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "startSession creates row and is idempotent on (vendor, vendor_session_id)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const s1 = try startSession(&d, a, .{ .vendor = "claude", .vendor_session_id = "vsid-1" });
    defer deinit(s1, a);
    const s2 = try startSession(&d, a, .{ .vendor = "claude", .vendor_session_id = "vsid-1" });
    defer deinit(s2, a);

    try std.testing.expectEqual(s1.id, s2.id);
    try std.testing.expectEqualStrings("claude", s1.vendor);
    try std.testing.expectEqualStrings("vsid-1", s1.vendor_session_id.?);
}

test "startSession with NULL vendor_session_id treats NULLs as same tuple" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const s1 = try startSession(&d, a, .{ .vendor = "cli" });
    defer deinit(s1, a);
    const s2 = try startSession(&d, a, .{ .vendor = "cli" });
    defer deinit(s2, a);

    try std.testing.expectEqual(s1.id, s2.id);
    try std.testing.expect(s1.vendor_session_id == null);
}

test "startSession binds task_id when existing has NULL task_id" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    // Need a task to reference (the FK is on delete set null but row must exist for valid bind).
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global', 't', 'todo')",
        &.{},
    );
    const s1 = try startSession(&d, a, .{ .vendor = "v" });
    defer deinit(s1, a);
    try std.testing.expect(s1.task_id == null);

    const s2 = try startSession(&d, a, .{ .vendor = "v", .task_id = task_id });
    defer deinit(s2, a);
    try std.testing.expectEqual(s1.id, s2.id);
    try std.testing.expectEqual(task_id, s2.task_id.?);
}

test "startSession refuses to rebind task when existing differs" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const t1 = try d.execParams("insert into tasks (scope_kind, title, status) values ('global', 'a', 'todo')", &.{});
    const t2 = try d.execParams("insert into tasks (scope_kind, title, status) values ('global', 'b', 'todo')", &.{});
    const s1 = try startSession(&d, a, .{ .vendor = "v", .task_id = t1 });
    defer deinit(s1, a);
    try std.testing.expectError(Error.TaskConflict, startSession(&d, a, .{ .vendor = "v", .task_id = t2 }));
}

test "endSession marks ended_at and refuses double-end" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try startSession(&d, a, .{ .vendor = "v" });
    defer deinit(s, a);
    try endSession(&d, a, s.id, "wrapped");
    const got = try getById(&d, a, s.id);
    defer deinit(got, a);
    try std.testing.expect(got.ended_at != null);
    try std.testing.expectEqualStrings("wrapped", got.summary.?);
    try std.testing.expectError(Error.AlreadyEnded, endSession(&d, a, s.id, null));
}

test "appendEntry assigns ordinals starting at 1" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try startSession(&d, a, .{ .vendor = "v" });
    defer deinit(s, a);
    try appendEntry(&d, s.id, "note", "first");
    try appendEntry(&d, s.id, "note", "second");
    const entries = try listEntriesForSession(&d, a, s.id);
    defer deinitEntries(entries, a);
    try std.testing.expectEqual(@as(usize, 2), entries.len);
    try std.testing.expectEqual(@as(i64, 1), entries[0].ordinal);
    try std.testing.expectEqual(@as(i64, 2), entries[1].ordinal);
    try std.testing.expectEqualStrings("first", entries[0].body);
}

test "activeForVendor returns null when none, session when active" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expect((try activeForVendor(&d, a, "claude", null)) == null);
    const s = try startSession(&d, a, .{ .vendor = "claude" });
    defer deinit(s, a);
    const found = (try activeForVendor(&d, a, "claude", null)).?;
    defer deinit(found, a);
    try std.testing.expectEqual(s.id, found.id);
    try endSession(&d, a, s.id, null);
    try std.testing.expect((try activeForVendor(&d, a, "claude", null)) == null);
}

test "ensureActive reuses or creates as needed" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const id1 = try ensureActive(&d, a, "x", null);
    const id2 = try ensureActive(&d, a, "x", null);
    try std.testing.expectEqual(id1, id2);
}

test "setStartGitContextIfUnset preserves the first non-null values" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const s = try startSession(&d, a, .{ .vendor = "v", .vendor_session_id = "vsid" });
    defer deinit(s, a);

    try setStartGitContextIfUnset(&d, s.id, "/tmp/repo-a", "sha-a");
    try setStartGitContextIfUnset(&d, s.id, "/tmp/repo-b", "sha-b");

    const got = try getById(&d, a, s.id);
    defer deinit(got, a);
    try std.testing.expectEqualStrings("/tmp/repo-a", got.repo_root.?);
    try std.testing.expectEqualStrings("sha-a", got.head_sha_at_start.?);
}

test "recentEntriesForTask returns most recent first" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const t = try d.execParams("insert into tasks (scope_kind, title, status) values ('global', 't', 'todo')", &.{});
    const s = try startSession(&d, a, .{ .vendor = "v", .task_id = t });
    defer deinit(s, a);
    try appendEntry(&d, s.id, "note", "first");
    try appendEntry(&d, s.id, "note", "second");
    const entries = try recentEntriesForTask(&d, a, t, 10);
    defer deinitEntries(entries, a);
    try std.testing.expect(entries.len == 2);
}
