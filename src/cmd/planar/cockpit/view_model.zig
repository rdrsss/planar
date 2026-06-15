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
// Decision Log view-model  (tasks 4021, 4022)
// =========================================================================

/// One row in the Decision Log navigator list. Ordered chronologically
/// (newest-first) so the most recent decision is at the top.
///
/// Schema reference: migrations/00002_planning.up.sql
///   decisions(id, title, body, status, decided_at, created_at, ...)
///   status check: ('proposed','accepted','superseded','withdrawn')
pub const DecisionLogRow = struct {
    id: i64,
    /// Decision title.
    title: []const u8,
    /// Status badge derived from the decisions.status column.
    badge: StatusBadge,
    /// ISO-8601 decided_at (nullable) or created_at as fallback for display.
    date_display: []const u8,
    /// Pre-formatted navigator display string: "[badge] YYYY-MM-DD  title".
    /// Heap-allocated so that grapheme pointers from printSegment remain valid
    /// after the render function returns (required for render-level tests).
    display_text: []const u8,

    pub fn deinit(self: DecisionLogRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.date_display);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []DecisionLogRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One derives-from target for a decision (from entity_links).
///
/// entity_links relationship = 'derives-from' means the decision was derived
/// from an artifact, plan, or spec. Per migration 00004_entity_links.up.sql
/// the valid relationships include 'derives-from'.
///
/// This struct surfaces both directions:
///   decision → artifact/plan (from_kind='decision', to_kind=<target>)
///   artifact/plan → decision is intentionally excluded (derives-from is
///   a directional edge — the decision "derives from" something).
pub const DecisionDerivesFromRow = struct {
    /// Target entity kind string, e.g. "artifact", "plan".
    target_kind: []const u8,
    /// Target entity id.
    target_id: i64,
    /// Human-readable label: "<kind>:<id> — <title>" or "<kind>:<id>".
    label: []const u8,

    pub fn deinit(self: DecisionDerivesFromRow, allocator: std.mem.Allocator) void {
        allocator.free(self.target_kind);
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []DecisionDerivesFromRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Detail pane data for a selected decision in the Decision Log.
///
/// Combines the decision's markdown body (task 4022) with the list of
/// derives-from targets sourced from entity_links (task 4022).
///
/// All fields are caller-owned; free via `DecisionLogDetail.deinit`.
pub const DecisionLogDetail = struct {
    /// Decision id.
    id: i64,
    /// Decision title.
    title: []const u8,
    /// Rendered markdown body: "**Status:** {s}\n\n{body_raw}".
    /// Always non-empty (decisions.body is NOT NULL in the schema).
    body: []const u8,
    /// derives-from targets from entity_links. May be empty.
    derives_from: []DecisionDerivesFromRow,

    pub fn deinit(self: DecisionLogDetail, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.body);
        DecisionDerivesFromRow.deinitMany(self.derives_from, allocator);
    }
};

/// Query all decisions, ordered newest-first (decided_at desc, created_at desc, id desc).
///
/// Uses decided_at as the primary sort key, falling back to created_at when
/// decided_at is NULL (proposed decisions have no decided_at).
///
/// Scope filter: when `filter` is `.repo`, includes only decisions whose
/// scope_kind='repo' and scope_id matches the project id, plus global decisions.
/// When `.all`, returns all decisions.
///
/// Caller owns the result; free via `DecisionLogRow.deinitMany`.
pub fn queryDecisionLog(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: ScopeFilter,
) ![]DecisionLogRow {
    const sql_all =
        \\select id, title, status,
        \\       coalesce(decided_at, created_at) as date_display
        \\from decisions
        \\order by coalesce(decided_at, created_at) desc, id desc
    ;
    const sql_repo =
        \\select id, title, status,
        \\       coalesce(decided_at, created_at) as date_display
        \\from decisions
        \\where (scope_kind = 'repo' and scope_id = ?) or scope_kind = 'global'
        \\order by coalesce(decided_at, created_at) desc, id desc
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

    var out: std.ArrayList(DecisionLogRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const status_text = try stmt.columnTextAlloc(2, allocator);
                defer allocator.free(status_text);
                const date_str = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(date_str);

                const badge = decisionStatusBadge(status_text);

                // Pre-format the navigator display string so that grapheme
                // pointers from printSegment remain valid after render returns.
                const date_slice = if (date_str.len >= 10) date_str[0..10] else date_str;
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s}  {s}",
                    .{ badge.glyph(), date_slice, title },
                );
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .badge = badge,
                    .date_display = date_str,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query derives-from targets for a decision from entity_links.
///
/// Finds entity_links rows where:
///   from_kind='decision', from_id=decision_id, relationship='derives-from'
///
/// Per migration 00004_entity_links.up.sql:
///   relationship check: ('derives-from','blocks','addresses','verifies','cites','supersedes','touches')
///
/// Resolves the target title by joining to the appropriate table.
/// Unresolvable targets (unknown kind) are surfaced with label "<kind>:<id>".
///
/// Caller owns the result; free via `DecisionDerivesFromRow.deinitMany`.
pub fn queryDecisionDerivesFrom(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    decision_id: i64,
) ![]DecisionDerivesFromRow {
    // Query the entity_links rows for this decision's derives-from edges.
    var stmt = d.prepare(
        \\select to_kind, to_id
        \\from entity_links
        \\where from_kind = 'decision'
        \\  and from_id = ?
        \\  and relationship = 'derives-from'
        \\order by to_kind asc, to_id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = decision_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(DecisionDerivesFromRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const to_kind = try stmt.columnTextAlloc(0, allocator);
                errdefer allocator.free(to_kind);
                const to_id = stmt.columnInt(1);

                // Resolve the title for this target entity.
                const title_opt = resolveEntityTitle(d, allocator, to_kind, to_id);
                const label = if (title_opt) |t|
                    std.fmt.allocPrint(allocator, "{s}:{d} — {s}", .{ to_kind, to_id, t }) catch blk: {
                        allocator.free(t);
                        break :blk std.fmt.allocPrint(allocator, "{s}:{d}", .{ to_kind, to_id }) catch to_kind;
                    }
                else
                    std.fmt.allocPrint(allocator, "{s}:{d}", .{ to_kind, to_id }) catch to_kind;
                errdefer allocator.free(label);

                // Free the resolved title if it was successfully embedded in label.
                if (title_opt) |t| allocator.free(t);

                try out.append(allocator, .{
                    .target_kind = to_kind,
                    .target_id = to_id,
                    .label = label,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Attempt to resolve an entity title for a given kind+id. Returns a heap-
/// allocated string or null when the kind is not recognized or the row is not
/// found. Caller owns the returned string when non-null.
fn resolveEntityTitle(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: []const u8,
    id: i64,
) ?[]const u8 {
    // prepare() requires a sentinel-terminated string. Use a tagged union of
    // comptime literals rather than a runtime []const u8.
    if (std.mem.eql(u8, kind, "artifact")) {
        var stmt = d.prepare("select title from artifacts where id = ?") catch return null;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = id }}) catch return null;
        switch (stmt.step() catch return null) {
            .done => return null,
            .row => return stmt.columnTextAlloc(0, allocator) catch null,
        }
    } else if (std.mem.eql(u8, kind, "plan")) {
        var stmt = d.prepare("select title from plans where id = ?") catch return null;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = id }}) catch return null;
        switch (stmt.step() catch return null) {
            .done => return null,
            .row => return stmt.columnTextAlloc(0, allocator) catch null,
        }
    } else if (std.mem.eql(u8, kind, "task")) {
        var stmt = d.prepare("select title from tasks where id = ?") catch return null;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = id }}) catch return null;
        switch (stmt.step() catch return null) {
            .done => return null,
            .row => return stmt.columnTextAlloc(0, allocator) catch null,
        }
    } else if (std.mem.eql(u8, kind, "decision")) {
        var stmt = d.prepare("select title from decisions where id = ?") catch return null;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = id }}) catch return null;
        switch (stmt.step() catch return null) {
            .done => return null,
            .row => return stmt.columnTextAlloc(0, allocator) catch null,
        }
    } else if (std.mem.eql(u8, kind, "question")) {
        var stmt = d.prepare("select title from questions where id = ?") catch return null;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = id }}) catch return null;
        switch (stmt.step() catch return null) {
            .done => return null,
            .row => return stmt.columnTextAlloc(0, allocator) catch null,
        }
    } else if (std.mem.eql(u8, kind, "test_scenario")) {
        var stmt = d.prepare("select title from test_scenarios where id = ?") catch return null;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = id }}) catch return null;
        switch (stmt.step() catch return null) {
            .done => return null,
            .row => return stmt.columnTextAlloc(0, allocator) catch null,
        }
    } else {
        return null;
    }
}

/// Query full Decision Log detail for a selected decision.
///
/// Combines the decision's body (markdown) with the derives-from
/// targets from entity_links (task 4022).
///
/// Returns null when the decision does not exist.
/// Caller owns the result; free via `DecisionLogDetail.deinit`.
pub fn queryDecisionLogDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    decision_id: i64,
) !?DecisionLogDetail {
    var stmt = d.prepare(
        "select title, body, status from decisions where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = decision_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return null,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            // decisions.body is NOT NULL per schema.
            const body_raw = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(body_raw);
            const status_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status_text);

            // Build the rendered body: prepend the status badge line.
            const body = try std.fmt.allocPrint(
                allocator,
                "**Status:** {s}\n\n{s}",
                .{ status_text, body_raw },
            );
            errdefer allocator.free(body);

            const derives_from = try queryDecisionDerivesFrom(d, allocator, decision_id);
            errdefer DecisionDerivesFromRow.deinitMany(derives_from, allocator);

            return .{
                .id = decision_id,
                .title = title,
                .body = body,
                .derives_from = derives_from,
            };
        },
    }
}

// =========================================================================
// View registry
// =========================================================================

// =========================================================================
// Open Questions view-model  (tasks 4023, 4024)
// =========================================================================

/// Status filter for the Open Questions view (task 4024).
///
/// Status values confirmed from migration 00003_work_items.up.sql:
///   check(status in ('open','answered','wontfix'))
pub const QuestionStatusFilter = enum {
    /// Show only open questions (default).
    open,
    /// Show only answered questions.
    answered,
    /// Show only wontfix questions.
    wontfix,
    /// Show all questions regardless of status.
    all,

    /// The next filter value in the cycle order: open → answered → wontfix → all → open.
    pub fn next(self: QuestionStatusFilter) QuestionStatusFilter {
        return switch (self) {
            .open => .answered,
            .answered => .wontfix,
            .wontfix => .all,
            .all => .open,
        };
    }

    /// SQL WHERE clause fragment for this filter. Returns null when filter=all.
    pub fn sqlWhere(self: QuestionStatusFilter) ?[]const u8 {
        return switch (self) {
            .open => "status = 'open'",
            .answered => "status = 'answered'",
            .wontfix => "status = 'wontfix'",
            .all => null,
        };
    }

    /// Short display label for the status bar.
    pub fn label(self: QuestionStatusFilter) []const u8 {
        return switch (self) {
            .open => "open",
            .answered => "answered",
            .wontfix => "wontfix",
            .all => "all",
        };
    }
};

