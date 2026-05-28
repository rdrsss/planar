const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");
const output = @import("../../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "manifest", "update" }, args_ptr);
    const ctx = runtime.current();
    const root = if (args.path) |p| p else engine.docs.manifest.default_docs_root;
    const prior = engine.docs.manifest.load(engine.docs.manifest.file_name, ctx.allocator) catch |e| switch (e) {
        error.FileNotFound => null,
        else => exit.die(ctx, e, "loading prior manifest: {s}", .{@errorName(e)}),
    };
    defer if (prior) |p| engine.docs.manifest.deinitManifest(p, ctx.allocator);
    var overlays: std.ArrayList(engine.docs.manifest.SourceOverlay) = .empty;
    defer overlays.deinit(ctx.allocator);
    if (prior) |p| {
        for (p.entries) |entry| {
            if (entry.entry.sources.len > 0) try overlays.append(ctx.allocator, .{ .path = entry.path, .sources = entry.entry.sources });
        }
    }
    const m = engine.docs.manifest.buildWithSources(root, ctx.allocator, overlays.items) catch |e|
        exit.die(ctx, e, "building manifest: {s}", .{@errorName(e)});
    defer engine.docs.manifest.deinitManifest(m, ctx.allocator);
    engine.docs.manifest.write(engine.docs.manifest.file_name, m, ctx.allocator) catch |e|
        exit.die(ctx, e, "writing manifest: {s}", .{@errorName(e)});
    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"root\":", .{});
        try output.writeJsonString(ctx.stdout, m.root);
        try ctx.stdout.print(",\"entries\":{d}}}\n", .{m.entries.len});
    } else {
        try ctx.stdout.print("wrote {s} ({d} entries, root {s})\n", .{ engine.docs.manifest.file_name, m.entries.len, m.root });
    }
}
