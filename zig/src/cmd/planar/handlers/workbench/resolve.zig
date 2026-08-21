const cli = @import("cli");
const std = @import("std");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "resolve" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const event_id = std.fmt.parseInt(i64, args.event_id, 10) catch
        exit.die(ctx, error.InvalidInput, "event-id must be an integer, got '{s}'", .{args.event_id});
    if (event_id < 1) {
        exit.die(ctx, error.InvalidInput, "invalid event-id '{s}' (must be a positive integer)", .{args.event_id});
    }
    const prefer: engine.workbench.sync.ConflictResolution = if (std.mem.eql(u8, args.prefer, "fs"))
        .fs
    else if (std.mem.eql(u8, args.prefer, "db"))
        .db
    else
        exit.die(ctx, error.InvalidInput, "--prefer must be fs|db, got '{s}'", .{args.prefer});
    engine.workbench.sync.resolve(d, event_id, prefer) catch |e|
        exit.die(ctx, e, "workbench resolve failed: {s}", .{@errorName(e)});

    const prefer_txt: []const u8 = if (prefer == .fs) "fs" else "db";
    if (args.json) {
        try ctx.stdout.print("{{\"resolved\":true,\"event_id\":{d},\"prefer\":\"{s}\"}}\n", .{ event_id, prefer_txt });
        return;
    }
    try ctx.stdout.print("resolved conflict event {d} (preferred {s})\n", .{ event_id, prefer_txt });
}
