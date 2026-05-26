//! handlers/task/list — `planar task list [--status] [--plan] [--priority-max] ...`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var filter: engine.planning.task.ListFilter = .{
        .plan_id = args.plan,
        .priority_max = args.priority_max,
        .scope = args.scope,
    };
    if (args.status) |s| {
        filter.status = engine.planning.task.Status.fromText(s) orelse
            exit.die(ctx, error.InvalidStatus, "unknown status '{s}'", .{s});
    }
    const tasks = if (args.touches) |slug| blk: {
        const repo_id = resolveRepoSlug(d, slug) catch |e| switch (e) {
            error.NotFound => exit.die(ctx, e, "repo '{s}' not found", .{slug}),
            else => exit.die(ctx, e, "repo lookup: {s}", .{@errorName(e)}),
        };
        break :blk engine.planning.task.listTouching(d, ctx.allocator, repo_id, filter) catch |e|
            exit.die(ctx, e, "task list --touches: {s}", .{@errorName(e)});
    } else engine.planning.task.list(d, ctx.allocator, filter) catch |e|
        exit.die(ctx, e, "task list: {s}", .{@errorName(e)});

    try output.emitList(ctx, engine.planning.task, tasks, .{ .json = args.json });
}

fn resolveRepoSlug(d: anytype, slug: []const u8) !i64 {
    var stmt = d.prepare("select id from projects where slug = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return error.QueryFailed;
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => error.NotFound,
        .row => stmt.columnInt(0),
    };
}
