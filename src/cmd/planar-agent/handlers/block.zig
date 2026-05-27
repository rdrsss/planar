//! handlers/block — `planar-agent block --claim <token> --blocker <id>`
//!
//! Atomic: INSERT entity_links(blocks) + task → blocked +
//! action.end(outcome=aborted) + claim status=released.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const terminal_common = @import("terminal_common.zig");

const atomic = engine.runtime.agentactivity.atomic;

pub const verb: cli.Cmd = .{
    .name = "block",
    .desc = "Atomically park the task on an external blocker.",
    .flags = &.{
        .{ .long = "--claim", .kind = .string, .required = true, .desc = "Claim token returned by pull/claim" },
        .{ .long = "--blocker", .kind = .int, .required = true, .desc = "Task id of the blocker (entity_links target)" },
        .{ .long = "--reason", .kind = .string, .desc = "Free-text reason recorded on the claim" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"block"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const result = atomic.blockWork(d, ctx.allocator, args.claim, args.blocker, args.reason) catch |e|
        exit.die(ctx, e, "block: {s}", .{@errorName(e)});
    defer result.deinit(ctx.allocator);

    try terminal_common.emit(ctx, d, result, args.json);
}
