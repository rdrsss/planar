//! handlers/claims — `planar-watch claims [--vendor v] [--plan id] [--status active|stale|all] [--json] [--follow] [--interval D]`
//!
//! JSON shape (tech-spec § "JSON shapes"):
//!   { generated_at: ISO8601, claims: [ClaimRow] }
//!
//! Distinct from `ps`: `ps` is the operator's "what's running" snapshot
//! (returns active + stale buckets). `claims` is the lower-level
//! claim ledger — returns whatever rows match `--status` in one flat
//! array, no bucketing.

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
    .name = "claims",
    .desc = "List claims in the agent_work_claims ledger (filterable by status).",
    .long_desc = "Returns claim rows from agent_work_claims. The default is\n" ++
        "  --status active.\n\n" ++
        "  --status active : claim row is in 'active' state with an\n" ++
        "                    unexpired lease (default).\n" ++
        "  --status stale  : status='stale' OR an expired-lease active\n" ++
        "                    claim (matches `ps --stale`).\n" ++
        "  --status all    : every row (active, released, completed,\n" ++
        "                    aborted, stale) — the full claim ledger.",
    .flags = &.{
        .{ .long = "--vendor", .kind = .string, .desc = "Filter by vendor" },
        .{ .long = "--plan", .kind = .int, .desc = "Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)" },
        .{ .long = "--status", .kind = .string, .desc = "active (default) | stale | all" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--follow", .kind = .bool, .default = .{ .bool = false }, .desc = "Stream snapshots until SIGINT" },
        .{ .long = "--interval", .kind = .string, .desc = "Poll interval for --follow (default 1s)" },
    },
    .run = cli.handler(handle),
};

const StatusFilter = enum { active, stale, all };

fn parseStatus(opt: ?[]const u8) StatusFilter {
    const s = opt orelse return .active;
    if (std.mem.eql(u8, s, "active")) return .active;
    if (std.mem.eql(u8, s, "stale")) return .stale;
    if (std.mem.eql(u8, s, "all")) return .all;
    return .active;
}

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"claims"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const interval_ns = ps.parseIntervalOrDefault(args.interval);
    if (args.follow) follow.installSigintHandler();

    while (true) {
        emitOnce(ctx.stdout, d, ctx.allocator, args) catch |e|
            exit.die(ctx, e, "claims: {s}", .{@errorName(e)});
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
    const status = parseStatus(args.status);
    const rows = try listClaims(d, allocator, status);
    defer agentactivity.types.Claim.deinitMany(rows, allocator);

    if (args.json) {
        try w.print("{{\"generated_at\":", .{});
        try ps.writeNowIso(w);
        try w.print(",\"claims\":[", .{});
        var first = true;
        for (rows) |c| {
            if (!claimMatches(d, c, args)) continue;
            if (!first) try w.print(",", .{});
            first = false;
            try agentactivity.json.writeClaim(w, c);
        }
        try w.print("]}}\n", .{});
    } else {
        try w.print("claims: {d}\n", .{rows.len});
        for (rows) |c| {
            if (!claimMatches(d, c, args)) continue;
            try w.print(
                "  {s}:{d}  status:{s}  vendor:{s}  token:{s}\n",
                .{ c.entity_kind.toText(), c.entity_id, c.status.toText(), c.vendor, c.claim_token },
            );
        }
    }
}

fn claimMatches(d: *db.sqlite.Db, c: agentactivity.types.Claim, args: anytype) bool {
    if (args.vendor) |v| {
        if (!std.mem.eql(u8, c.vendor, v)) return false;
    }
    if (args.plan) |pid| {
        // Widened --plan: plan-direct + task-on-plan + plan_step-on-plan.
        if (!planfilter.claimBelongsToPlan(d, c.entity_kind, c.entity_id, pid)) return false;
    }
    return true;
}

fn listClaims(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    status: StatusFilter,
) ![]agentactivity.types.Claim {
    const sql: [:0]const u8 = switch (status) {
        .active =>
        \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\       status, vendor, vendor_session_id, role, model,
        \\       worktree_id, worktree_path,
        \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
        \\       purpose, base_ref,
        \\       claimed_at, last_heartbeat_at, lease_expires_at,
        \\       released_at, release_reason
        \\from agent_work_claims
        \\where status = 'active'
        \\  and lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\order by claimed_at desc
        ,
        .stale =>
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
        ,
        .all =>
        \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\       status, vendor, vendor_session_id, role, model,
        \\       worktree_id, worktree_path,
        \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
        \\       purpose, base_ref,
        \\       claimed_at, last_heartbeat_at, lease_expires_at,
        \\       released_at, release_reason
        \\from agent_work_claims
        \\order by claimed_at desc
        ,
    };

    var stmt = d.prepare(sql) catch return error.QueryFailed;
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
