//! handlers/capture/note — `planar capture note <body> [--session]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const util = @import("util.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "capture", "note" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const sid = util.resolveSessionId(ctx, d, args.session);
    engine.runtime.capture.appendNote(d, sid, args.body) catch |e|
        exit.die(ctx, e, "capture note: {s}", .{@errorName(e)});
    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"session_id\":{d}}}\n", .{sid});
    } else {
        try ctx.stdout.print("captured note in session {d}\n", .{@as(u64, @intCast(sid))});
    }
}
