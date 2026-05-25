const cli = @import("cli");
const std = @import("std");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "archive" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan = common.resolvePlanArg(d, ctx.allocator, args.plan) catch |e| switch (e) {
        error.InvalidInput => exit.die(ctx, e, "invalid plan '{s}'", .{args.plan}),
        error.NotFound => exit.die(ctx, e, "plan not found: {s}", .{args.plan}),
        else => exit.die(ctx, e, "resolving plan '{s}' failed: {s}", .{ args.plan, @errorName(e) }),
    };
    defer plan.deinit(ctx.allocator);

    const root = common.resolveAndEnsureWorkbenchRoot(ctx.allocator, ctx.io) catch |e|
        exit.die(ctx, e, "resolving workbench root failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    const feature_dir = engine.workbench.sync.archive(d, plan.id, root, ctx.allocator) catch |e|
        exit.die(ctx, e, "workbench archive failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(feature_dir);

    if (args.json) {
        try std.json.Stringify.value(.{
            .archived = true,
            .plan = plan.id,
            .feature_dir = feature_dir,
        }, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }
    try ctx.stdout.print("archived: {s} (plan {d})\n", .{ feature_dir, plan.id });
}
