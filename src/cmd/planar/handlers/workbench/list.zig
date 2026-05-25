const cli = @import("cli");
const std = @import("std");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const root = common.resolveAndEnsureWorkbenchRoot(ctx.allocator, ctx.io) catch |e|
        exit.die(ctx, e, "resolving workbench root failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);
    const items = engine.workbench.sync.listActive(d, ctx.allocator, root) catch |e|
        exit.die(ctx, e, "listing features failed: {s}", .{@errorName(e)});
    defer engine.workbench.sync.freeActiveMany(ctx.allocator, items);

    if (args.json) {
        try ctx.stdout.print("[", .{});
        var first = true;
        for (items) |it| {
            if (!first) try ctx.stdout.print(",", .{});
            first = false;
            try std.json.Stringify.value(.{
                .plan = it.plan_id,
                .slug = it.slug,
                .status = it.status,
                .assoc = it.assoc_slug,
                .plan_key = it.plan_key,
                .has_fs_tree = it.has_fs_tree,
            }, .{}, ctx.stdout);
        }
        try ctx.stdout.print("]\n", .{});
        return;
    }

    if (items.len == 0) {
        try ctx.stdout.print("no features found\n", .{});
        return;
    }

    for (items) |it| {
        const tree = if (it.has_fs_tree) "tree" else "no-tree";
        const plan_slug = try std.fmt.allocPrint(ctx.allocator, "{s}-{s}", .{ it.plan_key, it.slug });
        defer ctx.allocator.free(plan_slug);
        try ctx.stdout.print("{s:<40}  {s:<10}  {s:<10}  {s}\n", .{
            plan_slug,
            it.status,
            tree,
            it.assoc_slug,
        });
    }
}
