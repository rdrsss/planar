//! handlers/sync/pull — `planar sync pull <link-id> | --all [--system <slug>]`
//!
//! Pull remote state for one or more external links. For each link, the
//! adapter is built from the system row and the engine's sync.pullLink is
//! invoked. Writes a sync_event per link (handled inside the engine).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");
const sync_common = @import("common.zig");
const scope_mod = @import("../../scope.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "sync", "pull" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    if (!args.all and args.ref == null) {
        exit.die(ctx, error.InvalidInput, "sync pull requires <link-id>, <kind:id>, or --all", .{});
    }

    var links = resolveTargetLinks(d, ctx.allocator, args.all, args.ref) catch |e| switch (e) {
        error.InvalidInput => exit.die(ctx, e, "invalid sync target; expected <link-id> or <kind:id>", .{}),
        error.NotFound => exit.die(ctx, e, "sync target not found", .{}),
        else => exit.die(ctx, e, "sync pull: resolve target: {s}", .{@errorName(e)}),
    };
    defer engine.external.link.deinitMany(links, ctx.allocator);

    if (!args.all) {
        const resolution = scope_mod.resolveForWrite(ctx, args.scope) catch |e|
            exit.die(ctx, e, "sync pull: resolving write scope: {s}", .{@errorName(e)});
        for (links) |link| {
            const entity_scope = sync_common.entityScopeSlug(d, ctx.allocator, link) catch |e|
                exit.die(ctx, e, "sync pull: resolving entity scope: {s}", .{@errorName(e)});
            defer if (entity_scope) |s| ctx.allocator.free(s);
            scope_mod.guardWithMembership(d, entity_scope, resolution.scope) catch
                exit.die(ctx, error.ScopeMismatch, "sync pull target is outside the operator write scope", .{});
        }
    }

    if (args.system) |sys_slug| {
        const sys = engine.external.system.showBySlug(d, ctx.allocator, sys_slug) catch |e|
            exit.die(ctx, e, "sync pull: system '{s}': {s}", .{ sys_slug, @errorName(e) });
        defer engine.external.system.deinit(sys, ctx.allocator);
        links = filterLinksBySystemId(ctx.allocator, links, sys.id) catch |e|
            exit.die(ctx, e, "sync pull: filter links: {s}", .{@errorName(e)});
    }

    if (!args.json and links.len == 0) {
        try ctx.stdout.print("no links matched\n", .{});
        return;
    }

    var had_error = false;
    var conflict_seen = false;

    for (links) |link| {
        const sys = engine.external.system.showById(d, ctx.allocator, link.system_id) catch |e|
            exit.die(ctx, e, "sync pull: system {d}: {s}", .{ link.system_id, @errorName(e) });
        defer engine.external.system.deinit(sys, ctx.allocator);

        var h = sync_common.handleForSystem(ctx.allocator, ctx.io, &ctx.environ, sys) catch |e|
            exit.die(ctx, e, "sync pull: build adapter for {s}: {s}", .{ sys.slug, @errorName(e) });
        defer {
            h.deinit();
            ctx.allocator.destroy(h);
        }

        const result = sync_common.pullLink(d, ctx.allocator, link, h) catch |e| {
            had_error = true;
            if (args.json) {
                try emitResultJSON(ctx.stdout, link.id, .@"error", &.{}, @errorName(e));
            } else {
                try ctx.stdout.print("  link {d}: error — {s}\n", .{ link.id, @errorName(e) });
            }
            continue;
        };
        defer engine.external.sync.deinitPullResult(result, ctx.allocator);
        if (result.outcome == .conflict) conflict_seen = true;

        if (args.json) {
            try emitResultJSON(ctx.stdout, link.id, result.outcome, result.fields_changed, result.detail);
        } else {
            try ctx.stdout.print("  link {d}: {s}", .{ link.id, @tagName(result.outcome) });
            if (result.fields_changed.len > 0) {
                try ctx.stdout.print(" —", .{});
                for (result.fields_changed) |f| try ctx.stdout.print(" {s}", .{f});
            }
            if (result.detail.len > 0) {
                try ctx.stdout.print(" — {s}", .{result.detail});
            }
            try ctx.stdout.print("\n", .{});
        }
    }

    if (had_error) {
        exit.die(ctx, error.SyncFailed, "one or more pull errors", .{});
    }
    if (conflict_seen and !had_error) {
        exit.die(ctx, error.Conflict, "one or more sync conflicts; use 'sync resolve' to settle", .{});
    }
}

fn emitResultJSON(
    w: *std.Io.Writer,
    link_id: i64,
    outcome: engine.external.sync.Outcome,
    fields_changed: []const []const u8,
    detail: []const u8,
) !void {
    try w.print("{{\"link_id\":{d},\"outcome\":", .{link_id});
    try output.writeJsonString(w, @tagName(outcome));
    try w.print(",\"fields_changed\":[", .{});
    for (fields_changed, 0..) |f, i| {
        if (i > 0) try w.print(",", .{});
        try output.writeJsonString(w, f);
    }
    try w.print("]", .{});
    if (detail.len > 0) {
        try w.print(",\"detail\":", .{});
        try output.writeJsonString(w, detail);
    }
    try w.print("}}\n", .{});
}

fn resolveTargetLinks(
    d: anytype,
    allocator: std.mem.Allocator,
    all: bool,
    ref_opt: ?[]const u8,
) ![]engine.external.link.ExtLink {
    if (all) {
        return engine.external.link.allPullable(d, allocator);
    }
    const ref = ref_opt orelse return error.InvalidInput;
    if (std.fmt.parseInt(i64, ref, 10)) |link_id| {
        const one = try engine.external.link.show(d, allocator, link_id);
        const out = try allocator.alloc(engine.external.link.ExtLink, 1);
        out[0] = one;
        return out;
    } else |_| {
        const kid = try sync_common.parseKindIDRef(ref);
        return engine.external.link.linksForEntity(d, allocator, kid.kind, kid.id);
    }
}

fn filterLinksBySystemId(
    allocator: std.mem.Allocator,
    links: []engine.external.link.ExtLink,
    system_id: i64,
) ![]engine.external.link.ExtLink {
    var kept: std.ArrayList(engine.external.link.ExtLink) = .empty;
    errdefer kept.deinit(allocator);
    for (links) |lnk| {
        if (lnk.system_id == system_id) {
            try kept.append(allocator, lnk);
        }
    }
    for (links) |lnk| if (lnk.system_id != system_id) engine.external.link.deinit(lnk, allocator);
    allocator.free(links);
    return kept.toOwnedSlice(allocator);
}
