//! handlers/plan/divergence — `planar plan divergence <plan-id> [--json]`
//!
//! READ-ONLY surface for the declared-vs-derived closure divergence measure
//! (decision D4, plan 636 M2.6). Computes, for every unordered pair of the
//! plan's open (todo) tasks, whether the two tasks overlap under the DECLARED
//! touch set vs. the DERIVED closure set, then reports the count of pairs
//! where the two sources disagree (flips) and the Jaccard distance between
//! the two overlap-pair sets.
//!
//! Writes nothing. Requires the plan to exist and have declared/derived data
//! in the DB; an empty plan returns all-zero counts (Jaccard = 0.0).
//!
//! JSON shape (--json):
//!   { "plan_id": int,
//!     "open_tasks": int,
//!     "pairs": int,
//!     "declared_overlaps": int,
//!     "derived_overlaps": int,
//!     "flips": int,
//!     "jaccard": float }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

const strategy = engine.planning.strategy;

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "divergence" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan_id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan id must be an integer, got '{s}'", .{args.plan_id});

    const div = strategy.divergence(d, ctx.allocator, plan_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "plan {d} not found", .{plan_id}),
        else => exit.die(ctx, e, "plan divergence: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        try ctx.stdout.print(
            "{{\"plan_id\":{d},\"open_tasks\":{d},\"pairs\":{d},\"declared_overlaps\":{d},\"derived_overlaps\":{d},\"flips\":{d},\"jaccard\":{d}}}\n",
            .{
                plan_id,
                div.open_tasks,
                div.pairs,
                div.declared_overlaps,
                div.derived_overlaps,
                div.flips,
                div.jaccard,
            },
        );
    } else {
        try ctx.stdout.print(
            "plan:{d}  open:{d}  pairs:{d}  declared_overlaps:{d}  derived_overlaps:{d}  flips:{d}  jaccard:{d:.4}\n",
            .{
                plan_id,
                div.open_tasks,
                div.pairs,
                div.declared_overlaps,
                div.derived_overlaps,
                div.flips,
                div.jaccard,
            },
        );
    }
}
