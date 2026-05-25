//! engine/external/sync - link-level pull/push/resolve orchestration.

const std = @import("std");
const db = @import("db");
const extsync = @import("../extsync/common.zig");
const external_link = @import("link.zig");

pub const Outcome = enum {
    ok,
    conflict,
    noop,
    @"error",

    fn toEventText(self: Outcome) []const u8 {
        return switch (self) {
            .ok => "ok",
            .conflict => "conflict",
            .noop => "noop",
            .@"error" => "error",
        };
    }
};

pub const ResolveKeep = enum { local, remote };

pub const PullResult = struct {
    link_id: i64,
    outcome: Outcome,
    fields_changed: []const []const u8 = &.{},
    detail: []const u8 = "",
};

pub const PushResult = struct {
    link_id: i64,
    outcome: Outcome,
    fields_changed: []const []const u8 = &.{},
    detail: []const u8 = "",
};

pub const ResolveResult = struct {
    ok: bool,
    event_id: i64,
    new_event_id: i64,
    keep: ResolveKeep,
};

pub const StatusRow = struct {
    link_id: i64,
    entity_kind: external_link.ExternalEntityKind,
    entity_id: i64,
    external_id: []const u8,
    system_id: i64,
    last_synced_at: ?[]const u8,
    last_sync_status: external_link.SyncStatus,
};

pub const Error = error{
    NotFound,
    QueryFailed,
    ReadOnly,
    UnsupportedEntityKind,
    InvalidKeep,
    NotConflict,
    AdapterFailed,
    LinkExists,
} || std.mem.Allocator.Error;

pub fn deinitPullResult(result: PullResult, allocator: std.mem.Allocator) void {
    if (result.fields_changed.len > 0) allocator.free(result.fields_changed);
    if (result.detail.len > 0) allocator.free(result.detail);
}

pub fn deinitPushResult(result: PushResult, allocator: std.mem.Allocator) void {
    if (result.fields_changed.len > 0) allocator.free(result.fields_changed);
    if (result.detail.len > 0) allocator.free(result.detail);
}

pub fn deinitStatusRows(rows: []const StatusRow, allocator: std.mem.Allocator) void {
    for (rows) |row| {
        allocator.free(row.external_id);
        if (row.last_synced_at) |ts| allocator.free(ts);
    }
    allocator.free(rows);
}

/// One row in the sync_events log surfaced for `audit trail --link`.
pub const SyncEvent = struct {
    id: i64,
    direction: []const u8,
    outcome: []const u8,
    fields_changed: ?[]const u8,
    detail: ?[]const u8,
    at: []const u8,
};

pub fn deinitSyncEvents(events: []const SyncEvent, allocator: std.mem.Allocator) void {
    for (events) |e| {
        allocator.free(e.direction);
        allocator.free(e.outcome);
        if (e.fields_changed) |s| allocator.free(s);
        if (e.detail) |s| allocator.free(s);
        allocator.free(e.at);
    }
    allocator.free(events);
}

