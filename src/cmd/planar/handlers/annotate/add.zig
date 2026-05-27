//! handlers/annotate/add — `planar annotate add [flags]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const scope_mod = @import("../../scope.zig");
const render = @import("render.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const anchor_path = args.anchor_path orelse exit.die(ctx, error.InvalidInput, "--anchor-path is required", .{});

    const resolution = scope_mod.resolve(ctx, args.scope) catch |e|
        exit.die(ctx, e, "annotate add: resolving scope failed: {s}", .{@errorName(e)});
    const effective_scope: ?[]const u8 = if (resolution.scope) |s| s else null;

    // --tags is a comma-separated list. Split + dedup happens in the
    // engine; we just slice the input here.
    var tag_buf: std.ArrayList([]const u8) = .empty;
    defer tag_buf.deinit(ctx.allocator);
    if (args.tags) |raw| {
        var it = std.mem.splitScalar(u8, raw, ',');
        while (it.next()) |part| {
            const t = std.mem.trim(u8, part, " \t");
            if (t.len > 0) try tag_buf.append(ctx.allocator, t);
        }
    }

    const ann = engine.planning.annotation.create(d, ctx.allocator, .{
        .anchor = .{
            .path = anchor_path,
            .line_start = args.line_start,
            .line_end = args.line_end,
            .commit_sha = args.commit_sha orelse "",
            .text_hash = args.text_hash orelse "",
            .text = args.text orelse "",
        },
        .title = args.title,
        .slug = args.slug,
        .body = args.body orelse "",
        .vendor = args.vendor orelse "",
        .plan_id = args.plan,
        .task_id = args.task,
        .tags = tag_buf.items,
        .scope = effective_scope,
    }) catch |e| exit.die(ctx, e, "annotate add: {s}", .{@errorName(e)});
    defer engine.planning.annotation.deinit(ann, ctx.allocator);

    try render.emitOne(ctx, ann, args.json);
}
