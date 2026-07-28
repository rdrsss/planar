//! handlers/task/packet — `planar task packet <task-id>`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "packet" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    var live = engine.routing.packet.assembleTask(ctx.allocator, d, id) catch |e| switch (e) {
        error.TaskNotFound => exit.die(ctx, e, "no task with id {d}", .{id}),
        else => exit.die(ctx, e, "task packet: {s}", .{@errorName(e)}),
    };
    defer live.deinit();

    if (args.json) {
        try std.json.Stringify.value(.{
            .input = live.packet.input,
            .canonical = live.packet.canonical,
            .digest = live.packet.digest[0..],
            .reasons = live.packet.reasons,
        }, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }
    try engine.routing.packet.renderText(live.packet, ctx.stdout);
}
