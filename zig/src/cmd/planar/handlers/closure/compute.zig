//! handlers/closure/compute — `planar closure compute <task-id> [--scope <s>] [--json]`
//!
//! Runs the M2 derived-closure extractor over the task's declared seed
//! paths (task_touch_paths) and persists the role-partitioned closure to
//! the `closures` table. Prints a summary (or JSON with `--json`).
//!
//! Write-scope guard: the task's stored scope must agree with the
//! operator's resolved write scope (cwd-derived or --scope). Mirrors
//! `task update`'s guard — no `--no-scope-check`.
//!
//! JSON shape:
//!   { "task_id": <n>, "seeds": <n>, "modify": <n>, "reference": <n>,
//!     "transitive": <n>, "rows_written": <n>,
//!     "extractor_version": "..." }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const scope_mod = @import("../../scope.zig");

const ComputeJSON = struct {
    task_id: i64,
    seeds: usize,
    modify: usize,
    reference: usize,
    transitive: usize,
    rows_written: usize,
    extractor_version: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "closure", "compute" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const task_id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    // Cross-scope write guard (mirrors task update).
    {
        const current = engine.planning.task.show(d, ctx.allocator, task_id) catch |e| switch (e) {
            error.NotFound => exit.die(ctx, e, "no task with id {d}", .{task_id}),
            else => exit.die(ctx, e, "task lookup: {s}", .{@errorName(e)}),
        };
        defer engine.planning.task.deinit(current, ctx.allocator);

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
        ) catch |e| exit.die(ctx, e, "closure compute: looking up entity scope: {s}", .{@errorName(e)});
        defer if (entity_scope) |s| ctx.allocator.free(s);

        const resolution = scope_mod.resolveForWrite(ctx, args.scope) catch |e|
            exit.die(ctx, e, "closure compute: resolving write scope: {s}", .{@errorName(e)});

        scope_mod.guard(entity_scope, resolution.scope) catch
            exit.die(ctx, error.ScopeMismatch, "scope mismatch: task {d} is in scope '{s}' but operator write scope is '{s}'; pass --scope {s} to write to that scope from here", .{
                task_id,
                if (entity_scope) |s| s else "global",
                if (resolution.scope) |s| s else "global",
                if (entity_scope) |s| s else "global",
            });
    }

    const res = engine.closure.store.compute(d, ctx.allocator, task_id) catch |e| switch (e) {
        error.NoSeeds => exit.die(
            ctx,
            error.InvalidInput,
            "closure compute: task {d} declares no path-level touches (task_touch_paths); nothing to compute",
            .{task_id},
        ),
        else => exit.die(ctx, e, "closure compute: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        const out = ComputeJSON{
            .task_id = res.task_id,
            .seeds = res.seeds,
            .modify = res.modify,
            .reference = res.reference,
            .transitive = res.transitive,
            .rows_written = res.rows_written,
            .extractor_version = engine.closure.store.extractor_version,
        };
        try std.json.Stringify.value(out, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print(
            "closure computed for task {d}: {d} seed(s) -> {d} rows " ++
                "(modify={d} reference={d} transitive={d}); extractor={s}\n",
            .{
                res.task_id,
                res.seeds,
                res.rows_written,
                res.modify,
                res.reference,
                res.transitive,
                engine.closure.store.extractor_version,
            },
        );
    }
}
