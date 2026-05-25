//! engine/extsync/strategy — strategy stickiness cache, restrategize abandon,
//! and verify-counterparts probe.
//!
//! Mirrors the strategy-stickiness slice of Go `internal/extsync/engine.go`:
//!
//!   - Cache the chosen strategy on `external_links.config_json` of the anchor
//!     plan's mirror link under the key "strategy". The cache key MUST NOT be
//!     renamed (Go and zig both depend on it).
//!   - `readCachedStrategy` returns the cached value or null.
//!   - `writeAnchorConfigJSON` merges arbitrary key/value pairs into the anchor
//!     plan's mirror link config_json.
//!   - `abandonCounterparts` records a `sync_events(outcome='strategy-abandoned')`
//!     row for every existing mirror link in the feature subtree, then DELETEs
//!     the links so re-propagation starts clean. Each abandonment runs in its
//!     own `begin immediate` / `commit` block (Go behaviour).
//!   - `recordCounterpartMissing` records a `sync_events(outcome='counterpart-missing')`
//!     row and optionally DELETEs the link (when `--unlink` or `--recreate` is
//!     passed). Single transaction per link.
//!
//! The CLI handler composes these helpers; this module makes no I/O assumptions
//! (no stdin prompts here — those live in the handler so unit tests can drive
//! the engine directly without simulating stdin).

const std = @import("std");
const db = @import("db");

/// Key under which the chosen strategy is cached in `external_links.config_json`.
/// Read-side tools (Go and zig) compare against this key by name.
pub const strategy_cache_key = "strategy";

pub const Error = error{
    QueryFailed,
    NotFound,
    ParseFailed,
} || std.mem.Allocator.Error;

/// readCachedStrategy returns the cached "strategy" value for the anchor plan's
/// mirror link on `system_id`, or null when no row, no config_json, or the JSON
/// has no "strategy" key. JSON parse failures return null (treated as "no cache").
pub fn readCachedStrategy(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
    system_id: i64,
) Error!?[]const u8 {
    var stmt = d.prepare(
        \\select coalesce(config_json, '') from external_links
        \\where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = system_id } }) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return null;
    const raw = try stmt.columnTextAlloc(0, allocator);
    defer allocator.free(raw);
    if (raw.len == 0) return null;

    var parsed = std.json.parseFromSlice(std.json.Value, allocator, raw, .{}) catch return null;
    defer parsed.deinit();
    if (parsed.value != .object) return null;
    const strategy_v = parsed.value.object.get(strategy_cache_key) orelse return null;
    if (strategy_v != .string) return null;
    return try allocator.dupe(u8, strategy_v.string);
}

/// writeAnchorConfigJSON merges the given key/value map into the anchor plan's
/// mirror-link config_json, preserving keys not in `pairs`. No-ops (returns
/// `NotFound`) when the anchor has no mirror link yet — the cache is written
/// alongside the very first link.create call in that case.
pub fn writeAnchorConfigJSON(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
    system_id: i64,
    pairs: []const ConfigPair,
) Error!void {
    // Read existing config_json (or empty object if NULL).
    var stmt = d.prepare(
        \\select coalesce(config_json, '{}') from external_links
        \\where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = system_id } }) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return Error.NotFound;
    const existing = try stmt.columnTextAlloc(0, allocator);
    defer allocator.free(existing);

    // Parse-or-empty, merge, re-serialize. Use a simple ordered slice (<10
    // keys typical) so we can preserve insertion order without
    // StringArrayHashMap (absent from this zig stdlib build).
    var entries: std.ArrayList(ConfigEntry) = .empty;
    defer {
        for (entries.items) |e| {
            allocator.free(e.key);
            allocator.free(e.value_json);
        }
        entries.deinit(allocator);
    }

    if (existing.len > 0 and existing[0] == '{') {
        var parsed = std.json.parseFromSlice(std.json.Value, allocator, existing, .{}) catch null;
        defer if (parsed) |*pp| pp.deinit();
        if (parsed) |p| if (p.value == .object) {
            var it = p.value.object.iterator();
            while (it.next()) |kv| {
                const k_owned = try allocator.dupe(u8, kv.key_ptr.*);
                errdefer allocator.free(k_owned);
                const v_owned = stringifyJsonValue(allocator, kv.value_ptr.*) catch return Error.ParseFailed;
                errdefer allocator.free(v_owned);
                try entries.append(allocator, .{ .key = k_owned, .value_json = v_owned });
            }
        };
    }

    for (pairs) |p| {
        const v_owned = p.toJsonOwned(allocator) catch return Error.ParseFailed;
        errdefer allocator.free(v_owned);
        // Replace or insert.
        var replaced = false;
        for (entries.items) |*e| {
            if (std.mem.eql(u8, e.key, p.key)) {
                allocator.free(e.value_json);
                e.value_json = v_owned;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            const k_owned = try allocator.dupe(u8, p.key);
            errdefer allocator.free(k_owned);
            try entries.append(allocator, .{ .key = k_owned, .value_json = v_owned });
        }
    }

    // Serialize back: {"k1":<v1>,"k2":<v2>,...}
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();
    const w = &out.writer;
    serializeMerged(w, entries.items) catch return Error.ParseFailed;
    const new_json = out.written();

    _ = d.execParams(
        \\update external_links set config_json = ?
        \\where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror'
    , &.{
        .{ .text = new_json },
        .{ .int = anchor_plan_id },
        .{ .int = system_id },
    }) catch return Error.QueryFailed;
}

