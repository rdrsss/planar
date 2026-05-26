//! handlers/annotate/bulk_archive — archive every annotation matching
//! the filter (any status; archive is the "clear the desk" terminal).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const bulk = @import("bulk.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "bulk-archive" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // bulk-archive intentionally drops the status filter: archiving
    // from resolved or dismissed is legitimate "tidy up" operator
    // intent. The engine's archive helper handles any source status
    // except `archived` (which already-archived rows skip via the
    // bulk helper's same-state filter).
    const filter: engine.planning.annotation.ListFilter = .{
        .anchor_path = args.anchor_path,
        .plan_id = args.plan,
        .task_id = args.task,
        .vendor = args.vendor,
        .tag = args.tag,
        .scope = args.scope,
    };

    const count = bulk.apply(d, ctx, filter, .archive) catch |e|
        exit.die(ctx, e, "annotate bulk-archive: {s}", .{@errorName(e)});

    try bulk.emit(ctx, "archived", count, args.json);
}
