//! cockpit/view_model.zig — pure view-model for the Planar cockpit.
//!
//! Maps `agentactivity` store reads and planning-store reads into
//! renderable row/tree/detail structs. Contains no terminal types;
//! safe to unit-test without a real TTY.
//!
//! Design invariants:
//!   - Every public struct is allocator-owned: string slices come from
//!     the allocator passed to the producing function; release via the
//!     per-type `deinit` helper.
//!   - No libvaxis / Window / Cell imports. The driver layer (app.zig)
//!     reads these structs each frame.
//!   - Query functions take `*db.sqlite.Db`; the view-model owns no
//!     DB handle.
//!
//! Task 4012 (view-model) — M2 foundation.

const std = @import("std");
const db = @import("db");
const engine = @import("engine");
const agentactivity = engine.runtime.agentactivity.store;
const aa_types = engine.runtime.agentactivity.types;

// =========================================================================
// Shared primitives
// =========================================================================

/// Visual status badge rendered by the tree-navigator and row widgets.
pub const StatusBadge = enum {
    active, // claim active, heartbeat fresh
    stale, // claim stale or lease expired
    done,
    todo,
    doing,
    blocked,
    cancelled,
    draft,
    paused,
    abandoned,
    none,

    /// One-character summary glyph for compact tree nodes.
    pub fn glyph(self: StatusBadge) []const u8 {
        return switch (self) {
            .active => "A",
            .stale => "S",
            .done => "D",
            .todo => " ",
            .doing => ">",
            .blocked => "B",
            .cancelled => "X",
            .draft => "d",
            .paused => "P",
            .abandoned => "~",
            .none => " ",
        };
    }
};

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

// =========================================================================
// Plan tree view-model
// =========================================================================

/// A plan node in the Scope Explorer / Task Board tree.
pub const PlanNode = struct {
    id: i64,
    title: []const u8,
    slug: []const u8,
    status_badge: StatusBadge,
    task_count: u32,
    done_count: u32,
    /// Parent plan id for depth computation. Null for top-level plans.
    parent_plan_id: ?i64,
    /// Depth in the tree (0 = root plan). Computed by the caller.
    depth: u32,
    /// Whether this node is currently expanded.
    expanded: bool,

    pub fn deinit(self: PlanNode, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.slug);
    }
};

