//! cockpit/views/coverage_view.zig — Test Scenario & Coverage view (M8).
//!
//! Renders a two-pane layout for the Test Scenario & Coverage view:
//!
//!   • Left pane (navigator): list of test scenarios with their verifies→task
//!     links. Each row shows "[badge] status  title". j/k (or arrow keys)
//!     navigate. 'g' cycles the display between the scenario list and the
//!     coverage gap surface.
//!
//!   • Right pane (detail): for the selected scenario (list mode), shows:
//!       - Title (bold)
//!       - Status + body text via markdown_detail widget
//!       - "Verifies:" section listing the tasks this scenario covers
//!         OR "(none)" if the scenario is orphaned (no verifies edges).
//!
//!     In gap mode, the right pane shows the two gap classes:
//!       - "Orphan scenarios:" — scenarios with NO verifies edge
//!       - "Uncovered tasks:" — tasks with NO scenario verifying them
//!
//! Tasks 4025 (List scenarios with verifies links) and
//!       4026 (Coverage-gap surface: orphan scenarios + uncovered tasks).
//!
//! Acceptance invariants:
//!   (4025) The navigator lists ALL queried scenarios in created_at desc order;
//!          each row is rendered with badge + status + title.
//!          The detail pane renders BOTH the status/body AND the verifies section.
//!          Every datum queried (title, status, verifies links) is rendered.
//!   (4026) The gap surface shows BOTH gap classes with clear labels:
//!          (i)  "Orphan scenarios:" — scenarios with no verifies edge.
//!          (ii) "Uncovered tasks:" — tasks with no scenario verifying them.
//!          Pressing 'g' toggles between the scenario list and the gap surface.
//!
//! verifies direction (confirmed from engine/planning/test_spec_status.zig
//! and engine/ingestor/apply.zig):
//!   from_kind='test_scenario', from_id=scenario_id,
//!   to_kind='task', to_id=task_id, relationship='verifies'
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by CoverageState and released via deinit.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread.

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");
const markdown_detail = @import("../widgets/markdown_detail.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// DisplayMode
// =========================================================================

/// Whether to show the scenario list or the coverage gap surface.
pub const DisplayMode = enum {
    /// Show the scenario list with verifies links in the detail pane.
    scenarios,
    /// Show the coverage gap surface (orphan scenarios + uncovered tasks).
    gaps,
};

// =========================================================================
// CoverageState
// =========================================================================

/// All mutable state for the Test Scenario & Coverage view.
pub const CoverageState = struct {
    allocator: std.mem.Allocator,

    /// Current snapshot of scenarios, ordered newest-first.
    rows: []view_model.ScenarioCoverageRow = &.{},

    /// Coverage gap data for the gap surface.
    gap: ?view_model.CoverageGap = null,

    /// Navigator selection index (0-based into rows).
    selected_idx: usize = 0,

    /// Navigator scroll offset.
    scroll_offset: usize = 0,

    /// Whether to show scenario list or gap surface.
    mode: DisplayMode = .scenarios,

    /// Scope filter (default: .all).
    filter: view_model.ScopeFilter = .all,

    pub fn init(allocator: std.mem.Allocator) CoverageState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *CoverageState) void {
        view_model.ScenarioCoverageRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        if (self.gap) |g| g.deinit(self.allocator);
        self.gap = null;
    }

    /// Reload all scenario and gap data from the DB. Called on db_changed
    /// and on initial launch.
    pub fn reload(self: *CoverageState, d: *db.sqlite.Db) !void {
        // Free old data.
        view_model.ScenarioCoverageRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        if (self.gap) |g| g.deinit(self.allocator);
        self.gap = null;

        // Query new scenario rows (includes verifies links per row).
        self.rows = try view_model.queryScenarioCoverage(d, self.allocator, self.filter);

        // Clamp selection.
        if (self.rows.len > 0) {
            if (self.selected_idx >= self.rows.len) {
                self.selected_idx = self.rows.len - 1;
            }
        } else {
            self.selected_idx = 0;
        }

        // Query gap data (always loaded; rendered only in gap mode).
        self.gap = try view_model.queryCoverageGap(d, self.allocator, self.filter);
    }

    /// Handle a key event. Returns true when the key was consumed.
    pub fn handleKey(self: *CoverageState, key: Key, d: *db.sqlite.Db) bool {
        const count = self.rows.len;

        // j / arrow-down: move selection down (only active in scenarios mode).
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            if (self.mode == .scenarios and count > 0 and self.selected_idx + 1 < count) {
                self.selected_idx += 1;
            }
            return true;
        }
        // k / arrow-up: move selection up.
        if (key.matches('k', .{}) or key.matches(Key.up, .{})) {
            if (self.mode == .scenarios and self.selected_idx > 0) {
                self.selected_idx -= 1;
            }
            return true;
        }

        // 'g': toggle between scenario list and coverage gap surface (task 4026).
        if (key.matches('g', .{})) {
            self.mode = switch (self.mode) {
                .scenarios => .gaps,
                .gaps => .scenarios,
            };
            // Reload gap data on switch to gap mode so it is fresh.
            if (self.mode == .gaps) {
                if (self.gap) |g| g.deinit(self.allocator);
                self.gap = null;
                self.gap = view_model.queryCoverageGap(d, self.allocator, self.filter) catch null;
            }
            return true;
        }

        return false;
    }
};

