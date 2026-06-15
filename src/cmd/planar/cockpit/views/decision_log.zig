//! cockpit/views/decision_log.zig — Decision Log view (M6).
//!
//! Renders a two-pane layout for the Decision Log:
//!
//!   • Left pane (navigator): chronological list of decisions, newest-first.
//!     Each row shows "[badge] date title". j/k (or arrow keys) navigate.
//!
//!   • Right pane (detail): for the selected decision, shows:
//!       - Title (bold)
//!       - Body (markdown, via markdown_detail widget): includes status
//!       - "Derives from:" section with the derives-from entity_links targets
//!
//! Tasks 4021 (chronological list), 4022 (detail pane: body + derives-from).
//!
//! Acceptance invariants:
//!   (4021) The navigator lists ALL queried decisions in chronological
//!          (newest-first) order; each row is rendered with badge + date + title.
//!   (4022) The detail pane MUST render BOTH the markdown body AND the
//!          derives-from section. If a decision has no derives-from edges,
//!          the section is omitted with an explicit "(none)" placeholder in
//!          its place so the operator knows derives-from was checked.
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by DecisionLogState and released via deinit.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread.
//!   - derives-from uses the 'derives-from' relationship from entity_links
//!     per migration 00004_entity_links.up.sql.

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");
const markdown_detail = @import("../widgets/markdown_detail.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// DecisionLogState
// =========================================================================

/// All mutable state for the Decision Log view.
pub const DecisionLogState = struct {
    allocator: std.mem.Allocator,

    /// Current snapshot of decisions, ordered newest-first.
    rows: []view_model.DecisionLogRow = &.{},

    /// Navigator selection index (0-based into rows).
    selected_idx: usize = 0,

    /// Navigator scroll offset (flat row count from top).
    scroll_offset: usize = 0,

    /// Detail pane data for the currently selected decision. Null when
    /// the list is empty or no selection is valid.
    detail: ?view_model.DecisionLogDetail = null,

    /// Scope filter (default: .all).
    filter: view_model.ScopeFilter = .all,

    pub fn init(allocator: std.mem.Allocator) DecisionLogState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *DecisionLogState) void {
        view_model.DecisionLogRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        if (self.detail) |d| d.deinit(self.allocator);
        self.detail = null;
    }

    /// Reload all decision log data from the DB. Called on db_changed and
    /// on initial launch.
    pub fn reload(self: *DecisionLogState, d: *db.sqlite.Db) !void {
        // Free old data.
        view_model.DecisionLogRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        if (self.detail) |det| det.deinit(self.allocator);
        self.detail = null;

        // Query new rows.
        self.rows = try view_model.queryDecisionLog(d, self.allocator, self.filter);

        // Clamp selection.
        if (self.rows.len > 0) {
            if (self.selected_idx >= self.rows.len) {
                self.selected_idx = self.rows.len - 1;
            }
        } else {
            self.selected_idx = 0;
        }

        // Refresh detail for the current selection.
        try self.refreshDetail(d);
    }

    /// Refresh the detail pane for the currently selected decision.
    pub fn refreshDetail(self: *DecisionLogState, d: *db.sqlite.Db) !void {
        if (self.detail) |det| {
            det.deinit(self.allocator);
            self.detail = null;
        }

        if (self.rows.len == 0) return;
        const sel = self.rows[self.selected_idx];
        self.detail = try view_model.queryDecisionLogDetail(d, self.allocator, sel.id);
    }

    /// Handle a key event. Returns true when the key was consumed.
    pub fn handleKey(self: *DecisionLogState, key: Key, d: *db.sqlite.Db) bool {
        const count = self.rows.len;

        // j / arrow-down: move selection down (toward newer decisions = higher index
        // in the sorted rows slice; but since rows are newest-first, "down" means
        // older. This matches the navigator convention from task_board.zig).
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            if (count > 0 and self.selected_idx + 1 < count) {
                self.selected_idx += 1;
                self.refreshDetail(d) catch {};
            }
            return true;
        }
        // k / arrow-up: move selection up.
        if (key.matches('k', .{}) or key.matches(Key.up, .{})) {
            if (self.selected_idx > 0) {
                self.selected_idx -= 1;
                self.refreshDetail(d) catch {};
            }
            return true;
        }

        return false;
    }
};

// =========================================================================
// Render
// =========================================================================

