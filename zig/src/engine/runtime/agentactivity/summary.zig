//! agentactivity/summary — compact per-entity activity rollup used by the
//! operator-facing read surfaces (`planar tree`, `planar audit trail`).
//!
//! M5 (plan 85): folds claim + action context into existing operator views
//! without introducing a parallel `planar agent` subcommand namespace.
//! Read-only; never writes. Degrades silently when the queried entity has
//! no activity (`forEntity` returns `null`).
//!
//! Two read paths:
//!   - `forEntity(d, allocator, kind, id)` — compact rollup: latest
//!     `action_kind`, vendor, last_event_at, active-claim count. Returns
//!     `null` when the entity has no `agent_actions` rows AND no
//!     `agent_work_claims` rows (any status). Used by `planar tree`.
//!   - `recentActionsForEntity` / `claimTransitionsForEntity` — full row
//!     slices for the audit-trail "Agent activity" section. Each returns
//!     an empty slice when nothing is recorded.

const std = @import("std");
const db = @import("db");

/// Compact per-entity rollup. Returned via pointer-or-null from
/// `forEntity` — `null` means "no activity, render nothing."
pub const ActivitySummary = struct {
    /// Latest `agent_actions.action_kind` for the entity (any session),
    /// e.g. `coder`, `reviewer`, `orchestrator`. Empty string only when
    /// the entity has claims but no actions yet (unusual; surfaced as-is).
    latest_action_kind: []const u8,
    /// Vendor of the latest action (or active claim when no actions).
    latest_vendor: []const u8,
    /// ISO8601 timestamp of the latest event — either the latest action's
    /// `started_at`/`ended_at` (later of the two) or the latest claim
    /// transition (`claimed_at`/`released_at`).
    last_event_at: []const u8,
    /// Number of currently-active, unexpired claims on the entity.
    active_claim_count: i64,

    pub fn deinit(self: ActivitySummary, allocator: std.mem.Allocator) void {
        allocator.free(self.latest_action_kind);
        allocator.free(self.latest_vendor);
        allocator.free(self.last_event_at);
    }
};

/// One recent agent_actions row for the audit-trail fold-in. The shape is
/// intentionally narrow — just the columns the human-readable trail
/// surfaces — so callers don't pay for unused columns.
pub const ActionRow = struct {
    id: i64,
    session_id: i64,
    action_kind: []const u8,
    vendor: []const u8,
    started_at: []const u8,
    ended_at: ?[]const u8,
    outcome: ?[]const u8,
    summary: ?[]const u8,

    pub fn deinit(self: ActionRow, allocator: std.mem.Allocator) void {
        allocator.free(self.action_kind);
        allocator.free(self.vendor);
        allocator.free(self.started_at);
        if (self.ended_at) |s| allocator.free(s);
        if (self.outcome) |s| allocator.free(s);
        if (self.summary) |s| allocator.free(s);
    }

    pub fn deinitMany(items: []const ActionRow, allocator: std.mem.Allocator) void {
        for (items) |r| r.deinit(allocator);
        allocator.free(items);
    }
};

/// One claim transition row for the audit-trail fold-in. Captures the
/// claim's terminal-or-current state plus when it was acquired.
pub const ClaimTransitionRow = struct {
    id: i64,
    claim_token: []const u8,
    status: []const u8,
    vendor: []const u8,
    role: ?[]const u8,
    claimed_at: []const u8,
    released_at: ?[]const u8,
    release_reason: ?[]const u8,

    pub fn deinit(self: ClaimTransitionRow, allocator: std.mem.Allocator) void {
        allocator.free(self.claim_token);
        allocator.free(self.status);
        allocator.free(self.vendor);
        if (self.role) |s| allocator.free(s);
        allocator.free(self.claimed_at);
        if (self.released_at) |s| allocator.free(s);
        if (self.release_reason) |s| allocator.free(s);
    }

    pub fn deinitMany(items: []const ClaimTransitionRow, allocator: std.mem.Allocator) void {
        for (items) |r| r.deinit(allocator);
        allocator.free(items);
    }
};

pub const Error = error{QueryFailed} || std.mem.Allocator.Error;

