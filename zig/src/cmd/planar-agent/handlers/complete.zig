//! handlers/complete — `planar-agent complete --claim <token>`
//!
//! Atomic: action.end(outcome=ok) + task doing→done + claim status=completed.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const terminal_common = @import("terminal_common.zig");

const atomic = engine.runtime.agentactivity.atomic;

pub const verb: cli.Cmd = .{
    .name = "complete",
    .desc = "Atomically end the work session: task → done, claim → completed.",
    .flags = &.{
        .{ .long = "--claim", .kind = .string, .required = true, .desc = "Claim token returned by pull/claim" },
        .{ .long = "--summary", .kind = .string, .desc = "Free-text completion summary recorded on the action" },
        .{ .long = "--no-locality-probe", .kind = .bool, .default = .{ .bool = false }, .desc = "Skip the git locality probe and commit collection" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"complete"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const result = atomic.completeWork(d, ctx.allocator, args.claim, args.summary) catch |e|
        exit.die(ctx, e, "complete: {s}", .{@errorName(e)});
    defer result.deinit(ctx.allocator);

    terminal_common.collectCommits(ctx, d, result, args.no_locality_probe);
    try terminal_common.emit(ctx, d, result, args.json);
}