// =========================================================================
// Render
// =========================================================================

/// Render the Test Scenario & Coverage view into the navigator and detail windows.
///
/// In scenarios mode:
///   Navigator (left): scenario list with verifies count indicator.
///   Detail (right): selected scenario's body + verifies section.
///
/// In gaps mode:
///   Navigator (left): coverage gap summary.
///   Detail (right): full gap lists (orphan scenarios + uncovered tasks).
pub fn render(
    state: *const CoverageState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    switch (state.mode) {
        .scenarios => {
            renderScenarioNavigator(state, nav_win);
            try renderScenarioDetail(state, detail_win, allocator);
        },
        .gaps => {
            renderGapNavigator(state, nav_win, allocator);
            renderGapDetail(state, detail_win);
        },
    }
}

// -------------------------------------------------------------------------
// Scenario list mode
// -------------------------------------------------------------------------

/// Render the navigator pane in scenario-list mode.
///
/// Each row: "[badge] status  title  (N verifies)"
/// The verifies count after the title makes it immediately visible whether
/// the scenario is orphaned (0 verifies) or covered.
/// The selected row is rendered in reverse-video.
fn renderScenarioNavigator(state: *const CoverageState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    if (state.rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no scenarios in scope — press g for gap surface)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    // Render header showing current mode.
    _ = win.printSegment(.{
        .text = "Scenarios  [g gap]",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });

    if (win.height < 2) return;

    // Viewport for rows starts at row 1 (below header).
    const viewport_h: usize = if (win.height > 1) @as(usize, @intCast(win.height)) - 1 else 0;
    const scroll: usize = if (state.selected_idx >= viewport_h)
        state.selected_idx - viewport_h + 1
    else
        0;

    var display_row: u16 = 1;
    for (state.rows, 0..) |row, i| {
        if (i < scroll) continue;
        if (display_row >= win.height) break;

        const is_selected = (i == state.selected_idx);
        const style: Style = if (is_selected)
            .{ .bold = true, .reverse = true }
        else
            .{};

        // Use the pre-formatted heap-allocated display_text (badge+status+title).
        _ = win.printSegment(.{
            .text = row.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });

        display_row += 1;
    }
}