/// Return a compact activity rollup for `(entity_kind, entity_id)` or
/// `null` when the entity has neither actions nor claims. Read-only.
///
/// The rollup prefers the latest action row for `latest_action_kind` and
/// `last_event_at`; when there are no actions but there are claims, it
/// falls back to the latest claim's transition time and reports the
/// claim's vendor with `latest_action_kind = ""`.
pub fn forEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_kind: []const u8,
    entity_id: i64,
) Error!?ActivitySummary {
    // Latest action (any session) — joined via the action_kind+vendor
    // path. We use the later of started_at and ended_at as the event
    // timestamp so a finished action shows when it ended, not when it
    // started.
    var latest_action_kind: ?[]const u8 = null;
    errdefer if (latest_action_kind) |s| allocator.free(s);
    var latest_action_vendor: ?[]const u8 = null;
    errdefer if (latest_action_vendor) |s| allocator.free(s);
    var latest_action_at: ?[]const u8 = null;
    errdefer if (latest_action_at) |s| allocator.free(s);

    {
        var stmt = d.prepare(
            \\select action_kind, vendor,
            \\       coalesce(ended_at, started_at) as event_at
            \\from agent_actions
            \\where entity_kind = ? and entity_id = ?
            \\order by event_at desc, id desc
            \\limit 1
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{
            .{ .text = entity_kind },
            .{ .int = entity_id },
        }) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => {},
            .row => {
                latest_action_kind = try stmt.columnTextAlloc(0, allocator);
                latest_action_vendor = try stmt.columnTextAlloc(1, allocator);
                latest_action_at = try stmt.columnTextAlloc(2, allocator);
            },
        }
    }

    // Active-unexpired-claim count.
    var active_count: i64 = 0;
    {
        var stmt = d.prepare(
            \\select count(*) from agent_work_claims
            \\where entity_kind = ? and entity_id = ?
            \\  and status = 'active'
            \\  and lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{
            .{ .text = entity_kind },
            .{ .int = entity_id },
        }) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => {},
            .row => active_count = stmt.columnInt(0),
        }
    }

    // If we have no action AND no claims at all, return null (silent
    // degrade). We check the claim table for any row, not just active —
    // a recently released claim still represents activity worth
    // surfacing.
    if (latest_action_kind == null) {
        var stmt = d.prepare(
            \\select claim_token, vendor,
            \\       coalesce(released_at, claimed_at) as event_at
            \\from agent_work_claims
            \\where entity_kind = ? and entity_id = ?
            \\order by event_at desc, id desc
            \\limit 1
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{
            .{ .text = entity_kind },
            .{ .int = entity_id },
        }) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return null, // no actions and no claims → silent degrade.
            .row => {
                // No action; fall back to the claim's transition info.
                const tok = try stmt.columnTextAlloc(0, allocator);
                defer allocator.free(tok);
                latest_action_kind = try allocator.dupe(u8, "");
                latest_action_vendor = try stmt.columnTextAlloc(1, allocator);
                latest_action_at = try stmt.columnTextAlloc(2, allocator);
            },
        }
    }

    return ActivitySummary{
        .latest_action_kind = latest_action_kind orelse try allocator.dupe(u8, ""),
        .latest_vendor = latest_action_vendor orelse try allocator.dupe(u8, ""),
        .last_event_at = latest_action_at orelse try allocator.dupe(u8, ""),
        .active_claim_count = active_count,
    };
}

/// Return up to `limit` most-recent `agent_actions` rows joined against
/// the entity. Empty slice when nothing is recorded.
pub fn recentActionsForEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_kind: []const u8,
    entity_id: i64,
    limit: i64,
) Error![]ActionRow {
    var sql_buf: [512]u8 = undefined;
    const sql = std.fmt.bufPrintZ(&sql_buf,
        \\select id, session_id, action_kind, vendor,
        \\       started_at, ended_at, outcome, summary
        \\from agent_actions
        \\where entity_kind = ? and entity_id = ?
        \\order by coalesce(ended_at, started_at) desc, id desc
        \\limit {d}
    , .{limit}) catch return Error.QueryFailed;

    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .text = entity_kind },
        .{ .int = entity_id },
    }) catch return Error.QueryFailed;

    var out: std.ArrayList(ActionRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const row = ActionRow{
                    .id = stmt.columnInt(0),
                    .session_id = stmt.columnInt(1),
                    .action_kind = try stmt.columnTextAlloc(2, allocator),
                    .vendor = try stmt.columnTextAlloc(3, allocator),
                    .started_at = try stmt.columnTextAlloc(4, allocator),
                    .ended_at = try stmt.columnTextOpt(5, allocator),
                    .outcome = try stmt.columnTextOpt(6, allocator),
                    .summary = try stmt.columnTextOpt(7, allocator),
                };
                try out.append(allocator, row);
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Return up to `limit` most-recent claim transitions on the entity.
/// Empty slice when no claims exist.
pub fn claimTransitionsForEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_kind: []const u8,
    entity_id: i64,
    limit: i64,
) Error![]ClaimTransitionRow {
    var sql_buf: [512]u8 = undefined;
    const sql = std.fmt.bufPrintZ(&sql_buf,
        \\select id, claim_token, status, vendor, role,
        \\       claimed_at, released_at, release_reason
        \\from agent_work_claims
        \\where entity_kind = ? and entity_id = ?
        \\order by coalesce(released_at, claimed_at) desc, id desc
        \\limit {d}
    , .{limit}) catch return Error.QueryFailed;

    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .text = entity_kind },
        .{ .int = entity_id },
    }) catch return Error.QueryFailed;

    var out: std.ArrayList(ClaimTransitionRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const row = ClaimTransitionRow{
                    .id = stmt.columnInt(0),
                    .claim_token = try stmt.columnTextAlloc(1, allocator),
                    .status = try stmt.columnTextAlloc(2, allocator),
                    .vendor = try stmt.columnTextAlloc(3, allocator),
                    .role = try stmt.columnTextOpt(4, allocator),
                    .claimed_at = try stmt.columnTextAlloc(5, allocator),
                    .released_at = try stmt.columnTextOpt(6, allocator),
                    .release_reason = try stmt.columnTextOpt(7, allocator),
                };
                try out.append(allocator, row);
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Tests
// =========================================================================

const migrate = @import("db").migrate;

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try migrate.applyAll(&d, allocator);
    return d;
}

