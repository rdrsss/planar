//! cockpit/views/task_board.zig — Task Board view (M5).
//!
//! Renders a two-pane layout for the Task Board:
//!
//!   • Left pane (navigator): tasks grouped into four status columns:
//!       open (todo) / doing / blocked / done
//!     Each column shows a header and the tasks in priority order.
//!     j/k (or arrow keys) navigate the flat list; the column a task
//!     belongs to is shown by the section header.
//!
//!   • Right pane (detail): for the selected task, shows:
//!       - Core fields (status, priority, next_action, body)
//!       - Reopen history from task_reopens (task 4019)
//!       - Touch paths from task_touch_paths (task 4019)
//!       - Blocking/dependency links from entity_links (task 4020)
//!
//! Tasks 4018 (status columns), 4019 (reopens + touch paths),
//! 4020 (blocking/dependency links from entity_links).
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by BoardState and released via `deinit`.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread is spawned here;
//!     the existing wake integration (app.zig's wakeThreadFn) is reused.
//!   - Status enum values are those in migration 00003_work_items.up.sql:
//!       check(status in ('todo','doing','blocked','done','cancelled'))
//!     The board shows todo→open, doing, blocked, done. Cancelled excluded.
//!   - Blocking links use the 'depends-on' relationship from entity_links per
//!     migration 00004_entity_links.up.sql.

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");
const markdown_detail = @import("../widgets/markdown_detail.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// Flat list entry for the navigator pane
// =========================================================================

/// A single entry in the board's flat navigator list. Either a section
/// header (column divider) or a task row.
const ListEntry = union(enum) {
    /// Section header: "OPEN (N)" / "DOING (N)" / "BLOCKED (N)" / "DONE (N)".
    header: struct {
        label: []const u8,
    },
    /// Task row.
    task: struct {
        id: i64,
        title: []const u8,
        badge: view_model.StatusBadge,
        has_claim: bool,
    },

    /// Whether this entry is selectable (task rows only).
    pub fn isSelectable(self: ListEntry) bool {
        return self == .task;
    }
};

// =========================================================================
// BoardState
// =========================================================================

/// All mutable state for the Task Board view.
pub const BoardState = struct {
    allocator: std.mem.Allocator,

    /// Current snapshot of tasks, grouped by status.
    snapshot: ?view_model.TaskBoardSnapshot = null,

    /// Flat list of entries for the navigator pane. Rebuilt on reload.
    /// The header strings point into comptime literals; task titles are
    /// heap-allocated strings owned by this struct.
    entries: []ListEntry = &.{},

    /// Navigator selection index (into the visible selectable rows).
    selected_idx: usize = 0,

    /// Navigator scroll offset.
    scroll_offset: usize = 0,

    /// Detail pane data for the currently selected task. Null when nothing
    /// is selected or the snapshot is empty.
    detail: ?view_model.TaskBoardDetail = null,

    /// Scope filter for the board (default: .all).
    filter: view_model.ScopeFilter = .all,

    pub fn init(allocator: std.mem.Allocator) BoardState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *BoardState) void {
        self.freeEntries();
        if (self.snapshot) |s| s.deinit(self.allocator);
        if (self.detail) |d| d.deinit(self.allocator);
    }

    fn freeEntries(self: *BoardState) void {
        // Both header labels and task titles are heap-allocated.
        for (self.entries) |entry| {
            switch (entry) {
                .task => |t| self.allocator.free(t.title),
                .header => |h| self.allocator.free(h.label),
            }
        }
        self.allocator.free(self.entries);
        self.entries = &.{};
    }

    /// Reload all board data from the DB. Called on db_changed and on launch.
    pub fn reload(self: *BoardState, d: *db.sqlite.Db) !void {
        const snap = try view_model.queryTaskBoard(d, self.allocator, self.filter);
        var snap_owned = true;
        errdefer if (snap_owned) snap.deinit(self.allocator);

        // Build the flat entry list.
        var out: std.ArrayList(ListEntry) = .empty;
        errdefer {
            for (out.items) |e| {
                switch (e) {
                    .task => |t| self.allocator.free(t.title),
                    .header => |h| self.allocator.free(h.label),
                }
            }
            out.deinit(self.allocator);
        }

        try appendSection(self.allocator, &out, "OPEN", snap.open);
        try appendSection(self.allocator, &out, "DOING", snap.doing);
        try appendSection(self.allocator, &out, "BLOCKED", snap.blocked);
        try appendSection(self.allocator, &out, "DONE", snap.done);

        const new_entries = try out.toOwnedSlice(self.allocator);

        // Swap only after the complete replacement snapshot is available.
        self.freeEntries();
        if (self.snapshot) |s| s.deinit(self.allocator);
        if (self.detail) |det| det.deinit(self.allocator);
        self.detail = null;
        self.snapshot = snap;
        snap_owned = false;
        self.entries = new_entries;

        // Clamp selection.
        const selectable = self.selectableCount();
        if (self.selected_idx >= selectable and selectable > 0) {
            self.selected_idx = selectable - 1;
        }

        // Refresh detail for the new selection.
        try self.refreshDetail(d);
    }

    /// Number of selectable (task) entries in the flat list.
    pub fn selectableCount(self: *const BoardState) usize {
        var count: usize = 0;
        for (self.entries) |e| {
            if (e == .task) count += 1;
        }
        return count;
    }

    /// The task entry at the current selected_idx, or null.
    pub fn selectedTask(self: *const BoardState) ?struct { id: i64, title: []const u8 } {
        var selectable: usize = 0;
        for (self.entries) |e| {
            switch (e) {
                .task => |t| {
                    if (selectable == self.selected_idx) {
                        return .{ .id = t.id, .title = t.title };
                    }
                    selectable += 1;
                },
                .header => {},
            }
        }
        return null;
    }

    pub fn focusTask(self: *BoardState, d: *db.sqlite.Db, task_id: i64) !bool {
        var selectable: usize = 0;
        for (self.entries) |entry| switch (entry) {
            .task => |task| {
                if (task.id == task_id) {
                    self.selected_idx = selectable;
                    try self.refreshDetail(d);
                    return true;
                }
                selectable += 1;
            },
            .header => {},
        };
        return false;
    }

    /// Refresh the detail pane for the currently selected task.
    pub fn refreshDetail(self: *BoardState, d: *db.sqlite.Db) !void {
        if (self.detail) |det| {
            det.deinit(self.allocator);
            self.detail = null;
        }

        const sel = self.selectedTask() orelse return;
        self.detail = try view_model.queryTaskBoardDetail(d, self.allocator, sel.id);
    }

    /// Handle a key event. Returns true when the key was consumed.
    pub fn handleKey(self: *BoardState, key: Key, d: *db.sqlite.Db) bool {
        const selectable = self.selectableCount();

        // j / arrow-down: move selection down.
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            if (selectable > 0 and self.selected_idx + 1 < selectable) {
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
// Helpers
// =========================================================================

/// Append a section header + task rows for one board column.
fn appendSection(
    allocator: std.mem.Allocator,
    out: *std.ArrayList(ListEntry),
    column_name: []const u8,
    tasks: []const view_model.BoardTaskRow,
) !void {
    // Section header: "OPEN (3)" etc. Use a fixed-size stack buffer for
    // the label to avoid allocation for these ephemeral strings.
    var hdr_buf: [64]u8 = undefined;
    const hdr_label = std.fmt.bufPrint(
        &hdr_buf,
        "{s} ({d})",
        .{ column_name, tasks.len },
    ) catch column_name;

    // Header entry holds a comptime-derived slice. We need to heap-dup it
    // so the lifetime is tied to the entry, not the stack buffer.
    const hdr_owned = try allocator.dupe(u8, hdr_label);
    errdefer allocator.free(hdr_owned);

    try out.append(allocator, .{ .header = .{ .label = hdr_owned } });

    for (tasks) |task| {
        const title = try allocator.dupe(u8, task.title);
        errdefer allocator.free(title);

        try out.append(allocator, .{ .task = .{
            .id = task.id,
            .title = title,
            .badge = task.status_badge,
            .has_claim = task.claim_token != null,
        } });
    }
}

// =========================================================================
// Render
// =========================================================================

/// Render the Task Board into the navigator and detail windows.
///
/// Navigator (left pane): flat list of section headers + task rows.
/// Detail (right pane): detail for the selected task (core fields,
/// reopens, touch paths, blocking links).
pub fn render(
    state: *const BoardState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    renderNavigator(state, nav_win, allocator);
    try renderDetail(state, detail_win, allocator);
}

/// Render the navigator pane (left).
fn renderNavigator(state: *const BoardState, win: Window, arena: std.mem.Allocator) void {
    if (win.height == 0 or win.width == 0) return;

    if (state.entries.len == 0) {
        _ = win.printSegment(.{
            .text = "(no tasks in scope)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    // Compute the visible window into the flat list based on scroll_offset.
    // We scroll by selectable-entry positions; the scroll_offset tracks how
    // many selectable entries are above the viewport.
    // For simplicity we use a flat row-based scroll (one screen line = one
    // entry). The selected entry is always in view.
    var display_row: u16 = 0;
    var selectable_idx: usize = 0;

    // Compute scroll to ensure selected is visible.
    // Count entries before the selected task (including headers in the way).
    var lines_before_selected: usize = 0;
    var cur_selectable: usize = 0;
    for (state.entries) |e| {
        if (cur_selectable == state.selected_idx and e == .task) break;
        lines_before_selected += 1;
        if (e == .task) cur_selectable += 1;
    }

    // Effective scroll: show the selected entry within the viewport.
    const viewport_h: usize = @intCast(win.height);
    const scroll: usize = if (lines_before_selected >= viewport_h)
        lines_before_selected - viewport_h + 1
    else
        0;

    var flat_row: usize = 0;
    for (state.entries) |entry| {
        if (flat_row < scroll) {
            flat_row += 1;
            if (entry == .task) {
                selectable_idx += 1;
            }
            continue;
        }
        if (display_row >= win.height) break;

        switch (entry) {
            .header => |h| {
                _ = win.printSegment(.{
                    .text = h.label,
                    .style = .{ .bold = true, .dim = true },
                }, .{ .row_offset = display_row, .col_offset = 0 });
            },
            .task => |t| {
                const is_selected = (selectable_idx == state.selected_idx);

                // Build the row text: "[badge] [*] title"
                // [*] marks a task with an active claim.
                // Use arena allocation so the slice remains valid through
                // vaxis.render() — a stack-local buf would dangle after
                // renderNavigator returns.
                const claim_marker: []const u8 = if (t.has_claim) "[*] " else "    ";
                const text = std.fmt.allocPrint(
                    arena,
                    "[{s}] {s}{s}",
                    .{ t.badge.glyph(), claim_marker, t.title },
                ) catch t.title;

                const style: Style = if (is_selected)
                    .{ .bold = true, .reverse = true }
                else
                    .{};

                _ = win.printSegment(.{
                    .text = text,
                    .style = style,
                }, .{ .row_offset = display_row, .col_offset = 0 });

                selectable_idx += 1;
            },
        }

        display_row += 1;
        flat_row += 1;
    }
}

/// Render the detail pane (right).
///
/// Layout (top to bottom):
///   row 0           — task title (bold)
///   rows 1..body_h  — markdown body (status, priority, next_action, body text)
///   separator       — blank row before supplemental sections
///   "Reopen history" section (4019)
///   "Touch paths" section (4019)
///   "Blocked by:" / "Blocks:" sections (4020)
fn renderDetail(state: *const BoardState, win: Window, arena: std.mem.Allocator) !void {
    if (win.height == 0 or win.width == 0) return;

    const detail = state.detail orelse {
        _ = win.printSegment(.{
            .text = "(no task selected)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    };

    var row: u16 = 0;

    // Title row.
    // Slice into detail.title (heap-allocated) for each byte — stable
    // grapheme pointer that outlives renderDetail's stack frame.
    if (detail.title.len > 0 and row < win.height) {
        const title_style: Style = .{ .bold = true };
        var col: usize = 0;
        var graphemes = vaxis.unicode.graphemeIterator(detail.title);
        while (graphemes.next()) |item| {
            const grapheme = item.bytes(detail.title);
            const width = win.gwidth(grapheme);
            if (width == 0) continue;
            const cell_width: usize = width;
            if (col + cell_width > win.width) break;
            win.writeCell(@intCast(col), row, .{
                .char = .{ .grapheme = grapheme, .width = @intCast(width) },
                .style = title_style,
            });
            col += cell_width;
        }
        row += 1;
    }

    // Compute how many rows the supplemental sections need so we can
    // carve that space from the bottom and give the markdown body the rest.
    //
    // Supplemental layout (each section only rendered when non-empty):
    //   1 separator row (blank)
    //   reopens:     1 header + N rows (when detail.reopens.len > 0)
    //   touch_paths: 1 header + N rows (when detail.touch_paths.len > 0)
    //   blocked_by:  1 header + N rows (when any blocks_this link exists)
    //   blocks:      1 header + N rows (when any this_blocks link exists)
    var blocks_this_count: usize = 0;
    var this_blocks_count: usize = 0;
    for (detail.links) |lnk| {
        switch (lnk.direction) {
            .blocks_this => blocks_this_count += 1,
            .this_blocks => this_blocks_count += 1,
        }
    }

    var supp_rows: u16 = 0;
    const has_supplemental = detail.reopens.len > 0 or
        detail.touch_paths.len > 0 or
        detail.links.len > 0;
    if (has_supplemental) {
        supp_rows += 1; // separator
        if (detail.reopens.len > 0)
            supp_rows += 1 + @as(u16, @intCast(detail.reopens.len));
        if (detail.touch_paths.len > 0)
            supp_rows += 1 + @as(u16, @intCast(detail.touch_paths.len));
        if (blocks_this_count > 0)
            supp_rows += 1 + @as(u16, @intCast(blocks_this_count));
        if (this_blocks_count > 0)
            supp_rows += 1 + @as(u16, @intCast(this_blocks_count));
    }

    // Give the markdown body the rows between the title and the supplemental
    // section. Reserve at least 1 row for the body.
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
        // Pass the frame arena to markdown_detail.render so its internal
        // writeCell calls for individual characters use arena-allocated
        // grapheme slices that outlive renderDetail's stack frame.
        try markdown_detail.render(body_win, arena, detail.body);
        row = body_start + body_h;
    }

    if (!has_supplemental) return;
    if (row >= win.height) return;

    // Separator row.
    row += 1;
    if (row >= win.height) return;

    // "Reopen history" section (task 4019).
    if (detail.reopens.len > 0 and row < win.height) {
        _ = win.printSegment(.{
            .text = "Reopen history",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;

        for (detail.reopens) |reo| {
            if (row >= win.height) break;
            // Use arena allocation so the slice remains valid through vaxis.render().
            const reason_part = reo.reason orelse "";
            const text = if (reason_part.len > 0)
                std.fmt.allocPrint(arena, "  {s} -> {s}  [{s}]  {s}", .{
                    reo.from_status, reo.to_status, reo.source, reason_part,
                }) catch reo.from_status
            else
                std.fmt.allocPrint(arena, "  {s} -> {s}  [{s}]", .{
                    reo.from_status, reo.to_status, reo.source,
                }) catch reo.from_status;
            _ = win.printSegment(.{
                .text = text,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 0 });
            row += 1;
        }
    }

    // "Touch paths" section (task 4019).
    if (detail.touch_paths.len > 0 and row < win.height) {
        _ = win.printSegment(.{
            .text = "Touch paths",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;

        for (detail.touch_paths) |tp| {
            if (row >= win.height) break;
            // Two printSegments: indent (string literal, permanently valid
            // grapheme pointers) + path (heap-owned, permanently valid).
            _ = win.printSegment(.{
                .text = "  ",
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{
                .text = tp.path,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 2 });
            row += 1;
        }
    }

    // "Blocked by:" section — tasks that block this task (task 4020).
    if (blocks_this_count > 0 and row < win.height) {
        _ = win.printSegment(.{
            .text = "Blocked by:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;

        for (detail.links) |lnk| {
            if (lnk.direction != .blocks_this) continue;
            if (row >= win.height) break;
            _ = win.printSegment(.{
                .text = "  ",
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{
                .text = lnk.label,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 2 });
            row += 1;
        }
    }

    // "Blocks:" section — tasks this task blocks (task 4020).
    if (this_blocks_count > 0 and row < win.height) {
        _ = win.printSegment(.{
            .text = "Blocks:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;

        for (detail.links) |lnk| {
            if (lnk.direction != .this_blocks) continue;
            if (row >= win.height) break;
            _ = win.printSegment(.{
                .text = "  ",
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{
                .text = lnk.label,
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

test "task_board: BoardState init and deinit are clean" {
    var state = BoardState.init(testing.allocator);
    defer state.deinit();
    try testing.expectEqual(@as(usize, 0), state.selectableCount());
    try testing.expectEqual(@as(?view_model.TaskBoardDetail, null), state.detail);
}

test "task_board: reload on empty DB yields no entries (task 4018 empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = BoardState.init(a);
    defer state.deinit();

    try state.reload(&d);

    // Still has 4 section headers (one per column) but no task entries.
    try testing.expectEqual(@as(usize, 0), state.selectableCount());
    try testing.expectEqual(@as(?view_model.TaskBoardDetail, null), state.detail);

    // Entries include headers for each of the 4 columns.
    var header_count: usize = 0;
    for (state.entries) |e| {
        if (e == .header) header_count += 1;
    }
    try testing.expectEqual(@as(usize, 4), header_count);
}

test "task_board: reload populates four columns with correct tasks (task 4018)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert one task per board status.
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','TodoTask','todo',1)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','DoingTask','doing',2)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','BlockedTask','blocked',3)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','DoneTask','done',4)",
        &.{},
    );
    // Cancelled must not appear.
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','CancelledTask','cancelled',5)",
        &.{},
    );

    var state = BoardState.init(a);
    defer state.deinit();

    try state.reload(&d);

    // 4 task entries + 4 headers = 8 entries.
    try testing.expectEqual(@as(usize, 4), state.selectableCount());

    // Verify snapshot column counts.
    const snap = state.snapshot.?;
    try testing.expectEqual(@as(usize, 1), snap.open.len);
    try testing.expectEqual(@as(usize, 1), snap.doing.len);
    try testing.expectEqual(@as(usize, 1), snap.blocked.len);
    try testing.expectEqual(@as(usize, 1), snap.done.len);

    // Column order in flat list: open header, todo task, doing header,
    // doing task, blocked header, blocked task, done header, done task.
    try testing.expectEqual(@as(usize, 8), state.entries.len);
    try testing.expect(state.entries[0] == .header); // OPEN (1)
    try testing.expect(state.entries[1] == .task); // TodoTask
    try testing.expect(state.entries[2] == .header); // DOING (1)
    try testing.expect(state.entries[3] == .task); // DoingTask
    try testing.expect(state.entries[4] == .header); // BLOCKED (1)
    try testing.expect(state.entries[5] == .task); // BlockedTask
    try testing.expect(state.entries[6] == .header); // DONE (1)
    try testing.expect(state.entries[7] == .task); // DoneTask
}

test "task_board: section headers contain task counts (task 4018)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','T1','todo',1)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','T2','todo',2)",
        &.{},
    );

    var state = BoardState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // First entry is the OPEN header; it must contain "(2)".
    const first = state.entries[0];
    try testing.expect(first == .header);
    try testing.expect(std.mem.indexOf(u8, first.header.label, "OPEN") != null);
    try testing.expect(std.mem.indexOf(u8, first.header.label, "(2)") != null);
}

test "task_board: selectedTask returns first task at idx=0" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Only','todo')",
        &.{},
    );

    var state = BoardState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const sel = state.selectedTask();
    try testing.expect(sel != null);
    try testing.expectEqualStrings("Only", sel.?.title);
}

test "task_board: selectedTask returns null when no tasks" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = BoardState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(?@TypeOf(state.selectedTask().?), null), state.selectedTask());
}

test "task_board: handleKey j/k moves selection" {
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

    var state = BoardState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 0), state.selected_idx);

    // j moves down.
    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    _ = state.handleKey(j_key, &d);
    try testing.expectEqual(@as(usize, 1), state.selected_idx);

    // k moves up.
    const k_key = Key{ .codepoint = 'k', .mods = .{} };
    _ = state.handleKey(k_key, &d);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "task_board: handleKey j at end of list does not overflow" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Only','todo')",
        &.{},
    );

    var state = BoardState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    _ = state.handleKey(j_key, &d);
    // Still at 0 (only one selectable row).
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "task_board: handleKey k at start of list does not underflow" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Only','todo')",
        &.{},
    );

    var state = BoardState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const k_key = Key{ .codepoint = 'k', .mods = .{} };
    _ = state.handleKey(k_key, &d);
    // Still at 0.
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "task_board: detail pane shows reopens and touch paths (task 4019)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','ReopenedTask','doing')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into task_reopens (task_id, from_status, to_status, source)
        \\values (?, 'done', 'doing', 'task-reopen')
    , &.{.{ .int = tid }});
    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('board-repo','BoardRepo')",
        &.{},
    );
    _ = try d.execParams(
        "insert into task_touch_paths (task_id, repo_id, path) values (?, ?, 'board/file.zig')",
        &.{ .{ .int = tid }, .{ .int = proj_id } },
    );

    var state = BoardState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Select the task.
    try state.refreshDetail(&d);
    const detail = state.detail orelse {
        try testing.expect(false); // should not be null
        return;
    };

    try testing.expectEqual(@as(usize, 1), detail.reopens.len);
    try testing.expectEqualStrings("done", detail.reopens[0].from_status);
    try testing.expectEqual(@as(usize, 1), detail.touch_paths.len);
    try testing.expectEqualStrings("board/file.zig", detail.touch_paths[0].path);
}

test "task_board: detail pane shows blocking links (task 4020)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const blocked_task = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Blockee','blocked')",
        &.{},
    );
    const blocker_task = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Blocker','done')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = blocker_task }, .{ .int = blocked_task } },
    );

    var state = BoardState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Navigate to the blocked task (first in the BLOCKED column = 3rd selectable
    // after any OPEN and DOING tasks; but here there's only one task per column so
    // it's at index 1 since OPEN is empty and DOING is empty, wait — let's check).
    // With only 2 tasks: blocker is 'done', blockee is 'blocked'.
    // Columns: OPEN(0), DOING(0), BLOCKED(1), DONE(1).
    // Selectable order: blocked_task(idx=0), blocker_task(idx=1).
    try testing.expectEqual(@as(usize, 2), state.selectableCount());
    state.selected_idx = 0; // Select the blocked task.
    try state.refreshDetail(&d);

    const detail = state.detail orelse {
        try testing.expect(false);
        return;
    };
    try testing.expectEqual(@as(usize, 1), detail.links.len);
    try testing.expectEqual(view_model.LinkDirection.blocks_this, detail.links[0].direction);
    try testing.expect(std.mem.indexOf(u8, detail.links[0].label, "Blocker") != null);
}

/// Extract all non-empty grapheme text from a Screen's cell buffer into a
/// single flat string. Used by render-level tests to assert that specific
/// text actually appears in the rendered output.
///
/// Each cell's char.grapheme slice may point into stack memory owned by the
/// printSegment call that wrote it. Because printSegment passes a slice of the
/// original text argument (which lives in the test's static memory or the
/// detail struct), the grapheme pointers remain valid for the duration of the
/// test. The result is appended into `out`; caller owns the result.
fn collectScreenText(screen: *const vaxis.Screen, out: *std.ArrayList(u8)) !void {
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) {
            try out.appendSlice(testing.allocator, g);
        }
    }
}

test "task_board: renderDetail renders reopens + touch_paths + links (tasks 4019/4020 render-level)" {
    // Red test (before the renderDetail fix): verifies that supplemental
    // sections actually appear in the rendered output, not just in the
    // detail struct.  This test should FAIL on the old renderDetail (which
    // only rendered title + body) and PASS after the fix.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Seed task with reopen, touch path, and a blocking link.
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','RenderTask','blocked')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into task_reopens (task_id, from_status, to_status, source)
        \\values (?, 'done', 'blocked', 'task-reopen')
    , &.{.{ .int = tid }});
    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('render-repo','RenderRepo')",
        &.{},
    );
    _ = try d.execParams(
        "insert into task_touch_paths (task_id, repo_id, path) values (?, ?, 'render/path.zig')",
        &.{ .{ .int = tid }, .{ .int = proj_id } },
    );
    const blocker_id = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','BlockerTask','done')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = blocker_id }, .{ .int = tid } },
    );
    // Also seed a this_blocks link: tid blocks another task.
    const blockee_id = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','BlockeeTask','todo')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = tid }, .{ .int = blockee_id } },
    );

    var state = BoardState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Navigate to RenderTask (it's in the blocked column).
    // Three tasks: blocker (done, idx=1), blockee (todo, idx=0 in OPEN),
    // RenderTask (blocked, idx in BLOCKED). We need to find and select it.
    var found_idx: ?usize = null;
    var sel_idx: usize = 0;
    for (state.entries) |e| {
        switch (e) {
            .task => |t| {
                if (std.mem.eql(u8, t.title, "RenderTask")) {
                    found_idx = sel_idx;
                }
                sel_idx += 1;
            },
            .header => {},
        }
    }
    try testing.expect(found_idx != null);
    state.selected_idx = found_idx.?;
    try state.refreshDetail(&d);

    // Verify the detail struct has the expected data (struct-level sanity).
    const detail = state.detail orelse {
        try testing.expect(false); // must not be null
        return;
    };
    try testing.expectEqual(@as(usize, 1), detail.reopens.len);
    try testing.expectEqual(@as(usize, 1), detail.touch_paths.len);
    try testing.expectEqual(@as(usize, 2), detail.links.len);

    // Build a real Screen backed by allocated cells and a Window over it.
    // Use a generous 80x40 terminal for the detail pane.
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

    // Render the detail pane into the real screen.
    // Use an arena so that allocPrint calls inside renderDetail do not
    // leak when inspected by testing.allocator (render functions own no
    // arena; the arena is reset each frame by renderFrame).
    var render_arena = std.heap.ArenaAllocator.init(a);
    defer render_arena.deinit();
    try renderDetail(&state, detail_win, render_arena.allocator());

    // Collect all rendered text from the screen buffer.
    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // Assert that the supplemental sections are present in the rendered output.
    // These assertions FAIL on the old renderDetail (which only rendered body
    // via markdown_detail and did not emit supplemental rows).
    try testing.expect(std.mem.indexOf(u8, text, "Reopen history") != null);
    try testing.expect(std.mem.indexOf(u8, text, "render/path.zig") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Blocked by:") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Blocks:") != null);
    // Verify the blocker and blockee labels appear.
    try testing.expect(std.mem.indexOf(u8, text, "BlockerTask") != null);
    try testing.expect(std.mem.indexOf(u8, text, "BlockeeTask") != null);
}