/// All sync_events for a given link, oldest first. Mirrors Go's
/// `external.EventsForLink` and feeds `audit trail --link <id>`.
pub fn eventsForLink(d: *db.sqlite.Db, allocator: std.mem.Allocator, link_id: i64) Error![]SyncEvent {
    var stmt = d.prepare(
        \\select id, direction, outcome, fields_changed, detail, at
        \\from sync_events
        \\where link_id = ?
        \\order by id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = link_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(SyncEvent) = .empty;
    errdefer {
        for (out.items) |e| {
            allocator.free(e.direction);
            allocator.free(e.outcome);
            if (e.fields_changed) |s| allocator.free(s);
            if (e.detail) |s| allocator.free(s);
            allocator.free(e.at);
        }
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const e = SyncEvent{
                    .id = stmt.columnInt(0),
                    .direction = try stmt.columnTextAlloc(1, allocator),
                    .outcome = try stmt.columnTextAlloc(2, allocator),
                    .fields_changed = try stmt.columnTextOpt(3, allocator),
                    .detail = try stmt.columnTextOpt(4, allocator),
                    .at = try stmt.columnTextAlloc(5, allocator),
                };
                try out.append(allocator, e);
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn pullLink(d: *db.sqlite.Db, allocator: std.mem.Allocator, link: external_link.ExtLink, adapter: anytype) Error!PullResult {
    const Adapter = @TypeOf(adapter.*);
    const remote = extsync.dispatch(Adapter, .pull, adapter, extsync.PullArgs{
        .allocator = allocator,
        .external_id = link.external_id,
    }) catch |e| {
        const detail = try allocator.dupe(u8, @errorName(e));
        _ = bestEffortErrorWrite(d, allocator, link.id, "pull", detail);
        return .{
            .link_id = link.id,
            .outcome = .@"error",
            .detail = detail,
        };
    };
    defer extsync.deinitRemoteState(remote, allocator);

    d.exec("begin immediate") catch return Error.QueryFailed;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    var result = PullResult{ .link_id = link.id, .outcome = .noop };
    switch (link.sync_direction) {
        .@"read-only", .@"write-back" => {
            result.outcome = .noop;
        },
        .@"two-way" => {
            const changed = try applyRemoteToLocal(d, allocator, link, remote, true, true);
            if (changed.len > 0) {
                result.outcome = .ok;
                result.fields_changed = changed;
            } else {
                result.outcome = .noop;
            }
        },
    }

    const status_to_store = outcomeToLinkStatus(result.outcome);
    external_link.updateSyncState(d, link.id, status_to_store) catch |e| switch (e) {
        error.NotFound => return Error.NotFound,
        else => return Error.QueryFailed,
    };

    const changed_json = marshalFieldsChanged(allocator, result.fields_changed) catch return Error.QueryFailed;
    defer if (changed_json) |s| allocator.free(s);
    _ = try insertSyncEvent(d, link.id, "pull", result.outcome.toEventText(), changed_json, if (result.detail.len == 0) null else result.detail);

    d.exec("commit") catch return Error.QueryFailed;
    committed = true;
    return result;
}

pub fn pushLink(d: *db.sqlite.Db, allocator: std.mem.Allocator, link: external_link.ExtLink, adapter: anytype) Error!PushResult {
    if (link.sync_direction == .@"read-only") return Error.ReadOnly;

    const fields = try localEntityFields(d, allocator, link.entity_kind, link.entity_id);
    defer deinitEntityFields(fields, allocator);

    const Adapter = @TypeOf(adapter.*);
    const update = extsync.dispatch(Adapter, .push, adapter, extsync.PushArgs{
        .allocator = allocator,
        .external_id = link.external_id,
        .fields = .{
            .title = fields.title,
            .status = fields.status,
        },
    }) catch |e| {
        const detail = try allocator.dupe(u8, @errorName(e));
        _ = bestEffortErrorWrite(d, allocator, link.id, "push", detail);
        return .{
            .link_id = link.id,
            .outcome = .@"error",
            .detail = detail,
        };
    };
    defer extsync.deinitUpdateOutcome(update, allocator);

    d.exec("begin immediate") catch return Error.QueryFailed;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    external_link.updateSyncState(d, link.id, .ok) catch |e| switch (e) {
        error.NotFound => return Error.NotFound,
        else => return Error.QueryFailed,
    };
    const changed_json = marshalFieldsChanged(allocator, update.fields_applied) catch return Error.QueryFailed;
    defer if (changed_json) |s| allocator.free(s);
    _ = try insertSyncEvent(d, link.id, "push", "ok", changed_json, null);

    d.exec("commit") catch return Error.QueryFailed;
    committed = true;

    return .{
        .link_id = link.id,
        .outcome = .ok,
        .fields_changed = if (update.fields_applied.len == 0) &.{} else try copyFieldNames(allocator, update.fields_applied),
    };
}

pub fn resolveConflict(d: *db.sqlite.Db, allocator: std.mem.Allocator, event_id: i64, keep: ResolveKeep, adapter: anytype) Error!ResolveResult {
    const link_id, const outcome = try loadSyncEventLinkOutcome(d, allocator, event_id);
    defer allocator.free(outcome);
    if (!std.mem.eql(u8, outcome, "conflict")) return Error.NotConflict;

    const link = external_link.show(d, allocator, link_id) catch |e| switch (e) {
        error.NotFound => return Error.NotFound,
        else => return Error.QueryFailed,
    };
    defer external_link.deinit(link, allocator);

    const direction: []const u8 = switch (keep) {
        .local => "push",
        .remote => "pull",
    };
    var pulled_remote: ?extsync.RemoteState = null;
    defer if (pulled_remote) |remote| extsync.deinitRemoteState(remote, allocator);

    if (keep == .local) {
        const fields = try localEntityFields(d, allocator, link.entity_kind, link.entity_id);
        defer deinitEntityFields(fields, allocator);
        const Adapter = @TypeOf(adapter.*);
        const pushed = extsync.dispatch(Adapter, .push, adapter, extsync.PushArgs{
            .allocator = allocator,
            .external_id = link.external_id,
            .fields = .{ .title = fields.title, .status = fields.status },
        }) catch return Error.AdapterFailed;
        defer extsync.deinitUpdateOutcome(pushed, allocator);
    } else {
        const Adapter = @TypeOf(adapter.*);
        const remote = extsync.dispatch(Adapter, .pull, adapter, extsync.PullArgs{
            .allocator = allocator,
            .external_id = link.external_id,
        }) catch return Error.AdapterFailed;
        pulled_remote = remote;
    }

    d.exec("begin immediate") catch return Error.QueryFailed;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    if (keep == .remote and pulled_remote != null) {
        const changed = try applyRemoteToLocal(d, allocator, link, pulled_remote.?, true, false);
        if (changed.len > 0) allocator.free(changed);
    }

    external_link.updateSyncState(d, link.id, .ok) catch |e| switch (e) {
        error.NotFound => return Error.NotFound,
        else => return Error.QueryFailed,
    };
    const detail = try std.fmt.allocPrint(allocator, "resolved={s}; from sync_event={d}", .{ @tagName(keep), event_id });
    defer allocator.free(detail);
    const new_event_id = try insertSyncEvent(d, link.id, direction, "ok", null, detail);

    d.exec("commit") catch return Error.QueryFailed;
    committed = true;

    return .{
        .ok = true,
        .event_id = event_id,
        .new_event_id = new_event_id,
        .keep = keep,
    };
}

pub fn status(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: external_link.ListFilter) Error![]StatusRow {
    const links = external_link.list(d, allocator, filter) catch return Error.QueryFailed;
    defer external_link.deinitMany(links, allocator);

    var out: std.ArrayList(StatusRow) = .empty;
    errdefer {
        for (out.items) |row| {
            allocator.free(row.external_id);
            if (row.last_synced_at) |ts| allocator.free(ts);
        }
        out.deinit(allocator);
    }
    for (links) |link| {
        try out.append(allocator, .{
            .link_id = link.id,
            .entity_kind = link.entity_kind,
            .entity_id = link.entity_id,
            .external_id = try allocator.dupe(u8, link.external_id),
            .system_id = link.system_id,
            .last_synced_at = if (link.last_synced_at) |ts| try allocator.dupe(u8, ts) else null,
            .last_sync_status = link.last_sync_status,
        });
    }
    return try out.toOwnedSlice(allocator);
}

fn loadSyncEventLinkOutcome(d: *db.sqlite.Db, allocator: std.mem.Allocator, event_id: i64) Error!struct { i64, []const u8 } {
    var stmt = d.prepare("select link_id, outcome from sync_events where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = event_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => blk: {
            if (stmt.columnIsNull(0)) return Error.NotFound;
            break :blk .{
                stmt.columnInt(0),
                try stmt.columnTextAlloc(1, allocator),
            };
        },
    };
}

const EntityFields = struct {
    title: []const u8,
    status: []const u8,
};

fn deinitEntityFields(fields: EntityFields, allocator: std.mem.Allocator) void {
    allocator.free(fields.title);
    allocator.free(fields.status);
}

fn localEntityFields(d: *db.sqlite.Db, allocator: std.mem.Allocator, kind: external_link.ExternalEntityKind, entity_id: i64) Error!EntityFields {
    const sql: [:0]const u8 = switch (kind) {
        .task => "select coalesce(title,''), coalesce(status,'') from tasks where id = ?",
        .plan => "select coalesce(title,''), coalesce(status,'') from plans where id = ?",
        .question => "select coalesce(title,''), coalesce(status,'') from questions where id = ?",
        .artifact => "select coalesce(title,''), coalesce(status,'') from artifacts where id = ?",
        else => return Error.UnsupportedEntityKind,
    };
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = entity_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => .{
            .title = try stmt.columnTextAlloc(0, allocator),
            .status = try stmt.columnTextAlloc(1, allocator),
        },
    };
}

fn applyRemoteToLocal(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    link: external_link.ExtLink,
    remote: extsync.RemoteState,
    skip_empty_remote_status: bool,
    allow_missing_local: bool,
) Error![]const []const u8 {
    const local = localEntityFields(d, allocator, link.entity_kind, link.entity_id) catch |e| switch (e) {
        Error.NotFound => if (allow_missing_local) return &.{} else return Error.NotFound,
        Error.UnsupportedEntityKind => return &.{},
        else => return e,
    };
    defer deinitEntityFields(local, allocator);

    const title_changed = remote.title.len > 0 and !std.mem.eql(u8, local.title, remote.title);
    const status_changed = if (skip_empty_remote_status) (remote.status.len > 0 and !std.mem.eql(u8, local.status, remote.status)) else (!std.mem.eql(u8, local.status, remote.status));
    if (!title_changed and !status_changed) return &.{};

    const table: []const u8 = switch (link.entity_kind) {
        .task => "tasks",
        .plan => "plans",
        .question => "questions",
        .artifact => "artifacts",
        else => return &.{},
    };

    if (title_changed and status_changed) {
        const sql = try std.fmt.allocPrint(allocator, "update {s} set title = ?, status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?", .{table});
        defer allocator.free(sql);
        const sql_z = try allocator.dupeZ(u8, sql);
        defer allocator.free(sql_z);
        _ = d.execParams(sql_z, &.{ .{ .text = remote.title }, .{ .text = remote.status }, .{ .int = link.entity_id } }) catch return Error.QueryFailed;
    } else if (title_changed) {
        const sql = try std.fmt.allocPrint(allocator, "update {s} set title = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?", .{table});
        defer allocator.free(sql);
        const sql_z = try allocator.dupeZ(u8, sql);
        defer allocator.free(sql_z);
        _ = d.execParams(sql_z, &.{ .{ .text = remote.title }, .{ .int = link.entity_id } }) catch return Error.QueryFailed;
    } else {
        const sql = try std.fmt.allocPrint(allocator, "update {s} set status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?", .{table});
        defer allocator.free(sql);
        const sql_z = try allocator.dupeZ(u8, sql);
        defer allocator.free(sql_z);
        _ = d.execParams(sql_z, &.{ .{ .text = remote.status }, .{ .int = link.entity_id } }) catch return Error.QueryFailed;
    }

    var changed: std.ArrayList([]const u8) = .empty;
    errdefer changed.deinit(allocator);
    if (title_changed) try changed.append(allocator, "title");
    if (status_changed) try changed.append(allocator, "status");
    return try changed.toOwnedSlice(allocator);
}

fn outcomeToLinkStatus(outcome: Outcome) external_link.SyncStatus {
    return switch (outcome) {
        .ok, .noop => .ok,
        .conflict => .conflict,
        .@"error" => .@"error",
    };
}

fn marshalFieldsChanged(allocator: std.mem.Allocator, fields: []const []const u8) !?[]const u8 {
    if (fields.len == 0) return null;
    return try extsync.jsonStringifyAlloc(allocator, fields);
}

fn copyFieldNames(allocator: std.mem.Allocator, fields: []const []const u8) ![]const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    defer out.deinit(allocator);
    for (fields) |field| try out.append(allocator, field);
    return try out.toOwnedSlice(allocator);
}