/// A task row in the Task Board / plan tree.
pub const TaskRow = struct {
    id: i64,
    plan_id: ?i64,
    title: []const u8,
    status_badge: StatusBadge,
    priority: i64,
    /// Active claim token if one exists.
    claim_token: ?[]const u8,

    pub fn deinit(self: TaskRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        if (self.claim_token) |s| allocator.free(s);
    }

    pub fn deinitMany(rows: []TaskRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

pub fn planStatusBadge(status_text: []const u8) StatusBadge {
    if (std.mem.eql(u8, status_text, "draft")) return .draft;
    if (std.mem.eql(u8, status_text, "active")) return .active;
    if (std.mem.eql(u8, status_text, "paused")) return .paused;
    if (std.mem.eql(u8, status_text, "done")) return .done;
    if (std.mem.eql(u8, status_text, "abandoned")) return .abandoned;
    return .none;
}

pub fn taskStatusBadge(status_text: []const u8) StatusBadge {
    if (std.mem.eql(u8, status_text, "todo")) return .todo;
    if (std.mem.eql(u8, status_text, "doing")) return .doing;
    if (std.mem.eql(u8, status_text, "blocked")) return .blocked;
    if (std.mem.eql(u8, status_text, "done")) return .done;
    if (std.mem.eql(u8, status_text, "cancelled")) return .cancelled;
    return .none;
}

/// Query a flat list of plans, ordered by parent_plan_id then id so the
/// caller can build a tree. Result is caller-owned; free each element
/// via `node.deinit(allocator)` then `allocator.free(slice)`.
pub fn queryPlanNodes(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]PlanNode {
    var stmt = d.prepare(
        \\select p.id, p.title, p.slug, p.status, p.parent_plan_id,
        \\       (select count(*) from tasks t where t.plan_id = p.id) as task_count,
        \\       (select count(*) from tasks t where t.plan_id = p.id and t.status = 'done') as done_count
        \\from plans p
        \\order by coalesce(p.parent_plan_id, p.id), p.id
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(PlanNode) = .empty;
    errdefer {
        for (out.items) |n| n.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const slug = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(slug);
                const status_text = try stmt.columnTextAlloc(3, allocator);
                defer allocator.free(status_text);
                const parent_plan_id = stmt.columnIntOpt(4);
                const task_count: u32 = @intCast(@max(0, stmt.columnInt(5)));
                const done_count: u32 = @intCast(@max(0, stmt.columnInt(6)));

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .slug = slug,
                    .status_badge = planStatusBadge(status_text),
                    .task_count = task_count,
                    .done_count = done_count,
                    .parent_plan_id = parent_plan_id,
                    .depth = 0,
                    .expanded = true,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query tasks for a specific plan, enriched with active claim info.
/// Result slice is caller-owned; free via `TaskRow.deinitMany`.
pub fn queryTaskRows(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) ![]TaskRow {
    var stmt = d.prepare(
        \\select t.id, t.plan_id, t.title, t.status, t.priority,
        \\       (select c.claim_token from agent_work_claims c
        \\        where c.entity_kind = 'task' and c.entity_id = t.id
        \\          and c.status = 'active'
        \\          and c.lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\        order by c.id desc limit 1) as active_claim_token
        \\from tasks t
        \\where t.plan_id = ?
        \\order by t.priority asc, t.id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(TaskRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const pid = stmt.columnIntOpt(1);
                const title = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(title);
                const status_text = try stmt.columnTextAlloc(3, allocator);
                defer allocator.free(status_text);
                const priority = stmt.columnInt(4);
                const claim_token = try stmt.columnTextOpt(5, allocator);

                try out.append(allocator, .{
                    .id = id,
                    .plan_id = pid,
                    .title = title,
                    .status_badge = taskStatusBadge(status_text),
                    .priority = priority,
                    .claim_token = claim_token,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Detail pane view-model
// =========================================================================

/// Kind of entity shown in the detail pane.
pub const DetailKind = enum {
    plan,
    task,
    claim,
    empty,
};

/// Content for the detail pane. The `body` slice is markdown text to
/// render via the markdown detail pane widget.
pub const DetailPane = struct {
    kind: DetailKind,
    title: []const u8,
    body: []const u8,

    pub fn deinit(self: DetailPane, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.body);
    }

    /// Empty detail pane (nothing selected).
    pub fn empty(allocator: std.mem.Allocator) !DetailPane {
        return .{
            .kind = .empty,
            .title = try allocator.dupe(u8, ""),
            .body = try allocator.dupe(u8, ""),
        };
    }
};

/// Build a DetailPane for a plan row from the DB.
pub fn queryPlanDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, summary, status from plans where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const summary_opt = try stmt.columnTextOpt(1, allocator);
            defer if (summary_opt) |s| allocator.free(s);
            const status_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status_text);

            const body = if (summary_opt) |s|
                try std.fmt.allocPrint(allocator, "**Status:** {s}\n\n{s}", .{ status_text, s })
            else
                try std.fmt.allocPrint(allocator, "**Status:** {s}", .{status_text});

            return .{
                .kind = .plan,
                .title = title,
                .body = body,
            };
        },
    }
}

/// Build a DetailPane for a task from the DB.
pub fn queryTaskDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, body, status, priority, next_action from tasks where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const status_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status_text);
            const priority = stmt.columnInt(3);
            const next_action_opt = try stmt.columnTextOpt(4, allocator);
            defer if (next_action_opt) |s| allocator.free(s);

            var body_parts: std.ArrayList(u8) = .empty;
            errdefer body_parts.deinit(allocator);
            {
                const first_line = try std.fmt.allocPrint(
                    allocator,
                    "**Status:** {s}  **Priority:** {d}",
                    .{ status_text, priority },
                );
                defer allocator.free(first_line);
                try body_parts.appendSlice(allocator, first_line);
            }
            if (next_action_opt) |na| {
                const part = try std.fmt.allocPrint(allocator, "\n\n**Next action:** {s}", .{na});
                defer allocator.free(part);
                try body_parts.appendSlice(allocator, part);
            }
            if (body_opt) |b| {
                const part = try std.fmt.allocPrint(allocator, "\n\n{s}", .{b});
                defer allocator.free(part);
                try body_parts.appendSlice(allocator, part);
            }

            return .{
                .kind = .task,
                .title = title,
                .body = try body_parts.toOwnedSlice(allocator),
            };
        },
    }
}

// =========================================================================
// Scope Explorer view-model  (tasks 3966–3970)
// =========================================================================

/// A scope filter for the Scope Explorer: either a specific repo scope
/// (by project.id) or all scopes.
pub const ScopeFilter = union(enum) {
    /// Show only entities belonging to the repo with this project id.
    repo: i64,
    /// Show entities across all scopes (global + every repo/association).
    all,
};

/// Query plans visible under `filter`, ordered by parent then id so that
/// child plans (subplans) follow their parent in the flat list. The caller
/// must build depth annotations from the parent_plan_id column.
///
/// Returns a flat slice; caller frees each element via `node.deinit(allocator)`
/// then `allocator.free(slice)`.
pub fn queryPlanNodesFiltered(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: ScopeFilter,
) ![]PlanNode {
    const sql_all =
        \\select p.id, p.title, p.slug, p.status, p.parent_plan_id,
        \\       (select count(*) from tasks t where t.plan_id = p.id) as task_count,
        \\       (select count(*) from tasks t where t.plan_id = p.id and t.status = 'done') as done_count
        \\from plans p
        \\order by coalesce(p.parent_plan_id, p.id), p.id
    ;
    const sql_repo =
        \\select p.id, p.title, p.slug, p.status, p.parent_plan_id,
        \\       (select count(*) from tasks t where t.plan_id = p.id) as task_count,
        \\       (select count(*) from tasks t where t.plan_id = p.id and t.status = 'done') as done_count
        \\from plans p
        \\where (p.scope_kind = 'repo' and p.scope_id = ?) or p.scope_kind = 'global'
        \\order by coalesce(p.parent_plan_id, p.id), p.id
    ;

    var stmt = switch (filter) {
        .all => blk: {
            var s = d.prepare(sql_all) catch return error.QueryFailed;
            s.bind(&.{}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
        .repo => blk: {
            var s = d.prepare(sql_repo) catch return error.QueryFailed;
            s.bind(&.{.{ .int = filter.repo }}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
    };
    defer stmt.finalize();

    var out: std.ArrayList(PlanNode) = .empty;
    errdefer {
        for (out.items) |n| n.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const slug = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(slug);
                const status_text = try stmt.columnTextAlloc(3, allocator);
                defer allocator.free(status_text);
                const parent_plan_id = stmt.columnIntOpt(4);
                const task_count: u32 = @intCast(@max(0, stmt.columnInt(5)));
                const done_count: u32 = @intCast(@max(0, stmt.columnInt(6)));

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .slug = slug,
                    .status_badge = planStatusBadge(status_text),
                    .task_count = task_count,
                    .done_count = done_count,
                    .parent_plan_id = parent_plan_id,
                    .depth = 0, // caller computes depth from parent_plan_id
                    .expanded = true,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// One child-entity row under a drilled plan (task, decision, question,
/// scenario, or artifact).
pub const DrillKind = enum {
    task,
    decision,
    question,
    scenario,
    artifact,
};

pub const DrillRow = struct {
    id: i64,
    kind: DrillKind,
    title: []const u8,
    status_badge: StatusBadge,
    /// plan_id the row belongs to.
    plan_id: i64,

    pub fn deinit(self: DrillRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
    }

    pub fn deinitMany(rows: []DrillRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

fn decisionStatusBadge(status_text: []const u8) StatusBadge {
    // decisions status: proposed, accepted, superseded, withdrawn.
    if (std.mem.eql(u8, status_text, "accepted")) return .done;
    if (std.mem.eql(u8, status_text, "withdrawn")) return .cancelled;
    if (std.mem.eql(u8, status_text, "superseded")) return .abandoned;
    return .draft; // proposed
}

fn questionStatusBadge(status_text: []const u8) StatusBadge {
    // questions status: open, answered, wontfix.
    if (std.mem.eql(u8, status_text, "open")) return .todo;
    if (std.mem.eql(u8, status_text, "answered")) return .done;
    if (std.mem.eql(u8, status_text, "wontfix")) return .cancelled;
    return .none;
}

fn scenarioStatusBadge(status_text: []const u8) StatusBadge {
    // test_scenarios status: draft, ready, verified, failing, retired.
    if (std.mem.eql(u8, status_text, "draft")) return .draft;
    if (std.mem.eql(u8, status_text, "verified")) return .done;
    if (std.mem.eql(u8, status_text, "failing")) return .blocked;
    if (std.mem.eql(u8, status_text, "retired")) return .abandoned;
    return .none; // ready
}

/// Query all child entities for a plan: tasks, decisions, questions,
/// scenarios, artifacts linked to the plan. Returns a flat list ordered
/// by kind then id. Caller owns result; free via `DrillRow.deinitMany`.
pub fn queryPlanDrillRows(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) ![]DrillRow {
    var out: std.ArrayList(DrillRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    // Tasks.
    {
        var stmt = d.prepare(
            "select id, title, status from tasks where plan_id = ? order by priority asc, id asc",
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const status = try stmt.columnTextAlloc(2, allocator);
                    defer allocator.free(status);
                    try out.append(allocator, .{
                        .id = id,
                        .kind = .task,
                        .title = title,
                        .status_badge = taskStatusBadge(status),
                        .plan_id = plan_id,
                    });
                },
            }
        }
    }

    // Decisions linked to this plan via entity_links.
    // entity_links uses from_kind/from_id/to_kind/to_id/relationship.
    // Check both directions: plan→decision and decision→plan.
    {
        var stmt = d.prepare(
            \\select d.id, d.title, d.status
            \\from decisions d
            \\where d.id in (
            \\  select el.to_id from entity_links el
            \\  where el.from_kind = 'plan' and el.from_id = ? and el.to_kind = 'decision'
            \\  union
            \\  select el.from_id from entity_links el
            \\  where el.to_kind = 'plan' and el.to_id = ? and el.from_kind = 'decision'
            \\)
            \\order by d.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const status = try stmt.columnTextAlloc(2, allocator);
                    defer allocator.free(status);
                    try out.append(allocator, .{
                        .id = id,
                        .kind = .decision,
                        .title = title,
                        .status_badge = decisionStatusBadge(status),
                        .plan_id = plan_id,
                    });
                },
            }
        }
    }

    // Questions linked to this plan via entity_links (both directions).
    {
        var stmt = d.prepare(
            \\select q.id, q.title, q.status
            \\from questions q
            \\where q.id in (
            \\  select el.to_id from entity_links el
            \\  where el.from_kind = 'plan' and el.from_id = ? and el.to_kind = 'question'
            \\  union
            \\  select el.from_id from entity_links el
            \\  where el.to_kind = 'plan' and el.to_id = ? and el.from_kind = 'question'
            \\)
            \\order by q.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const status = try stmt.columnTextAlloc(2, allocator);
                    defer allocator.free(status);
                    try out.append(allocator, .{
                        .id = id,
                        .kind = .question,
                        .title = title,
                        .status_badge = questionStatusBadge(status),
                        .plan_id = plan_id,
                    });
                },
            }
        }
    }

    // Test scenarios linked to this plan via entity_links (both directions).
    {
        var stmt = d.prepare(
            \\select ts.id, ts.title, ts.status
            \\from test_scenarios ts
            \\where ts.id in (
            \\  select el.to_id from entity_links el
            \\  where el.from_kind = 'plan' and el.from_id = ? and el.to_kind = 'test_scenario'
            \\  union
            \\  select el.from_id from entity_links el
            \\  where el.to_kind = 'plan' and el.to_id = ? and el.from_kind = 'test_scenario'
            \\)
            \\order by ts.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const status = try stmt.columnTextAlloc(2, allocator);
                    defer allocator.free(status);
                    try out.append(allocator, .{
                        .id = id,
                        .kind = .scenario,
                        .title = title,
                        .status_badge = scenarioStatusBadge(status),
                        .plan_id = plan_id,
                    });
                },
            }
        }
    }

    // Artifacts linked to this plan via entity_links (both directions).
    // artifacts.kind is the column (not artifact_kind).
    {
        var stmt = d.prepare(
            \\select a.id, a.title, a.kind
            \\from artifacts a
            \\where a.id in (
            \\  select el.to_id from entity_links el
            \\  where el.from_kind = 'plan' and el.from_id = ? and el.to_kind = 'artifact'
            \\  union
            \\  select el.from_id from entity_links el
            \\  where el.to_kind = 'plan' and el.to_id = ? and el.from_kind = 'artifact'
            \\)
            \\order by a.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    try out.append(allocator, .{
                        .id = id,
                        .kind = .artifact,
                        .title = title,
                        .status_badge = .none,
                        .plan_id = plan_id,
                    });
                },
            }
        }
    }

    return try out.toOwnedSlice(allocator);
}

