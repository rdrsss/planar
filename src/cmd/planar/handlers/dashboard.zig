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
//! `engine.runtime.agentactivity.json.writeClaim` and includes the
//! locality columns (`repo_root`, `branch`, `head_sha_at_claim`,
//! `dirty_at_claim`) plus the worktree columns. The integration
//! suite asserts on those columns specifically.

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

    const stale_claims = listStaleClaims(d, ctx.allocator) catch |e|
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
        try agentactivity.json.writeClaim(w, c);
    }
    try w.print("],\"stale\":[", .{});
    for (stale_claims, 0..) |c, i| {
        if (i > 0) try w.print(",", .{});
        try agentactivity.json.writeClaim(w, c);
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

/// List every claim whose status is `stale` (reconcile-marked) OR
/// `active` with an expired lease. The latter set is "stale in
/// practice" — reconcile has not run yet but the claim is no longer
/// honored. The dashboard surfaces both so the operator can decide
/// whether to abort or wait for the next reconcile pass.
fn listStaleClaims(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]agentactivity.types.Claim {
    var stmt = d.prepare(
        \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\       status, vendor, vendor_session_id, role, model,
        \\       worktree_id, worktree_path,
        \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
        \\       purpose, base_ref,
        \\       claimed_at, last_heartbeat_at, lease_expires_at,
        \\       released_at, release_reason
        \\from agent_work_claims
        \\where status = 'stale'
        \\   or (status = 'active' and lease_expires_at < strftime('%Y-%m-%dT%H:%M:%fZ','now'))
        \\order by claimed_at desc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(agentactivity.types.Claim) = .empty;
    errdefer {
        for (out.items) |c| c.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readClaimRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Read one `agent_work_claims` row off `stmt`. Column order must
/// match the SELECT in `listStaleClaims`. Kept private here because
/// the engine's `store.zig` already has a near-identical helper, but
/// it's private to that module. Lifting it is overkill for one
/// caller.
fn readClaimRow(
    stmt: *db.sqlite.Stmt,
    allocator: std.mem.Allocator,
) !agentactivity.types.Claim {
    const types = agentactivity.types;

    const kind_text = try stmt.columnTextAlloc(3, allocator);
    defer allocator.free(kind_text);
    const kind = types.EntityKind.fromText(kind_text) orelse return error.QueryFailed;

    const scope_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(scope_text);
    const scope = types.ClaimScope.fromText(scope_text) orelse return error.QueryFailed;

    const status_text = try stmt.columnTextAlloc(6, allocator);
    defer allocator.free(status_text);
    const status = types.ClaimStatus.fromText(status_text) orelse return error.QueryFailed;

    const dirty_opt = try stmt.columnTextOpt(16, allocator);
    var dirty: ?types.Dirty = null;
    if (dirty_opt) |d_text| {
        defer allocator.free(d_text);
        dirty = types.Dirty.fromText(d_text);
    }

    return .{
        .id = stmt.columnInt(0),
        .claim_token = try stmt.columnTextAlloc(1, allocator),
        .session_id = stmt.columnInt(2),
        .entity_kind = kind,
        .entity_id = stmt.columnInt(4),
        .claim_scope = scope,
        .status = status,
        .vendor = try stmt.columnTextAlloc(7, allocator),
        .vendor_session_id = try stmt.columnTextOpt(8, allocator),
        .role = try stmt.columnTextOpt(9, allocator),
        .model = try stmt.columnTextOpt(10, allocator),
        .worktree_id = stmt.columnIntOpt(11),
        .worktree_path = try stmt.columnTextOpt(12, allocator),
        .repo_root = try stmt.columnTextOpt(13, allocator),
        .branch = try stmt.columnTextOpt(14, allocator),
        .head_sha_at_claim = try stmt.columnTextOpt(15, allocator),
        .dirty_at_claim = dirty,
        .purpose = try stmt.columnTextOpt(17, allocator),
        .base_ref = try stmt.columnTextOpt(18, allocator),
        .claimed_at = try stmt.columnTextAlloc(19, allocator),
        .last_heartbeat_at = try stmt.columnTextAlloc(20, allocator),
        .lease_expires_at = try stmt.columnTextAlloc(21, allocator),
        .released_at = try stmt.columnTextOpt(22, allocator),
        .release_reason = try stmt.columnTextOpt(23, allocator),
    };
}