/// One row in the Open Questions navigator list.
///
/// Schema reference: migrations/00003_work_items.up.sql
///   questions(id, scope_kind, scope_id, title, body, status,
///             answer_body, answered_at, created_at, updated_at)
///   status check: ('open','answered','wontfix')
pub const OpenQuestionsRow = struct {
    id: i64,
    /// Question title.
    title: []const u8,
    /// Status badge derived from the questions.status column.
    badge: StatusBadge,
    /// Status string for display ("open" / "answered" / "wontfix").
    status: []const u8,
    /// Pre-formatted display string: "[badge] status  title" — heap-allocated
    /// so grapheme pointers from printSegment remain valid after render returns.
    display_text: []const u8,

    pub fn deinit(self: OpenQuestionsRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.status);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []OpenQuestionsRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One linked entity for a question (from entity_links).
///
/// Questions can be linked to plans, artifacts, tasks, decisions, etc. via
/// entity_links (both from_kind='question' and to_kind='question' directions).
/// This struct surfaces the target entity so the operator can jump to it.
///
/// The 'addresses' relationship is the canonical question→plan/artifact link
/// (a question "addresses" a plan's concern). Both directions are surfaced.
pub const QuestionLinkedEntity = struct {
    /// Target entity kind string, e.g. "plan", "artifact".
    target_kind: []const u8,
    /// Target entity id.
    target_id: i64,
    /// Relationship kind, e.g. "addresses", "derives-from".
    relationship: []const u8,
    /// Human-readable label: "<kind>:<id> — <title>" or "<kind>:<id>".
    label: []const u8,

    pub fn deinit(self: QuestionLinkedEntity, allocator: std.mem.Allocator) void {
        allocator.free(self.target_kind);
        allocator.free(self.relationship);
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []QuestionLinkedEntity, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Detail pane data for a selected question in the Open Questions view.
///
/// Combines the question's body (markdown) with the list of linked entities
/// from entity_links and the answer_body when status='answered'.
///
/// All fields are caller-owned; free via `OpenQuestionsDetail.deinit`.
pub const OpenQuestionsDetail = struct {
    /// Question id.
    id: i64,
    /// Question title.
    title: []const u8,
    /// Rendered markdown body: "**Status:** {s}\n\n{body}\n\n**Answer:** {answer_body}".
    body: []const u8,
    /// Linked entities from entity_links (both directions). May be empty.
    linked: []QuestionLinkedEntity,

    pub fn deinit(self: OpenQuestionsDetail, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.body);
        QuestionLinkedEntity.deinitMany(self.linked, allocator);
    }
};

/// Query questions filtered by status, ordered newest-first (updated_at desc, id desc).
///
/// Status values confirmed from migration 00003_work_items.up.sql:
///   check(status in ('open','answered','wontfix'))
///
/// Caller owns the result; free via `OpenQuestionsRow.deinitMany`.
pub fn queryOpenQuestions(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: QuestionStatusFilter,
) ![]OpenQuestionsRow {
    // Build the query dynamically based on the filter. Using two compile-time
    // constants avoids string interpolation and keeps the query typed/safe.
    const sql_all =
        \\select id, title, status
        \\from questions
        \\order by updated_at desc, id desc
    ;
    const sql_open =
        \\select id, title, status
        \\from questions
        \\where status = 'open'
        \\order by updated_at desc, id desc
    ;
    const sql_answered =
        \\select id, title, status
        \\from questions
        \\where status = 'answered'
        \\order by updated_at desc, id desc
    ;
    const sql_wontfix =
        \\select id, title, status
        \\from questions
        \\where status = 'wontfix'
        \\order by updated_at desc, id desc
    ;

    const sql = switch (filter) {
        .all => sql_all,
        .open => sql_open,
        .answered => sql_answered,
        .wontfix => sql_wontfix,
    };

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return error.QueryFailed;

    var out: std.ArrayList(OpenQuestionsRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const status_text = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(status_text);

                const badge = questionStatusBadge(status_text);

                // Pre-format the navigator display string so that grapheme
                // pointers from printSegment remain valid after render returns.
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s}  {s}",
                    .{ badge.glyph(), status_text, title },
                );
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .badge = badge,
                    .status = status_text,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query linked entities for a question from entity_links.
///
/// Surfaces both directions:
///   (1) Outgoing: question→X (from_kind='question', from_id=question_id)
///   (2) Incoming: X→question (to_kind='question', to_id=question_id)
///
/// Per migration 00004_entity_links.up.sql:
///   from_kind/to_kind include 'question' (confirmed in the CHECK constraint)
///   valid relationships: ('derives-from','blocks','addresses','verifies','cites','supersedes','touches')
///
/// For each linked entity, the title is resolved via resolveEntityTitle.
/// Caller owns the result; free via `QuestionLinkedEntity.deinitMany`.
pub fn queryQuestionLinkedEntities(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    question_id: i64,
) ![]QuestionLinkedEntity {
    var out: std.ArrayList(QuestionLinkedEntity) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    // (1) Outgoing: question → X (question as from_kind).
    {
        var stmt = d.prepare(
            \\select to_kind, to_id, relationship
            \\from entity_links
            \\where from_kind = 'question' and from_id = ?
            \\order by relationship asc, to_kind asc, to_id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = question_id }}) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const to_kind = try stmt.columnTextAlloc(0, allocator);
                    errdefer allocator.free(to_kind);
                    const to_id = stmt.columnInt(1);
                    const rel = try stmt.columnTextAlloc(2, allocator);
                    errdefer allocator.free(rel);

                    const title_opt = resolveEntityTitle(d, allocator, to_kind, to_id);
                    const label = if (title_opt) |t|
                        std.fmt.allocPrint(allocator, "{s}:{d} — {s}", .{ to_kind, to_id, t }) catch blk: {
                            allocator.free(t);
                            break :blk std.fmt.allocPrint(allocator, "{s}:{d}", .{ to_kind, to_id }) catch to_kind;
                        }
                    else
                        std.fmt.allocPrint(allocator, "{s}:{d}", .{ to_kind, to_id }) catch to_kind;
                    errdefer allocator.free(label);

                    if (title_opt) |t| allocator.free(t);

                    try out.append(allocator, .{
                        .target_kind = to_kind,
                        .target_id = to_id,
                        .relationship = rel,
                        .label = label,
                    });
                },
            }
        }
    }

    // (2) Incoming: X → question (question as to_kind).
    {
        var stmt = d.prepare(
            \\select from_kind, from_id, relationship
            \\from entity_links
            \\where to_kind = 'question' and to_id = ?
            \\order by relationship asc, from_kind asc, from_id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = question_id }}) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const from_kind = try stmt.columnTextAlloc(0, allocator);
                    errdefer allocator.free(from_kind);
                    const from_id = stmt.columnInt(1);
                    const rel = try stmt.columnTextAlloc(2, allocator);
                    errdefer allocator.free(rel);

                    const title_opt = resolveEntityTitle(d, allocator, from_kind, from_id);
                    const label = if (title_opt) |t|
                        std.fmt.allocPrint(allocator, "{s}:{d} — {s}", .{ from_kind, from_id, t }) catch blk: {
                            allocator.free(t);
                            break :blk std.fmt.allocPrint(allocator, "{s}:{d}", .{ from_kind, from_id }) catch from_kind;
                        }
                    else
                        std.fmt.allocPrint(allocator, "{s}:{d}", .{ from_kind, from_id }) catch from_kind;
                    errdefer allocator.free(label);

                    if (title_opt) |t| allocator.free(t);

                    try out.append(allocator, .{
                        .target_kind = from_kind,
                        .target_id = from_id,
                        .relationship = rel,
                        .label = label,
                    });
                },
            }
        }
    }

    return try out.toOwnedSlice(allocator);
}

/// Query full Open Questions detail for a selected question.
///
/// Combines the question's body (markdown) with the linked entities from
/// entity_links and the answer_body when status='answered'.
///
/// Returns null when the question does not exist.
/// Caller owns the result; free via `OpenQuestionsDetail.deinit`.
pub fn queryOpenQuestionsDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    question_id: i64,
) !?OpenQuestionsDetail {
    var stmt = d.prepare(
        "select title, body, status, answer_body from questions where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = question_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return null,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const status_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status_text);
            const answer_opt = try stmt.columnTextOpt(3, allocator);
            defer if (answer_opt) |s| allocator.free(s);

            // Build the markdown body including status, body text, and answer.
            var body_buf: std.ArrayList(u8) = .empty;
            errdefer body_buf.deinit(allocator);

            const status_line = try std.fmt.allocPrint(
                allocator,
                "**Status:** {s}",
                .{status_text},
            );
            defer allocator.free(status_line);
            try body_buf.appendSlice(allocator, status_line);

            if (body_opt) |b| {
                const part = try std.fmt.allocPrint(allocator, "\n\n{s}", .{b});
                defer allocator.free(part);
                try body_buf.appendSlice(allocator, part);
            }

            if (answer_opt) |ans| {
                const part = try std.fmt.allocPrint(allocator, "\n\n**Answer:** {s}", .{ans});
                defer allocator.free(part);
                try body_buf.appendSlice(allocator, part);
            }

            const body = try body_buf.toOwnedSlice(allocator);
            errdefer allocator.free(body);

            const linked = try queryQuestionLinkedEntities(d, allocator, question_id);
            errdefer QuestionLinkedEntity.deinitMany(linked, allocator);

            return .{
                .id = question_id,
                .title = title,
                .body = body,
                .linked = linked,
            };
        },
    }
}

/// Registered view IDs for the view-switcher.
pub const ViewId = enum {
    agent_monitor,
    scope_explorer,
    task_board,
    decision_log,
    open_questions,
    coverage_view,
    entity_link_graph,
    external_ops_plane,
    /// M11: Sessions & Handoff view (sessions, session_entries, handoffs,
    /// context_snapshots, session_commits).
    sessions_handoff,
    /// M12: Audit Log view (audit_log).
    audit_log,
};

// =========================================================================
// Test Scenario & Coverage view-model  (tasks 4025, 4026)
// =========================================================================

