const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"nodoc"}, args_ptr);
    const ctx = runtime.current();
    if (args.remove) {
        engine.docs.cover.removeNodoc(ctx.allocator, ".", args.source) catch |e|
            exit.die(ctx, e, "nodoc remove failed: {s}", .{@errorName(e)});
        try ctx.stdout.print("planar-doc nodoc: removed {s}\n", .{args.source});
    } else {
        engine.docs.cover.addNodoc(ctx.allocator, ".", args.source) catch |e|
            exit.die(ctx, e, "nodoc add failed: {s}", .{@errorName(e)});
        try ctx.stdout.print("planar-doc nodoc: added {s}\n", .{args.source});
    }
}
