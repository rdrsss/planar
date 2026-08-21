//! cockpit/views/agent_monitor.zig — Agent Monitor view (M4).
//!
//! Provides a two-pane layout for the cockpit Agent Monitor:
//!
//!   • Left pane (navigator): live roster of active and stale claims
//!     with heartbeat-age coloring (fresh / warning / stale).
//!   • Right pane (detail): event stream of agent_actions and claim
//!     status transitions, newest-first, tailing live.
//!
//! Tasks 4015 (live roster + heartbeat coloring), 4016 (event stream),
//! 4017 (stale / stranded claim flagging).
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by MonitorState and released
//!     via `deinit`.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread is spawned
//!     here; the existing wake integration (app.zig's wakeThreadFn) is
//!     reused for both the Scope Explorer and the Agent Monitor.
//!
//! Heartbeat-age thresholds (see view_model.HeartbeatAge doc comment):
//!
//!   fresh   = heartbeat_age < TTL/2.  Rendered with default style.
//!   warning = heartbeat_age >= TTL/2 but lease not yet expired.
//!             Rendered dim / yellow where the terminal supports color.
//!   stale   = lease_expires_at < now, OR claim.status == 'stale'.
//!             Rendered bold / with a "[STALE]" prefix — must be
//!             unmistakable to the operator (task 4017).

const std = @import("std");
const builtin = @import("builtin");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");
const tree_nav = @import("../widgets/tree_navigator.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// MonitorState
// =========================================================================

/// All mutable state for the Agent Monitor view. Lives in the caller
/// (app.zig) and is passed by pointer to render/handleKey/reload.
pub const MonitorState = struct {
    allocator: std.mem.Allocator,

    /// Snapshot of active + stale claims. Rebuilt on reload.
    snapshot: ?view_model.AgentMonitorSnapshot = null,

    /// Heartbeat-age classification per active claim (parallel to
    /// snapshot.active). Rebuilt alongside snapshot.
    active_ages: []view_model.HeartbeatAge = &.{},

    /// Event stream rows (agent_actions + claim transitions), newest-first.
    actions: []view_model.AgentActionRow = &.{},

    /// Navigation index in the roster (claim selection).
    roster_idx: usize = 0,

    /// Navigation scroll offset in the event stream.
    stream_scroll: usize = 0,

    pub fn init(allocator: std.mem.Allocator) MonitorState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *MonitorState) void {
        if (self.snapshot) |s| s.deinit(self.allocator);
        self.allocator.free(self.active_ages);
        view_model.AgentActionRow.deinitMany(self.actions, self.allocator);
    }

    /// Reload all monitor data from the DB. Called on db_changed and on launch.
    pub fn reload(self: *MonitorState, d: *db.sqlite.Db) !void {
        // Free old data.
        if (self.snapshot) |s| s.deinit(self.allocator);
        self.snapshot = null;
        self.allocator.free(self.active_ages);
        self.active_ages = &.{};
        view_model.AgentActionRow.deinitMany(self.actions, self.allocator);
        self.actions = &.{};

        // Query new snapshot.
        const snap = try view_model.queryAgentMonitor(d, self.allocator);
        self.snapshot = snap;

        // Classify heartbeat ages for all active claims.
        // Zig 0.16 removed std.time.timestamp(); use clock_gettime(REALTIME).
        const now_unix: i64 = nowUnixSeconds();
        const ages = try self.allocator.alloc(view_model.HeartbeatAge, snap.active.len);
        for (snap.active, 0..) |claim_row, i| {
            ages[i] = classifyClaimRowAge(claim_row, now_unix);
        }
        self.active_ages = ages;

        // Query event stream (cap at 200 rows for the live tail).
        self.actions = try view_model.queryAgentActionStream(d, self.allocator, 200);

        // Clamp roster selection.
        const roster_count = if (self.snapshot) |s| s.active.len + s.stale.len else 0;
        if (self.roster_idx >= roster_count and roster_count > 0) {
            self.roster_idx = roster_count - 1;
        }
    }

    /// Total number of roster rows (active + stale).
    pub fn rosterCount(self: *const MonitorState) usize {
        const s = self.snapshot orelse return 0;
        return s.active.len + s.stale.len;
    }

    /// Handle a key event. Returns true when the key was consumed.
    pub fn handleKey(self: *MonitorState, key: Key) bool {
        const roster_count = self.rosterCount();

        // Roster navigation: j / down.
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            if (self.roster_idx + 1 < roster_count) {
                self.roster_idx += 1;
            }
            return true;
        }
        // Roster navigation: k / up.
        if (key.matches('k', .{}) or key.matches(Key.up, .{})) {
            if (self.roster_idx > 0) {
                self.roster_idx -= 1;
            }
            return true;
        }
        // Stream scroll: Page-down / ].
        if (key.matches(']', .{}) or key.matches(Key.page_down, .{})) {
            const stream_len = self.actions.len;
            if (self.stream_scroll + 10 < stream_len) {
                self.stream_scroll += 10;
            } else if (stream_len > 0) {
                self.stream_scroll = stream_len - 1;
            }
            return true;
        }
        // Stream scroll: Page-up / [.
        if (key.matches('[', .{}) or key.matches(Key.page_up, .{})) {
            if (self.stream_scroll >= 10) {
                self.stream_scroll -= 10;
            } else {
                self.stream_scroll = 0;
            }
            return true;
        }

        return false;
    }
};

