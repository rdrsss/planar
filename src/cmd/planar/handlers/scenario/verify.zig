//! handlers/scenario/verify — `planar scenario verify <scenario-id> [--outcome --summary]`

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

    // `--outcome` defaults to `pass` for backward compatibility
    // with the original single-purpose verb signature.
    const outcome_text = args.outcome orelse "pass";
    const outcome = engine.planning.scenario.Outcome.fromText(outcome_text) orelse
        exit.die(ctx, error.InvalidInput, "unknown outcome '{s}' (want pass|fail|error|skipped)", .{outcome_text});

    const s = engine.planning.scenario.verify(d, ctx.allocator, id, outcome, args.summary) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no scenario with id {d}", .{id}),
        else => exit.die(ctx, e, "scenario verify: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.scenario, s, .{ .json = args.json });
}
