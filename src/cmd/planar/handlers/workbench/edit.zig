const cli = @import("cli");
const std = @import("std");
const engine = @import("engine");
const editor = @import("../../editor.zig");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "edit" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan = common.resolvePlanArg(d, ctx.allocator, args.plan) catch |e| switch (e) {
        error.InvalidInput => exit.die(ctx, e, "invalid plan '{s}'", .{args.plan}),
        error.NotFound => exit.die(ctx, e, "plan not found: {s}", .{args.plan}),
        else => exit.die(ctx, e, "resolving plan '{s}' failed: {s}", .{ args.plan, @errorName(e) }),
    };
    defer plan.deinit(ctx.allocator);

    // `workbench edit` always uses the default failure-terminal filter and
    // never auto-cleans pre-existing terminal files; the editor-first flow
    // doesn't expose --filter-mode or --apply-cleanup.
    const push_result = engine.workbench.sync.push(d, ctx.allocator, plan.id, .failures, false) catch |e|
        exit.die(ctx, e, "workbench push failed: {s}", .{@errorName(e)});
    defer engine.workbench.sync.deinitResult(ctx.allocator, push_result);

    if (args.json) {
        try std.json.Stringify.value(push_result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try common.printSyncResult(ctx.stdout, plan.id, plan.slug, .push, "push", push_result, false);
    }
    if (push_result.conflicts > 0) {
        exit.die(ctx, error.Conflict, "{d} conflict(s) require 'workbench resolve <event-id> --prefer fs|db'", .{push_result.conflicts});
    }

    const root = common.resolveAndEnsureWorkbenchRoot(ctx.allocator, ctx.io) catch |e|
        exit.die(ctx, e, "resolving workbench root failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    const feature_dir = common.resolveFeatureDirForPlan(d, ctx.allocator, root, plan.id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "plan not found: {s}", .{args.plan}),
        else => exit.die(ctx, e, "resolving feature directory failed: {s}", .{@errorName(e)}),
    };
    defer ctx.allocator.free(feature_dir);

    const override = if (args.editor.len == 0) null else args.editor;
    const editor_cmd = editor.resolveEditor(ctx.allocator, override) catch |e|
        exit.die(ctx, e, "resolving editor failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(editor_cmd);

    const editor_status = runEditor(ctx.io, editor_cmd, feature_dir) catch |e|
        exit.die(ctx, e, "running editor failed: {s}", .{@errorName(e)});
    if (editor_status != 0) {
        exit.die(ctx, error.EditorFailed, "editor \"{s}\" exited with status {d}", .{ editor_cmd, editor_status });
    }

    const pull_result = engine.workbench.sync.pull(d, ctx.allocator, plan.id) catch |e|
        exit.die(ctx, e, "workbench pull failed: {s}", .{@errorName(e)});
    defer engine.workbench.sync.deinitResult(ctx.allocator, pull_result);

    if (args.json) {
        try std.json.Stringify.value(pull_result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try common.printSyncResult(ctx.stdout, plan.id, plan.slug, .pull, "pull", pull_result, false);
    }
    if (pull_result.conflicts > 0) {
        exit.die(ctx, error.Conflict, "{d} conflict(s) require 'workbench resolve <event-id> --prefer fs|db'", .{pull_result.conflicts});
    }
}

fn runEditor(io: std.Io, editor_cmd: []const u8, target_dir: []const u8) !u8 {
    var argv: [2][]const u8 = .{ editor_cmd, target_dir };
    var child = try std.process.spawn(io, .{
        .argv = &argv,
        .stdin = .inherit,
        .stdout = .inherit,
        .stderr = .inherit,
    });

    const term = try child.wait(io);
    return switch (term) {
        .exited => |code| code,
        else => 1,
    };
}
