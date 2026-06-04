//! handlers/task/touches/add — `planar task touches add <task-id> <repo-slug> [--path <p>]`
//!
//! Without --path: inserts entity_links(relationship='touches',
//! from_kind='task', to_kind='repo') where to_id is resolved from the
//! projects table via the repo-slug (unchanged legacy behavior).
//!
//! With --path <p>: ALSO writes a path-level task_touch_paths row
//! (task_id, repo_id, path) via engine.planning.task.addTouchPath. A path-touch
//! implies the repo-touch, so the repo-level entity_links edge is written
//! too (idempotently — a pre-existing edge is not an error in this mode),
//! keeping the coarse repo signal consistent with the fine path signal.
//! The parallelizability rules (`plan recommend-strategy`) read the
//! path-level rows for rules 2/3/4.
//!
//! Link verbs are documented UNGUARDED (CLAUDE.md §Cross-scope guard). No
//! scope_guard.check call is made here. --scope is accepted for CLI parity.
//!
//! Repo-slug -> repo-id resolution is done with a direct DB query because
//! engine.entitylink.resolveRef does not support the 'repo' kind for slug
//! lookup (projects uses the 'slug' column but is not in the engine's slug
//! resolution table).
//!
//! JSON shape:
//!   repo-level: { "ok": true, "task_id": <n>, "repo_id": <n>, "repo_slug": "..." }
//!   path-level: { "ok": true, "task_id": <n>, "repo_id": <n>, "repo_slug": "...", "path": "..." }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");

const TouchesAddResult = struct {
    ok: bool,
    task_id: i64,
    repo_id: i64,
    repo_slug: []const u8,
    path: ?[]const u8 = null,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "touches", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const task_id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    // --scope accepted for parity; link verbs are unguarded.
    _ = args.scope;

    // Resolve repo-slug -> repo-id via the projects table. The engine's
    // resolveRef does not support 'repo' kind slug resolution (D-no-engine-edits).
    const repo_id = resolveRepoSlug(d, args.repo_slug) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "repo '{s}' not found", .{args.repo_slug}),
        else => exit.die(ctx, e, "repo lookup: {s}", .{@errorName(e)}),
    };

    // Write the repo-level touches edge. When a path is being declared, a
    // pre-existing repo edge is fine (the path-touch implies it) — we treat
    // LinkExists as a no-op in that mode rather than an error.
    if (engine.entitylink.add(d, ctx.allocator, .{
        .from_kind = .task,
        .from_id = task_id,
        .to_kind = .repo,
        .to_id = repo_id,
        .relationship = .touches,
    })) |link| {
        engine.entitylink.deinit(link, ctx.allocator);
    } else |e| switch (e) {
        error.LinkExists => if (args.path == null) {
            exit.die(ctx, e, "touches link task:{d} -> repo:{s} already exists", .{ task_id, args.repo_slug });
        },
        error.UnsupportedScope => exit.die(ctx, e, "scoped entity links not yet supported (M3)", .{}),
        else => exit.die(ctx, e, "task touches add: {s}", .{@errorName(e)}),
    }

    // Path-level declaration: write the task_touch_paths row.
    if (args.path) |path| {
        engine.planning.task.addTouchPath(d, task_id, repo_id, path) catch |e|
            exit.die(ctx, e, "task touches add --path: {s}", .{@errorName(e)});
    }

    if (args.json) {
        const result = TouchesAddResult{
            .ok = true,
            .task_id = task_id,
            .repo_id = repo_id,
            .repo_slug = args.repo_slug,
            .path = args.path,
        };
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else if (args.path) |path| {
        try ctx.stdout.print("path-touch added: task:{d} -> repo:{s} path:{s}\n", .{ task_id, args.repo_slug, path });
    } else {
        try ctx.stdout.print("touches link added: task:{d} -> repo:{s}\n", .{ task_id, args.repo_slug });
    }
}

/// resolveRepoSlug looks up a project id by its slug in the projects table.
///
/// Returns error.NotFound when no project with that slug exists.
/// Returns error.QueryFailed on a DB error.
fn resolveRepoSlug(d: anytype, slug: []const u8) !i64 {
    var stmt = d.prepare("select id from projects where slug = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return error.QueryFailed;
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => error.NotFound,
        .row => stmt.columnInt(0),
    };
}
