//! handlers/capture/end — `planar capture end [<session-id>] [--summary]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "capture", "end" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var sid: i64 = 0;
    if (args.session_id) |raw| {
        sid = std.fmt.parseInt(i64, raw, 10) catch
            exit.die(ctx, error.InvalidInput, "session id must be an integer, got '{s}'", .{raw});
    } else if (args.session) |s| {
        sid = s;
    } else {
        // Resolve active session from env.
        const vendor: []const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR")) |v|
            (if (v.len > 0) @as([]const u8, v) else "cli")
        else
            "cli";
        const vsid: ?[]const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR_SESSION_ID")) |v|
            (if (v.len > 0) @as([]const u8, v) else null)
        else
            null;
        const found = engine.runtime.session.activeForVendor(d, ctx.allocator, vendor, vsid) catch |e|
            exit.die(ctx, e, "finding active session: {s}", .{@errorName(e)});
        if (found) |s| {
            sid = s.id;
            engine.runtime.session.deinit(s, ctx.allocator);
        } else {
            exit.die(ctx, error.NotFound, "no active session", .{});
        }
    }

    engine.runtime.capture.closeSession(d, ctx.allocator, .{
        .session_id = sid,
        .summary = args.summary,
    }) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "session {d} not found", .{sid}),
        error.AlreadyEnded => exit.die(ctx, e, "session {d} is already ended", .{sid}),
        else => exit.die(ctx, e, "capture end: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"id\":{d}}}\n", .{sid});
    } else {
        try ctx.stdout.print("session {d} ended\n", .{@as(u64, @intCast(sid))});
    }
}