/// Build a DetailPane for a decision from the DB (for the split detail pane).
/// Note: decisions.body is NOT NULL in the schema.
pub fn queryDecisionDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    decision_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, body, status from decisions where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = decision_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            // decisions.body is NOT NULL.
            const body_raw = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(body_raw);
            const status = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status);

            const body = try std.fmt.allocPrint(
                allocator,
                "**Status:** {s}\n\n{s}",
                .{ status, body_raw },
            );

            return .{ .kind = .plan, .title = title, .body = body };
        },
    }
}

/// Build a DetailPane for a question from the DB.
pub fn queryQuestionDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    question_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, body, status from questions where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = question_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const status = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status);

            const body = if (body_opt) |b|
                try std.fmt.allocPrint(allocator, "**Status:** {s}\n\n{s}", .{ status, b })
            else
                try std.fmt.allocPrint(allocator, "**Status:** {s}", .{status});

            return .{ .kind = .plan, .title = title, .body = body };
        },
    }
}

/// Build a DetailPane for a test scenario from the DB.
pub fn queryScenarioDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    scenario_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, body, status from test_scenarios where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = scenario_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const status = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status);

            const body = if (body_opt) |b|
                try std.fmt.allocPrint(allocator, "**Status:** {s}\n\n{s}", .{ status, b })
            else
                try std.fmt.allocPrint(allocator, "**Status:** {s}", .{status});

            return .{ .kind = .plan, .title = title, .body = body };
        },
    }
}

/// Build a DetailPane for an artifact from the DB.
/// Note: artifacts.kind is the column name (not artifact_kind).
pub fn queryArtifactDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    artifact_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, body, kind from artifacts where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = artifact_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const kind_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(kind_text);

            const body = if (body_opt) |b|
                try std.fmt.allocPrint(allocator, "**Kind:** {s}\n\n{s}", .{ kind_text, b })
            else
                try std.fmt.allocPrint(allocator, "**Kind:** {s}", .{kind_text});

            return .{ .kind = .plan, .title = title, .body = body };
        },
    }
}

// =========================================================================
// Task Board view-model  (tasks 4018, 4019, 4020)
// =========================================================================