/// ConfigPair is a typed key/value pair for `writeAnchorConfigJSON`. The value
/// is serialized as a JSON literal — strings are quoted; ints and bools render
/// bare.
pub const ConfigPair = struct {
    key: []const u8,
    value: Value,

    pub const Value = union(enum) {
        string: []const u8,
        int: i64,
        boolean: bool,
    };

    fn toJsonOwned(self: ConfigPair, allocator: std.mem.Allocator) ![]u8 {
        var out: std.Io.Writer.Allocating = .init(allocator);
        defer out.deinit();
        const w = &out.writer;
        switch (self.value) {
            .string => |s| try writeJsonString(w, s),
            .int => |n| try w.print("{d}", .{n}),
            .boolean => |b| try w.writeAll(if (b) "true" else "false"),
        }
        return try allocator.dupe(u8, out.written());
    }
};

/// abandonCounterparts records a `sync_events(outcome='strategy-abandoned')`
/// row for every mirror link in the feature subtree (anchor plan + all
/// descendants — plans via parent_plan_id, tasks via entity_links
/// derives-from), then DELETEs the link rows. Returns the count of abandoned
/// links. Each abandonment is its own transaction so a mid-loop failure leaves
/// already-abandoned rows committed (matching Go's per-link tx pattern).
///
/// The old remote counterparts are NOT contacted — Planar only stops tracking
/// them; the operator cleans up the remote manually.
pub fn abandonCounterparts(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
    system_id: i64,
    old_strategy: []const u8,
    new_strategy: []const u8,
) Error!usize {
    // Collect link rows first (avoid mutating during iteration).
    const Row = struct {
        id: i64,
        entity_kind: []const u8,
        entity_id: i64,
        external_id: []const u8,
    };
    var rows: std.ArrayList(Row) = .empty;
    defer {
        for (rows.items) |r| {
            allocator.free(r.entity_kind);
            allocator.free(r.external_id);
        }
        rows.deinit(allocator);
    }

    {
        var stmt = d.prepare(
            \\with recursive plan_tree(id) as (
            \\  select ? union all
            \\  select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id
            \\)
            \\select el.id, el.entity_kind, el.entity_id, el.external_id
            \\from external_links el
            \\where el.system_id = ? and el.link_role = 'mirror'
            \\  and (
            \\    (el.entity_kind = 'plan' and el.entity_id in (select id from plan_tree))
            \\    or (el.entity_kind = 'task' and el.entity_id in (
            \\      select t.id from tasks t
            \\      join entity_links tl on tl.from_kind = 'task' and tl.from_id = t.id
            \\                          and tl.to_kind = 'plan' and tl.relationship = 'derives-from'
            \\      where tl.to_id in (select id from plan_tree)
            \\    ))
            \\  )
            \\order by el.id
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = system_id } }) catch return Error.QueryFailed;
        while (true) {
            const step = stmt.step() catch return Error.QueryFailed;
            if (step == .done) break;
            const id = stmt.columnInt(0);
            const ek = try stmt.columnTextAlloc(1, allocator);
            errdefer allocator.free(ek);
            const eid = stmt.columnInt(2);
            const xid = try stmt.columnTextAlloc(3, allocator);
            errdefer allocator.free(xid);
            try rows.append(allocator, .{ .id = id, .entity_kind = ek, .entity_id = eid, .external_id = xid });
        }
    }

    for (rows.items) |r| {
        const ctx_json = try std.fmt.allocPrint(
            allocator,
            "{{\"old_strategy\":\"{s}\",\"new_strategy\":\"{s}\",\"external_id\":\"{s}\",\"entity_kind\":\"{s}\",\"entity_id\":{d}}}",
            .{ old_strategy, new_strategy, r.external_id, r.entity_kind, r.entity_id },
        );
        defer allocator.free(ctx_json);

        d.exec("begin immediate") catch return Error.QueryFailed;
        var committed = false;
        defer if (!committed) d.exec("rollback") catch {};

        _ = d.execParams(
            \\insert into sync_events (link_id, scope, direction, outcome, context_json)
            \\values (?, 'external', 'push', 'strategy-abandoned', ?)
        , &.{ .{ .int = r.id }, .{ .text = ctx_json } }) catch return Error.QueryFailed;
        _ = d.execParams(
            "delete from external_links where id = ?",
            &.{.{ .int = r.id }},
        ) catch return Error.QueryFailed;

        d.exec("commit") catch return Error.QueryFailed;
        committed = true;
    }

    return rows.items.len;
}

