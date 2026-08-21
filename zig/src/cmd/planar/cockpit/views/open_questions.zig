//! cockpit/views/open_questions.zig — Open Questions view (M7).
//!
//! Renders a two-pane layout for the Open Questions view:
//!
//!   • Left pane (navigator): list of questions filtered by status, newest-first.
//!     Each row shows "[badge] status  title". j/k (or arrow keys) navigate.
//!     'f' cycles the status filter: open → answered → wontfix → all → open.
//!
//!   • Right pane (detail): for the selected question, shows:
//!       - Title (bold)
//!       - Body (markdown, via markdown_detail widget): includes status + body + answer
//!       - "Linked to:" section with entity_links targets (both directions)
//!         showing each linked entity's kind:id and title, and its relationship.
//!       - "Jump:" affordance — pressing 'g' on a selected question that has a
//!         linked entity performs a real cross-view focus jump: handleKey returns
//!         a entity_link_graph.FocusRequest with switch_to_view=.entity_link_graph
//!         so that app.zig switches to the Entity-Link Graph view focused on the
//!         target entity. When no linked entity exists, 'g' is a safe no-op.
//!         This mechanism was introduced in M9 for entity_link_graph and is reused
//!         here (same FocusRequest type, same app.zig dispatch path). Task 4136.
//!
//! Tasks 4023 (List questions filtered by status + linked entities) and
//!       4024 (Status filter cycling + jump-to-linked-entity affordance).
//!
//! Acceptance invariants:
//!   (4023) The navigator lists ALL queried questions in updated_at desc order;
//!          each row is rendered with badge + status + title.
//!          The detail pane renders BOTH the body AND the linked entities section.
//!          Every datum queried (title, status, body, linked entities) is rendered.
//!   (4024) 'f' cycles the status filter; the rendered list changes accordingly.
//!          The detail pane surfaces linked entity labels (kind:id — title)
//!          so the operator can navigate to the linked plan/artifact.
//!   (4136) 'g' on a question with a linked entity returns a FocusRequest that
//!          causes app.zig to switch to the Entity-Link Graph view centered on
//!          the linked entity. No linked entity → 'g' is a no-op (no crash).
//!
//! Status enum values (from migration 00003_work_items.up.sql):
//!   check(status in ('open','answered','wontfix'))
//!
//! entity_links from migration 00004_entity_links.up.sql:
//!   from_kind/to_kind include 'question' (in the CHECK constraint)
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by OpenQuestionsState and released via deinit.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread.
//!   - Jump-to-linked-entity (task 4136): 'g' returns a FocusRequest{kind, id,
//!     switch_to_view=.entity_link_graph} so app.zig can switch + refocus the
//!     Entity-Link Graph view on the first linked entity. The jump_target field
//!     still surfaces the label in the detail pane for the in-view affordance.
//!   - linked entities use the entity_links table's both-direction query.

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");
const markdown_detail = @import("../widgets/markdown_detail.zig");
const entity_link_graph = @import("entity_link_graph.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// OpenQuestionsState
// =========================================================================

/// All mutable state for the Open Questions view.
pub const OpenQuestionsState = struct {
    allocator: std.mem.Allocator,

    /// Current snapshot of questions under the active filter, newest-first.
    rows: []view_model.OpenQuestionsRow = &.{},

    /// Navigator selection index (0-based into rows).
    selected_idx: usize = 0,

    /// Navigator scroll offset.
    scroll_offset: usize = 0,

    /// Status filter (default: .open per task 4024 — show open questions first).
    filter: view_model.QuestionStatusFilter = .open,

    /// Detail pane data for the currently selected question. Null when the
    /// list is empty or no selection is valid.
    detail: ?view_model.OpenQuestionsDetail = null,

    /// Jump target: when the operator presses 'g', this is set to the first
    /// linked entity's label so it can be prominently surfaced in the status bar.
    /// Heap-allocated; freed on next reload or deinit.
    jump_target: ?[]const u8 = null,

    pub fn init(allocator: std.mem.Allocator) OpenQuestionsState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *OpenQuestionsState) void {
        view_model.OpenQuestionsRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        if (self.detail) |d| d.deinit(self.allocator);
        self.detail = null;
        if (self.jump_target) |s| self.allocator.free(s);
        self.jump_target = null;
    }

    /// Reload all questions data from the DB. Called on db_changed and on launch.
    pub fn reload(self: *OpenQuestionsState, d: *db.sqlite.Db) !void {
        // Free old data.
        view_model.OpenQuestionsRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        if (self.detail) |det| det.deinit(self.allocator);
        self.detail = null;
        if (self.jump_target) |s| self.allocator.free(s);
        self.jump_target = null;

        // Query new rows.
        self.rows = try view_model.queryOpenQuestions(d, self.allocator, self.filter);

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

    /// Refresh the detail pane for the currently selected question.
    pub fn refreshDetail(self: *OpenQuestionsState, d: *db.sqlite.Db) !void {
        if (self.detail) |det| {
            det.deinit(self.allocator);
            self.detail = null;
        }

        if (self.rows.len == 0) return;
        const sel = self.rows[self.selected_idx];
        self.detail = try view_model.queryOpenQuestionsDetail(d, self.allocator, sel.id);
    }

    /// Result returned by handleKey.
    ///
    /// Mirrors entity_link_graph.EntityLinkState.HandleKeyResult so that
    /// app.zig can apply the same FocusRequest dispatch path for both views.
    /// When focus is non-null, app.zig switches to the Entity-Link Graph view
    /// and calls entity_graph.reloadFor(fr.kind, fr.id).
    pub const HandleKeyResult = struct {
        consumed: bool,
        focus: ?entity_link_graph.FocusRequest,
    };

    /// Handle a key event. Returns a HandleKeyResult.
    ///
    /// consumed=true when the key was handled (triggers a re-render).
    /// focus != null when 'g' was pressed on a question with a linked entity;
    ///   app.zig uses focus to switch+refocus the Entity-Link Graph view.
    pub fn handleKey(self: *OpenQuestionsState, key: Key, d: *db.sqlite.Db) HandleKeyResult {
        const count = self.rows.len;

        // j / arrow-down: move selection down.
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            if (count > 0 and self.selected_idx + 1 < count) {
                self.selected_idx += 1;
                self.refreshDetail(d) catch {};
                // Clear jump target on navigation.
                if (self.jump_target) |s| {
                    self.allocator.free(s);
                    self.jump_target = null;
                }
            }
            return .{ .consumed = true, .focus = null };
        }
        // k / arrow-up: move selection up.
        if (key.matches('k', .{}) or key.matches(Key.up, .{})) {
            if (self.selected_idx > 0) {
                self.selected_idx -= 1;
                self.refreshDetail(d) catch {};
                // Clear jump target on navigation.
                if (self.jump_target) |s| {
                    self.allocator.free(s);
                    self.jump_target = null;
                }
            }
            return .{ .consumed = true, .focus = null };
        }

        // 'f': cycle the status filter (task 4024).
        if (key.matches('f', .{})) {
            self.filter = self.filter.next();
            // Clear jump target on filter change.
            if (self.jump_target) |s| {
                self.allocator.free(s);
                self.jump_target = null;
            }
            self.selected_idx = 0;
            self.reload(d) catch {};
            return .{ .consumed = true, .focus = null };
        }

        // 'g': jump-to-linked-entity (task 4024 affordance + task 4136 real jump).
        //
        // Task 4136: when the question has a linked entity, return a FocusRequest
        // so that app.zig switches to the Entity-Link Graph view focused on that
        // entity. The jump_target field is also set so the detail pane surfaces
        // the label as the in-view affordance.
        //
        // When no linked entity exists, 'g' is a no-op (consumed but no focus).
        if (key.matches('g', .{})) {
            if (self.jump_target) |s| {
                self.allocator.free(s);
                self.jump_target = null;
            }
            if (self.detail) |det| {
                if (det.linked.len > 0) {
                    const linked = det.linked[0];
                    // Surface the first linked entity's label in the detail pane.
                    self.jump_target = self.allocator.dupe(u8, linked.label) catch null;
                    // Return a FocusRequest so app.zig switches to the entity-link
                    // graph view focused on this entity. The kind and id fields
                    // point into the detail's linked slice which is owned by this
                    // state and valid until the next reload().
                    return .{
                        .consumed = true,
                        .focus = .{
                            .kind = linked.target_kind,
                            .id = linked.target_id,
                            .switch_to_view = .entity_link_graph,
                        },
                    };
                }
            }
            // No linked entity: key was consumed (no crash), no focus change.
            return .{ .consumed = true, .focus = null };
        }

        return .{ .consumed = false, .focus = null };
    }
};

