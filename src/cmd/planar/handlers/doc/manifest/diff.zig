const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");
const output = @import("../../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "manifest", "diff" }, args_ptr);
    const ctx = runtime.current();
    const root = if (args.path) |p| p else engine.docs.manifest.default_docs_root;
    const stored = engine.docs.manifest.load(engine.docs.manifest.file_name, ctx.allocator) catch |e| switch (e) {
        error.FileNotFound => exit.die(ctx, error.NotFound, ".manifest-docs not found; run `planar doc manifest update`", .{}),
        else => exit.die(ctx, e, "loading manifest: {s}", .{@errorName(e)}),
    };
    defer engine.docs.manifest.deinitManifest(stored, ctx.allocator);
    var overlays: std.ArrayList(engine.docs.manifest.SourceOverlay) = .empty;
    defer overlays.deinit(ctx.allocator);
    for (stored.entries) |entry| {
        if (entry.entry.sources.len > 0) try overlays.append(ctx.allocator, .{ .path = entry.path, .sources = entry.entry.sources });
    }
    const current = engine.docs.manifest.buildWithSources(root, ctx.allocator, overlays.items) catch |e|
        exit.die(ctx, e, "building manifest: {s}", .{@errorName(e)});
    defer engine.docs.manifest.deinitManifest(current, ctx.allocator);
    const changes = engine.docs.manifest.diff(stored, current, ctx.allocator) catch |e|
        exit.die(ctx, e, "diffing manifest: {s}", .{@errorName(e)});
    defer engine.docs.manifest.deinitChanges(changes, ctx.allocator);
    if (args.json) {
        try ctx.stdout.print("{{\"ok\":{},\"stored_root\":", .{changes.len == 0});
        try output.writeJsonString(ctx.stdout, stored.root);
        try ctx.stdout.print(",\"current_root\":", .{});
        try output.writeJsonString(ctx.stdout, current.root);
        try ctx.stdout.print(",\"changes\":[", .{});
        for (changes, 0..) |change, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"path\":", .{});
            try output.writeJsonString(ctx.stdout, change.path);
            try ctx.stdout.print(",\"signal\":", .{});
            try output.writeJsonString(ctx.stdout, change.signal.toText());
            try ctx.stdout.print(",\"detail\":", .{});
            try output.writeJsonString(ctx.stdout, change.detail);
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]}}\n", .{});
    } else if (changes.len == 0) {
        try ctx.stdout.print("manifest is up to date (root {s})\n", .{stored.root});
    } else {
        for (changes) |change| try ctx.stdout.print("{s}\t{s}\t{s}\n", .{ change.signal.toText(), change.path, change.detail });
    }
    // diff is a survey: it reports the four-signal classification
    // and exits 0 regardless. The drift gate is `manifest verify`
    // (O(1) root compare, exits non-zero on drift). Splitting the
    // two surfaces lets pre-commit hooks use verify while operators
    // read diff to plan the fix.
}
