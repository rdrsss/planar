//! handlers/task/done — `planar task done <task-id>`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "done" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    const task = engine.planning.task.markDone(d, ctx.allocator, id, args.force) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no task with id {d}", .{id}),
        error.TaskClaimed => exit.die(
            ctx,
            e,
            "task {d} has an active work claim — operator status flip refused.\n" ++
                "Release or complete the claim via the agent path, or re-run with --force to override.",
            .{id},
        ),
        else => exit.die(ctx, e, "task done: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.task, task, .{ .json = args.json });
}
