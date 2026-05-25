//! engine/external/link — local-entity ↔ external-ticket links.
//!
//! Mirrors Go `internal/external/external` link semantics:
//! - enum sets match migration CHECK constraints
//! - create defaults: link_role=mirror, sync_direction=two-way, status=never
//! - list helpers for entity/system filters and pull/push candidate scans
//! - sync state updates + sync_direction update audit row

const std = @import("std");
const db = @import("db");

pub const ExternalEntityKind = enum {
    plan,
    task,
    question,
    test_scenario,
    artifact,
    decision,
    session,

    pub fn fromText(s: []const u8) ?ExternalEntityKind {
        if (std.mem.eql(u8, s, "plan")) return .plan;
        if (std.mem.eql(u8, s, "task")) return .task;
        if (std.mem.eql(u8, s, "question")) return .question;
        if (std.mem.eql(u8, s, "test_scenario")) return .test_scenario;
        if (std.mem.eql(u8, s, "artifact")) return .artifact;
        if (std.mem.eql(u8, s, "decision")) return .decision;
        if (std.mem.eql(u8, s, "session")) return .session;
        return null;
    }
};

pub const LinkRole = enum {
    mirror,
    parent,
    child,
    reference,

    pub fn fromText(s: []const u8) ?LinkRole {
        if (std.mem.eql(u8, s, "mirror")) return .mirror;
        if (std.mem.eql(u8, s, "parent")) return .parent;
        if (std.mem.eql(u8, s, "child")) return .child;
        if (std.mem.eql(u8, s, "reference")) return .reference;
        return null;
    }
};

pub const SyncDirection = enum {
    @"read-only",
    @"write-back",
    @"two-way",

    pub fn fromText(s: []const u8) ?SyncDirection {
        if (std.mem.eql(u8, s, "read-only")) return .@"read-only";
        if (std.mem.eql(u8, s, "write-back")) return .@"write-back";
        if (std.mem.eql(u8, s, "two-way")) return .@"two-way";
        return null;
    }

    pub fn toText(self: SyncDirection) []const u8 {
        return switch (self) {
            .@"read-only" => "read-only",
            .@"write-back" => "write-back",
            .@"two-way" => "two-way",
        };
    }
};

pub const SyncStatus = enum {
    ok,
    conflict,
    @"error",
    never,

    pub fn fromText(s: []const u8) ?SyncStatus {
        if (std.mem.eql(u8, s, "ok")) return .ok;
        if (std.mem.eql(u8, s, "conflict")) return .conflict;
        if (std.mem.eql(u8, s, "error")) return .@"error";
        if (std.mem.eql(u8, s, "never")) return .never;
        return null;
    }

    pub fn toText(self: SyncStatus) []const u8 {
        return switch (self) {
            .ok => "ok",
            .conflict => "conflict",
            .@"error" => "error",
            .never => "never",
        };
    }
};

pub const ExtLink = struct {
    id: i64,
    entity_kind: ExternalEntityKind,
    entity_id: i64,
    system_id: i64,
    external_id: []const u8,
    external_url: ?[]const u8,
    link_role: LinkRole,
    sync_direction: SyncDirection,
    last_synced_at: ?[]const u8,
    last_sync_status: SyncStatus,
    config_json: ?[]const u8,
    created_at: []const u8,
};

pub fn deinit(item: ExtLink, allocator: std.mem.Allocator) void {
    allocator.free(item.external_id);
    if (item.external_url) |s| allocator.free(s);
    if (item.last_synced_at) |s| allocator.free(s);
    if (item.config_json) |s| allocator.free(s);
    allocator.free(item.created_at);
}

pub fn deinitMany(items: []const ExtLink, allocator: std.mem.Allocator) void {
    for (items) |item| deinit(item, allocator);
    allocator.free(items);
}

pub const CreateArgs = struct {
    entity_kind: ExternalEntityKind,
    entity_id: i64,
    system_id: i64,
    external_id: []const u8,
    external_url: ?[]const u8 = null,
    link_role: LinkRole = .mirror,
    sync_direction: SyncDirection = .@"two-way",
    initial_status: SyncStatus = .never,
    config_json: ?[]const u8 = null,
};

pub const ListFilter = struct {
    entity_kind: ?ExternalEntityKind = null,
    entity_id: ?i64 = null,
    system_id: ?i64 = null,
    system_slug: ?[]const u8 = null,
};

