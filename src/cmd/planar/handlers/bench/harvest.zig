//! handlers/bench/harvest — `planar bench harvest <run-uid>
//!   --task <id> --worktree <path> [--base <sha> --head <sha>]`
//!
//! Runs `git diff --name-only` against the specified worktree and
//! inserts one `run_touches kind='actual'` row per distinct changed
//! path. Without --base/--head the diff is against HEAD (working tree).
//! With both --base and --head the diff spans the range base..head.
//!
//! Prints the count of touch rows written.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "bench", "harvest" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // --base and --head must appear together or not at all.
    if ((args.base == null) != (args.head == null)) {
        exit.die(
            ctx,
            error.InvalidInput,
            "bench harvest: --base and --head must be supplied together",
            .{},
        );
    }

    // Look up the run by uid.
    const run = engine.runs.lifecycle.showByUid(d, ctx.allocator, args.run_uid) catch |e| switch (e) {
        error.NotFound => exit.die(
            ctx,
            error.NotFound,
            "bench harvest: run '{s}' not found",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "bench harvest: {s}", .{@errorName(e)}),
    };
    defer engine.runs.lifecycle.deinit(run, ctx.allocator);

    const spec: engine.runs.harvest.DiffSpec = if (args.base) |base|
        .{ .range = .{ .base = base, .head = args.head.? } }
    else
        .working_tree;

    const n = engine.runs.harvest.harvest(d, ctx.allocator, ctx.io, .{
        .worktree = args.worktree,
        .run_id = run.id,
        .task_id = args.task,
        .spec = spec,
    }) catch |e| switch (e) {
        error.GitFailed => exit.die(
            ctx,
            e,
            "bench harvest: git diff failed in worktree '{s}'",
            .{args.worktree},
        ),
        else => exit.die(ctx, e, "bench harvest: {s}", .{@errorName(e)}),
    };

    try ctx.stdout.print("{d}\n", .{n});
}
