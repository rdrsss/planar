//! handlers/ps — `planar-watch ps [--vendor v] [--plan id] [--stale] [--json] [--follow] [--interval D] [--sort-by <lease|heartbeat>]`
//!
//! Snapshot of active claims (and, with --stale, also claims whose
//! lease has expired or that reconcile has marked stale). Same JSON
//! shape as the dashboard --agents fold-in. Used by operators to
//! answer "what's running right now and where is anything wedged?"
//!
//! Plan 467 M3 additions:
//!   - Text column `activity:"<latest-action-summary>"` (task 3053)
//!   - Text column `worktree:<basename>` (task 3054)
//!   - Text column `last_hb:<relative>` using relativeTime (task 3055)
//!   - JSON field `latest_action:{kind,summary,started_at}` (task 3056)
//!   - Default sort by `last_heartbeat_at desc`; `--sort-by lease`
//!     restores the old `claimed_at desc` ordering (task 3057)
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
//! ClaimRow now includes `latest_action: {kind, summary, started_at}|null`.
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

/// Sort dimension for the active-claims list.
const SortBy = enum {
    /// Sort by `last_heartbeat_at desc` — freshest-heartbeated first.
    /// This is the default post-M3 behavior.
    heartbeat,
    /// Sort by `claimed_at desc` — most-recently-acquired first.
    /// Matches the pre-M3 default; restored via `--sort-by lease`.
    lease,
};