fn insertSyncEvent(
    d: *db.sqlite.Db,
    link_id: i64,
    direction: []const u8,
    outcome: []const u8,
    fields_changed_json: ?[]const u8,
    detail: ?[]const u8,
) Error!i64 {
    return d.execParams(
        \\insert into sync_events (link_id, direction, outcome, fields_changed, detail)
        \\values (?, ?, ?, ?, ?)
    , &.{
        .{ .int = link_id },
        .{ .text = direction },
        .{ .text = outcome },
        if (fields_changed_json) |s| .{ .text = s } else .{ .null = {} },
        if (detail) |s| .{ .text = s } else .{ .null = {} },
    }) catch return Error.QueryFailed;
}

fn bestEffortErrorWrite(d: *db.sqlite.Db, allocator: std.mem.Allocator, link_id: i64, direction: []const u8, detail: []const u8) void {
    external_link.updateSyncState(d, link_id, .@"error") catch return;
    _ = insertSyncEvent(d, link_id, direction, "error", null, detail) catch {};
    _ = allocator;
}

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var dn = try db.sqlite.Db.openMemory();
    errdefer dn.close();
    try db.migrate.applyAll(&dn, allocator);
    return dn;
}

fn mustInsertTask(d: *db.sqlite.Db, title: []const u8, status_text: []const u8) !i64 {
    return try d.execParams(
        \\insert into tasks (scope_kind, scope_id, title, status, priority)
        \\values ('global', null, ?, ?, 100)
    , &.{ .{ .text = title }, .{ .text = status_text } });
}