/// One row in a Task Board column.
///
/// `status_badge` reflects the task's actual status. The board groups rows
/// into four columns by status:
///
///   open    — tasks with status = 'todo'
///   doing   — tasks with status = 'doing'
///   blocked — tasks with status = 'blocked'
///   done    — tasks with status = 'done'
///
/// Cancelled tasks are excluded from the board (terminal, not actionable).
pub const BoardTaskRow = struct {
    id: i64,
    plan_id: ?i64,
    title: []const u8,
    status_badge: StatusBadge,
    priority: i64,
    /// Active claim token if one exists (for the claiming-agent indicator).
    claim_token: ?[]const u8,

    pub fn deinit(self: BoardTaskRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        if (self.claim_token) |s| allocator.free(s);
    }

    pub fn deinitMany(rows: []BoardTaskRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Grouped task board: tasks bucketed into the four board columns.
/// All slices are caller-owned; free via `TaskBoardSnapshot.deinit`.
pub const TaskBoardSnapshot = struct {
    /// Tasks with status = 'todo' (board column: open).
    open: []BoardTaskRow,
    /// Tasks with status = 'doing'.
    doing: []BoardTaskRow,
    /// Tasks with status = 'blocked'.
    blocked: []BoardTaskRow,
    /// Tasks with status = 'done'.
    done: []BoardTaskRow,

    pub fn deinit(self: TaskBoardSnapshot, allocator: std.mem.Allocator) void {
        BoardTaskRow.deinitMany(self.open, allocator);
        BoardTaskRow.deinitMany(self.doing, allocator);
        BoardTaskRow.deinitMany(self.blocked, allocator);
        BoardTaskRow.deinitMany(self.done, allocator);
    }

    /// Total row count across all columns.
    pub fn totalCount(self: *const TaskBoardSnapshot) usize {
        return self.open.len + self.doing.len + self.blocked.len + self.done.len;
    }
};

/// One reopen record for a task (from task_reopens).
pub const TaskReopenRow = struct {
    id: i64,
    /// Status before the reopen (done or cancelled).
    from_status: []const u8,
    /// Status after the reopen (todo, doing, or blocked).
    to_status: []const u8,
    /// Source verb (task-reopen or task-update-force).
    source: []const u8,
    /// Optional operator-provided reason.
    reason: ?[]const u8,
    /// ISO-8601 created_at.
    created_at: []const u8,

    pub fn deinit(self: TaskReopenRow, allocator: std.mem.Allocator) void {
        allocator.free(self.from_status);
        allocator.free(self.to_status);
        allocator.free(self.source);
        if (self.reason) |s| allocator.free(s);
        allocator.free(self.created_at);
    }

    pub fn deinitMany(rows: []TaskReopenRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One touch-path record for a task (from task_touch_paths).
pub const TaskTouchPathRow = struct {
    id: i64,
    /// Repo-relative file path.
    path: []const u8,
    /// ISO-8601 created_at.
    created_at: []const u8,

    pub fn deinit(self: TaskTouchPathRow, allocator: std.mem.Allocator) void {
        allocator.free(self.path);
        allocator.free(self.created_at);
    }

    pub fn deinitMany(rows: []TaskTouchPathRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One blocking/dependency link row for a task (from entity_links).
///
/// The entity_links `blocks` relationship encodes:
///
///   from_kind=task, from_id=A, to_kind=task, to_id=B, relationship='blocks'
///   → task A blocks task B.
///
/// For the board detail pane we surface:
///   • what blocks this task: where this task is the `to_id` (blocked-by set)
///   • what this task blocks: where this task is the `from_id` (blocks set)
pub const TaskLinkRow = struct {
    id: i64,
    /// Human-readable label: "task:<id> — <title>".
    label: []const u8,
    /// Direction from the perspective of the selected task:
    ///   .blocks_this = another task blocks the selected task
    ///   .this_blocks = the selected task blocks another task
    direction: LinkDirection,

    pub fn deinit(self: TaskLinkRow, allocator: std.mem.Allocator) void {
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []TaskLinkRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Direction of a blocking link relative to the selected task.
pub const LinkDirection = enum {
    /// Another task blocks this task (entity_links: other→this, relationship='blocks').
    blocks_this,
    /// This task blocks another task (entity_links: this→other, relationship='blocks').
    this_blocks,
};

/// Detail pane data for a selected task on the Task Board. Combines the
/// basic task fields with reopen history, touch paths, and blocking links.
///
/// All fields are caller-owned; free via `TaskBoardDetail.deinit`.
pub const TaskBoardDetail = struct {
    /// Task id.
    id: i64,
    /// Task title.
    title: []const u8,
    /// Rendered markdown body (status, priority, next_action, body).
    body: []const u8,
    /// Reopen history (chronological, oldest-first).
    reopens: []TaskReopenRow,
    /// Touch paths (alphabetical).
    touch_paths: []TaskTouchPathRow,
    /// Blocking links (blocked-by first, then blocks).
    links: []TaskLinkRow,

    pub fn deinit(self: TaskBoardDetail, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.body);
        TaskReopenRow.deinitMany(self.reopens, allocator);
        TaskTouchPathRow.deinitMany(self.touch_paths, allocator);
        TaskLinkRow.deinitMany(self.links, allocator);
    }
};

/// Query all non-cancelled tasks and group them into board columns.
///
/// Filters to the current scope if `scope_filter` is `.repo`; shows all
/// scopes when `.all`. Ordered within each column by priority asc, id asc.
///
/// Status values confirmed from migration 00003_work_items.up.sql:
///   check(status in ('todo','doing','blocked','done','cancelled'))
///
/// 'cancelled' tasks are excluded (terminal, not board-actionable).
pub fn queryTaskBoard(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    scope_filter: ScopeFilter,
) !TaskBoardSnapshot {
    var open_rows: std.ArrayList(BoardTaskRow) = .empty;
    errdefer {
        for (open_rows.items) |r| r.deinit(allocator);
        open_rows.deinit(allocator);
    }
    var doing_rows: std.ArrayList(BoardTaskRow) = .empty;
    errdefer {
        for (doing_rows.items) |r| r.deinit(allocator);
        doing_rows.deinit(allocator);
    }
    var blocked_rows: std.ArrayList(BoardTaskRow) = .empty;
    errdefer {
        for (blocked_rows.items) |r| r.deinit(allocator);
        blocked_rows.deinit(allocator);
    }
    var done_rows: std.ArrayList(BoardTaskRow) = .empty;
    errdefer {
        for (done_rows.items) |r| r.deinit(allocator);
        done_rows.deinit(allocator);
    }

    // SQL: select all non-cancelled tasks plus their active claim token
    // (sub-select same as queryTaskRows). Filter to scope when repo.
    const sql_all =
        \\select t.id, t.plan_id, t.title, t.status, t.priority,
        \\       (select c.claim_token from agent_work_claims c
        \\        where c.entity_kind = 'task' and c.entity_id = t.id
        \\          and c.status = 'active'
        \\          and c.lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\        order by c.id desc limit 1) as active_claim_token
        \\from tasks t
        \\where t.status in ('todo','doing','blocked','done')
        \\order by t.priority asc, t.id asc
    ;
    const sql_repo =
        \\select t.id, t.plan_id, t.title, t.status, t.priority,
        \\       (select c.claim_token from agent_work_claims c
        \\        where c.entity_kind = 'task' and c.entity_id = t.id
        \\          and c.status = 'active'
        \\          and c.lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\        order by c.id desc limit 1) as active_claim_token
        \\from tasks t
        \\where t.status in ('todo','doing','blocked','done')
        \\  and (t.scope_kind = 'repo' and t.scope_id = ? or t.scope_kind = 'global')
        \\order by t.priority asc, t.id asc
    ;

    var stmt = switch (scope_filter) {
        .all => blk: {
            var s = d.prepare(sql_all) catch return error.QueryFailed;
            s.bind(&.{}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
        .repo => |pid| blk: {
            var s = d.prepare(sql_repo) catch return error.QueryFailed;
            s.bind(&.{.{ .int = pid }}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
    };
    defer stmt.finalize();

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const pid = stmt.columnIntOpt(1);
                const title = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(title);
                const status_text = try stmt.columnTextAlloc(3, allocator);
                defer allocator.free(status_text);
                const priority = stmt.columnInt(4);
                const claim_token = try stmt.columnTextOpt(5, allocator);

                const row: BoardTaskRow = .{
                    .id = id,
                    .plan_id = pid,
                    .title = title,
                    .status_badge = taskStatusBadge(status_text),
                    .priority = priority,
                    .claim_token = claim_token,
                };

                // Route to the appropriate column.
                if (std.mem.eql(u8, status_text, "todo")) {
                    try open_rows.append(allocator, row);
                } else if (std.mem.eql(u8, status_text, "doing")) {
                    try doing_rows.append(allocator, row);
                } else if (std.mem.eql(u8, status_text, "blocked")) {
                    try blocked_rows.append(allocator, row);
                } else if (std.mem.eql(u8, status_text, "done")) {
                    try done_rows.append(allocator, row);
                } else {
                    // Unknown status — skip (should not happen given the WHERE clause).
                    row.deinit(allocator);
                }
            },
        }
    }

    return .{
        .open = try open_rows.toOwnedSlice(allocator),
        .doing = try doing_rows.toOwnedSlice(allocator),
        .blocked = try blocked_rows.toOwnedSlice(allocator),
        .done = try done_rows.toOwnedSlice(allocator),
    };
}

/// Query reopen history for a task. Rows are ordered chronologically
/// (oldest-first). Caller owns the result; free via `TaskReopenRow.deinitMany`.
pub fn queryTaskReopens(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) ![]TaskReopenRow {
    var stmt = d.prepare(
        \\select id, from_status, to_status, source, reason, created_at
        \\from task_reopens
        \\where task_id = ?
        \\order by created_at asc, id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(TaskReopenRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const from_s = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(from_s);
                const to_s = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(to_s);
                const source = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(source);
                const reason = try stmt.columnTextOpt(4, allocator);
                errdefer if (reason) |s| allocator.free(s);
                const created = try stmt.columnTextAlloc(5, allocator);
                errdefer allocator.free(created);

                try out.append(allocator, .{
                    .id = id,
                    .from_status = from_s,
                    .to_status = to_s,
                    .source = source,
                    .reason = reason,
                    .created_at = created,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query touch paths for a task. Rows are ordered by path (alphabetical).
/// Caller owns the result; free via `TaskTouchPathRow.deinitMany`.
pub fn queryTaskTouchPaths(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) ![]TaskTouchPathRow {
    var stmt = d.prepare(
        \\select id, path, created_at
        \\from task_touch_paths
        \\where task_id = ?
        \\order by path asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(TaskTouchPathRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const path = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(path);
                const created = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(created);

                try out.append(allocator, .{
                    .id = id,
                    .path = path,
                    .created_at = created,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query blocking links for a task from entity_links.
///
/// Surfaces two directions:
///   (1) tasks that block this task: entity_links rows where
///       to_kind='task', to_id=task_id, from_kind='task', relationship='blocks'
///       → direction = .blocks_this
///   (2) tasks this task blocks: entity_links rows where
///       from_kind='task', from_id=task_id, to_kind='task', relationship='blocks'
///       → direction = .this_blocks
///
/// The `blocks` relationship is the only one checked here per migration
/// 00004_entity_links.up.sql, which confirms the valid set:
///   ('derives-from','blocks','addresses','verifies','cites','supersedes','touches')
///
/// Caller owns the result; free via `TaskLinkRow.deinitMany`.
pub fn queryTaskBlockingLinks(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) ![]TaskLinkRow {
    var out: std.ArrayList(TaskLinkRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    // (1) Tasks that block this task (blocked-by set).
    {
        var stmt = d.prepare(
            \\select t.id, t.title
            \\from tasks t
            \\join entity_links el on el.from_kind = 'task'
            \\  and el.from_id = t.id
            \\  and el.to_kind = 'task'
            \\  and el.to_id = ?
            \\  and el.relationship = 'blocks'
            \\order by t.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const label = try std.fmt.allocPrint(
                        allocator,
                        "task:{d} — {s}",
                        .{ id, title },
                    );
                    allocator.free(title);
                    errdefer allocator.free(label);

                    try out.append(allocator, .{
                        .id = id,
                        .label = label,
                        .direction = .blocks_this,
                    });
                },
            }
        }
    }

    // (2) Tasks this task blocks (this_blocks set).
    {
        var stmt = d.prepare(
            \\select t.id, t.title
            \\from tasks t
            \\join entity_links el on el.to_kind = 'task'
            \\  and el.to_id = t.id
            \\  and el.from_kind = 'task'
            \\  and el.from_id = ?
            \\  and el.relationship = 'blocks'
            \\order by t.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const label = try std.fmt.allocPrint(
                        allocator,
                        "task:{d} — {s}",
                        .{ id, title },
                    );
                    allocator.free(title);
                    errdefer allocator.free(label);

                    try out.append(allocator, .{
                        .id = id,
                        .label = label,
                        .direction = .this_blocks,
                    });
                },
            }
        }
    }

    return try out.toOwnedSlice(allocator);
}

/// Query full Task Board detail for a selected task.
///
/// Combines the basic task fields (status, priority, next_action, body)
/// with reopen history (task_reopens), touch paths (task_touch_paths),
/// and blocking/dependency links (entity_links, relationship='blocks').
///
/// Returns null when the task does not exist.
/// Caller owns the result; free via `TaskBoardDetail.deinit`.
pub fn queryTaskBoardDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) !?TaskBoardDetail {
    var stmt = d.prepare(
        "select title, body, status, priority, next_action from tasks where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return null,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const status_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status_text);
            const priority = stmt.columnInt(3);
            const next_action_opt = try stmt.columnTextOpt(4, allocator);
            defer if (next_action_opt) |s| allocator.free(s);

            // Build the markdown body.
            var body_buf: std.ArrayList(u8) = .empty;
            errdefer body_buf.deinit(allocator);

            const first_line = try std.fmt.allocPrint(
                allocator,
                "**Status:** {s}  **Priority:** {d}",
                .{ status_text, priority },
            );
            defer allocator.free(first_line);
            try body_buf.appendSlice(allocator, first_line);

            if (next_action_opt) |na| {
                const part = try std.fmt.allocPrint(allocator, "\n\n**Next action:** {s}", .{na});
                defer allocator.free(part);
                try body_buf.appendSlice(allocator, part);
            }
            if (body_opt) |b| {
                const part = try std.fmt.allocPrint(allocator, "\n\n{s}", .{b});
                defer allocator.free(part);
                try body_buf.appendSlice(allocator, part);
            }

            const body = try body_buf.toOwnedSlice(allocator);
            errdefer allocator.free(body);

            // Sub-queries.
            const reopens = try queryTaskReopens(d, allocator, task_id);
            errdefer TaskReopenRow.deinitMany(reopens, allocator);

            const touch_paths = try queryTaskTouchPaths(d, allocator, task_id);
            errdefer TaskTouchPathRow.deinitMany(touch_paths, allocator);

            const links = try queryTaskBlockingLinks(d, allocator, task_id);
            errdefer TaskLinkRow.deinitMany(links, allocator);

            return .{
                .id = task_id,
                .title = title,
                .body = body,
                .reopens = reopens,
                .touch_paths = touch_paths,
                .links = links,
            };
        },
    }
}

/// Try to resolve the current working directory's scope from the DB.
/// Returns the project id if a single-association repo match is found,
/// or null if the cwd maps to multiple/no scopes (caller falls back to
/// all-scopes mode).
///
/// This is a best-effort lookup: failures (SQL error, cwd not found)
/// return null silently rather than crashing the cockpit.
pub fn cwdScopeProjectId(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    cwd: []const u8,
) ?i64 {
    // Find the project whose root_path is the longest prefix of cwd.
    var stmt = d.prepare(
        "select id, root_path from projects where root_path is not null order by length(root_path) desc",
    ) catch return null;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return null;

    while (true) {
        switch (stmt.step() catch return null) {
            .done => return null,
            .row => {
                const pid = stmt.columnInt(0);
                const root = stmt.columnTextOpt(1, allocator) catch return null;
                if (root) |r| {
                    defer allocator.free(r);
                    if (std.mem.startsWith(u8, cwd, r)) {
                        // Count associations for this project.
                        var count_stmt = d.prepare(
                            "select count(*) from project_associations where project_id = ?",
                        ) catch return pid; // conservative: return pid on error
                        defer count_stmt.finalize();
                        count_stmt.bind(&.{.{ .int = pid }}) catch return pid;
                        switch (count_stmt.step() catch return pid) {
                            .done => return pid,
                            .row => {
                                const cnt = count_stmt.columnInt(0);
                                // Only filter to this scope when the project
                                // has at least one association.
                                if (cnt >= 1) return pid;
                                return null;
                            },
                        }
                    }
                }
            },
        }
    }
}

// =========================================================================
// View registry
// =========================================================================

/// Registered view IDs for the view-switcher.
pub const ViewId = enum {
    agent_monitor,
    scope_explorer,
    task_board,
};

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "view_model: StatusBadge.glyph returns expected chars" {
    try testing.expectEqualStrings("A", StatusBadge.active.glyph());
    try testing.expectEqualStrings("S", StatusBadge.stale.glyph());
    try testing.expectEqualStrings("D", StatusBadge.done.glyph());
    try testing.expectEqualStrings(">", StatusBadge.doing.glyph());
    try testing.expectEqualStrings("B", StatusBadge.blocked.glyph());
    try testing.expectEqualStrings(" ", StatusBadge.none.glyph());
}

test "view_model: queryAgentMonitor on empty DB returns empty slices" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const snap = try queryAgentMonitor(&d, a);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 0), snap.active.len);
    try testing.expectEqual(@as(usize, 0), snap.stale.len);
}

test "view_model: queryAgentMonitor surfaces active claims" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert a session, a task, and acquire a claim.
    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','vm-test-task','todo')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\  status, vendor, lease_expires_at
        \\) values (
        \\  'test-tok', ?, 'task', ?, 'exclusive', 'active', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '+600 seconds')
        \\)
    ,
        &.{
            .{ .int = sid },
            .{ .int = tid },
        },
    );

    const snap = try queryAgentMonitor(&d, a);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 1), snap.active.len);
    try testing.expectEqual(@as(usize, 0), snap.stale.len);
    try testing.expectEqual(StatusBadge.active, snap.active[0].badge);
    try testing.expectEqualStrings("claude", snap.active[0].vendor);
    // Verify entity_ref has "task:<id>" format.
    try testing.expect(std.mem.startsWith(u8, snap.active[0].entity_ref, "task:"));
}

test "view_model: queryAgentMonitor surfaces stale claims" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','stale-task','todo')",
        &.{},
    );
    // Insert an already-expired (stale) active claim.
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\  status, vendor, lease_expires_at
        \\) values (
        \\  'stale-tok', ?, 'task', ?, 'exclusive', 'stale', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '-60 seconds')
        \\)
    ,
        &.{
            .{ .int = sid },
            .{ .int = tid },
        },
    );

    const snap = try queryAgentMonitor(&d, a);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 0), snap.active.len);
    try testing.expectEqual(@as(usize, 1), snap.stale.len);
    try testing.expectEqual(StatusBadge.stale, snap.stale[0].badge);
}

