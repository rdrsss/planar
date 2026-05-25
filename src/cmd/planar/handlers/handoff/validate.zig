//! handlers/handoff/validate — `planar handoff validate <handoff-id>`
//!
//! Transition a pending handoff → validated.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const render = @import("render.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "handoff", "validate" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.handoff_id, 10) catch
        exit.die(ctx, error.InvalidInput, "handoff id must be an integer, got '{s}'", .{args.handoff_id});

    const h = engine.runtime.handoff.validate(d, ctx.allocator, id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "handoff {d} not found", .{id}),
        error.IllegalTransition => exit.die(ctx, e, "handoff {d} cannot transition to validated", .{id}),
        else => exit.die(ctx, e, "handoff validate: {s}", .{@errorName(e)}),
    };
    defer engine.runtime.handoff.deinit(h, ctx.allocator);

    try render.emitOne(ctx, h, args.json);
}
