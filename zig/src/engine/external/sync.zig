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
    StaleConflict,
    EvidenceChanged,
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
    context_json: ?[]const u8,
    at: []const u8,
};

pub fn deinitSyncEvents(events: []const SyncEvent, allocator: std.mem.Allocator) void {
    for (events) |e| {
        allocator.free(e.direction);
        allocator.free(e.outcome);
        if (e.fields_changed) |s| allocator.free(s);
        if (e.detail) |s| allocator.free(s);
        if (e.context_json) |s| allocator.free(s);
        allocator.free(e.at);
    }
    allocator.free(events);
}

/// All sync_events for a given link, oldest first. Mirrors Go's
/// `external.EventsForLink` and feeds `audit trail --link <id>`.
pub fn eventsForLink(d: *db.sqlite.Db, allocator: std.mem.Allocator, link_id: i64) Error![]SyncEvent {
    var stmt = d.prepare(
        \\select id, direction, outcome, fields_changed, detail, context_json, at
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
            if (e.context_json) |s| allocator.free(s);
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
                    .context_json = try stmt.columnTextOpt(5, allocator),
                    .at = try stmt.columnTextAlloc(6, allocator),
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
    var conflict_local: ?EntityFields = null;
    defer if (conflict_local) |fields| deinitEntityFields(fields, allocator);
    switch (link.sync_direction) {
        .@"write-back" => {
            result.outcome = .noop;
        },
        .@"read-only", .@"two-way" => {
            var allow_title = true;
            var allow_status = true;
            var baseline = try loadBaseline(d, allocator, link.id);
            defer baseline.deinit(allocator);
            if (link.sync_direction == .@"two-way" and baseline.present()) {
                conflict_local = localEntityFields(d, allocator, link.entity_kind, link.entity_id) catch |err| switch (err) {
                    Error.NotFound => null,
                    else => return err,
                };
                if (conflict_local) |fields| {
                    const title_remote_changed = remote.title.len > 0 and !std.mem.eql(u8, remote.title, baseline.title.?);
                    const title_local_changed = !std.mem.eql(u8, fields.title, baseline.title.?);
                    const status_remote_changed = remote.status.len > 0 and !std.mem.eql(u8, remote.status, baseline.status.?);
                    const status_local_changed = !std.mem.eql(u8, fields.status, baseline.status.?);
                    const title_conflict = title_remote_changed and title_local_changed and !std.mem.eql(u8, remote.title, fields.title);
                    const status_conflict = status_remote_changed and status_local_changed and !std.mem.eql(u8, remote.status, fields.status);
                    if (title_conflict or status_conflict) {
                        var conflicts: std.ArrayList([]const u8) = .empty;
                        if (title_conflict) try conflicts.append(allocator, "title");
                        if (status_conflict) try conflicts.append(allocator, "status");
                        result.outcome = .conflict;
                        result.fields_changed = try conflicts.toOwnedSlice(allocator);
                        result.detail = try allocator.dupe(u8, "local and remote changed since the last successful sync");
                    } else {
                        allow_title = title_remote_changed;
                        allow_status = status_remote_changed;
                    }
                }
            }

            if (result.outcome != .conflict) {
                const changed = try applyRemoteToLocal(d, allocator, link, remote, true, true, allow_title, allow_status);
                if (changed.len > 0) {
                    result.outcome = .ok;
                    result.fields_changed = changed;
                } else {
                    result.outcome = .noop;
                }

                const after = localEntityFields(d, allocator, link.entity_kind, link.entity_id) catch |err| switch (err) {
                    Error.NotFound => null,
                    else => return err,
                };
                if (after) |fields| {
                    defer deinitEntityFields(fields, allocator);
                    const baseline_title = if (allow_title or !baseline.present()) fields.title else baseline.title.?;
                    const baseline_status = if (allow_status or !baseline.present()) fields.status else baseline.status.?;
                    try storeBaseline(d, link.id, baseline_title, baseline_status);
                }
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
    var evidence_json: ?[]const u8 = null;
    defer if (evidence_json) |s| allocator.free(s);
    if (result.outcome == .conflict) {
        const local_fields = conflict_local orelse return Error.NotFound;
        evidence_json = try conflictEvidenceJson(d, allocator, link.id, local_fields, remote);
    }
    _ = try insertSyncEvent(d, link.id, "pull", result.outcome.toEventText(), changed_json, if (result.detail.len == 0) null else result.detail, evidence_json);

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
    try storeBaseline(d, link.id, fields.title, fields.status);
    const changed_json = marshalFieldsChanged(allocator, update.fields_applied) catch return Error.QueryFailed;
    defer if (changed_json) |s| allocator.free(s);
    _ = try insertSyncEvent(d, link.id, "push", "ok", changed_json, null, null);

    d.exec("commit") catch return Error.QueryFailed;
    committed = true;

    return .{
        .link_id = link.id,
        .outcome = .ok,
        .fields_changed = if (update.fields_applied.len == 0) &.{} else try copyFieldNames(allocator, update.fields_applied),
    };
}

pub fn resolveConflict(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    event_id: i64,
    keep: ResolveKeep,
    expected_evidence_token: []const u8,
    expected_local_updated_at: []const u8,
    adapter: anytype,
) Error!ResolveResult {
    d.exec("begin immediate") catch return Error.QueryFailed;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    const event = try loadConflictEvent(d, allocator, event_id);
    defer event.deinit(allocator);
    if (!std.mem.eql(u8, event.outcome, "conflict")) return Error.NotConflict;
    if (!std.mem.eql(u8, event.evidence.token, expected_evidence_token)) return Error.EvidenceChanged;
    if (try latestEventId(d, event.link_id) != event_id) return Error.StaleConflict;

    const link = external_link.show(d, allocator, event.link_id) catch |e| switch (e) {
        error.NotFound => return Error.NotFound,
        else => return Error.QueryFailed,
    };
    defer external_link.deinit(link, allocator);
    if (link.last_sync_status != .conflict) return Error.StaleConflict;

    const local = try localEntityFields(d, allocator, link.entity_kind, link.entity_id);
    defer deinitEntityFields(local, allocator);
    if (!std.mem.eql(u8, local.updated_at, expected_local_updated_at)) return Error.EvidenceChanged;

    const direction: []const u8 = switch (keep) {
        .local => "push",
        .remote => "pull",
    };
    const Adapter = @TypeOf(adapter.*);
    const remote = extsync.dispatch(Adapter, .pull, adapter, extsync.PullArgs{
        .allocator = allocator,
        .external_id = link.external_id,
    }) catch return Error.AdapterFailed;
    defer extsync.deinitRemoteState(remote, allocator);
    if (event.evidence.remote.version.len == 0 or
        remote.version.len == 0 or
        !std.mem.eql(u8, remote.title, event.evidence.remote.title) or
        !std.mem.eql(u8, remote.status, event.evidence.remote.status) or
        !std.mem.eql(u8, remote.version, event.evidence.remote.version))
    {
        return Error.EvidenceChanged;
    }

    if (keep == .local) {
        const pushed = extsync.dispatch(Adapter, .push, adapter, extsync.PushArgs{
            .allocator = allocator,
            .external_id = link.external_id,
            .fields = .{ .title = local.title, .status = local.status },
        }) catch return Error.AdapterFailed;
        defer extsync.deinitUpdateOutcome(pushed, allocator);
    }

    if (keep == .remote) {
        const changed = try applyRemoteToLocal(d, allocator, link, remote, true, false, true, true);
        if (changed.len > 0) allocator.free(changed);
    }

    external_link.updateSyncState(d, link.id, .ok) catch |e| switch (e) {
        error.NotFound => return Error.NotFound,
        else => return Error.QueryFailed,
    };
    const resolved_fields = try localEntityFields(d, allocator, link.entity_kind, link.entity_id);
    defer deinitEntityFields(resolved_fields, allocator);
    try storeBaseline(d, link.id, resolved_fields.title, resolved_fields.status);
    const detail = try std.fmt.allocPrint(allocator, "resolved={s}; from sync_event={d}", .{ @tagName(keep), event_id });
    defer allocator.free(detail);
    const new_event_id = try insertSyncEvent(d, link.id, direction, "ok", null, detail, null);

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

const ConflictEvidence = struct {
    version: i64,
    token: []const u8,
    observed_at: []const u8,
    local: struct { title: []const u8, status: []const u8, updated_at: []const u8, source: []const u8 },
    remote: struct { title: []const u8, status: []const u8, version: []const u8, source: []const u8 },
};

const LoadedConflictEvent = struct {
    link_id: i64,
    outcome: []const u8,
    context_json: []const u8,
    parsed: std.json.Parsed(ConflictEvidence),
    evidence: ConflictEvidence,

    fn deinit(self: LoadedConflictEvent, allocator: std.mem.Allocator) void {
        allocator.free(self.outcome);
        allocator.free(self.context_json);
        self.parsed.deinit();
    }
};

fn loadConflictEvent(d: *db.sqlite.Db, allocator: std.mem.Allocator, event_id: i64) Error!LoadedConflictEvent {
    var stmt = d.prepare("select link_id, outcome, context_json from sync_events where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = event_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => blk: {
            if (stmt.columnIsNull(0)) return Error.NotFound;
            const outcome = try stmt.columnTextAlloc(1, allocator);
            errdefer allocator.free(outcome);
            const context_json = (try stmt.columnTextOpt(2, allocator)) orelse return Error.EvidenceChanged;
            errdefer allocator.free(context_json);
            const parsed = std.json.parseFromSlice(ConflictEvidence, allocator, context_json, .{ .ignore_unknown_fields = true }) catch return Error.EvidenceChanged;
            break :blk .{ .link_id = stmt.columnInt(0), .outcome = outcome, .context_json = context_json, .parsed = parsed, .evidence = parsed.value };
        },
    };
}

fn latestEventId(d: *db.sqlite.Db, link_id: i64) Error!i64 {
    var stmt = d.prepare("select coalesce(max(id), 0) from sync_events where link_id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = link_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => 0,
        .row => stmt.columnInt(0),
    };
}

const EntityFields = struct {
    title: []const u8,
    status: []const u8,
    updated_at: []const u8,
};

const Baseline = struct {
    title: ?[]const u8,
    status: ?[]const u8,

    fn present(self: Baseline) bool {
        return self.title != null and self.status != null;
    }

    fn deinit(self: Baseline, allocator: std.mem.Allocator) void {
        if (self.title) |s| allocator.free(s);
        if (self.status) |s| allocator.free(s);
    }
};

fn loadBaseline(d: *db.sqlite.Db, allocator: std.mem.Allocator, link_id: i64) Error!Baseline {
    var stmt = d.prepare("select baseline_title, baseline_status from external_links where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = link_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => .{
            .title = try stmt.columnTextOpt(0, allocator),
            .status = try stmt.columnTextOpt(1, allocator),
        },
    };
}

fn storeBaseline(d: *db.sqlite.Db, link_id: i64, title: []const u8, status_value: []const u8) Error!void {
    _ = d.execParams(
        "update external_links set baseline_title = ?, baseline_status = ? where id = ?",
        &.{ .{ .text = title }, .{ .text = status_value }, .{ .int = link_id } },
    ) catch return Error.QueryFailed;
}

fn deinitEntityFields(fields: EntityFields, allocator: std.mem.Allocator) void {
    allocator.free(fields.title);
    allocator.free(fields.status);
    allocator.free(fields.updated_at);
}

fn localEntityFields(d: *db.sqlite.Db, allocator: std.mem.Allocator, kind: external_link.ExternalEntityKind, entity_id: i64) Error!EntityFields {
    const sql: [:0]const u8 = switch (kind) {
        .task => "select coalesce(title,''), coalesce(status,''), updated_at from tasks where id = ?",
        .plan => "select coalesce(title,''), coalesce(status,''), updated_at from plans where id = ?",
        .question => "select coalesce(title,''), coalesce(status,''), updated_at from questions where id = ?",
        .artifact => "select coalesce(title,''), coalesce(status,''), updated_at from artifacts where id = ?",
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
            .updated_at = try stmt.columnTextAlloc(2, allocator),
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
    allow_title: bool,
    allow_status: bool,
) Error![]const []const u8 {
    const local = localEntityFields(d, allocator, link.entity_kind, link.entity_id) catch |e| switch (e) {
        Error.NotFound => if (allow_missing_local) return &.{} else return Error.NotFound,
        Error.UnsupportedEntityKind => return &.{},
        else => return e,
    };
    defer deinitEntityFields(local, allocator);

    const title_changed = allow_title and remote.title.len > 0 and !std.mem.eql(u8, local.title, remote.title);
    const status_changed = allow_status and if (skip_empty_remote_status) (remote.status.len > 0 and !std.mem.eql(u8, local.status, remote.status)) else (!std.mem.eql(u8, local.status, remote.status));
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

fn conflictEvidenceJson(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    link_id: i64,
    local: EntityFields,
    remote: extsync.RemoteState,
) Error![]const u8 {
    var ts_stmt = d.prepare("select strftime('%Y-%m-%dT%H:%M:%fZ','now')") catch return Error.QueryFailed;
    defer ts_stmt.finalize();
    if ((ts_stmt.step() catch return Error.QueryFailed) != .row) return Error.QueryFailed;
    const observed_at = try ts_stmt.columnTextAlloc(0, allocator);
    defer allocator.free(observed_at);
    var token_input: std.Io.Writer.Allocating = .init(allocator);
    defer token_input.deinit();
    token_input.writer.print("v1\x00{d}\x00{s}\x00{s}\x00{s}\x00{s}\x00{s}\x00{s}", .{
        link_id,
        local.title,
        local.status,
        local.updated_at,
        remote.title,
        remote.status,
        remote.version,
    }) catch return Error.QueryFailed;
    var digest: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(token_input.written(), &digest, .{});
    const token_hex = std.fmt.bytesToHex(digest, .lower);

    const evidence = .{
        .version = @as(i64, 1),
        .token = token_hex[0..],
        .observed_at = observed_at,
        .local = .{ .title = local.title, .status = local.status, .updated_at = local.updated_at, .source = "planar entity" },
        .remote = .{ .title = remote.title, .status = remote.status, .version = remote.version, .source = "external adapter pull" },
    };
    return extsync.jsonStringifyAlloc(allocator, evidence) catch return Error.QueryFailed;
}

fn insertSyncEvent(
    d: *db.sqlite.Db,
    link_id: i64,
    direction: []const u8,
    outcome: []const u8,
    fields_changed_json: ?[]const u8,
    detail: ?[]const u8,
    context_json: ?[]const u8,
) Error!i64 {
    return d.execParams(
        \\insert into sync_events (link_id, direction, outcome, fields_changed, detail, context_json)
        \\values (?, ?, ?, ?, ?, ?)
    , &.{
        .{ .int = link_id },
        .{ .text = direction },
        .{ .text = outcome },
        if (fields_changed_json) |s| .{ .text = s } else .{ .null = {} },
        if (detail) |s| .{ .text = s } else .{ .null = {} },
        if (context_json) |s| .{ .text = s } else .{ .null = {} },
    }) catch return Error.QueryFailed;
}

fn bestEffortErrorWrite(d: *db.sqlite.Db, allocator: std.mem.Allocator, link_id: i64, direction: []const u8, detail: []const u8) void {
    external_link.updateSyncState(d, link_id, .@"error") catch return;
    _ = insertSyncEvent(d, link_id, direction, "error", null, detail, null) catch {};
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
    remote_version: []const u8 = "adapter-v1",
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
            .version = try allocator.dupe(u8, self.remote_version),
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

const SeededConflict = struct {
    id: i64,
    token: []const u8,
    local_updated_at: []const u8,

    fn deinit(self: SeededConflict, allocator: std.mem.Allocator) void {
        allocator.free(self.token);
        allocator.free(self.local_updated_at);
    }
};

fn seedConflictForTest(d: *db.sqlite.Db, allocator: std.mem.Allocator, link: external_link.ExtLink, adapter: *FakeAdapter) !SeededConflict {
    const local = try localEntityFields(d, allocator, link.entity_kind, link.entity_id);
    defer deinitEntityFields(local, allocator);
    const remote = try adapter.pull(allocator, link.external_id);
    defer extsync.deinitRemoteState(remote, allocator);
    const evidence_json = try conflictEvidenceJson(d, allocator, link.id, local, remote);
    defer allocator.free(evidence_json);
    var parsed = try std.json.parseFromSlice(ConflictEvidence, allocator, evidence_json, .{});
    defer parsed.deinit();
    try external_link.updateSyncState(d, link.id, .conflict);
    return .{
        .id = try insertSyncEvent(d, link.id, "pull", "conflict", "[\"title\"]", "test conflict", evidence_json),
        .token = try allocator.dupe(u8, parsed.value.token),
        .local_updated_at = try allocator.dupe(u8, local.updated_at),
    };
}

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

test "pull read-only applies remote state without enabling push" {
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
    try std.testing.expectEqual(Outcome.ok, result.outcome);

    var stmt = try d.prepare("select title from tasks where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    switch (try stmt.step()) {
        .done => return error.TestUnexpectedResult,
        .row => {
            const title = try stmt.columnTextAlloc(0, a);
            defer a.free(title);
            try std.testing.expectEqualStrings("Remote", title);
        },
    }
}

test "two-way pull reports a conflict when local and remote diverge from baseline" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try mustInsertTask(&d, "Initial", "todo");
    const system_id = try mustInsertSystem(&d, "sys-conflict");
    const link = try external_link.create(&d, a, .{
        .entity_kind = .task,
        .entity_id = task_id,
        .system_id = system_id,
        .external_id = "PROJ-C",
        .sync_direction = .@"two-way",
    });
    defer external_link.deinit(link, a);

    var adapter = FakeAdapter{ .remote_title = "Baseline", .remote_status = "todo" };
    const initial = try pullLink(&d, a, link, &adapter);
    deinitPullResult(initial, a);
    _ = try d.execParams("update tasks set title = 'Local edit' where id = ?", &.{.{ .int = task_id }});
    adapter.remote_title = "Remote edit";

    const conflicted = try pullLink(&d, a, link, &adapter);
    defer deinitPullResult(conflicted, a);
    try std.testing.expectEqual(Outcome.conflict, conflicted.outcome);
    try std.testing.expectEqual(@as(usize, 1), conflicted.fields_changed.len);
    try std.testing.expectEqualStrings("title", conflicted.fields_changed[0]);

    var stmt = try d.prepare("select title from tasks where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    switch (try stmt.step()) {
        .done => return error.TestUnexpectedResult,
        .row => {
            const title = try stmt.columnTextAlloc(0, a);
            defer a.free(title);
            try std.testing.expectEqualStrings("Local edit", title);
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

    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "doing" };
    const conflict = try seedConflictForTest(&d, a, link, &adapter);
    defer conflict.deinit(a);
    const resolved = try resolveConflict(&d, a, conflict.id, .local, conflict.token, conflict.local_updated_at, &adapter);
    try std.testing.expect(resolved.ok);
    try std.testing.expect(resolved.new_event_id > conflict.id);

    const refreshed = try external_link.show(&d, a, link.id);
    defer external_link.deinit(refreshed, a);
    try std.testing.expectEqual(external_link.SyncStatus.ok, refreshed.last_sync_status);
}

test "resolveConflict rejects a non-latest conflict without remote mutation" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d, "Conflict Task", "todo");
    const system_id = try mustInsertSystem(&d, "sys-stale-event");
    const link = try external_link.create(&d, a, .{ .entity_kind = .task, .entity_id = task_id, .system_id = system_id, .external_id = "PROJ-STALE", .sync_direction = .@"two-way" });
    defer external_link.deinit(link, a);
    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "doing" };
    const conflict = try seedConflictForTest(&d, a, link, &adapter);
    defer conflict.deinit(a);
    _ = try insertSyncEvent(&d, link.id, "pull", "noop", null, null, null);

    try std.testing.expectError(Error.StaleConflict, resolveConflict(&d, a, conflict.id, .local, conflict.token, conflict.local_updated_at, &adapter));
    try std.testing.expectEqual(@as(usize, 0), adapter.pushes);
}

test "resolveConflict rejects an intervening local version without remote mutation" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d, "Conflict Task", "todo");
    const system_id = try mustInsertSystem(&d, "sys-local-race");
    const link = try external_link.create(&d, a, .{ .entity_kind = .task, .entity_id = task_id, .system_id = system_id, .external_id = "PROJ-LOCAL", .sync_direction = .@"two-way" });
    defer external_link.deinit(link, a);
    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "doing" };
    const conflict = try seedConflictForTest(&d, a, link, &adapter);
    defer conflict.deinit(a);
    _ = try d.execParams("update tasks set title = 'Manual merge', updated_at = '2099-01-01T00:00:00.000Z' where id = ?", &.{.{ .int = task_id }});

    try std.testing.expectError(Error.EvidenceChanged, resolveConflict(&d, a, conflict.id, .local, conflict.token, conflict.local_updated_at, &adapter));
    try std.testing.expectEqual(@as(usize, 0), adapter.pushes);
}