// =========================================================================
// HeartbeatAge / parseIso8601Unix tests (tasks 4015, 4017)
// =========================================================================

test "view_model: classifyHeartbeatAge returns fresh when well within TTL" {
    // now=100, claimed=0, expires=200, last_hb=90 → age=10, TTL=200, TTL/2=100 → fresh
    const age = classifyHeartbeatAge(100, 90, 200, 0);
    try testing.expectEqual(HeartbeatAge.fresh, age);
}

test "view_model: classifyHeartbeatAge returns warning when past TTL/2" {
    // now=160, claimed=0, expires=200, last_hb=10 → age=150, TTL=200, TTL/2=100 → warning
    const age = classifyHeartbeatAge(160, 10, 200, 0);
    try testing.expectEqual(HeartbeatAge.warning, age);
}

test "view_model: classifyHeartbeatAge returns stale when lease expired" {
    // now=300, expires=200 → stale regardless of heartbeat
    const age = classifyHeartbeatAge(300, 190, 200, 0);
    try testing.expectEqual(HeartbeatAge.stale, age);
}

test "view_model: classifyHeartbeatAge stale with status=stale record" {
    // Simulates an already-stale claim: expires in past
    const age = classifyHeartbeatAge(1000, 500, 600, 0);
    try testing.expectEqual(HeartbeatAge.stale, age);
}

test "view_model: classifyHeartbeatAge zero TTL falls back to warning" {
    // claimed_at == lease_expires_at → TTL=0 → guard triggers → warning
    const age = classifyHeartbeatAge(100, 100, 200, 200);
    try testing.expectEqual(HeartbeatAge.warning, age);
}

test "view_model: parseIso8601Unix round-trips a known epoch" {
    // 1970-01-01T00:00:00.000Z = Unix epoch 0
    const result = parseIso8601Unix("1970-01-01T00:00:00.000Z");
    try testing.expect(result != null);
    try testing.expectEqual(@as(i64, 0), result.?);
}

test "view_model: parseIso8601Unix returns null for short string" {
    try testing.expectEqual(@as(?i64, null), parseIso8601Unix("short"));
}

test "view_model: parseIso8601Unix handles 2026-06-14T12:00:00Z" {
    // Sanity-check a date in the project's active timeframe.
    const result = parseIso8601Unix("2026-06-14T12:00:00.000Z");
    try testing.expect(result != null);
    // Should be substantially past 2020 (> 1577836800).
    try testing.expect(result.? > 1_577_836_800);
}

// =========================================================================
// AgentActionStream tests (task 4016)
// =========================================================================

test "view_model: queryAgentActionStream on empty DB returns empty slice" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const rows = try queryAgentActionStream(&d, a, 50);
    defer AgentActionRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryAgentActionStream returns action rows" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    // Insert a task and a claim (needed for action FK).
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','stream-task','doing')",
        &.{},
    );
    _ = tid;
    // Insert a completed action directly.
    _ = try d.execParams(
        \\insert into agent_actions (
        \\  session_id, action_kind, vendor, started_at, ended_at, outcome, summary
        \\) values (
        \\  ?, 'coder', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '-10 seconds'),
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now'),
        \\  'ok', 'wrote the code'
        \\)
    , &.{.{ .int = sid }});

    const rows = try queryAgentActionStream(&d, a, 50);
    defer AgentActionRow.deinitMany(rows, a);

    try testing.expect(rows.len >= 1);
    // The first row should be the action we inserted.
    try testing.expectEqualStrings("coder", rows[0].kind_label);
    try testing.expect(rows[0].summary != null);
    try testing.expectEqualStrings("wrote the code", rows[0].summary.?);
}

