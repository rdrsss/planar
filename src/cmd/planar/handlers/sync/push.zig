//! handlers/sync/push — `planar sync push <link-id> | --all [--system <slug>]`
//!
//! Push local state for one or more external links. Mirror of pull.zig but
//! through engine.external.sync.pushLink. Read-only links are surfaced as
//! a per-link line and skipped (matches Go's runSyncPush behavior).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");
const sync_common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "sync", "push" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    _ = args.scope;

    if (!args.all and args.ref == null) {
        exit.die(ctx, error.InvalidInput, "sync push requires <link-id>, <kind:id>, or --all", .{});
    }

    var links = resolveTargetLinks(d, ctx.allocator, args.all, args.ref) catch |e| switch (e) {
        error.InvalidInput => exit.die(ctx, e, "invalid sync target; expected <link-id> or <kind:id>", .{}),
        error.NotFound => exit.die(ctx, e, "sync target not found", .{}),
        else => exit.die(ctx, e, "sync push: resolve target: {s}", .{@errorName(e)}),
    };
    defer engine.external.link.deinitMany(links, ctx.allocator);

    if (args.system) |sys_slug| {
        const sys = engine.external.system.showBySlug(d, ctx.allocator, sys_slug) catch |e|
            exit.die(ctx, e, "sync push: system '{s}': {s}", .{ sys_slug, @errorName(e) });
        defer engine.external.system.deinit(sys, ctx.allocator);
        links = filterLinksBySystemId(ctx.allocator, links, sys.id) catch |e|
            exit.die(ctx, e, "sync push: filter links: {s}", .{@errorName(e)});
    }

    if (!args.json and links.len == 0) {
        try ctx.stdout.print("no links matched\n", .{});
        return;
    }
    var had_error = false;
    for (links) |link| {
        const sys = engine.external.system.showById(d, ctx.allocator, link.system_id) catch |e|
            exit.die(ctx, e, "sync push: system {d}: {s}", .{ link.system_id, @errorName(e) });
        defer engine.external.system.deinit(sys, ctx.allocator);

        var h = sync_common.handleForSystem(ctx.allocator, ctx.io, &ctx.environ, sys) catch |e|
            exit.die(ctx, e, "sync push: build adapter for {s}: {s}", .{ sys.slug, @errorName(e) });
        defer {
            h.deinit();
            ctx.allocator.destroy(h);
        }

        const result = sync_common.pushLink(d, ctx.allocator, link, h) catch |e| {
            had_error = true;
            if (args.json) {
                try emitResultJSON(ctx.stdout, link.id, .@"error", &.{}, @errorName(e));
            } else {
                try ctx.stdout.print("  link {d}: error — {s}\n", .{ link.id, @errorName(e) });
            }
            continue;
        };
        defer engine.external.sync.deinitPushResult(result, ctx.allocator);

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
        exit.die(ctx, error.SyncFailed, "one or more push errors", .{});
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

const KindID = struct {
    kind: engine.external.link.ExternalEntityKind,
    id: i64,
};

fn parseKindIDRef(s: []const u8) !KindID {
    var i: usize = s.len;
    while (i > 0) : (i -= 1) {
        if (s[i - 1] == ':') {
            const kind_text = s[0 .. i - 1];
            const id_text = s[i..];
            if (kind_text.len == 0 or id_text.len == 0) break;
            const kind = engine.external.link.ExternalEntityKind.fromText(kind_text) orelse break;
            const id = std.fmt.parseInt(i64, id_text, 10) catch break;
            return .{ .kind = kind, .id = id };
        }
    }
    return error.InvalidInput;
}

fn resolveTargetLinks(
    d: anytype,
    allocator: std.mem.Allocator,
    all: bool,
    ref_opt: ?[]const u8,
) ![]engine.external.link.ExtLink {
    if (all) {
        return engine.external.link.allPushable(d, allocator);
    }
    const ref = ref_opt orelse return error.InvalidInput;
    if (std.fmt.parseInt(i64, ref, 10)) |link_id| {
        const one = try engine.external.link.show(d, allocator, link_id);
        const out = try allocator.alloc(engine.external.link.ExtLink, 1);
        out[0] = one;
        return out;
    } else |_| {
        const kid = try parseKindIDRef(ref);
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
        } else {
            engine.external.link.deinit(lnk, allocator);
        }
    }
    allocator.free(links);
    return kept.toOwnedSlice(allocator);
}
