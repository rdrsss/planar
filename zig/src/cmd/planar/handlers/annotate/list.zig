//! handlers/annotate/list — `planar annotate list [filters] [--json]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const render = @import("render.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var filter: engine.planning.annotation.ListFilter = .{
        .anchor_path = args.anchor_path,
        .plan_id = args.plan,
        .task_id = args.task,
        .vendor = args.vendor,
        .tag = args.tag,
        .scope = args.scope,
    };
    if (args.status) |s| {
        filter.status = engine.planning.annotation.Status.fromText(s) orelse
            exit.die(ctx, error.InvalidStatus, "unknown status '{s}'", .{s});
    }

    const items = engine.planning.annotation.list(d, ctx.allocator, filter) catch |e|
        exit.die(ctx, e, "annotate list: {s}", .{@errorName(e)});
    defer engine.planning.annotation.deinitMany(items, ctx.allocator);

    try render.emitList(ctx, items, args.json);
}
