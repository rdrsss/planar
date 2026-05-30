//! handlers/ps — `planar-watch ps [--vendor v] [--plan id] [--stale] [--json] [--follow] [--interval D] [--sort-by <lease|heartbeat>] [--group-by <role|scope|vendor>]`
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
//!   - `--group-by role|scope|vendor` — group active/stale rows by a
//!     dimension (task 3058). Text: one section header per group;
//!     JSON: `groups: {key: [...]}` wrapper inside each bucket.
//!
//! JSON shape (tech-spec § "JSON shapes"):
//!   Without --group-by:
//!     {
//!       generated_at: ISO8601,
//!       active: [ClaimRow],
//!       stale:  [ClaimRow]
//!     }
//!   With --group-by:
//!     {
//!       generated_at: ISO8601,
//!       groups: { "<key>": [ClaimRow], ... }
//!     }
//!   (The top-level `active`/`stale` envelope is UNCHANGED when
//!   `--group-by` is absent — strict backward compat.)
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
const format = @import("format.zig");
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

/// Grouping dimension for `--group-by` (task 3058).
const GroupBy = enum {
    /// Group by the claim's `role` field (coder, reviewer, planner, ...).
    role,
    /// Group by the resolved scope slug of the claim's entity.
    scope,
    /// Group by the claim's `vendor` field (claude, codex, copilot, ...).
    vendor,
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
        .{ .long = "--group-by", .kind = .string, .desc = "Group claims by dimension: role, scope, or vendor" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"ps"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const interval_ns = parseIntervalOrDefault(args.interval);
    const sort_by = parseSortBy(args.sort_by) catch |e| exit.die(ctx, e, "ps: --sort-by: accepted values are 'heartbeat' (default) or 'lease'", .{});
    const group_by_opt = parseGroupBy(args.group_by) catch |e| exit.die(ctx, e, "ps: --group-by: accepted values are 'role', 'scope', or 'vendor'", .{});

    if (args.follow) follow.installSigintHandler();

    var live_d = d;
    while (true) {
        emitOnce(ctx.stdout, live_d, ctx.allocator, args, sort_by, group_by_opt) catch |e|
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

fn parseGroupBy(opt: ?[]const u8) error{InvalidValue}!?GroupBy {
    const s = opt orelse return null;
    if (std.mem.eql(u8, s, "role")) return .role;
    if (std.mem.eql(u8, s, "scope")) return .scope;
    if (std.mem.eql(u8, s, "vendor")) return .vendor;
    return error.InvalidValue;
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
    group_by_opt: ?GroupBy,
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
        if (group_by_opt) |gb| {
            try emitJsonGrouped(w, d, allocator, active, stale_rows, args, gb);
        } else {
            try emitJson(w, d, allocator, active, stale_rows, args);
        }
    } else {
        if (group_by_opt) |gb| {
            try emitTextGrouped(w, d, allocator, active, stale_rows, args, gb);
        } else {
            try emitText(w, d, allocator, active, stale_rows);
        }
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
// Group-by rendering (task 3058)
// =========================================================================

/// Return the group key for a claim under the given `GroupBy` dimension.
/// Returned slice is borrowed from the claim's allocator-owned memory or
/// a static literal — do NOT free it.
fn groupKeyForClaim(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    c: agentactivity.types.Claim,
    gb: GroupBy,
    scope_out: *?agentactivity.types.ClaimScopeInfo,
) []const u8 {
    switch (gb) {
        .role => return c.role orelse "unknown",
        .vendor => return c.vendor,
        .scope => {
            // Resolve scope and cache it for the caller (avoids double resolve).
            const s = agentactivity.store.resolveClaimScope(d, allocator, c);
            scope_out.* = s;
            return s.label();
        },
    }
}

/// Write the JSON grouped shape:
///   { "generated_at": ..., "groups": { "<key>": [ClaimRow, ...], ... } }
/// All rows from both active and stale (when --stale is set) are merged
/// into the groups object. A claim filtered by --vendor/--plan is excluded.
fn emitJsonGrouped(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    active: []const agentactivity.types.Claim,
    stale_rows: []const agentactivity.types.Claim,
    args: anytype,
    gb: GroupBy,
) !void {
    // Build a map from group-key → claim indices (active then stale).
    // Use an ArrayList of {key, claim} pairs sorted by first-seen key so the
    // output order is deterministic.
    const GroupEntry = struct { key: []const u8, claim: agentactivity.types.Claim };
    var entries: std.ArrayList(GroupEntry) = .empty;
    defer entries.deinit(allocator);

    // Merge active + stale into one flat sequence after filtering.
    const all_slices = [_][]const agentactivity.types.Claim{ active, stale_rows };
    for (all_slices) |slice| {
        for (slice) |c| {
            if (!claimMatches(d, c, args)) continue;
            var scope_cached: ?agentactivity.types.ClaimScopeInfo = null;
            // For scope grouping we resolve and free below; for other dims we
            // don't allocate.  Track whether we own the scope object.
            const key = groupKeyForClaim(d, allocator, c, gb, &scope_cached);
            // We free the scope right away — we only needed `label()` which
            // is a borrowed slice from the scope's internal storage.  Since
            // `label()` returns either a static literal or a field of the scope
            // struct itself, we must dupe the key string before freeing.
            const key_owned = try allocator.dupe(u8, key);
            if (scope_cached) |s| s.deinit(allocator);
            try entries.append(allocator, .{ .key = key_owned, .claim = c });
        }
    }
    defer for (entries.items) |e| allocator.free(e.key);

    // Collect unique keys in insertion order.
    var seen_keys: std.ArrayList([]const u8) = .empty;
    defer seen_keys.deinit(allocator);
    outer: for (entries.items) |e| {
        for (seen_keys.items) |k| {
            if (std.mem.eql(u8, k, e.key)) continue :outer;
        }
        try seen_keys.append(allocator, e.key);
    }

    try w.print("{{\"generated_at\":", .{});
    try writeNowIso(w);
    try w.print(",\"groups\":{{", .{});

    var first_group = true;
    for (seen_keys.items) |grp_key| {
        if (!first_group) try w.print(",", .{});
        first_group = false;
        try std.json.Stringify.encodeJsonString(grp_key, .{}, w);
        try w.print(":[", .{});
        var first_claim = true;
        for (entries.items) |e| {
            if (!std.mem.eql(u8, e.key, grp_key)) continue;
            if (!first_claim) try w.print(",", .{});
            first_claim = false;
            const scope = agentactivity.store.resolveClaimScope(d, allocator, e.claim);
            defer scope.deinit(allocator);
            const action_row = agentactivity.store.latestActionForClaim(d, allocator, e.claim.id) catch null;
            defer if (action_row) |a| a.deinit(allocator);
            const action_info: ?agentactivity.json.LatestActionInfo = if (action_row) |a| .{
                .kind = a.action_kind.toText(),
                .summary = a.summary,
                .started_at = a.started_at,
            } else null;
            try agentactivity.json.writeClaimWithActivity(w, e.claim, scope, true, action_info);
        }
        try w.print("]", .{});
    }
    try w.print("}}}}\n", .{});
}

/// Text grouped output: one section header per group key, claims indented
/// beneath. Section header format: `[group: <key>]` (two spaces indent for
/// claims matching the existing renderClaimLine indentation).
fn emitTextGrouped(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    active: []const agentactivity.types.Claim,
    stale_rows: []const agentactivity.types.Claim,
    args: anytype,
    gb: GroupBy,
) !void {
    // Build (key, claim) sequence.
    const GroupEntry = struct { key: []const u8, claim: agentactivity.types.Claim };
    var entries: std.ArrayList(GroupEntry) = .empty;
    defer entries.deinit(allocator);

    const all_slices = [_][]const agentactivity.types.Claim{ active, stale_rows };
    for (all_slices) |slice| {
        for (slice) |c| {
            if (!claimMatches(d, c, args)) continue;
            var scope_cached: ?agentactivity.types.ClaimScopeInfo = null;
            const key = groupKeyForClaim(d, allocator, c, gb, &scope_cached);
            const key_owned = try allocator.dupe(u8, key);
            if (scope_cached) |s| s.deinit(allocator);
            try entries.append(allocator, .{ .key = key_owned, .claim = c });
        }
    }
    defer for (entries.items) |e| allocator.free(e.key);

    if (entries.items.len == 0) {
        try w.print("active: 0\n", .{});
        return;
    }

    // Collect unique keys in insertion order.
    var seen_keys: std.ArrayList([]const u8) = .empty;
    defer seen_keys.deinit(allocator);
    outer: for (entries.items) |e| {
        for (seen_keys.items) |k| {
            if (std.mem.eql(u8, k, e.key)) continue :outer;
        }
        try seen_keys.append(allocator, e.key);
    }

    for (seen_keys.items) |grp_key| {
        try w.print("[group: {s}]\n", .{grp_key});
        for (entries.items) |e| {
            if (!std.mem.eql(u8, e.key, grp_key)) continue;
            try renderClaimLine(w, d, allocator, e.claim);
        }
    }
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
/// Delegates to format.renderActivitySummary; kept here for backward
/// compat with callers already referencing ps.renderActivitySummary.
fn renderActivitySummary(allocator: std.mem.Allocator, summary: ?[]const u8) ![]u8 {
    return format.renderActivitySummary(allocator, summary);
}

/// Render the worktree column for text output (task 3054).
/// Delegates to format.renderWorktreeColumn.
fn renderWorktreeColumn(allocator: std.mem.Allocator, worktree_path: ?[]const u8) ![]u8 {
    return format.renderWorktreeColumn(allocator, worktree_path);
}

/// Render the relative-heartbeat column for text output (task 3055).
/// Delegates to format.renderRelativeHeartbeat.
fn renderRelativeHeartbeat(allocator: std.mem.Allocator, last_heartbeat_at: []const u8) ![]u8 {
    return format.renderRelativeHeartbeat(allocator, last_heartbeat_at);
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