pub const Error = error{
    NotFound,
    LinkExists,
    QueryFailed,
} || std.mem.Allocator.Error;

pub fn create(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: CreateArgs) Error!ExtLink {
    const insert_sql: [:0]const u8 =
        \\insert into external_links
        \\  (entity_kind, entity_id, system_id, external_id, external_url, link_role, sync_direction, last_sync_status, config_json)
        \\values (?, ?, ?, ?, ?, ?, ?, ?, ?)
    ;
    const id = d.execParams(insert_sql, &.{
        .{ .text = @tagName(args.entity_kind) },
        .{ .int = args.entity_id },
        .{ .int = args.system_id },
        .{ .text = args.external_id },
        if (args.external_url) |s| .{ .text = s } else .{ .null = {} },
        .{ .text = @tagName(args.link_role) },
        .{ .text = args.sync_direction.toText() },
        .{ .text = args.initial_status.toText() },
        if (args.config_json) |s| .{ .text = s } else .{ .null = {} },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.LinkExists;
        std.log.err("external.link.create exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };
    return try show(d, allocator, id);
}

pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!ExtLink {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

pub fn linksForEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_kind: ExternalEntityKind,
    entity_id: i64,
) Error![]ExtLink {
    return try list(d, allocator, .{
        .entity_kind = entity_kind,
        .entity_id = entity_id,
    });
}

pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: ListFilter) Error![]ExtLink {
    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, select_list_select);

    if (filter.system_slug != null) {
        try sql_buf.appendSlice(allocator, " join external_systems es on es.id = el.system_id");
    }
    try sql_buf.appendSlice(allocator, " where 1 = 1");

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    if (filter.system_slug) |slug| {
        try sql_buf.appendSlice(allocator, " and es.slug = ?");
        try params.append(allocator, .{ .text = slug });
    }
    if (filter.entity_kind) |k| {
        try sql_buf.appendSlice(allocator, " and el.entity_kind = ?");
        try params.append(allocator, .{ .text = @tagName(k) });
    }
    if (filter.entity_id) |id| {
        try sql_buf.appendSlice(allocator, " and el.entity_id = ?");
        try params.append(allocator, .{ .int = id });
    }
    if (filter.system_id) |sid| {
        try sql_buf.appendSlice(allocator, " and el.system_id = ?");
        try params.append(allocator, .{ .int = sid });
    }
    try sql_buf.appendSlice(allocator, " order by el.id");

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var out: std.ArrayList(ExtLink) = .empty;
    errdefer {
        for (out.items) |item| deinit(item, allocator);
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

pub fn allPullable(d: *db.sqlite.Db, allocator: std.mem.Allocator) Error![]ExtLink {
    return try readMany(d, allocator, pullable_sql);
}

pub fn allPushable(d: *db.sqlite.Db, allocator: std.mem.Allocator) Error![]ExtLink {
    return try readMany(d, allocator, pushable_sql);
}

pub fn updateSyncState(d: *db.sqlite.Db, link_id: i64, status: SyncStatus) Error!void {
    _ = d.execParams(
        \\update external_links
        \\set last_synced_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'),
        \\    last_sync_status = ?
        \\where id = ?
    , &.{ .{ .text = status.toText() }, .{ .int = link_id } }) catch {
        return Error.QueryFailed;
    };
    const changed = d.intQuery("select changes()") catch return Error.QueryFailed;
    if (changed == 0) return Error.NotFound;
}

pub fn updateSyncDirection(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    link_id: i64,
    direction: SyncDirection,
) Error!SyncDirection {
    d.exec("begin immediate") catch return Error.QueryFailed;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    var stmt = d.prepare("select sync_direction from external_links where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = link_id }}) catch return Error.QueryFailed;
    const prior: SyncDirection = switch (stmt.step() catch return Error.QueryFailed) {
        .done => return Error.NotFound,
        .row => blk: {
            const txt = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(txt);
            break :blk SyncDirection.fromText(txt) orelse return Error.QueryFailed;
        },
    };

    _ = d.execParams(
        "update external_links set sync_direction = ? where id = ?",
        &.{ .{ .text = direction.toText() }, .{ .int = link_id } },
    ) catch return Error.QueryFailed;

    const detail = try std.fmt.allocPrint(
        allocator,
        "{{\"old_direction\":\"{s}\",\"new_direction\":\"{s}\",\"operation\":\"sync_direction_update\"}}",
        .{ prior.toText(), direction.toText() },
    );
    defer allocator.free(detail);
    _ = d.execParams(
        \\insert into sync_events (link_id, direction, outcome, fields_changed, detail)
        \\values (?, 'push', 'ok', ?, ?)
    , &.{ .{ .int = link_id }, .{ .text = "[\"sync_direction\"]" }, .{ .text = detail } }) catch return Error.QueryFailed;

    d.exec("commit") catch return Error.QueryFailed;
    committed = true;
    return prior;
}

pub fn delete(d: *db.sqlite.Db, link_id: i64) Error!void {
    _ = d.execParams("delete from external_links where id = ?", &.{.{ .int = link_id }}) catch {
        return Error.QueryFailed;
    };
    const changed = d.intQuery("select changes()") catch return Error.QueryFailed;
    if (changed == 0) return Error.NotFound;
}

const select_columns =
    "id, entity_kind, entity_id, system_id, external_id, external_url, link_role, " ++
    "sync_direction, last_synced_at, last_sync_status, config_json, created_at";
const select_one_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from external_links where id = ?";
const select_list_select =
    "select " ++
    "el.id, el.entity_kind, el.entity_id, el.system_id, el.external_id, el.external_url, el.link_role, " ++
    "el.sync_direction, el.last_synced_at, el.last_sync_status, el.config_json, el.created_at " ++
    "from external_links el";
const pullable_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from external_links where sync_direction in ('read-only','two-way') order by id";
const pushable_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from external_links where sync_direction in ('write-back','two-way') order by id";

fn readMany(d: *db.sqlite.Db, allocator: std.mem.Allocator, sql: [:0]const u8) Error![]ExtLink {
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return Error.QueryFailed;

    var out: std.ArrayList(ExtLink) = .empty;
    errdefer {
        for (out.items) |item| deinit(item, allocator);
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

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!ExtLink {
    const entity_kind_text = try stmt.columnTextAlloc(1, allocator);
    defer allocator.free(entity_kind_text);
    const entity_kind = ExternalEntityKind.fromText(entity_kind_text) orelse return Error.QueryFailed;

    const role_text = try stmt.columnTextAlloc(6, allocator);
    defer allocator.free(role_text);
    const role = LinkRole.fromText(role_text) orelse return Error.QueryFailed;

    const dir_text = try stmt.columnTextAlloc(7, allocator);
    defer allocator.free(dir_text);
    const direction = SyncDirection.fromText(dir_text) orelse return Error.QueryFailed;

    const status_text = try stmt.columnTextAlloc(9, allocator);
    defer allocator.free(status_text);
    const status = SyncStatus.fromText(status_text) orelse return Error.QueryFailed;

    return .{
        .id = stmt.columnInt(0),
        .entity_kind = entity_kind,
        .entity_id = stmt.columnInt(2),
        .system_id = stmt.columnInt(3),
        .external_id = try stmt.columnTextAlloc(4, allocator),
        .external_url = try stmt.columnTextOpt(5, allocator),
        .link_role = role,
        .sync_direction = direction,
        .last_synced_at = try stmt.columnTextOpt(8, allocator),
        .last_sync_status = status,
        .config_json = try stmt.columnTextOpt(10, allocator),
        .created_at = try stmt.columnTextAlloc(11, allocator),
    };
}

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var dn = try db.sqlite.Db.openMemory();
    errdefer dn.close();
    try db.migrate.applyAll(&dn, allocator);
    return dn;
}

fn mustInsertTask(d: *db.sqlite.Db) !i64 {
    return try d.execParams(
        \\insert into tasks (scope_kind, scope_id, title, status, priority)
        \\values ('global', null, 'ext-link task', 'todo', 100)
    , &.{});
}

fn mustInsertSystem(d: *db.sqlite.Db, slug: []const u8) !i64 {
    return try d.execParams(
        \\insert into external_systems (kind, slug, base_url, auth_method, auth_ref)
        \\values ('jira', ?, 'https://example.atlassian.net', 'token-env', 'JIRA_TOKEN')
    , &.{.{ .text = slug }});
}

fn expectContains(haystack: []const u8, needle: []const u8) !void {
    try std.testing.expect(std.mem.indexOf(u8, haystack, needle) != null);
}

test "create defaults: role mirror, direction two-way, status never" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d);
    const system_id = try mustInsertSystem(&d, "sys-main");

    const link = try create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "PROJ-1",
    });
    defer deinit(link, a);

    try std.testing.expectEqual(LinkRole.mirror, link.link_role);
    try std.testing.expectEqual(SyncDirection.@"two-way", link.sync_direction);
    try std.testing.expectEqual(SyncStatus.never, link.last_sync_status);
    try std.testing.expect(link.external_url == null);
}

test "create duplicate returns LinkExists" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d);
    const system_id = try mustInsertSystem(&d, "sys-dup");

    const link = try create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "PROJ-2",
    });
    defer deinit(link, a);

    try std.testing.expectError(
        Error.LinkExists,
        create(&d, a, .{
            .entity_kind = .task,
            .entity_id = task_id,
            .system_id = system_id,
            .external_id = "PROJ-2",
        }),
    );
}

