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
        allocator.free(self.last_heartbeat_at);
        allocator.free(self.lease_expires_at);
        if (self.latest_action_summary) |s| allocator.free(s);
    }

    pub fn deinitMany(rows: []ClaimRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
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
    /// Depth in the tree (0 = root plan).
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

fn planStatusBadge(status_text: []const u8) StatusBadge {
    if (std.mem.eql(u8, status_text, "draft")) return .draft;
    if (std.mem.eql(u8, status_text, "active")) return .active;
    if (std.mem.eql(u8, status_text, "paused")) return .paused;
    if (std.mem.eql(u8, status_text, "done")) return .done;
    if (std.mem.eql(u8, status_text, "abandoned")) return .abandoned;
    return .none;
}

fn taskStatusBadge(status_text: []const u8) StatusBadge {
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
        \\select p.id, p.title, p.slug, p.status,
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
                const task_count: u32 = @intCast(@max(0, stmt.columnInt(4)));
                const done_count: u32 = @intCast(@max(0, stmt.columnInt(5)));

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .slug = slug,
                    .status_badge = planStatusBadge(status_text),
                    .task_count = task_count,
                    .done_count = done_count,
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

test "view_model compiles" {
    std.testing.refAllDecls(@This());
}