/// MirrorLink is a row returned by `listMirrorLinksInTree`. All slices are
/// allocator-owned and must be freed via `deinitMirrorLinks`.
pub const MirrorLink = struct {
    id: i64,
    entity_kind: []const u8,
    entity_id: i64,
    external_id: []const u8,
    external_url: []const u8,
};

pub fn deinitMirrorLinks(items: []const MirrorLink, allocator: std.mem.Allocator) void {
    for (items) |m| {
        allocator.free(m.entity_kind);
        allocator.free(m.external_id);
        allocator.free(m.external_url);
    }
    allocator.free(items);
}

/// listMirrorLinksInTree returns every mirror link for `system_id` belonging
/// to the feature subtree rooted at `anchor_plan_id`. Used by --verify-counterparts.
pub fn listMirrorLinksInTree(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
    system_id: i64,
) Error![]MirrorLink {
    var out: std.ArrayList(MirrorLink) = .empty;
    errdefer {
        for (out.items) |m| {
            allocator.free(m.entity_kind);
            allocator.free(m.external_id);
            allocator.free(m.external_url);
        }
        out.deinit(allocator);
    }

    var stmt = d.prepare(
        \\with recursive plan_tree(id) as (
        \\  select ? union all
        \\  select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id
        \\)
        \\select el.id, el.entity_kind, el.entity_id, el.external_id, coalesce(el.external_url, '')
        \\from external_links el
        \\where el.system_id = ? and el.link_role = 'mirror'
        \\  and (
        \\    (el.entity_kind = 'plan' and el.entity_id in (select id from plan_tree))
        \\    or (el.entity_kind = 'task' and el.entity_id in (
        \\      select t.id from tasks t
        \\      join entity_links tl on tl.from_kind = 'task' and tl.from_id = t.id
        \\                          and tl.to_kind = 'plan' and tl.relationship = 'derives-from'
        \\      where tl.to_id in (select id from plan_tree)
        \\    ))
        \\  )
        \\order by el.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = system_id } }) catch return Error.QueryFailed;
    while (true) {
        const step = stmt.step() catch return Error.QueryFailed;
        if (step == .done) break;
        const id = stmt.columnInt(0);
        const ek = try stmt.columnTextAlloc(1, allocator);
        errdefer allocator.free(ek);
        const eid = stmt.columnInt(2);
        const xid = try stmt.columnTextAlloc(3, allocator);
        errdefer allocator.free(xid);
        const xurl = try stmt.columnTextAlloc(4, allocator);
        errdefer allocator.free(xurl);
        try out.append(allocator, .{
            .id = id,
            .entity_kind = ek,
            .entity_id = eid,
            .external_id = xid,
            .external_url = xurl,
        });
    }
    return try out.toOwnedSlice(allocator);
}

