//! handlers/annotate/tag — `planar annotate tag <id> <tag> [--remove] [--json]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "tag" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.annotation_id, 10) catch
        exit.die(ctx, error.InvalidInput, "annotation id must be an integer, got '{s}'", .{args.annotation_id});

    if (args.remove) {
        engine.planning.annotation.removeTag(d, id, args.tag) catch |e|
            exit.die(ctx, e, "annotate tag --remove: {s}", .{@errorName(e)});
    } else {
        engine.planning.annotation.addTag(d, ctx.allocator, id, args.tag) catch |e|
            exit.die(ctx, e, "annotate tag: {s}", .{@errorName(e)});
    }

    if (args.json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"id\":{d},\"tag\":\"{s}\",\"action\":\"{s}\"}}\n",
            .{ id, args.tag, if (args.remove) "remove" else "add" },
        );
    } else {
        try ctx.stdout.print(
            "annotation {d}: {s} tag '{s}'\n",
            .{ id, if (args.remove) "removed" else "added", args.tag },
        );
    }
}
