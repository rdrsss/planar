//! Agent claims, heartbeat health, and action-stream cockpit queries.

const std = @import("std");
const db = @import("db");
const engine = @import("engine");
const agentactivity = engine.runtime.agentactivity.store;
const aa_types = engine.runtime.agentactivity.types;
const StatusBadge = @import("common.zig").StatusBadge;

// =========================================================================
// Agent Monitor view-model (reads agent_work_claims + agent_actions)
// =========================================================================

/// One row in the Agent Monitor roster. Represents one live or recently
/// terminal agent_work_claims row.
pub const ClaimRow = struct {
    id: i64,
    /// Human-readable entity reference, e.g. "task:1234".
    entity_ref: []const u8,
    /// Vendor string, e.g. "claude".
    vendor: []const u8,
    /// Role label or empty.
    role: ?[]const u8,
    /// Derived status badge.
    badge: StatusBadge,
    /// ISO-8601 claim acquisition timestamp. Used to derive the real TTL for
    /// heartbeat-age classification (TTL = lease_expires_at - claimed_at).
    claimed_at: []const u8,
    /// ISO-8601 heartbeat timestamp for display.
    last_heartbeat_at: []const u8,
    /// ISO-8601 expiry for display.
    lease_expires_at: []const u8,
    /// Most-recent action summary, or null.
    latest_action_summary: ?[]const u8,

    pub fn deinit(self: ClaimRow, allocator: std.mem.Allocator) void {
        allocator.free(self.entity_ref);
        allocator.free(self.vendor);
        if (self.role) |s| allocator.free(s);
        allocator.free(self.claimed_at);
        allocator.free(self.last_heartbeat_at);
        allocator.free(self.lease_expires_at);
        if (self.latest_action_summary) |s| allocator.free(s);
    }

    pub fn deinitMany(rows: []ClaimRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Heartbeat-age classification for a live claim.
///
/// Thresholds (applied relative to now):
///
///   fresh   — last_heartbeat_at is within TTL/2 of now. The claim is
///             actively being worked; no operator attention needed.
///             Rendered green / normal.
///
///   warning — last_heartbeat_at is older than TTL/2 but the lease has
///             not yet expired (lease_expires_at >= now). The agent may
///             be long-running or slow to heartbeat; watch, but not
///             yet stranded. Rendered yellow.
///
///   stale   — lease_expires_at < now. The claim has lapsed; the task is
///             stranded. This is the same state the operator just
///             recovered from after a crash and must be unmistakable.
///             Rendered red / bold.
///
/// TTL/2 is computed as (lease_expires_at - claimed_at) / 2. When the
/// intervals cannot be parsed the age class falls back to .warning so
/// as not to suppress a potentially real signal.
pub const HeartbeatAge = enum {
    fresh,
    warning,
    stale,
};

/// Snapshot of all active and stale claims for the Agent Monitor view.
pub const AgentMonitorSnapshot = struct {
    active: []ClaimRow,
    stale: []ClaimRow,

    pub fn deinit(self: AgentMonitorSnapshot, allocator: std.mem.Allocator) void {
        ClaimRow.deinitMany(self.active, allocator);
        ClaimRow.deinitMany(self.stale, allocator);
    }
};

/// One row in the Agent Monitor event stream (agent_actions + claim transitions).
/// Rows are ordered newest-first so the view tails in live.
pub const AgentActionRow = struct {
    id: i64,
    /// ISO-8601 timestamp (started_at for actions, claimed_at/released_at for transitions).
    ts: []const u8,
    /// Short label, e.g. "coder", "heartbeat", "claim:completed".
    kind_label: []const u8,
    /// Entity reference, e.g. "task:42", or empty.
    entity_ref: []const u8,
    /// Vendor string.
    vendor: []const u8,
    /// Optional summary / release reason.
    summary: ?[]const u8,

    pub fn deinit(self: AgentActionRow, allocator: std.mem.Allocator) void {
        allocator.free(self.ts);
        allocator.free(self.kind_label);
        allocator.free(self.entity_ref);
        allocator.free(self.vendor);
        if (self.summary) |s| allocator.free(s);
    }

    pub fn deinitMany(rows: []AgentActionRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

fn badgeFromClaim(c: aa_types.Claim) StatusBadge {
    return switch (c.status) {
        .active => .active,
        .stale => .stale,
        .released => .none,
        .completed => .done,
        .aborted => .cancelled,
    };
}

fn buildClaimRow(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    c: aa_types.Claim,
) !ClaimRow {
    const entity_ref = try std.fmt.allocPrint(
        allocator,
        "{s}:{d}",
        .{ c.entity_kind.toText(), c.entity_id },
    );
    errdefer allocator.free(entity_ref);

    const vendor = try allocator.dupe(u8, c.vendor);
    errdefer allocator.free(vendor);

    const role: ?[]const u8 = if (c.role) |r| try allocator.dupe(u8, r) else null;
    errdefer if (role) |r| allocator.free(r);

    const claimed = try allocator.dupe(u8, c.claimed_at);
    errdefer allocator.free(claimed);

    const hb = try allocator.dupe(u8, c.last_heartbeat_at);
    errdefer allocator.free(hb);

    const exp = try allocator.dupe(u8, c.lease_expires_at);
    errdefer allocator.free(exp);

    // Best-effort: look up the latest action summary.
    var latest_summary: ?[]const u8 = null;
    if (agentactivity.latestActionForClaim(d, allocator, c.id)) |maybe_action| {
        if (maybe_action) |action| {
            defer action.deinit(allocator);
            if (action.summary) |s| {
                latest_summary = try allocator.dupe(u8, s);
            }
        }
    } else |_| {}

    return .{
        .id = c.id,
        .entity_ref = entity_ref,
        .vendor = vendor,
        .role = role,
        .badge = badgeFromClaim(c),
        .claimed_at = claimed,
        .last_heartbeat_at = hb,
        .lease_expires_at = exp,
        .latest_action_summary = latest_summary,
    };
}

/// Query the DB and build an AgentMonitorSnapshot. Caller owns the result;
/// release via `snapshot.deinit(allocator)`.
pub fn queryAgentMonitor(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) !AgentMonitorSnapshot {
    const raw_active = try agentactivity.listActive(d, allocator, null);
    defer aa_types.Claim.deinitMany(raw_active, allocator);

    const raw_stale = try agentactivity.listStale(d, allocator);
    defer aa_types.Claim.deinitMany(raw_stale, allocator);

    var active_rows: std.ArrayList(ClaimRow) = .empty;
    errdefer {
        for (active_rows.items) |r| r.deinit(allocator);
        active_rows.deinit(allocator);
    }
    for (raw_active) |c| {
        try active_rows.append(allocator, try buildClaimRow(d, allocator, c));
    }

    var stale_rows: std.ArrayList(ClaimRow) = .empty;
    errdefer {
        for (stale_rows.items) |r| r.deinit(allocator);
        stale_rows.deinit(allocator);
    }
    for (raw_stale) |c| {
        try stale_rows.append(allocator, try buildClaimRow(d, allocator, c));
    }

    return .{
        .active = try active_rows.toOwnedSlice(allocator),
        .stale = try stale_rows.toOwnedSlice(allocator),
    };
}

/// Classify the heartbeat age of a single active claim row.
///
/// Parameters:
///   now_unix — current Unix timestamp in seconds (caller passes
///              `std.time.timestamp()`; factored out so the function is
///              unit-testable without wall-clock dependency).
///   last_heartbeat_unix — Unix timestamp of last_heartbeat_at.
///   lease_expires_unix  — Unix timestamp of lease_expires_at.
///   claimed_unix        — Unix timestamp of claimed_at (used to derive TTL).
///
/// Returns .stale when the lease has expired; .warning when past TTL/2;
/// .fresh otherwise.
pub fn classifyHeartbeatAge(
    now_unix: i64,
    last_heartbeat_unix: i64,
    lease_expires_unix: i64,
    claimed_unix: i64,
) HeartbeatAge {
    // Lease already lapsed — unmistakably stale.
    if (lease_expires_unix < now_unix) return .stale;

    // TTL is the lease window from claim time. Guard against zero/negative TTL.
    const ttl_secs = lease_expires_unix - claimed_unix;
    if (ttl_secs <= 0) return .warning;

    // Heartbeat age: seconds since last heartbeat.
    const hb_age_secs = now_unix - last_heartbeat_unix;

    // Warning threshold = TTL/2.
    if (hb_age_secs > @divTrunc(ttl_secs, 2)) return .warning;

    return .fresh;
}

/// Parse an ISO-8601 timestamp string ("YYYY-MM-DDTHH:MM:SS.mmmZ") into a
/// Unix timestamp in seconds. Returns null when the string is malformed or
/// the timestamp cannot be represented.
///
/// Handles the SQLite strftime output shape: "YYYY-MM-DDTHH:MM:SS.mmmZ".
/// Fractional seconds are truncated (we only need second-level resolution
/// for heartbeat-age classification).
pub fn parseIso8601Unix(ts: []const u8) ?i64 {
    // Minimum length: "YYYY-MM-DDTHH:MM:SSZ" = 20 chars.
    if (ts.len < 20) return null;

    const year = std.fmt.parseInt(i64, ts[0..4], 10) catch return null;
    const month = std.fmt.parseInt(u8, ts[5..7], 10) catch return null;
    const day = std.fmt.parseInt(u8, ts[8..10], 10) catch return null;
    const hour = std.fmt.parseInt(u8, ts[11..13], 10) catch return null;
    const minute = std.fmt.parseInt(u8, ts[14..16], 10) catch return null;
    const second = std.fmt.parseInt(u8, ts[17..19], 10) catch return null;

    if (month < 1 or month > 12) return null;
    if (day < 1 or day > 31) return null;

    // Days since Unix epoch (1970-01-01). Use a simple proleptic Gregorian
    // approximation: accurate enough for heartbeat classification.
    const y: i64 = if (month <= 2) year - 1 else year;
    const era: i64 = @divFloor(y, 400);
    const yoe: i64 = y - era * 400;
    const doy: i64 = @divTrunc(153 * (@as(i64, month) + (if (month > 2) @as(i64, -3) else @as(i64, 9))) + 2, 5) + day - 1;
    const doe: i64 = yoe * 365 + @divTrunc(yoe, 4) - @divTrunc(yoe, 100) + doy;
    const days: i64 = era * 146097 + doe - 719468;

    const unix: i64 = days * 86400 + @as(i64, hour) * 3600 + @as(i64, minute) * 60 + second;
    return unix;
}

/// Query the most-recent `limit` agent_actions rows and claim status transitions
/// in descending timestamp order (newest-first) for the Agent Monitor event
/// stream. Combines agent_actions and terminal claim transitions (completed /
/// released / aborted / stale) so the operator sees a unified tail.
///
/// `limit` caps the result set; 200 is a reasonable default.
pub fn queryAgentActionStream(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    limit: u32,
) ![]AgentActionRow {
    var out: std.ArrayList(AgentActionRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    // --- agent_actions rows -------------------------------------------
    var stmt = d.prepare(
        \\select aa.id, aa.started_at, aa.action_kind,
        \\       coalesce(aa.entity_kind, ''), coalesce(aa.entity_id, -1),
        \\       aa.vendor, aa.summary
        \\from agent_actions aa
        \\order by aa.started_at desc, aa.id desc
        \\limit ?
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    var lim_param: [1]db.sqlite.Param = .{.{ .int = limit }};
    stmt.bind(&lim_param) catch return error.QueryFailed;

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const ts = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(ts);
                const kind_str = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(kind_str);
                const ent_kind_str = try stmt.columnTextAlloc(3, allocator);
                defer allocator.free(ent_kind_str);
                const ent_id_raw = stmt.columnInt(4);
                const vendor = try stmt.columnTextAlloc(5, allocator);
                errdefer allocator.free(vendor);
                const summary_opt = try stmt.columnTextOpt(6, allocator);

                // Build entity_ref: "kind:id" when entity columns are set,
                // empty string otherwise.
                const entity_ref = if (ent_kind_str.len > 0 and ent_id_raw >= 0)
                    try std.fmt.allocPrint(allocator, "{s}:{d}", .{ ent_kind_str, ent_id_raw })
                else
                    try allocator.dupe(u8, "");
                errdefer allocator.free(entity_ref);

                try out.append(allocator, .{
                    .id = id,
                    .ts = ts,
                    .kind_label = kind_str,
                    .entity_ref = entity_ref,
                    .vendor = vendor,
                    .summary = summary_opt,
                });
            },
        }
    }

    // --- claim terminal-transition rows --------------------------------
    // We surface claim transitions as synthetic rows with kind_label =
    // "claim:<status>" so the operator can see when claims were completed,
    // released, or aborted in the same stream.
    var cstmt = d.prepare(
        \\select c.id,
        \\       coalesce(c.released_at, c.claimed_at) as ts,
        \\       c.status,
        \\       c.entity_kind, c.entity_id,
        \\       c.vendor,
        \\       c.release_reason
        \\from agent_work_claims c
        \\where c.status in ('completed', 'released', 'aborted', 'stale')
        \\order by ts desc, c.id desc
        \\limit ?
    ) catch return error.QueryFailed;
    defer cstmt.finalize();
    var clim_param: [1]db.sqlite.Param = .{.{ .int = limit }};
    cstmt.bind(&clim_param) catch return error.QueryFailed;

    while (true) {
        switch (cstmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = cstmt.columnInt(0);
                const ts = try cstmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(ts);
                const status_str = try cstmt.columnTextAlloc(2, allocator);
                defer allocator.free(status_str);
                const ent_kind_str = try cstmt.columnTextAlloc(3, allocator);
                defer allocator.free(ent_kind_str);
                const ent_id_raw = cstmt.columnInt(4);
                const vendor = try cstmt.columnTextAlloc(5, allocator);
                errdefer allocator.free(vendor);
                const reason_opt = try cstmt.columnTextOpt(6, allocator);

                const kind_label = try std.fmt.allocPrint(
                    allocator,
                    "claim:{s}",
                    .{status_str},
                );
                errdefer allocator.free(kind_label);

                const entity_ref = try std.fmt.allocPrint(
                    allocator,
                    "{s}:{d}",
                    .{ ent_kind_str, ent_id_raw },
                );
                errdefer allocator.free(entity_ref);

                try out.append(allocator, .{
                    .id = -id, // negative to distinguish from action ids
                    .ts = ts,
                    .kind_label = kind_label,
                    .entity_ref = entity_ref,
                    .vendor = vendor,
                    .summary = reason_opt,
                });
            },
        }
    }

    // Sort unified list newest-first by ts (lexicographic ISO-8601 is
    // timestamp-sortable, so string comparison works correctly).
    const rows = try out.toOwnedSlice(allocator);
    std.mem.sort(AgentActionRow, rows, {}, struct {
        fn lessThan(_: void, a: AgentActionRow, b: AgentActionRow) bool {
            // Descending: "b < a" in string order == a is newer.
            return std.mem.order(u8, a.ts, b.ts) == .gt;
        }
    }.lessThan);

    // Trim to limit.
    if (rows.len > limit) {
        for (rows[limit..]) |r| r.deinit(allocator);
        return allocator.realloc(rows, limit) catch rows;
    }
    return rows;
}
