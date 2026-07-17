const cli = @import("cli");
const std = @import("std");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "lint" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const selected = @as(usize, @intFromBool(args.plan != null)) +
        @as(usize, @intFromBool(args.all)) +
        @as(usize, @intFromBool(args.path != null));
    if (selected != 1) {
        exit.die(ctx, error.InvalidInput, "choose exactly one lint target: <plan>, --all, or --path <file-or-directory>", .{});
    }

    const target = if (args.path) |path|
        try ctx.allocator.dupe(u8, path)
    else blk: {
        const root = engine.workbench.resolveRoot(ctx.allocator, ctx.environ) catch |err|
            exit.die(ctx, err, "resolving workbench root failed: {s}", .{@errorName(err)});
        if (args.all) break :blk root;

        const plan_arg = args.plan.?;
        const plan = common.resolvePlanArg(d, ctx.allocator, plan_arg) catch |err| switch (err) {
            error.InvalidInput => exit.die(ctx, err, "invalid plan '{s}'", .{plan_arg}),
            error.NotFound => exit.die(ctx, err, "plan not found: {s}", .{plan_arg}),
            else => exit.die(ctx, err, "resolving plan '{s}' failed: {s}", .{ plan_arg, @errorName(err) }),
        };
        defer plan.deinit(ctx.allocator);
        const feature_dir = common.resolveFeatureDirForPlan(d, ctx.allocator, root, plan.id) catch |err|
            exit.die(ctx, err, "resolving workbench tree for plan {d} failed: {s}", .{ plan.id, @errorName(err) });
        ctx.allocator.free(root);
        break :blk feature_dir;
    };
    defer ctx.allocator.free(target);

    const result = engine.workbench.lint.run(d, ctx.allocator, ctx.io, target) catch |err| switch (err) {
        error.FileNotFound => exit.die(ctx, err, "lint target not found: {s}", .{target}),
        error.InvalidInput => exit.die(ctx, err, "lint target must be a Markdown file or directory: {s}", .{target}),
        else => exit.die(ctx, err, "workbench lint failed for {s}: {s}", .{ target, @errorName(err) }),
    };
    defer engine.workbench.lint.deinitResult(ctx.allocator, result);

    if (args.json) {
        for (result.issues) |issue| {
            try std.json.Stringify.value(issue, .{}, ctx.stdout);
            try ctx.stdout.print("\n", .{});
        }
    } else {
        for (result.issues) |issue| {
            try ctx.stdout.print("{s}:{d}:\n  {s}[{s}]: {s}\n", .{
                issue.path,
                issue.line,
                @tagName(issue.severity),
                issue.code,
                issue.message,
            });
            if (issue.hint.len > 0) try ctx.stdout.print("  hint: {s}\n", .{issue.hint});
        }
        try ctx.stdout.print("{d} files scanned, {d} errors, {d} warnings.\n", .{
            result.files_scanned,
            result.errors,
            result.warnings,
        });
    }

    if (result.errors > 0 or result.warnings > 0) {
        exit.die(ctx, error.WorkbenchLintIssues, "workbench lint found {d} error(s) and {d} warning(s)", .{ result.errors, result.warnings });
    }
}
