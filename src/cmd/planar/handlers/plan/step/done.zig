//! handlers/plan/step/done — `planar plan step done <step-id> [--json]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("../../../runtime.zig");
const exit = @import("../../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "step", "done" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const step_id = std.fmt.parseInt(i64, args.step_id, 10) catch
        exit.die(ctx, error.InvalidInput, "step-id must be an integer, got '{s}'", .{args.step_id});

    _ = args.scope; // accepted for CLI parity; not used by the engine.

    const step = engine.planning.plan_step.markDone(d, ctx.allocator, step_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no step with id {d}", .{step_id}),
        error.InvalidTransition => exit.die(ctx, e, "step {d} is already terminal", .{step_id}),
        else => exit.die(ctx, e, "plan step done: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        try emitStepJSON(ctx, step);
    } else {
        try renderStepText(ctx, step);
    }
}

fn renderStepText(ctx: *const runtime.Ctx, step: engine.planning.plan_step.Step) !void {
    try ctx.stdout.print("id:       {d}\n", .{step.id});
    try ctx.stdout.print("plan_id:  {d}\n", .{step.plan_id});
    try ctx.stdout.print("ordinal:  {d}\n", .{step.ordinal});
    try ctx.stdout.print("status:   {s}\n", .{step.status.toText()});
    if (step.task_id) |tid| {
        try ctx.stdout.print("task_id:  {d}\n", .{tid});
    }
    try ctx.stdout.print("body:     {s}\n", .{step.body});
    try ctx.stdout.print("created:  {s}\n", .{step.created_at});
    try ctx.stdout.print("updated:  {s}\n", .{step.updated_at});
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
    try ctx.stdout.print("\n", .{});
}
