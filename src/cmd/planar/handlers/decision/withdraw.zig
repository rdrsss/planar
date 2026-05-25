//! handlers/decision/withdraw — `planar decision withdraw <decision-id> [--scope]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "decision", "withdraw" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.decision_id, 10) catch
        exit.die(ctx, error.InvalidInput, "decision id must be an integer, got '{s}'", .{args.decision_id});

    _ = args.scope;

    const dec = engine.planning.decision.withdraw(d, ctx.allocator, id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no decision with id {d}", .{id}),
        error.TerminalStatus => exit.die(ctx, e, "decision {d} is terminal; cannot withdraw", .{id}),
        else => exit.die(ctx, e, "decision withdraw: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.decision, dec, .{ .json = args.json });
}
