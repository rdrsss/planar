//! handlers/task/touches/add — `planar task touches add <task-id> <repo-slug>`
//!
//! Inserts entity_links(relationship='touches', from_kind='task', to_kind='repo')
//! where to_id is resolved from the projects table via the repo-slug.
//!
//! Link verbs are documented UNGUARDED (CLAUDE.md §Cross-scope guard). No
//! scope_guard.check call is made here. --scope is accepted for CLI parity.
//!
//! Repo-slug -> repo-id resolution is done with a direct DB query because
//! engine.entitylink.resolveRef does not support the 'repo' kind for slug
//! lookup (projects uses the 'slug' column but is not in the engine's slug
//! resolution table; D-no-engine-edits prohibits extending the engine here).
//!
//! JSON shape (mirrors Go's task touches add anonymous struct):
//!   { "ok": true, "task_id": <n>, "repo_id": <n>, "repo_slug": "..." }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("../../../runtime.zig");
const exit = @import("../../../exit.zig");

const TouchesAddResult = struct {
    ok: bool,
    task_id: i64,
    repo_id: i64,
    repo_slug: []const u8,
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

    const link = engine.entitylink.add(d, ctx.allocator, .{
        .from_kind = .task,
        .from_id = task_id,
        .to_kind = .repo,
        .to_id = repo_id,
        .relationship = .touches,
    }) catch |e| switch (e) {
        error.LinkExists => exit.die(ctx, e, "touches link task:{d} -> repo:{s} already exists", .{ task_id, args.repo_slug }),
        error.UnsupportedScope => exit.die(ctx, e, "scoped entity links not yet supported (M3)", .{}),
        else => exit.die(ctx, e, "task touches add: {s}", .{@errorName(e)}),
    };
    defer engine.entitylink.deinit(link, ctx.allocator);

    if (args.json) {
        const result = TouchesAddResult{
            .ok = true,
            .task_id = task_id,
            .repo_id = repo_id,
            .repo_slug = args.repo_slug,
        };
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
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