test "list supports entity and system slug filters" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d);
    const sys_a = try mustInsertSystem(&d, "sys-a");
    const sys_b = try mustInsertSystem(&d, "sys-b");

    const a_link = try create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = sys_a,
        .external_id = "A-1",
    });
    defer deinit(a_link, a);
    const b_link = try create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = sys_b,
        .external_id = "B-1",
    });
    defer deinit(b_link, a);

    const by_entity = try linksForEntity(&d, a, .task, task_id);
    defer deinitMany(by_entity, a);
    try std.testing.expectEqual(@as(usize, 2), by_entity.len);

    const by_slug = try list(&d, a, .{ .system_slug = "sys-b" });
    defer deinitMany(by_slug, a);
    try std.testing.expectEqual(@as(usize, 1), by_slug.len);
    try std.testing.expectEqualStrings("B-1", by_slug[0].external_id);
}

test "allPullable and allPushable match sync_direction policy" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d);
    const system_id = try mustInsertSystem(&d, "sys-sync");

    const ro = try create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "RO",
        .sync_direction = .@"read-only",
    });
    defer deinit(ro, a);
    const wb = try create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "WB",
        .sync_direction = .@"write-back",
    });
    defer deinit(wb, a);
    const tw = try create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "TW",
        .sync_direction = .@"two-way",
    });
    defer deinit(tw, a);

    const pullable = try allPullable(&d, a);
    defer deinitMany(pullable, a);
    try std.testing.expectEqual(@as(usize, 2), pullable.len);

    const pushable = try allPushable(&d, a);
    defer deinitMany(pushable, a);
    try std.testing.expectEqual(@as(usize, 2), pushable.len);
}

