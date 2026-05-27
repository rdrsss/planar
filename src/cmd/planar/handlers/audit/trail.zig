//! handlers/audit/trail — `planar audit trail <entity-id> [--kind] [--grep] | --link <id>`
//!
//! Two forms:
//!   1. Entity form (M7, slug engine-audit-read): reads `audit_log` rows for
//!      `<kind>:<entity-id>` plus everything reachable via `entity_links`.
//!   2. Link form (M8, task 2297): walks `external_links` + `sync_events` for
//!      the specified link id, matching the Go binary's `audit trail <link-id>`.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "audit", "trail" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    if (args.link) |link_raw| {
        const link_id = std.fmt.parseInt(i64, link_raw, 10) catch
            exit.die(ctx, error.InvalidInput, "invalid --link value '{s}'", .{link_raw});
        return runLinkForm(ctx, d, link_id, args.json);
    }

    const entity_raw = args.entity_id orelse
        exit.die(ctx, error.InvalidInput, "audit trail requires <entity-id> or --link <id>", .{});

    const id = std.fmt.parseInt(i64, entity_raw, 10) catch
        exit.die(ctx, error.InvalidInput, "entity id must be an integer, got '{s}'", .{entity_raw});

    const kind: []const u8 = args.kind orelse "task";

    const entries = if (args.grep) |pat|
        engine.runtime.audit_trail.forEntityGrep(d, ctx.allocator, kind, id, pat) catch |e|
            exit.die(ctx, e, "audit trail grep: {s}", .{@errorName(e)})
    else
        engine.runtime.audit_trail.forEntityWithLinks(d, ctx.allocator, kind, id) catch |e|
            exit.die(ctx, e, "audit trail: {s}", .{@errorName(e)});
    defer engine.runtime.audit_trail.deinitEntries(entries, ctx.allocator);

    if (args.json) {
        try ctx.stdout.print("{{\"entity_kind\":", .{});
        try output.writeJsonString(ctx.stdout, kind);
        try ctx.stdout.print(",\"entity_id\":{d},\"entries\":[", .{id});
        for (entries, 0..) |e, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"id\":{d},\"verb\":", .{e.id});
            try output.writeJsonString(ctx.stdout, e.verb);
            try ctx.stdout.print(",\"entity_kind\":", .{});
            try output.writeJsonString(ctx.stdout, e.entity_kind);
            try ctx.stdout.print(",\"entity_id\":{d},\"recorded_at\":", .{e.entity_id});
            try output.writeJsonString(ctx.stdout, e.recorded_at);
            if (e.actor) |a| {
                try ctx.stdout.print(",\"actor\":", .{});
                try output.writeJsonString(ctx.stdout, a);
            }
            if (e.scope) |s| {
                try ctx.stdout.print(",\"scope\":", .{});
                try output.writeJsonString(ctx.stdout, s);
            }
            if (e.summary) |s| {
                try ctx.stdout.print(",\"summary\":", .{});
                try output.writeJsonString(ctx.stdout, s);
            }
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]}}\n", .{});
        return;
    }

    try ctx.stdout.print("audit trail for {s}:{d}  ({d} entries)\n", .{ kind, @as(u64, @intCast(id)), entries.len });
    if (entries.len == 0) {
        try ctx.stdout.print("  (no audit_log entries)\n", .{});
        return;
    }
    for (entries) |e| {
        try ctx.stdout.print(
            "  [{s}]  {s:<14}  {s}:{d}",
            .{ e.recorded_at, e.verb, e.entity_kind, @as(u64, @intCast(e.entity_id)) },
        );
        if (e.summary) |s| try ctx.stdout.print("  — {s}", .{s});
        try ctx.stdout.print("\n", .{});
    }
}

const SessionRow = struct {
    id: i64,
    vendor: []const u8,
    started_at: []const u8,
    summary: []const u8,
};

const DecisionRow = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
    decided_at: []const u8,
};

fn freeSessions(rows: []const SessionRow, allocator: std.mem.Allocator) void {
    for (rows) |r| {
        allocator.free(r.vendor);
        allocator.free(r.started_at);
        allocator.free(r.summary);
    }
    allocator.free(rows);
}

fn freeDecisions(rows: []const DecisionRow, allocator: std.mem.Allocator) void {
    for (rows) |r| {
        allocator.free(r.title);
        allocator.free(r.status);
        allocator.free(r.decided_at);
    }
    allocator.free(rows);
}

