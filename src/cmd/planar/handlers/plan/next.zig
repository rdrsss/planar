//! handlers/plan/next — `planar plan next <plan-id> [--json]`
//!
//! Returns the highest-priority eligible task on the plan. Mirrors the
//! Go archive's `plan-next` handler. Reinstated by Q236 (plan 351)
//! after the M20 cutover dropped it; the orchestrator skill / agent
//! docs reference this verb as their claim-aware dispatch hook.
//!
//! Eligibility: tasks whose status is in {todo, doing, blocked} on the
//! given plan. Sort: priority ascending (1 = highest), then id. The
//! first row wins; the handler emits it as text or JSON.
//!
//! Exit 0 + empty payload when no eligible task exists. Exit 1 +
//! NotFound when the plan id itself is missing.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "next" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan_id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan id must be an integer, got '{s}'", .{args.plan_id});

    // Confirm the plan exists; otherwise NotFound (exit 1).
    {
        var stmt = d.prepare("select count(*) from plans where id = ?") catch |e|
            exit.die(ctx, e, "plan lookup prep: {s}", .{@errorName(e)});
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch |e|
            exit.die(ctx, e, "plan lookup bind: {s}", .{@errorName(e)});
        const n = switch (stmt.step() catch |e| exit.die(ctx, e, "plan lookup step: {s}", .{@errorName(e)})) {
            .done => 0,
            .row => stmt.columnInt(0),
        };
        if (n == 0) exit.die(ctx, error.NotFound, "plan {d} not found", .{plan_id});
    }

    var stmt = d.prepare(
        "select id, title, coalesce(status,''), priority" ++
            " from tasks" ++
            " where plan_id = ?" ++
            "   and status in ('todo','doing','blocked')" ++
            " order by priority, id" ++
            " limit 1",
    ) catch |e| exit.die(ctx, e, "plan next prep: {s}", .{@errorName(e)});
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch |e|
        exit.die(ctx, e, "plan next bind: {s}", .{@errorName(e)});

    switch (stmt.step() catch |e| exit.die(ctx, e, "plan next step: {s}", .{@errorName(e)})) {
        .done => {
            if (args.json) {
                try ctx.stdout.print("null\n", .{});
            } else {
                try ctx.stdout.print("(no eligible task on plan {d})\n", .{plan_id});
            }
            return;
        },
        .row => {
            const id = stmt.columnInt(0);
            const title = stmt.columnTextAlloc(1, ctx.allocator) catch |e|
                exit.die(ctx, e, "plan next title: {s}", .{@errorName(e)});
            defer ctx.allocator.free(title);
            const status = stmt.columnTextAlloc(2, ctx.allocator) catch |e|
                exit.die(ctx, e, "plan next status: {s}", .{@errorName(e)});
            defer ctx.allocator.free(status);
            const priority = stmt.columnInt(3);

            if (args.json) {
                try ctx.stdout.print(
                    "{{\"id\":{d},\"title\":",
                    .{id},
                );
                try std.json.Stringify.encodeJsonString(title, .{}, ctx.stdout);
                try ctx.stdout.print(
                    ",\"status\":\"{s}\",\"priority\":{d},\"plan_id\":{d}}}\n",
                    .{ status, priority, plan_id },
                );
            } else {
                try ctx.stdout.print(
                    "task:{d}  {s}  [{s}, pri:{d}]\n",
                    .{ id, title, status, priority },
                );
            }
        },
    }
}
