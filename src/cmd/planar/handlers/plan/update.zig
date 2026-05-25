//! handlers/plan/update — `planar plan update <plan-id> [--title] [--status] …`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "update" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan id must be an integer, got '{s}'", .{args.plan_id});

    var patch: engine.planning.plan.UpdateArgs = .{
        .title = args.title,
        .slug = args.slug,
        .summary = args.summary,
        .scope = args.scope,
    };
    if (args.parent) |pid| {
        if (pid == 0) {
            patch.clear_parent = true;
        } else {
            patch.parent_plan_id = pid;
        }
    }
    if (args.status) |s| {
        patch.status = engine.planning.plan.Status.fromText(s) orelse
            exit.die(ctx, error.InvalidStatus, "unknown status '{s}'", .{s});
    }

    const plan = engine.planning.plan.update(d, ctx.allocator, id, patch) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no plan with id {d}", .{id}),
        else => exit.die(ctx, e, "plan update: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.plan, plan, .{ .json = args.json });
}