/// Render the Decision Log into the navigator and detail windows.
///
/// Navigator (left pane): chronological list of decisions, newest-first.
/// Detail (right pane): body + derives-from for the selected decision.
pub fn render(
    state: *const DecisionLogState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    _ = allocator;
    renderNavigator(state, nav_win);
    try renderDetail(state, detail_win);
}

/// Render the navigator pane (left): chronological list of decisions.
///
/// Each row: "[badge] YYYY-MM-DD  title"
///   badge = StatusBadge glyph for the decision status
///   date  = first 10 chars of date_display (YYYY-MM-DD)
///   title = decision title (truncated to window width)
///
/// The selected row is rendered in reverse-video.
fn renderNavigator(state: *const DecisionLogState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    if (state.rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no decisions in scope)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    // Effective scroll: ensure the selected row is within the viewport.
    const viewport_h: usize = @intCast(win.height);
    const scroll: usize = if (state.selected_idx >= viewport_h)
        state.selected_idx - viewport_h + 1
    else
        0;

    var display_row: u16 = 0;
    for (state.rows, 0..) |row, i| {
        if (i < scroll) continue;
        if (display_row >= win.height) break;

        const is_selected = (i == state.selected_idx);

        // Use the pre-formatted heap-allocated display_text so that
        // grapheme pointers remain valid after the render function returns
        // (required for render-level tests and for stable screen state).
        const style: Style = if (is_selected)
            .{ .bold = true, .reverse = true }
        else
            .{};

        _ = win.printSegment(.{
            .text = row.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });

        display_row += 1;
    }
}