fn insertSession(d: *db.sqlite.Db) !i64 {
    return try d.execParams(
        "insert into sessions (vendor) values ('test')",
        &.{},
    );
}

fn insertTask(d: *db.sqlite.Db) !i64 {
    return try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','t','todo')",
        &.{},
    );
}

test "summary.forEntity returns null when the entity has no activity" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const tid = try insertTask(&d);

    const got = try forEntity(&d, a, "task", tid);
    try std.testing.expect(got == null);
}

test "summary.forEntity surfaces latest action_kind, vendor, and active-claim count" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertSession(&d);
    const tid = try insertTask(&d);

    _ = try d.execParams(
        \\insert into agent_actions
        \\  (session_id, action_kind, entity_kind, entity_id, vendor)
        \\values (?, 'coder', 'task', ?, 'test')
    , &.{ .{ .int = sid }, .{ .int = tid } });

    _ = try d.execParams(
        \\insert into agent_work_claims
        \\  (claim_token, session_id, entity_kind, entity_id,
        \\   status, vendor,
        \\   lease_expires_at)
        \\values ('tok-summary-test', ?, 'task', ?,
        \\   'active', 'test',
        \\   strftime('%Y-%m-%dT%H:%M:%fZ','now', '+600 seconds'))
    , &.{ .{ .int = sid }, .{ .int = tid } });

    const got = (try forEntity(&d, a, "task", tid)) orelse @panic("expected non-null summary");
    defer got.deinit(a);
    try std.testing.expectEqualStrings("coder", got.latest_action_kind);
    try std.testing.expectEqualStrings("test", got.latest_vendor);
    try std.testing.expectEqual(@as(i64, 1), got.active_claim_count);
    try std.testing.expect(got.last_event_at.len > 0);
}

test "summary.recentActionsForEntity returns rows in event_at desc order" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertSession(&d);
    const tid = try insertTask(&d);

    _ = try d.execParams(
        \\insert into agent_actions
        \\  (session_id, action_kind, entity_kind, entity_id, vendor)
        \\values (?, 'coder', 'task', ?, 'claude')
    , &.{ .{ .int = sid }, .{ .int = tid } });
    _ = try d.execParams(
        \\insert into agent_actions
        \\  (session_id, action_kind, entity_kind, entity_id, vendor)
        \\values (?, 'reviewer', 'task', ?, 'codex')
    , &.{ .{ .int = sid }, .{ .int = tid } });

    const rows = try recentActionsForEntity(&d, a, "task", tid, 5);
    defer ActionRow.deinitMany(rows, a);
    try std.testing.expectEqual(@as(usize, 2), rows.len);
    // The latest insert (reviewer) has the highest id and is returned first.
    try std.testing.expectEqualStrings("reviewer", rows[0].action_kind);
}

test "summary.claimTransitionsForEntity returns empty slice when no claims exist" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const tid = try insertTask(&d);

    const rows = try claimTransitionsForEntity(&d, a, "task", tid, 5);
    defer ClaimTransitionRow.deinitMany(rows, a);
    try std.testing.expectEqual(@as(usize, 0), rows.len);
}
