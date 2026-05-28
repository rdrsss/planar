//! handlers/plans — `planar-watch plans [--in-flight-only] [--json] [--follow] [--interval D]`
//!
//! JSON shape (tech-spec § "JSON shapes"):
//!   { generated_at: ISO8601,
//!     plans: [{ plan: Plan, in_flight: bool, active_claims: int,
//!               active_actions: int, last_event_at: ISO8601 | null }] }
//!
//! "In-flight" = at least one active, unexpired claim OR at least one
//! action still open (ended_at is NULL) on a task within the plan.
//! Idle plans (zero active claims AND zero open actions) drop out
//! when --in-flight-only is set.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const follow = @import("follow.zig");
const ps = @import("ps.zig");

const plan_mod = engine.planning.plan;

pub const verb: cli.Cmd = .{
    .name = "plans",
    .desc = "List plans with in-flight agent work.",
    .long_desc = "Each row pairs a plan with its in-flight summary:\n" ++
        "    active_claims  — claims with status='active' and\n" ++
        "                     lease_expires_at >= now() targeting any task\n" ++
        "                     under the plan.\n" ++
        "    active_actions — agent_actions rows with ended_at IS NULL\n" ++
        "                     whose entity_kind/entity_id refer to a task\n" ++
        "                     under the plan.\n" ++
        "    last_event_at  — max of claim claimed_at / heartbeat /\n" ++
        "                     released_at and action started_at /\n" ++
        "                     ended_at across the plan's tasks; null\n" ++
        "                     when no events recorded.\n\n" ++
        "  --in-flight-only drops plans where active_claims=0 AND\n" ++
        "  active_actions=0.",
    .flags = &.{
        .{ .long = "--in-flight-only", .kind = .bool, .default = .{ .bool = false }, .desc = "Skip plans with no live work" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--follow", .kind = .bool, .default = .{ .bool = false }, .desc = "Stream snapshots until SIGINT" },
        .{ .long = "--interval", .kind = .string, .desc = "Poll interval for --follow (default 1s)" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"plans"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const interval_ns = ps.parseIntervalOrDefault(args.interval);
    if (args.follow) follow.installSigintHandler();

    var live_d = d;
    while (true) {
        emitOnce(ctx.stdout, live_d, ctx.allocator, args) catch |e|
            exit.die(ctx, e, "plans: {s}", .{@errorName(e)});
        try ctx.stdout.flush();
        if (!args.follow) return;
        if (follow.shouldStop()) return;
        follow.interruptibleSleep(interval_ns);
        if (follow.shouldStop()) return;
        live_d = runtime.ensureDbStrictReadOnly() catch |e|
            exit.die(ctx, e, "{s}", .{@errorName(e)});
    }
}

fn emitOnce(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: anytype,
) !void {
    // List ALL plans (not just active) so the operator sees in-flight
    // work on paused / draft plans too. Caller can narrow via
    // --in-flight-only.
    const plans = plan_mod.list(d, allocator, .{
        .statuses = &.{ .draft, .active, .paused, .done, .abandoned },
    }) catch return error.QueryFailed;
    defer plan_mod.deinitMany(plans, allocator);

    if (args.json) {
        try w.print("{{\"generated_at\":", .{});
        try ps.writeNowIso(w);
        try w.print(",\"plans\":[", .{});
    } else {
        try w.print("plans: {d}\n", .{plans.len});
    }

    var first = true;
    for (plans) |p| {
        const stats = try collectPlanStats(d, allocator, p.id);
        defer if (stats.last_event_at) |s| allocator.free(s);
        const in_flight = stats.active_claims > 0 or stats.active_actions > 0;
        if (args.in_flight_only and !in_flight) continue;

        if (args.json) {
            if (!first) try w.print(",", .{});
            first = false;
            try w.print("{{\"plan\":", .{});
            try writePlan(w, p);
            try w.print(",\"in_flight\":{s}", .{if (in_flight) "true" else "false"});
            try w.print(",\"active_claims\":{d}", .{stats.active_claims});
            try w.print(",\"active_actions\":{d}", .{stats.active_actions});
            try w.print(",\"last_event_at\":", .{});
            if (stats.last_event_at) |s| {
                try std.json.Stringify.encodeJsonString(s, .{}, w);
            } else {
                try w.print("null", .{});
            }
            try w.print("}}", .{});
        } else {
            try w.print(
                "  plan:{d}  [{s}]  in_flight:{s}  claims:{d}  actions:{d}  {s}\n",
                .{
                    p.id,                           @tagName(p.status),
                    if (in_flight) "yes" else "no", stats.active_claims,
                    stats.active_actions,           p.title,
                },
            );
        }
    }

    if (args.json) try w.print("]}}\n", .{});
}

const PlanStats = struct {
    active_claims: i64,
    active_actions: i64,
    /// Owned slice — caller frees with the suite allocator. May be
    /// null if there are no recorded events on any of the plan's
    /// tasks.
    last_event_at: ?[]const u8,
};

fn collectPlanStats(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) !PlanStats {
    var active_claims: i64 = 0;
    {
        var stmt = d.prepare(
            \\select count(*) from agent_work_claims c
            \\where c.status = 'active'
            \\  and c.lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
            \\  and (
            \\    (c.entity_kind = 'plan' and c.entity_id = ?)
            \\    or (c.entity_kind = 'task' and exists (
            \\         select 1 from tasks t where t.id = c.entity_id and t.plan_id = ?))
            \\  )
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return error.QueryFailed;
        switch (stmt.step() catch return error.QueryFailed) {
            .done => {},
            .row => active_claims = stmt.columnInt(0),
        }
    }

    var active_actions: i64 = 0;
    {
        var stmt = d.prepare(
            \\select count(*) from agent_actions a
            \\where a.ended_at is null
            \\  and (
            \\    (a.entity_kind = 'plan' and a.entity_id = ?)
            \\    or (a.entity_kind = 'task' and exists (
            \\         select 1 from tasks t where t.id = a.entity_id and t.plan_id = ?))
            \\  )
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return error.QueryFailed;
        switch (stmt.step() catch return error.QueryFailed) {
            .done => {},
            .row => active_actions = stmt.columnInt(0),
        }
    }

    // last_event_at — max across all the watermark columns from claim
    // + action rows whose entity is the plan or a task under the
    // plan. NULL when no rows match.
    var last_event_at: ?[]const u8 = null;
    {
        var stmt = d.prepare(
            \\select max(event_at) from (
            \\  select max(claimed_at, last_heartbeat_at, coalesce(released_at, claimed_at)) as event_at
            \\  from agent_work_claims c
            \\  where (c.entity_kind = 'plan' and c.entity_id = ?)
            \\     or (c.entity_kind = 'task' and exists (
            \\         select 1 from tasks t where t.id = c.entity_id and t.plan_id = ?))
            \\  union all
            \\  select coalesce(ended_at, started_at) as event_at
            \\  from agent_actions a
            \\  where (a.entity_kind = 'plan' and a.entity_id = ?)
            \\     or (a.entity_kind = 'task' and exists (
            \\         select 1 from tasks t where t.id = a.entity_id and t.plan_id = ?))
            \\)
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{
            .{ .int = plan_id }, .{ .int = plan_id },
            .{ .int = plan_id }, .{ .int = plan_id },
        }) catch return error.QueryFailed;
        switch (stmt.step() catch return error.QueryFailed) {
            .done => {},
            .row => {
                if (!stmt.columnIsNull(0)) {
                    last_event_at = stmt.columnTextAlloc(0, allocator) catch null;
                }
            },
        }
    }

    return .{
        .active_claims = active_claims,
        .active_actions = active_actions,
        .last_event_at = last_event_at,
    };
}

fn writePlan(w: *std.Io.Writer, p: plan_mod.Plan) !void {
    try w.print("{{\"id\":{d}", .{p.id});
    try w.print(",\"scope_kind\":\"{s}\"", .{@tagName(p.scope_kind)});
    if (p.scope_id) |sid| {
        try w.print(",\"scope_id\":{d}", .{sid});
    } else {
        try w.print(",\"scope_id\":null", .{});
    }
    try w.print(",\"title\":", .{});
    try std.json.Stringify.encodeJsonString(p.title, .{}, w);
    try w.print(",\"slug\":", .{});
    try std.json.Stringify.encodeJsonString(p.slug, .{}, w);
    if (p.summary) |s| {
        try w.print(",\"summary\":", .{});
        try std.json.Stringify.encodeJsonString(s, .{}, w);
    } else {
        try w.print(",\"summary\":null", .{});
    }
    try w.print(",\"status\":\"{s}\"", .{@tagName(p.status)});
    if (p.parent_plan_id) |pp| {
        try w.print(",\"parent_plan_id\":{d}", .{pp});
    } else {
        try w.print(",\"parent_plan_id\":null", .{});
    }
    try w.print(",\"created_at\":", .{});
    try std.json.Stringify.encodeJsonString(p.created_at, .{}, w);
    try w.print(",\"updated_at\":", .{});
    try std.json.Stringify.encodeJsonString(p.updated_at, .{}, w);
    try w.print("}}", .{});
}
