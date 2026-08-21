//! handlers/plan/recompute_status — `planar plan recompute-status (--plan <id> | --all)`
//!
//! Mirrors Go's plan_recompute_status.go:
//!   - Without --all: recomputeStatus(plan_id) for the supplied --plan.
//!   - With --all: list every plan, call recomputeStatus on each, collect results.
//! The engine is single-plan; the handler orchestrates the multi-plan --all walk.
//! (D-recompute-all-flag)

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "recompute-status" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan_flag: ?i64 = args.plan; // null when --plan not supplied
    const all_flag = args.all;

    // Validate: exactly one of --plan or --all must be given.
    if (plan_flag == null and !all_flag) {
        exit.die(ctx, error.InvalidInput, "either --plan <id> or --all is required", .{});
    }
    if (plan_flag != null and all_flag) {
        exit.die(ctx, error.InvalidInput, "--plan and --all are mutually exclusive", .{});
    }

    if (!all_flag) {
        // Single-plan mode.
        const result = engine.planning.plan.recomputeStatus(d, ctx.allocator, plan_flag.?) catch |e| switch (e) {
            error.NotFound => exit.die(ctx, e, "no plan with id {d}", .{plan_flag.?}),
            else => exit.die(ctx, e, "recompute-status: {s}", .{@errorName(e)}),
        };
        defer result.deinit(ctx.allocator);

        if (args.json) {
            try emitResultJSON(ctx, result);
        } else {
            if (result.flipped) {
                try ctx.stdout.print("plan {d}: {s} → {s}\n", .{
                    result.plan_id,
                    @tagName(result.status_before),
                    @tagName(result.status_after),
                });
            } else {
                try ctx.stdout.print("plan {d}: {s} (no change)\n", .{
                    result.plan_id,
                    @tagName(result.status_before),
                });
            }
        }
        return;
    }

    // --all: walk every plan in the DB.
    const plans = engine.planning.plan.list(d, ctx.allocator, .{}) catch |e|
        exit.die(ctx, e, "listing plans: {s}", .{@errorName(e)});
    defer engine.planning.plan.deinitMany(plans, ctx.allocator);

    var changed: usize = 0;

    for (plans) |p| {
        const result = engine.planning.plan.recomputeStatus(d, ctx.allocator, p.id) catch |e| {
            try ctx.stderr.print("plan {d}: error: {s}\n", .{ p.id, @errorName(e) });
            continue;
        };
        defer result.deinit(ctx.allocator);

        if (result.flipped) changed += 1;

        if (args.json) {
            try emitResultJSON(ctx, result);
        } else {
            if (result.flipped) {
                try ctx.stdout.print("plan {d}: {s} → {s}\n", .{
                    result.plan_id,
                    @tagName(result.status_before),
                    @tagName(result.status_after),
                });
            }
        }
    }

    if (!args.json) {
        try ctx.stdout.print("\nrecomputed {d} plans; {d} transitioned\n", .{ plans.len, changed });
    }
}

fn emitResultJSON(ctx: *const runtime.Ctx, result: engine.planning.plan.RecomputeResult) !void {
    const ResultJSON = struct {
        plan_id: i64,
        status_before: []const u8,
        status_after: []const u8,
        flipped: bool,
    };
    const row = ResultJSON{
        .plan_id = result.plan_id,
        .status_before = @tagName(result.status_before),
        .status_after = @tagName(result.status_after),
        .flipped = result.flipped,
    };
    try std.json.Stringify.value(row, .{}, ctx.stdout);
    try ctx.stdout.print("\n", .{});
}
