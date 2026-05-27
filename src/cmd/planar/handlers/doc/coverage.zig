const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "coverage" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const c = engine.docs.queries.coverage(d, ctx.allocator) catch |e|
        exit.die(ctx, e, "doc coverage failed: {s}", .{@errorName(e)});
    defer engine.docs.queries.deinitCoverage(c, ctx.allocator);
    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"total_done\":{d},\"covered\":{d},\"uncovered\":[", .{ c.total_done, c.covered });
        for (c.uncovered, 0..) |row, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"id\":{d},\"title\":", .{row.id});
            try output.writeJsonString(ctx.stdout, row.title);
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]}}\n", .{});
    } else {
        try ctx.stdout.print("done plans: {d}, covered: {d}, uncovered: {d}\n", .{ c.total_done, c.covered, c.uncovered.len });
        for (c.uncovered) |row| try ctx.stdout.print("{d}\t{s}\n", .{ row.id, row.title });
    }
}
