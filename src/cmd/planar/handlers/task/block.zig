//! handlers/task/block — `planar task block <task-id> --on <blocker-id>`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "block" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    const task = engine.planning.task.markBlocked(d, ctx.allocator, id, args.on, args.reason) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no task with id {d}", .{id}),
        else => exit.die(ctx, e, "task block: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.task, task, .{ .json = args.json });
}