// =========================================================================
// Render
// =========================================================================

/// Render the Open Questions view into the navigator and detail windows.
///
/// Navigator (left pane): list of questions filtered by status, newest-first.
/// Detail (right pane): body + linked entities for the selected question.
pub fn render(
    state: *const OpenQuestionsState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    renderNavigator(state, nav_win);
    try renderDetail(state, detail_win, allocator);
}

/// Render the navigator pane (left): list of questions filtered by status.
///
/// Each row: "[badge] status  title"
///   badge = StatusBadge glyph for the question status
///   status = "open" / "answered" / "wontfix"
///   title = question title (truncated to window width)
///
/// The selected row is rendered in reverse-video.
fn renderNavigator(state: *const OpenQuestionsState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    if (state.rows.len == 0) {
        // Empty state: show filter context so operator knows it was applied.
        // Use two printSegment calls with stable pointers (string literals and
        // the filter label from QuestionStatusFilter.label() which returns a
        // string literal) so that grapheme pointers remain valid in render-level
        // tests (no stack-allocated fmt buffer).
        _ = win.printSegment(.{
            .text = "(no questions — filter: ",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = state.filter.label(),
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 24 });
        _ = win.printSegment(.{
            .text = ")",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = @intCast(24 + state.filter.label().len) });
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
///   row 0            — question title (bold)
///   rows 1..body_end — markdown body (status + body text + answer) via markdown_detail
///   separator        — blank row before linked section
///   "Linked to:"     — section header (bold dim)
///   N rows           — one per linked entity ("  relationship: label")
///                      OR "(none)" when linked is empty (explicit)
///   (optional)       — "Jump: <label>" when jump_target is set (task 4024)
///
/// INVARIANT (tasks 4023/4024): BOTH the body AND the linked section are
/// ALWAYS rendered when a question is selected. This matches the M6 pattern.
fn renderDetail(state: *const OpenQuestionsState, win: Window, arena: std.mem.Allocator) !void {
    if (win.height == 0 or win.width == 0) return;

    const detail = state.detail orelse {
        _ = win.printSegment(.{
            .text = "(no question selected)",
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
    //   1 "Linked to:" header
    //   max(1, linked.len) rows — either N targets or "(none)"
    //   1 "Jump:" row when jump_target is set
    const linked_rows: u16 = @intCast(@max(1, detail.linked.len));
    // Jump section takes 2 rows: 1 blank separator + 1 "Jump: <label>" line.
    const jump_rows: u16 = if (state.jump_target != null) 2 else 0;
    const supp_rows: u16 = 1 + 1 + linked_rows + jump_rows; // sep + header + targets + jump

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
        try markdown_detail.render(body_win, arena, detail.body);
        row = body_start + body_h;
    }

    // ---- Supplemental: linked entities section ---------------------------
    //
    // Separator.
    if (row >= win.height) return;
    row += 1; // blank separator
    if (row >= win.height) return;

    // "Linked to:" header.
    _ = win.printSegment(.{
        .text = "Linked to:",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = row, .col_offset = 0 });
    row += 1;

    if (detail.linked.len == 0) {
        // Explicit "(none)" so the operator can distinguish "not loaded"
        // from "genuinely has no linked entities".
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (none)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
    } else {
        // One row per linked entity: "  relationship: label"
        // Use separate printSegment calls with stable-pointer strings to avoid
        // the stack-allocated format-buffer grapheme instability issue (the same
        // pattern as decision_log.zig's derives-from rendering). le.relationship
        // and le.label are heap-allocated (stable). "  " and ": " are literals.
        for (detail.linked) |le| {
            if (row >= win.height) break;
            // "  " prefix — string literal (stable pointer).
            _ = win.printSegment(.{
                .text = "  ",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            // relationship — heap-allocated (stable pointer).
            _ = win.printSegment(.{
                .text = le.relationship,
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 2 });
            // ": " separator — string literal (stable pointer).
            _ = win.printSegment(.{
                .text = ": ",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = @intCast(2 + le.relationship.len) });
            // label — heap-allocated (stable pointer).
            _ = win.printSegment(.{
                .text = le.label,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = @intCast(2 + le.relationship.len + 2) });
            row += 1;
        }
    }

    // ---- Jump target (task 4024 affordance) ------------------------------
    // When the operator has pressed 'g', the jump_target is set to the
    // first linked entity's label. Render it prominently below the linked
    // section so the operator has the entity identity to navigate to.
    if (state.jump_target) |jt| {
        if (row >= win.height) return;
        row += 1; // extra blank before jump line
        if (row >= win.height) return;
        _ = win.printSegment(.{
            .text = "Jump:",
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = " ",
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 5 });
        _ = win.printSegment(.{
            .text = jt,
            .style = .{ .bold = true, .reverse = true },
        }, .{ .row_offset = row, .col_offset = 6 });
    }
}

/// Return a one-line legend string for the key legend bar.
pub fn legendLabel(buf: []u8) []const u8 {
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Select  f Filter  g Jump  Tab Focus  1-9 View",
        .{},
    ) catch "  q Quit  j/k Select  f Filter  g Jump";
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
// OpenQuestionsState lifecycle tests
// -------------------------------------------------------------------------

test "open_questions: init and deinit are clean" {
    var state = OpenQuestionsState.init(testing.allocator);
    defer state.deinit();
    try testing.expectEqual(@as(usize, 0), state.rows.len);
    try testing.expectEqual(@as(?view_model.OpenQuestionsDetail, null), state.detail);
}

test "open_questions: reload on empty DB yields empty rows (task 4023 empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = OpenQuestionsState.init(a);
    defer state.deinit();

    try state.reload(&d);

    try testing.expectEqual(@as(usize, 0), state.rows.len);
    try testing.expectEqual(@as(?view_model.OpenQuestionsDetail, null), state.detail);
}

test "open_questions: reload populates rows from DB (task 4023)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','Q1','body1','open')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status, answer_body, answered_at) values ('global','Q2','body2','answered','answer text','2025-01-01T00:00:00.000Z')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    // Default filter is 'open', so only Q1 shows.
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqualStrings("Q1", state.rows[0].title);
    try testing.expectEqualStrings("open", state.rows[0].status);
}

test "open_questions: filter=answered shows only answered questions (task 4024)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','OpenQ','body','open')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status, answer_body, answered_at) values ('global','AnsweredQ','body','answered','the answer','2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','WontfixQ','body','wontfix')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .answered;
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqualStrings("AnsweredQ", state.rows[0].title);
}

test "open_questions: filter=wontfix shows only wontfix questions (task 4024)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','OpenQ','b','open')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','WontfixQ','b','wontfix')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .wontfix;
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqualStrings("WontfixQ", state.rows[0].title);
}

test "open_questions: filter=all shows all questions (task 4024)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','Q1','b','open')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status, answer_body, answered_at) values ('global','Q2','b','answered','a','2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','Q3','b','wontfix')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .all;
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 3), state.rows.len);
}

