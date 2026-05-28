//! handlers/heartbeat — `planar-agent heartbeat --claim <token>`
//!
//! Refresh the lease (last_heartbeat_at = now, lease_expires_at = now + ttl).
//! Wraps in BEGIN IMMEDIATE per the engine's transactional invariants.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const json = @import("json.zig");

const store = engine.runtime.agentactivity.store;

pub const verb: cli.Cmd = .{
    .name = "heartbeat",
    .desc = "Refresh the lease on an active claim.",
    .flags = &.{
        .{ .long = "--claim", .kind = .string, .required = true, .desc = "Claim token to refresh" },
        .{ .long = "--ttl", .kind = .string, .default = .{ .string = "600" }, .desc = "New TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"heartbeat"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const ttl_secs = cli.duration.parseSeconds(args.ttl) catch |e|
        exit.die(ctx, e, "invalid --ttl '{s}': expected bare seconds (e.g. 600) or suffixed duration (e.g. 10m, 1h, 500ms)", .{args.ttl});

    d.exec("BEGIN IMMEDIATE") catch |e| exit.die(ctx, e, "BEGIN IMMEDIATE: {s}", .{@errorName(e)});

    const c = store.heartbeatClaim(d, ctx.allocator, args.claim, ttl_secs) catch |e| {
        d.exec("ROLLBACK") catch {};
        exit.die(ctx, e, "heartbeat: {s}", .{@errorName(e)});
    };
    defer c.deinit(ctx.allocator);

    d.exec("COMMIT") catch |e| exit.die(ctx, e, "COMMIT: {s}", .{@errorName(e)});

    if (args.json) {
        const w = ctx.stdout;
        try w.print("{{\"ok\":true,\"claim_token\":", .{});
        try std.json.Stringify.encodeJsonString(c.claim_token, .{}, w);
        try w.print(",\"claim\":", .{});
        try json.writeClaim(w, c);
        try w.print("}}\n", .{});
    } else {
        try ctx.stdout.print("ok claim:{s} expires:{s}\n", .{ c.claim_token, c.lease_expires_at });
    }
}
