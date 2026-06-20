//! handlers/task/update — `planar task update <task-id> [...]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");
const scope_mod = @import("../../scope.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "task", "update" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    // --force enables reopen from terminal statuses (done/cancelled). When
    // combined with --status <reopen-target>, the engine will insert a
    // task_reopens row with source='task-update-force'. --reason is forwarded
    // to the reopen audit row; the field is optional for this path (unlike
    // `task reopen` which requires it).

    // Cross-scope write guard (plan 352 task 2451).
    //
    // Look up the task's stored scope and compare against the
    // operator's resolved write scope (--scope override or cwd-
    // derive). Refuse with ScopeMismatch when they disagree. The
    // matrix lives in policy.scope_guard.check; the slug
    // conversion lives in engine.identity.scope.slugFromRef.
    {
        const current = engine.planning.task.show(d, ctx.allocator, id) catch |e| switch (e) {
            error.NotFound => exit.die(ctx, e, "no task with id {d}", .{id}),
            else => exit.die(ctx, e, "task lookup: {s}", .{@errorName(e)}),
        };
        defer engine.planning.task.deinit(current, ctx.allocator);

        // engine.planning.task.ScopeKind and engine.identity.scope
        // .ScopeKind are sibling enums; translate by tag name.
        const scope_kind_enum: engine.identity.scope.ScopeKind = switch (current.scope_kind) {
            .global => .global,
            .association => .association,
            .repo => .repo,
        };
        const entity_scope: ?[]const u8 = engine.identity.scope.slugFromRef(
            d,
            ctx.allocator,
            scope_kind_enum,
            current.scope_id,
        ) catch |e| exit.die(ctx, e, "task update: looking up entity scope: {s}", .{@errorName(e)});
        defer if (entity_scope) |s| ctx.allocator.free(s);

        const resolution = scope_mod.resolveForWrite(ctx, args.scope) catch |e|
            exit.die(ctx, e, "task update: resolving write scope: {s}", .{@errorName(e)});

        scope_mod.guard(entity_scope, resolution.scope) catch
            exit.die(ctx, error.ScopeMismatch, "scope mismatch: task {d} is in scope '{s}' but operator write scope is '{s}'; pass --scope {s} to write to that scope from here", .{
                id,
                if (entity_scope) |s| s else "global",
                if (resolution.scope) |s| s else "global",
                if (entity_scope) |s| s else "global",
            });
    }

    var patch: engine.planning.task.UpdateArgs = .{
        .title = args.title,
        .body = args.body,
        .priority = args.priority,
        .next_action = args.next_action,
        .due_at = args.due,
        .slug = args.slug,
        .no_auto_promote = args.no_auto_promote,
        .scope = args.scope,
        .force = args.force,
        .reason = args.reason,
    };
    if (args.plan) |pid| {
        if (pid == 0) {
            patch.clear_plan = true;
        } else {
            patch.plan_id = pid;
        }
    }
    if (args.status) |s| {
        patch.status = engine.planning.task.Status.fromText(s) orelse
            exit.die(ctx, error.InvalidStatus, "unknown status '{s}'", .{s});
    }

    const task = engine.planning.task.update(d, ctx.allocator, id, patch) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no task with id {d}", .{id}),
        error.TaskClaimed => exit.die(
            ctx,
            e,
            "task {d} has an active work claim — operator status flip refused.\n" ++
                "Release or complete the claim via the agent path, or re-run with --force to override.",
            .{id},
        ),
        else => exit.die(ctx, e, "task update: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.task, task, .{ .json = args.json });
}
