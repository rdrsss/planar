//! handlers/plan/edit — `planar plan edit <plan-id> [--no-pull]`
//!
//! Thin shim: parse positional + flags, delegate to editflow.edit.

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const editflow = @import("../../editflow.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "edit" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan id must be an integer, got '{s}'", .{args.plan_id});

    try editflow.edit(ctx, d, .plan, id, .{});
}
