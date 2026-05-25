//! handlers/decision/accept — `planar decision accept <decision-id> [--scope]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "decision", "accept" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.decision_id, 10) catch
        exit.die(ctx, error.InvalidInput, "decision id must be an integer, got '{s}'", .{args.decision_id});

    // --scope is accepted for CLI parity; first-cut decisions are global,
    // so engine ignores it. Surfacing UnsupportedScope here would be
    // surprising — that gate is only meaningful on `create` for now.
    _ = args.scope;

    const dec = engine.planning.decision.accept(d, ctx.allocator, id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no decision with id {d}", .{id}),
        error.TerminalStatus => exit.die(ctx, e, "decision {d} is terminal; cannot accept", .{id}),
        else => exit.die(ctx, e, "decision accept: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.decision, dec, .{ .json = args.json });
}