/// recordCounterpartMissing records a `sync_events(outcome='counterpart-missing')`
/// row for the given link. When `unlink_or_recreate` is true, the link row is
/// also DELETEd (the `--unlink` and `--recreate` flags both want the row gone;
/// the difference is only that `--recreate` then re-propagates on a subsequent
/// call). Transactional.
pub fn recordCounterpartMissing(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    link_id: i64,
    entity_kind: []const u8,
    entity_id: i64,
    external_id: []const u8,
    unlink_or_recreate: bool,
) Error!void {
    const ctx_json = try std.fmt.allocPrint(
        allocator,
        "{{\"entity_kind\":\"{s}\",\"entity_id\":{d},\"external_id\":\"{s}\"}}",
        .{ entity_kind, entity_id, external_id },
    );
    defer allocator.free(ctx_json);

    d.exec("begin immediate") catch return Error.QueryFailed;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    _ = d.execParams(
        \\insert into sync_events (link_id, scope, direction, outcome, context_json)
        \\values (?, 'external', 'push', 'counterpart-missing', ?)
    , &.{ .{ .int = link_id }, .{ .text = ctx_json } }) catch return Error.QueryFailed;

    if (unlink_or_recreate) {
        _ = d.execParams(
            "delete from external_links where id = ?",
            &.{.{ .int = link_id }},
        ) catch return Error.QueryFailed;
    }

    d.exec("commit") catch return Error.QueryFailed;
    committed = true;
}

// ---- internal helpers -------------------------------------------------------

fn writeJsonString(w: *std.Io.Writer, s: []const u8) !void {
    var jsw: std.json.Stringify = .{ .writer = w, .options = .{} };
    try jsw.write(s);
}

/// ConfigEntry is one already-JSON-encoded key/value pair used internally by
/// `writeAnchorConfigJSON` and `serializeMerged`.
const ConfigEntry = struct { key: []u8, value_json: []u8 };

/// serializeMerged writes a `{"k":<v>, ...}` object literal where each value
/// is already-JSON-encoded.
fn serializeMerged(w: *std.Io.Writer, entries: []const ConfigEntry) !void {
    try w.writeAll("{");
    for (entries, 0..) |e, i| {
        if (i != 0) try w.writeAll(",");
        try writeJsonString(w, e.key);
        try w.writeAll(":");
        try w.writeAll(e.value_json);
    }
    try w.writeAll("}");
}

fn stringifyJsonValue(allocator: std.mem.Allocator, v: std.json.Value) ![]u8 {
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();
    var jsw: std.json.Stringify = .{ .writer = &out.writer, .options = .{} };
    try jsw.write(v);
    return try allocator.dupe(u8, out.written());
}

// ---- tests ------------------------------------------------------------------

const testing = std.testing;

fn setupDb() !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    try db.migrate.applyAll(&d, testing.allocator);
    return d;
}

test "readCachedStrategy returns null when no link row exists" {
    var d = try setupDb();
    defer d.close();
    const cached = try readCachedStrategy(testing.allocator, &d, 99, 1);
    try testing.expect(cached == null);
}

test "readCachedStrategy returns cached value when config_json carries it" {
    var d = try setupDb();
    defer d.close();
    // Seed an external system + an anchor plan + an external_links row.
    _ = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('github-issues', 'gh', '', '', 'token-env', 'X')",
        &.{},
    );
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global', 'p', 'p')", &.{});
    _ = try d.execParams(
        \\insert into external_links (entity_kind, entity_id, system_id, external_id, link_role, sync_direction, last_sync_status, config_json)
        \\values ('plan', 1, 1, 'ext-1', 'mirror', 'read-only', 'ok', '{"strategy":"github-tracking-issue","extra":"keep"}')
    , &.{});

    const cached = try readCachedStrategy(testing.allocator, &d, 1, 1);
    defer if (cached) |s| testing.allocator.free(s);
    try testing.expect(cached != null);
    try testing.expectEqualStrings("github-tracking-issue", cached.?);
}

test "writeAnchorConfigJSON merges keys and preserves prior entries" {
    var d = try setupDb();
    defer d.close();
    _ = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('github-issues', 'gh', '', '', 'token-env', 'X')",
        &.{},
    );
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global', 'p', 'p')", &.{});
    _ = try d.execParams(
        \\insert into external_links (entity_kind, entity_id, system_id, external_id, link_role, sync_direction, last_sync_status, config_json)
        \\values ('plan', 1, 1, 'ext-1', 'mirror', 'read-only', 'ok', '{"keep_me":"yes"}')
    , &.{});

    try writeAnchorConfigJSON(testing.allocator, &d, 1, 1, &.{
        .{ .key = "strategy", .value = .{ .string = "github-tracking-issue" } },
        .{ .key = "sub_issue_supported", .value = .{ .boolean = true } },
    });

    var stmt = try d.prepare("select config_json from external_links where id = 1");
    defer stmt.finalize();
    _ = try stmt.step();
    const got = try stmt.columnTextAlloc(0, testing.allocator);
    defer testing.allocator.free(got);
    // Order is insertion order via StringArrayHashMap; existing keys preserved.
    try testing.expect(std.mem.indexOf(u8, got, "\"keep_me\":\"yes\"") != null);
    try testing.expect(std.mem.indexOf(u8, got, "\"strategy\":\"github-tracking-issue\"") != null);
    try testing.expect(std.mem.indexOf(u8, got, "\"sub_issue_supported\":true") != null);
}

