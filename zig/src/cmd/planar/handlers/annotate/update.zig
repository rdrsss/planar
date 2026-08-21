//! handlers/annotate/update — `planar annotate update <id> [patch flags] [--json]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const render = @import("render.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "update" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.annotation_id, 10) catch
        exit.die(ctx, error.InvalidInput, "annotation id must be an integer, got '{s}'", .{args.annotation_id});

    var patch: engine.planning.annotation.UpdateArgs = .{
        .title = args.title,
        .slug = args.slug,
        .body = args.body,
        .plan_id = args.plan,
        .task_id = args.task,
        .scope = args.scope,
    };
    if (args.status) |s| {
        patch.status = engine.planning.annotation.Status.fromText(s) orelse
            exit.die(ctx, error.InvalidStatus, "unknown status '{s}'", .{s});
    }

    const ann = engine.planning.annotation.update(d, ctx.allocator, id, patch) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no annotation with id {d}", .{id}),
        else => exit.die(ctx, e, "annotate update: {s}", .{@errorName(e)}),
    };
    defer engine.planning.annotation.deinit(ann, ctx.allocator);

    try render.emitOne(ctx, ann, args.json);
}
