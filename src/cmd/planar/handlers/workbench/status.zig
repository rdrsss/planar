const cli = @import("cli");
const std = @import("std");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "status" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    if (args.plan == null) {
        const root = try engine.workbench.resolveRoot(ctx.allocator, ctx.environ);
        defer ctx.allocator.free(root);
        const items = engine.workbench.sync.listActive(d, ctx.allocator, root) catch |e|
            exit.die(ctx, e, "workbench status failed: {s}", .{@errorName(e)});
        defer engine.workbench.sync.freeActiveMany(ctx.allocator, items);

        var total = engine.workbench.sync.Summary{};
        var malformed_files: std.ArrayList(engine.workbench.sync.MalformedFile) = .empty;
        defer {
            for (malformed_files.items) |file| {
                ctx.allocator.free(file.path);
                ctx.allocator.free(file.parse_error);
            }
            malformed_files.deinit(ctx.allocator);
        }
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
            total.malformed += s.malformed;
            for (s.malformed_files) |file| {
                const path = try ctx.allocator.dupe(u8, file.path);
                errdefer ctx.allocator.free(path);
                const parse_error = try ctx.allocator.dupe(u8, file.parse_error);
                errdefer ctx.allocator.free(parse_error);
                try malformed_files.append(ctx.allocator, .{
                    .path = path,
                    .parse_error = parse_error,
                });
            }
            if (!args.json) {
                try common.printSyncResult(ctx.stdout, it.plan_id, it.slug, .status, "status", s, args.verbose);
            }
        }
        if (args.json) {
            try std.json.Stringify.value(.{
                .applied = total.applied,
                .pending = total.pending,
                .conflicts = total.conflicts,
                .malformed = total.malformed,
                .malformed_files = malformed_files.items,
                .filtered = total.filtered,
                .pre_existing_terminal = total.pre_existing_terminal,
                .cleaned = total.cleaned,
            }, .{}, ctx.stdout);
            try ctx.stdout.print("\n", .{});
        }
        if (!args.json and !any_tree) {
            try ctx.stdout.print("no active features found\n", .{});
        }
        if (total.malformed > 0) {
            exit.die(ctx, error.MalformedWorkbench, "{d} malformed workbench file(s); run 'planar workbench lint --all' for details", .{total.malformed});
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
    } else {
        try common.printSyncResult(ctx.stdout, plan.id, plan.slug, .status, "status", summary, args.verbose);
    }
    if (summary.malformed > 0) {
        exit.die(ctx, error.MalformedWorkbench, "{d} malformed workbench file(s); run 'planar workbench lint {d}' for details", .{ summary.malformed, plan.id });
    }
}