test "open_questions: handleKey j/k moves selection (task 4024)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','Q1','b','open')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','Q2','b','open')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
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

test "open_questions: handleKey f cycles the filter (task 4024)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(view_model.QuestionStatusFilter.open, state.filter);

    const f_key = Key{ .codepoint = 'f', .mods = .{} };
    _ = state.handleKey(f_key, &d);
    try testing.expectEqual(view_model.QuestionStatusFilter.answered, state.filter);

    _ = state.handleKey(f_key, &d);
    try testing.expectEqual(view_model.QuestionStatusFilter.wontfix, state.filter);

    _ = state.handleKey(f_key, &d);
    try testing.expectEqual(view_model.QuestionStatusFilter.all, state.filter);

    _ = state.handleKey(f_key, &d);
    try testing.expectEqual(view_model.QuestionStatusFilter.open, state.filter);
}

test "open_questions: badge reflects question status (task 4023)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','OpenQ','b','open')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status, answer_body, answered_at) values ('global','AnsweredQ','b','answered','a','2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','WontfixQ','b','wontfix')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .all;
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 3), state.rows.len);
    for (state.rows) |row| {
        if (std.mem.eql(u8, row.title, "OpenQ")) {
            try testing.expectEqual(view_model.StatusBadge.todo, row.badge);
        }
        if (std.mem.eql(u8, row.title, "AnsweredQ")) {
            try testing.expectEqual(view_model.StatusBadge.done, row.badge);
        }
        if (std.mem.eql(u8, row.title, "WontfixQ")) {
            try testing.expectEqual(view_model.StatusBadge.cancelled, row.badge);
        }
    }
}

