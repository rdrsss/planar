//! handlers/ps — `planar-watch ps [--vendor v] [--plan id] [--stale] [--json] [--follow] [--interval D]`
//!
//! Snapshot of active claims (and, with --stale, also claims whose
//! lease has expired or that reconcile has marked stale). Same JSON
//! shape as the dashboard --agents fold-in. Used by operators to
//! answer "what's running right now and where is anything wedged?"
//!
//! JSON shape (tech-spec § "JSON shapes"):
//!   {
//!     generated_at: ISO8601,
//!     active: [ClaimRow],
//!     stale:  [ClaimRow]   // present when --stale or always (matches
//!                           // the documented contract — both keys
//!                           // are always emitted, possibly with empty
//!                           // arrays so the schema is stable).
//!   }
//!
//! `--follow` is a thin wrapper: emit a JSON snapshot, sleep
//! `--interval`, emit another, until SIGINT. (Tier-1 poll.)

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const follow = @import("follow.zig");
const planfilter = @import("planfilter.zig");

const agentactivity = engine.runtime.agentactivity;

pub const verb: cli.Cmd = .{
    .name = "ps",
    .desc = "Snapshot of active (and stale) agent claims.",
    .long_desc = "Lists every currently active agent claim — one row per claim_token.\n\n" ++
        "  --stale also includes claims whose lease has expired OR whose\n" ++
        "  status is `stale` (set by `planar-agent reconcile`).\n\n" ++
        "  --vendor / --plan narrow the result.\n\n" ++
        "  --follow turns the snapshot into a streaming view (Tier-1 poll;\n" ++
        "  --interval defaults to 1s). Exits 0 on SIGINT.",
    .flags = &.{
        .{ .long = "--vendor", .kind = .string, .desc = "Filter by vendor (claude, codex, copilot, ...)" },
        .{ .long = "--plan", .kind = .int, .desc = "Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)" },
        .{ .long = "--stale", .kind = .bool, .default = .{ .bool = false }, .desc = "Include stale + lease-expired claims" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--follow", .kind = .bool, .default = .{ .bool = false }, .desc = "Stream snapshots until SIGINT" },
        .{ .long = "--interval", .kind = .string, .desc = "Poll interval for --follow (default 1s; e.g. 100ms)" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"ps"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const interval_ns = parseIntervalOrDefault(args.interval);

    if (args.follow) follow.installSigintHandler();

    var live_d = d;
    while (true) {
        emitOnce(ctx.stdout, live_d, ctx.allocator, args) catch |e|
            exit.die(ctx, e, "ps: {s}", .{@errorName(e)});
        try ctx.stdout.flush();

        if (!args.follow) return;
        if (follow.shouldStop()) return;
        // `interruptibleSleep` refreshes the read-only DB handle on
        // return (plan 85 t#2623 WAL-rotation fix). Re-pin the
        // pointer because the singleton's address can move.
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
    const active = try agentactivity.store.listActive(d, allocator, null);
    defer agentactivity.types.Claim.deinitMany(active, allocator);

    var stale_rows: []agentactivity.types.Claim = &.{};
    var stale_owned = false;
    defer if (stale_owned) agentactivity.types.Claim.deinitMany(stale_rows, allocator);
    if (args.stale) {
        stale_rows = try listStaleClaims(d, allocator);
        stale_owned = true;
    }

    if (args.json) {
        try emitJson(w, d, allocator, active, stale_rows, args);
    } else {
        try emitText(w, d, allocator, active, stale_rows);
    }
}

fn emitJson(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    active: []const agentactivity.types.Claim,
    stale_rows: []const agentactivity.types.Claim,
    args: anytype,
) !void {
    try w.print("{{\"generated_at\":", .{});
    try writeNowIso(w);
    try w.print(",\"active\":[", .{});
    var first = true;
    for (active) |c| {
        if (!claimMatches(d, c, args)) continue;
        if (!first) try w.print(",", .{});
        first = false;
        const scope = agentactivity.store.resolveClaimScope(d, allocator, c);
        defer scope.deinit(allocator);
        try agentactivity.json.writeClaim(w, c, scope);
    }
    try w.print("],\"stale\":[", .{});
    first = true;
    for (stale_rows) |c| {
        if (!claimMatches(d, c, args)) continue;
        if (!first) try w.print(",", .{});
        first = false;
        const scope = agentactivity.store.resolveClaimScope(d, allocator, c);
        defer scope.deinit(allocator);
        try agentactivity.json.writeClaim(w, c, scope);
    }
    try w.print("]}}\n", .{});
}

fn emitText(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    active: []const agentactivity.types.Claim,
    stale_rows: []const agentactivity.types.Claim,
) !void {
    try w.print("active: {d}\n", .{active.len});
    for (active) |c| try renderClaimLine(w, d, allocator, c);
    if (stale_rows.len > 0) {
        try w.print("stale: {d}\n", .{stale_rows.len});
        for (stale_rows) |c| try renderClaimLine(w, d, allocator, c);
    }
}

fn renderClaimLine(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    c: agentactivity.types.Claim,
) !void {
    const branch = c.branch orelse "?";
    const sha_full = c.head_sha_at_claim orelse "?";
    const sha = if (sha_full.len >= 8) sha_full[0..8] else sha_full;
    const scope = agentactivity.store.resolveClaimScope(d, allocator, c);
    defer scope.deinit(allocator);
    try w.print(
        "  {s}:{d}  scope:{s}  vendor:{s}  branch:{s}  sha:{s}  token:{s}\n",
        .{ c.entity_kind.toText(), c.entity_id, scope.label(), c.vendor, branch, sha, c.claim_token },
    );
}

fn claimMatches(d: *db.sqlite.Db, c: agentactivity.types.Claim, args: anytype) bool {
    if (args.vendor) |v| {
        if (!std.mem.eql(u8, c.vendor, v)) return false;
    }
    if (args.plan) |pid| {
        // Widened --plan: matches plan-direct claims AND task-on-plan
        // claims AND plan_step-on-plan claims. See planfilter.zig.
        if (!planfilter.claimBelongsToPlan(d, c.entity_kind, c.entity_id, pid)) return false;
    }
    return true;
}

pub fn parseIntervalOrDefault(opt: ?[]const u8) u64 {
    if (opt) |s| return follow.parseDurationNs(s) catch follow.default_interval_ns;
    return follow.default_interval_ns;
}

pub fn writeNowIso(w: *std.Io.Writer) !void {
    // Render the current UTC time in the same ISO8601 form SQLite's
    // strftime('%Y-%m-%dT%H:%M:%fZ') emits, so downstream consumers
    // can compare against the row timestamps without parser quirks.
    // Pull the wall-clock timestamp via the process Io rather than
    // legacy `std.time.milliTimestamp` (removed in Zig 0.16).
    const ctx = runtime.current();
    const ts = std.Io.Clock.now(.real, ctx.io);
    const ms_total = ts.toMilliseconds();
    const secs: i64 = @divFloor(ms_total, 1000);
    const sub_ms: i64 = @mod(ms_total, 1000);
    const ep_secs: u64 = @intCast(secs);
    const ep = std.time.epoch.EpochSeconds{ .secs = ep_secs };
    const ed = ep.getEpochDay();
    const ydn = ed.calculateYearDay();
    const mdn = ydn.calculateMonthDay();
    const ds = ep.getDaySeconds();

    try w.print(
        "\"{d:0>4}-{d:0>2}-{d:0>2}T{d:0>2}:{d:0>2}:{d:0>2}.{d:0>3}Z\"",
        .{
            @as(u32, @intCast(ydn.year)),
            mdn.month.numeric(),
            mdn.day_index + 1,
            ds.getHoursIntoDay(),
            ds.getMinutesIntoHour(),
            ds.getSecondsIntoMinute(),
            @as(u32, @intCast(sub_ms)),
        },
    );
}

/// List every claim whose status is `stale` (reconcile-marked) OR
/// `active` with an expired lease. Same query as the operator-side
/// dashboard helper.
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