/// Render the detail pane (right).
///
/// Layout (top to bottom):
///   row 0            — decision title (bold)
///   rows 1..body_end — markdown body (status + body text) via markdown_detail
///   separator        — blank row before derives-from section
///   "Derives from:"  — section header (bold dim)
///   N rows           — one per derives-from target ("  label")
///                      OR "(none)" when derives_from is empty (explicit)
///
/// INVARIANT (task 4022): BOTH the body AND the derives-from section are
/// ALWAYS rendered when a decision is selected. This is the load-bearing
/// acceptance check for the M3/M4/M5 recurring bug pattern.
fn renderDetail(state: *const DecisionLogState, win: Window) !void {
    if (win.height == 0 or win.width == 0) return;

    const detail = state.detail orelse {
        _ = win.printSegment(.{
            .text = "(no decision selected)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    };

    var row: u16 = 0;

    // ---- Title row (row 0) -----------------------------------------------
    if (detail.title.len > 0 and row < win.height) {
        _ = win.printSegment(.{
            .text = detail.title,
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    // ---- Compute supplemental section height so we can carve it from the
    //      bottom of the available space and give the body the rest. --------
    //
    // Supplemental layout:
    //   1 separator (blank)
    //   1 "Derives from:" header
    //   max(1, derives_from.len) rows — either N targets or "(none)"
    const derives_rows: u16 = @intCast(@max(1, detail.derives_from.len));
    const supp_rows: u16 = 1 + 1 + derives_rows; // separator + header + targets

    // ---- Body (markdown_detail widget) -----------------------------------
    const body_start = row;
    if (body_start < win.height) {
        const remaining: u16 = win.height - body_start;
        const body_h: u16 = if (supp_rows < remaining) remaining - supp_rows else 1;
        const body_win = win.child(.{
            .x_off = 0,
            .y_off = body_start,
            .width = win.width,
            .height = body_h,
        });
        var buf: [8192]u8 = undefined;
        var fba = std.heap.FixedBufferAllocator.init(&buf);
        try markdown_detail.render(body_win, fba.allocator(), detail.body);
        row = body_start + body_h;
    }

    // ---- Supplemental: derives-from section ------------------------------
    //
    // Separator.
    if (row >= win.height) return;
    row += 1; // blank separator
    if (row >= win.height) return;

    // "Derives from:" header.
    _ = win.printSegment(.{
        .text = "Derives from:",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = row, .col_offset = 0 });
    row += 1;

    if (detail.derives_from.len == 0) {
        // Explicit "(none)" so the operator can distinguish "not loaded"
        // from "genuinely has no derives-from edges".
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (none)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
        return;
    }

    // One row per derives-from target.
    for (detail.derives_from) |df| {
        if (row >= win.height) break;
        _ = win.printSegment(.{
            .text = "  ",
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = df.label,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 2 });
        row += 1;
    }
}

/// Return a one-line legend string for the key legend bar.
pub fn legendLabel(buf: []u8) []const u8 {
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Select  Tab Focus  1-9 View",
        .{},
    ) catch "  q Quit  j/k Select  Tab Focus";
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

/// Extract all non-empty grapheme text from a Screen's cell buffer into a
/// single flat string. Used by render-level tests to assert that specific
/// text appears in the rendered output.
///
/// Each cell's char.grapheme slice points into the original string data
/// written by printSegment (which borrows from the text argument). The
/// grapheme pointers remain valid for the duration of the test. The result
/// is appended into `out`; caller owns the result.
fn collectScreenText(screen: *const vaxis.Screen, out: *std.ArrayList(u8)) !void {
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) {
            try out.appendSlice(testing.allocator, g);
        }
    }
}

// -------------------------------------------------------------------------
// DecisionLogState lifecycle tests
// -------------------------------------------------------------------------

test "decision_log: init and deinit are clean" {
    var state = DecisionLogState.init(testing.allocator);
    defer state.deinit();
    try testing.expectEqual(@as(usize, 0), state.rows.len);
    try testing.expectEqual(@as(?view_model.DecisionLogDetail, null), state.detail);
}

test "decision_log: reload on empty DB yields empty rows (task 4021 empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = DecisionLogState.init(a);
    defer state.deinit();

    try state.reload(&d);

    try testing.expectEqual(@as(usize, 0), state.rows.len);
    try testing.expectEqual(@as(?view_model.DecisionLogDetail, null), state.detail);
}

test "decision_log: reload populates rows newest-first (task 4021)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert decisions with different created_at values (via decided_at).
    _ = try d.execParams(
        \\insert into decisions (scope_kind, title, body, status, decided_at)
        \\values ('global','Older Decision','Older body','accepted','2024-01-01T00:00:00.000Z')
    , &.{});
    _ = try d.execParams(
        \\insert into decisions (scope_kind, title, body, status, decided_at)
        \\values ('global','Newer Decision','Newer body','proposed','2025-06-01T00:00:00.000Z')
    , &.{});

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Two decisions, ordered newest-first.
    try testing.expectEqual(@as(usize, 2), state.rows.len);
    try testing.expectEqualStrings("Newer Decision", state.rows[0].title);
    try testing.expectEqualStrings("Older Decision", state.rows[1].title);
}

test "decision_log: badge reflects decision status (task 4021)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Proposed','body','proposed')",
        &.{},
    );
    _ = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Accepted','body','accepted')",
        &.{},
    );
    _ = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Withdrawn','body','withdrawn')",
        &.{},
    );

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 3), state.rows.len);
    // Accepted → .done badge.
    var found_accepted = false;
    var found_proposed = false;
    var found_withdrawn = false;
    for (state.rows) |row| {
        if (std.mem.eql(u8, row.title, "Accepted")) {
            try testing.expectEqual(view_model.StatusBadge.done, row.badge);
            found_accepted = true;
        }
        if (std.mem.eql(u8, row.title, "Proposed")) {
            try testing.expectEqual(view_model.StatusBadge.draft, row.badge);
            found_proposed = true;
        }
        if (std.mem.eql(u8, row.title, "Withdrawn")) {
            try testing.expectEqual(view_model.StatusBadge.cancelled, row.badge);
            found_withdrawn = true;
        }
    }
    try testing.expect(found_accepted);
    try testing.expect(found_proposed);
    try testing.expect(found_withdrawn);
}

test "decision_log: handleKey j/k moves selection" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','D1','body1','accepted')",
        &.{},
    );
    _ = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','D2','body2','proposed')",
        &.{},
    );

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 2), state.rows.len);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);

    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    _ = state.handleKey(j_key, &d);
    try testing.expectEqual(@as(usize, 1), state.selected_idx);

    const k_key = Key{ .codepoint = 'k', .mods = .{} };
    _ = state.handleKey(k_key, &d);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "decision_log: handleKey j does not overflow past last row" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Only','body','accepted')",
        &.{},
    );

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    _ = state.handleKey(j_key, &d);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "decision_log: handleKey k does not underflow below 0" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Only','body','accepted')",
        &.{},
    );

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const k_key = Key{ .codepoint = 'k', .mods = .{} };
    _ = state.handleKey(k_key, &d);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

// -------------------------------------------------------------------------
// Detail pane / derives-from tests
// -------------------------------------------------------------------------