// -------------------------------------------------------------------------
// Detail + linked-entity tests (task 4023)
// -------------------------------------------------------------------------

test "open_questions: detail body is populated with status and body text (task 4023)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','DetailQ','The question body text.','open')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };
    try testing.expectEqualStrings("DetailQ", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "open") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "The question body text.") != null);
}

test "open_questions: detail includes answer_body for answered questions (task 4023)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status, answer_body, answered_at) values ('global','AnsweredQ','Question body.','answered','The definitive answer.','2025-06-01T00:00:00.000Z')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .answered;
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };
    try testing.expect(std.mem.indexOf(u8, detail.body, "The definitive answer.") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "answered") != null);
}

test "open_questions: detail linked entities populated from entity_links (task 4023)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const qid = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','LinkedQ','body','open')",
        &.{},
    );
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','AnchorPlan','anchor-plan','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('question', ?, 'plan', ?, 'addresses')",
        &.{ .{ .int = qid }, .{ .int = pid } },
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };

    try testing.expectEqual(@as(usize, 1), detail.linked.len);
    try testing.expectEqualStrings("plan", detail.linked[0].target_kind);
    try testing.expectEqual(pid, detail.linked[0].target_id);
    try testing.expectEqualStrings("addresses", detail.linked[0].relationship);
    // Label must contain the plan title.
    try testing.expect(std.mem.indexOf(u8, detail.linked[0].label, "AnchorPlan") != null);
}

