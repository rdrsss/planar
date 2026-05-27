const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "regenerate" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const selectors = @as(u8, if (args.slug != null) 1 else 0) + @as(u8, if (args.path != null) 1 else 0) + @as(u8, if (args.all) 1 else 0);
    if (selectors != 1) exit.die(ctx, error.InvalidInput, "exactly one of --slug, --path, or --all is required", .{});
    if (args.force and args.merge) exit.die(ctx, error.InvalidInput, "--force and --merge are mutually exclusive", .{});

    if (args.all) {
        if (args.body_file != null) exit.die(ctx, error.InvalidInput, "--body-file is not allowed with --all", .{});
        const targets = engine.docs.regenerate.candidates(d, ctx.allocator) catch |e|
            exit.die(ctx, e, "doc regenerate candidate scan failed: {s}", .{@errorName(e)});
        defer engine.docs.regenerate.deinitTargets(targets, ctx.allocator);
        if (args.json) {
            try ctx.stdout.print("{{\"ok\":true,\"regenerated\":[", .{});
            for (targets, 0..) |target, i| {
                if (i > 0) try ctx.stdout.print(",", .{});
                try ctx.stdout.print("{{\"path\":", .{});
                try output.writeJsonString(ctx.stdout, target.path);
                try ctx.stdout.print(",\"kind\":", .{});
                try output.writeJsonString(ctx.stdout, target.kind);
                try ctx.stdout.print(",\"hand_edit_detected\":{},\"action\":\"candidate\"}}", .{target.hand_edit_detected});
            }
            try ctx.stdout.print("],\"skipped\":[]}}\n", .{});
        } else if (targets.len == 0) {
            try ctx.stdout.print("no docs need regeneration\n", .{});
        } else {
            for (targets) |target| try ctx.stdout.print("candidate\t{s}\t{s}\n", .{ target.path, target.kind });
        }
        return;
    }

    const body_file = args.body_file orelse
        exit.die(ctx, error.InvalidInput, "no body provided; run pl-doc-regenerate skill or pass --body-file", .{});
    const path = if (args.path) |p| p else resolveSlug(ctx.allocator, args.slug.?);
    defer if (args.path == null) ctx.allocator.free(path);

    const result = engine.docs.regenerate.runPath(
        d,
        ctx.allocator,
        path,
        body_file,
        args.force,
        args.merge,
        args.out,
    ) catch |e| switch (e) {
        error.NoBodyProvided => exit.die(ctx, error.InvalidInput, "no body provided; run pl-doc-regenerate skill or pass --body-file", .{}),
        error.HandEditDetected => exit.die(ctx, error.InvalidInput, "hand-edit detected; run with --force or --merge", .{}),
        error.InvalidInput, error.InvalidEntityRef => exit.die(ctx, error.InvalidInput, "invalid doc regenerate input", .{}),
        error.NotFound => exit.die(ctx, error.NotFound, "source entity not found", .{}),
        else => exit.die(ctx, e, "doc regenerate failed: {s}", .{@errorName(e)}),
    };
    defer engine.docs.regenerate.deinitResult(result, ctx.allocator);

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"regenerated\":[{{\"path\":", .{});
        try output.writeJsonString(ctx.stdout, result.path);
        try ctx.stdout.print(",\"kind\":", .{});
        try output.writeJsonString(ctx.stdout, result.kind);
        try ctx.stdout.print(",\"hand_edit_detected\":{},\"action\":", .{result.hand_edit_detected});
        try output.writeJsonString(ctx.stdout, result.action);
        try ctx.stdout.print("}}],\"skipped\":[],\"manifest_root\":", .{});
        try output.writeJsonString(ctx.stdout, result.manifest_root);
        try ctx.stdout.print("}}\n", .{});
    } else {
        try ctx.stdout.print("{s} {s} (kind {s}, manifest root {s})\n", .{ result.action, result.path, result.kind, result.manifest_root });
    }
}

fn resolveSlug(allocator: std.mem.Allocator, slug: []const u8) []const u8 {
    return std.fmt.allocPrint(allocator, "docs/features/{s}.md", .{slug}) catch @panic("OOM");
}