test "resolveConflict rejects an intervening remote value without push" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d, "Conflict Task", "todo");
    const system_id = try mustInsertSystem(&d, "sys-remote-race");
    const link = try external_link.create(&d, a, .{ .entity_kind = .task, .entity_id = task_id, .system_id = system_id, .external_id = "PROJ-REMOTE", .sync_direction = .@"two-way" });
    defer external_link.deinit(link, a);
    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "doing" };
    const conflict = try seedConflictForTest(&d, a, link, &adapter);
    defer conflict.deinit(a);
    adapter.remote_title = "Changed again";

    try std.testing.expectError(Error.EvidenceChanged, resolveConflict(&d, a, conflict.id, .local, conflict.token, conflict.local_updated_at, &adapter));
    try std.testing.expectEqual(@as(usize, 0), adapter.pushes);
}

test "resolveConflict rejects absent approved or fresh provider versions without mutation" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const task_id = try mustInsertTask(&d, "Conflict Task", "todo");
    const system_id = try mustInsertSystem(&d, "sys-missing-version");
    const link = try external_link.create(&d, a, .{ .entity_kind = .task, .entity_id = task_id, .system_id = system_id, .external_id = "PROJ-NOVERSION", .sync_direction = .@"two-way" });
    defer external_link.deinit(link, a);
    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "doing" };

    const approved_missing = try seedConflictForTest(&d, a, link, &adapter);
    defer approved_missing.deinit(a);
    _ = try d.execParams(
        "update sync_events set context_json = replace(context_json, 'adapter-v1', '') where id = ?",
        &.{.{ .int = approved_missing.id }},
    );
    try std.testing.expectError(Error.EvidenceChanged, resolveConflict(&d, a, approved_missing.id, .local, approved_missing.token, approved_missing.local_updated_at, &adapter));
    try std.testing.expectEqual(@as(usize, 0), adapter.pushes);
    try std.testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from sync_events"));

    _ = try d.execParams("delete from sync_events where id = ?", &.{.{ .int = approved_missing.id }});
    const fresh_missing = try seedConflictForTest(&d, a, link, &adapter);
    defer fresh_missing.deinit(a);
    adapter.remote_version = "";
    try std.testing.expectError(Error.EvidenceChanged, resolveConflict(&d, a, fresh_missing.id, .remote, fresh_missing.token, fresh_missing.local_updated_at, &adapter));
    try std.testing.expectEqual(@as(usize, 0), adapter.pushes);
    try std.testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from sync_events"));
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

    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "" };
    const conflict = try seedConflictForTest(&d, a, link, &adapter);
    defer conflict.deinit(a);
    _ = try resolveConflict(&d, a, conflict.id, .remote, conflict.token, conflict.local_updated_at, &adapter);

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

    const evidence = "{\"version\":1,\"token\":\"missing\",\"observed_at\":\"now\",\"local\":{\"title\":\"\",\"status\":\"\",\"updated_at\":\"missing\",\"source\":\"planar entity\"},\"remote\":{\"title\":\"Remote title\",\"status\":\"doing\",\"version\":\"adapter-v1\",\"source\":\"external adapter pull\"}}";
    const event_id = try insertSyncEvent(&d, link.id, "pull", "conflict", null, "status conflict", evidence);
    try external_link.updateSyncState(&d, link.id, .conflict);

    var adapter = FakeAdapter{ .remote_title = "Remote title", .remote_status = "doing" };
    try std.testing.expectError(Error.NotFound, resolveConflict(&d, a, event_id, .remote, "missing", "missing", &adapter));

    const refreshed = try external_link.show(&d, a, link.id);
    defer external_link.deinit(refreshed, a);
    try std.testing.expectEqual(external_link.SyncStatus.conflict, refreshed.last_sync_status);

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
