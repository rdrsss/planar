const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "orphans" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const rows = engine.docs.queries.orphans(d, ctx.allocator, args.kind, args.since) catch |e|
        exit.die(ctx, e, "doc orphans failed: {s}", .{@errorName(e)});
    defer engine.docs.queries.deinitOrphans(rows, ctx.allocator);
    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"kind\":", .{});
        try output.writeJsonString(ctx.stdout, args.kind);
        try ctx.stdout.print(",\"since_days\":{d},\"orphans\":[", .{args.since});
        for (rows, 0..) |row, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"id\":{d},\"kind\":", .{row.id});
            try output.writeJsonString(ctx.stdout, row.kind);
            try ctx.stdout.print(",\"title\":", .{});
            try output.writeJsonString(ctx.stdout, row.title);
            try ctx.stdout.print(",\"updated_at\":", .{});
            try output.writeJsonString(ctx.stdout, row.updated_at);
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]}}\n", .{});
    } else {
        for (rows) |row| try ctx.stdout.print("{d}\t{s}\t{s}\n", .{ row.id, row.kind, row.title });
    }
}