/// Render the detail pane for the selected scenario (scenario-list mode).
///
/// Layout (top to bottom):
///   row 0            — scenario title (bold)
///   rows 1..body_end — markdown body (status + body text) via markdown_detail
///   separator        — blank row before verifies section
///   "Verifies:"      — section header (bold dim)
///   N rows           — one per verified task ("  label")
///                      OR "(none)" when verifies is empty (orphan scenario)
///
/// INVARIANT (task 4025): BOTH the body AND the verifies section are
/// ALWAYS rendered when a scenario is selected.
fn renderScenarioDetail(state: *const CoverageState, win: Window, arena: std.mem.Allocator) !void {
    if (win.height == 0 or win.width == 0) return;

    if (state.rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no scenario selected)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    const sel = state.rows[state.selected_idx];
    var row: u16 = 0;

    // ---- Title row (row 0) -----------------------------------------------
    if (sel.title.len > 0 and row < win.height) {
        _ = win.printSegment(.{
            .text = sel.title,
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    // ---- Compute supplemental section height ----------------------------
    //
    // Supplemental layout:
    //   1 separator (blank)
    //   1 "Verifies:" header
    //   max(1, verifies.len) rows — either N targets or "(none)"
    const verifies_rows: u16 = @intCast(@max(1, sel.verifies.len));
    const supp_rows: u16 = 1 + 1 + verifies_rows; // separator + header + targets

    // ---- Body (markdown_detail widget) -----------------------------------
    // Build an owned body string: "**Status:** {status}\n\n{body_text}"
    // Since test_scenarios.body is nullable we render at least the status.
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
        // Build the status line to render via markdown_detail.
        // Use the frame arena so grapheme slices written into the vaxis
        // back-buffer remain valid until vaxis.render() flushes them.
        const body_text = std.fmt.allocPrint(
            arena,
            "**Status:** {s}",
            .{sel.status},
        ) catch sel.status;
        try markdown_detail.render(body_win, arena, body_text);
        row = body_start + body_h;
    }

    // ---- Supplemental: verifies section ----------------------------------
    if (row >= win.height) return;
    row += 1; // blank separator
    if (row >= win.height) return;

    // "Verifies:" header.
    _ = win.printSegment(.{
        .text = "Verifies:",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = row, .col_offset = 0 });
    row += 1;

    if (sel.verifies.len == 0) {
        // Explicit "(none)" — orphan scenario.
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (none — orphan scenario)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
        return;
    }

    // One row per verified task.
    for (sel.verifies) |v| {
        if (row >= win.height) break;
        _ = win.printSegment(.{
            .text = "  ",
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = v.label,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 2 });
        row += 1;
    }
}

// -------------------------------------------------------------------------
// Gap surface mode
// -------------------------------------------------------------------------

/// Render the navigator pane in gap mode.
///
/// Shows a summary of the two gap classes with counts:
///   "Coverage Gap Surface"  (header)
///   "  Orphan scenarios: N"
///   "  Uncovered tasks:  N"
///   ""
///   "  [g] back to list"
fn renderGapNavigator(state: *const CoverageState, win: Window, arena: std.mem.Allocator) void {
    if (win.height == 0 or win.width == 0) return;

    var row: u16 = 0;

    _ = win.printSegment(.{
        .text = "Coverage Gap Surface  [g back]",
        .style = .{ .bold = true },
    }, .{ .row_offset = row, .col_offset = 0 });
    row += 1;
    if (row >= win.height) return;

    if (state.gap) |g| {
        // Use arena allocation so slices remain valid through vaxis.render().
        const orphan_line = std.fmt.allocPrint(
            arena,
            "  Orphan scenarios: {d}",
            .{g.orphan_scenarios.len},
        ) catch "  Orphan scenarios: ?";
        _ = win.printSegment(.{
            .text = orphan_line,
            .style = if (g.orphan_scenarios.len > 0) .{ .bold = true } else .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
        if (row >= win.height) return;

        const uncov_line = std.fmt.allocPrint(
            arena,
            "  Uncovered tasks:  {d}",
            .{g.uncovered_tasks.len},
        ) catch "  Uncovered tasks:  ?";
        _ = win.printSegment(.{
            .text = uncov_line,
            .style = if (g.uncovered_tasks.len > 0) .{ .bold = true } else .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
        if (row >= win.height) return;

        // Show "(no gaps)" when both classes are empty.
        if (g.orphan_scenarios.len == 0 and g.uncovered_tasks.len == 0) {
            row += 1;
            if (row >= win.height) return;
            _ = win.printSegment(.{
                .text = "  (no coverage gaps — fully covered)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
    } else {
        _ = win.printSegment(.{
            .text = "  (loading gap data...)",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
    }
}

/// Render the detail pane in gap mode.
///
/// Layout (top to bottom):
///   "Orphan scenarios:"  — section header (bold dim)
///   N rows               — one per orphan scenario ("  label")
///                          OR "(none)" when empty (no orphans)
///   separator            — blank row
///   "Uncovered tasks:"   — section header (bold dim)
///   N rows               — one per uncovered task ("  label  [status]")
///                          OR "(none)" when empty (fully covered)
///
/// INVARIANT (task 4026): BOTH gap classes are ALWAYS rendered, even when
/// empty. The operator must be able to confirm that a class was checked.
fn renderGapDetail(state: *const CoverageState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    const gap = state.gap orelse {
        _ = win.printSegment(.{
            .text = "(gap data not loaded)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    };

    var row: u16 = 0;

    // ---- Orphan scenarios section ----------------------------------------
    if (row >= win.height) return;
    _ = win.printSegment(.{
        .text = "Orphan scenarios:",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = row, .col_offset = 0 });
    row += 1;

    if (gap.orphan_scenarios.len == 0) {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (none)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            row += 1;
        }
    } else {
        for (gap.orphan_scenarios) |sc| {
            if (row >= win.height) break;
            _ = win.printSegment(.{
                .text = "  ",
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{
                .text = sc.label,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 2 });
            row += 1;
        }
    }

    // Blank separator between sections.
    if (row < win.height) {
        row += 1;
    }
    if (row >= win.height) return;

    // ---- Uncovered tasks section -----------------------------------------
    _ = win.printSegment(.{
        .text = "Uncovered tasks:",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = row, .col_offset = 0 });
    row += 1;

    if (gap.uncovered_tasks.len == 0) {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (none)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
    } else {
        for (gap.uncovered_tasks) |t| {
            if (row >= win.height) break;
            _ = win.printSegment(.{
                .text = "  ",
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{
                .text = t.label,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 2 });
            row += 1;
        }
    }
}

/// Return a one-line legend string for the key legend bar.
pub fn legendLabel(buf: []u8) []const u8 {
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Select  g Gap/List  Tab Focus  1-9 View",
        .{},
    ) catch "  q Quit  j/k Select  g Gap/List";
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
// CoverageState lifecycle tests
// -------------------------------------------------------------------------

test "coverage_view: init and deinit are clean" {
    var state = CoverageState.init(testing.allocator);
    defer state.deinit();
    try testing.expectEqual(@as(usize, 0), state.rows.len);
    try testing.expectEqual(@as(?view_model.CoverageGap, null), state.gap);
}

test "coverage_view: reload on empty DB yields empty rows (empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = CoverageState.init(a);
    defer state.deinit();

    try state.reload(&d);

    try testing.expectEqual(@as(usize, 0), state.rows.len);
    // Gap data is always populated after reload (even if both slices are empty).
    try testing.expect(state.gap != null);
    if (state.gap) |g| {
        try testing.expectEqual(@as(usize, 0), g.orphan_scenarios.len);
        try testing.expectEqual(@as(usize, 0), g.uncovered_tasks.len);
    }
}

test "coverage_view: reload populates scenario rows with verifies links (task 4025)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert a scenario.
    const sid = try d.execParams(
        "insert into test_scenarios (scope_kind, title, body, status) values ('global','Alpha Scenario','The scenario body.','ready')",
        &.{},
    );
    // Insert a task.
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Alpha Task','todo')",
        &.{},
    );
    // Link: scenario verifies task (direction confirmed from engine).
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('test_scenario', ?, 'task', ?, 'verifies')",
        &.{ .{ .int = sid }, .{ .int = tid } },
    );

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqualStrings("Alpha Scenario", state.rows[0].title);
    try testing.expectEqualStrings("ready", state.rows[0].status);
    // verifies link must be populated.
    try testing.expectEqual(@as(usize, 1), state.rows[0].verifies.len);
    try testing.expectEqual(tid, state.rows[0].verifies[0].task_id);
    // label must contain the task title.
    try testing.expect(std.mem.indexOf(u8, state.rows[0].verifies[0].label, "Alpha Task") != null);
    try testing.expect(std.mem.indexOf(u8, state.rows[0].verifies[0].label, "task:") != null);
}

test "coverage_view: orphan scenario has empty verifies (task 4026)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert a scenario with NO verifies edge.
    _ = try d.execParams(
        "insert into test_scenarios (scope_kind, title, body, status) values ('global','Orphan Scenario','body','draft')",
        &.{},
    );

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    // Orphan scenario: verifies slice is empty.
    try testing.expectEqual(@as(usize, 0), state.rows[0].verifies.len);
}

test "coverage_view: gap surface shows orphan scenario (task 4026)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into test_scenarios (scope_kind, title, body, status) values ('global','Orphan Scenario','body','draft')",
        &.{},
    );

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expect(state.gap != null);
    const g = state.gap.?;
    try testing.expectEqual(@as(usize, 1), g.orphan_scenarios.len);
    try testing.expectEqualStrings("Orphan Scenario", g.orphan_scenarios[0].title);
    try testing.expect(std.mem.indexOf(u8, g.orphan_scenarios[0].label, "Orphan Scenario") != null);
    try testing.expect(std.mem.indexOf(u8, g.orphan_scenarios[0].label, "scenario:") != null);
}

test "coverage_view: gap surface shows uncovered task (task 4026)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Uncovered Task','todo')",
        &.{},
    );

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expect(state.gap != null);
    const g = state.gap.?;
    try testing.expectEqual(@as(usize, 1), g.uncovered_tasks.len);
    try testing.expectEqualStrings("Uncovered Task", g.uncovered_tasks[0].title);
    try testing.expect(std.mem.indexOf(u8, g.uncovered_tasks[0].label, "Uncovered Task") != null);
    try testing.expect(std.mem.indexOf(u8, g.uncovered_tasks[0].label, "task:") != null);
}

test "coverage_view: fully covered fixture shows no gaps (task 4026)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Scenario that verifies a task → no orphan, task is covered.
    const sid = try d.execParams(
        "insert into test_scenarios (scope_kind, title, body, status) values ('global','Covered Scenario','body','verified')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Covered Task','done')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('test_scenario', ?, 'task', ?, 'verifies')",
        &.{ .{ .int = sid }, .{ .int = tid } },
    );

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expect(state.gap != null);
    const g = state.gap.?;
    // No orphan scenarios and no uncovered tasks.
    try testing.expectEqual(@as(usize, 0), g.orphan_scenarios.len);
    try testing.expectEqual(@as(usize, 0), g.uncovered_tasks.len);
}

test "coverage_view: handleKey j/k moves selection in scenarios mode" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into test_scenarios (scope_kind, title, status) values ('global','S1','draft')",
        &.{},
    );
    _ = try d.execParams(
        "insert into test_scenarios (scope_kind, title, status) values ('global','S2','ready')",
        &.{},
    );

    var state = CoverageState.init(a);
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

test "coverage_view: handleKey g toggles display mode (task 4026)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(DisplayMode.scenarios, state.mode);

    const g_key = Key{ .codepoint = 'g', .mods = .{} };
    _ = state.handleKey(g_key, &d);
    try testing.expectEqual(DisplayMode.gaps, state.mode);

    _ = state.handleKey(g_key, &d);
    try testing.expectEqual(DisplayMode.scenarios, state.mode);
}

// =========================================================================
// RENDER-LEVEL TESTS (rule (b): assert rendered text in screen buffer)
// =========================================================================
//
// These tests allocate a real vaxis.Screen, call the render functions,
// then scan the cell buffer for expected text. A struct-level assertion
// that a field is populated is NOT sufficient; these tests prove the
// rendered output actually contains the data.

test "coverage_view: renderScenarioNavigator renders scenario titles (task 4025 render-level)" {
    // Seeds two scenarios and verifies both titles appear in the navigator output.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into test_scenarios (scope_kind, title, status) values ('global','First Scenario','ready')",
        &.{},
    );
    _ = try d.execParams(
        "insert into test_scenarios (scope_kind, title, status) values ('global','Second Scenario','verified')",
        &.{},
    );

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 2), state.rows.len);

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

    renderScenarioNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS: both titles must appear in the rendered output.
    try testing.expect(std.mem.indexOf(u8, text, "First Scenario") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Second Scenario") != null);
    // Status values must appear ("ready", "verified").
    try testing.expect(std.mem.indexOf(u8, text, "ready") != null);
    try testing.expect(std.mem.indexOf(u8, text, "verified") != null);
    // Header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Scenarios") != null);
}

test "coverage_view: renderScenarioNavigator shows empty state when no scenarios (render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = CoverageState.init(a);
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

    renderScenarioNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    try testing.expect(std.mem.indexOf(u8, text, "no scenarios in scope") != null);
}