test "updateSyncState sets status and timestamp" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d);
    const system_id = try mustInsertSystem(&d, "sys-state");

    const link = try create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "S-1",
    });
    defer deinit(link, a);

    try updateSyncState(&d, link.id, .ok);

    const refreshed = try show(&d, a, link.id);
    defer deinit(refreshed, a);
    try std.testing.expectEqual(SyncStatus.ok, refreshed.last_sync_status);
    try std.testing.expect(refreshed.last_synced_at != null);
}

test "updateSyncDirection writes sync_events audit row and returns previous direction" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d);
    const system_id = try mustInsertSystem(&d, "sys-dir");

    const link = try create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "D-1",
        .sync_direction = .@"read-only",
    });
    defer deinit(link, a);

    const previous = try updateSyncDirection(&d, a, link.id, .@"write-back");
    try std.testing.expectEqual(SyncDirection.@"read-only", previous);

    const changed = try show(&d, a, link.id);
    defer deinit(changed, a);
    try std.testing.expectEqual(SyncDirection.@"write-back", changed.sync_direction);

    const count = try d.intQuery("select count(*) from sync_events where outcome = 'ok'");
    try std.testing.expectEqual(@as(i64, 1), count);

    var stmt = try d.prepare("select detail from sync_events where link_id = ? order by id desc limit 1");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = link.id }});
    switch (try stmt.step()) {
        .done => return error.TestUnexpectedResult,
        .row => {
            const detail = try stmt.columnTextAlloc(0, a);
            defer a.free(detail);
            try expectContains(detail, "read-only");
            try expectContains(detail, "write-back");
            try expectContains(detail, "sync_direction_update");
        },
    }
}

test "delete removes a link and not-found is surfaced" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d);
    const system_id = try mustInsertSystem(&d, "sys-del");

    const link = try create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "DEL-1",
    });
    defer deinit(link, a);

    try delete(&d, link.id);
    try std.testing.expectError(Error.NotFound, show(&d, a, link.id));
    try std.testing.expectError(Error.NotFound, delete(&d, 99999));
}
