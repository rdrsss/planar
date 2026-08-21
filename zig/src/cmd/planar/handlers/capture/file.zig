//! handlers/capture/file — `planar capture file <path> [--role] [--session]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const util = @import("util.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "capture", "file" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const sid = util.resolveSessionId(ctx, d, args.session);

    var body_owned: ?[]u8 = null;
    defer if (body_owned) |b| ctx.allocator.free(b);
    const body: []const u8 = if (args.role) |role| blk: {
        const composed = try std.fmt.allocPrint(
            ctx.allocator,
            "{s} [{s}]",
            .{ args.path, role },
        );
        body_owned = composed;
        break :blk composed;
    } else args.path;

    engine.runtime.capture.appendFile(d, sid, body) catch |e|
        exit.die(ctx, e, "capture file: {s}", .{@errorName(e)});

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"session_id\":{d}}}\n", .{sid});
    } else {
        try ctx.stdout.print("captured file in session {d}\n", .{@as(u64, @intCast(sid))});
    }
}
