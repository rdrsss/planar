//! handlers/dashboard.zig — `planar dashboard [--scope <s>] [--agents] [--json]`
//!
//! Operator situational-awareness view. Without `--agents` the
//! dashboard is a lightweight roll-up of in-flight plans (status in
//! draft/active/paused). With `--agents` it folds in the live claim
//! state from `agent_work_claims` plus a per-plan "next available work"
//! tally — the operator's read surface on agent coordination state.
//!
//! Lives on the `planar` (operator) binary by design. The `planar
//! agent` subcommand namespace does not exist; agent observability is
//! split between (a) operator fold-ins like this one, (b) the
//! `planar-watch` viewer binary (M8), and (c) the `planar-agent`
//! ritual binary (M2).
//!
//! JSON shape (tech spec § "JSON shapes"):
//!   Without --agents:
//!     { active_plans: [Plan] }
//!   With --agents:
//!     { active_plans: [Plan],
//!       claims: { active: [ClaimRow], stale: [ClaimRow] },
//!       next_available_by_plan: { "<plan_id>": [Task] } }
//!
//! `ClaimRow` is the canonical shape from
//! `engine.runtime.agentactivity.json.writeClaimWithActivity` and
//! includes the locality columns (`repo_root`, `branch`,
//! `head_sha_at_claim`, `dirty_at_claim`), the worktree columns, and
//! (plan 467 M3 task 3060) the `latest_action` field matching the shape
//! emitted by `planar-watch ps --json`. The integration suite asserts on
//! those columns specifically.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

const agentactivity = engine.runtime.agentactivity;
const plan_mod = engine.planning.plan;
const task_mod = engine.planning.task;

pub const verb: cli.Cmd = .{
    .name = "dashboard",
    .desc = "Operator situational-awareness view of in-flight plans (and, with --agents, live claims).",
    .long_desc = "Roll-up of in-flight plans in the current scope.\n\n  --agents folds in the live claim state from agent_work_claims —\n  active claims, stale claims, and the per-plan 'next available'\n  task list. Without --agents the dashboard is a plain plan summary.\n\n  This is the operator's read surface for agent activity; the\n  `planar agent` subcommand namespace does not exist by design.\n  See `planar-agent` for the ritual (claim/heartbeat/complete) and\n  `planar-watch` for the live streaming view.",
    .flags = &.{
        .{ .long = "--scope", .kind = .string, .desc = "Limit to a single scope slug" },
        .{ .long = "--agents", .kind = .bool, .default = .{ .bool = false }, .desc = "Fold in live claim state + next-available-work per plan" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"dashboard"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e|
        exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});

    // Pull active plans (draft / active / paused) honoring optional --scope.
    const plans = plan_mod.list(d, ctx.allocator, .{
        .statuses = &.{ .draft, .active, .paused },
        .scope = args.scope,
    }) catch |e| exit.die(ctx, e, "plan list: {s}", .{@errorName(e)});
    defer plan_mod.deinitMany(plans, ctx.allocator);

    if (!args.agents) {
        if (args.json) {
            try emitJsonPlainPlans(ctx.stdout, plans);
        } else {
            try emitTextPlainPlans(ctx.stdout, plans);
        }
        return;
    }

    // --agents: gather claims and next-available-per-plan.
    const all_claims = agentactivity.store.listActive(d, ctx.allocator, null) catch |e|
        exit.die(ctx, e, "claims list: {s}", .{@errorName(e)});
    defer agentactivity.types.Claim.deinitMany(all_claims, ctx.allocator);

    const stale_claims = agentactivity.store.listStale(d, ctx.allocator) catch |e|
        exit.die(ctx, e, "stale claims list: {s}", .{@errorName(e)});
    defer agentactivity.types.Claim.deinitMany(stale_claims, ctx.allocator);

    if (args.json) {
        try emitJsonAgents(ctx.stdout, d, ctx.allocator, plans, all_claims, stale_claims);
    } else {
        try emitTextAgents(ctx.stdout, d, ctx.allocator, plans, all_claims, stale_claims);
    }
}

// =========================================================================
// Plain-plans rendering (no --agents).
// =========================================================================

fn emitTextPlainPlans(w: *std.Io.Writer, plans: []const plan_mod.Plan) !void {
    try w.print("active plans: {d}\n", .{plans.len});
    for (plans) |p| {
        try w.print(
            "  plan:{d}  [{s}]  {s}\n",
            .{ p.id, @tagName(p.status), p.title },
        );
    }
}

fn emitJsonPlainPlans(w: *std.Io.Writer, plans: []const plan_mod.Plan) !void {
    try w.print("{{\"active_plans\":[", .{});
    for (plans, 0..) |p, i| {
        if (i > 0) try w.print(",", .{});
        try writePlan(w, p);
    }
    try w.print("]}}\n", .{});
}

// =========================================================================
// --agents rendering.
// =========================================================================

