//! handlers/task/touches/list — `planar task touches list <task-id> [--json]`
//!
//! Surfaces the touches declared on a task at both granularities:
//!   - repo-level: entity_links(from_kind='task', to_kind='repo',
//!     relationship='touches') -> the repo slugs the task touches.
//!   - path-level: task_touch_paths rows (repo slug + repo-relative path),
//!     the precise file declarations the parallelizability rules consume.
//!
//! JSON shape:
//!   {
//!     "task_id": <n>,
//!     "repos": ["slug", ...],
//!     "paths": [ { "repo": "slug", "path": "..." }, ... ]
//!   }

const std = @import("std");
const cli = @import("cli");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");

const PathRow = struct {
    repo: []const u8,
    path: []const u8,
};

const TouchesListResult = struct {
    task_id: i64,
    repos: []const []const u8,
    paths: []const PathRow,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "touches", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const a = ctx.allocator;

    const task_id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    // Repo-level touches: the repo slugs linked via entity_links.
    var repos: std.ArrayList([]const u8) = .empty;
    {
        var stmt = d.prepare(
            \\select p.slug
            \\from entity_links el
            \\join projects p on p.id = el.to_id
            \\where el.from_kind = 'task' and el.from_id = ?
            \\  and el.to_kind = 'repo' and el.relationship = 'touches'
            \\order by p.slug
        ) catch |e| exit.die(ctx, e, "task touches list (repos): {s}", .{@errorName(e)});
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch |e| exit.die(ctx, e, "task touches list bind: {s}", .{@errorName(e)});
        while (true) {
            switch (stmt.step() catch |e| exit.die(ctx, e, "task touches list step: {s}", .{@errorName(e)})) {
                .done => break,
                .row => {
                    const slug = stmt.columnTextAlloc(0, a) catch |e|
                        exit.die(ctx, e, "task touches list: {s}", .{@errorName(e)});
                    try repos.append(a, slug);
                },
            }
        }
    }

    // Path-level touches: task_touch_paths rows, joined to the repo slug.
    var paths: std.ArrayList(PathRow) = .empty;
    {
        var stmt = d.prepare(
            \\select p.slug, ttp.path
            \\from task_touch_paths ttp
            \\join projects p on p.id = ttp.repo_id
            \\where ttp.task_id = ?
            \\order by p.slug, ttp.path
        ) catch |e| exit.die(ctx, e, "task touches list (paths): {s}", .{@errorName(e)});
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch |e| exit.die(ctx, e, "task touches list bind: {s}", .{@errorName(e)});
        while (true) {
            switch (stmt.step() catch |e| exit.die(ctx, e, "task touches list step: {s}", .{@errorName(e)})) {
                .done => break,
                .row => {
                    const slug = stmt.columnTextAlloc(0, a) catch |e|
                        exit.die(ctx, e, "task touches list: {s}", .{@errorName(e)});
                    const path = stmt.columnTextAlloc(1, a) catch |e|
                        exit.die(ctx, e, "task touches list: {s}", .{@errorName(e)});
                    try paths.append(a, .{ .repo = slug, .path = path });
                },
            }
        }
    }

    if (args.json) {
        const result = TouchesListResult{
            .task_id = task_id,
            .repos = repos.items,
            .paths = paths.items,
        };
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("task:{d} touches\n", .{task_id});
        if (repos.items.len == 0 and paths.items.len == 0) {
            try ctx.stdout.print("  (none declared)\n", .{});
        } else {
            for (repos.items) |slug| {
                try ctx.stdout.print("  repo: {s}\n", .{slug});
            }
            for (paths.items) |row| {
                try ctx.stdout.print("  path: {s}:{s}\n", .{ row.repo, row.path });
            }
        }
    }
}