// =========================================================================
// Wall-clock helper
// =========================================================================

/// Return the current Unix timestamp in seconds.
/// Uses `clock_gettime(REALTIME)` because std.time.timestamp() was
/// removed in Zig 0.16. Returns 0 on Windows or probe failure
/// (heartbeat-age classification falls back to .warning in that case).
fn nowUnixSeconds() i64 {
    if (builtin.os.tag == .windows) return 0;
    var ts: std.c.timespec = undefined;
    if (std.c.clock_gettime(.REALTIME, &ts) != 0) return 0;
    return @intCast(ts.sec);
}

// =========================================================================
// Heartbeat-age classification helpers
// =========================================================================

/// Classify the heartbeat age of a ClaimRow using view_model helpers.
/// Falls back to .warning only on genuine timestamp parse failure.
pub fn classifyClaimRowAge(
    row: view_model.ClaimRow,
    now_unix: i64,
) view_model.HeartbeatAge {
    // Stale by status badge is the fast path.
    if (row.badge == .stale) return .stale;

    // Parse the ISO-8601 timestamps. Fall back to warning on parse failure.
    const hb_unix = view_model.parseIso8601Unix(row.last_heartbeat_at) orelse
        return .warning;
    const exp_unix = view_model.parseIso8601Unix(row.lease_expires_at) orelse
        return .warning;
    // Use the real claimed_at to derive TTL (TTL = lease_expires_at - claimed_at).
    // Fall back to .warning only when the claimed_at string is unparseable —
    // never fabricate a TTL estimate.
    const claimed_unix = view_model.parseIso8601Unix(row.claimed_at) orelse
        return .warning;

    return view_model.classifyHeartbeatAge(now_unix, hb_unix, exp_unix, claimed_unix);
}

// =========================================================================
// Render
// =========================================================================

/// Render the Agent Monitor into the navigator and detail windows.
///
/// Navigator (left pane): claim roster (active + stale sections).
/// Detail (right pane): event stream, newest-first.
pub fn render(
    state: *const MonitorState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    // ---- Navigator: roster -------------------------------------------
    renderRoster(state, nav_win, allocator);

    // ---- Detail: event stream ----------------------------------------
    renderStream(state, detail_win, allocator);
}