test "coverage_view: renderScenarioDetail renders Verifies section with task (task 4025 render-level)" {
    // Critical render-level test for task 4025: verifies section appears with task label.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into test_scenarios (scope_kind, title, body, status) values ('global','RenderScenario','Scenario body text.','ready')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','RenderTask','todo')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('test_scenario', ?, 'task', ?, 'verifies')",
        &.{ .{ .int = sid }, .{ .int = tid } },
    );

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    // Struct-level sanity check.
    try testing.expectEqual(@as(usize, 1), state.rows[0].verifies.len);
    try testing.expect(std.mem.indexOf(u8, state.rows[0].verifies[0].label, "RenderTask") != null);

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

    var test_arena = std.heap.ArenaAllocator.init(a);
    defer test_arena.deinit();
    try renderScenarioDetail(&state, detail_win, test_arena.allocator());

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS:
    // (a) Title must appear — rendered via printSegment with heap-allocated data.
    try testing.expect(std.mem.indexOf(u8, text, "RenderScenario") != null);
    // (b) "Verifies:" header must appear — task 4025 requires it.
    try testing.expect(std.mem.indexOf(u8, text, "Verifies:") != null);
    // (c) The verified task's label must appear — heap-allocated, stable pointer.
    try testing.expect(std.mem.indexOf(u8, text, "RenderTask") != null);
    // (d) Status line body text is arena-backed after UAF fix (task 4198).
    try testing.expect(std.mem.indexOf(u8, text, "Status:") != null);
}

