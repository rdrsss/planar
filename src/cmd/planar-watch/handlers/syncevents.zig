//! handlers/syncevents — `planar-watch sync-events [--plan <id>] [--system <slug>]
//!                        [--entity kind:id] [--outcome <val>] [--since <ts>]
//!                        [--limit <n>] --json`
//!
//! JSON shape:
//!   { generated_at: ISO8601, sync_events: [SyncEventRow] }
//!
//! Returns rows from sync_events in `at` descending order (most recent first).
//! All filters are optional and compose with AND. The `--plan` filter matches
//! events whose `link_id` points at an external_links row whose entity is the
//! specified plan. The `--system` filter matches by external_systems.slug.
//! The `--entity` filter accepts `kind:id` form and matches external_links rows
//! directly. Rows with `link_id IS NULL` (workbench conflict events with no
//! counterpart) are excluded by the `--plan`, `--system`, and `--entity` filters.
//!
//! read-only: planar-watch opens the database read-only; no writes happen here.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const ps = @import("ps.zig");

pub const verb: cli.Cmd = .{
    .name = "sync-events",
    .desc = "List sync_events rows with optional filters (read-only).",
    .long_desc = "Returns sync_events rows ordered by `at` descending.\n\n" ++
        "  --plan     : restrict to events whose link belongs to the given plan id.\n" ++
        "  --system   : restrict to events via a link on the given external system slug.\n" ++
        "  --entity   : restrict to events via a link on one entity, `kind:id` form.\n" ++
        "  --outcome  : filter by outcome value (ok, conflict, error, noop, …).\n" ++
        "  --since    : only return rows with `at` >= this ISO8601 timestamp.\n" ++
        "  --limit    : cap row count (default 100).",
    .flags = &.{
        .{ .long = "--plan", .kind = .int, .desc = "Filter by plan id" },
        .{ .long = "--system", .kind = .string, .desc = "Filter by external system slug" },
        .{ .long = "--entity", .kind = .string, .desc = "Filter by entity, kind:id form (e.g. task:42)" },
        .{ .long = "--outcome", .kind = .string, .desc = "Filter by outcome (ok, conflict, error, noop, …)" },
        .{ .long = "--since", .kind = .string, .desc = "Only rows at >= this ISO8601 timestamp" },
        .{ .long = "--limit", .kind = .int, .desc = "Row cap (default 100)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

const SyncEventRow = struct {
    id: i64,
    link_id: ?i64,
    scope: []const u8,
    direction: []const u8,
    outcome: []const u8,
    fields_changed: ?[]const u8,
    detail: ?[]const u8,
    context_json: ?[]const u8,
    at: []const u8,

    fn deinit(self: SyncEventRow, allocator: std.mem.Allocator) void {
        allocator.free(self.scope);
        allocator.free(self.direction);
        allocator.free(self.outcome);
        if (self.fields_changed) |s| allocator.free(s);
        if (self.detail) |s| allocator.free(s);
        if (self.context_json) |s| allocator.free(s);
        allocator.free(self.at);
    }

    fn deinitMany(rows: []SyncEventRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"sync-events"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    emitOnce(ctx.stdout, d, ctx.allocator, args) catch |e|
        exit.die(ctx, e, "sync-events: {s}", .{@errorName(e)});
    try ctx.stdout.flush();
}

fn emitOnce(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: anytype,
) !void {
    const limit = if (args.limit) |n| n else 100;

    // Decompose --entity if supplied (kind:id form).
    var entity_kind_filter: ?[]const u8 = null;
    var entity_id_filter: ?i64 = null;
    if (args.entity) |e_text| {
        const colon = std.mem.indexOf(u8, e_text, ":") orelse return error.InvalidInput;
        entity_kind_filter = e_text[0..colon];
        entity_id_filter = std.fmt.parseInt(i64, e_text[colon + 1 ..], 10) catch return error.InvalidInput;
    }

    const rows = try listSyncEvents(d, allocator, limit);
    defer SyncEventRow.deinitMany(rows, allocator);

    if (args.json) {
        try w.print("{{\"generated_at\":", .{});
        try ps.writeNowIso(w);
        try w.print(",\"sync_events\":[", .{});
        var first = true;
        for (rows) |row| {
            if (!eventMatches(d, row, args, entity_kind_filter, entity_id_filter)) continue;
            if (!first) try w.print(",", .{});
            first = false;
            try writeSyncEventJSON(w, row);
        }
        try w.print("]}}\n", .{});
    } else {
        var count: usize = 0;
        for (rows) |row| {
            if (!eventMatches(d, row, args, entity_kind_filter, entity_id_filter)) continue;
            count += 1;
        }
        try w.print("sync_events: {d}\n", .{count});
        for (rows) |row| {
            if (!eventMatches(d, row, args, entity_kind_filter, entity_id_filter)) continue;
            const lid_str = if (row.link_id) |l| l else 0;
            try w.print(
                "  id:{d}  link:{d}  scope:{s}  direction:{s}  outcome:{s}  at:{s}\n",
                .{ row.id, lid_str, row.scope, row.direction, row.outcome, row.at },
            );
        }
    }
}

fn eventMatches(
    d: *db.sqlite.Db,
    row: SyncEventRow,
    args: anytype,
    entity_kind_filter: ?[]const u8,
    entity_id_filter: ?i64,
) bool {
    // Outcome filter (direct column match).
    if (args.outcome) |o| {
        if (!std.mem.eql(u8, row.outcome, o)) return false;
    }

    // Since filter (at >= <timestamp>).
    if (args.since) |s| {
        if (std.mem.lessThan(u8, row.at, s)) return false;
    }

    // --plan, --system, --entity all require a link_id to join through.
    const need_link = (args.plan != null or args.system != null or
        entity_kind_filter != null or entity_id_filter != null);
    if (need_link) {
        const lid = row.link_id orelse return false;
        if (args.plan) |pid| {
            if (!linkBelongsToPlan(d, lid, pid)) return false;
        }
        if (args.system) |slug| {
            if (!linkBelongsToSystem(d, lid, slug)) return false;
        }
        if (entity_kind_filter) |ek| {
            if (!linkHasEntity(d, lid, ek, entity_id_filter)) return false;
        }
    }

    return true;
}

/// Returns true if external_links row `link_id` has entity_kind='plan' AND entity_id=plan_id.
fn linkBelongsToPlan(d: *db.sqlite.Db, link_id: i64, plan_id: i64) bool {
    var stmt = d.prepare(
        "select 1 from external_links where id = ? and entity_kind = 'plan' and entity_id = ?",
    ) catch return false;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = link_id }, .{ .int = plan_id } }) catch return false;
    const step = stmt.step() catch return false;
    return step == .row;
}

/// Returns true if external_links row `link_id` joins to an external_systems row with the given slug.
fn linkBelongsToSystem(d: *db.sqlite.Db, link_id: i64, slug: []const u8) bool {
    var stmt = d.prepare(
        "select 1 from external_links el" ++
            " join external_systems es on es.id = el.system_id" ++
            " where el.id = ? and es.slug = ?",
    ) catch return false;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = link_id }, .{ .text = slug } }) catch return false;
    const step = stmt.step() catch return false;
    return step == .row;
}

