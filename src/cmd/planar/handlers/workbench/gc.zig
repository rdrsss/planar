const cli = @import("cli");
const std = @import("std");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "gc" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const filter_mode = common.parseFilterMode(args.filter_mode) catch
        exit.die(ctx, error.InvalidInput, "invalid --filter-mode '{s}' (expected 'failures' or 'all')", .{args.filter_mode orelse ""});

    const root = common.resolveAndEnsureWorkbenchRoot(ctx.allocator, ctx.environ, ctx.io) catch |e|
        exit.die(ctx, e, "resolving workbench root failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    const opts: engine.workbench.gc.Options = .{
        .dry_run = args.dry_run,
        .yes = args.yes,
        .filter_mode = filter_mode,
        .all_scopes = args.all_scopes,
    };

    var summary: engine.workbench.gc.Summary = if (args.all_scopes)
        try engine.workbench.gc.runAllScopes(d, ctx.allocator, root, opts)
    else blk: {
        const plan_arg = args.plan orelse exit.die(ctx, error.InvalidInput, "plan argument required unless --all-scopes is set", .{});
        const plan = common.resolvePlanArg(d, ctx.allocator, plan_arg) catch |e| switch (e) {
            error.NotFound => exit.die(ctx, e, "plan not found: {s}", .{plan_arg}),
            else => exit.die(ctx, e, "resolving plan '{s}' failed: {s}", .{ plan_arg, @errorName(e) }),
        };
        defer plan.deinit(ctx.allocator);
        break :blk try engine.workbench.gc.runForPlan(d, ctx.allocator, plan.id, root, opts);
    };
    defer summary.deinit(ctx.allocator);

    if (summary.drifted_skipped > 0 and !args.yes) {
        try ctx.stderr.print(
            "workbench gc: refused to remove {d} file(s) with FS-content drift from DB; re-run with --yes to discard, or 'workbench pull' first\n",
            .{summary.drifted_skipped},
        );
        for (summary.drifted_paths) |p| {
            try ctx.stderr.print("  drift: {s}\n", .{p});
        }
        std.process.exit(1);
    }

    if (args.json) {
        try ctx.stdout.print(
            "{{\"removed\":{d},\"kept\":{d},\"drifted_skipped\":{d},\"errors\":{d},\"dry_run\":{},\"filter_mode\":\"{s}\"}}\n",
            .{ summary.removed, summary.kept, summary.drifted_skipped, summary.errors, args.dry_run, @tagName(filter_mode) },
        );
    } else {
        if (args.dry_run) {
            try ctx.stdout.print(
                "workbench gc (--dry-run): would remove {d}, keep {d}, drifted-skipped {d}, errors {d} (mode={s})\n",
                .{ summary.removed, summary.kept, summary.drifted_skipped, summary.errors, @tagName(filter_mode) },
            );
        } else {
            try ctx.stdout.print(
                "workbench gc: removed {d}, kept {d}, drifted-skipped {d}, errors {d} (mode={s})\n",
                .{ summary.removed, summary.kept, summary.drifted_skipped, summary.errors, @tagName(filter_mode) },
            );
        }
    }
}
