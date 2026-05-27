//! Handoff handler rendering helpers (text + JSON).
//!
//! Keep emission inline rather than going through `output.emit` because
//! handoff is operator-state with a hand-rolled JSON shape that mirrors
//! the Go binary's `handoffJSON`, not the engine pattern's `renderText`.

const std = @import("std");
const engine = @import("engine");
const runtime = @import("runtime");
const output = @import("../../output.zig");

pub fn emitOne(ctx: *const runtime.Ctx, h: engine.runtime.handoff.Handoff, json: bool) !void {
    if (json) {
        try writeJson(ctx, h);
        try ctx.stdout.print("\n", .{});
    } else {
        try writeText(ctx, h);
    }
}

pub fn emitList(ctx: *const runtime.Ctx, items: []const engine.runtime.handoff.Handoff, json: bool) !void {
    if (json) {
        // Newline-delimited JSON to match Go's `handoff list --json` shape.
        for (items) |h| {
            try writeJson(ctx, h);
            try ctx.stdout.print("\n", .{});
        }
        return;
    }
    if (items.len == 0) {
        try ctx.stdout.print("no handoffs\n", .{});
        return;
    }
    try ctx.stdout.print("{s:<4}  {s:<8}  {s:<11}  {s:<11}  {s}\n", .{
        "id", "snapshot", "from-vendor", "to-vendor", "status",
    });
    for (items) |h| {
        const to_v: []const u8 = h.to_vendor orelse "-";
        try ctx.stdout.print("{d:<4}  {d:<8}  {s:<11}  {s:<11}  {s}\n", .{
            @as(u64, @intCast(h.id)),
            @as(u64, @intCast(h.from_snapshot_id)),
            h.from_vendor,
            to_v,
            @tagName(h.status),
        });
    }
}

fn writeJson(ctx: *const runtime.Ctx, h: engine.runtime.handoff.Handoff) !void {
    try ctx.stdout.print("{{\"id\":{d},\"from_snapshot_id\":{d},\"from_vendor\":", .{
        h.id, h.from_snapshot_id,
    });
    try output.writeJsonString(ctx.stdout, h.from_vendor);
    try ctx.stdout.print(",\"status\":", .{});
    try output.writeJsonString(ctx.stdout, @tagName(h.status));
    try ctx.stdout.print(",\"created_at\":", .{});
    try output.writeJsonString(ctx.stdout, h.created_at);
    if (h.to_session_id) |sid| try ctx.stdout.print(",\"to_session_id\":{d}", .{sid});
    if (h.to_vendor) |v| {
        try ctx.stdout.print(",\"to_vendor\":", .{});
        try output.writeJsonString(ctx.stdout, v);
    }
    if (h.validated_at) |v| {
        try ctx.stdout.print(",\"validated_at\":", .{});
        try output.writeJsonString(ctx.stdout, v);
    }
    if (h.consumed_at) |v| {
        try ctx.stdout.print(",\"consumed_at\":", .{});
        try output.writeJsonString(ctx.stdout, v);
    }
    try ctx.stdout.print("}}", .{});
}

fn writeText(ctx: *const runtime.Ctx, h: engine.runtime.handoff.Handoff) !void {
    try ctx.stdout.print("handoff {d}  [{s}]\n", .{ @as(u64, @intCast(h.id)), @tagName(h.status) });
    try ctx.stdout.print("  from_snapshot: {d}\n", .{@as(u64, @intCast(h.from_snapshot_id))});
    try ctx.stdout.print("  from_vendor:   {s}\n", .{h.from_vendor});
    if (h.to_vendor) |v| try ctx.stdout.print("  to_vendor:     {s}\n", .{v});
    if (h.to_session_id) |sid| try ctx.stdout.print("  to_session:    {d}\n", .{@as(u64, @intCast(sid))});
    if (h.validated_at) |v| try ctx.stdout.print("  validated_at:  {s}\n", .{v});
    if (h.consumed_at) |v| try ctx.stdout.print("  consumed_at:   {s}\n", .{v});
    try ctx.stdout.print("  created_at:    {s}\n", .{h.created_at});
}
