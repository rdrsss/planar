//! handlers/question/answer — `planar question answer <question-id> --answer <text>`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "question", "answer" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.question_id, 10) catch
        exit.die(ctx, error.InvalidInput, "question id must be an integer, got '{s}'", .{args.question_id});

    const answer_text = args.answer orelse
        exit.die(ctx, error.InvalidInput, "--answer is required", .{});

    const q = engine.planning.question.answer(d, ctx.allocator, id, answer_text) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no question with id {d}", .{id}),
        error.AnswerRequired => exit.die(ctx, e, "--answer must be non-empty", .{}),
        else => exit.die(ctx, e, "question answer: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.question, q, .{ .json = args.json });
}