test "open_questions: detail linked entities empty when no links (task 4023)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','StandaloneQ','body','open')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };
    try testing.expectEqual(@as(usize, 0), detail.linked.len);
}

test "open_questions: handleKey g sets jump_target to first linked entity (task 4024)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const qid = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','JumpQ','body','open')",
        &.{},
    );
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','LinkedPlan','linked-plan','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('question', ?, 'plan', ?, 'addresses')",
        &.{ .{ .int = qid }, .{ .int = pid } },
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    try testing.expect(state.jump_target == null);

    const g_key = Key{ .codepoint = 'g', .mods = .{} };
    _ = state.handleKey(g_key, &d);

    // jump_target must be set to the first linked entity's label.
    try testing.expect(state.jump_target != null);
    try testing.expect(std.mem.indexOf(u8, state.jump_target.?, "LinkedPlan") != null);
}

test "open_questions: handleKey g with no linked entities does not set jump_target (task 4024)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','NoLinkQ','body','open')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    const g_key = Key{ .codepoint = 'g', .mods = .{} };
    _ = state.handleKey(g_key, &d);

    // No linked entities → jump_target stays null.
    try testing.expect(state.jump_target == null);
}

// -------------------------------------------------------------------------
// Task 4136: 'g' returns a real FocusRequest for cross-view jump
// -------------------------------------------------------------------------

test "open_questions: handleKey g returns FocusRequest with correct kind/id (task 4136)" {
    // Core contract: 'g' on a question with a linked entity returns a
    // HandleKeyResult where:
    //   - consumed = true
    //   - focus.kind = linked entity kind ("plan")
    //   - focus.id = linked entity id
    //   - focus.switch_to_view = .entity_link_graph
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const qid = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','FocusReqQ','body','open')",
        &.{},
    );
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','FocusPlan','focus-plan','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('question', ?, 'plan', ?, 'addresses')",
        &.{ .{ .int = qid }, .{ .int = pid } },
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    const g_key = Key{ .codepoint = 'g', .mods = .{} };
    const result = state.handleKey(g_key, &d);

    // Must be consumed.
    try testing.expect(result.consumed);

    // Focus request must be non-null and point at the linked plan.
    try testing.expect(result.focus != null);
    const fr = result.focus.?;
    try testing.expectEqualStrings("plan", fr.kind);
    try testing.expectEqual(pid, fr.id);

    // switch_to_view must be .entity_link_graph (the cross-view jump target).
    try testing.expect(fr.switch_to_view != null);
    try testing.expectEqual(@as(view_model.ViewId, .entity_link_graph), fr.switch_to_view.?);
}

test "open_questions: handleKey g no-linked-entity is a no-op (task 4136)" {
    // When the question has no linked entity, 'g' must be consumed (no crash)
    // and focus must be null (no spurious view switch).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','NolinkQ2','body','open')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    const g_key = Key{ .codepoint = 'g', .mods = .{} };
    const result = state.handleKey(g_key, &d);

    // Consumed (no crash, no spurious view switch).
    try testing.expect(result.consumed);
    // No FocusRequest — must not switch views.
    try testing.expect(result.focus == null);
}

