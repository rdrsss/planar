//! handlers/decision/list — `planar decision list [--scope --status]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "decision", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var filter: engine.planning.decision.ListFilter = .{ .scope = args.scope };
    if (args.status) |s| {
        filter.status = engine.planning.decision.Status.fromText(s) orelse
            exit.die(ctx, error.InvalidInput, "unknown status '{s}'", .{s});
    }

    const items = engine.planning.decision.list(d, ctx.allocator, filter) catch |e|
        exit.die(ctx, e, "decision list: {s}", .{@errorName(e)});

    try output.emitList(ctx, engine.planning.decision, items, .{ .json = args.json });
}
