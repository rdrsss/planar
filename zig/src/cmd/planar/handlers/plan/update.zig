//! handlers/plan/update — `planar plan update <plan-id> [--title] [--status] …`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
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

    // Advisory — never a refusal — when closing a plan that still has open
    // descendants.
    //
    // `plan closeout` gates on tasks-terminal + descendants-terminal + no
    // live claims. This verb gates on nothing and reaches the same state, so
    // it is the ungated path to `done`. 11 anchor plans in the operator's DB
    // were closed this way with 27 open child plans between them; nothing
    // resurfaced those children, and milestones that were fully delivered sat
    // open for months because their parent already looked closed.
    //
    // Warn rather than refuse: closing a parent whose remaining milestones are
    // moot is a legitimate move, and only the operator can tell that from an
    // accident. Same reasoning as the unguarded link verbs (CLAUDE.md
    // §Cross-scope guard) and their orphan-repo advisory.
    if (patch.status) |st| {
        if ((st == .done or st == .abandoned) and !args.json) {
            warnOpenDescendants(ctx, d, id);
        }
    }

    const plan = engine.planning.plan.update(d, ctx.allocator, id, patch) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no plan with id {d}", .{id}),
        else => exit.die(ctx, e, "plan update: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.plan, plan, .{ .json = args.json });
}

/// warnOpenDescendants emits an advisory when `plan_id` still has descendant
/// plans in a non-terminal state. Recursive, matching the closeout gate's
/// definition of "descendant" rather than only direct children.
///
/// Best-effort: any query failure is silent. A diagnostic must never turn a
/// legitimate status update into a hard error.
fn warnOpenDescendants(ctx: anytype, d: anytype, plan_id: i64) void {
    var stmt = d.prepare(
        \\with recursive desc_plans(id) as (
        \\  select id from plans where parent_plan_id = ?
        \\  union all
        \\  select p.id from plans p join desc_plans dp on p.parent_plan_id = dp.id
        \\)
        \\select count(*) from plans
        \\where id in (select id from desc_plans)
        \\  and status in ('draft','active','paused')
    ) catch return;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return;
    const open: i64 = switch (stmt.step() catch return) {
        .done => return,
        .row => stmt.columnInt(0),
    };
    if (open == 0) return;

    ctx.stderr.print(
        "warning: plan {d} still has {d} open descendant plan(s); closing it here " ++
            "does not close them, and nothing will resurface them\n" ++
            "         `planar plan closeout {d}` gates on descendants and reports what blocks\n",
        .{ plan_id, open, plan_id },
    ) catch return;
}
