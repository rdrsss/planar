const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("../../../runtime.zig");
const exit = @import("../../../exit.zig");
const output = @import("../../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "manifest", "validate" }, args_ptr);
    const ctx = runtime.current();
    const path = if (args.path) |p| p else engine.docs.manifest.file_name;
    const m = engine.docs.manifest.load(path, ctx.allocator) catch |e| switch (e) {
        error.FileNotFound => exit.die(ctx, error.NotFound, "manifest not found: {s}", .{path}),
        else => exit.die(ctx, e, "manifest invalid: {s}", .{@errorName(e)}),
    };
    defer engine.docs.manifest.deinitManifest(m, ctx.allocator);
    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"path\":", .{});
        try output.writeJsonString(ctx.stdout, path);
        try ctx.stdout.print(",\"root\":", .{});
        try output.writeJsonString(ctx.stdout, m.root);
        try ctx.stdout.print(",\"entries\":{d}}}\n", .{m.entries.len});
    } else {
        try ctx.stdout.print("manifest valid ({d} entries, root {s})\n", .{ m.entries.len, m.root });
    }
}
