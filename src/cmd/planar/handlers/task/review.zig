const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const editflow = @import("../../editflow.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "review" }, args_ptr);
    const ctx = runtime.current();
    const id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});
    if (id <= 0) {
        exit.die(ctx, error.InvalidInput, "task id must be a positive integer, got {d}", .{id});
    }
    if (args.approve and args.request_changes) {
        exit.die(ctx, error.InvalidInput, "--approve and --request-changes are mutually exclusive", .{});
    }

    const verdict: ?editflow.ReviewVerdict = if (args.approve)
        .approve
    else if (args.request_changes)
        .request_changes
    else
        null;

    const d = try runtime.ensureDb();
    editflow.review(ctx, d, .task, id, verdict, args.json) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no task with id {d}", .{id}),
        error.NoPlanLink => exit.die(ctx, e, "task {d} has no plan link; cannot resolve anchor plan", .{id}),
        else => exit.die(ctx, e, "task review failed: {s}", .{@errorName(e)}),
    };
}
