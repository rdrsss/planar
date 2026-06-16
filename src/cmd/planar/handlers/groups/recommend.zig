//! handlers/groups/recommend — `planar groups recommend <plan-id>
//!     [--budget <tokens>] [--json]`
//!
//! READ-ONLY operator surface for the M3 grouping arm. Loads a plan's open
//! (todo) tasks, each task's effective derived closure (the `closures` rows,
//! role modify/reference), and the task dependency DAG (`entity_links`
//! `blocks` edges), then forms **slices** that minimize closure replication
//! under a window budget via the greedy heuristic (M3.1). Reports the slices,
//! their unioned closures + per-slice cost, and the total. Writes nothing.
//!
//! Default budget: 128000 tokens (a typical large context window). Override
//! with `--budget <tokens>`.
//!
//! JSON shape (--json):
//!   { "plan_id": int,
//!     "budget": int,
//!     "open_tasks": int,
//!     "slices": [ { "task_ids": [int],
//!                   "union_symbols": [str],
//!                   "cost": int } ],
//!     "summary": { "slices": int, "total_cost": int } }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

const load = engine.grouping.load;

/// Default per-slice token budget when `--budget` is not supplied: a typical
/// large context window. Documented in the verb help and the module docblock.
const default_budget: u32 = 128_000;

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "groups", "recommend" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan_id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan id must be an integer, got '{s}'", .{args.plan_id});

    const budget: u32 = if (args.budget) |b|
        (std.fmt.parseInt(u32, b, 10) catch
            exit.die(ctx, error.InvalidInput, "--budget must be a non-negative integer, got '{s}'", .{b}))
    else
        default_budget;

    var rec = load.recommend(d, ctx.allocator, plan_id, budget) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "plan {d} not found", .{plan_id}),
        else => exit.die(ctx, e, "groups recommend: {s}", .{@errorName(e)}),
    };
    defer rec.deinit(ctx.allocator);

    if (args.json) {
        try emitJSON(ctx, rec);
    } else {
        try emitText(ctx, rec);
    }
}

fn emitJSON(ctx: *const runtime.Ctx, rec: load.Recommendation) !void {
    const w = ctx.stdout;
    try w.print(
        "{{\"plan_id\":{d},\"budget\":{d},\"open_tasks\":{d},\"slices\":[",
        .{ rec.plan_id, rec.budget, rec.open_tasks },
    );
    for (rec.grouping.slices, 0..) |s, i| {
        if (i != 0) try w.print(",", .{});
        try w.print("{{\"task_ids\":[", .{});
        for (s.task_ids, 0..) |id, j| {
            if (j != 0) try w.print(",", .{});
            try w.print("{d}", .{id});
        }
        try w.print("],\"union_symbols\":[", .{});
        for (s.union_symbols, 0..) |sym, j| {
            if (j != 0) try w.print(",", .{});
            try output.writeJsonString(w, sym);
        }
        try w.print("],\"cost\":{d}}}", .{s.cost});
    }
    try w.print(
        "],\"summary\":{{\"slices\":{d},\"total_cost\":{d}}}}}\n",
        .{ rec.grouping.slices.len, rec.grouping.totalCost() },
    );
}

fn emitText(ctx: *const runtime.Ctx, rec: load.Recommendation) !void {
    const w = ctx.stdout;
    try w.print(
        "plan:{d}  budget:{d}  open:{d}  slices:{d}  total_cost:{d}\n",
        .{ rec.plan_id, rec.budget, rec.open_tasks, rec.grouping.slices.len, rec.grouping.totalCost() },
    );
    if (rec.grouping.slices.len == 0) {
        try w.print("  (no open tasks to group)\n", .{});
        return;
    }
    for (rec.grouping.slices, 0..) |s, i| {
        try w.print("slice {d}  cost:{d}  tasks:[", .{ i + 1, s.cost });
        for (s.task_ids, 0..) |id, j| {
            if (j != 0) try w.print(", ", .{});
            try w.print("{d}", .{id});
        }
        try w.print("]\n", .{});
        for (s.union_symbols) |sym| {
            try w.print("    - {s}\n", .{sym});
        }
    }
}