fn mustInsertSystem(d: *db.sqlite.Db, slug: []const u8) !i64 {
    return try d.execParams(
        \\insert into external_systems (kind, slug, base_url, auth_method, auth_ref)
        \\values ('jira', ?, 'https://example.atlassian.net', 'token-env', 'JIRA_TOKEN')
    , &.{.{ .text = slug }});
}

const FakeAdapter = struct {
    remote_title: []const u8,
    remote_status: []const u8,
    pushes: usize = 0,

    pub fn pull(self: *const @This(), allocator: std.mem.Allocator, external_id: []const u8) !extsync.RemoteState {
        return .{
            .external_id = try allocator.dupe(u8, external_id),
            .title = try allocator.dupe(u8, self.remote_title),
            .body = try allocator.dupe(u8, ""),
            .status = try allocator.dupe(u8, self.remote_status),
            .assignee = try allocator.dupe(u8, ""),
            .priority = 0,
            .due_at = try allocator.dupe(u8, ""),
            .url = try allocator.dupe(u8, ""),
            .raw_status = try allocator.dupe(u8, ""),
        };
    }

    pub fn push(self: *FakeAdapter, allocator: std.mem.Allocator, _: []const u8, fields: extsync.FieldChangeSet) !extsync.UpdateOutcome {
        self.pushes += 1;
        var applied: std.ArrayList([]const u8) = .empty;
        defer applied.deinit(allocator);
        if (fields.title != null) try applied.append(allocator, "title");
        if (fields.status != null) try applied.append(allocator, "status");
        return .{ .fields_applied = try applied.toOwnedSlice(allocator) };
    }

    pub fn validate(_: *const @This(), _: []const u8) !void {}

    pub fn render(_: *const @This(), allocator: std.mem.Allocator, _: extsync.LocalEntity, _: extsync.CreateOptions) ![]const u8 {
        return try allocator.dupe(u8, "{}");
    }
};

