//! handlers/annotate/bulk_dismiss — dismiss every active annotation matching the filter.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const bulk = @import("bulk.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "bulk-dismiss" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const filter: engine.planning.annotation.ListFilter = .{
        .anchor_path = args.anchor_path,
        .plan_id = args.plan,
        .task_id = args.task,
        .vendor = args.vendor,
        .tag = args.tag,
        .scope = args.scope,
        .status = .active,
    };

    const count = bulk.apply(d, ctx, filter, .dismiss) catch |e|
        exit.die(ctx, e, "annotate bulk-dismiss: {s}", .{@errorName(e)});

    try bulk.emit(ctx, "dismissed", count, args.json);
}
