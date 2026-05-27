//! handlers/release — `planar-agent release --claim <token> [--reason <text>]`
//!
//! Atomic: action.end(outcome=aborted) + task doing→todo + claim status=released.
//! Semantically distinct from `fail` — "graceful give-up without attempting"
//! vs "I tried and it failed". Same task-state effect; different claim
//! status (released vs aborted) and action outcome (aborted vs error).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const terminal_common = @import("terminal_common.zig");

const atomic = engine.runtime.agentactivity.atomic;

pub const verb: cli.Cmd = .{
    .name = "release",
    .desc = "Graceful give-up: task → todo, claim → released (vs fail's aborted).",
    .flags = &.{
        .{ .long = "--claim", .kind = .string, .required = true, .desc = "Claim token returned by pull/claim" },
        .{ .long = "--reason", .kind = .string, .desc = "Optional reason for releasing" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"release"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const result = atomic.releaseWork(d, ctx.allocator, args.claim, args.reason) catch |e|
        exit.die(ctx, e, "release: {s}", .{@errorName(e)});
    defer result.deinit(ctx.allocator);

    try terminal_common.emit(ctx, d, result, args.json);
}