fn emitTextAgents(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plans: []const plan_mod.Plan,
    active_claims: []const agentactivity.types.Claim,
    stale_claims: []const agentactivity.types.Claim,
) !void {
    try w.print(
        "active plans: {d}    active claims: {d}    stale claims: {d}\n",
        .{ plans.len, active_claims.len, stale_claims.len },
    );
    for (plans) |p| {
        try w.print(
            "  plan:{d}  [{s}]  {s}\n",
            .{ p.id, @tagName(p.status), p.title },
        );
    }
    if (active_claims.len > 0) {
        try w.print("active claims:\n", .{});
        for (active_claims) |c| try renderClaimLine(w, c);
    }
    if (stale_claims.len > 0) {
        try w.print("stale claims:\n", .{});
        for (stale_claims) |c| try renderClaimLine(w, c);
    }
    try w.print("next available by plan:\n", .{});
    for (plans) |p| {
        const nw = agentactivity.store.nextWork(d, allocator, p.id) catch continue;
        defer nw.deinit(allocator);
        var available_count: usize = 0;
        for (nw.rows) |r| if (r.bucket == .available) {
            available_count += 1;
        };
        try w.print("  plan:{d}  available:{d}\n", .{ p.id, available_count });
    }
}

fn renderClaimLine(w: *std.Io.Writer, c: agentactivity.types.Claim) !void {
    const branch = c.branch orelse "?";
    const sha_full = c.head_sha_at_claim orelse "?";
    const sha = if (sha_full.len >= 8) sha_full[0..8] else sha_full;
    const dirty_text = if (c.dirty_at_claim) |dv| dv.toText() else "unknown";
    const repo = c.repo_root orelse "?";
    try w.print(
        "  {s}:{d}  vendor:{s}  branch:{s}  sha:{s}  dirty:{s}  repo:{s}  token:{s}\n",
        .{ c.entity_kind.toText(), c.entity_id, c.vendor, branch, sha, dirty_text, repo, c.claim_token },
    );
}

fn emitJsonAgents(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plans: []const plan_mod.Plan,
    active_claims: []const agentactivity.types.Claim,
    stale_claims: []const agentactivity.types.Claim,
) !void {
    try w.print("{{\"active_plans\":[", .{});
    for (plans, 0..) |p, i| {
        if (i > 0) try w.print(",", .{});
        try writePlan(w, p);
    }
    try w.print("]", .{});

    try w.print(",\"claims\":{{\"active\":[", .{});
    for (active_claims, 0..) |c, i| {
        if (i > 0) try w.print(",", .{});
        const scope = agentactivity.store.resolveClaimScope(d, allocator, c);
        defer scope.deinit(allocator);
        // Plan 467 M3 task 3060: enrich dashboard claims with latest_action,
        // matching the shape that planar-watch ps --json emits (task 3056).
        const action_row = agentactivity.store.latestActionForClaim(d, allocator, c.id) catch null;
        defer if (action_row) |a| a.deinit(allocator);
        const action_info: ?agentactivity.json.LatestActionInfo = if (action_row) |a| .{
            .kind = a.action_kind.toText(),
            .summary = a.summary,
            .started_at = a.started_at,
        } else null;
        try agentactivity.json.writeClaimWithActivity(w, c, scope, true, action_info);
    }
    try w.print("],\"stale\":[", .{});
    for (stale_claims, 0..) |c, i| {
        if (i > 0) try w.print(",", .{});
        const scope = agentactivity.store.resolveClaimScope(d, allocator, c);
        defer scope.deinit(allocator);
        const action_row = agentactivity.store.latestActionForClaim(d, allocator, c.id) catch null;
        defer if (action_row) |a| a.deinit(allocator);
        const action_info: ?agentactivity.json.LatestActionInfo = if (action_row) |a| .{
            .kind = a.action_kind.toText(),
            .summary = a.summary,
            .started_at = a.started_at,
        } else null;
        try agentactivity.json.writeClaimWithActivity(w, c, scope, true, action_info);
    }
    try w.print("]}}", .{});

    try w.print(",\"next_available_by_plan\":{{", .{});
    var first = true;
    for (plans) |p| {
        const nw = agentactivity.store.nextWork(d, allocator, p.id) catch continue;
        defer nw.deinit(allocator);
        if (!first) try w.print(",", .{});
        first = false;
        try w.print("\"{d}\":[", .{p.id});
        var task_first = true;
        for (nw.rows) |r| {
            if (r.bucket != .available) continue;
            if (!task_first) try w.print(",", .{});
            task_first = false;
            const task = task_mod.show(d, allocator, r.task_id) catch continue;
            defer task_mod.deinit(task, allocator);
            try agentactivity.json.writeTask(w, task);
        }
        try w.print("]", .{});
    }
    try w.print("}}}}\n", .{});
}

// =========================================================================
// Helpers.
// =========================================================================

/// Render a Plan struct to JSON with the canonical
/// `planar plan show --json` field shape. Hand-rolled because Plan's
/// status enum needs to render textually.
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
