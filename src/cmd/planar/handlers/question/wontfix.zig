//! handlers/question/wontfix — `planar question wontfix <question-id> [--reason]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "question", "wontfix" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.question_id, 10) catch
        exit.die(ctx, error.InvalidInput, "question id must be an integer, got '{s}'", .{args.question_id});

    const q = engine.planning.question.wontfix(d, ctx.allocator, id, args.reason) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no question with id {d}", .{id}),
        else => exit.die(ctx, e, "question wontfix: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.question, q, .{ .json = args.json });
}