test "pull two-way updates local mapped fields and writes sync_event" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try mustInsertTask(&d, "Original", "todo");
    const system_id = try mustInsertSystem(&d, "sys-pull");
    const link = try external_link.create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "PROJ-1",
        .sync_direction = .@"two-way",
    });
    defer external_link.deinit(link, a);

    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "doing" };
    const result = try pullLink(&d, a, link, &adapter);
    defer deinitPullResult(result, a);
    try std.testing.expectEqual(Outcome.ok, result.outcome);
    try std.testing.expectEqual(@as(usize, 2), result.fields_changed.len);

    var stmt = try d.prepare("select title, status from tasks where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    switch (try stmt.step()) {
        .done => return error.TestUnexpectedResult,
        .row => {
            const title = try stmt.columnTextAlloc(0, a);
            defer a.free(title);
            const status_text = try stmt.columnTextAlloc(1, a);
            defer a.free(status_text);
            try std.testing.expectEqualStrings("Remote title", title);
            try std.testing.expectEqualStrings("doing", status_text);
        },
    }

    const event_count = try d.intQuery("select count(*) from sync_events where direction = 'pull' and outcome = 'ok'");
    try std.testing.expectEqual(@as(i64, 1), event_count);
}

test "pull read-only is noop and leaves local entity unchanged" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try mustInsertTask(&d, "Keep", "todo");
    const system_id = try mustInsertSystem(&d, "sys-readonly");
    const link = try external_link.create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "PROJ-2",
        .sync_direction = .@"read-only",
    });
    defer external_link.deinit(link, a);

    var adapter = FakeAdapter{ .remote_title = "Remote", .remote_status = "doing" };
    const result = try pullLink(&d, a, link, &adapter);
    defer deinitPullResult(result, a);
    try std.testing.expectEqual(Outcome.noop, result.outcome);

    var stmt = try d.prepare("select title from tasks where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    switch (try stmt.step()) {
        .done => return error.TestUnexpectedResult,
        .row => {
            const title = try stmt.columnTextAlloc(0, a);
            defer a.free(title);
            try std.testing.expectEqualStrings("Keep", title);
        },
    }
}