test "view_model: queryAgentActionStream includes claim transitions" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','trans-task','done')",
        &.{},
    );
    // Insert a completed claim (terminal status).
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\  status, vendor, lease_expires_at,
        \\  released_at, release_reason
        \\) values (
        \\  'comp-tok', ?, 'task', ?, 'exclusive', 'completed', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '+600 seconds'),
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now'),
        \\  'work done'
        \\)
    , &.{
        .{ .int = sid },
        .{ .int = tid },
    });

    const rows = try queryAgentActionStream(&d, a, 50);
    defer AgentActionRow.deinitMany(rows, a);

    // Should contain the claim transition row.
    var found_claim = false;
    for (rows) |r| {
        if (std.mem.startsWith(u8, r.kind_label, "claim:")) {
            found_claim = true;
            try testing.expectEqualStrings("claim:completed", r.kind_label);
        }
    }
    try testing.expect(found_claim);
}

test "view_model: queryAgentActionStream limit is respected" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('test')",
        &.{},
    );
    // Insert 5 action rows.
    var i: usize = 0;
    while (i < 5) : (i += 1) {
        _ = try d.execParams(
            \\insert into agent_actions (session_id, action_kind, vendor, started_at)
            \\values (?, 'other', 'test', strftime('%Y-%m-%dT%H:%M:%fZ','now'))
        , &.{.{ .int = sid }});
    }

    const rows = try queryAgentActionStream(&d, a, 3);
    defer AgentActionRow.deinitMany(rows, a);
    try testing.expect(rows.len <= 3);
}

test "view_model: queryPlanNodes on empty DB returns empty slice" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const nodes = try queryPlanNodes(&d, a);
    defer {
        for (nodes) |n| n.deinit(a);
        a.free(nodes);
    }
    try testing.expectEqual(@as(usize, 0), nodes.len);
}

test "view_model: queryPlanNodes populates badge and counts" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert an active plan.
    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Test Plan','test-plan','active')",
        &.{},
    );
    // Insert two tasks, one done.
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 'task-a','done')",
        &.{.{ .int = plan_id }},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 'task-b','todo')",
        &.{.{ .int = plan_id }},
    );

    const nodes = try queryPlanNodes(&d, a);
    defer {
        for (nodes) |n| n.deinit(a);
        a.free(nodes);
    }

    try testing.expectEqual(@as(usize, 1), nodes.len);
    try testing.expectEqual(StatusBadge.active, nodes[0].status_badge);
    try testing.expectEqual(@as(u32, 2), nodes[0].task_count);
    try testing.expectEqual(@as(u32, 1), nodes[0].done_count);
    try testing.expectEqualStrings("Test Plan", nodes[0].title);
    try testing.expectEqualStrings("test-plan", nodes[0].slug);
}

test "view_model: queryTaskRows on missing plan returns empty" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const rows = try queryTaskRows(&d, a, 9999);
    defer TaskRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryTaskRows returns tasks with correct status badges" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','P1','p1','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 'T1','doing',10)",
        &.{.{ .int = plan_id }},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 'T2','blocked',20)",
        &.{.{ .int = plan_id }},
    );

    const rows = try queryTaskRows(&d, a, plan_id);
    defer TaskRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 2), rows.len);
    try testing.expectEqual(StatusBadge.doing, rows[0].status_badge);
    try testing.expectEqual(StatusBadge.blocked, rows[1].status_badge);
    try testing.expectEqual(@as(?[]const u8, null), rows[0].claim_token);
}

test "view_model: queryTaskRows surfaces active claim token" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','ClaimPlan','cp','active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 'claimtask','doing')",
        &.{.{ .int = plan_id }},
    );
    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\  status, vendor, lease_expires_at
        \\) values (
        \\  'active-claim-tok', ?, 'task', ?, 'exclusive', 'active', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '+600 seconds')
        \\)
    ,
        &.{
            .{ .int = sid },
            .{ .int = task_id },
        },
    );

    const rows = try queryTaskRows(&d, a, plan_id);
    defer TaskRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expect(rows[0].claim_token != null);
    try testing.expectEqualStrings("active-claim-tok", rows[0].claim_token.?);
}

test "view_model: queryPlanDetail returns empty pane for missing plan" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const detail = try queryPlanDetail(&d, a, 9999);
    defer detail.deinit(a);
    try testing.expectEqual(DetailKind.empty, detail.kind);
}

test "view_model: queryPlanDetail returns plan title and status" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status, summary) values ('global','Detail Plan','dp','active','A plan summary')",
        &.{},
    );

    const detail = try queryPlanDetail(&d, a, pid);
    defer detail.deinit(a);

    try testing.expectEqual(DetailKind.plan, detail.kind);
    try testing.expectEqualStrings("Detail Plan", detail.title);
    // Body should contain the status and summary.
    try testing.expect(std.mem.indexOf(u8, detail.body, "active") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "A plan summary") != null);
}

test "view_model: queryTaskDetail returns empty pane for missing task" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const detail = try queryTaskDetail(&d, a, 9999);
    defer detail.deinit(a);
    try testing.expectEqual(DetailKind.empty, detail.kind);
}

test "view_model: queryTaskDetail returns task fields" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, body, status, priority, next_action) values ('global','My Task','Task body text','doing',5,'Write the code')",
        &.{},
    );

    const detail = try queryTaskDetail(&d, a, task_id);
    defer detail.deinit(a);

    try testing.expectEqual(DetailKind.task, detail.kind);
    try testing.expectEqualStrings("My Task", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "doing") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "Write the code") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "Task body text") != null);
}

test "view_model: DetailPane.empty is valid" {
    const a = testing.allocator;
    const pane = try DetailPane.empty(a);
    defer pane.deinit(a);
    try testing.expectEqual(DetailKind.empty, pane.kind);
    try testing.expectEqualStrings("", pane.title);
    try testing.expectEqualStrings("", pane.body);
}

// =========================================================================
// Scope Explorer tests (tasks 3966–3970)
// =========================================================================

test "view_model: queryPlanNodesFiltered all-scopes on empty DB returns empty" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const nodes = try queryPlanNodesFiltered(&d, a, .all);
    defer {
        for (nodes) |n| n.deinit(a);
        a.free(nodes);
    }
    try testing.expectEqual(@as(usize, 0), nodes.len);
}

test "view_model: queryPlanNodesFiltered all-scopes returns plans" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Alpha','alpha','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Beta','beta','draft')",
        &.{},
    );

    const nodes = try queryPlanNodesFiltered(&d, a, .all);
    defer {
        for (nodes) |n| n.deinit(a);
        a.free(nodes);
    }
    try testing.expectEqual(@as(usize, 2), nodes.len);
}

test "view_model: queryPlanNodesFiltered repo filter excludes mismatched scope" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert a project and a repo-scoped plan.
    const proj_id = try d.execParams(
        "insert into projects (slug, name, root_path) values ('proj1','Proj1','/work/proj1')",
        &.{},
    );
    _ = try d.execParams(
        "insert into plans (scope_kind, scope_id, title, slug, status) values ('repo', ?, 'Repo Plan','rp','active')",
        &.{.{ .int = proj_id }},
    );
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Global Plan','gp','active')",
        &.{},
    );

    // Filter to proj_id: should include both the repo-scoped plan and
    // global plans (global is always included per the SQL filter).
    const nodes = try queryPlanNodesFiltered(&d, a, .{ .repo = proj_id });
    defer {
        for (nodes) |n| n.deinit(a);
        a.free(nodes);
    }
    // Repo plan + global plan = 2.
    try testing.expectEqual(@as(usize, 2), nodes.len);
}

test "view_model: queryPlanDrillRows returns tasks and linked decisions" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','DrillPlan','dp','active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 'T1','todo')",
        &.{.{ .int = plan_id }},
    );
    _ = task_id;
    // Insert a decision and link it to the plan.
    // decisions.body is NOT NULL — provide a non-empty body.
    const dec_id = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','D1','Decision body','accepted')",
        &.{},
    );
    // entity_links uses from_kind/from_id/to_kind/to_id/relationship.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('plan', ?, 'decision', ?, 'derives-from')",
        &.{ .{ .int = plan_id }, .{ .int = dec_id } },
    );

    const rows = try queryPlanDrillRows(&d, a, plan_id);
    defer DrillRow.deinitMany(rows, a);

    // At minimum: 1 task + 1 decision.
    try testing.expect(rows.len >= 2);
    // First rows should be tasks.
    try testing.expectEqual(DrillKind.task, rows[0].kind);
    // Find the decision in the result.
    var found_dec = false;
    for (rows) |r| {
        if (r.kind == .decision) {
            found_dec = true;
            try testing.expectEqual(StatusBadge.done, r.status_badge);
        }
    }
    try testing.expect(found_dec);
}

