//! handlers/task/update — `planar task update <task-id> [...]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "update" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    if (args.force) {
        try ctx.stderr.print("warning: --force not yet implemented; no constraint relaxation in effect\n", .{});
    }

    var patch: engine.planning.task.UpdateArgs = .{
        .title = args.title,
        .body = args.body,
        .priority = args.priority,
        .next_action = args.next_action,
        .due_at = args.due,
        .slug = args.slug,
        .no_auto_promote = args.no_auto_promote,
        .scope = args.scope,
    };
    if (args.plan) |pid| {
        if (pid == 0) {
            patch.clear_plan = true;
        } else {
            patch.plan_id = pid;
        }
    }
    if (args.status) |s| {
        patch.status = engine.planning.task.Status.fromText(s) orelse
            exit.die(ctx, error.InvalidStatus, "unknown status '{s}'", .{s});
    }

    const task = engine.planning.task.update(d, ctx.allocator, id, patch) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no task with id {d}", .{id}),
        else => exit.die(ctx, e, "task update: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.task, task, .{ .json = args.json });
}
