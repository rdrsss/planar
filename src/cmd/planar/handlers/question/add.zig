//! handlers/question/add — `planar question add <title> [--body --editor --scope]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "question", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    if (args.editor) {
        try ctx.stderr.print("warning: --editor not yet implemented; falling back to inline create\n", .{});
    }

    const q = engine.planning.question.create(d, ctx.allocator, .{
        .title = args.title,
        .body = args.body,
        .scope = args.scope,
        .plan_id = args.plan,
    }) catch |e| exit.die(ctx, e, "question add: {s}", .{@errorName(e)});

    try output.emit(ctx, engine.planning.question, q, .{ .json = args.json });
}
