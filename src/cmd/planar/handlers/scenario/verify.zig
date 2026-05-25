//! handlers/scenario/verify — `planar scenario verify <scenario-id> [--summary]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "scenario", "verify" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.scenario_id, 10) catch
        exit.die(ctx, error.InvalidInput, "scenario id must be an integer, got '{s}'", .{args.scenario_id});

    const s = engine.planning.scenario.verify(d, ctx.allocator, id, args.summary) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no scenario with id {d}", .{id}),
        else => exit.die(ctx, e, "scenario verify: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.scenario, s, .{ .json = args.json });
}
