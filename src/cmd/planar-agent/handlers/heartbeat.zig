//! handlers/heartbeat — `planar-agent heartbeat --claim <token> [--status <text>]`
//!
//! Refresh the lease (last_heartbeat_at = now, lease_expires_at = now + ttl).
//! When --status is provided, also inserts a closed heartbeat action row with
//! the supplied text in the `summary` column so `planar-watch ps` can surface
//! current activity. When --status is omitted no action row is written
//! (current behavior preserved bit-for-bit).
//!
//! Wraps in BEGIN IMMEDIATE per the engine's transactional invariants.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const json = @import("json.zig");

const store = engine.runtime.agentactivity.store;
const types = engine.runtime.agentactivity.types;

pub const verb: cli.Cmd = .{
    .name = "heartbeat",
    .desc = "Refresh the lease on an active claim.",
    .flags = &.{
        .{ .long = "--claim", .kind = .string, .required = true, .desc = "Claim token to refresh" },
        .{ .long = "--ttl", .kind = .string, .default = .{ .string = "600" }, .desc = "New TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)" },
        .{ .long = "--status", .kind = .string, .desc = "Free-text status string recorded on the heartbeat action row's summary column" },
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

    // When --status is provided, insert a closed heartbeat action row so the
    // activity stream carries meaningful text. Omitting --status preserves the
    // current behavior (no action row written). An explicit --status "" is a
    // distinct value from omission: it writes an action row with an empty
    // string in summary (not NULL).
    if (args.status) |status_text| {
        const action_id = store.startAction(d, ctx.allocator, .{
            .session_id = c.session_id,
            .claim_id = c.id,
            .action_kind = .heartbeat,
            .vendor = c.vendor,
        }) catch |e| {
            d.exec("ROLLBACK") catch {};
            exit.die(ctx, e, "heartbeat: startAction: {s}", .{@errorName(e)});
        };
        store.endAction(d, ctx.allocator, action_id, .ok, status_text) catch |e| {
            d.exec("ROLLBACK") catch {};
            exit.die(ctx, e, "heartbeat: endAction: {s}", .{@errorName(e)});
        };
    }

    d.exec("COMMIT") catch |e| exit.die(ctx, e, "COMMIT: {s}", .{@errorName(e)});

    if (args.json) {
        const w = ctx.stdout;
        try w.print("{{\"ok\":true,\"claim_token\":", .{});
        try std.json.Stringify.encodeJsonString(c.claim_token, .{}, w);
        try w.print(",\"claim\":", .{});
        try json.writeClaim(w, c, null);
        try w.print("}}\n", .{});
    } else {
        try ctx.stdout.print("ok claim:{s} expires:{s}\n", .{ c.claim_token, c.lease_expires_at });
    }
}