test "writeAnchorConfigJSON returns NotFound when no mirror link exists" {
    var d = try setupDb();
    defer d.close();
    const err = writeAnchorConfigJSON(testing.allocator, &d, 99, 1, &.{
        .{ .key = "strategy", .value = .{ .string = "x" } },
    });
    try testing.expectError(Error.NotFound, err);
}

test "abandonCounterparts deletes links and records sync_events" {
    var d = try setupDb();
    defer d.close();
    _ = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('github-issues', 'gh', '', '', 'token-env', 'X')",
        &.{},
    );
    _ = try d.execParams("insert into plans (scope_kind, title, slug, parent_plan_id) values ('global', 'anchor', 'a', null)", &.{});
    _ = try d.execParams("insert into plans (scope_kind, title, slug, parent_plan_id) values ('global', 'child', 'c', 1)", &.{});
    _ = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, link_role, sync_direction, last_sync_status) values ('plan', 1, 1, 'a#1', 'mirror', 'read-only', 'ok')",
        &.{},
    );
    _ = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, link_role, sync_direction, last_sync_status) values ('plan', 2, 1, 'c#1', 'mirror', 'read-only', 'ok')",
        &.{},
    );

    const n = try abandonCounterparts(testing.allocator, &d, 1, 1, "github-parent-issue", "github-tracking-issue");
    try testing.expectEqual(@as(usize, 2), n);

    const remaining = try d.intQuery("select count(*) from external_links where system_id = 1 and link_role = 'mirror'");
    try testing.expectEqual(@as(i64, 0), remaining);

    const events = try d.intQuery("select count(*) from sync_events where outcome = 'strategy-abandoned'");
    try testing.expectEqual(@as(i64, 2), events);
}

test "listMirrorLinksInTree walks plan subtree + derives-from tasks" {
    var d = try setupDb();
    defer d.close();
    _ = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('github-issues', 'gh', '', '', 'token-env', 'X')",
        &.{},
    );
    _ = try d.execParams("insert into plans (scope_kind, title, slug, parent_plan_id) values ('global', 'anchor', 'a', null)", &.{});
    _ = try d.execParams("insert into plans (scope_kind, title, slug, parent_plan_id) values ('global', 'child', 'c', 1)", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, title) values ('global', 't')", &.{});
    _ = try d.execParams("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'plan', 2, 'derives-from')", &.{});
    _ = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, link_role, sync_direction, last_sync_status) values ('plan', 1, 1, 'a#1', 'mirror', 'read-only', 'ok')",
        &.{},
    );
    _ = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, link_role, sync_direction, last_sync_status) values ('task', 1, 1, 'a#2', 'mirror', 'read-only', 'ok')",
        &.{},
    );

    const links = try listMirrorLinksInTree(testing.allocator, &d, 1, 1);
    defer deinitMirrorLinks(links, testing.allocator);
    try testing.expectEqual(@as(usize, 2), links.len);
}

test "recordCounterpartMissing inserts sync_event and optionally deletes link" {
    var d = try setupDb();
    defer d.close();
    _ = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('github-issues', 'gh', '', '', 'token-env', 'X')",
        &.{},
    );
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global', 'p', 'p')", &.{});
    _ = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, link_role, sync_direction, last_sync_status) values ('plan', 1, 1, 'gone#1', 'mirror', 'read-only', 'ok')",
        &.{},
    );

    // First, record-only (no unlink): the link survives.
    try recordCounterpartMissing(testing.allocator, &d, 1, "plan", 1, "gone#1", false);
    try testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from external_links where id = 1"));
    try testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from sync_events where outcome = 'counterpart-missing'"));

    // Then, with unlink: the link goes away.
    try recordCounterpartMissing(testing.allocator, &d, 1, "plan", 1, "gone#1", true);
    try testing.expectEqual(@as(i64, 0), try d.intQuery("select count(*) from external_links where id = 1"));
    try testing.expectEqual(@as(i64, 2), try d.intQuery("select count(*) from sync_events where outcome = 'counterpart-missing'"));
}