test "task_board: renderDetail — non-ASCII title characters are NOT dropped (task 4195)" {
    // Regression test for the byte-drop bug fixed in task 4195.
    //
    // The bug: the title render loop gated on `byte & 0x80 == 0` and silently
    // dropped every high byte — a title like "Café•λ" rendered as "Caf".
    //
    // RED-BEFORE: assertions for é (0xC3 0xA9), • (0xE2 0x80 0xA2), and
    // λ (0xCE 0xBB) all FAILED (indexOf returned null).
    // GREEN-AFTER: all assertions pass after converting to cpSeqLen iteration.
    const a = testing.allocator;
    // "Café•λ" — 'é' is 2-byte (0xC3 0xA9), '•' is 3-byte (0xE2 0x80 0xA2),
    // 'λ' is 2-byte (0xCE 0xBB).
    const non_ascii_title = "Caf\xc3\xa9\xe2\x80\xa2\xce\xbb";

    var state = BoardState.init(a);
    defer state.deinit();
    // Inject a detail pane with a non-ASCII title directly (no DB needed).
    state.detail = view_model.TaskBoardDetail{
        .id = 1,
        .title = try a.dupe(u8, non_ascii_title),
        .body = try a.dupe(u8, ""),
        .reopens = try a.alloc(view_model.TaskReopenRow, 0),
        .touch_paths = try a.alloc(view_model.TaskTouchPathRow, 0),
        .links = try a.alloc(view_model.TaskLinkRow, 0),
    };

    const win_w: u16 = 80;
    const win_h: u16 = 6;
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

    var render_arena = std.heap.ArenaAllocator.init(a);
    defer render_arena.deinit();
    try renderDetail(&state, detail_win, render_arena.allocator());

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // No replacement characters.
    try testing.expect(std.mem.indexOf(u8, text, "\xef\xbf\xbd") == null);
    // ASCII prefix must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Caf") != null);
    // é (U+00E9) must appear — 2-byte sequence.
    try testing.expect(std.mem.indexOf(u8, text, "\xc3\xa9") != null);
    // • (U+2022) must appear — 3-byte sequence.
    try testing.expect(std.mem.indexOf(u8, text, "\xe2\x80\xa2") != null);
    // λ (U+03BB) must appear — 2-byte sequence.
    try testing.expect(std.mem.indexOf(u8, text, "\xce\xbb") != null);
}

test "task_board: legendLabel fits in buf" {
    var buf: [128]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Quit") != null);
}

test "task_board compiles" {
    std.testing.refAllDecls(@This());
}
