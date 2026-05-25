const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const editflow = @import("../../editflow.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "diff" }, args_ptr);
    const ctx = runtime.current();
    const id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});
    if (id <= 0) {
        exit.die(ctx, error.InvalidInput, "task id must be a positive integer, got {d}", .{id});
    }
    const d = try runtime.ensureDb();
    editflow.diff(ctx, d, .task, id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no task with id {d}", .{id}),
        error.NoPlanLink => exit.die(ctx, e, "task {d} has no plan link; cannot resolve anchor plan", .{id}),
        else => exit.die(ctx, e, "task diff failed: {s}", .{@errorName(e)}),
    };
}