/// One task that a scenario verifies (from entity_links).
///
/// Direction confirmed from engine/planning/test_spec_status.zig and
/// engine/ingestor/apply.zig:
///   from_kind='test_scenario', from_id=scenario_id,
///   to_kind='task', to_id=task_id, relationship='verifies'
///
/// The scenario "verifies" the task (scenario is the source/from side).
pub const ScenarioVerifiesRow = struct {
    /// task id that the scenario verifies.
    task_id: i64,
    /// Human-readable label: "task:<id> — <title>".
    label: []const u8,

    pub fn deinit(self: ScenarioVerifiesRow, allocator: std.mem.Allocator) void {
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []ScenarioVerifiesRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One row in the Coverage view navigator list: a test scenario with its
/// verifies→task links.
///
/// Schema reference: migrations/00003_work_items.up.sql
///   test_scenarios(id, scope_kind, scope_id, title, body, status,
///                  related_artifact_id, last_run_at, last_outcome,
///                  created_at, updated_at)
///   status check: ('draft','ready','verified','failing','retired')
pub const ScenarioCoverageRow = struct {
    id: i64,
    /// Scenario title.
    title: []const u8,
    /// Status badge.
    badge: StatusBadge,
    /// Status string for display.
    status: []const u8,
    /// Pre-formatted display string: "[badge] status  title" — heap-allocated
    /// so grapheme pointers from printSegment remain valid after render returns.
    display_text: []const u8,
    /// Tasks this scenario verifies (may be empty = orphan scenario).
    verifies: []ScenarioVerifiesRow,

    pub fn deinit(self: ScenarioCoverageRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.status);
        allocator.free(self.display_text);
        ScenarioVerifiesRow.deinitMany(self.verifies, allocator);
    }

    pub fn deinitMany(rows: []ScenarioCoverageRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Coverage gap data for the gap surface (task 4026).
///
/// Two gap classes:
///   (i)  Orphan scenarios: test_scenarios with NO verifies edge from them.
///   (ii) Uncovered tasks: tasks with NO scenario verifying them.
///
/// All fields are caller-owned; free via `CoverageGap.deinit`.
pub const CoverageGap = struct {
    /// Scenario rows with zero verifies edges (orphan scenarios).
    orphan_scenarios: []OrphanScenarioRow,
    /// Task rows not verified by any scenario (uncovered tasks).
    uncovered_tasks: []UncoveredTaskRow,

    pub fn deinit(self: CoverageGap, allocator: std.mem.Allocator) void {
        OrphanScenarioRow.deinitMany(self.orphan_scenarios, allocator);
        UncoveredTaskRow.deinitMany(self.uncovered_tasks, allocator);
    }
};

/// One orphan scenario: a test_scenario with no outgoing verifies edge.
pub const OrphanScenarioRow = struct {
    id: i64,
    /// Scenario title.
    title: []const u8,
    /// Human-readable label: "scenario:<id> — <title>".
    label: []const u8,

    pub fn deinit(self: OrphanScenarioRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []OrphanScenarioRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One uncovered task: a task with no test_scenario verifying it.
pub const UncoveredTaskRow = struct {
    id: i64,
    /// Task title.
    title: []const u8,
    /// Task status.
    status: []const u8,
    /// Human-readable label: "task:<id> — <title>".
    label: []const u8,

    pub fn deinit(self: UncoveredTaskRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.status);
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []UncoveredTaskRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Query test scenarios with their verifies→task links (task 4025).
///
/// Returns all test_scenarios (within the scope filter) ordered by
/// created_at desc, id desc. For each scenario, the verifies slice
/// lists the tasks it verifies via entity_links.
///
/// verifies direction (confirmed from engine/planning/test_spec_status.zig
/// and engine/ingestor/apply.zig):
///   from_kind='test_scenario', from_id=scenario_id,
///   to_kind='task', to_id=task_id, relationship='verifies'
///
/// Caller owns the result; free via `ScenarioCoverageRow.deinitMany`.
pub fn queryScenarioCoverage(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: ScopeFilter,
) ![]ScenarioCoverageRow {
    const sql_all =
        \\select id, title, status
        \\from test_scenarios
        \\order by created_at desc, id desc
    ;
    const sql_repo =
        \\select id, title, status
        \\from test_scenarios
        \\where (scope_kind = 'repo' and scope_id = ?) or scope_kind = 'global'
        \\order by created_at desc, id desc
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

    var out: std.ArrayList(ScenarioCoverageRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const status_text = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(status_text);

                const badge = scenarioStatusBadge(status_text);

                // Pre-format display string so grapheme pointers remain valid.
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s}  {s}",
                    .{ badge.glyph(), status_text, title },
                );
                errdefer allocator.free(display_text);

                // Query verifies→task links for this scenario.
                const verifies = try queryScenarioVerifies(d, allocator, id);
                errdefer ScenarioVerifiesRow.deinitMany(verifies, allocator);

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .badge = badge,
                    .status = status_text,
                    .display_text = display_text,
                    .verifies = verifies,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query the tasks that a scenario verifies (outgoing verifies edges).
///
/// verifies direction:
///   from_kind='test_scenario', from_id=scenario_id,
///   to_kind='task', to_id=task_id, relationship='verifies'
///
/// Caller owns the result; free via `ScenarioVerifiesRow.deinitMany`.
fn queryScenarioVerifies(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    scenario_id: i64,
) ![]ScenarioVerifiesRow {
    var stmt = d.prepare(
        \\select el.to_id, t.title
        \\from entity_links el
        \\left join tasks t on t.id = el.to_id
        \\where el.from_kind = 'test_scenario'
        \\  and el.from_id = ?
        \\  and el.to_kind = 'task'
        \\  and el.relationship = 'verifies'
        \\order by el.to_id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = scenario_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(ScenarioVerifiesRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const task_id = stmt.columnInt(0);
                const task_title_opt = try stmt.columnTextOpt(1, allocator);
                defer if (task_title_opt) |t| allocator.free(t);

                // Build label: "task:<id> — <title>" or "task:<id>".
                const label = if (task_title_opt) |t|
                    try std.fmt.allocPrint(allocator, "task:{d} — {s}", .{ task_id, t })
                else
                    try std.fmt.allocPrint(allocator, "task:{d}", .{task_id});
                errdefer allocator.free(label);

                try out.append(allocator, .{
                    .task_id = task_id,
                    .label = label,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query coverage gaps within the given scope filter (task 4026).
///
/// Gap class (i): Orphan scenarios — test_scenarios with NO outgoing
/// verifies edge (no tasks they verify).
///
/// Gap class (ii): Uncovered tasks — tasks in non-cancelled status with
/// NO test_scenario verifying them (no incoming verifies edge).
///
/// Caller owns the result; free via `CoverageGap.deinit`.
pub fn queryCoverageGap(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: ScopeFilter,
) !CoverageGap {
    // ---- Orphan scenarios (gap class i) ----------------------------------
    // Scenarios with no outgoing 'verifies' edge from them.
    const orphan_sql_all =
        \\select ts.id, ts.title
        \\from test_scenarios ts
        \\where not exists (
        \\  select 1 from entity_links el
        \\  where el.from_kind = 'test_scenario'
        \\    and el.from_id = ts.id
        \\    and el.to_kind = 'task'
        \\    and el.relationship = 'verifies'
        \\)
        \\order by ts.created_at desc, ts.id desc
    ;
    const orphan_sql_repo =
        \\select ts.id, ts.title
        \\from test_scenarios ts
        \\where ((ts.scope_kind = 'repo' and ts.scope_id = ?) or ts.scope_kind = 'global')
        \\  and not exists (
        \\    select 1 from entity_links el
        \\    where el.from_kind = 'test_scenario'
        \\      and el.from_id = ts.id
        \\      and el.to_kind = 'task'
        \\      and el.relationship = 'verifies'
        \\  )
        \\order by ts.created_at desc, ts.id desc
    ;

    var orphan_stmt = switch (filter) {
        .all => blk: {
            var s = d.prepare(orphan_sql_all) catch return error.QueryFailed;
            s.bind(&.{}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
        .repo => blk: {
            var s = d.prepare(orphan_sql_repo) catch return error.QueryFailed;
            s.bind(&.{.{ .int = filter.repo }}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
    };
    defer orphan_stmt.finalize();

    var orphans: std.ArrayList(OrphanScenarioRow) = .empty;
    errdefer {
        for (orphans.items) |r| r.deinit(allocator);
        orphans.deinit(allocator);
    }

    while (true) {
        switch (orphan_stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = orphan_stmt.columnInt(0);
                const title = try orphan_stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);

                // Build label heap-owned; keep dup/free symmetric.
                const label = try std.fmt.allocPrint(
                    allocator,
                    "scenario:{d} — {s}",
                    .{ id, title },
                );
                errdefer allocator.free(label);

                try orphans.append(allocator, .{
                    .id = id,
                    .title = title,
                    .label = label,
                });
            },
        }
    }

    // ---- Uncovered tasks (gap class ii) ----------------------------------
    // Tasks (non-cancelled) with no incoming 'verifies' edge pointing at them.
    const uncov_sql_all =
        \\select t.id, t.title, t.status
        \\from tasks t
        \\where t.status not in ('cancelled')
        \\  and not exists (
        \\    select 1 from entity_links el
        \\    where el.from_kind = 'test_scenario'
        \\      and el.to_kind = 'task'
        \\      and el.to_id = t.id
        \\      and el.relationship = 'verifies'
        \\  )
        \\order by t.priority asc, t.id asc
    ;
    const uncov_sql_repo =
        \\select t.id, t.title, t.status
        \\from tasks t
        \\where t.status not in ('cancelled')
        \\  and ((t.scope_kind = 'repo' and t.scope_id = ?) or t.scope_kind = 'global')
        \\  and not exists (
        \\    select 1 from entity_links el
        \\    where el.from_kind = 'test_scenario'
        \\      and el.to_kind = 'task'
        \\      and el.to_id = t.id
        \\      and el.relationship = 'verifies'
        \\  )
        \\order by t.priority asc, t.id asc
    ;

    var uncov_stmt = switch (filter) {
        .all => blk: {
            var s = d.prepare(uncov_sql_all) catch return error.QueryFailed;
            s.bind(&.{}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
        .repo => blk: {
            var s = d.prepare(uncov_sql_repo) catch return error.QueryFailed;
            s.bind(&.{.{ .int = filter.repo }}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
    };
    defer uncov_stmt.finalize();

    var uncovered: std.ArrayList(UncoveredTaskRow) = .empty;
    errdefer {
        for (uncovered.items) |r| r.deinit(allocator);
        uncovered.deinit(allocator);
    }

    while (true) {
        switch (uncov_stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = uncov_stmt.columnInt(0);
                const title = try uncov_stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const status_text = try uncov_stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(status_text);

                // Build label heap-owned; keep dup/free symmetric.
                const label = try std.fmt.allocPrint(
                    allocator,
                    "task:{d} — {s}",
                    .{ id, title },
                );
                errdefer allocator.free(label);

                try uncovered.append(allocator, .{
                    .id = id,
                    .title = title,
                    .status = status_text,
                    .label = label,
                });
            },
        }
    }

    return .{
        .orphan_scenarios = try orphans.toOwnedSlice(allocator),
        .uncovered_tasks = try uncovered.toOwnedSlice(allocator),
    };
}

// =========================================================================
// Entity-Link Graph view-model  (tasks 4027, 4028)
// =========================================================================

/// The relationship kind carried by an entity_links row.
///
/// These are the valid values from the CHECK constraint in
/// migrations/00004_entity_links.up.sql:
///   ('derives-from','blocks','addresses','verifies','cites','supersedes','touches')
pub const LinkRelationship = enum {
    derives_from,
    blocks,
    addresses,
    verifies,
    cites,
    supersedes,
    touches,

    pub fn fromText(s: []const u8) ?LinkRelationship {
        if (std.mem.eql(u8, s, "derives-from")) return .derives_from;
        if (std.mem.eql(u8, s, "blocks")) return .blocks;
        if (std.mem.eql(u8, s, "addresses")) return .addresses;
        if (std.mem.eql(u8, s, "verifies")) return .verifies;
        if (std.mem.eql(u8, s, "cites")) return .cites;
        if (std.mem.eql(u8, s, "supersedes")) return .supersedes;
        if (std.mem.eql(u8, s, "touches")) return .touches;
        return null;
    }

    pub fn toText(self: LinkRelationship) []const u8 {
        return switch (self) {
            .derives_from => "derives-from",
            .blocks => "blocks",
            .addresses => "addresses",
            .verifies => "verifies",
            .cites => "cites",
            .supersedes => "supersedes",
            .touches => "touches",
        };
    }
};

/// Direction of a link relative to the focus entity.
pub const LinkEdgeDirection = enum {
    /// Focus entity is the source (from_kind/from_id = focus); the link
    /// points outward to another entity.
    outbound,
    /// Focus entity is the target (to_kind/to_id = focus); another entity
    /// points inward to the focus.
    inbound,
};

/// One related-entity row for the Entity-Link Graph view.
///
/// Each row represents one entity_links edge (task 4027). The navigator
/// groups rows by relationship kind + direction.
///
/// All string fields are heap-owned by the EntityLinkGraphData that
/// contains this row; freed by EntityLinkGraphData.deinit.
pub const EntityLinkRow = struct {
    /// The entity_links.id for this edge.
    link_id: i64,
    /// Relationship kind (e.g. .derives_from, .blocks).
    relationship: LinkRelationship,
    /// Direction relative to the focus entity.
    direction: LinkEdgeDirection,
    /// Kind string of the *other* (non-focus) entity, e.g. "task".
    other_kind: []const u8,
    /// ID of the other entity.
    other_id: i64,
    /// Pre-formatted readable label:
    ///   "[direction] relationship  other_kind:other_id — title"
    /// Heap-owned; stays valid for the duration of the EntityLinkGraphData
    /// that owns it (required for render-level test stability).
    display_text: []const u8,

    pub fn deinit(self: EntityLinkRow, allocator: std.mem.Allocator) void {
        allocator.free(self.other_kind);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []EntityLinkRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Focus entity for the Entity-Link Graph view.
///
/// Specifies the entity whose neighborhood is currently displayed.
/// A null focus means "no entity selected" — the view shows an empty state.
pub const EntityFocus = struct {
    /// Entity kind string, e.g. "task", "plan", "decision".
    kind: []const u8,
    /// Entity id.
    id: i64,
    /// Resolved title (may be empty when resolution fails).
    title: []const u8,
    /// Human-readable header: "kind:id — title".
    header: []const u8,

    pub fn deinit(self: EntityFocus, allocator: std.mem.Allocator) void {
        allocator.free(self.kind);
        allocator.free(self.title);
        allocator.free(self.header);
    }
};

/// All data for the Entity-Link Graph view for the current focus entity.
///
/// Caller owns the result; free via `EntityLinkGraphData.deinit`.
pub const EntityLinkGraphData = struct {
    /// Focus entity. Null when nothing is selected.
    focus: ?EntityFocus,
    /// All related-entity rows (both inbound + outbound, all relationship kinds).
    /// Ordered by: relationship text asc, direction (outbound first) asc, other_id asc.
    rows: []EntityLinkRow,
    /// Pre-formatted "Links: N" label — heap-owned so grapheme pointers from
    /// printSegment remain valid after the render function returns. This avoids
    /// the stack-buffer-dangling-pointer hazard in render-level tests.
    links_count_label: []const u8,

    pub fn deinit(self: EntityLinkGraphData, allocator: std.mem.Allocator) void {
        if (self.focus) |f| f.deinit(allocator);
        EntityLinkRow.deinitMany(self.rows, allocator);
        allocator.free(self.links_count_label);
    }
};

/// Query the Entity-Link Graph data for a given focus entity (task 4027).
///
/// Returns all entity_links rows where either:
///   (a) from_kind=kind AND from_id=id (outbound: focus → other)
///   (b) to_kind=kind   AND to_id=id   (inbound: other → focus)
///
/// For each row, resolves the other entity's title via resolveEntityTitle.
/// Builds a human-readable display_text for each row:
///   "[out] derives-from  artifact:5 — FoundingSpec"
///   "[in]  blocks        task:12 — Implement parser"
///
/// All strings in the result are heap-owned. Caller frees via
/// EntityLinkGraphData.deinit.
pub fn queryEntityLinkGraph(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    focus_kind: []const u8,
    focus_id: i64,
) !EntityLinkGraphData {
    var rows: std.ArrayList(EntityLinkRow) = .empty;
    errdefer {
        for (rows.items) |r| r.deinit(allocator);
        rows.deinit(allocator);
    }

    // ---- Outbound edges: focus is the from side ---------------------------
    {
        var stmt = d.prepare(
            \\select id, to_kind, to_id, relationship
            \\from entity_links
            \\where from_kind = ? and from_id = ?
            \\order by relationship asc, to_kind asc, to_id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        var params = [_]db.sqlite.Param{
            .{ .text = focus_kind },
            .{ .int = focus_id },
        };
        stmt.bind(&params) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const link_id = stmt.columnInt(0);
                    const to_kind = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(to_kind);
                    const to_id = stmt.columnInt(2);
                    const rel_str = try stmt.columnTextAlloc(3, allocator);
                    defer allocator.free(rel_str);

                    const rel = LinkRelationship.fromText(rel_str) orelse {
                        // Unknown relationship — skip rather than panic.
                        allocator.free(to_kind);
                        continue;
                    };

                    // Resolve other entity title.
                    const title_opt = resolveEntityTitle(d, allocator, to_kind, to_id);
                    defer if (title_opt) |t| allocator.free(t);

                    // Build display_text.  Memory contract: display_text is heap-owned
                    // and stable; to_kind is also kept as other_kind.
                    const display_text = if (title_opt) |t|
                        try std.fmt.allocPrint(
                            allocator,
                            "[out] {s}  {s}:{d} — {s}",
                            .{ rel.toText(), to_kind, to_id, t },
                        )
                    else
                        try std.fmt.allocPrint(
                            allocator,
                            "[out] {s}  {s}:{d}",
                            .{ rel.toText(), to_kind, to_id },
                        );
                    errdefer allocator.free(display_text);

                    try rows.append(allocator, .{
                        .link_id = link_id,
                        .relationship = rel,
                        .direction = .outbound,
                        .other_kind = to_kind,
                        .other_id = to_id,
                        .display_text = display_text,
                    });
                },
            }
        }
    }

    // ---- Inbound edges: focus is the to side ------------------------------
    {
        var stmt = d.prepare(
            \\select id, from_kind, from_id, relationship
            \\from entity_links
            \\where to_kind = ? and to_id = ?
            \\order by relationship asc, from_kind asc, from_id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        var params = [_]db.sqlite.Param{
            .{ .text = focus_kind },
            .{ .int = focus_id },
        };
        stmt.bind(&params) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const link_id = stmt.columnInt(0);
                    const from_kind = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(from_kind);
                    const from_id = stmt.columnInt(2);
                    const rel_str = try stmt.columnTextAlloc(3, allocator);
                    defer allocator.free(rel_str);

                    const rel = LinkRelationship.fromText(rel_str) orelse {
                        allocator.free(from_kind);
                        continue;
                    };

                    const title_opt = resolveEntityTitle(d, allocator, from_kind, from_id);
                    defer if (title_opt) |t| allocator.free(t);

                    const display_text = if (title_opt) |t|
                        try std.fmt.allocPrint(
                            allocator,
                            "[in]  {s}  {s}:{d} — {s}",
                            .{ rel.toText(), from_kind, from_id, t },
                        )
                    else
                        try std.fmt.allocPrint(
                            allocator,
                            "[in]  {s}  {s}:{d}",
                            .{ rel.toText(), from_kind, from_id },
                        );
                    errdefer allocator.free(display_text);

                    try rows.append(allocator, .{
                        .link_id = link_id,
                        .relationship = rel,
                        .direction = .inbound,
                        .other_kind = from_kind,
                        .other_id = from_id,
                        .display_text = display_text,
                    });
                },
            }
        }
    }

    // Sort: relationship text asc, direction (outbound < inbound) asc, other_id asc.
    // This groups all outbound edges for a relationship before inbound edges.
    const sorted_rows = try rows.toOwnedSlice(allocator);
    std.mem.sort(EntityLinkRow, sorted_rows, {}, struct {
        fn lt(_: void, a: EntityLinkRow, b: EntityLinkRow) bool {
            const ra = a.relationship.toText();
            const rb = b.relationship.toText();
            const rel_cmp = std.mem.order(u8, ra, rb);
            if (rel_cmp != .eq) return rel_cmp == .lt;
            // Same relationship: outbound < inbound.
            const da: u8 = if (a.direction == .outbound) 0 else 1;
            const db_dir: u8 = if (b.direction == .outbound) 0 else 1;
            if (da != db_dir) return da < db_dir;
            // Same direction: sort by other_id ascending.
            return a.other_id < b.other_id;
        }
    }.lt);

    // Build the focus entity descriptor.
    const kind_owned = try allocator.dupe(u8, focus_kind);
    errdefer allocator.free(kind_owned);

    const title_opt = resolveEntityTitle(d, allocator, focus_kind, focus_id);
    // MEMORY GUARD (brief rule (c)): on OOM in resolveEntityTitle the result is null;
    // we use an owned empty string — never alias focus_kind into the title field.
    const title_owned = title_opt orelse try allocator.dupe(u8, "");
    errdefer allocator.free(title_owned);

    const header = if (title_owned.len > 0)
        try std.fmt.allocPrint(allocator, "{s}:{d} — {s}", .{ focus_kind, focus_id, title_owned })
    else
        try std.fmt.allocPrint(allocator, "{s}:{d}", .{ focus_kind, focus_id });
    errdefer allocator.free(header);

    // Pre-format the "Links: N" label as a heap string so render functions can
    // pass it directly to printSegment without a stack-buffer dangling-pointer hazard.
    const links_count_label = try std.fmt.allocPrint(allocator, "Links: {d}", .{sorted_rows.len});
    errdefer allocator.free(links_count_label);

    return .{
        .focus = .{
            .kind = kind_owned,
            .id = focus_id,
            .title = title_owned,
            .header = header,
        },
        .rows = sorted_rows,
        .links_count_label = links_count_label,
    };
}

/// Query entity-link graph data for an entity chosen from the first available
/// entity in the DB (task, plan, decision — whichever has the most links first).
/// Used as the default-first-entity heuristic for the view's initial load.
///
/// Returns null focus data (empty rows) when the DB has no entities.
pub fn queryEntityLinkGraphDefault(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) !EntityLinkGraphData {
    // Pick the entity that appears most often in entity_links as a starting point.
    // Fallback ordering: check entity_links first (from side), then tasks, then plans.
    var stmt = d.prepare(
        \\select from_kind, from_id from entity_links
        \\group by from_kind, from_id
        \\order by count(*) desc limit 1
    ) catch return makeEmptyEntityLinkGraphData(allocator);
    defer stmt.finalize();
    stmt.bind(&.{}) catch return makeEmptyEntityLinkGraphData(allocator);

    switch (stmt.step() catch return makeEmptyEntityLinkGraphData(allocator)) {
        .done => {
            // No entity_links at all. Try a task as default.
            // NOTE: stmt will be finalized by the defer above; do NOT call
            // stmt.finalize() here or it would double-finalize.
            return queryEntityLinkGraphFirstTask(d, allocator);
        },
        .row => {
            const kind_str = stmt.columnTextAlloc(0, allocator) catch
                return makeEmptyEntityLinkGraphData(allocator);
            defer allocator.free(kind_str);
            const id = stmt.columnInt(1);
            return queryEntityLinkGraph(d, allocator, kind_str, id);
        },
    }
}

fn queryEntityLinkGraphFirstTask(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) !EntityLinkGraphData {
    var stmt = d.prepare("select id from tasks order by id asc limit 1") catch
        return makeEmptyEntityLinkGraphData(allocator);
    defer stmt.finalize();
    stmt.bind(&.{}) catch return makeEmptyEntityLinkGraphData(allocator);
    switch (stmt.step() catch return makeEmptyEntityLinkGraphData(allocator)) {
        .done => return makeEmptyEntityLinkGraphData(allocator),
        .row => {
            const tid = stmt.columnInt(0);
            return queryEntityLinkGraph(d, allocator, "task", tid);
        },
    }
}

/// Build an empty EntityLinkGraphData with heap-allocated (zero-length) rows
/// slice so that deinit can safely call allocator.free(rows).
fn makeEmptyEntityLinkGraphData(allocator: std.mem.Allocator) !EntityLinkGraphData {
    // Allocate a zero-length slice so deinit's allocator.free(rows) is safe.
    const empty_rows = try allocator.alloc(EntityLinkRow, 0);
    const label = try allocator.dupe(u8, "Links: 0");
    return .{
        .focus = null,
        .rows = empty_rows,
        .links_count_label = label,
    };
}

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

// =========================================================================
// External / Ops Plane view-model  (tasks 4029, 4030, 4031)
// =========================================================================
//
// Schema reference: migrations/00006_external.up.sql
//
//   external_systems (id, kind, slug, base_url, default_project,
//                     auth_method, auth_ref, created_at, updated_at)
//     kind CHECK: 'jira','github-issues','gitlab-issues','linear'
//
//   external_links (id, entity_kind, entity_id, system_id, external_id,
//                   external_url, link_role, sync_direction,
//                   last_synced_at, last_sync_status, config_json,
//                   created_at)
//     last_sync_status CHECK: 'ok','conflict','error','never'
//     link_role CHECK: 'mirror','parent','child','reference'
//
//   sync_events (id, link_id, scope, direction, outcome, fields_changed,
//                detail, context_json, at)
//     outcome CHECK: 'ok','conflict','error','noop','resolved-fs',
//                    'resolved-db','partial','success','failure',
//                    'strategy-abandoned','counterpart-missing'
//     UNRESOLVED CONFLICTS: outcome = 'conflict' (no subsequent
//       'resolved-fs' or 'resolved-db' for the same link_id)
//
// Conflict resolution: A sync_event with outcome='conflict' is
// UNRESOLVED when there is no later sync_event on the same link_id
// with outcome in ('resolved-fs','resolved-db').  That is:
//   select se.* from sync_events se
//   where se.outcome = 'conflict'
//     and not exists (
//       select 1 from sync_events r
//       where r.link_id = se.link_id
//         and r.outcome in ('resolved-fs','resolved-db')
//         and r.at > se.at
//     )
// When link_id is null (workbench conflicts) the conflict is always
// surfaced because there is no link_id to correlate a resolution against.

/// One registered external system row (task 4029).
///
/// Derived from external_systems.
pub const ExtSystemRow = struct {
    id: i64,
    /// System kind: 'jira', 'github-issues', 'gitlab-issues', 'linear'.
    kind: []const u8,
    /// Human-readable slug (unique identifier).
    slug: []const u8,
    /// Optional base URL.
    base_url: ?[]const u8,
    /// Optional default project key/org-repo.
    default_project: ?[]const u8,
    /// Count of external_links rows referencing this system.
    link_count: i64,
    /// Count of links with last_sync_status = 'conflict'.
    conflict_count: i64,
    /// Count of links with last_sync_status = 'error'.
    error_count: i64,
    /// Pre-formatted display text: "[kind] slug (N links)" — heap-allocated
    /// so grapheme pointers remain valid after render.
    display_text: []const u8,

    pub fn deinit(self: ExtSystemRow, allocator: std.mem.Allocator) void {
        allocator.free(self.kind);
        allocator.free(self.slug);
        if (self.base_url) |s| allocator.free(s);
        if (self.default_project) |s| allocator.free(s);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []ExtSystemRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One external_links row with resolved local-entity title (task 4029).
pub const ExtLinkRow = struct {
    id: i64,
    /// system_id for grouping.
    system_id: i64,
    /// Entity kind (plan, task, etc.).
    entity_kind: []const u8,
    /// Entity id.
    entity_id: i64,
    /// Resolved local-entity title (best-effort; may be "(unknown)" on OOM).
    entity_title: []const u8,
    /// External ticket/issue id.
    external_id: []const u8,
    /// Optional external URL.
    external_url: ?[]const u8,
    /// Link role: 'mirror','parent','child','reference'.
    link_role: []const u8,
    /// Sync direction: 'read-only','write-back','two-way'.
    sync_direction: []const u8,
    /// Last sync timestamp (ISO-8601) or null if never synced.
    last_synced_at: ?[]const u8,
    /// Mapping/sync status: 'ok','conflict','error','never'.
    last_sync_status: []const u8,
    /// Pre-formatted display text: "entity_kind:id — status" — heap-allocated.
    display_text: []const u8,

    pub fn deinit(self: ExtLinkRow, allocator: std.mem.Allocator) void {
        allocator.free(self.entity_kind);
        allocator.free(self.entity_title);
        allocator.free(self.external_id);
        if (self.external_url) |s| allocator.free(s);
        allocator.free(self.link_role);
        allocator.free(self.sync_direction);
        if (self.last_synced_at) |s| allocator.free(s);
        allocator.free(self.last_sync_status);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []ExtLinkRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Latest sync event for a given external system (task 4030).
///
/// Surfaces the most recent sync event (by `at`) for any link belonging to
/// the system — giving a per-system "last-sync" summary row.
pub const ExtSystemSyncStatus = struct {
    system_id: i64,
    /// ISO-8601 timestamp of the most recent sync event.
    last_sync_at: []const u8,
    /// Outcome of the most recent sync event.
    last_outcome: []const u8,
    /// Direction of the most recent sync event: 'pull' or 'push'.
    last_direction: []const u8,

    pub fn deinit(self: ExtSystemSyncStatus, allocator: std.mem.Allocator) void {
        allocator.free(self.last_sync_at);
        allocator.free(self.last_outcome);
        allocator.free(self.last_direction);
    }
};

/// One unresolved sync conflict row (task 4031).
///
/// A sync_event with outcome='conflict' is UNRESOLVED when no later event
/// on the same link_id has outcome in ('resolved-fs','resolved-db').
/// When link_id is null the conflict is always unresolved.
pub const UnresolvedConflictRow = struct {
    id: i64,
    /// Nullable link_id.
    link_id: ?i64,
    /// Scope: 'external' or 'workbench'.
    scope: []const u8,
    /// Direction: 'pull' or 'push'.
    direction: []const u8,
    /// Optional detail text.
    detail: ?[]const u8,
    /// Conflict timestamp (ISO-8601).
    at: []const u8,
    /// Pre-formatted display text: "scope:direction [at]" — heap-allocated.
    display_text: []const u8,

    pub fn deinit(self: UnresolvedConflictRow, allocator: std.mem.Allocator) void {
        allocator.free(self.scope);
        allocator.free(self.direction);
        if (self.detail) |s| allocator.free(s);
        allocator.free(self.at);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []UnresolvedConflictRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Full snapshot for the External/Ops Plane view (tasks 4029, 4030, 4031).
pub const ExtOpsSnapshot = struct {
    /// All registered external systems (task 4029).
    systems: []ExtSystemRow,
    /// All external links across all systems (task 4029).
    links: []ExtLinkRow,
    /// Per-system latest sync status (task 4030).  May have fewer entries
    /// than systems when a system has no sync events yet.
    system_sync: []ExtSystemSyncStatus,
    /// Unresolved sync conflicts (task 4031).
    conflicts: []UnresolvedConflictRow,

    pub fn deinit(self: ExtOpsSnapshot, allocator: std.mem.Allocator) void {
        ExtSystemRow.deinitMany(self.systems, allocator);
        ExtLinkRow.deinitMany(self.links, allocator);
        for (self.system_sync) |ss| ss.deinit(allocator);
        allocator.free(self.system_sync);
        UnresolvedConflictRow.deinitMany(self.conflicts, allocator);
    }
};

/// Query all registered external systems, enriched with per-system link
/// counts and conflict/error tallies (task 4029).
pub fn queryExtSystems(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]ExtSystemRow {
    var stmt = d.prepare(
        \\select
        \\  es.id, es.kind, es.slug,
        \\  es.base_url, es.default_project,
        \\  (select count(*) from external_links el where el.system_id = es.id) as link_count,
        \\  (select count(*) from external_links el where el.system_id = es.id and el.last_sync_status = 'conflict') as conflict_count,
        \\  (select count(*) from external_links el where el.system_id = es.id and el.last_sync_status = 'error') as error_count
        \\from external_systems es
        \\order by es.id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(ExtSystemRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const kind = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(kind);
                const slug = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(slug);
                const base_url = try stmt.columnTextOpt(3, allocator);
                errdefer if (base_url) |s| allocator.free(s);
                const default_project = try stmt.columnTextOpt(4, allocator);
                errdefer if (default_project) |s| allocator.free(s);
                const link_count = stmt.columnInt(5);
                const conflict_count = stmt.columnInt(6);
                const error_count = stmt.columnInt(7);

                // Build display_text: "[kind] slug (N links)"
                // MEMORY GUARD (rule (c)): display_text is always freshly allocated;
                // never aliases kind or slug.
                const display_text = std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s} ({d} links)",
                    .{ kind, slug, link_count },
                ) catch try allocator.dupe(u8, slug);
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .kind = kind,
                    .slug = slug,
                    .base_url = base_url,
                    .default_project = default_project,
                    .link_count = link_count,
                    .conflict_count = conflict_count,
                    .error_count = error_count,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query all external_links rows, with resolved local-entity titles (task 4029).
///
/// Links are ordered by system_id asc, entity_kind asc, entity_id asc so the
/// caller can group them by system when rendering.
pub fn queryExtLinks(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]ExtLinkRow {
    var stmt = d.prepare(
        \\select
        \\  el.id, el.system_id,
        \\  el.entity_kind, el.entity_id,
        \\  el.external_id, el.external_url,
        \\  el.link_role, el.sync_direction,
        \\  el.last_synced_at, el.last_sync_status
        \\from external_links el
        \\order by el.system_id asc, el.entity_kind asc, el.entity_id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(ExtLinkRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const system_id = stmt.columnInt(1);
                const entity_kind = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(entity_kind);
                const entity_id = stmt.columnInt(3);
                const external_id = try stmt.columnTextAlloc(4, allocator);
                errdefer allocator.free(external_id);
                const external_url = try stmt.columnTextOpt(5, allocator);
                errdefer if (external_url) |s| allocator.free(s);
                const link_role = try stmt.columnTextAlloc(6, allocator);
                errdefer allocator.free(link_role);
                const sync_direction = try stmt.columnTextAlloc(7, allocator);
                errdefer allocator.free(sync_direction);
                const last_synced_at = try stmt.columnTextOpt(8, allocator);
                errdefer if (last_synced_at) |s| allocator.free(s);
                const last_sync_status = try stmt.columnTextAlloc(9, allocator);
                errdefer allocator.free(last_sync_status);

                // Resolve entity title via resolveEntityTitle. On OOM/error,
                // MEMORY GUARD (rule (c)): use a freshly duped placeholder,
                // never alias entity_kind.
                const entity_title = resolveEntityTitle(d, allocator, entity_kind, entity_id) orelse
                    allocator.dupe(u8, "(unknown)") catch try allocator.dupe(u8, "");
                errdefer allocator.free(entity_title);

                // Build display_text: "entity_kind:id — status"
                // MEMORY GUARD: always freshly allocated.
                const display_text = std.fmt.allocPrint(
                    allocator,
                    "{s}:{d} — {s}",
                    .{ entity_kind, entity_id, last_sync_status },
                ) catch try allocator.dupe(u8, last_sync_status);
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .system_id = system_id,
                    .entity_kind = entity_kind,
                    .entity_id = entity_id,
                    .entity_title = entity_title,
                    .external_id = external_id,
                    .external_url = external_url,
                    .link_role = link_role,
                    .sync_direction = sync_direction,
                    .last_synced_at = last_synced_at,
                    .last_sync_status = last_sync_status,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query the latest sync event per external system (task 4030).
///
/// For each system_id that has at least one sync_event (via external_links),
/// returns the most recent event. Systems with no events are omitted.
pub fn queryExtSystemSyncStatus(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]ExtSystemSyncStatus {
    // Correlated subquery: for each system, pick the sync_event with the
    // maximum `at` value (lexicographic ISO-8601 sort works correctly).
    // Tie-break by max(id) when two events share the same `at` timestamp
    // so the result is deterministic regardless of insertion order.
    var stmt = d.prepare(
        \\select
        \\  el.system_id,
        \\  se.at,
        \\  se.outcome,
        \\  se.direction
        \\from sync_events se
        \\join external_links el on el.id = se.link_id
        \\where se.id = (
        \\  select se2.id
        \\  from sync_events se2
        \\  join external_links el2 on el2.id = se2.link_id
        \\  where el2.system_id = el.system_id
        \\  order by se2.at desc, se2.id desc
        \\  limit 1
        \\)
        \\group by el.system_id
        \\order by el.system_id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(ExtSystemSyncStatus) = .empty;
    errdefer {
        for (out.items) |ss| ss.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const system_id = stmt.columnInt(0);
                const last_sync_at = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(last_sync_at);
                const last_outcome = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(last_outcome);
                const last_direction = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(last_direction);

                try out.append(allocator, .{
                    .system_id = system_id,
                    .last_sync_at = last_sync_at,
                    .last_outcome = last_outcome,
                    .last_direction = last_direction,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query unresolved sync conflicts (task 4031).
///
/// The two scope classes use different resolution semantics and are queried
/// separately, then unified via UNION ALL:
///
/// EXTERNAL conflicts: driven from external_links.last_sync_status='conflict'.
///   A conflicted external link is cleared by a later successful sync that
///   flips last_sync_status back to 'ok' (via updateSyncState in external/sync.zig).
///   The external sync Outcome enum never emits 'resolved-fs' / 'resolved-db';
///   those outcomes are workbench-only.  For display info (direction, detail, at)
///   we join to the latest sync_event with outcome='conflict' for that link,
///   ordered by at desc then id desc for determinism when timestamps tie.
///
/// WORKBENCH conflicts: from sync_events where scope='workbench' and outcome='conflict'.
///   Workbench conflicts are resolved by an in-place UPDATE of the sync_events row
///   to outcome='resolved-fs' or 'resolved-db' (workbench/sync.zig ~line 202-205).
///   A row still showing outcome='conflict' is genuinely unresolved.
///   Workbench conflict rows have link_id IS NULL.
///
/// Both the systems navigator's conflict_count (which reads external_links.last_sync_status)
/// and this surface now draw from the same signal for external links, so the two
/// panes cannot contradict each other.
///
/// Returns rows ordered by at desc (newest unresolved conflicts first).
pub fn queryUnresolvedConflicts(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]UnresolvedConflictRow {
    // UNION ALL of:
    //   Part A — external conflicts: one row per external_link at last_sync_status='conflict',
    //            with display info from the latest conflict sync_event for that link.
    //            Uses a correlated subquery with max(id) tiebreak for determinism.
    //   Part B — workbench conflicts: sync_event rows still at outcome='conflict'
    //            (in-place resolution means outcome='conflict' <=> unresolved).
    var stmt = d.prepare(
        \\select
        \\  coalesce(se.id, -el.id) as sort_id,
        \\  el.id          as link_id,
        \\  'external'     as scope,
        \\  coalesce(se.direction, 'pull') as direction,
        \\  se.detail,
        \\  coalesce(se.at, el.last_synced_at, el.created_at) as at
        \\from external_links el
        \\left join sync_events se
        \\  on se.id = (
        \\    select id from sync_events
        \\    where link_id = el.id
        \\      and outcome = 'conflict'
        \\    order by at desc, id desc
        \\    limit 1
        \\  )
        \\where el.last_sync_status = 'conflict'
        \\union all
        \\select
        \\  se.id as sort_id,
        \\  null  as link_id,
        \\  'workbench' as scope,
        \\  se.direction,
        \\  se.detail,
        \\  se.at
        \\from sync_events se
        \\where se.scope = 'workbench'
        \\  and se.outcome = 'conflict'
        \\order by at desc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(UnresolvedConflictRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const link_id = stmt.columnIntOpt(1);
                const scope = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(scope);
                const direction = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(direction);
                const detail = try stmt.columnTextOpt(4, allocator);
                errdefer if (detail) |s| allocator.free(s);
                const at = try stmt.columnTextAlloc(5, allocator);
                errdefer allocator.free(at);

                // Build display_text: "scope:direction [at-prefix]"
                // MEMORY GUARD: always freshly allocated; never aliases scope/direction/at.
                const at_prefix: []const u8 = if (at.len >= 16) at[0..16] else at;
                const display_text = std.fmt.allocPrint(
                    allocator,
                    "{s}:{s} [{s}]",
                    .{ scope, direction, at_prefix },
                ) catch try allocator.dupe(u8, scope);
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .link_id = link_id,
                    .scope = scope,
                    .direction = direction,
                    .detail = detail,
                    .at = at,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query the full ExtOpsSnapshot (tasks 4029, 4030, 4031).
/// Caller owns the result; free via `snapshot.deinit(allocator)`.
pub fn queryExtOpsSnapshot(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) !ExtOpsSnapshot {
    const systems = try queryExtSystems(d, allocator);
    errdefer ExtSystemRow.deinitMany(systems, allocator);

    const links = try queryExtLinks(d, allocator);
    errdefer ExtLinkRow.deinitMany(links, allocator);

    const system_sync = try queryExtSystemSyncStatus(d, allocator);
    errdefer {
        for (system_sync) |ss| ss.deinit(allocator);
        allocator.free(system_sync);
    }

    const conflicts = try queryUnresolvedConflicts(d, allocator);
    errdefer UnresolvedConflictRow.deinitMany(conflicts, allocator);

    return .{
        .systems = systems,
        .links = links,
        .system_sync = system_sync,
        .conflicts = conflicts,
    };
}

// =========================================================================
// view_model external-ops tests
// =========================================================================

fn setupTestDbExtOps(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "view_model: queryExtSystems on empty DB yields empty slice" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const rows = try queryExtSystems(&d, a);
    defer ExtSystemRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryExtSystems returns registered system with link counts (task 4029)" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        \\insert into external_systems (kind, slug, auth_method, auth_ref)
        \\values ('github-issues', 'gh-rdrsss', 'gh-cli', 'gh')
    , &.{});

    // Seed a task.
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Linked Task','todo')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status)
        \\values ('task', ?, ?, 'ISSUE-42', 'ok')
    , &.{ .{ .int = tid }, .{ .int = sys_id } });

    const rows = try queryExtSystems(&d, a);
    defer ExtSystemRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqualStrings("github-issues", rows[0].kind);
    try testing.expectEqualStrings("gh-rdrsss", rows[0].slug);
    try testing.expectEqual(@as(i64, 1), rows[0].link_count);
    try testing.expectEqual(@as(i64, 0), rows[0].conflict_count);
    try testing.expectEqual(@as(i64, 0), rows[0].error_count);
    // display_text must contain slug and link count.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "gh-rdrsss") != null);
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "1") != null);
}

test "view_model: queryExtLinks returns link with entity title (task 4029)" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-corp','token-env','JIRA_TOKEN')",
        &.{},
    );
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','My Plan','my-plan','active')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into external_links
        \\  (entity_kind, entity_id, system_id, external_id, link_role, sync_direction, last_sync_status)
        \\values ('plan', ?, ?, 'PROJ-1', 'mirror', 'two-way', 'ok')
    , &.{ .{ .int = pid }, .{ .int = sys_id } });

    const links = try queryExtLinks(&d, a);
    defer ExtLinkRow.deinitMany(links, a);

    try testing.expectEqual(@as(usize, 1), links.len);
    try testing.expectEqualStrings("plan", links[0].entity_kind);
    try testing.expectEqual(pid, links[0].entity_id);
    // Entity title resolved from plans table.
    try testing.expectEqualStrings("My Plan", links[0].entity_title);
    try testing.expectEqualStrings("PROJ-1", links[0].external_id);
    try testing.expectEqualStrings("ok", links[0].last_sync_status);
    // display_text must contain entity_kind:id and status.
    try testing.expect(std.mem.indexOf(u8, links[0].display_text, "plan:") != null);
    try testing.expect(std.mem.indexOf(u8, links[0].display_text, "ok") != null);
}

test "view_model: queryExtLinks unknown entity title falls back to placeholder" {
    // When entity_kind is an entity type resolveEntityTitle cannot look up,
    // entity_title is the placeholder "(unknown)" — not an alias of entity_kind.
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','corp','token-env','T')",
        &.{},
    );
    // Insert with entity_kind='session' which resolveEntityTitle does not handle.
    _ = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('session', 1, ?, 'S-1', 'never')",
        &.{.{ .int = sys_id }},
    );

    const links = try queryExtLinks(&d, a);
    defer ExtLinkRow.deinitMany(links, a);

    try testing.expectEqual(@as(usize, 1), links.len);
    // Placeholder must be its own allocation, not a pointer alias.
    const title = links[0].entity_title;
    try testing.expect(title.len > 0); // either "(unknown)" or ""
    // The important invariant: entity_title != entity_kind pointer.
    // We verify by content — if it were an alias of entity_kind it would be "session".
    try testing.expect(!std.mem.eql(u8, title, "session"));
}

test "view_model: queryExtSystemSyncStatus returns latest sync event (task 4030)" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('github-issues','gh-test','gh-cli','gh')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-1', 'ok')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    // Insert two sync events; the later one should be returned.
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'pull', 'ok', '2024-01-01T10:00:00.000Z')",
        &.{.{ .int = link_id }},
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'push', 'ok', '2025-06-01T12:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    const sync_statuses = try queryExtSystemSyncStatus(&d, a);
    defer {
        for (sync_statuses) |ss| ss.deinit(a);
        a.free(sync_statuses);
    }

    try testing.expectEqual(@as(usize, 1), sync_statuses.len);
    try testing.expectEqual(sys_id, sync_statuses[0].system_id);
    // Latest event: the 2025-06-01 push.
    try testing.expect(std.mem.indexOf(u8, sync_statuses[0].last_sync_at, "2025") != null);
    try testing.expectEqualStrings("ok", sync_statuses[0].last_outcome);
    try testing.expectEqualStrings("push", sync_statuses[0].last_direction);
}

test "view_model: queryExtSystemSyncStatus id tiebreak is deterministic (task 4030)" {
    // When two sync_events share the same `at` timestamp, the one with the
    // higher id (inserted later) must win.  Without the `id desc` tiebreak
    // SQLite's arbitrary GROUP BY row selection could return either one.
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('github-issues','gh-tie','gh-cli','gh')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-T', 'ok')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    // Two events at the same timestamp; the second (higher id) should win.
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'pull', 'conflict', '2025-05-01T00:00:00.000Z')",
        &.{.{ .int = link_id }},
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'push', 'ok', '2025-05-01T00:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    const sync_statuses = try queryExtSystemSyncStatus(&d, a);
    defer {
        for (sync_statuses) |ss| ss.deinit(a);
        a.free(sync_statuses);
    }

    try testing.expectEqual(@as(usize, 1), sync_statuses.len);
    // Higher-id event (push / ok) must win the tiebreak.
    try testing.expectEqualStrings("ok", sync_statuses[0].last_outcome);
    try testing.expectEqualStrings("push", sync_statuses[0].last_direction);
}

test "view_model: queryUnresolvedConflicts returns unresolved conflict (task 4031)" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-c','token-env','J')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-1', 'conflict')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, detail, at) values (?, 'external', 'pull', 'conflict', 'field mismatch', '2025-01-01T10:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    const conflicts = try queryUnresolvedConflicts(&d, a);
    defer UnresolvedConflictRow.deinitMany(conflicts, a);

    try testing.expectEqual(@as(usize, 1), conflicts.len);
    try testing.expectEqualStrings("external", conflicts[0].scope);
    try testing.expectEqualStrings("pull", conflicts[0].direction);
    try testing.expect(conflicts[0].detail != null);
    try testing.expectEqualStrings("field mismatch", conflicts[0].detail.?);
    try testing.expect(std.mem.indexOf(u8, conflicts[0].at, "2025") != null);
    // display_text: must contain scope and direction.
    try testing.expect(std.mem.indexOf(u8, conflicts[0].display_text, "external") != null);
    try testing.expect(std.mem.indexOf(u8, conflicts[0].display_text, "pull") != null);
}

test "view_model: queryUnresolvedConflicts external link re-synced ok drops from surface (task 4031)" {
    // Real external clear path: a conflicted external link is cleared when
    // updateSyncState flips last_sync_status back to 'ok' and a later 'ok'
    // sync_event is appended.  The external Outcome enum never emits
    // 'resolved-fs'/'resolved-db' — those are workbench-only.
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-r','token-env','J')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    const link_id = try d.execParams(
        // last_sync_status='ok' — the re-sync cleared the conflict.
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-1', 'ok')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    // conflict event followed by a later ok event (real production shape).
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'pull', 'conflict', '2025-01-01T10:00:00.000Z')",
        &.{.{ .int = link_id }},
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'push', 'ok', '2025-01-02T10:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    const conflicts = try queryUnresolvedConflicts(&d, a);
    defer UnresolvedConflictRow.deinitMany(conflicts, a);

    // last_sync_status='ok' → not in the unresolved surface.
    try testing.expectEqual(@as(usize, 0), conflicts.len);
}

test "view_model: queryUnresolvedConflicts external link still at conflict remains (task 4031)" {
    // Counterpart: external link still at last_sync_status='conflict' (no re-sync yet)
    // MUST appear in the unresolved surface.  The systems navigator computes
    // conflict_count from the same column, so the two panes agree.
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-s','token-env','J')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-2', 'conflict')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, detail, at) values (?, 'external', 'pull', 'conflict', 'field clash', '2025-03-01T09:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    const conflicts = try queryUnresolvedConflicts(&d, a);
    defer UnresolvedConflictRow.deinitMany(conflicts, a);

    try testing.expectEqual(@as(usize, 1), conflicts.len);
    try testing.expectEqualStrings("external", conflicts[0].scope);
    try testing.expect(conflicts[0].detail != null);
    try testing.expectEqualStrings("field clash", conflicts[0].detail.?);
}

test "view_model: queryUnresolvedConflicts workbench in-place resolved drops from surface (task 4031)" {
    // Real workbench clear path: the workbench sync engine updates the
    // sync_events row in place to outcome='resolved-fs' or 'resolved-db'
    // (workbench/sync.zig ~line 202-205).  A row at resolved-fs is cleared;
    // a row still at 'conflict' remains.
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    // Two workbench conflict rows: one resolved in-place, one still open.
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, detail, at) values (null, 'workbench', 'pull', 'resolved-fs', 'resolved', '2025-02-01T08:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, detail, at) values (null, 'workbench', 'push', 'conflict', 'open wb conflict', '2025-02-02T08:00:00.000Z')",
        &.{},
    );

    const conflicts = try queryUnresolvedConflicts(&d, a);
    defer UnresolvedConflictRow.deinitMany(conflicts, a);

    // Only the still-at-conflict row is unresolved.
    try testing.expectEqual(@as(usize, 1), conflicts.len);
    try testing.expectEqualStrings("workbench", conflicts[0].scope);
    try testing.expect(conflicts[0].link_id == null);
    try testing.expectEqualStrings("open wb conflict", conflicts[0].detail.?);
}

test "view_model: queryExtOpsSnapshot on empty DB has all slices empty" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    var snap = try queryExtOpsSnapshot(&d, a);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 0), snap.systems.len);
    try testing.expectEqual(@as(usize, 0), snap.links.len);
    try testing.expectEqual(@as(usize, 0), snap.system_sync.len);
    try testing.expectEqual(@as(usize, 0), snap.conflicts.len);
}

// =========================================================================
// Sessions & Handoff view-model  (tasks 4032, 4033, 4034)
// =========================================================================
//
// Schema sources (confirmed from migrations):
//   sessions (00005_sessions.up.sql, 00021_session_commits.up.sql):
//     id, task_id, project_id, agent_id, vendor, vendor_session_id, model,
//     started_at, ended_at, summary, repo_root, head_sha_at_start
//   session_entries (00005_sessions.up.sql):
//     id, session_id, ordinal, prefix, body, created_at
//     prefix check: ('action','observation','decision','question','file',
//                    'command','note','error','read')
//   context_snapshots (00005_sessions.up.sql):
//     id, session_id, task_id, vendor, vendor_session_id, body, next_action,
//     created_at
//   handoffs (00005_sessions.up.sql, 00017_handoffs_worktree.up.sql):
//     id, from_snapshot_id, to_session_id, from_vendor, to_vendor,
//     status, validated_at, consumed_at, created_at,
//     worktree_path, repo_root, branch
//     status check: ('pending','validated','consumed','abandoned')
//   session_commits (00021_session_commits.up.sql):
//     id, session_id, claim_id, sha, repo_root, branch, subject, author,
//     committed_at, recorded_at
//
// Resume-readiness representation: A handoff is resume-ready when its
// status is 'validated' (the handoff has been validated and its snapshot
// body is populated) or 'consumed' (the handoff was already consumed by a
// successor session — historically ready). A status of 'pending' means the
// snapshot has not yet been validated; 'abandoned' means the handoff was
// intentionally cancelled. This is checked by looking at handoffs.status
// and the associated context_snapshots row. A context_snapshot row with a
// non-null next_action is the minimum readiness marker.

/// One row in the Sessions navigator list.
///
/// Columns: sessions.id, vendor, model, started_at, ended_at, summary,
///          task_id (for display), entry count.
pub const SessionRow = struct {
    id: i64,
    /// Vendor string (e.g. "claude", "codex").
    vendor: []const u8,
    /// Model string or null.
    model: ?[]const u8,
    /// ISO-8601 started_at.
    started_at: []const u8,
    /// ISO-8601 ended_at or null (session still active).
    ended_at: ?[]const u8,
    /// Optional summary.
    summary: ?[]const u8,
    /// Referenced task id (nullable).
    task_id: ?i64,
    /// Count of session_entries rows for this session.
    entry_count: i64,
    /// Pre-formatted display text: "vendor [model?] started_at (N entries)".
    /// Heap-allocated; freed by deinit.
    display_text: []const u8,

    pub fn deinit(self: SessionRow, allocator: std.mem.Allocator) void {
        allocator.free(self.vendor);
        if (self.model) |s| allocator.free(s);
        allocator.free(self.started_at);
        if (self.ended_at) |s| allocator.free(s);
        if (self.summary) |s| allocator.free(s);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []SessionRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One entry in the session lineage detail pane.
///
/// Columns: session_entries.id, ordinal, prefix, body, created_at.
pub const SessionEntryRow = struct {
    id: i64,
    ordinal: i64,
    /// Entry kind prefix: 'action', 'observation', 'decision', etc.
    prefix: []const u8,
    /// Entry body text.
    body: []const u8,
    /// ISO-8601 created_at.
    created_at: []const u8,
    /// Pre-formatted display text: "[prefix] body_prefix".
    display_text: []const u8,

    pub fn deinit(self: SessionEntryRow, allocator: std.mem.Allocator) void {
        allocator.free(self.prefix);
        allocator.free(self.body);
        allocator.free(self.created_at);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []SessionEntryRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One commit associated with a session (from session_commits).
///
/// Columns: session_commits.sha, subject, author, branch, committed_at.
pub const SessionCommitRow = struct {
    id: i64,
    /// Full commit SHA.
    sha: []const u8,
    /// Commit subject line (first line of message), or null.
    subject: ?[]const u8,
    /// Author string, or null.
    author: ?[]const u8,
    /// Branch name, or null.
    branch: ?[]const u8,
    /// ISO-8601 committed_at, or null.
    committed_at: ?[]const u8,
    /// Pre-formatted display text: "sha_short subject_prefix".
    display_text: []const u8,

    pub fn deinit(self: SessionCommitRow, allocator: std.mem.Allocator) void {
        allocator.free(self.sha);
        if (self.subject) |s| allocator.free(s);
        if (self.author) |s| allocator.free(s);
        if (self.branch) |s| allocator.free(s);
        if (self.committed_at) |s| allocator.free(s);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []SessionCommitRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One handoff row with resume-readiness information.
///
/// Columns: handoffs.id, from_vendor, to_vendor, status, created_at,
///          validated_at, consumed_at, context_snapshots.next_action,
///          context_snapshots.body (non-null presence).
///
/// Resume-readiness: a handoff is ready when status in ('validated','consumed')
/// AND the associated context_snapshot has a non-null next_action.
/// A 'pending' handoff is not yet ready (snapshot not validated).
/// An 'abandoned' handoff is not resume-ready.
pub const HandoffRow = struct {
    id: i64,
    /// Source vendor.
    from_vendor: []const u8,
    /// Destination vendor, or null.
    to_vendor: ?[]const u8,
    /// Handoff lifecycle status: 'pending', 'validated', 'consumed', 'abandoned'.
    /// Surfaced as lifecycle info only — it does NOT gate resume-readiness.
    status: []const u8,
    /// ISO-8601 created_at.
    created_at: []const u8,
    /// ISO-8601 validated_at, or null.
    validated_at: ?[]const u8,
    /// Whether the associated task has a non-empty next_action (tasks.next_action).
    /// Mirrors the first condition in resume.validate (src/engine/runtime/resume.zig:300).
    task_has_next_action: bool,
    /// Whether a context_snapshots row scoped to the associated task exists.
    /// Mirrors the second condition in resume.validate (src/engine/runtime/resume.zig:314).
    task_has_snapshot: bool,
    /// Pre-formatted display text: "from→to [status] (ready/not-ready)".
    display_text: []const u8,

    /// True when the handoff is resume-ready, mirroring the engine's authoritative
    /// oracle at src/engine/runtime/resume.zig (validate, lines 300-336):
    ///   tasks.next_action is non-empty AND a task-scoped context_snapshots row exists.
    /// handoffs.status is NOT a readiness gate — it is lifecycle metadata only.
    pub fn isResumeReady(self: *const HandoffRow) bool {
        return self.task_has_next_action and self.task_has_snapshot;
    }

    pub fn deinit(self: HandoffRow, allocator: std.mem.Allocator) void {
        allocator.free(self.from_vendor);
        if (self.to_vendor) |s| allocator.free(s);
        allocator.free(self.status);
        allocator.free(self.created_at);
        if (self.validated_at) |s| allocator.free(s);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []HandoffRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Full snapshot for the Sessions & Handoff view.
pub const SessionsHandoffSnapshot = struct {
    /// Sessions list, newest-first.
    sessions: []SessionRow,
    /// Handoffs list, newest-first.
    handoffs: []HandoffRow,

    pub fn deinit(self: SessionsHandoffSnapshot, allocator: std.mem.Allocator) void {
        SessionRow.deinitMany(self.sessions, allocator);
        HandoffRow.deinitMany(self.handoffs, allocator);
    }
};

/// Query sessions ordered newest-first. Includes entry count per session.
/// Caller owns the result; free via `SessionRow.deinitMany`.
pub fn querySessions(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]SessionRow {
    var stmt = d.prepare(
        \\select s.id, s.vendor, s.model, s.started_at, s.ended_at, s.summary,
        \\       s.task_id,
        \\       (select count(*) from session_entries se where se.session_id = s.id)
        \\         as entry_count
        \\from sessions s
        \\order by s.started_at desc, s.id desc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(SessionRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const vendor = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(vendor);
                const model = try stmt.columnTextOpt(2, allocator);
                errdefer if (model) |s| allocator.free(s);
                const started_at = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(started_at);
                const ended_at = try stmt.columnTextOpt(4, allocator);
                errdefer if (ended_at) |s| allocator.free(s);
                const summary = try stmt.columnTextOpt(5, allocator);
                errdefer if (summary) |s| allocator.free(s);
                const task_id = stmt.columnIntOpt(6);
                const entry_count = stmt.columnInt(7);

                // Build display_text: "vendor [model] started_prefix (N entries)"
                // Use at most the date part of started_at (first 10 chars).
                const date_prefix: []const u8 = if (started_at.len >= 10)
                    started_at[0..10]
                else
                    started_at;

                const display_text = if (model) |m|
                    try std.fmt.allocPrint(
                        allocator,
                        "{s} [{s}] {s} ({d} entries)",
                        .{ vendor, m, date_prefix, entry_count },
                    )
                else
                    try std.fmt.allocPrint(
                        allocator,
                        "{s} {s} ({d} entries)",
                        .{ vendor, date_prefix, entry_count },
                    );
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .vendor = vendor,
                    .model = model,
                    .started_at = started_at,
                    .ended_at = ended_at,
                    .summary = summary,
                    .task_id = task_id,
                    .entry_count = entry_count,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query session_entries for a specific session, ordered by ordinal.
/// Each entry is the activity lineage of the session.
/// Caller owns the result; free via `SessionEntryRow.deinitMany`.
pub fn querySessionEntries(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
) ![]SessionEntryRow {
    var stmt = d.prepare(
        \\select se.id, se.ordinal, se.prefix, se.body, se.created_at
        \\from session_entries se
        \\where se.session_id = ?
        \\order by se.ordinal asc, se.id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(SessionEntryRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const ordinal = stmt.columnInt(1);
                const prefix = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(prefix);
                const body = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(body);
                const created_at = try stmt.columnTextAlloc(4, allocator);
                errdefer allocator.free(created_at);

                // display_text: "[prefix] body_prefix (first 60 chars of body)"
                const body_preview: []const u8 = if (body.len > 60) body[0..60] else body;
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s}",
                    .{ prefix, body_preview },
                );
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .ordinal = ordinal,
                    .prefix = prefix,
                    .body = body,
                    .created_at = created_at,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query session_commits for a specific session, ordered by committed_at then id.
/// Caller owns the result; free via `SessionCommitRow.deinitMany`.
pub fn querySessionCommits(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
) ![]SessionCommitRow {
    var stmt = d.prepare(
        \\select sc.id, sc.sha, sc.subject, sc.author, sc.branch, sc.committed_at
        \\from session_commits sc
        \\where sc.session_id = ?
        \\order by coalesce(sc.committed_at, sc.recorded_at) asc, sc.id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(SessionCommitRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const sha = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(sha);
                const subject = try stmt.columnTextOpt(2, allocator);
                errdefer if (subject) |s| allocator.free(s);
                const author = try stmt.columnTextOpt(3, allocator);
                errdefer if (author) |s| allocator.free(s);
                const branch = try stmt.columnTextOpt(4, allocator);
                errdefer if (branch) |s| allocator.free(s);
                const committed_at = try stmt.columnTextOpt(5, allocator);
                errdefer if (committed_at) |s| allocator.free(s);

                // display_text: "sha_short [subject_preview]"
                const sha_short: []const u8 = if (sha.len >= 8) sha[0..8] else sha;
                const display_text = if (subject) |subj|
                    try std.fmt.allocPrint(
                        allocator,
                        "{s} {s}",
                        .{ sha_short, subj },
                    )
                else
                    try allocator.dupe(u8, sha_short);
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .sha = sha,
                    .subject = subject,
                    .author = author,
                    .branch = branch,
                    .committed_at = committed_at,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query handoffs ordered newest-first.
///
/// Joins through context_snapshots → tasks to compute the task-centric
/// resume-readiness signal that mirrors the engine's authoritative oracle
/// at src/engine/runtime/resume.zig (validate, lines 300-336):
///
///   task_has_next_action: tasks.next_action is non-empty for the task
///     associated with this handoff's source snapshot.
///   task_has_snapshot: at least one context_snapshots row is scoped
///     to that task (context_snapshots.task_id = tasks.id).
///
/// handoffs.status is surfaced as lifecycle metadata but does NOT gate
/// readiness — a 'pending' handoff whose task satisfies the engine rule
/// is READY; a 'consumed' handoff whose task next_action was cleared is
/// NOT READY. isResumeReady() encodes only the task-centric rule.
///
/// Caller owns the result; free via `HandoffRow.deinitMany`.
pub fn queryHandoffs(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]HandoffRow {
    var stmt = d.prepare(
        \\select h.id, h.from_vendor, h.to_vendor, h.status, h.created_at,
        \\       h.validated_at,
        \\       (t.next_action is not null and t.next_action != '') as task_has_next_action,
        \\       (exists (
        \\           select 1 from context_snapshots cs2
        \\           where cs2.task_id = cs.task_id and cs2.task_id is not null
        \\       )) as task_has_snapshot
        \\from handoffs h
        \\left join context_snapshots cs on cs.id = h.from_snapshot_id
        \\left join tasks t on t.id = cs.task_id
        \\order by h.created_at desc, h.id desc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(HandoffRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const from_vendor = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(from_vendor);
                const to_vendor = try stmt.columnTextOpt(2, allocator);
                errdefer if (to_vendor) |s| allocator.free(s);
                const status = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(status);
                const created_at = try stmt.columnTextAlloc(4, allocator);
                errdefer allocator.free(created_at);
                const validated_at = try stmt.columnTextOpt(5, allocator);
                errdefer if (validated_at) |s| allocator.free(s);
                const task_has_next_action = stmt.columnInt(6) != 0;
                const task_has_snapshot = stmt.columnInt(7) != 0;

                // Readiness is task-centric: both task conditions must hold.
                const is_ready = task_has_next_action and task_has_snapshot;

                // Determine readiness label.
                const ready_label: []const u8 = if (is_ready)
                    "ready"
                else
                    "not-ready";

                // display_text: "from→to  [status]  (ready/not-ready)"
                const to_str: []const u8 = to_vendor orelse "?";
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "{s}→{s}  [{s}]  ({s})",
                    .{ from_vendor, to_str, status, ready_label },
                );
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .from_vendor = from_vendor,
                    .to_vendor = to_vendor,
                    .status = status,
                    .created_at = created_at,
                    .validated_at = validated_at,
                    .task_has_next_action = task_has_next_action,
                    .task_has_snapshot = task_has_snapshot,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query the full SessionsHandoffSnapshot.
pub fn querySessionsHandoffSnapshot(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) !SessionsHandoffSnapshot {
    const sessions = try querySessions(d, allocator);
    errdefer SessionRow.deinitMany(sessions, allocator);

    const handoffs = try queryHandoffs(d, allocator);
    errdefer HandoffRow.deinitMany(handoffs, allocator);

    return .{
        .sessions = sessions,
        .handoffs = handoffs,
    };
}

// =========================================================================
// Sessions & Handoff view-model tests
// =========================================================================

fn setupTestDbSessions(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "view_model: querySessions on empty DB returns empty slice (task 4032)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const rows = try querySessions(&d, a);
    defer SessionRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: querySessions returns session row with entry count (task 4032)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Session Task','todo')",
        &.{},
    );
    const sid = try d.execParams(
        "insert into sessions (task_id, vendor, model, started_at) values (?, 'claude', 'claude-3', '2026-06-10T10:00:00.000Z')",
        &.{.{ .int = tid }},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'action', 'Implemented the feature')",
        &.{.{ .int = sid }},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 2, 'observation', 'Tests pass')",
        &.{.{ .int = sid }},
    );

    const rows = try querySessions(&d, a);
    defer SessionRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqualStrings("claude", rows[0].vendor);
    try testing.expect(rows[0].model != null);
    try testing.expectEqualStrings("claude-3", rows[0].model.?);
    try testing.expectEqual(@as(i64, 2), rows[0].entry_count);
    try testing.expectEqual(tid, rows[0].task_id.?);
    // display_text must contain vendor and entry count.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "claude") != null);
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "2") != null);
}

test "view_model: querySessionEntries returns entries in ordinal order (task 4032)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor, started_at) values ('codex', '2026-06-11T09:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 2, 'observation', 'Second entry')",
        &.{.{ .int = sid }},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'action', 'First entry')",
        &.{.{ .int = sid }},
    );

    const entries = try querySessionEntries(&d, a, sid);
    defer SessionEntryRow.deinitMany(entries, a);

    try testing.expectEqual(@as(usize, 2), entries.len);
    // Ordered by ordinal asc.
    try testing.expectEqual(@as(i64, 1), entries[0].ordinal);
    try testing.expectEqualStrings("action", entries[0].prefix);
    try testing.expectEqualStrings("First entry", entries[0].body);
    try testing.expectEqual(@as(i64, 2), entries[1].ordinal);
    try testing.expectEqualStrings("observation", entries[1].prefix);
    // display_text includes prefix.
    try testing.expect(std.mem.indexOf(u8, entries[0].display_text, "action") != null);
    try testing.expect(std.mem.indexOf(u8, entries[0].display_text, "First entry") != null);
}

test "view_model: querySessionCommits returns commits for session (task 4034)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor, started_at) values ('claude', '2026-06-12T10:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into session_commits (session_id, sha, subject, author, branch, committed_at)
        \\values (?, 'abc1234567890def', 'feat: add sessions view', 'Dev <dev@example.com>', 'feature/m11', '2026-06-12T11:00:00.000Z')
    , &.{.{ .int = sid }});
    _ = try d.execParams(
        \\insert into session_commits (session_id, sha, subject, committed_at)
        \\values (?, 'deadbeef11223344', 'fix: typo in render', '2026-06-12T12:00:00.000Z')
    , &.{.{ .int = sid }});

    const commits = try querySessionCommits(&d, a, sid);
    defer SessionCommitRow.deinitMany(commits, a);

    try testing.expectEqual(@as(usize, 2), commits.len);
    // Ordered by committed_at asc.
    try testing.expectEqualStrings("abc1234567890def", commits[0].sha);
    try testing.expect(commits[0].subject != null);
    try testing.expectEqualStrings("feat: add sessions view", commits[0].subject.?);
    try testing.expect(commits[0].author != null);
    try testing.expectEqualStrings("Dev <dev@example.com>", commits[0].author.?);
    try testing.expect(commits[0].branch != null);
    try testing.expectEqualStrings("feature/m11", commits[0].branch.?);
    // display_text: "sha_short subject"
    try testing.expect(std.mem.indexOf(u8, commits[0].display_text, "abc12345") != null);
    try testing.expect(std.mem.indexOf(u8, commits[0].display_text, "feat: add sessions view") != null);
    // Second commit.
    try testing.expectEqualStrings("deadbeef11223344", commits[1].sha);
    try testing.expect(std.mem.indexOf(u8, commits[1].display_text, "deadbeef") != null);
}

test "view_model: querySessionCommits returns empty for session with no commits (task 4034)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor, started_at) values ('claude', '2026-06-13T10:00:00.000Z')",
        &.{},
    );

    const commits = try querySessionCommits(&d, a, sid);
    defer SessionCommitRow.deinitMany(commits, a);

    try testing.expectEqual(@as(usize, 0), commits.len);
}

test "view_model: queryHandoffs pending handoff with task next_action + snapshot IS resume-ready (task 4033)" {
    // Engine rule (resume.zig:300-336): READY iff tasks.next_action non-empty
    // AND a task-scoped context_snapshots row exists.  handoffs.status is NOT a
    // gate — a 'pending' handoff whose task satisfies the rule must be READY.
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    // Insert a task with next_action set.
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, next_action) values ('global', 'fix bug', 'Continue implementing the fix')",
        &.{},
    );
    const s1 = try d.execParams(
        "insert into sessions (vendor, started_at, task_id) values ('claude', '2026-06-10T08:00:00.000Z', ?)",
        &.{.{ .int = task_id }},
    );
    // Snapshot is task-scoped (task_id set).
    const snap_id = try d.execParams(
        "insert into context_snapshots (session_id, task_id, vendor, body) values (?, ?, 'claude', 'Summary body')",
        &.{ .{ .int = s1 }, .{ .int = task_id } },
    );
    // Status is 'pending' — the old rule would say NOT READY, the engine says READY.
    _ = try d.execParams(
        "insert into handoffs (from_snapshot_id, from_vendor, to_vendor, status) values (?, 'claude', 'codex', 'pending')",
        &.{.{ .int = snap_id }},
    );

    const handoffs = try queryHandoffs(&d, a);
    defer HandoffRow.deinitMany(handoffs, a);

    try testing.expectEqual(@as(usize, 1), handoffs.len);
    try testing.expectEqualStrings("claude", handoffs[0].from_vendor);
    try testing.expect(handoffs[0].to_vendor != null);
    try testing.expectEqualStrings("codex", handoffs[0].to_vendor.?);
    // Status is 'pending' — lifecycle info, not a readiness gate.
    try testing.expectEqualStrings("pending", handoffs[0].status);
    // Task-centric conditions must both be true.
    try testing.expect(handoffs[0].task_has_next_action);
    try testing.expect(handoffs[0].task_has_snapshot);
    // isResumeReady() must be true despite pending status.
    try testing.expect(handoffs[0].isResumeReady());
    // display_text must say "ready".
    try testing.expect(std.mem.indexOf(u8, handoffs[0].display_text, "ready") != null);
}

test "view_model: queryHandoffs consumed handoff with cleared task next_action is NOT resume-ready (task 4033)" {
    // Engine rule: tasks.next_action empty → NOT READY regardless of handoffs.status.
    // A 'consumed' handoff whose task's next_action was cleared is NOT resumable.
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    // Task with no next_action (cleared after consumption).
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title) values ('global', 'finished task')",
        &.{},
    );
    const s1 = try d.execParams(
        "insert into sessions (vendor, started_at, task_id) values ('claude', '2026-06-11T08:00:00.000Z', ?)",
        &.{.{ .int = task_id }},
    );
    const snap_id = try d.execParams(
        "insert into context_snapshots (session_id, task_id, vendor, body) values (?, ?, 'claude', 'Completed summary')",
        &.{ .{ .int = s1 }, .{ .int = task_id } },
    );
    // Status is 'consumed' — the old rule would say READY, the engine says NOT READY.
    _ = try d.execParams(
        "insert into handoffs (from_snapshot_id, from_vendor, status, consumed_at) values (?, 'claude', 'consumed', '2026-06-11T10:00:00.000Z')",
        &.{.{ .int = snap_id }},
    );

    const handoffs = try queryHandoffs(&d, a);
    defer HandoffRow.deinitMany(handoffs, a);

    try testing.expectEqual(@as(usize, 1), handoffs.len);
    try testing.expectEqualStrings("consumed", handoffs[0].status);
    // task_has_next_action must be false (task.next_action is null/empty).
    try testing.expect(!handoffs[0].task_has_next_action);
    // task_has_snapshot is true (snapshot exists), but that alone is not enough.
    try testing.expect(handoffs[0].task_has_snapshot);
    // isResumeReady() must be false despite consumed status.
    try testing.expect(!handoffs[0].isResumeReady());
    // display_text must say "not-ready".
    try testing.expect(std.mem.indexOf(u8, handoffs[0].display_text, "not-ready") != null);
}

test "view_model: queryHandoffs handoff with no task-scoped snapshot is NOT resume-ready (task 4033)" {
    // Engine rule: snapshot must exist scoped to the task — snapshot alone
    // without task_id set is not sufficient.
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    // Task with next_action — but snapshot not linked to the task.
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, next_action) values ('global', 'needs snapshot', 'Do the thing')",
        &.{},
    );
    const s1 = try d.execParams(
        "insert into sessions (vendor, started_at, task_id) values ('claude', '2026-06-12T08:00:00.000Z', ?)",
        &.{.{ .int = task_id }},
    );
    // Snapshot has NO task_id — not task-scoped.
    const snap_id = try d.execParams(
        "insert into context_snapshots (session_id, vendor, body) values (?, 'claude', 'Partial body')",
        &.{.{ .int = s1 }},
    );
    _ = try d.execParams(
        "insert into handoffs (from_snapshot_id, from_vendor, status) values (?, 'claude', 'validated')",
        &.{.{ .int = snap_id }},
    );

    const handoffs = try queryHandoffs(&d, a);
    defer HandoffRow.deinitMany(handoffs, a);

    try testing.expectEqual(@as(usize, 1), handoffs.len);
    // The snapshot is not task-scoped, so the task join returns no task row;
    // task_has_next_action and task_has_snapshot are both false.
    try testing.expect(!handoffs[0].task_has_next_action);
    try testing.expect(!handoffs[0].task_has_snapshot);
    try testing.expect(!handoffs[0].isResumeReady());
    try testing.expect(std.mem.indexOf(u8, handoffs[0].display_text, "not-ready") != null);
}

test "view_model: queryHandoffs empty DB returns empty slice (task 4033)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const handoffs = try queryHandoffs(&d, a);
    defer HandoffRow.deinitMany(handoffs, a);

    try testing.expectEqual(@as(usize, 0), handoffs.len);
}

test "view_model: querySessionsHandoffSnapshot combines both (task 4032/4033)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    _ = try d.execParams(
        "insert into sessions (vendor, started_at) values ('claude', '2026-06-12T08:00:00.000Z')",
        &.{},
    );

    var snap = try querySessionsHandoffSnapshot(&d, a);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 1), snap.sessions.len);
    try testing.expectEqual(@as(usize, 0), snap.handoffs.len);
}

// =========================================================================
// Audit Log view-model  (tasks 4035, 4036, M12)
//
// Reads: audit_log (id, verb, entity_kind, entity_id, actor, scope,
//        summary, recorded_at). Schema confirmed from
//        migrations/00014_audit_log.up.sql.
//
// Filter: optional entity_kind+entity_id pair. When set, only rows for
//         that entity are returned. When null, all rows are returned.
// Order:  newest-first (recorded_at DESC).
// =========================================================================

/// One row in the Audit Log navigator list. All string fields are
/// heap-allocated and owned by the caller; release via deinit.
pub const AuditLogRow = struct {
    id: i64,
    /// Mutation verb: "create" | "update" | "delete" | "status_change" |
    /// "link" | "unlink". Confirmed CHECK constraint from migration 00014.
    verb: []const u8,
    /// Entity kind string, e.g. "task", "plan", "decision".
    entity_kind: []const u8,
    entity_id: i64,
    /// Optional actor label (free-text, e.g. "claude" or CLI username).
    actor: ?[]const u8,
    /// Optional scope string.
    scope: ?[]const u8,
    /// Optional human-readable mutation summary.
    summary: ?[]const u8,
    /// ISO-8601 timestamp when the mutation was recorded.
    recorded_at: []const u8,
    /// Pre-formatted display text: "[verb] entity_kind:entity_id  actor  recorded_at".
    /// Heap-allocated. Used by the navigator renderer directly so that
    /// grapheme pointers remain valid for the duration of the render.
    display_text: []const u8,
    /// Pre-formatted entity reference: "entity_kind:entity_id" (e.g. "task:55").
    /// Heap-allocated. Used by the detail pane renderer to avoid stack-local
    /// format buffers whose grapheme pointers dangle after the render function
    /// returns (MEMORY GUARD rule (c)).
    entity_ref: []const u8,

    pub fn deinit(self: AuditLogRow, allocator: std.mem.Allocator) void {
        allocator.free(self.verb);
        allocator.free(self.entity_kind);
        if (self.actor) |s| allocator.free(s);
        if (self.scope) |s| allocator.free(s);
        if (self.summary) |s| allocator.free(s);
        allocator.free(self.recorded_at);
        allocator.free(self.display_text);
        allocator.free(self.entity_ref);
    }

    pub fn deinitMany(rows: []AuditLogRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Entity filter for the Audit Log view.
///
/// When filter is .all, all rows are returned. When filter is .entity,
/// only rows where entity_kind=kind AND entity_id=id are returned.
/// This is the entity-narrowing affordance for task 4036.
pub const AuditEntityFilter = union(enum) {
    all,
    entity: struct {
        kind: []const u8,
        id: i64,
    },

    /// Return a human-readable label for display in the filter bar.
    /// Uses a fixed stack buffer; the result is valid for the call duration.
    pub fn label(self: AuditEntityFilter, buf: []u8) []const u8 {
        return switch (self) {
            .all => std.fmt.bufPrint(buf, "all entities", .{}) catch "all entities",
            .entity => |e| std.fmt.bufPrint(buf, "{s}:{d}", .{ e.kind, e.id }) catch "entity",
        };
    }
};

/// Query audit_log rows, newest-first. When `filter` is `.entity`, narrows
/// to the given entity_kind+entity_id pair. Returns a heap-allocated slice;
/// caller owns and must release via `AuditLogRow.deinitMany`.
///
/// Column order: id(0), verb(1), entity_kind(2), entity_id(3),
///               actor(4), scope(5), summary(6), recorded_at(7).
///
/// MEMORY GUARD (brief rule (c)): all string fields are freshly duped from
/// the statement columns. display_text is independently allocated via
/// allocPrint. deinit frees every field symmetrically.
pub fn queryAuditLog(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: AuditEntityFilter,
) ![]AuditLogRow {
    var rows: std.ArrayList(AuditLogRow) = .empty;
    errdefer {
        for (rows.items) |r| r.deinit(allocator);
        rows.deinit(allocator);
    }

    switch (filter) {
        .all => {
            var stmt = try d.prepare(
                \\select id, coalesce(verb,''), coalesce(entity_kind,''), entity_id,
                \\       actor, scope, summary, coalesce(recorded_at,'')
                \\from audit_log
                \\order by recorded_at desc, id desc
            );
            defer stmt.finalize();
            try stmt.bind(&.{});
            while (true) {
                switch (try stmt.step()) {
                    .done => break,
                    .row => {
                        const row = try readAuditLogRow(&stmt, allocator);
                        errdefer row.deinit(allocator);
                        try rows.append(allocator, row);
                    },
                }
            }
        },
        .entity => |e| {
            var stmt = try d.prepare(
                \\select id, coalesce(verb,''), coalesce(entity_kind,''), entity_id,
                \\       actor, scope, summary, coalesce(recorded_at,'')
                \\from audit_log
                \\where entity_kind = ? and entity_id = ?
                \\order by recorded_at desc, id desc
            );
            defer stmt.finalize();
            try stmt.bind(&.{ .{ .text = e.kind }, .{ .int = e.id } });
            while (true) {
                switch (try stmt.step()) {
                    .done => break,
                    .row => {
                        const row = try readAuditLogRow(&stmt, allocator);
                        errdefer row.deinit(allocator);
                        try rows.append(allocator, row);
                    },
                }
            }
        },
    }

    return rows.toOwnedSlice(allocator);
}

/// Read one AuditLogRow from the current statement row (called while stmt
/// is in the `.row` state). Takes `*db.sqlite.Stmt` so column methods
/// receive a mutable receiver, matching the Stmt API.
fn readAuditLogRow(
    stmt: *db.sqlite.Stmt,
    allocator: std.mem.Allocator,
) !AuditLogRow {
    const id = stmt.columnInt(0);

    const verb = try stmt.columnTextAlloc(1, allocator);
    errdefer allocator.free(verb);

    const entity_kind = try stmt.columnTextAlloc(2, allocator);
    errdefer allocator.free(entity_kind);

    const entity_id = stmt.columnInt(3);

    const actor: ?[]const u8 = if (stmt.columnIsNull(4)) null else try stmt.columnTextAlloc(4, allocator);
    errdefer if (actor) |s| allocator.free(s);

    const scope: ?[]const u8 = if (stmt.columnIsNull(5)) null else try stmt.columnTextAlloc(5, allocator);
    errdefer if (scope) |s| allocator.free(s);

    const summary: ?[]const u8 = if (stmt.columnIsNull(6)) null else try stmt.columnTextAlloc(6, allocator);
    errdefer if (summary) |s| allocator.free(s);

    const recorded_at = try stmt.columnTextAlloc(7, allocator);
    errdefer allocator.free(recorded_at);

    // Build pre-formatted display_text independently; no alias into any
    // field above. Format: "[verb] entity_kind:entity_id  actor  ts_short"
    // ts_short = first 19 chars of recorded_at (YYYY-MM-DDTHH:MM:SS).
    const ts_short = if (recorded_at.len >= 19) recorded_at[0..19] else recorded_at;
    const actor_display: []const u8 = actor orelse "\u{2014}";
    const display_text = try std.fmt.allocPrint(
        allocator,
        "[{s}] {s}:{d}  {s}  {s}",
        .{ verb, entity_kind, entity_id, actor_display, ts_short },
    );
    errdefer allocator.free(display_text);

    // Build pre-formatted entity_ref ("entity_kind:entity_id") independently.
    // MEMORY GUARD (rule (c)): used by renderDetail instead of a stack-local
    // format buffer so grapheme pointers remain valid after the render function
    // returns (stack buffers dangle; heap slices are stable).
    const entity_ref = try std.fmt.allocPrint(
        allocator,
        "{s}:{d}",
        .{ entity_kind, entity_id },
    );
    errdefer allocator.free(entity_ref);

    return .{
        .id = id,
        .verb = verb,
        .entity_kind = entity_kind,
        .entity_id = entity_id,
        .actor = actor,
        .scope = scope,
        .summary = summary,
        .recorded_at = recorded_at,
        .display_text = display_text,
        .entity_ref = entity_ref,
    };
}

/// Query all unique (entity_kind, entity_id) pairs present in audit_log,
/// sorted by entity_kind ASC then entity_id ASC. Used by the filter-cycle
/// affordance in the Audit Log view so the operator can tab through entities.
/// Returns a heap-allocated slice of (kind, id) tuples; caller must free.
pub const AuditEntityRef = struct {
    kind: []const u8,
    id: i64,

    pub fn deinit(self: AuditEntityRef, allocator: std.mem.Allocator) void {
        allocator.free(self.kind);
    }

    pub fn deinitMany(refs: []AuditEntityRef, allocator: std.mem.Allocator) void {
        for (refs) |r| r.deinit(allocator);
        allocator.free(refs);
    }
};

/// Query distinct entity refs in audit_log (for filter cycling).
pub fn queryAuditEntities(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]AuditEntityRef {
    var refs: std.ArrayList(AuditEntityRef) = .empty;
    errdefer {
        for (refs.items) |r| r.deinit(allocator);
        refs.deinit(allocator);
    }

    var stmt = try d.prepare(
        \\select distinct entity_kind, entity_id
        \\from audit_log
        \\order by entity_kind asc, entity_id asc
    );
    defer stmt.finalize();
    try stmt.bind(&.{});
    while (true) {
        const step = try stmt.step();
        if (step == .done) break;
        const kind = try stmt.columnTextAlloc(0, allocator);
        errdefer allocator.free(kind);
        const eid = stmt.columnInt(1);
        try refs.append(allocator, .{ .kind = kind, .id = eid });
    }

    return refs.toOwnedSlice(allocator);
}

// =========================================================================
// Audit Log view-model tests (task 4035, 4036)
// =========================================================================

fn setupTestDbAudit(a: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, a);
    return d;
}

test "view_model: queryAuditLog on empty DB returns empty slice (task 4035 empty)" {
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    const rows = try queryAuditLog(&d, a, .all);
    defer AuditLogRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryAuditLog returns rows newest-first (task 4035)" {
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 1, 'alice', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('update', 'task', 1, 'bob', '2025-06-01T00:00:00.000Z')",
        &.{},
    );

    const rows = try queryAuditLog(&d, a, .all);
    defer AuditLogRow.deinitMany(rows, a);

    // Two rows, newest-first.
    try testing.expectEqual(@as(usize, 2), rows.len);
    try testing.expectEqualStrings("update", rows[0].verb);
    try testing.expectEqualStrings("create", rows[1].verb);
}

test "view_model: queryAuditLog row fields populated (task 4035)" {
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, scope, summary, recorded_at) values ('status_change', 'task', 42, 'claude', 'project/myrepo', 'moved to doing', '2026-01-15T10:30:00.000Z')",
        &.{},
    );

    const rows = try queryAuditLog(&d, a, .all);
    defer AuditLogRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    const r = rows[0];
    try testing.expectEqualStrings("status_change", r.verb);
    try testing.expectEqualStrings("task", r.entity_kind);
    try testing.expectEqual(@as(i64, 42), r.entity_id);
    try testing.expect(r.actor != null);
    try testing.expectEqualStrings("claude", r.actor.?);
    try testing.expect(r.scope != null);
    try testing.expectEqualStrings("project/myrepo", r.scope.?);
    try testing.expect(r.summary != null);
    try testing.expectEqualStrings("moved to doing", r.summary.?);
    // Timestamp must start with 2026.
    try testing.expect(std.mem.startsWith(u8, r.recorded_at, "2026"));
    // display_text must contain verb, entity_kind, entity_id, actor, and ts.
    try testing.expect(std.mem.indexOf(u8, r.display_text, "status_change") != null);
    try testing.expect(std.mem.indexOf(u8, r.display_text, "task") != null);
    try testing.expect(std.mem.indexOf(u8, r.display_text, "42") != null);
    try testing.expect(std.mem.indexOf(u8, r.display_text, "claude") != null);
}

test "view_model: queryAuditLog entity filter narrows rows (task 4036)" {
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 10, 'alice', '2026-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('update', 'task', 10, 'alice', '2026-01-02T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'plan', 5, 'bob', '2026-01-03T00:00:00.000Z')",
        &.{},
    );

    // Filter to task:10 — should return 2 rows.
    const filtered = try queryAuditLog(&d, a, .{ .entity = .{ .kind = "task", .id = 10 } });
    defer AuditLogRow.deinitMany(filtered, a);
    try testing.expectEqual(@as(usize, 2), filtered.len);
    for (filtered) |r| {
        try testing.expectEqualStrings("task", r.entity_kind);
        try testing.expectEqual(@as(i64, 10), r.entity_id);
    }

    // Filter to plan:5 — should return 1 row.
    const plan_rows = try queryAuditLog(&d, a, .{ .entity = .{ .kind = "plan", .id = 5 } });
    defer AuditLogRow.deinitMany(plan_rows, a);
    try testing.expectEqual(@as(usize, 1), plan_rows.len);
    try testing.expectEqualStrings("plan", plan_rows[0].entity_kind);

    // All — should return 3 rows.
    const all_rows = try queryAuditLog(&d, a, .all);
    defer AuditLogRow.deinitMany(all_rows, a);
    try testing.expectEqual(@as(usize, 3), all_rows.len);
}

test "view_model: queryAuditEntities returns distinct entity refs (task 4036)" {
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 1, 'alice', '2026-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('update', 'task', 1, 'alice', '2026-01-02T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'plan', 5, 'bob', '2026-01-03T00:00:00.000Z')",
        &.{},
    );

    const refs = try queryAuditEntities(&d, a);
    defer AuditEntityRef.deinitMany(refs, a);

    // Two distinct (kind,id) pairs: plan:5 and task:1 (sorted by kind asc).
    try testing.expectEqual(@as(usize, 2), refs.len);
    try testing.expectEqualStrings("plan", refs[0].kind);
    try testing.expectEqual(@as(i64, 5), refs[0].id);
    try testing.expectEqualStrings("task", refs[1].kind);
    try testing.expectEqual(@as(i64, 1), refs[1].id);
}