/// Returns true if external_links row `link_id` has entity_kind = `ek` and (if given) entity_id = `eid`.
fn linkHasEntity(d: *db.sqlite.Db, link_id: i64, ek: []const u8, eid: ?i64) bool {
    if (eid) |id| {
        var stmt = d.prepare(
            "select 1 from external_links where id = ? and entity_kind = ? and entity_id = ?",
        ) catch return false;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = link_id }, .{ .text = ek }, .{ .int = id } }) catch return false;
        const step = stmt.step() catch return false;
        return step == .row;
    } else {
        var stmt = d.prepare(
            "select 1 from external_links where id = ? and entity_kind = ?",
        ) catch return false;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = link_id }, .{ .text = ek } }) catch return false;
        const step = stmt.step() catch return false;
        return step == .row;
    }
}

fn listSyncEvents(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    limit: i64,
) ![]SyncEventRow {
    var sql_buf: [512]u8 = undefined;
    const sql = std.fmt.bufPrintZ(&sql_buf,
        \\select id, link_id, scope, direction, outcome,
        \\       fields_changed, detail, context_json, at
        \\from sync_events
        \\order by at desc, id desc
        \\limit {d}
    , .{limit}) catch return error.QueryFailed;

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(SyncEventRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readSyncEventRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn readSyncEventRow(
    stmt: *db.sqlite.Stmt,
    allocator: std.mem.Allocator,
) !SyncEventRow {
    return .{
        .id = stmt.columnInt(0),
        .link_id = stmt.columnIntOpt(1),
        .scope = try stmt.columnTextAlloc(2, allocator),
        .direction = try stmt.columnTextAlloc(3, allocator),
        .outcome = try stmt.columnTextAlloc(4, allocator),
        .fields_changed = try stmt.columnTextOpt(5, allocator),
        .detail = try stmt.columnTextOpt(6, allocator),
        .context_json = try stmt.columnTextOpt(7, allocator),
        .at = try stmt.columnTextAlloc(8, allocator),
    };
}

fn writeSyncEventJSON(w: *std.Io.Writer, row: SyncEventRow) !void {
    try w.print("{{", .{});
    try w.print("\"id\":{d}", .{row.id});
    if (row.link_id) |lid| {
        try w.print(",\"link_id\":{d}", .{lid});
    } else {
        try w.print(",\"link_id\":null", .{});
    }
    try w.writeAll(",\"scope\":");
    try writeJsonString(w, row.scope);
    try w.writeAll(",\"direction\":");
    try writeJsonString(w, row.direction);
    try w.writeAll(",\"outcome\":");
    try writeJsonString(w, row.outcome);
    if (row.fields_changed) |fc| {
        try w.writeAll(",\"fields_changed\":");
        try writeJsonString(w, fc);
    } else {
        try w.print(",\"fields_changed\":null", .{});
    }
    if (row.detail) |det| {
        try w.writeAll(",\"detail\":");
        try writeJsonString(w, det);
    } else {
        try w.print(",\"detail\":null", .{});
    }
    if (row.context_json) |cj| {
        // context_json is already JSON — emit raw, not as a string.
        try w.print(",\"context_json\":{s}", .{cj});
    } else {
        try w.print(",\"context_json\":null", .{});
    }
    try w.writeAll(",\"at\":");
    try writeJsonString(w, row.at);
    try w.print("}}", .{});
}

fn writeJsonString(w: *std.Io.Writer, value: []const u8) !void {
    try std.json.Stringify.encodeJsonString(value, .{}, w);
}
