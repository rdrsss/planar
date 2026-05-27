//! handlers/skills/render — `planar skills render [slug...]`.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "skills", "render" }, args_ptr);
    const ctx = runtime.current();

    if (args.diff and !args.check) {
        exit.die(ctx, error.InvalidInput, "--diff requires --check", .{});
    }

    const slugs = collectSlugFilter(ctx.allocator, ctx.argv) catch |e|
        exit.die(ctx, e, "skills render: parsing slug args: {s}", .{@errorName(e)});
    defer {
        for (slugs) |s| ctx.allocator.free(s);
        ctx.allocator.free(slugs);
    }

    if (args.check) {
        var res = engine.skillrender.checkTree(ctx.allocator, .{
            .src_dir = args.src,
            .out_dir = args.out,
            .slug_filter = slugs,
            .emit_diff = args.diff,
        }) catch |e| exit.die(ctx, e, "skills render --check: {s}", .{@errorName(e)});
        defer res.deinit(ctx.allocator);
        if (res.inSync()) return;

        const reasons = [_]engine.skillrender.DriftReason{
            .content,
            .missing,
            .orphan,
        };
        for (reasons) |reason| {
            for (res.drifted) |d| {
                if (d.reason != reason) continue;
                try ctx.stderr.print("[{s}] {s}\n", .{ d.reason.text(), d.path });
                if (args.diff and d.reason == .content) {
                    if (res.diffForPath(d.path)) |body| try ctx.stderr.print("{s}", .{body});
                }
            }
        }
        exit.die(ctx, error.SkillRenderDrift, "skills render --check: {d} file(s) drifted", .{res.drifted.len});
    }

    var rendered = engine.skillrender.renderTree(ctx.allocator, .{
        .src_dir = args.src,
        .out_dir = args.out,
        .slug_filter = slugs,
    }) catch |e| exit.die(ctx, e, "skills render: {s}", .{@errorName(e)});
    defer rendered.deinit(ctx.allocator);
    for (rendered.written_paths) |path| try ctx.stdout.print("{s}\n", .{path});
}

fn collectSlugFilter(allocator: std.mem.Allocator, argv: []const []const u8) ![][]const u8 {
    var found: ?usize = null;
    var i: usize = 1;
    while (i + 1 < argv.len) : (i += 1) {
        if (std.mem.eql(u8, argv[i], "skills") and std.mem.eql(u8, argv[i + 1], "render")) {
            found = i + 2;
            break;
        }
    }
    if (found == null) return allocator.alloc([]const u8, 0);

    var slugs = std.ArrayList([]const u8).empty;
    errdefer {
        for (slugs.items) |s| allocator.free(s);
        slugs.deinit(allocator);
    }

    var cursor = found.?;
    var passthrough = false;
    while (cursor < argv.len) : (cursor += 1) {
        const tok = argv[cursor];
        if (!passthrough and std.mem.eql(u8, tok, "--")) {
            passthrough = true;
            continue;
        }
        if (!passthrough and std.mem.startsWith(u8, tok, "-")) {
            if (std.mem.eql(u8, tok, "--src") or std.mem.eql(u8, tok, "--out")) {
                if (cursor + 1 < argv.len) cursor += 1;
                continue;
            }
            if (std.mem.startsWith(u8, tok, "--src=") or std.mem.startsWith(u8, tok, "--out=")) continue;
            if (std.mem.eql(u8, tok, "--check") or std.mem.eql(u8, tok, "--diff")) continue;
            continue;
        }
        try slugs.append(allocator, try allocator.dupe(u8, tok));
    }
    return slugs.toOwnedSlice(allocator);
}