test "push read-only returns ReadOnly; write-back writes event" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try mustInsertTask(&d, "Push Task", "todo");
    const system_id = try mustInsertSystem(&d, "sys-push");
    const ro_link = try external_link.create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "PROJ-3",
        .sync_direction = .@"read-only",
    });
    defer external_link.deinit(ro_link, a);

    var adapter = FakeAdapter{ .remote_title = "", .remote_status = "" };
    try std.testing.expectError(Error.ReadOnly, pushLink(&d, a, ro_link, &adapter));

    const wb_link = try external_link.create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "PROJ-4",
        .sync_direction = .@"write-back",
    });
    defer external_link.deinit(wb_link, a);

    const result = try pushLink(&d, a, wb_link, &adapter);
    defer deinitPushResult(result, a);
    try std.testing.expectEqual(Outcome.ok, result.outcome);
    try std.testing.expectEqual(@as(usize, 1), adapter.pushes);
}

test "resolveConflict keep local writes resolution event and sets link status ok" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try mustInsertTask(&d, "Conflict Task", "todo");
    const system_id = try mustInsertSystem(&d, "sys-resolve");
    const link = try external_link.create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "PROJ-5",
        .sync_direction = .@"two-way",
    });
    defer external_link.deinit(link, a);

    const event_id = try d.execParams(
        \\insert into sync_events (link_id, direction, outcome, detail)
        \\values (?, 'pull', 'conflict', 'status conflict')
    , &.{.{ .int = link.id }});

    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "doing" };
    const resolved = try resolveConflict(&d, a, event_id, .local, &adapter);
    try std.testing.expect(resolved.ok);
    try std.testing.expect(resolved.new_event_id > event_id);

    const refreshed = try external_link.show(&d, a, link.id);
    defer external_link.deinit(refreshed, a);
    try std.testing.expectEqual(external_link.SyncStatus.ok, refreshed.last_sync_status);
}

test "pull two-way with missing local entity is noop and still records sync event" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const system_id = try mustInsertSystem(&d, "sys-missing");
    const link = try external_link.create(&d, a, .{
        .entity_kind = .task,
        .entity_id = 999_999,
        .system_id = system_id,
        .external_id = "PROJ-6",
        .sync_direction = .@"two-way",
    });
    defer external_link.deinit(link, a);

    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "doing" };
    const result = try pullLink(&d, a, link, &adapter);
    defer deinitPullResult(result, a);
    try std.testing.expectEqual(Outcome.noop, result.outcome);

    var stmt = try d.prepare("select count(*) from sync_events where link_id = ? and direction = 'pull' and outcome = 'noop'");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = link.id }});
    switch (try stmt.step()) {
        .done => return error.TestUnexpectedResult,
        .row => try std.testing.expectEqual(@as(i64, 1), stmt.columnInt(0)),
    }
}

