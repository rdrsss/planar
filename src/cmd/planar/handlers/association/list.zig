//! handlers/association/list — `planar assoc list [--kind]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "assoc", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var filter: engine.identity.association.ListFilter = .{};
    if (args.kind) |k| {
        filter.kind = engine.identity.association.Kind.fromText(k) orelse
            exit.die(ctx, error.InvalidInput, "unknown kind '{s}'", .{k});
    }

    const items = engine.identity.association.list(d, ctx.allocator, filter) catch |e|
        exit.die(ctx, e, "association list: {s}", .{@errorName(e)});

    try output.emitList(ctx, engine.identity.association, items, .{ .json = args.json });
}
