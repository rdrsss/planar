//! handlers/audit/publish-decision — publish a decision as comments on every
//! directly or transitively linked operational-plane target.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const scope_mod = @import("../../scope.zig");
const adapter_factory = @import("../ext/adapter_factory.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "audit", "publish-decision" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const decision_id = std.fmt.parseInt(i64, args.decision_id, 10) catch
        exit.die(ctx, error.InvalidInput, "decision id must be an integer, got '{s}'", .{args.decision_id});
    const decision = engine.planning.decision.show(d, ctx.allocator, decision_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "decision {d} not found", .{decision_id}),
        else => exit.die(ctx, e, "audit publish-decision: loading decision: {s}", .{@errorName(e)}),
    };
    defer engine.planning.decision.deinit(decision, ctx.allocator);

    const resolution = scope_mod.resolveForWrite(ctx, args.scope) catch |e|
        exit.die(ctx, e, "audit publish-decision: resolving scope: {s}", .{@errorName(e)});
    const entity_scope = engine.identity.scope.slugFromRef(
        d,
        ctx.allocator,
        switch (decision.scope_kind) {
            .global => .global,
            .association => .association,
            .repo => .repo,
        },
        decision.scope_id,
    ) catch |e| exit.die(ctx, e, "audit publish-decision: resolving decision scope: {s}", .{@errorName(e)});
    defer if (entity_scope) |s| ctx.allocator.free(s);
    scope_mod.guardWithMembership(d, entity_scope, resolution.scope) catch
        exit.die(ctx, error.ScopeMismatch, "decision {d} belongs to a different scope", .{decision_id});

    const comment = buildDecisionComment(ctx.allocator, decision) catch |e|
        exit.die(ctx, e, "audit publish-decision: rendering comment: {s}", .{@errorName(e)});
    defer ctx.allocator.free(comment);
    const session_ref = loadSessionRef(d, ctx.allocator, decision.session_id) catch
        try ctx.allocator.dupe(u8, decision.created_at);
    defer ctx.allocator.free(session_ref);

    var posted: i64 = 0;
    publishTarget(ctx, d, "decision", decision_id, comment, session_ref, &posted) catch |e|
        exit.die(ctx, e, "audit publish-decision: direct target: {s}", .{@errorName(e)});

    var stmt = d.prepare(
        \\select to_kind, to_id
        \\from entity_links
        \\where from_kind = 'decision' and from_id = ?
        \\order by id
    ) catch |e| exit.die(ctx, e, "audit publish-decision: preparing targets: {s}", .{@errorName(e)});
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = decision_id }}) catch |e|
        exit.die(ctx, e, "audit publish-decision: binding targets: {s}", .{@errorName(e)});
    while (true) {
        switch (stmt.step() catch |e| exit.die(ctx, e, "audit publish-decision: reading targets: {s}", .{@errorName(e)})) {
            .done => break,
            .row => {
                const kind = stmt.columnTextAlloc(0, ctx.allocator) catch |e|
                    exit.die(ctx, e, "audit publish-decision: reading target kind: {s}", .{@errorName(e)});
                defer ctx.allocator.free(kind);
                publishTarget(ctx, d, kind, stmt.columnInt(1), comment, session_ref, &posted) catch |e|
                    exit.die(ctx, e, "audit publish-decision: linked target: {s}", .{@errorName(e)});
            },
        }
    }

    if (args.json) {
        try std.json.Stringify.value(.{
            .ok = true,
            .decision_id = decision_id,
            .comments_posted = posted,
        }, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("decision {d} published: {d} comment(s) posted\n", .{ decision_id, posted });
    }
}

fn publishTarget(
    ctx: *const runtime.Ctx,
    d: *db.sqlite.Db,
    kind_text: []const u8,
    entity_id: i64,
    comment: []const u8,
    session_ref: []const u8,
    posted: *i64,
) !void {
    const kind = engine.external.link.ExternalEntityKind.fromText(kind_text) orelse return;
    const links = try engine.external.link.linksForEntity(d, ctx.allocator, kind, entity_id);
    defer engine.external.link.deinitMany(links, ctx.allocator);

    for (links) |link| {
        const system = engine.external.system.showById(d, ctx.allocator, link.system_id) catch |e| {
            recordResult(d, link.id, false, @errorName(e));
            continue;
        };
        defer engine.external.system.deinit(system, ctx.allocator);

        var adapter = adapter_factory.build(ctx.allocator, ctx.io, &ctx.environ, system) catch |e| {
            recordResult(d, link.id, false, @errorName(e));
            continue;
        };
        defer {
            adapter.deinit();
            ctx.allocator.destroy(adapter);
        }

        const footer = try std.fmt.allocPrint(
            ctx.allocator,
            "— posted by planar (entity: {s}:{d}, session: {s}, link: ext:{d})",
            .{ kind_text, entity_id, session_ref, link.id },
        );
        defer ctx.allocator.free(footer);
        const body = try std.fmt.allocPrint(ctx.allocator, "{s}\n\n{s}", .{ comment, footer });
        defer ctx.allocator.free(body);

        const result = switch (adapter.kind) {
            .jira => adapter.jira_adapter.?.postComment(ctx.allocator, link.external_id, body),
            .github => adapter.github_adapter.?.postComment(ctx.allocator, link.external_id, body),
        };
        result catch |e| {
            recordResult(d, link.id, false, @errorName(e));
            continue;
        };
        recordResult(d, link.id, true, "decision-comment");
        posted.* += 1;
    }
}

fn buildDecisionComment(allocator: std.mem.Allocator, decision: engine.planning.decision.Decision) ![]u8 {
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();
    try out.writer.print("**Decision: {s}** [{s}]\n\n", .{ decision.title, @tagName(decision.status) });
    if (decision.body.len > 0) try out.writer.print("{s}\n\n", .{decision.body});
    if (decision.rationale) |rationale| {
        if (rationale.len > 0) try out.writer.print("_Rationale:_ {s}", .{rationale});
    }
    return try allocator.dupe(u8, out.written());
}

fn loadSessionRef(d: *db.sqlite.Db, allocator: std.mem.Allocator, session_id: ?i64) ![]const u8 {
    const id = session_id orelse return error.NotFound;
    var stmt = try d.prepare("select started_at from sessions where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = id }});
    return switch (try stmt.step()) {
        .done => error.NotFound,
        .row => try stmt.columnTextAlloc(0, allocator),
    };
}

fn recordResult(d: *db.sqlite.Db, link_id: i64, ok: bool, detail: []const u8) void {
    d.exec("begin immediate") catch return;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};
    _ = d.execParams(
        \\update external_links
        \\set last_synced_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'), last_sync_status = ?
        \\where id = ?
    , &.{ .{ .text = if (ok) "ok" else "error" }, .{ .int = link_id } }) catch return;
    _ = d.execParams(
        "insert into sync_events (link_id, direction, outcome, detail) values (?, 'push', ?, ?)",
        &.{ .{ .int = link_id }, .{ .text = if (ok) "ok" else "error" }, .{ .text = detail } },
    ) catch return;
    d.exec("commit") catch return;
    committed = true;
}