test "resolveConflict keep remote skips empty remote status while applying non-empty title" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try mustInsertTask(&d, "Local title", "doing");
    const system_id = try mustInsertSystem(&d, "sys-resolve-remote");
    const link = try external_link.create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "PROJ-7",
        .sync_direction = .@"two-way",
    });
    defer external_link.deinit(link, a);

    const event_id = try d.execParams(
        \\insert into sync_events (link_id, direction, outcome, detail)
        \\values (?, 'pull', 'conflict', 'status conflict')
    , &.{.{ .int = link.id }});

    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "" };
    _ = try resolveConflict(&d, a, event_id, .remote, &adapter);

    var stmt = try d.prepare("select title, status from tasks where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    switch (try stmt.step()) {
        .done => return error.TestUnexpectedResult,
        .row => {
            const title = try stmt.columnTextAlloc(0, a);
            defer a.free(title);
            const status_text = try stmt.columnTextAlloc(1, a);
            defer a.free(status_text);
            try std.testing.expectEqualStrings("Remote title", title);
            try std.testing.expectEqualStrings("doing", status_text);
        },
    }
}

test "resolveConflict keep remote fails for missing local entity and writes no resolution event" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const system_id = try mustInsertSystem(&d, "sys-resolve-remote-missing");
    const link = try external_link.create(&d, a, .{
        .entity_kind = .task,
        .entity_id = 424_242,
        .system_id = system_id,
        .external_id = "PROJ-8",
        .sync_direction = .@"two-way",
    });
    defer external_link.deinit(link, a);

    const event_id = try d.execParams(
        \\insert into sync_events (link_id, direction, outcome, detail)
        \\values (?, 'pull', 'conflict', 'status conflict')
    , &.{.{ .int = link.id }});

    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "doing" };
    try std.testing.expectError(Error.NotFound, resolveConflict(&d, a, event_id, .remote, &adapter));

    const refreshed = try external_link.show(&d, a, link.id);
    defer external_link.deinit(refreshed, a);
    try std.testing.expectEqual(external_link.SyncStatus.never, refreshed.last_sync_status);

    var stmt = try d.prepare("select count(*) from sync_events where link_id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = link.id }});
    switch (try stmt.step()) {
        .done => return error.TestUnexpectedResult,
        .row => try std.testing.expectEqual(@as(i64, 1), stmt.columnInt(0)),
    }
}

test "eventsForLink returns recorded sync_events oldest-first" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try mustInsertTask(&d, "Event Task", "todo");
    const system_id = try mustInsertSystem(&d, "sys-events");
    const link = try external_link.create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "EV-1",
        .sync_direction = .@"two-way",
    });
    defer external_link.deinit(link, a);

    _ = try d.execParams(
        \\insert into sync_events (link_id, direction, outcome, fields_changed, detail)
        \\values (?, 'pull', 'ok', '["title"]', 'first')
    , &.{.{ .int = link.id }});
    _ = try d.execParams(
        \\insert into sync_events (link_id, direction, outcome, detail)
        \\values (?, 'push', 'conflict', 'second')
    , &.{.{ .int = link.id }});

    const events = try eventsForLink(&d, a, link.id);
    defer deinitSyncEvents(events, a);
    try std.testing.expectEqual(@as(usize, 2), events.len);
    try std.testing.expectEqualStrings("pull", events[0].direction);
    try std.testing.expectEqualStrings("ok", events[0].outcome);
    try std.testing.expect(events[0].fields_changed != null);
    try std.testing.expectEqualStrings("[\"title\"]", events[0].fields_changed.?);
    try std.testing.expectEqualStrings("push", events[1].direction);
    try std.testing.expectEqualStrings("conflict", events[1].outcome);
    try std.testing.expect(events[1].detail != null);
    try std.testing.expectEqualStrings("second", events[1].detail.?);
}
