const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"verify"}, args_ptr);
    const ctx = runtime.current();
    const result = engine.docs.builder.verify(ctx.allocator, ".") catch |e|
        exit.die(ctx, e, "verify failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(result.stored_root);
    defer ctx.allocator.free(result.computed_root);
    if (args.json) {
        try ctx.stdout.print(
            "{{\"root\":\"{s}\",\"manifest_root\":\"{s}\",\"drift\":{}}}\n",
            .{ result.computed_root, result.stored_root, !result.matches },
        );
    } else if (result.matches) {
        try ctx.stdout.print("planar-doc verify: ok (root={s})\n", .{result.stored_root});
    } else {
        try ctx.stdout.print(
            "planar-doc verify: DRIFT — stored={s} computed={s}\n",
            .{ result.stored_root, result.computed_root },
        );
    }
    if (!result.matches) std.process.exit(1);
}
