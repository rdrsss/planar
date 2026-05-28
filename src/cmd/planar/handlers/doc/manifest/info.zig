//! handlers/doc/manifest/info — `planar doc manifest info <path>`
//!
//! Inspector for a single manifest entry: prints the doc hash,
//! sources hash, composite entry hash, and the resolved sources
//! (provenance refs + their hashes). Lets operators answer
//! "what does the manifest think about this one doc, and what
//! is it tracking as its source?" without grepping a JSON blob.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");
const output = @import("../../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "manifest", "info" }, args_ptr);
    const ctx = runtime.current();
    const stored = engine.docs.manifest.load(engine.docs.manifest.file_name, ctx.allocator) catch |e| switch (e) {
        error.FileNotFound => exit.die(ctx, error.NotFound, ".manifest-docs not found; run `planar doc manifest update`", .{}),
        else => exit.die(ctx, e, "loading manifest: {s}", .{@errorName(e)}),
    };
    defer engine.docs.manifest.deinitManifest(stored, ctx.allocator);

    const want_path = args.entry_path;
    var found: ?engine.docs.manifest.EntryRow = null;
    for (stored.entries) |row| {
        if (std.mem.eql(u8, row.path, want_path)) {
            found = row;
            break;
        }
    }
    const row = found orelse exit.die(ctx, error.NotFound, "no manifest entry for path '{s}'", .{want_path});

    if (args.json) {
        try ctx.stdout.print("{{\"path\":", .{});
        try output.writeJsonString(ctx.stdout, row.path);
        try ctx.stdout.print(",\"doc_hash\":", .{});
        try output.writeJsonString(ctx.stdout, row.entry.doc_hash);
        try ctx.stdout.print(",\"sources_hash\":", .{});
        try output.writeJsonString(ctx.stdout, row.entry.sources_hash);
        try ctx.stdout.print(",\"entry_hash\":", .{});
        try output.writeJsonString(ctx.stdout, row.entry.entry_hash);
        try ctx.stdout.print(",\"sources\":[", .{});
        for (row.entry.sources, 0..) |s, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"ref\":", .{});
            try output.writeJsonString(ctx.stdout, s.ref);
            try ctx.stdout.print(",\"hash\":", .{});
            try output.writeJsonString(ctx.stdout, s.hash);
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]}}\n", .{});
    } else {
        try ctx.stdout.print("path:         {s}\n", .{row.path});
        try ctx.stdout.print("doc_hash:     {s}\n", .{row.entry.doc_hash});
        try ctx.stdout.print("sources_hash: {s}\n", .{row.entry.sources_hash});
        try ctx.stdout.print("entry_hash:   {s}\n", .{row.entry.entry_hash});
        if (row.entry.sources.len == 0) {
            try ctx.stdout.print("sources:      (none)\n", .{});
        } else {
            try ctx.stdout.print("sources:\n", .{});
            for (row.entry.sources) |s| try ctx.stdout.print("  {s}  {s}\n", .{ s.hash, s.ref });
        }
    }
}
