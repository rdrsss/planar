//! handlers/handoff/consume — `planar handoff consume <handoff-id> [--session]`
//!
//! Transition pending|validated → consumed.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const render = @import("render.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "handoff", "consume" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.handoff_id, 10) catch
        exit.die(ctx, error.InvalidInput, "handoff id must be an integer, got '{s}'", .{args.handoff_id});

    const h = engine.runtime.handoff.consume(d, ctx.allocator, .{
        .id = id,
        .session_id = args.session,
    }) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "handoff {d} not found", .{id}),
        error.IllegalTransition => exit.die(ctx, e, "handoff {d} is terminal; cannot consume", .{id}),
        else => exit.die(ctx, e, "handoff consume: {s}", .{@errorName(e)}),
    };
    defer engine.runtime.handoff.deinit(h, ctx.allocator);

    try render.emitOne(ctx, h, args.json);
}
