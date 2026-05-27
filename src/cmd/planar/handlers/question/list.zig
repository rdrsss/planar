//! handlers/question/list — `planar question list [--scope --status]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "question", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var statuses: std.ArrayList(engine.planning.question.Status) = .empty;
    defer statuses.deinit(ctx.allocator);
    if (args.status) |s| {
        var it = std.mem.splitScalar(u8, s, ',');
        while (it.next()) |tok| {
            const trimmed = std.mem.trim(u8, tok, " ");
            if (trimmed.len == 0) continue;
            const st = engine.planning.question.Status.fromText(trimmed) orelse
                exit.die(ctx, error.InvalidStatus, "unknown status '{s}'", .{trimmed});
            try statuses.append(ctx.allocator, st);
        }
    }

    var scopes: std.ArrayList([]const u8) = .empty;
    defer scopes.deinit(ctx.allocator);
    if (args.scope) |s| {
        var it = std.mem.splitScalar(u8, s, ',');
        while (it.next()) |tok| {
            const trimmed = std.mem.trim(u8, tok, " ");
            if (trimmed.len == 0) continue;
            try scopes.append(ctx.allocator, trimmed);
        }
    }

    const filter: engine.planning.question.ListFilter = .{
        .statuses = statuses.items,
        .scopes = scopes.items,
    };

    const items = if (args.touches) |slug| blk: {
        const repo_id = resolveRepoSlug(d, slug) catch |e| switch (e) {
            error.NotFound => exit.die(ctx, e, "repo '{s}' not found", .{slug}),
            else => exit.die(ctx, e, "repo lookup: {s}", .{@errorName(e)}),
        };
        break :blk engine.planning.question.listTouching(d, ctx.allocator, repo_id, filter) catch |e|
            exit.die(ctx, e, "question list --touches: {s}", .{@errorName(e)});
    } else engine.planning.question.list(d, ctx.allocator, filter) catch |e|
        exit.die(ctx, e, "question list: {s}", .{@errorName(e)});

    try output.emitList(ctx, engine.planning.question, items, .{ .json = args.json });
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
