//! handlers/capture/command — `planar capture command <body> [--outcome] [--session]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const util = @import("util.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "capture", "command" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const sid = util.resolveSessionId(ctx, d, args.session);

    var body_owned: ?[]u8 = null;
    defer if (body_owned) |b| ctx.allocator.free(b);
    const body: []const u8 = if (args.outcome) |out| blk: {
        const composed = try std.fmt.allocPrint(
            ctx.allocator,
            "{s}\noutcome: {s}",
            .{ args.command, out },
        );
        body_owned = composed;
        break :blk composed;
    } else args.command;

    engine.runtime.capture.appendCommand(d, sid, body) catch |e|
        exit.die(ctx, e, "capture command: {s}", .{@errorName(e)});

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"session_id\":{d}}}\n", .{sid});
    } else {
        try ctx.stdout.print("captured command in session {d}\n", .{@as(u64, @intCast(sid))});
    }
}