test "open_questions: app-level dispatch — entity_graph reloadFor invoked with correct kind/id (task 4136)" {
    // Simulate the app.zig dispatch path: after 'g' returns a FocusRequest,
    // call entity_graph.reloadFor(fr.kind, fr.id) and assert the entity-link
    // graph state is now focused on the linked plan.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const qid = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','DispatchQ','body','open')",
        &.{},
    );
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','DispatchPlan','dispatch-plan','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('question', ?, 'plan', ?, 'addresses')",
        &.{ .{ .int = qid }, .{ .int = pid } },
    );

    // Open Questions state.
    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    // Entity-Link Graph state (starts with no focus).
    var eg_state = entity_link_graph.EntityLinkState.init(a);
    defer eg_state.deinit();
    try testing.expect(eg_state.data.focus == null);

    // Press 'g' to get the FocusRequest.
    const g_key = Key{ .codepoint = 'g', .mods = .{} };
    const result = state.handleKey(g_key, &d);
    try testing.expect(result.consumed);
    try testing.expect(result.focus != null);

    const fr = result.focus.?;

    // Simulate app.zig: call reloadFor on entity_graph with the focus kind/id.
    try eg_state.reloadFor(&d, fr.kind, fr.id);

    // Assert the entity-link graph is now focused on the linked plan.
    try testing.expect(eg_state.data.focus != null);
    try testing.expectEqualStrings("plan", eg_state.data.focus.?.kind);
    try testing.expectEqual(pid, eg_state.data.focus.?.id);
    // Title must be resolved.
    try testing.expectEqualStrings("DispatchPlan", eg_state.data.focus.?.title);
}

test "open_questions: detail incoming link from artifact (task 4023)" {
    // Verify the incoming direction of entity_links (artifact → question).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const qid = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','IncomingQ','body','open')",
        &.{},
    );
    const aid = try d.execParams(
        "insert into artifacts (scope_kind, title, body, kind) values ('global','SourceArtifact','artifact body','tech_spec')",
        &.{},
    );
    // Incoming: artifact → question (cites).
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('artifact', ?, 'question', ?, 'cites')",
        &.{ .{ .int = aid }, .{ .int = qid } },
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };
    // Should surface the incoming artifact→question link.
    try testing.expectEqual(@as(usize, 1), detail.linked.len);
    try testing.expectEqualStrings("artifact", detail.linked[0].target_kind);
    try testing.expect(std.mem.indexOf(u8, detail.linked[0].label, "SourceArtifact") != null);
}

// =========================================================================
// RENDER-LEVEL TESTS (rule (b): assert rendered text in screen buffer)
// =========================================================================
//
// These tests allocate a real vaxis.Screen, call the render functions,
// then scan the cell buffer for expected text. A struct-level assertion
// that a field is populated is NOT sufficient; these tests prove the
// rendered output actually contains the data.

test "open_questions: renderNavigator renders question titles in screen buffer (task 4023 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','First Question','body1','open')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','Second Question','body2','open')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
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

    renderNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS: both titles must appear in the rendered output.
    try testing.expect(std.mem.indexOf(u8, text, "First Question") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Second Question") != null);
    // Status must appear ("open").
    try testing.expect(std.mem.indexOf(u8, text, "open") != null);
}

test "open_questions: renderNavigator shows empty state with filter label (task 4023 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
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

    // Empty state must mention "no questions" and the filter name.
    try testing.expect(std.mem.indexOf(u8, text, "no questions") != null);
    try testing.expect(std.mem.indexOf(u8, text, "open") != null);
}

