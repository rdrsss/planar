//! handlers/doc/manifest/verify — `planar doc manifest verify`
//!
//! O(1) root-hash compare between the stored `.manifest-docs` and a
//! fresh scan of `docs/`. Exits 0 when the roots match; non-zero when
//! they differ. The drift gate for pre-commit hooks and CI. The
//! survey counterpart is `planar doc manifest diff` (per-path four-
//! signal classifier; always exits 0).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");
const output = @import("../../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "manifest", "verify" }, args_ptr);
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

    const ok = engine.docs.manifest.verify(stored, current);
    if (args.json) {
        try ctx.stdout.print("{{\"ok\":{},\"stored_root\":", .{ok});
        try output.writeJsonString(ctx.stdout, stored.root);
        try ctx.stdout.print(",\"current_root\":", .{});
        try output.writeJsonString(ctx.stdout, current.root);
        try ctx.stdout.print("}}\n", .{});
    } else if (ok) {
        try ctx.stdout.print("manifest verified (root {s})\n", .{stored.root});
    } else {
        try ctx.stdout.print("manifest drift: stored {s}, current {s}\n", .{ stored.root, current.root });
    }
    if (!ok) exit.die(ctx, error.InvalidInput, "manifest is out of date; run `planar doc manifest diff` to inspect, then `update` to refresh", .{});
}