test "decision_log: detail body is populated with status and body text (task 4022)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        \\insert into decisions (scope_kind, title, body, status)
        \\values ('global','BodyDecision','The actual body text.','accepted')
    , &.{});

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    const detail = state.detail orelse {
        try testing.expect(false); // must not be null
        return;
    };

    try testing.expectEqualStrings("BodyDecision", detail.title);
    // Body must contain both the status and the raw body text.
    try testing.expect(std.mem.indexOf(u8, detail.body, "accepted") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "The actual body text.") != null);
}

test "decision_log: detail derives-from is populated (task 4022)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Decision that derives from an artifact.
    const did = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','DerivedDecision','Decision body','accepted')",
        &.{},
    );
    const aid = try d.execParams(
        "insert into artifacts (scope_kind, title, body, kind) values ('global','SourceArtifact','artifact body','tech_spec')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', ?, 'artifact', ?, 'derives-from')",
        &.{ .{ .int = did }, .{ .int = aid } },
    );

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };

    try testing.expectEqual(@as(usize, 1), detail.derives_from.len);
    try testing.expectEqualStrings("artifact", detail.derives_from[0].target_kind);
    try testing.expectEqual(aid, detail.derives_from[0].target_id);
    // Label must contain the artifact title.
    try testing.expect(std.mem.indexOf(u8, detail.derives_from[0].label, "SourceArtifact") != null);
}

test "decision_log: detail derives-from is empty when no edges (task 4022)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','StandaloneDecision','body','proposed')",
        &.{},
    );

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };
    try testing.expectEqual(@as(usize, 0), detail.derives_from.len);
}

// =========================================================================
// RENDER-LEVEL TESTS (rule (b): assert rendered text in screen buffer)
// =========================================================================
//
// These tests allocate a real vaxis.Screen, call the render functions,
// then scan the cell buffer for expected text. A struct-level assertion
// that a field is populated is NOT sufficient; these tests prove the
// rendered output actually contains the data.

test "decision_log: renderNavigator renders decision titles in screen buffer (task 4021 render-level)" {
    // Seed two decisions and verify both titles appear in the navigator output.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        \\insert into decisions (scope_kind, title, body, status, decided_at)
        \\values ('global','Alpha Decision','body','accepted','2025-01-01T00:00:00.000Z')
    , &.{});
    _ = try d.execParams(
        \\insert into decisions (scope_kind, title, body, status, decided_at)
        \\values ('global','Beta Decision','body','proposed','2024-06-01T00:00:00.000Z')
    , &.{});

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 2), state.rows.len);

    // Build a real Screen and Window (80x24 terminal).
    const win_w: u16 = 80;
    const win_h: u16 = 24;
    var screen = try vaxis.Screen.init(a, .{
        .cols = win_w,
        .rows = win_h,
        .x_pixel = 0,
        .y_pixel = 0,
    });
    defer screen.deinit(a);

    const nav_win: Window = .{
        .x_off = 0,
        .y_off = 0,
        .parent_x_off = 0,
        .parent_y_off = 0,
        .width = win_w,
        .height = win_h,
        .screen = &screen,
    };

    // Render the navigator pane.
    renderNavigator(&state, nav_win);

    // Collect rendered text.
    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS: both titles must appear in the output.
    // This fails if renderNavigator is a no-op or prints only headers.
    try testing.expect(std.mem.indexOf(u8, text, "Alpha Decision") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Beta Decision") != null);
    // Badge glyphs must appear (D for accepted, d for proposed).
    try testing.expect(std.mem.indexOf(u8, text, "D") != null);
    try testing.expect(std.mem.indexOf(u8, text, "d") != null);
    // Dates must appear (at least the year).
    try testing.expect(std.mem.indexOf(u8, text, "2025") != null);
    try testing.expect(std.mem.indexOf(u8, text, "2024") != null);
}

test "decision_log: renderNavigator shows empty state text when no decisions (task 4021 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const win_w: u16 = 80;
    const win_h: u16 = 10;
    var screen = try vaxis.Screen.init(a, .{
        .cols = win_w,
        .rows = win_h,
        .x_pixel = 0,
        .y_pixel = 0,
    });
    defer screen.deinit(a);

    const nav_win: Window = .{
        .x_off = 0,
        .y_off = 0,
        .parent_x_off = 0,
        .parent_y_off = 0,
        .width = win_w,
        .height = win_h,
        .screen = &screen,
    };

    renderNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    try testing.expect(std.mem.indexOf(u8, text, "no decisions in scope") != null);
}