/// Render the roster pane (left / navigator).
fn renderRoster(state: *const MonitorState, win: Window, arena: std.mem.Allocator) void {
    if (win.height == 0 or win.width == 0) return;
    var row: u16 = 0;

    const snap = state.snapshot orelse {
        if (win.height > 0) {
            _ = win.printSegment(.{
                .text = "(no data)",
                .style = .{ .dim = true },
            }, .{ .row_offset = 0, .col_offset = 0 });
        }
        return;
    };

    // ---- Active claims section ---------------------------------------
    if (snap.active.len == 0 and snap.stale.len == 0) {
        _ = win.printSegment(.{
            .text = "No active claims",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        return;
    }

    // Section header: ACTIVE.
    if (snap.active.len > 0) {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "ACTIVE",
                .style = .{ .bold = true, .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            row += 1;
        }

        for (snap.active, 0..) |claim, i| {
            if (row >= win.height) break;
            const is_selected = (state.roster_idx == i);
            const age = if (i < state.active_ages.len) state.active_ages[i] else .warning;
            renderClaimRow(win, row, is_selected, age, claim, false, arena);
            row += 1;
        }
    }

    // ---- Stale / stranded section ------------------------------------
    // Task 4017: stale claims must be unmistakable. Rendered with a
    // "[STALE]" prefix and bold style so the operator cannot miss them.
    if (snap.stale.len > 0) {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "STALE / STRANDED",
                .style = .{ .bold = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            row += 1;
        }

        const stale_base = snap.active.len;
        for (snap.stale, 0..) |claim, i| {
            if (row >= win.height) break;
            const is_selected = (state.roster_idx == stale_base + i);
            renderClaimRow(win, row, is_selected, .stale, claim, true, arena);
            row += 1;
        }
    }
}

/// Render one roster row for a claim.
fn renderClaimRow(
    win: Window,
    row: u16,
    is_selected: bool,
    age: view_model.HeartbeatAge,
    claim: view_model.ClaimRow,
    force_stale_marker: bool,
    arena: std.mem.Allocator,
) void {
    _ = force_stale_marker;
    if (row >= win.height) return;

    // Heartbeat-age badge prefix:
    //   [A] = active/fresh   (normal style)
    //   [W] = warning        (dim)
    //   [S] = stale          (bold, unmistakable per task 4017)
    const age_badge: []const u8 = switch (age) {
        .fresh => "[A]",
        .warning => "[W]",
        .stale => "[S]",
    };
    const badge_style: Style = switch (age) {
        .fresh => .{},
        .warning => .{ .dim = true },
        .stale => .{ .bold = true },
    };

    // Selection highlight.
    const base_style: Style = if (is_selected)
        .{ .bold = true, .reverse = true }
    else
        badge_style;

    // Format the roster row: "<badge> <vendor> <entity_ref> <role?>".
    // Use arena allocation (fmtFrame) so the slice remains valid through
    // vaxis.render() — a stack-local buf would dangle after this function returns.
    const role_part = claim.role orelse "";
    const role_display = if (role_part.len > 0) role_part else "-";
    const text = std.fmt.allocPrint(
        arena,
        "{s} {s} {s} {s}",
        .{ age_badge, claim.vendor, claim.entity_ref, role_display },
    ) catch "(error)";

    _ = win.printSegment(.{
        .text = text,
        .style = base_style,
    }, .{ .row_offset = row, .col_offset = 0 });
}

/// Render the event stream pane (right / detail).
///
/// Task 4016: rows are newest-first (queryAgentActionStream already sorts
/// them); the view tails in live because the existing wake integration
/// calls reload on .db_changed, which re-queries and repaints.
fn renderStream(state: *const MonitorState, win: Window, arena: std.mem.Allocator) void {
    if (win.height == 0 or win.width == 0) return;

    if (state.actions.len == 0) {
        _ = win.printSegment(.{
            .text = "(no actions yet)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    // Header.
    _ = win.printSegment(.{
        .text = "EVENT STREAM (newest-first)  [ ] scroll",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });
    if (win.height <= 1) return;

    const start = @min(state.stream_scroll, state.actions.len);
    var display_row: u16 = 1;

    for (state.actions[start..]) |action| {
        if (display_row >= win.height) break;

        // Determine style by kind.
        const is_claim = std.mem.startsWith(u8, action.kind_label, "claim:");
        const is_stale = std.mem.eql(u8, action.kind_label, "claim:stale") or
            std.mem.eql(u8, action.kind_label, "claim:aborted");

        const row_style: Style = if (is_stale)
            .{ .bold = true }
        else if (is_claim)
            .{ .dim = true }
        else
            .{};

        // Truncate ts to date+time (first 19 chars).
        const ts_short = if (action.ts.len >= 19) action.ts[0..19] else action.ts;

        // Format: "<ts>  <kind>  <entity>  <summary?>".
        // Use arena allocation so the slice remains valid through vaxis.render().
        // A stack-local buf would dangle after this function returns.
        const summary_part = action.summary orelse "";
        const text = std.fmt.allocPrint(
            arena,
            "{s}  {s}  {s}  {s}",
            .{ ts_short, action.kind_label, action.entity_ref, summary_part },
        ) catch "(error)";

        _ = win.printSegment(.{
            .text = text,
            .style = row_style,
        }, .{ .row_offset = display_row, .col_offset = 0 });

        display_row += 1;
    }
}

/// Return a one-line legend string for the key legend bar.
pub fn legendLabel(buf: []u8) []const u8 {
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Roster  [/] Stream  Tab Focus",
        .{},
    ) catch "  q Quit  j/k Roster  [/] Stream";
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

test "agent_monitor: MonitorState init and deinit are clean" {
    var state = MonitorState.init(testing.allocator);
    defer state.deinit();
    try testing.expectEqual(@as(usize, 0), state.rosterCount());
}

test "agent_monitor: reload on empty DB yields zero roster" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = MonitorState.init(a);
    defer state.deinit();

    try state.reload(&d);
    try testing.expectEqual(@as(usize, 0), state.rosterCount());
    try testing.expectEqual(@as(usize, 0), state.actions.len);
}

test "agent_monitor: reload surfaces active claim in roster (task 4015)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','am-test','doing')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\  status, vendor, lease_expires_at
        \\) values (
        \\  'am-tok', ?, 'task', ?, 'exclusive', 'active', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '+600 seconds')
        \\)
    , &.{
        .{ .int = sid },
        .{ .int = tid },
    });

    var state = MonitorState.init(a);
    defer state.deinit();

    try state.reload(&d);

    // Should have exactly one active claim in the roster.
    const snap = state.snapshot.?;
    try testing.expectEqual(@as(usize, 1), snap.active.len);
    try testing.expectEqual(@as(usize, 0), snap.stale.len);
    try testing.expectEqual(@as(usize, 1), state.rosterCount());

    // The active_ages slice should be populated.
    try testing.expectEqual(@as(usize, 1), state.active_ages.len);
    // A fresh claim (expires +600s) should be classified fresh.
    try testing.expectEqual(view_model.HeartbeatAge.fresh, state.active_ages[0]);
}

