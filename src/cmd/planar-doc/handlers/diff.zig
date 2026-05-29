const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"diff"}, args_ptr);
    const ctx = runtime.current();
    var result = engine.docs.differ.diff(ctx.allocator, ".") catch |e|
        exit.die(ctx, e, "diff failed: {s}", .{@errorName(e)});
    defer result.deinit();
    if (args.json) {
        for (result.records) |r| {
            try ctx.stdout.print(
                "{{\"signal\":\"{s}\",\"path\":\"{s}\",\"doc\":\"{s}\",\"detail\":\"{s}\"}}\n",
                .{ r.signal.toString(), r.path, r.doc, r.detail },
            );
        }
    } else {
        for (result.records) |r| {
            try ctx.stdout.print("{s} {s} ({s})\n", .{ r.signal.toString(), r.path, r.detail });
        }
        if (result.records.len == 0) try ctx.stdout.print("planar-doc diff: no drift\n", .{});
    }
    if (result.records.len > 0) std.process.exit(1);
}
