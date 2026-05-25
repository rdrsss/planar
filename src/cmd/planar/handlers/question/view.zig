//! handlers/question/view — `planar question view <question-id>`
//!
//! Thin shim: parse positional, delegate to editflow.view.

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const editflow = @import("../../editflow.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "question", "view" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.question_id, 10) catch
        exit.die(ctx, error.InvalidInput, "question id must be an integer, got '{s}'", .{args.question_id});

    try editflow.view(ctx, d, .question, id);
}
