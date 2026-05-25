//! handlers/scenario/show — `planar scenario show <scenario-id>`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "scenario", "show" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.scenario_id, 10) catch
        exit.die(ctx, error.InvalidInput, "scenario id must be an integer, got '{s}'", .{args.scenario_id});

    const s = engine.planning.scenario.show(d, ctx.allocator, id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no scenario with id {d}", .{id}),
        else => exit.die(ctx, e, "scenario show: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.scenario, s, .{ .json = args.json });
}
