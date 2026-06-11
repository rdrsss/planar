//! handlers/audit/commits — `planar audit commits [--session <id>] [--task <id>] [--json|--shas]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "audit", "commits" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    if (args.json and args.shas) {
        exit.die(ctx, error.InvalidInput, "cannot combine --json with --shas", .{});
    }

    if (args.session) |session_id| {
        const session_row = engine.runtime.session.getById(d, ctx.allocator, session_id) catch |e| switch (e) {
            error.NotFound => exit.die(ctx, e, "session {d} not found", .{session_id}),
            else => exit.die(ctx, e, "audit commits: {s}", .{@errorName(e)}),
        };
        engine.runtime.session.deinit(session_row, ctx.allocator);
    }

    if (args.task) |task_id| {
        const task_row = engine.planning.task.show(d, ctx.allocator, task_id) catch |e| switch (e) {
            error.NotFound => exit.die(ctx, e, "task {d} not found", .{task_id}),
            else => exit.die(ctx, e, "audit commits: {s}", .{@errorName(e)}),
        };
        engine.planning.task.deinit(task_row, ctx.allocator);
    }

    const rows = engine.runtime.sessioncommits.listFiltered(d, ctx.allocator, .{
        .session_id = args.session,
        .task_id = args.task,
    }) catch |e| exit.die(ctx, e, "audit commits: {s}", .{@errorName(e)});
    defer engine.runtime.sessioncommits.Row.deinitMany(rows, ctx.allocator);

    if (args.json) {
        try engine.runtime.sessioncommits.writeJsonList(ctx.stdout, rows);
        try ctx.stdout.print("\n", .{});
        return;
    }

    if (args.shas) {
        for (rows) |row| try ctx.stdout.print("{s}\n", .{row.sha});
        return;
    }

    try writeHumanTable(ctx.stdout, rows);
}

fn writeHumanTable(writer: *std.Io.Writer, rows: []const engine.runtime.sessioncommits.Row) !void {
    try writer.print("{s:<40}  {s:>7}  {s:>5}  {s:<25}  {s}\n", .{
        "SHA",
        "session",
        "claim",
        "committed_at",
        "subject",
    });
    for (rows) |row| {
        try writer.print("{s:<40}  {d:>7}  ", .{ row.sha, @as(u64, @intCast(row.session_id)) });
        if (row.claim_id) |claim_id| {
            try writer.print("{d:>5}", .{@as(u64, @intCast(claim_id))});
        } else {
            try writer.print("{s:>5}", .{"-"});
        }
        try writer.print("  ", .{});
        if (row.committed_at) |committed_at| {
            try writer.print("{s:<25}", .{committed_at});
        } else {
            try writer.print("{s:<25}", .{"-"});
        }
        try writer.print("  {s}\n", .{row.subject orelse ""});
    }
}
