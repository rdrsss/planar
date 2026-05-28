//! handlers/actions — `planar-watch actions [--vendor v] [--kind k] [--entity kind:id] [--plan id] [--task id] [--limit N] [--json] [--follow] [--interval D]`
//!
//! JSON shape (tech-spec § "JSON shapes"):
//!   { generated_at: ISO8601, actions: [ActionRow] }
//!
//! Returns rows from agent_actions in started_at descending order.
//! Filters are applied at the SQL layer where possible; the
//! `--entity` flag accepts the canonical `kind:id` form (e.g.
//! `task:42`) and rewrites into entity_kind / entity_id predicates.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const follow = @import("follow.zig");
const planfilter = @import("planfilter.zig");
const ps = @import("ps.zig");

const agentactivity = engine.runtime.agentactivity;

pub const verb: cli.Cmd = .{
    .name = "actions",
    .desc = "List agent_actions rows with optional filters.",
    .long_desc = "Returns agent_actions rows ordered by started_at descending.\n\n" ++
        "  --kind     : action_kind filter (coder, reviewer, tool_call, etc.).\n" ++
        "  --entity   : restrict to one entity, `kind:id` form (e.g. `task:42`).\n" ++
        "  --plan     : restrict to actions on the plan, or on tasks/plan_steps belonging to it.\n" ++
        "  --task     : restrict to actions whose entity_kind=task, entity_id=N.\n" ++
        "  --vendor   : vendor filter.\n" ++
        "  --limit    : cap row count (default 100).",
    .flags = &.{
        .{ .long = "--vendor", .kind = .string, .desc = "Vendor filter" },
        .{ .long = "--kind", .kind = .string, .desc = "action_kind filter" },
        .{ .long = "--entity", .kind = .string, .desc = "Restrict to one entity, kind:id form" },
        .{ .long = "--plan", .kind = .int, .desc = "Filter by plan id" },
        .{ .long = "--task", .kind = .int, .desc = "Filter by task id" },
        .{ .long = "--limit", .kind = .int, .desc = "Row cap (default 100)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--follow", .kind = .bool, .default = .{ .bool = false }, .desc = "Stream snapshots until SIGINT" },
        .{ .long = "--interval", .kind = .string, .desc = "Poll interval for --follow (default 1s)" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"actions"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const interval_ns = ps.parseIntervalOrDefault(args.interval);
    if (args.follow) follow.installSigintHandler();

    while (true) {
        emitOnce(ctx.stdout, d, ctx.allocator, args) catch |e|
            exit.die(ctx, e, "actions: {s}", .{@errorName(e)});
        try ctx.stdout.flush();
        if (!args.follow) return;
        if (follow.shouldStop()) return;
        follow.interruptibleSleep(interval_ns);
        if (follow.shouldStop()) return;
    }
}

fn emitOnce(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: anytype,
) !void {
    const limit = if (args.limit) |n| n else 100;

    // Decompose --entity if supplied (kind:id form).
    var entity_kind_filter: ?[]const u8 = null;
    var entity_id_filter: ?i64 = null;
    if (args.entity) |e_text| {
        const colon = std.mem.indexOf(u8, e_text, ":") orelse return error.InvalidInput;
        entity_kind_filter = e_text[0..colon];
        entity_id_filter = std.fmt.parseInt(i64, e_text[colon + 1 ..], 10) catch return error.InvalidInput;
    }
    // --task is sugar for --entity task:N.
    if (args.task) |tid| {
        entity_kind_filter = "task";
        entity_id_filter = tid;
    }

    const rows = try listActions(d, allocator, limit);
    defer agentactivity.types.Action.deinitMany(rows, allocator);

    if (args.json) {
        try w.print("{{\"generated_at\":", .{});
        try ps.writeNowIso(w);
        try w.print(",\"actions\":[", .{});
        var first = true;
        for (rows) |a| {
            if (!actionMatches(d, a, args, entity_kind_filter, entity_id_filter)) continue;
            if (!first) try w.print(",", .{});
            first = false;
            try agentactivity.json.writeAction(w, a);
        }
        try w.print("]}}\n", .{});
    } else {
        try w.print("actions: {d}\n", .{rows.len});
        for (rows) |a| {
            if (!actionMatches(d, a, args, entity_kind_filter, entity_id_filter)) continue;
            const ent_kind: []const u8 = if (a.entity_kind) |k| k.toText() else "-";
            const ent_id: i64 = a.entity_id orelse 0;
            try w.print(
                "  action:{d}  kind:{s}  vendor:{s}  entity:{s}:{d}  started:{s}\n",
                .{ a.id, a.action_kind.toText(), a.vendor, ent_kind, ent_id, a.started_at },
            );
        }
    }
}

fn actionMatches(
    d: *db.sqlite.Db,
    a: agentactivity.types.Action,
    args: anytype,
    entity_kind_filter: ?[]const u8,
    entity_id_filter: ?i64,
) bool {
    if (args.vendor) |v| {
        if (!std.mem.eql(u8, a.vendor, v)) return false;
    }
    if (args.kind) |k| {
        if (!std.mem.eql(u8, a.action_kind.toText(), k)) return false;
    }
    if (entity_kind_filter) |ek| {
        const k = a.entity_kind orelse return false;
        if (!std.mem.eql(u8, k.toText(), ek)) return false;
    }
    if (entity_id_filter) |eid| {
        const id = a.entity_id orelse return false;
        if (id != eid) return false;
    }
    if (args.plan) |pid| {
        // Widened --plan: plan-direct + task-on-plan + plan_step-on-plan.
        const k = a.entity_kind orelse return false;
        const id = a.entity_id orelse return false;
        if (!planfilter.actionBelongsToPlan(d, k, id, pid)) return false;
    }
    return true;
}

fn listActions(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    limit: i64,
) ![]agentactivity.types.Action {
    var sql_buf: [768]u8 = undefined;
    const sql = std.fmt.bufPrintZ(&sql_buf,
        \\select id, session_id, session_entry_id, parent_action_id, claim_id,
        \\       action_kind, entity_kind, entity_id,
        \\       vendor, vendor_role, model,
        \\       started_at, ended_at, outcome, summary,
        \\       head_sha, dirty
        \\from agent_actions
        \\order by started_at desc, id desc
        \\limit {d}
    , .{limit}) catch return error.QueryFailed;

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(agentactivity.types.Action) = .empty;
    errdefer {
        for (out.items) |a| a.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readActionRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn readActionRow(
    stmt: *db.sqlite.Stmt,
    allocator: std.mem.Allocator,
) !agentactivity.types.Action {
    const types = agentactivity.types;

    const kind_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(kind_text);
    const kind = types.ActionKind.fromText(kind_text) orelse return error.QueryFailed;

    var ent_kind: ?types.ActionEntityKind = null;
    if (try stmt.columnTextOpt(6, allocator)) |ek_text| {
        defer allocator.free(ek_text);
        ent_kind = types.ActionEntityKind.fromText(ek_text);
    }

    var outcome: ?types.Outcome = null;
    if (try stmt.columnTextOpt(13, allocator)) |o_text| {
        defer allocator.free(o_text);
        outcome = types.Outcome.fromText(o_text);
    }

    var dirty: ?types.Dirty = null;
    if (try stmt.columnTextOpt(16, allocator)) |d_text| {
        defer allocator.free(d_text);
        dirty = types.Dirty.fromText(d_text);
    }

    return .{
        .id = stmt.columnInt(0),
        .session_id = stmt.columnInt(1),
        .session_entry_id = stmt.columnIntOpt(2),
        .parent_action_id = stmt.columnIntOpt(3),
        .claim_id = stmt.columnIntOpt(4),
        .action_kind = kind,
        .entity_kind = ent_kind,
        .entity_id = stmt.columnIntOpt(7),
        .vendor = try stmt.columnTextAlloc(8, allocator),
        .vendor_role = try stmt.columnTextOpt(9, allocator),
        .model = try stmt.columnTextOpt(10, allocator),
        .started_at = try stmt.columnTextAlloc(11, allocator),
        .ended_at = try stmt.columnTextOpt(12, allocator),
        .outcome = outcome,
        .summary = try stmt.columnTextOpt(14, allocator),
        .head_sha = try stmt.columnTextOpt(15, allocator),
        .dirty = dirty,
    };
}
