const cli = @import("cli");
const std = @import("std");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "status" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    if (args.plan == null) {
        const root = try resolveWorkbenchRoot(ctx.allocator);
        defer ctx.allocator.free(root);
        const items = engine.workbench.sync.listActive(d, ctx.allocator, root) catch |e|
            exit.die(ctx, e, "workbench status failed: {s}", .{@errorName(e)});
        defer engine.workbench.sync.freeActiveMany(ctx.allocator, items);

        var total = engine.workbench.sync.Summary{};
        var any_tree = false;
        for (items) |it| {
            if (!it.has_fs_tree) continue;
            any_tree = true;
            const s = engine.workbench.sync.status(d, ctx.allocator, it.plan_id) catch |e|
                exit.die(ctx, e, "workbench status failed for plan {d}: {s}", .{ it.plan_id, @errorName(e) });
            defer engine.workbench.sync.deinitResult(ctx.allocator, s);
            total.applied += s.applied;
            total.pending += s.pending;
            total.conflicts += s.conflicts;
            if (!args.json) {
                try common.printSyncResult(ctx.stdout, it.plan_id, it.slug, .status, "status", s, args.verbose);
            }
        }
        if (args.json) {
            try std.json.Stringify.value(total, .{}, ctx.stdout);
            try ctx.stdout.print("\n", .{});
            return;
        }
        if (!any_tree) {
            try ctx.stdout.print("no active features found\n", .{});
        }
        return;
    }
    const p = args.plan.?;
    const plan = common.resolvePlanArg(d, ctx.allocator, p) catch |e| switch (e) {
        error.InvalidInput => exit.die(ctx, e, "invalid plan '{s}'", .{p}),
        error.NotFound => exit.die(ctx, e, "plan not found: {s}", .{p}),
        else => exit.die(ctx, e, "resolving plan '{s}' failed: {s}", .{ p, @errorName(e) }),
    };
    defer plan.deinit(ctx.allocator);

    const summary = engine.workbench.sync.status(d, ctx.allocator, plan.id) catch |e|
        exit.die(ctx, e, "workbench status failed: {s}", .{@errorName(e)});
    defer engine.workbench.sync.deinitResult(ctx.allocator, summary);
    if (args.json) {
        try std.json.Stringify.value(summary, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }
    try common.printSyncResult(ctx.stdout, plan.id, plan.slug, .status, "status", summary, args.verbose);
}

fn resolveWorkbenchRoot(allocator: std.mem.Allocator) ![]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_WORKBENCH_ROOT=")) {
            return allocator.dupe(u8, s["PLANAR_WORKBENCH_ROOT=".len..]);
        }
    }
    var j: usize = 0;
    while (raw[j]) |entry| : (j += 1) {
        const s = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "HOME=")) {
            return std.fs.path.join(allocator, &.{ s["HOME=".len..], ".planar", "workbench" });
        }
    }
    return allocator.dupe(u8, ".");
}
