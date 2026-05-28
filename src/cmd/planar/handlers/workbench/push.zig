const cli = @import("cli");
const std = @import("std");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "push" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const plan = common.resolvePlanArg(d, ctx.allocator, args.plan) catch |e| switch (e) {
        error.InvalidInput => exit.die(ctx, e, "invalid plan '{s}'", .{args.plan}),
        error.NotFound => exit.die(ctx, e, "plan not found: {s}", .{args.plan}),
        else => exit.die(ctx, e, "resolving plan '{s}' failed: {s}", .{ args.plan, @errorName(e) }),
    };
    defer plan.deinit(ctx.allocator);

    const filter_mode = common.parseFilterMode(args.filter_mode) catch
        exit.die(ctx, error.InvalidInput, "invalid --filter-mode '{s}' (expected 'failures' or 'all')", .{args.filter_mode orelse ""});

    const summary = engine.workbench.sync.push(d, ctx.allocator, plan.id, filter_mode) catch |e|
        exit.die(ctx, e, "workbench push failed: {s}", .{@errorName(e)});
    defer engine.workbench.sync.deinitResult(ctx.allocator, summary);
    if (args.json) {
        try std.json.Stringify.value(summary, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try common.printSyncResult(ctx.stdout, plan.id, plan.slug, .push, "push", summary, args.verbose);
    }
    if (summary.conflicts > 0) {
        exit.die(ctx, error.Conflict, "{d} conflict(s) require 'workbench resolve <event-id> --prefer fs|db'", .{summary.conflicts});
    }
}
