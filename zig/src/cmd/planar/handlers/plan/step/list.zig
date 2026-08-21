//! handlers/plan/step/list — `planar plan step list <plan-id> [--json]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "step", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan_id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan-id must be an integer, got '{s}'", .{args.plan_id});

    const steps = engine.planning.plan_step.list(d, ctx.allocator, plan_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no plan with id {d}", .{plan_id}),
        else => exit.die(ctx, e, "plan step list: {s}", .{@errorName(e)}),
    };
    defer engine.planning.plan_step.deinitMany(steps, ctx.allocator);

    if (args.json) {
        // Emit as a JSON array.
        try ctx.stdout.print("[", .{});
        for (steps, 0..) |step, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try emitStepJSON(ctx, step);
        }
        try ctx.stdout.print("]\n", .{});
    } else {
        if (steps.len == 0) {
            try ctx.stdout.print("no steps for plan {d}\n", .{plan_id});
            return;
        }
        try ctx.stdout.print("{s:<5}  {s:<10}  {s:<10}  {s}\n", .{ "ord", "id", "status", "body" });
        for (steps) |step| {
            const task_suffix = if (step.task_id) |tid| tid else @as(i64, 0);
            _ = task_suffix;
            try ctx.stdout.print("{d:<5}  {d:<10}  {s:<10}  {s}", .{
                step.ordinal, step.id, step.status.toText(), step.body,
            });
            if (step.task_id) |tid| {
                try ctx.stdout.print("  [task:{d}]", .{tid});
            }
            try ctx.stdout.print("\n", .{});
        }
    }
}

fn emitStepJSON(ctx: *const runtime.Ctx, step: engine.planning.plan_step.Step) !void {
    const StepJSON = struct {
        id: i64,
        plan_id: i64,
        ordinal: i64,
        body: []const u8,
        status: []const u8,
        task_id: ?i64,
        created_at: []const u8,
        updated_at: []const u8,
    };
    const row = StepJSON{
        .id = step.id,
        .plan_id = step.plan_id,
        .ordinal = step.ordinal,
        .body = step.body,
        .status = step.status.toText(),
        .task_id = step.task_id,
        .created_at = step.created_at,
        .updated_at = step.updated_at,
    };
    try std.json.Stringify.value(row, .{}, ctx.stdout);
}
