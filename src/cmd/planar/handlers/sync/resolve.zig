//! handlers/sync/resolve — `planar sync resolve <event-id> --keep <local|remote>`
//!
//! Settle a sync conflict. Loads the sync_events row referenced by event-id,
//! looks up the link + system, builds the adapter, then drives
//! engine.external.sync.resolveConflict.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");
const adapter_factory = @import("../ext/adapter_factory.zig");
const sync_common = @import("common.zig");

const ResolveJSON = struct {
    ok: bool,
    event_id: i64,
    new_event_id: i64,
    keep: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "sync", "resolve" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const event_id = std.fmt.parseInt(i64, args.event_id, 10) catch
        exit.die(ctx, error.InvalidInput, "invalid event-id '{s}'", .{args.event_id});

    const keep: engine.external.sync.ResolveKeep = if (std.mem.eql(u8, args.keep, "local"))
        .local
    else if (std.mem.eql(u8, args.keep, "remote"))
        .remote
    else
        exit.die(ctx, error.InvalidInput, "--keep must be 'local' or 'remote', got '{s}'", .{args.keep});

    // Resolve the link from the event so we can build the right adapter.
    const link_id = readLinkIdForEvent(d, event_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "sync event {d} not found", .{event_id}),
        else => exit.die(ctx, e, "sync resolve: load event: {s}", .{@errorName(e)}),
    };
    const link = engine.external.link.show(d, ctx.allocator, link_id) catch |e|
        exit.die(ctx, e, "sync resolve: load link {d}: {s}", .{ link_id, @errorName(e) });
    defer engine.external.link.deinit(link, ctx.allocator);

    const sys = engine.external.system.showById(d, ctx.allocator, link.system_id) catch |e|
        exit.die(ctx, e, "sync resolve: system {d}: {s}", .{ link.system_id, @errorName(e) });
    defer engine.external.system.deinit(sys, ctx.allocator);

    var h = adapter_factory.build(ctx.allocator, ctx.io, &ctx.environ, sys) catch |e|
        exit.die(ctx, e, "sync resolve: build adapter: {s}", .{@errorName(e)});
    defer {
        h.deinit();
        ctx.allocator.destroy(h);
    }

    const result = sync_common.resolveConflict(d, ctx.allocator, event_id, keep, h) catch |e| switch (e) {
        error.NotConflict => exit.die(ctx, error.InvalidInput, "sync event {d} is not a conflict; nothing to resolve", .{event_id}),
        error.AdapterFailed => exit.die(ctx, e, "sync resolve: adapter call failed", .{}),
        else => exit.die(ctx, e, "sync resolve: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        const out = ResolveJSON{
            .ok = result.ok,
            .event_id = event_id,
            .new_event_id = result.new_event_id,
            .keep = @tagName(keep),
        };
        try std.json.Stringify.value(out, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print(
            "resolved sync event {d} (kept {s}); new event {d}\n",
            .{ event_id, @tagName(keep), result.new_event_id },
        );
    }
}

fn readLinkIdForEvent(d: anytype, event_id: i64) !i64 {
    var stmt = try d.prepare("select link_id from sync_events where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = event_id }});
    return switch (try stmt.step()) {
        .done => error.NotFound,
        .row => stmt.columnInt(0),
    };
}

comptime {
    _ = output;
}