/// loadSessionsForEntity mirrors Go's sessionsForEntity. For tasks: read
/// sessions.task_id directly. For other kinds: traverse entity_links
/// (both directions) into the sessions table.
fn loadSessionsForEntity(
    allocator: std.mem.Allocator,
    d: anytype,
    entity_kind: []const u8,
    entity_id: i64,
) ![]SessionRow {
    var rows: std.ArrayList(SessionRow) = .empty;
    errdefer {
        for (rows.items) |r| {
            allocator.free(r.vendor);
            allocator.free(r.started_at);
            allocator.free(r.summary);
        }
        rows.deinit(allocator);
    }

    if (std.mem.eql(u8, entity_kind, "task")) {
        var stmt = try d.prepare(
            \\select id, coalesce(vendor,''), coalesce(started_at,''), coalesce(summary,'')
            \\from sessions where task_id = ? order by started_at desc
        );
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = entity_id }});
        while (true) {
            const step = try stmt.step();
            if (step == .done) break;
            try rows.append(allocator, .{
                .id = stmt.columnInt(0),
                .vendor = try stmt.columnTextAlloc(1, allocator),
                .started_at = try stmt.columnTextAlloc(2, allocator),
                .summary = try stmt.columnTextAlloc(3, allocator),
            });
        }
        return try rows.toOwnedSlice(allocator);
    }

    var stmt = try d.prepare(
        \\select distinct s.id, coalesce(s.vendor,''), coalesce(s.started_at,''), coalesce(s.summary,'')
        \\from sessions s where s.id in (
        \\  select to_id   from entity_links where from_kind = ? and from_id = ? and to_kind   = 'session'
        \\  union
        \\  select from_id from entity_links where to_kind   = ? and to_id   = ? and from_kind = 'session'
        \\)
        \\order by s.started_at desc
    );
    defer stmt.finalize();
    try stmt.bind(&.{
        .{ .text = entity_kind }, .{ .int = entity_id },
        .{ .text = entity_kind }, .{ .int = entity_id },
    });
    while (true) {
        const step = try stmt.step();
        if (step == .done) break;
        try rows.append(allocator, .{
            .id = stmt.columnInt(0),
            .vendor = try stmt.columnTextAlloc(1, allocator),
            .started_at = try stmt.columnTextAlloc(2, allocator),
            .summary = try stmt.columnTextAlloc(3, allocator),
        });
    }
    return try rows.toOwnedSlice(allocator);
}

/// loadDecisionsForEntity mirrors Go's decisionsForEntity. Joins decisions
/// via entity_links(to_kind='decision').
fn loadDecisionsForEntity(
    allocator: std.mem.Allocator,
    d: anytype,
    entity_kind: []const u8,
    entity_id: i64,
) ![]DecisionRow {
    var rows: std.ArrayList(DecisionRow) = .empty;
    errdefer {
        for (rows.items) |r| {
            allocator.free(r.title);
            allocator.free(r.status);
            allocator.free(r.decided_at);
        }
        rows.deinit(allocator);
    }

    var stmt = try d.prepare(
        \\select d.id, coalesce(d.title,''), coalesce(d.status,''), coalesce(d.decided_at,'')
        \\from decisions d
        \\join entity_links el on el.to_kind = 'decision' and el.to_id = d.id
        \\where el.from_kind = ? and el.from_id = ?
        \\order by d.created_at desc
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .text = entity_kind }, .{ .int = entity_id } });
    while (true) {
        const step = try stmt.step();
        if (step == .done) break;
        try rows.append(allocator, .{
            .id = stmt.columnInt(0),
            .title = try stmt.columnTextAlloc(1, allocator),
            .status = try stmt.columnTextAlloc(2, allocator),
            .decided_at = try stmt.columnTextAlloc(3, allocator),
        });
    }
    return try rows.toOwnedSlice(allocator);
}