pub const verb: cli.Cmd = .{
    .name = "ps",
    .desc = "Snapshot of active (and stale) agent claims.",
    .long_desc = "Lists every currently active agent claim — one row per claim_token.\n\n" ++
        "  --stale also includes claims whose lease has expired OR whose\n" ++
        "  status is `stale` (set by `planar-agent reconcile`).\n\n" ++
        "  --vendor / --plan narrow the result.\n\n" ++
        "  --sort-by heartbeat (default) orders by most-recently-heartbeated\n" ++
        "  first. --sort-by lease restores the pre-M3 claimed_at ordering.\n\n" ++
        "  --follow turns the snapshot into a streaming view (Tier-1 poll;\n" ++
        "  --interval defaults to 1s). Exits 0 on SIGINT.",
    .flags = &.{
        .{ .long = "--vendor", .kind = .string, .desc = "Filter by vendor (claude, codex, copilot, ...)" },
        .{ .long = "--plan", .kind = .int, .desc = "Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)" },
        .{ .long = "--stale", .kind = .bool, .default = .{ .bool = false }, .desc = "Include stale + lease-expired claims" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--follow", .kind = .bool, .default = .{ .bool = false }, .desc = "Stream snapshots until SIGINT" },
        .{ .long = "--interval", .kind = .string, .desc = "Poll interval for --follow (default 1s; e.g. 100ms)" },
        .{ .long = "--sort-by", .kind = .string, .desc = "Sort order for active claims: heartbeat (default) or lease" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"ps"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const interval_ns = parseIntervalOrDefault(args.interval);
    const sort_by = parseSortBy(args.sort_by) catch |e| exit.die(ctx, e, "ps: --sort-by: accepted values are 'heartbeat' (default) or 'lease'", .{});

    if (args.follow) follow.installSigintHandler();

    var live_d = d;
    while (true) {
        emitOnce(ctx.stdout, live_d, ctx.allocator, args, sort_by) catch |e|
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

fn parseSortBy(opt: ?[]const u8) error{InvalidValue}!SortBy {
    const s = opt orelse return .heartbeat; // default
    if (std.mem.eql(u8, s, "heartbeat")) return .heartbeat;
    if (std.mem.eql(u8, s, "lease")) return .lease;
    return error.InvalidValue;
}

fn emitOnce(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: anytype,
    sort_by: SortBy,
) !void {
    const active = try listActiveSorted(d, allocator, sort_by);
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

// =========================================================================
// Active-claims list with configurable sort
// =========================================================================

/// List all active (unexpired) claims, sorted by the chosen dimension.
/// When `sort_by = .heartbeat`, orders by `last_heartbeat_at desc`
/// (freshest first — the new M3 default). When `sort_by = .lease`,
/// orders by `claimed_at desc` (old behavior, restored via
/// `--sort-by lease`). Claims with NULL `last_heartbeat_at` sort to the
/// end deterministically via `nulls last`.
fn listActiveSorted(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    sort_by: SortBy,
) ![]agentactivity.types.Claim {
    // SQLite doesn't have NULLS LAST syntax; use `CASE WHEN ... IS NULL THEN 1 ELSE 0 END` trick.
    const sql: [:0]const u8 = switch (sort_by) {
        .heartbeat =>
        \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\       status, vendor, vendor_session_id, role, model,
        \\       worktree_id, worktree_path,
        \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
        \\       purpose, base_ref,
        \\       claimed_at, last_heartbeat_at, lease_expires_at,
        \\       released_at, release_reason
        \\from agent_work_claims
        \\where status = 'active'
        \\order by case when last_heartbeat_at is null then 1 else 0 end asc,
        \\         last_heartbeat_at desc, id desc
        ,
        .lease =>
        \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\       status, vendor, vendor_session_id, role, model,
        \\       worktree_id, worktree_path,
        \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
        \\       purpose, base_ref,
        \\       claimed_at, last_heartbeat_at, lease_expires_at,
        \\       released_at, release_reason
        \\from agent_work_claims
        \\where status = 'active'
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

// =========================================================================
// JSON rendering
// =========================================================================

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
        // Fetch latest action for the latest_action JSON field (task 3056).
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
    first = true;
    for (stale_rows) |c| {
        if (!claimMatches(d, c, args)) continue;
        if (!first) try w.print(",", .{});
        first = false;
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
    try w.print("]}}\n", .{});
}

// =========================================================================
// Text rendering
// =========================================================================

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

/// Render one active/stale claim as a human-readable text line.
///
/// Column order (M3):
///   <entity>:<id>  scope:<label>  activity:"<summary>"  vendor:<v>
///   branch:<b>  worktree:<basename>  sha:<short>  last_hb:<rel>
///   token:<tok>
///
/// Tasks:
///   3053 — activity:"<summary>" column (truncated to 80 bytes with …)
///   3054 — worktree:<basename> column (>40-char full path → …<basename>)
///   3055 — last_hb:<relative> via relativeTime helper
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

    // --- task 3053: activity column ---
    // Fetch the latest action summary. Truncate to 80 bytes with U+2026.
    const action_row = agentactivity.store.latestActionForClaim(d, allocator, c.id) catch null;
    defer if (action_row) |a| a.deinit(allocator);
    const raw_summary: ?[]const u8 = if (action_row) |a| a.summary else null;
    const activity_buf = try renderActivitySummary(allocator, raw_summary);
    defer allocator.free(activity_buf);

    // --- task 3054: worktree column ---
    const worktree_buf = try renderWorktreeColumn(allocator, c.worktree_path);
    defer allocator.free(worktree_buf);

    // --- task 3055: last_hb column (relative heartbeat time) ---
    const hb_buf = try renderRelativeHeartbeat(allocator, c.last_heartbeat_at);
    defer allocator.free(hb_buf);

    try w.print(
        "  {s}:{d}  scope:{s}  activity:{s}  vendor:{s}  branch:{s}  worktree:{s}  sha:{s}  last_hb:{s}  token:{s}\n",
        .{
            c.entity_kind.toText(), c.entity_id,
            scope.label(),          activity_buf,
            c.vendor,               branch,
            worktree_buf,           sha,
            hb_buf,                 c.claim_token,
        },
    );
}

/// Render the activity summary for text output (task 3053).
/// Returns a heap-allocated string that must be freed by the caller.
/// Format: `"<summary>"` (quoted). Truncation: > 80 bytes → truncated
/// at a safe byte boundary with U+2026 (…, 3 UTF-8 bytes) as the
/// trailing marker such that the total rendered length including quotes
/// is ≤ 82 bytes (80 content bytes + 2 quote bytes).
/// Empty quotes (`""`) when the summary is null or empty.
fn renderActivitySummary(allocator: std.mem.Allocator, summary: ?[]const u8) ![]u8 {
    const s = summary orelse "";
    if (s.len == 0) return allocator.dupe(u8, "\"\"");

    // Truncation limit: 80 bytes of content. The ellipsis is 3 bytes (…),
    // so the payload before ellipsis is at most 77 bytes.
    const limit = 80;
    if (s.len <= limit) {
        // No truncation needed — allocate `"<s>"`.
        var buf = try allocator.alloc(u8, s.len + 2);
        buf[0] = '"';
        @memcpy(buf[1 .. 1 + s.len], s);
        buf[1 + s.len] = '"';
        return buf;
    }

    // Need to truncate. Walk back from byte 77 to find a safe UTF-8
    // codepoint boundary so we don't split a multi-byte sequence.
    const ellipsis = "\xE2\x80\xA6"; // U+2026, 3 bytes
    const content_max = limit - ellipsis.len; // 77
    var cut = content_max;
    // Walk backwards while we're in the middle of a UTF-8 continuation byte
    // (0x80..0xBF). This is safe to call up to 3 times.
    while (cut > 0 and (s[cut] & 0xC0) == 0x80) cut -= 1;

    // Build the output: `"<s[0..cut]>…"`
    const out_len = 1 + cut + ellipsis.len + 1; // `"` + payload + `…` + `"`
    var buf = try allocator.alloc(u8, out_len);
    buf[0] = '"';
    @memcpy(buf[1 .. 1 + cut], s[0..cut]);
    @memcpy(buf[1 + cut .. 1 + cut + ellipsis.len], ellipsis);
    buf[out_len - 1] = '"';
    return buf;
}

/// Render the worktree column for text output (task 3054).
/// Returns a heap-allocated string that must be freed by the caller.
/// When `worktree_path` is null → `""`.
/// When the full path is ≤ 40 chars → the basename only.
/// When the full path is > 40 chars → `…<basename>`.
fn renderWorktreeColumn(allocator: std.mem.Allocator, worktree_path: ?[]const u8) ![]u8 {
    const path = worktree_path orelse return allocator.dupe(u8, "\"\"");
    if (path.len == 0) return allocator.dupe(u8, "\"\"");

    // Extract the basename (last path component after '/').
    const basename: []const u8 = if (std.mem.lastIndexOfScalar(u8, path, '/')) |idx|
        path[idx + 1 ..]
    else
        path;

    if (path.len <= 40) {
        return std.fmt.allocPrint(allocator, "{s}", .{basename});
    }
    // Long path: prefix with the U+2026 ellipsis marker.
    return std.fmt.allocPrint(allocator, "\xE2\x80\xA6{s}", .{basename});
}

/// Render the relative-heartbeat column for text output (task 3055).
/// Returns a heap-allocated string that must be freed by the caller.
/// Format strips the " ago" suffix from relativeTime output since the
/// column key `last_hb:` already provides context.
/// Returns `""` when `last_heartbeat_at` is empty.
fn renderRelativeHeartbeat(allocator: std.mem.Allocator, last_heartbeat_at: []const u8) ![]u8 {
    if (last_heartbeat_at.len == 0) return allocator.dupe(u8, "");

    // Build the "now" millisecond timestamp the same way writeNowIso does.
    const ctx = runtime.current();
    const ts = std.Io.Clock.now(.real, ctx.io);
    const now_ms = ts.toMilliseconds();

    const rel = follow.relativeTime(allocator, now_ms, last_heartbeat_at) catch {
        // Parse failure — return empty rather than propagating the error.
        return allocator.dupe(u8, "");
    };
    defer allocator.free(rel);

    // Strip the trailing " ago" when present.
    const suffix = " ago";
    if (std.mem.endsWith(u8, rel, suffix)) {
        return allocator.dupe(u8, rel[0 .. rel.len - suffix.len]);
    }
    // "just now" has no " ago" — return as-is.
    return allocator.dupe(u8, rel);
}

// =========================================================================
// Shared helpers
// =========================================================================

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
