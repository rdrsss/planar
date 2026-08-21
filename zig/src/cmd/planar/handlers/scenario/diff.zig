const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const editflow = @import("../../editflow.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "scenario", "diff" }, args_ptr);
    const ctx = runtime.current();
    const id = std.fmt.parseInt(i64, args.scenario_id, 10) catch
        exit.die(ctx, error.InvalidInput, "scenario id must be an integer, got '{s}'", .{args.scenario_id});
    if (id <= 0) {
        exit.die(ctx, error.InvalidInput, "scenario id must be a positive integer, got {d}", .{id});
    }
    const d = try runtime.ensureDb();
    editflow.diff(ctx, d, .scenario, id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no scenario with id {d}", .{id}),
        error.NoPlanLink => exit.die(ctx, e, "scenario {d} is not linked to a plan; cannot resolve anchor plan", .{id}),
        else => exit.die(ctx, e, "scenario diff failed: {s}", .{@errorName(e)}),
    };
}