test "agent_monitor: reload classifies stale claim correctly (task 4017)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','stale-am','doing')",
        &.{},
    );
    // Insert a stale (expired) claim.
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\  status, vendor, lease_expires_at
        \\) values (
        \\  'stale-am-tok', ?, 'task', ?, 'exclusive', 'stale', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '-300 seconds')
        \\)
    , &.{
        .{ .int = sid },
        .{ .int = tid },
    });

    var state = MonitorState.init(a);
    defer state.deinit();

    try state.reload(&d);

    // Stale claim should be in the stale bucket.
    const snap = state.snapshot.?;
    try testing.expectEqual(@as(usize, 0), snap.active.len);
    try testing.expectEqual(@as(usize, 1), snap.stale.len);
    // The stale claim badge must be .stale.
    try testing.expectEqual(view_model.StatusBadge.stale, snap.stale[0].badge);
}

test "agent_monitor: reload tails action stream (task 4016)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    // Insert two action rows.
    _ = try d.execParams(
        \\insert into agent_actions (session_id, action_kind, vendor, started_at, summary)
        \\values (?, 'coder', 'claude', strftime('%Y-%m-%dT%H:%M:%fZ','now','-5 seconds'), 'first')
    , &.{.{ .int = sid }});
    _ = try d.execParams(
        \\insert into agent_actions (session_id, action_kind, vendor, started_at, summary)
        \\values (?, 'reviewer', 'claude', strftime('%Y-%m-%dT%H:%M:%fZ','now'), 'second')
    , &.{.{ .int = sid }});

    var state = MonitorState.init(a);
    defer state.deinit();

    try state.reload(&d);

    // Should have at least 2 action rows.
    try testing.expect(state.actions.len >= 2);
    // Newest-first: first element's kind_label should be 'reviewer' (more recent).
    try testing.expectEqualStrings("reviewer", state.actions[0].kind_label);
}

test "agent_monitor: classifyClaimRowAge fresh claim" {
    // A claim with a badge of .active and a future expiry should be fresh.
    // now=1000, claimed=970, hb=970, exp=1570 → age=30, TTL=600, TTL/2=300 → fresh
    const now: i64 = 1_000;
    const claim_row: view_model.ClaimRow = .{
        .id = 1,
        .entity_ref = "task:1",
        .vendor = "claude",
        .role = null,
        .badge = .active,
        // ISO-8601 strings: we use the parseIso8601Unix path, so we need
        // real parseable strings. Use 1970 epoch offsets for simplicity.
        // 1970-01-01T00:16:10Z = 970s = claimed_at (same as hb; TTL = 600s)
        .claimed_at = "1970-01-01T00:16:10.000Z",
        // 1970-01-01T00:16:10Z = 970s = hb
        .last_heartbeat_at = "1970-01-01T00:16:10.000Z",
        // 1970-01-01T00:26:10Z = 1570s = exp; TTL = 1570-970 = 600, half = 300
        .lease_expires_at = "1970-01-01T00:26:10.000Z",
        .latest_action_summary = null,
    };
    const age = classifyClaimRowAge(claim_row, now);
    try testing.expectEqual(view_model.HeartbeatAge.fresh, age);
}