test "view_model: queryPlanDrillRows empty plan returns empty" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Empty','ep','draft')",
        &.{},
    );
    const rows = try queryPlanDrillRows(&d, a, plan_id);
    defer DrillRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryDecisionDetail returns body and status" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // decisions.body is NOT NULL and decisions.status values: proposed/accepted/superseded/withdrawn.
    const did = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Dec Title','Dec body text','accepted')",
        &.{},
    );
    const detail = try queryDecisionDetail(&d, a, did);
    defer detail.deinit(a);

    try testing.expectEqualStrings("Dec Title", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "accepted") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "Dec body text") != null);
}

test "view_model: queryQuestionDetail returns empty for missing" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const detail = try queryQuestionDetail(&d, a, 9999);
    defer detail.deinit(a);
    try testing.expectEqual(DetailKind.empty, detail.kind);
}

test "view_model: queryScenarioDetail returns body and status" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // test_scenarios status: draft, ready, verified, failing, retired.
    const sid = try d.execParams(
        "insert into test_scenarios (scope_kind, title, body, status) values ('global','Scenario Title','Scenario body','verified')",
        &.{},
    );
    const detail = try queryScenarioDetail(&d, a, sid);
    defer detail.deinit(a);

    try testing.expectEqualStrings("Scenario Title", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "verified") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "Scenario body") != null);
}

test "view_model: queryArtifactDetail returns kind and body" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // artifacts.kind is the column name (not artifact_kind).
    const aid = try d.execParams(
        "insert into artifacts (scope_kind, kind, title, body) values ('global','tech_spec','Artifact Title','Artifact body text')",
        &.{},
    );
    const detail = try queryArtifactDetail(&d, a, aid);
    defer detail.deinit(a);

    try testing.expectEqualStrings("Artifact Title", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "tech_spec") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "Artifact body text") != null);
}

test "view_model: cwdScopeProjectId returns null on empty DB" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const result = cwdScopeProjectId(&d, a, "/some/path");
    try testing.expectEqual(@as(?i64, null), result);
}

test "view_model: cwdScopeProjectId returns project id when path matches" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // projects requires name (NOT NULL).
    const proj_id = try d.execParams(
        "insert into projects (slug, name, root_path) values ('myrepo','MyRepo','/work/myrepo')",
        &.{},
    );
    // Add an association so the count >= 1 path is satisfied.
    const assoc_id = try d.execParams(
        "insert into associations (slug, name, kind) values ('myorg','MyOrg','org')",
        &.{},
    );
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = proj_id }, .{ .int = assoc_id } },
    );

    const result = cwdScopeProjectId(&d, a, "/work/myrepo/src/foo");
    try testing.expectEqual(proj_id, result.?);
}

test "view_model: decisionStatusBadge maps known statuses" {
    // decisions status: proposed, accepted, superseded, withdrawn.
    try testing.expectEqual(StatusBadge.done, decisionStatusBadge("accepted"));
    try testing.expectEqual(StatusBadge.cancelled, decisionStatusBadge("withdrawn"));
    try testing.expectEqual(StatusBadge.abandoned, decisionStatusBadge("superseded"));
    try testing.expectEqual(StatusBadge.draft, decisionStatusBadge("proposed"));
}

test "view_model: questionStatusBadge maps known statuses" {
    // questions status: open, answered, wontfix.
    try testing.expectEqual(StatusBadge.todo, questionStatusBadge("open"));
    try testing.expectEqual(StatusBadge.done, questionStatusBadge("answered"));
    try testing.expectEqual(StatusBadge.cancelled, questionStatusBadge("wontfix"));
}

test "view_model: scenarioStatusBadge maps known statuses" {
    // test_scenarios status: draft, ready, verified, failing, retired.
    try testing.expectEqual(StatusBadge.draft, scenarioStatusBadge("draft"));
    try testing.expectEqual(StatusBadge.done, scenarioStatusBadge("verified"));
    try testing.expectEqual(StatusBadge.blocked, scenarioStatusBadge("failing"));
    try testing.expectEqual(StatusBadge.abandoned, scenarioStatusBadge("retired"));
}

// =========================================================================
// Task Board view-model tests (tasks 4018, 4019, 4020)
// =========================================================================

test "view_model: queryTaskBoard on empty DB yields empty snapshot" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const snap = try queryTaskBoard(&d, a, .all);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 0), snap.open.len);
    try testing.expectEqual(@as(usize, 0), snap.doing.len);
    try testing.expectEqual(@as(usize, 0), snap.blocked.len);
    try testing.expectEqual(@as(usize, 0), snap.done.len);
    try testing.expectEqual(@as(usize, 0), snap.totalCount());
}

test "view_model: queryTaskBoard groups tasks into four status columns (task 4018)" {
    // Verifies: tasks across all four status groups land in the right column.
    // Status enum confirmed from migration 00003_work_items.up.sql:
    //   check(status in ('todo','doing','blocked','done','cancelled'))
    // 'todo' → open, 'doing' → doing, 'blocked' → blocked, 'done' → done.
    // 'cancelled' is excluded.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','A','todo',1)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','B','doing',2)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','C','blocked',3)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','D','done',4)",
        &.{},
    );
    // Cancelled should be excluded.
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','E','cancelled',5)",
        &.{},
    );

    const snap = try queryTaskBoard(&d, a, .all);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 1), snap.open.len);
    try testing.expectEqual(@as(usize, 1), snap.doing.len);
    try testing.expectEqual(@as(usize, 1), snap.blocked.len);
    try testing.expectEqual(@as(usize, 1), snap.done.len);
    try testing.expectEqual(@as(usize, 4), snap.totalCount());

    // Verify badge mapping.
    try testing.expectEqual(StatusBadge.todo, snap.open[0].status_badge);
    try testing.expectEqual(StatusBadge.doing, snap.doing[0].status_badge);
    try testing.expectEqual(StatusBadge.blocked, snap.blocked[0].status_badge);
    try testing.expectEqual(StatusBadge.done, snap.done[0].status_badge);

    // Verify titles landed in the right column.
    try testing.expectEqualStrings("A", snap.open[0].title);
    try testing.expectEqualStrings("B", snap.doing[0].title);
    try testing.expectEqualStrings("C", snap.blocked[0].title);
    try testing.expectEqualStrings("D", snap.done[0].title);
}

test "view_model: queryTaskBoard multiple tasks per column ordered by priority (task 4018)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','High','todo',1)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','Low','todo',99)",
        &.{},
    );

    const snap = try queryTaskBoard(&d, a, .all);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 2), snap.open.len);
    // Priority 1 comes before priority 99.
    try testing.expectEqualStrings("High", snap.open[0].title);
    try testing.expectEqualStrings("Low", snap.open[1].title);
}

test "view_model: queryTaskBoard repo filter excludes cancelled global task (sql_repo precedence fix)" {
    // Regression test for the operator-precedence bug in sql_repo:
    //   BUGGY:  where status in (...) and (scope_kind='repo' and scope_id=?) or scope_kind='global'
    //           parsed as: (status filter AND repo clause) OR global clause
    //           → returns ALL global tasks regardless of status, including cancelled.
    //   FIXED:  where status in (...) and (scope_kind='repo' and scope_id=? or scope_kind='global')
    //           → status filter applies to both repo and global tasks.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('filter-repo','FilterRepo')",
        &.{},
    );
    // A cancelled global task — must be excluded under repo filter.
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','CancelledGlobal','cancelled')",
        &.{},
    );
    // An in-scope todo task — must appear.
    _ = try d.execParams(
        "insert into tasks (scope_kind, scope_id, title, status, priority) values ('repo', ?, 'RepoTodo','todo',1)",
        &.{.{ .int = proj_id }},
    );

    const snap = try queryTaskBoard(&d, a, .{ .repo = proj_id });
    defer snap.deinit(a);

    // Only the in-scope todo task should appear; cancelled global must be excluded.
    try testing.expectEqual(@as(usize, 1), snap.open.len);
    try testing.expectEqual(@as(usize, 0), snap.doing.len);
    try testing.expectEqual(@as(usize, 0), snap.blocked.len);
    try testing.expectEqual(@as(usize, 0), snap.done.len);
    try testing.expectEqualStrings("RepoTodo", snap.open[0].title);
}