/// `audit trail --link <id>` — link-scoped traversal: external_links row +
/// external_system slug + sync_events history + linked sessions + linked
/// decisions. Mirrors Go's runAuditTrail (link-id form).
fn runLinkForm(
    ctx: *const runtime.Ctx,
    d: anytype,
    link_id: i64,
    json: bool,
) !void {
    const link = engine.external.link.show(d, ctx.allocator, link_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "external link {d} not found", .{link_id}),
        else => exit.die(ctx, e, "audit trail --link: load link: {s}", .{@errorName(e)}),
    };
    defer engine.external.link.deinit(link, ctx.allocator);

    // Resolve the system slug, falling back to a stack-buffered
    // "system:<id>" placeholder when the system row vanished. We track
    // ownership explicitly via `sys_slug_owned` rather than inferring it
    // from a `startsWith("system:")` heuristic — a real system slug
    // beginning with "system:" would otherwise leak the heap dupe AND
    // dangle on the stack buffer when the placeholder branch fired.
    var sys_slug_buf: [64]u8 = undefined;
    var sys_slug_owned: bool = false;
    const sys_slug: []const u8 = blk: {
        const sys = engine.external.system.showById(d, ctx.allocator, link.system_id) catch {
            break :blk std.fmt.bufPrint(&sys_slug_buf, "system:{d}", .{link.system_id}) catch unreachable;
        };
        defer engine.external.system.deinit(sys, ctx.allocator);
        const owned = try ctx.allocator.dupe(u8, sys.slug);
        sys_slug_owned = true;
        break :blk owned;
    };
    defer if (sys_slug_owned) ctx.allocator.free(sys_slug);

    const events = engine.external.sync.eventsForLink(d, ctx.allocator, link_id) catch |e|
        exit.die(ctx, e, "audit trail --link: load events: {s}", .{@errorName(e)});
    defer engine.external.sync.deinitSyncEvents(events, ctx.allocator);

    const entity_kind_text = @tagName(link.entity_kind);
    const sessions = loadSessionsForEntity(ctx.allocator, d, entity_kind_text, link.entity_id) catch |e|
        exit.die(ctx, e, "audit trail --link: load sessions: {s}", .{@errorName(e)});
    defer freeSessions(sessions, ctx.allocator);

    const decisions = loadDecisionsForEntity(ctx.allocator, d, entity_kind_text, link.entity_id) catch |e|
        exit.die(ctx, e, "audit trail --link: load decisions: {s}", .{@errorName(e)});
    defer freeDecisions(decisions, ctx.allocator);

    if (json) {
        try ctx.stdout.print("{{\"link_id\":{d},\"entity_kind\":", .{link.id});
        try output.writeJsonString(ctx.stdout, entity_kind_text);
        try ctx.stdout.print(",\"entity_id\":{d},\"external_id\":", .{link.entity_id});
        try output.writeJsonString(ctx.stdout, link.external_id);
        try ctx.stdout.print(",\"system_slug\":", .{});
        try output.writeJsonString(ctx.stdout, sys_slug);

        // sessions
        try ctx.stdout.print(",\"sessions\":[", .{});
        for (sessions, 0..) |s, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"id\":{d},\"vendor\":", .{s.id});
            try output.writeJsonString(ctx.stdout, s.vendor);
            try ctx.stdout.print(",\"started_at\":", .{});
            try output.writeJsonString(ctx.stdout, s.started_at);
            if (s.summary.len > 0) {
                try ctx.stdout.print(",\"summary\":", .{});
                try output.writeJsonString(ctx.stdout, s.summary);
            }
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]", .{});

        // decisions
        try ctx.stdout.print(",\"decisions\":[", .{});
        for (decisions, 0..) |dec, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"id\":{d},\"title\":", .{dec.id});
            try output.writeJsonString(ctx.stdout, dec.title);
            try ctx.stdout.print(",\"status\":", .{});
            try output.writeJsonString(ctx.stdout, dec.status);
            if (dec.decided_at.len > 0) {
                try ctx.stdout.print(",\"decided_at\":", .{});
                try output.writeJsonString(ctx.stdout, dec.decided_at);
            }
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]", .{});

        // sync_events
        try ctx.stdout.print(",\"sync_events\":[", .{});
        for (events, 0..) |e, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"id\":{d},\"direction\":", .{e.id});
            try output.writeJsonString(ctx.stdout, e.direction);
            try ctx.stdout.print(",\"outcome\":", .{});
            try output.writeJsonString(ctx.stdout, e.outcome);
            if (e.fields_changed) |fc| {
                try ctx.stdout.print(",\"fields_changed\":", .{});
                try output.writeJsonString(ctx.stdout, fc);
            }
            if (e.detail) |dt| {
                try ctx.stdout.print(",\"detail\":", .{});
                try output.writeJsonString(ctx.stdout, dt);
            }
            try ctx.stdout.print(",\"at\":", .{});
            try output.writeJsonString(ctx.stdout, e.at);
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]}}\n", .{});
        return;
    }

    try ctx.stdout.print(
        "audit trail for link {d}  ({s}:{d} ↔ {s}:{s})\n",
        .{ link.id, entity_kind_text, link.entity_id, sys_slug, link.external_id },
    );

    if (sessions.len > 0) {
        try ctx.stdout.print("\nsessions:\n", .{});
        for (sessions) |s| {
            const summary_display = if (s.summary.len > 0) s.summary else "(no summary)";
            try ctx.stdout.print("  {s}  {s}  session:{d}  \"{s}\"\n", .{ s.started_at, s.vendor, s.id, summary_display });
        }
    }

    if (decisions.len > 0) {
        try ctx.stdout.print("\ndecisions:\n", .{});
        for (decisions) |dec| {
            try ctx.stdout.print("  {s}  \"{s}\"  [{s}]\n", .{ dec.decided_at, dec.title, dec.status });
        }
    }

    if (events.len > 0) {
        try ctx.stdout.print("\nsync events:\n", .{});
        for (events) |e| {
            const fc = e.fields_changed orelse "(no fields)";
            try ctx.stdout.print("  {s}  {s:<5}  {s:<10}  {s}\n", .{ e.at, e.direction, e.outcome, fc });
        }
    }

    if (sessions.len == 0 and decisions.len == 0 and events.len == 0) {
        try ctx.stdout.print("  (no sessions, decisions, or sync events)\n", .{});
    }
}
