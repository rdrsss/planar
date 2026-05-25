//! handlers/handoff/list — `planar handoff list [--status]`
//!
//! Status filter parses comma-separated kinds (e.g. `--status pending,validated`).
//! Default (no filter) lists pending handoffs, matching Go behavior.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const render = @import("render.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "handoff", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var statuses: std.ArrayList(engine.runtime.handoff.Status) = .empty;
    defer statuses.deinit(ctx.allocator);

    if (args.status) |s| {
        var it = std.mem.splitScalar(u8, s, ',');
        while (it.next()) |tok| {
            const trimmed = std.mem.trim(u8, tok, " ");
            if (trimmed.len == 0) continue;
            const st = engine.runtime.handoff.Status.fromText(trimmed) orelse
                exit.die(ctx, error.InvalidInput, "unknown handoff status '{s}'", .{trimmed});
            try statuses.append(ctx.allocator, st);
        }
    }

    const list = engine.runtime.handoff.list(d, ctx.allocator, .{
        .statuses = statuses.items,
    }) catch |e| exit.die(ctx, e, "handoff list: {s}", .{@errorName(e)});
    defer engine.runtime.handoff.deinitMany(list, ctx.allocator);

    try render.emitList(ctx, list, args.json);
}
