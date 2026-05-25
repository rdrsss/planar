const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    _ = cli.castArgs(main.root, &.{ "workbench", "publish" }, args_ptr);
    return error.NotImplemented;
}