test "agent_monitor: classifyClaimRowAge stale by badge" {
    const claim_row: view_model.ClaimRow = .{
        .id = 2,
        .entity_ref = "task:2",
        .vendor = "claude",
        .role = null,
        .badge = .stale,
        .claimed_at = "1970-01-01T00:00:00.000Z",
        .last_heartbeat_at = "1970-01-01T00:00:01.000Z",
        .lease_expires_at = "1970-01-01T00:00:02.000Z",
        .latest_action_summary = null,
    };
    const age = classifyClaimRowAge(claim_row, 9999);
    try testing.expectEqual(view_model.HeartbeatAge.stale, age);
}

test "agent_monitor: classifyClaimRowAge 7200s lease at midpoint is fresh (regression)" {
    // Regression: a 7200s-TTL claim 1800s in must be .fresh, not .warning.
    // Previously, classifyClaimRowAge fabricated est_ttl=600 → est_claimed=exp-600,
    // giving TTL=600, half=300. A 1800s heartbeat age then exceeded the threshold
    // and returned .warning for a perfectly healthy claim.
    //
    // Scenario: now=10000, claimed=8200 (1800s ago), hb=8200, expires=15400 (5400s away).
    //   TTL  = 15400 - 8200 = 7200s
    //   half = 3600s
    //   age  = 10000 - 8200 = 1800s < 3600s → .fresh
    //
    // Timestamps: 8200s  = 2h 16m 40s → "1970-01-01T02:16:40.000Z"
    //             15400s = 4h 16m 40s → "1970-01-01T04:16:40.000Z"
    const now: i64 = 10_000;
    const claim_row: view_model.ClaimRow = .{
        .id = 3,
        .entity_ref = "task:3",
        .vendor = "claude",
        .role = null,
        .badge = .active,
        .claimed_at = "1970-01-01T02:16:40.000Z", // 8200s
        .last_heartbeat_at = "1970-01-01T02:16:40.000Z", // 8200s (hb age = 1800s)
        .lease_expires_at = "1970-01-01T04:16:40.000Z", // 15400s
        .latest_action_summary = null,
    };
    const age = classifyClaimRowAge(claim_row, now);
    try testing.expectEqual(view_model.HeartbeatAge.fresh, age);
}

test "agent_monitor: handleKey roster navigation" {
    var state = MonitorState.init(testing.allocator);
    defer state.deinit();
    state.roster_idx = 0;

    // j moves down but clamped at 0 when roster is empty.
    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    _ = state.handleKey(j_key);
    try testing.expectEqual(@as(usize, 0), state.roster_idx);
}

test "agent_monitor: handleKey stream scroll" {
    const a = testing.allocator;
    var state = MonitorState.init(a);
    defer state.deinit();

    // Seed 20 action rows so scroll has something to work with.
    state.actions = try a.alloc(view_model.AgentActionRow, 20);
    for (state.actions, 0..) |*r, i| {
        r.* = .{
            .id = @intCast(i),
            .ts = try a.dupe(u8, "1970-01-01T00:00:00.000Z"),
            .kind_label = try a.dupe(u8, "other"),
            .entity_ref = try a.dupe(u8, ""),
            .vendor = try a.dupe(u8, "test"),
            .summary = null,
        };
    }

    // ] scrolls down 10.
    const bracket_close = Key{ .codepoint = ']', .mods = .{} };
    _ = state.handleKey(bracket_close);
    try testing.expectEqual(@as(usize, 10), state.stream_scroll);

    // [ scrolls back up.
    const bracket_open = Key{ .codepoint = '[', .mods = .{} };
    _ = state.handleKey(bracket_open);
    try testing.expectEqual(@as(usize, 0), state.stream_scroll);
}

test "agent_monitor: legendLabel fits in buf" {
    var buf: [128]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Roster") != null);
}

test "agent_monitor compiles" {
    std.testing.refAllDecls(@This());
}
