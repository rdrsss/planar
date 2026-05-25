const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "backlinks" }, args_ptr);
    const ctx = runtime.current();
    const rows = engine.docs.queries.backlinks(ctx.allocator, args.ref) catch |e| switch (e) {
        error.FileNotFound => exit.die(ctx, error.NotFound, ".manifest-docs not found", .{}),
        else => exit.die(ctx, e, "doc backlinks failed: {s}", .{@errorName(e)}),
    };
    defer engine.docs.queries.deinitBacklinks(rows, ctx.allocator);
    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"entity\":", .{});
        try output.writeJsonString(ctx.stdout, args.ref);
        try ctx.stdout.print(",\"docs\":[", .{});
        for (rows, 0..) |row, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"path\":", .{});
            try output.writeJsonString(ctx.stdout, row.path);
            try ctx.stdout.print(",\"source_hash\":", .{});
            try output.writeJsonString(ctx.stdout, row.source_hash);
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]}}\n", .{});
    } else {
        for (rows) |row| try ctx.stdout.print("{s}\t{s}\n", .{ row.path, row.source_hash });
    }
}
