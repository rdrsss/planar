const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"build"}, args_ptr);
    const ctx = runtime.current();
    const result = engine.docs.builder.build(ctx.allocator, ".") catch |e|
        exit.die(ctx, e, "build failed: {s}", .{@errorName(e)});
    defer engine.docs.manifest_v2.deinitManifest(ctx.allocator, result.manifest);
    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"root\":\"{s}\",\"pre_existed\":{}}}\n", .{ result.manifest.root, result.pre_existed });
    } else {
        try ctx.stdout.print("planar-doc build: root={s} pre_existed={}\n", .{ result.manifest.root, result.pre_existed });
    }
}