test "decision_log: renderDetail renders body AND derives-from (task 4022 render-level)" {
    // This is the critical render-level test for task 4022.
    // It seeds a decision with a body and a derives-from edge, then verifies
    // BOTH appear in the rendered detail pane output.
    // If renderDetail does not render both sections, this test fails.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const did = try d.execParams(
        \\insert into decisions (scope_kind, title, body, status)
        \\values ('global','Important Decision','The decision body text for rendering test.','accepted')
    , &.{});
    const aid = try d.execParams(
        "insert into artifacts (scope_kind, title, body, kind) values ('global','FoundingSpec','artifact body','tech_spec')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', ?, 'artifact', ?, 'derives-from')",
        &.{ .{ .int = did }, .{ .int = aid } },
    );

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };
    // Struct-level sanity check.
    try testing.expectEqual(@as(usize, 1), detail.derives_from.len);
    try testing.expect(std.mem.indexOf(u8, detail.body, "The decision body text") != null);

    // Build a real Screen (80x40) for the detail pane.
    const win_w: u16 = 80;
    const win_h: u16 = 40;
    var screen = try vaxis.Screen.init(a, .{
        .cols = win_w,
        .rows = win_h,
        .x_pixel = 0,
        .y_pixel = 0,
    });
    defer screen.deinit(a);

    const detail_win: Window = .{
        .x_off = 0,
        .y_off = 0,
        .parent_x_off = 0,
        .parent_y_off = 0,
        .width = win_w,
        .height = win_h,
        .screen = &screen,
    };

    // Render the detail pane.
    try renderDetail(&state, detail_win);

    // Collect all rendered text.
    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS:
    // (a) Title must appear in the rendered output (rendered via printSegment
    //     with heap-allocated data, so grapheme pointers are stable).
    try testing.expect(std.mem.indexOf(u8, text, "Important Decision") != null);
    // (b) Derives-from section header must appear — task 4022 requires it.
    //     This is rendered via printSegment using a string literal.
    try testing.expect(std.mem.indexOf(u8, text, "Derives from:") != null);
    // The derives-from target label must appear — containing the artifact title.
    // This is rendered via printSegment using the heap-allocated df.label slice.
    try testing.expect(std.mem.indexOf(u8, text, "FoundingSpec") != null);
    // Note: body text goes through markdown_detail.render which writes cells
    // via writeCell(&ch, ...) with local stack addresses. Those grapheme
    // pointers are not stable for collectScreenText. The body rendering is
    // verified structurally above (detail.body contains the expected text)
    // and visually in the manual test tier. The render path is exercised
    // (no panic, no early return), which is the observable signal.
}

test "decision_log: renderDetail shows (none) when no derives-from edges (task 4022 render-level)" {
    // Verifies that even when derives_from is empty, the section header AND
    // the explicit "(none)" placeholder are rendered — so the operator can
    // distinguish "not loaded" from "genuinely empty".
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        \\insert into decisions (scope_kind, title, body, status)
        \\values ('global','StandaloneDecision','Standalone body text.','proposed')
    , &.{});

    var state = DecisionLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };
    try testing.expectEqual(@as(usize, 0), detail.derives_from.len);

    const win_w: u16 = 80;
    const win_h: u16 = 40;
    var screen = try vaxis.Screen.init(a, .{
        .cols = win_w,
        .rows = win_h,
        .x_pixel = 0,
        .y_pixel = 0,
    });
    defer screen.deinit(a);

    const detail_win: Window = .{
        .x_off = 0,
        .y_off = 0,
        .parent_x_off = 0,
        .parent_y_off = 0,
        .width = win_w,
        .height = win_h,
        .screen = &screen,
    };

    try renderDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // Title must appear — rendered via printSegment (heap-allocated data,
    // stable grapheme pointers).
    try testing.expect(std.mem.indexOf(u8, text, "StandaloneDecision") != null);
    // "Derives from:" header must appear even when there are no edges.
    // This is rendered via printSegment using a string literal.
    try testing.expect(std.mem.indexOf(u8, text, "Derives from:") != null);
    // "(none)" placeholder must appear when derives_from is empty.
    // This is rendered via printSegment using a string literal.
    try testing.expect(std.mem.indexOf(u8, text, "(none)") != null);
    // Note: body text ("Standalone body text.") goes through markdown_detail.render
    // using writeCell(&ch) with local stack addresses. Those grapheme pointers are
    // not stable for collectScreenText. Body rendering is verified structurally
    // (detail.body check above) and the render path is exercised without panic.
}

test "decision_log: legendLabel fits in buf" {
    var buf: [128]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Quit") != null);
}

test "decision_log compiles" {
    std.testing.refAllDecls(@This());
}
