const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"cover"}, args_ptr);
    const ctx = runtime.current();
    if (args.remove) {
        engine.docs.cover.removeCover(ctx.allocator, ".", args.doc, args.source) catch |e|
            exit.die(ctx, e, "cover remove failed: {s}", .{@errorName(e)});
        try ctx.stdout.print("planar-doc cover: removed {s} from {s}\n", .{ args.source, args.doc });
    } else {
        engine.docs.cover.addCover(ctx.allocator, ".", args.doc, args.source) catch |e|
            exit.die(ctx, e, "cover add failed: {s}", .{@errorName(e)});
        try ctx.stdout.print("planar-doc cover: added {s} to {s}\n", .{ args.source, args.doc });
    }
}
