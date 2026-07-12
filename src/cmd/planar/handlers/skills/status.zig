const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "skills", "status" }, args_ptr);
    const ctx = runtime.current();
    const homes = common.resolveHomes(ctx) catch |e| exit.die(ctx, e, "skills status: resolving install homes: {s}", .{@errorName(e)});
    defer homes.deinit(ctx.allocator);
    var result = engine.installedsurface.status(ctx.allocator, homes.options(args.vendor)) catch |e| switch (e) {
        error.InvalidVendor => exit.die(ctx, error.InvalidInput, "skills status: --vendor must be claude, codex, or copilot", .{}),
        else => exit.die(ctx, e, "skills status: {s}", .{@errorName(e)}),
    };
    defer result.deinit();
    if (args.json) try common.emitStatusJson(ctx, result) else try common.emitStatusText(ctx, result);
}
