//! handlers/task/add — `planar task add <title> [...]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");
const editor = @import("../../editor.zig");
const scope_mod = @import("../../scope.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Resolve write scope: --scope override wins; otherwise cwd-
    // derive uses the project-at-cwd's bound assoc when present.
    // Plan 352 task 2450.
    const resolution = scope_mod.resolve(ctx, args.scope) catch |e|
        exit.die(ctx, e, "task add: resolving scope failed: {s}", .{@errorName(e)});
    const effective_scope: ?[]const u8 = if (resolution.scope) |s| s else null;

    var body_owned: ?[]u8 = null;
    defer if (body_owned) |b| ctx.allocator.free(b);
    var body_value: ?[]const u8 = args.body;
    const stdout_is_tty = std.Io.File.stdout().isTty(ctx.io) catch false;

    if (body_value == null and args.editor and stdout_is_tty) {
        const res = editor.invoke(ctx.io, ctx.allocator, "", .{}) catch |e|
            exit.die(ctx, e, "opening editor failed: {s}", .{@errorName(e)});
        defer res.deinit(ctx.allocator);
        if (res.editor_exit_code != 0) {
            exit.die(ctx, error.InvalidInput, "editor exited with code {d}", .{res.editor_exit_code});
        }
        body_owned = try ctx.allocator.dupe(u8, res.content);
        body_value = body_owned;
    }

    const task = engine.planning.task.create(d, ctx.allocator, .{
        .title = args.title,
        .body = body_value,
        .priority = args.priority,
        .plan_id = args.plan,
        .parent_task_id = args.parent,
        .next_action = args.next_action,
        .due_at = args.due,
        .slug = args.slug,
        .no_auto_promote = args.no_auto_promote,
        .scope = effective_scope,
    }) catch |e| exit.die(ctx, e, "task add: {s}", .{@errorName(e)});

    try output.emit(ctx, engine.planning.task, task, .{ .json = args.json });
}