test "view_model: queryTaskBoard only-cancelled tasks yields empty snapshot (task 4018 empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','CancelledTask','cancelled')",
        &.{},
    );

    const snap = try queryTaskBoard(&d, a, .all);
    defer snap.deinit(a);
    try testing.expectEqual(@as(usize, 0), snap.totalCount());
}

test "view_model: queryTaskReopens on task with no reopens returns empty (task 4019)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','NoReopenTask','done')",
        &.{},
    );

    const rows = try queryTaskReopens(&d, a, tid);
    defer TaskReopenRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryTaskReopens surfaces reopen history (task 4019)" {
    // Seeds a task with two reopens and asserts both appear in order.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','ReopenTask','doing')",
        &.{},
    );
    // First reopen: done → todo.
    _ = try d.execParams(
        \\insert into task_reopens (task_id, from_status, to_status, source, reason, created_at)
        \\values (?, 'done', 'todo', 'task-reopen', 'first reopen',
        \\        strftime('%Y-%m-%dT%H:%M:%fZ','now','-60 seconds'))
    , &.{.{ .int = tid }});
    // Second reopen: cancelled → doing.
    _ = try d.execParams(
        \\insert into task_reopens (task_id, from_status, to_status, source, reason, created_at)
        \\values (?, 'cancelled', 'doing', 'task-update-force', 'second reopen',
        \\        strftime('%Y-%m-%dT%H:%M:%fZ','now'))
    , &.{.{ .int = tid }});

    const rows = try queryTaskReopens(&d, a, tid);
    defer TaskReopenRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 2), rows.len);
    // Oldest-first: first reopen is done→todo.
    try testing.expectEqualStrings("done", rows[0].from_status);
    try testing.expectEqualStrings("todo", rows[0].to_status);
    try testing.expectEqualStrings("task-reopen", rows[0].source);
    try testing.expect(rows[0].reason != null);
    try testing.expectEqualStrings("first reopen", rows[0].reason.?);
    // Second reopen is cancelled→doing.
    try testing.expectEqualStrings("cancelled", rows[1].from_status);
    try testing.expectEqualStrings("doing", rows[1].to_status);
}

test "view_model: queryTaskTouchPaths on task with no paths returns empty (task 4019)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','NoPaths','todo')",
        &.{},
    );

    const rows = try queryTaskTouchPaths(&d, a, tid);
    defer TaskTouchPathRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryTaskTouchPaths surfaces touch paths alphabetically (task 4019)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','TouchTask','doing')",
        &.{},
    );
    // Insert a project for the repo_id FK.
    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('touch-repo','TouchRepo')",
        &.{},
    );
    // Insert two paths; verify alphabetical order.
    _ = try d.execParams(
        "insert into task_touch_paths (task_id, repo_id, path) values (?, ?, 'src/z.zig')",
        &.{ .{ .int = tid }, .{ .int = proj_id } },
    );
    _ = try d.execParams(
        "insert into task_touch_paths (task_id, repo_id, path) values (?, ?, 'src/a.zig')",
        &.{ .{ .int = tid }, .{ .int = proj_id } },
    );

    const rows = try queryTaskTouchPaths(&d, a, tid);
    defer TaskTouchPathRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 2), rows.len);
    // Alphabetical: src/a.zig before src/z.zig.
    try testing.expectEqualStrings("src/a.zig", rows[0].path);
    try testing.expectEqualStrings("src/z.zig", rows[1].path);
}

test "view_model: queryTaskBlockingLinks on task with no links returns empty (task 4020)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','NoLinks','todo')",
        &.{},
    );

    const rows = try queryTaskBlockingLinks(&d, a, tid);
    defer TaskLinkRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryTaskBlockingLinks surfaces blocks-this direction (task 4020)" {
    // Task A blocks task B. Query from B's perspective → direction=.blocks_this.
    // entity_links relationship 'blocks' confirmed from migration 00004:
    //   check(relationship in ('derives-from','blocks','addresses','verifies','cites','supersedes','touches'))
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_a = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','TaskA','done')",
        &.{},
    );
    const task_b = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','TaskB','blocked')",
        &.{},
    );
    // A blocks B: from_id=A, to_id=B.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'blocks')",
        &.{ .{ .int = task_a }, .{ .int = task_b } },
    );

    // Query from B's perspective.
    const rows = try queryTaskBlockingLinks(&d, a, task_b);
    defer TaskLinkRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqual(LinkDirection.blocks_this, rows[0].direction);
    // Label format: "task:<id> — <title>".
    try testing.expect(std.mem.indexOf(u8, rows[0].label, "TaskA") != null);
}

test "view_model: queryTaskBlockingLinks surfaces blocked-by-this direction (task 4020)" {
    // Task A blocks task B. Query from A's perspective → direction=.this_blocks.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_a = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Blocker','doing')",
        &.{},
    );
    const task_b = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Blockee','blocked')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'blocks')",
        &.{ .{ .int = task_a }, .{ .int = task_b } },
    );

    // Query from A's perspective.
    const rows = try queryTaskBlockingLinks(&d, a, task_a);
    defer TaskLinkRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqual(LinkDirection.this_blocks, rows[0].direction);
    try testing.expect(std.mem.indexOf(u8, rows[0].label, "Blockee") != null);
}

test "view_model: queryTaskBlockingLinks both directions simultaneously (task 4020)" {
    // Task X blocks task Y; task Z blocks task X.
    // From X's perspective: 1 blocks_this (Z blocks X) + 1 this_blocks (X blocks Y).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_x = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','X','blocked')",
        &.{},
    );
    const task_y = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Y','blocked')",
        &.{},
    );
    const task_z = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Z','done')",
        &.{},
    );
    // Z blocks X.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'blocks')",
        &.{ .{ .int = task_z }, .{ .int = task_x } },
    );
    // X blocks Y.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'blocks')",
        &.{ .{ .int = task_x }, .{ .int = task_y } },
    );

    const rows = try queryTaskBlockingLinks(&d, a, task_x);
    defer TaskLinkRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 2), rows.len);
    // First: blocks_this (Z blocks X).
    try testing.expectEqual(LinkDirection.blocks_this, rows[0].direction);
    // Second: this_blocks (X blocks Y).
    try testing.expectEqual(LinkDirection.this_blocks, rows[1].direction);
}

test "view_model: queryTaskBoardDetail returns null for missing task" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const result = try queryTaskBoardDetail(&d, a, 9999);
    try testing.expectEqual(@as(?TaskBoardDetail, null), result);
}

test "view_model: queryTaskBoardDetail full detail: body + reopens + touch_paths + links (tasks 4019, 4020)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Seed the task under test.
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, body, status, priority, next_action) values ('global','DetailTask','task body','doing',7,'next step')",
        &.{},
    );

    // Seed a reopen.
    _ = try d.execParams(
        \\insert into task_reopens (task_id, from_status, to_status, source)
        \\values (?, 'done', 'doing', 'task-reopen')
    , &.{.{ .int = tid }});

    // Seed a touch path.
    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('det-repo','DetRepo')",
        &.{},
    );
    _ = try d.execParams(
        "insert into task_touch_paths (task_id, repo_id, path) values (?, ?, 'src/detail.zig')",
        &.{ .{ .int = tid }, .{ .int = proj_id } },
    );

    // Seed a blocking link: another task blocks this one.
    const blocker_id = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Blocker','done')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'blocks')",
        &.{ .{ .int = blocker_id }, .{ .int = tid } },
    );

    const detail_opt = try queryTaskBoardDetail(&d, a, tid);
    try testing.expect(detail_opt != null);
    const detail = detail_opt.?;
    defer detail.deinit(a);

    // Body must mention status and priority.
    try testing.expectEqualStrings("DetailTask", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "doing") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "7") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "next step") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "task body") != null);

    // Reopens.
    try testing.expectEqual(@as(usize, 1), detail.reopens.len);
    try testing.expectEqualStrings("done", detail.reopens[0].from_status);
    try testing.expectEqualStrings("doing", detail.reopens[0].to_status);

    // Touch paths.
    try testing.expectEqual(@as(usize, 1), detail.touch_paths.len);
    try testing.expectEqualStrings("src/detail.zig", detail.touch_paths[0].path);

    // Blocking links.
    try testing.expectEqual(@as(usize, 1), detail.links.len);
    try testing.expectEqual(LinkDirection.blocks_this, detail.links[0].direction);
    try testing.expect(std.mem.indexOf(u8, detail.links[0].label, "Blocker") != null);
}

test "view_model compiles" {
    std.testing.refAllDecls(@This());
}
