const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque, cmd_name: []const u8) anyerror!void {
    _ = args_ptr;
    const ctx = runtime.current();
    exit.die(
        ctx,
        error.NotImplemented,
        "annotate {s} is scaffolded for M1 and will land in a follow-up milestone",
        .{cmd_name},
    );
}