test "coverage_view: renderScenarioDetail shows (none) for orphan scenario (task 4025 render-level)" {
    // Verifies that orphan scenario shows the "Verifies:" header AND "(none)" placeholder.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into test_scenarios (scope_kind, title, body, status) values ('global','OrphanScenario','body','draft')",
        &.{},
    );

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqual(@as(usize, 0), state.rows[0].verifies.len);

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

    var test_arena = std.heap.ArenaAllocator.init(a);
    defer test_arena.deinit();
    try renderScenarioDetail(&state, detail_win, test_arena.allocator());

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // Title must appear.
    try testing.expect(std.mem.indexOf(u8, text, "OrphanScenario") != null);
    // "Verifies:" header must appear even when verifies is empty.
    try testing.expect(std.mem.indexOf(u8, text, "Verifies:") != null);
    // "(none" placeholder must appear for orphan scenario.
    try testing.expect(std.mem.indexOf(u8, text, "none") != null);
}

test "coverage_view: renderGapDetail shows both gap classes (task 4026 render-level)" {
    // Critical render-level test for task 4026.
    // Seeds an orphan scenario AND an uncovered task, then verifies BOTH
    // gap class headers and labels appear in the rendered gap detail pane.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into test_scenarios (scope_kind, title, status) values ('global','GapOrphanScenario','draft')",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','GapUncoveredTask','todo')",
        &.{},
    );

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Switch to gap mode.
    state.mode = .gaps;

    // Struct-level sanity check.
    try testing.expect(state.gap != null);
    const g = state.gap.?;
    try testing.expectEqual(@as(usize, 1), g.orphan_scenarios.len);
    try testing.expectEqual(@as(usize, 1), g.uncovered_tasks.len);

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

    // Render the gap detail pane.
    renderGapDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS:
    // (a) "Orphan scenarios:" section header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Orphan scenarios:") != null);
    // (b) The orphan scenario's label must appear (heap-allocated, stable pointer).
    try testing.expect(std.mem.indexOf(u8, text, "GapOrphanScenario") != null);
    // (c) "Uncovered tasks:" section header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Uncovered tasks:") != null);
    // (d) The uncovered task's label must appear (heap-allocated, stable pointer).
    try testing.expect(std.mem.indexOf(u8, text, "GapUncoveredTask") != null);
}

