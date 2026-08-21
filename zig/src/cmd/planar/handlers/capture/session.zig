//! handlers/capture/session — `planar capture session [--vendor] [--vendor-session-id] [--model] [--task]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "capture", "session" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Vendor defaults: --vendor flag > $PLANAR_VENDOR > "cli".
    var vendor: []const u8 = "cli";
    if (args.vendor) |v| {
        vendor = v;
    } else if (ctx.environ.getPosix("PLANAR_VENDOR")) |v| {
        if (v.len > 0) vendor = v;
    }
    var vsid: ?[]const u8 = args.vendor_session_id;
    if (vsid == null) {
        if (ctx.environ.getPosix("PLANAR_VENDOR_SESSION_ID")) |v| {
            if (v.len > 0) vsid = v;
        }
    }

    const s = engine.runtime.capture.openSession(d, ctx.allocator, ctx.io, .{
        .vendor = vendor,
        .vendor_session_id = vsid,
        .task_id = args.task,
        .model = args.model,
    }) catch |e| switch (e) {
        error.TaskConflict => exit.die(ctx, e, "session already bound to a different task", .{}),
        else => exit.die(ctx, e, "capture session: {s}", .{@errorName(e)}),
    };
    defer engine.runtime.session.deinit(s, ctx.allocator);

    if (args.json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"id\":{d},\"vendor\":\"{s}\"",
            .{ s.id, s.vendor },
        );
        if (s.vendor_session_id) |v| try ctx.stdout.print(",\"vendor_session_id\":\"{s}\"", .{v});
        if (s.task_id) |t| try ctx.stdout.print(",\"task_id\":{d}", .{t});
        try ctx.stdout.print("}}\n", .{});
        return;
    }
    try ctx.stdout.print("session {d} opened (vendor: {s}", .{ @as(u64, @intCast(s.id)), s.vendor });
    if (s.vendor_session_id) |v| try ctx.stdout.print(", vsid: {s}", .{v});
    if (s.task_id) |t| try ctx.stdout.print(", task: {d}", .{t});
    try ctx.stdout.print(")\n", .{});
}
