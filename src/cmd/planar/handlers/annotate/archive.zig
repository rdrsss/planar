const stub = @import("_stub.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    return stub.handle(args_ptr, "archive");
}