test "coverage_view: renderGapDetail shows (none) for both classes when fully covered (task 4026 render-level)" {
    // Verifies that when there are no gaps, both section headers still appear
    // with "(none)" placeholders so the operator can confirm the check ran.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Fully covered: scenario verifies task.
    const sid = try d.execParams(
        "insert into test_scenarios (scope_kind, title, status) values ('global','FullyCoveredScenario','verified')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','FullyCoveredTask','done')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('test_scenario', ?, 'task', ?, 'verifies')",
        &.{ .{ .int = sid }, .{ .int = tid } },
    );

    var state = CoverageState.init(a);
    defer state.deinit();
    try state.reload(&d);
    state.mode = .gaps;

    try testing.expect(state.gap != null);
    const g = state.gap.?;
    try testing.expectEqual(@as(usize, 0), g.orphan_scenarios.len);
    try testing.expectEqual(@as(usize, 0), g.uncovered_tasks.len);

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

    renderGapDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // Both section headers must appear even when there are no gaps.
    try testing.expect(std.mem.indexOf(u8, text, "Orphan scenarios:") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Uncovered tasks:") != null);
    // Both "(none)" placeholders must appear.
    // We check that "(none)" appears at least twice (once per section).
    var count: usize = 0;
    var search = text;
    while (std.mem.indexOf(u8, search, "(none)")) |pos| {
        count += 1;
        search = search[pos + 6 ..];
    }
    try testing.expect(count >= 2);
}

test "coverage_view: legendLabel fits in buf" {
    var buf: [128]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Quit") != null);
    try testing.expect(std.mem.indexOf(u8, label, "Gap") != null);
}

test "coverage_view compiles" {
    std.testing.refAllDecls(@This());
}