test "view_model: AuditEntityFilter label all returns expected string" {
    var buf: [64]u8 = undefined;
    const f: AuditEntityFilter = .all;
    const lbl = f.label(&buf);
    try testing.expectEqualStrings("all entities", lbl);
}

test "view_model: AuditEntityFilter label entity returns kind:id" {
    var buf: [64]u8 = undefined;
    const f = AuditEntityFilter{ .entity = .{ .kind = "task", .id = 42 } };
    const lbl = f.label(&buf);
    try testing.expect(std.mem.indexOf(u8, lbl, "task") != null);
    try testing.expect(std.mem.indexOf(u8, lbl, "42") != null);
}

test "view_model: AuditLogRow deinit handles null optional fields" {
    // Verify that a row with null actor/scope/summary deinit cleanly.
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    // Insert with nulls — no actor, no scope, no summary.
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, recorded_at) values ('delete', 'artifact', 99, '2026-03-01T00:00:00.000Z')",
        &.{},
    );

    const rows = try queryAuditLog(&d, a, .all);
    defer AuditLogRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expect(rows[0].actor == null);
    try testing.expect(rows[0].scope == null);
    try testing.expect(rows[0].summary == null);
    // display_text must use "—" placeholder when actor is null.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "\u{2014}") != null);
}

test "view_model compiles" {
    std.testing.refAllDecls(@This());
}
