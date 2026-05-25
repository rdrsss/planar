//! handlers/task/reopen — `planar task reopen <task-id> --reason <…> [--status <…>]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "reopen" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    const reason = args.reason orelse
        exit.die(ctx, error.InvalidInput, "--reason is required for reopen", .{});

    // Default to `todo` if --status not provided; otherwise parse.
    var new_status: engine.planning.task.Status = .todo;
    if (args.status) |s| {
        new_status = engine.planning.task.Status.fromText(s) orelse
            exit.die(ctx, error.InvalidStatus, "unknown status '{s}'", .{s});
    }

    const task = engine.planning.task.reopen(d, ctx.allocator, id, new_status, reason) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no task with id {d}", .{id}),
        else => exit.die(ctx, e, "task reopen: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.task, task, .{ .json = args.json });
}
