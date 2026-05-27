//! handlers/annotate/remove — `planar annotate remove <id> [--json]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "remove" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.annotation_id, 10) catch
        exit.die(ctx, error.InvalidInput, "annotation id must be an integer, got '{s}'", .{args.annotation_id});

    engine.planning.annotation.remove(d, ctx.allocator, id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no annotation with id {d}", .{id}),
        else => exit.die(ctx, e, "annotate remove: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"id\":{d}}}\n", .{id});
    } else {
        try ctx.stdout.print("annotation {d} removed\n", .{id});
    }
}
