const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "promote" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const body_file = args.body_file orelse
        exit.die(ctx, error.InvalidInput, "no body provided; run pl-doc-promote skill or pass --body-file", .{});
    var sources_buf = [_][]const u8{args.source};
    const result = engine.docs.promote.run(
        d,
        ctx.allocator,
        args.kind,
        sources_buf[0..],
        args.slug,
        args.out,
        body_file,
        args.title,
    ) catch |e| switch (e) {
        error.NoBodyProvided => exit.die(ctx, error.InvalidInput, "no body provided; run pl-doc-promote skill or pass --body-file", .{}),
        error.InvalidInput, error.InvalidEntityRef => exit.die(ctx, error.InvalidInput, "invalid doc promote input", .{}),
        error.NotFound => exit.die(ctx, error.NotFound, "source entity not found", .{}),
        else => exit.die(ctx, e, "doc promote failed: {s}", .{@errorName(e)}),
    };
    defer engine.docs.promote.deinitResult(result, ctx.allocator);

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"path\":", .{});
        try output.writeJsonString(ctx.stdout, result.path);
        try ctx.stdout.print(",\"kind\":", .{});
        try output.writeJsonString(ctx.stdout, result.kind);
        try ctx.stdout.print(",\"sources\":[", .{});
        for (result.sources, 0..) |source, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"ref\":", .{});
            try output.writeJsonString(ctx.stdout, source.ref);
            try ctx.stdout.print(",\"hash\":", .{});
            try output.writeJsonString(ctx.stdout, source.hash);
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("],\"manifest_root\":", .{});
        try output.writeJsonString(ctx.stdout, result.manifest_root);
        try ctx.stdout.print("}}\n", .{});
    } else {
        try ctx.stdout.print("wrote {s} (kind {s}, {d} source(s), manifest root {s})\n", .{
            result.path,
            result.kind,
            result.sources.len,
            result.manifest_root,
        });
    }
}