test "open_questions: renderDetail renders body AND linked section (task 4023 render-level)" {
    // Critical render-level test for tasks 4023/4024.
    // Seeds a question with a body and a linked plan, then verifies
    // BOTH appear in the rendered detail pane output.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const qid = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','RenderQuestion','The render question body.','open')",
        &.{},
    );
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','RenderPlan','render-plan','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('question', ?, 'plan', ?, 'addresses')",
        &.{ .{ .int = qid }, .{ .int = pid } },
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };
    // Struct-level sanity check.
    try testing.expectEqual(@as(usize, 1), detail.linked.len);
    try testing.expect(std.mem.indexOf(u8, detail.body, "The render question body.") != null);

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

    var test_arena = std.heap.ArenaAllocator.init(a);
    defer test_arena.deinit();
    try renderDetail(&state, detail_win, test_arena.allocator());

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS:
    // (a) Title must appear in the rendered output (heap-allocated, stable pointers).
    try testing.expect(std.mem.indexOf(u8, text, "RenderQuestion") != null);
    // (b) "Linked to:" header must appear — task 4023 requires it.
    try testing.expect(std.mem.indexOf(u8, text, "Linked to:") != null);
    // (c) The linked plan's label must appear (heap-allocated label slice, stable).
    try testing.expect(std.mem.indexOf(u8, text, "RenderPlan") != null);
    // (d) The relationship must appear.
    try testing.expect(std.mem.indexOf(u8, text, "addresses") != null);
    // (e) Body text is arena-backed after UAF fix (task 4198); verify it renders.
    try testing.expect(std.mem.indexOf(u8, text, "render question body") != null);
}

test "open_questions: renderDetail shows (none) when no linked entities (task 4023 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','StandaloneQ','Standalone body text.','open')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);

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
    try renderDetail(&state, detail_win, test_arena.allocator());

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // Title must appear.
    try testing.expect(std.mem.indexOf(u8, text, "StandaloneQ") != null);
    // "Linked to:" header must appear even when there are no linked entities.
    try testing.expect(std.mem.indexOf(u8, text, "Linked to:") != null);
    // "(none)" placeholder must appear when linked is empty.
    try testing.expect(std.mem.indexOf(u8, text, "(none)") != null);
    // Body text is arena-backed after UAF fix (task 4198); verify it renders.
    try testing.expect(std.mem.indexOf(u8, text, "Standalone body text") != null);
}

test "open_questions: renderNavigator changes rendered set when filter changes (task 4024 render-level)" {
    // Verify that filter cycling actually changes what is rendered.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','OpenOnly','b','open')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, body, status, answer_body, answered_at) values ('global','AnsweredOnly','b','answered','a','2025-01-01T00:00:00.000Z')",
        &.{},
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    const win_w: u16 = 80;
    const win_h: u16 = 10;

    // Render with filter=open.
    {
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
        // open filter: OpenOnly appears, AnsweredOnly does not.
        try testing.expect(std.mem.indexOf(u8, text, "OpenOnly") != null);
        try testing.expect(std.mem.indexOf(u8, text, "AnsweredOnly") == null);
    }

    // Switch to filter=answered.
    const f_key = Key{ .codepoint = 'f', .mods = .{} };
    _ = state.handleKey(f_key, &d);

    // Render with filter=answered.
    {
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
        // answered filter: AnsweredOnly appears, OpenOnly does not.
        try testing.expect(std.mem.indexOf(u8, text, "AnsweredOnly") != null);
        try testing.expect(std.mem.indexOf(u8, text, "OpenOnly") == null);
    }
}

test "open_questions: renderDetail shows Jump line when jump_target is set (task 4024 render-level)" {
    // Verifies that after pressing 'g', the Jump affordance appears rendered.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const qid = try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global','JumpRenderQ','body','open')",
        &.{},
    );
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','JumpTarget','jump-target','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('question', ?, 'plan', ?, 'addresses')",
        &.{ .{ .int = qid }, .{ .int = pid } },
    );

    var state = OpenQuestionsState.init(a);
    defer state.deinit();
    state.filter = .open;
    try state.reload(&d);

    // Press 'g' to set the jump target.
    const g_key = Key{ .codepoint = 'g', .mods = .{} };
    _ = state.handleKey(g_key, &d);
    try testing.expect(state.jump_target != null);

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
    try renderDetail(&state, detail_win, test_arena.allocator());

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // "Jump:" must appear in the rendered output when jump_target is set.
    try testing.expect(std.mem.indexOf(u8, text, "Jump:") != null);
    // The jump target label (containing the plan title) must appear.
    try testing.expect(std.mem.indexOf(u8, text, "JumpTarget") != null);
}

test "open_questions: legendLabel fits in buf" {
    var buf: [128]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Filter") != null);
    try testing.expect(std.mem.indexOf(u8, label, "Jump") != null);
}

test "open_questions compiles" {
    std.testing.refAllDecls(@This());
}
