//! handlers/annotate/bulk_resolve — `planar annotate bulk-resolve [filter flags] [--json]`
//!
//! Resolves every `active` annotation matching the filter. Filter
//! shape mirrors `annotate list`. Returns the count of annotations
//! that transitioned to `resolved`.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const bulk = @import("bulk.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "bulk-resolve" }, args_ptr);
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

    const count = bulk.apply(d, ctx, filter, .resolve) catch |e|
        exit.die(ctx, e, "annotate bulk-resolve: {s}", .{@errorName(e)});

    try bulk.emit(ctx, "resolved", count, args.json);
}
