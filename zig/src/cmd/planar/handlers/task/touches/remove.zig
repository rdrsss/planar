//! handlers/task/touches/remove — `planar task touches remove <task-id> <repo-slug> [--path <p>]`
//!
//! Without --path: deletes the entity_links(relationship='touches',
//! from_kind='task', to_kind='repo') row between the given task and repo.
//!
//! With --path <p>: deletes ONE `task_touch_paths` row and leaves the repo
//! edge in place. This is the inverse of `touches add --path` at the same
//! granularity. It is deliberately NOT symmetric with add's implication
//! (a path-touch implies the repo-touch): withdrawing one file should not
//! silently withdraw the repo claim, which may still be carrying other
//! paths or an intentional whole-repo declaration.
//!
//! Why the flag exists: a path-level touch could be created two ways
//! (`touches add --path`, `touches infer --apply`) and removed by none, so
//! a mis-declared path was only correctable with raw SQL. Removing the repo
//! edge is not a substitute — rule 2's loadTouches reads `task_touch_paths`
//! directly, so orphaned path rows keep driving eligibility after their
//! edge is gone.
//!
//! Removal requires finding the link-id first: list by from_kind=task + from_id +
//! to_kind=repo + to_id + relationship=touches, then call engine.entitylink.remove.
//!
//! Link verbs are documented UNGUARDED (CLAUDE.md §Cross-scope guard). No
//! scope_guard.check call is made here. --scope is accepted for CLI parity.
//!
//! JSON shape (mirrors Go's task touches remove anonymous struct):
//!   { "ok": true, "task_id": <n>, "repo_id": <n>, "repo_slug": "..." }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");

const TouchesRemoveResult = struct {
    ok: bool,
    task_id: i64,
    repo_id: i64,
    repo_slug: []const u8,
    path: ?[]const u8 = null,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "touches", "remove" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const task_id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    // --scope accepted for parity; link verbs are unguarded.
    _ = args.scope;

    // Resolve repo-slug → repo-id (same reasoning as touches/add.zig).
    const repo_id = resolveRepoSlug(d, args.repo_slug) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "repo '{s}' not found", .{args.repo_slug}),
        else => exit.die(ctx, e, "repo lookup: {s}", .{@errorName(e)}),
    };

    // Path-level withdrawal: delete the one task_touch_paths row and stop.
    // The repo edge is intentionally preserved (see the module comment).
    if (args.path) |path| {
        engine.planning.task.removeTouchPath(d, task_id, repo_id, path) catch |e| switch (e) {
            error.NotFound => exit.die(
                ctx,
                e,
                "task:{d} has no declared path touch '{s}' on repo:{s}",
                .{ task_id, path, args.repo_slug },
            ),
            else => exit.die(ctx, e, "task touches remove --path: {s}", .{@errorName(e)}),
        };

        if (args.json) {
            const result = TouchesRemoveResult{
                .ok = true,
                .task_id = task_id,
                .repo_id = repo_id,
                .repo_slug = args.repo_slug,
                .path = path,
            };
            try std.json.Stringify.value(result, .{}, ctx.stdout);
            try ctx.stdout.print("\n", .{});
        } else {
            try ctx.stdout.print(
                "path-touch removed: task:{d} -> repo:{s} path:{s}\n",
                .{ task_id, args.repo_slug, path },
            );
        }
        return;
    }

    // Find the touches link id.
    const links = engine.entitylink.list(d, ctx.allocator, .{
        .from_kind = .task,
        .from_id = task_id,
        .to_kind = .repo,
        .to_id = repo_id,
        .relationship = .touches,
    }) catch |e|
        exit.die(ctx, e, "task touches remove lookup: {s}", .{@errorName(e)});
    defer engine.entitylink.deinitMany(links, ctx.allocator);

    if (links.len == 0) {
        exit.die(ctx, error.NotFound, "no touches link between task:{d} and repo:{s}", .{ task_id, args.repo_slug });
    }

    // Remove the first (and normally only) match.
    engine.entitylink.remove(d, ctx.allocator, links[0].id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "link not found (already removed?)", .{}),
        else => exit.die(ctx, e, "task touches remove: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        const result = TouchesRemoveResult{
            .ok = true,
            .task_id = task_id,
            .repo_id = repo_id,
            .repo_slug = args.repo_slug,
        };
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("touches link removed: task:{d} → repo:{s}\n", .{ task_id, args.repo_slug });
    }
}

/// resolveRepoSlug looks up a project id by its slug in the projects table.
fn resolveRepoSlug(d: anytype, slug: []const u8) !i64 {
    var stmt = d.prepare("select id from projects where slug = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return error.QueryFailed;
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => error.NotFound,
        .row => stmt.columnInt(0),
    };
}
