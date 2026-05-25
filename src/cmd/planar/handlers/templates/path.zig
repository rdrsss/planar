//! handlers/templates/path — `planar templates path`.
//!
//! Print the resolved templates root directory.

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "templates", "path" }, args_ptr);
    const ctx = runtime.current();
    _ = args.system;
    _ = args.set;
    _ = args.json;

    const root = common.resolveTemplatesRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving templates root: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    try ctx.stdout.print("{s}\n", .{root});
}
