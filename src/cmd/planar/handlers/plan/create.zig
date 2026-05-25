//! handlers/plan/create — `planar plan create <title> [--summary] [--slug] …`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "create" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const status = engine.planning.plan.Status.fromText(args.status) orelse
        exit.die(ctx, error.InvalidStatus, "unknown status '{s}'", .{args.status});

    const plan = engine.planning.plan.create(d, ctx.allocator, .{
        .title = args.title,
        .slug = args.slug,
        .summary = args.summary,
        .status = status,
        .parent_plan_id = args.parent,
        .scope = args.scope,
    }) catch |e| exit.die(ctx, e, "plan create: {s}", .{@errorName(e)});

    try output.emit(ctx, engine.planning.plan, plan, .{ .json = args.json });
}
