//! cockpit/views/external_ops_plane.zig — External / Ops Plane view (M10).
//!
//! Renders a two-pane layout for the External/Ops Plane:
//!
//!   • Left pane (navigator): list of registered external systems, each
//!     with its link count and aggregate conflict/error tally.
//!     Selecting a system filters the detail pane to that system's links
//!     and most-recent sync event. 'c' toggles to the conflicts surface.
//!
//!   • Right pane (detail): in systems mode, shows for the selected system:
//!       - System identity: kind, slug, base_url, default_project
//!       - Latest sync status from sync_events (task 4030)
//!       - Per-link table: entity ref → external id → role → status
//!
//!     In conflicts mode:
//!       - "Unresolved conflicts:" section (task 4031)
//!       - One row per unresolved sync conflict with scope:direction [time]
//!         and optional detail text
//!       - "(none — all conflicts resolved)" when there are no unresolved conflicts
//!
//! Tasks:
//!   4029 (List external_systems and external_links with mapping status)
//!   4030 (Sync status and last-sync from sync_events)
//!   4031 (Surface unresolved sync conflicts)
//!
//! Acceptance invariants:
//!   (4029) Every queried datum from external_systems and external_links is
//!          rendered: system kind + slug + link count; per-link entity ref,
//!          external id, link role, sync direction, and last_sync_status.
//!   (4030) The latest sync event (at + outcome + direction) for the selected
//!          system is rendered in the detail pane.
//!   (4031) Unresolved conflicts are surfaced in the conflicts surface.
//!          A "no conflicts" clean fixture shows "(none)" explicitly.
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by ExtOpsState and released via deinit.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread.
//!   - MEMORY GUARD (brief rule (c)): display_text strings are always freshly
//!     allocated in view_model; entity_title on OOM falls back to a freshly
//!     duped placeholder, never an alias into kind/id pointers.

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

/// Whether to show the systems+links surface or the conflicts surface.
pub const DisplayMode = enum {
    /// Show external systems list with link detail.
    systems,
    /// Show the unresolved-conflicts surface.
    conflicts,
};

// =========================================================================
// ExtOpsState
// =========================================================================

/// All mutable state for the External/Ops Plane view.
pub const ExtOpsState = struct {
    allocator: std.mem.Allocator,

    /// Current snapshot (all systems, links, sync_status, conflicts).
    snapshot: ?view_model.ExtOpsSnapshot = null,

    /// Navigator selection index (0-based into snapshot.systems).
    selected_idx: usize = 0,

    /// Whether to show the systems surface or the conflicts surface.
    mode: DisplayMode = .systems,

    pub fn init(allocator: std.mem.Allocator) ExtOpsState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *ExtOpsState) void {
        if (self.snapshot) |snap| snap.deinit(self.allocator);
        self.snapshot = null;
    }

    /// Reload all data from the DB. Called on db_changed and on initial launch.
    pub fn reload(self: *ExtOpsState, d: *db.sqlite.Db) !void {
        if (self.snapshot) |snap| snap.deinit(self.allocator);
        self.snapshot = null;

        const snap = try view_model.queryExtOpsSnapshot(d, self.allocator);
        self.snapshot = snap;

        // Clamp selection.
        const sys_count = snap.systems.len;
        if (sys_count > 0) {
            if (self.selected_idx >= sys_count) {
                self.selected_idx = sys_count - 1;
            }
        } else {
            self.selected_idx = 0;
        }
    }

    /// Handle a key event. Returns true when the key was consumed.
    pub fn handleKey(self: *ExtOpsState, key: Key) bool {
        const snap = self.snapshot orelse return false;
        const count = snap.systems.len;

        // j / arrow-down: move selection down (only in systems mode).
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            if (self.mode == .systems and count > 0 and self.selected_idx + 1 < count) {
                self.selected_idx += 1;
            }
            return true;
        }
        // k / arrow-up: move selection up.
        if (key.matches('k', .{}) or key.matches(Key.up, .{})) {
            if (self.mode == .systems and self.selected_idx > 0) {
                self.selected_idx -= 1;
            }
            return true;
        }
        // 'c': toggle between systems list and conflicts surface (task 4031).
        if (key.matches('c', .{})) {
            self.mode = switch (self.mode) {
                .systems => .conflicts,
                .conflicts => .systems,
            };
            return true;
        }

        return false;
    }

    /// Look up the latest sync status for the currently selected system.
    /// Returns null when no sync events exist for the system.
    pub fn selectedSystemSyncStatus(self: *const ExtOpsState) ?view_model.ExtSystemSyncStatus {
        const snap = self.snapshot orelse return null;
        if (snap.systems.len == 0) return null;
        const sel_sys = snap.systems[self.selected_idx];
        for (snap.system_sync) |ss| {
            if (ss.system_id == sel_sys.id) return ss;
        }
        return null;
    }

    /// Return a slice of ExtLinkRow for the currently selected system.
    /// The caller must not free this slice — it points into snapshot.links.
    pub fn linksForSelected(self: *const ExtOpsState) []const view_model.ExtLinkRow {
        const snap = self.snapshot orelse return &.{};
        if (snap.systems.len == 0) return &.{};
        const sel_sys = snap.systems[self.selected_idx];
        // Links are ordered by system_id asc — find the contiguous slice.
        var start: ?usize = null;
        var end: usize = 0;
        for (snap.links, 0..) |link, i| {
            if (link.system_id == sel_sys.id) {
                if (start == null) start = i;
                end = i + 1;
            } else if (start != null) {
                break;
            }
        }
        if (start) |s| {
            return snap.links[s..end];
        }
        return &.{};
    }
};

// =========================================================================
// Render
// =========================================================================

/// Render the External/Ops Plane view into the navigator and detail windows.
pub fn render(
    state: *const ExtOpsState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    _ = allocator;
    switch (state.mode) {
        .systems => {
            renderSystemsNavigator(state, nav_win);
            try renderSystemsDetail(state, detail_win);
        },
        .conflicts => {
            renderConflictsNavigator(state, nav_win);
            renderConflictsDetail(state, detail_win);
        },
    }
}

// -------------------------------------------------------------------------
// Systems + links surface
// -------------------------------------------------------------------------

/// Render the navigator pane in systems mode.
///
/// Shows header + list of external systems.
/// Each row: "[kind] slug  (N links)"
/// Conflict/error tally shown on rows with non-zero counts.
/// Empty state: "(no external systems registered)"
fn renderSystemsNavigator(state: *const ExtOpsState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    const snap = state.snapshot orelse {
        _ = win.printSegment(.{
            .text = "(loading...)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    };

    if (snap.systems.len == 0) {
        _ = win.printSegment(.{
            .text = "(no external systems registered)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    // Header row.
    _ = win.printSegment(.{
        .text = "Systems  [c conflicts]",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });
    if (win.height < 2) return;

    const viewport_h: usize = if (win.height > 1) @as(usize, @intCast(win.height)) - 1 else 0;
    const scroll: usize = if (state.selected_idx >= viewport_h)
        state.selected_idx - viewport_h + 1
    else
        0;

    var display_row: u16 = 1;
    for (snap.systems, 0..) |sys, i| {
        if (i < scroll) continue;
        if (display_row >= win.height) break;

        const is_selected = (i == state.selected_idx);
        const style: Style = if (is_selected)
            .{ .bold = true, .reverse = true }
        else
            .{};

        // Use pre-formatted heap-allocated display_text.
        _ = win.printSegment(.{
            .text = sys.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });

        display_row += 1;

        // Conflict/error indicator on the next row (indented), when non-zero.
        if (display_row < win.height and (sys.conflict_count > 0 or sys.error_count > 0)) {
            var count_buf: [64]u8 = undefined;
            const indicator = std.fmt.bufPrint(
                &count_buf,
                "  ! conflicts:{d} errors:{d}",
                .{ sys.conflict_count, sys.error_count },
            ) catch "  ! (tally error)";
            _ = win.printSegment(.{
                .text = indicator,
                .style = .{ .bold = true },
            }, .{ .row_offset = display_row, .col_offset = 0 });
            display_row += 1;
        }
    }
}

/// Render the detail pane in systems mode.
///
/// Layout (top to bottom):
///   row 0              — system title: "[kind] slug" (bold)
///   row 1              — base_url (dim) or "(no base url)"
///   row 2              — default_project (dim) or blank
///   separator          — blank row
///   "Latest sync:"     — section header (bold dim)
///   1 row              — "  push  ok  2025-06-01T12:00Z" or "(never synced)"
///   separator          — blank row
///   "Links:"           — section header (bold dim)
///   N rows             — one per link: "entity_kind:id  ext_id  [role/dir/status]"
///                        OR "(no links for this system)"
///
/// INVARIANT (task 4029): ALL of kind, slug, link entity ref, external id,
/// link_role, sync_direction, and last_sync_status ARE rendered.
/// INVARIANT (task 4030): The latest sync event (at + outcome + direction)
/// is rendered when available.
fn renderSystemsDetail(state: *const ExtOpsState, win: Window) !void {
    if (win.height == 0 or win.width == 0) return;

    const snap = state.snapshot orelse {
        _ = win.printSegment(.{
            .text = "(no data)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    };

    if (snap.systems.len == 0) {
        _ = win.printSegment(.{
            .text = "(no external systems — register one with `planar ext system add`)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    const sel = snap.systems[state.selected_idx];
    var row: u16 = 0;

    // ---- System title (row 0): "[kind] slug" ----------------------------
    // Use separate printSegment calls for heap-allocated strings so grapheme
    // pointers remain valid after this function returns.
    if (row < win.height) {
        _ = win.printSegment(.{ .text = "[", .style = .{ .bold = true } }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{ .text = sel.kind, .style = .{ .bold = true } }, .{ .row_offset = row, .col_offset = 1 });
        const kind_len: u16 = @intCast(sel.kind.len);
        _ = win.printSegment(.{ .text = "] ", .style = .{ .bold = true } }, .{ .row_offset = row, .col_offset = 1 + kind_len });
        _ = win.printSegment(.{ .text = sel.slug, .style = .{ .bold = true } }, .{ .row_offset = row, .col_offset = 3 + kind_len });
        row += 1;
    }

    // ---- base_url (row 1) -----------------------------------------------
    if (row < win.height) {
        if (sel.base_url) |url| {
            _ = win.printSegment(.{
                .text = url,
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        } else {
            _ = win.printSegment(.{
                .text = "(no base url)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
        row += 1;
    }

    // ---- default_project (row 2) ----------------------------------------
    if (row < win.height) {
        if (sel.default_project) |proj| {
            _ = win.printSegment(.{ .text = "Project: ", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{ .text = proj, .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 9 });
        }
        row += 1;
    }

    // ---- Separator -------------------------------------------------------
    if (row < win.height) row += 1;

    // ---- Latest sync status (task 4030) ----------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "Latest sync:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }
    if (row < win.height) {
        if (state.selectedSystemSyncStatus()) |ss| {
            // Render: "  direction  outcome  at-prefix"
            // All fields are heap-allocated by view_model — use separate
            // printSegment calls so grapheme pointers remain valid after return.
            _ = win.printSegment(.{ .text = "  ", .style = .{} }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{ .text = ss.last_direction, .style = .{} }, .{ .row_offset = row, .col_offset = 2 });
            const dir_len: u16 = @intCast(ss.last_direction.len);
            _ = win.printSegment(.{ .text = "  ", .style = .{} }, .{ .row_offset = row, .col_offset = 2 + dir_len });
            _ = win.printSegment(.{ .text = ss.last_outcome, .style = .{} }, .{ .row_offset = row, .col_offset = 4 + dir_len });
            const out_len: u16 = @intCast(ss.last_outcome.len);
            _ = win.printSegment(.{ .text = "  ", .style = .{} }, .{ .row_offset = row, .col_offset = 4 + dir_len + out_len });
            // at is heap-allocated; render as much as fits.
            _ = win.printSegment(.{ .text = ss.last_sync_at, .style = .{} }, .{ .row_offset = row, .col_offset = 6 + dir_len + out_len });
        } else {
            _ = win.printSegment(.{
                .text = "  (never synced)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
        row += 1;
    }

    // ---- Separator -------------------------------------------------------
    if (row < win.height) row += 1;

    // ---- Links section (task 4029) ---------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "Links:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    const sys_links = state.linksForSelected();
    if (sys_links.len == 0) {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (no links for this system)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
        return;
    }

    // One row per link. Each link spans two to three screen rows:
    //   Row 1: "  " + display_text (heap: "entity_kind:id — status") + "  " + entity_title (heap)
    //   Row 2: "    ext:" + external_id (heap) + "  role:" + link_role (heap) +
    //          "  dir:" + sync_direction (heap)
    //   Row 3: "    synced:" + last_synced_at (heap) — only when last_synced_at is set
    //
    // MEMORY NOTE: all strings written via printSegment must be heap-allocated
    // or string literals so the grapheme pointers stored in the vaxis Screen cells
    // remain valid after renderSystemsDetail returns and collectScreenText runs.
    // display_text, entity_title, external_id, link_role, sync_direction,
    // last_sync_status, and last_synced_at are all heap-allocated in ExtLinkRow.
    // "    ext:", "  role:", "  dir:", "  " are string literals (static lifetime).
    //
    // INVARIANT (4029): ALL of entity ref (in display_text), external id,
    // link_role, sync_direction, and last_sync_status are rendered in every row.
    for (sys_links) |link| {
        if (row >= win.height) break;

        // Row 1: indent + display_text (heap) + "  " + entity_title (heap).
        // display_text = "entity_kind:id — last_sync_status" (fully heap-allocated).
        _ = win.printSegment(.{ .text = "  ", .style = .{} }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{ .text = link.display_text, .style = .{} }, .{ .row_offset = row, .col_offset = 2 });
        _ = win.printSegment(.{ .text = "  ", .style = .{} }, .{ .row_offset = row, .col_offset = 2 + @as(u16, @intCast(link.display_text.len)) });
        _ = win.printSegment(.{ .text = link.entity_title, .style = .{} }, .{ .row_offset = row, .col_offset = 4 + @as(u16, @intCast(link.display_text.len)) });
        row += 1;
        if (row >= win.height) break;

        // Row 2 (dim): external_id, link_role, sync_direction.
        // All heap-allocated; literals used for labels.
        _ = win.printSegment(.{ .text = "    ext:", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
        var col: u16 = 8;
        _ = win.printSegment(.{ .text = link.external_id, .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = col });
        col += @intCast(link.external_id.len);
        _ = win.printSegment(.{ .text = "  role:", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = col });
        col += 7;
        _ = win.printSegment(.{ .text = link.link_role, .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = col });
        col += @intCast(link.link_role.len);
        _ = win.printSegment(.{ .text = "  dir:", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = col });
        col += 6;
        _ = win.printSegment(.{ .text = link.sync_direction, .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = col });
        row += 1;

        // Row 3 (dim, optional): synced timestamp.
        if (link.last_synced_at) |ts| {
            if (row < win.height) {
                _ = win.printSegment(.{ .text = "    synced:", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
                _ = win.printSegment(.{ .text = ts, .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 11 });
                row += 1;
            }
        }
    }
}

// -------------------------------------------------------------------------
// Conflicts surface
// -------------------------------------------------------------------------

/// Render the navigator pane in conflicts mode.
///
/// Shows a summary of unresolved conflicts:
///   "Unresolved Conflicts  [c back]"
///   "  N unresolved"
///   "(no conflicts — all resolved)" when clean
fn renderConflictsNavigator(state: *const ExtOpsState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    var row: u16 = 0;

    _ = win.printSegment(.{
        .text = "Unresolved Conflicts  [c back]",
        .style = .{ .bold = true },
    }, .{ .row_offset = row, .col_offset = 0 });
    row += 1;
    if (row >= win.height) return;

    const snap = state.snapshot orelse {
        _ = win.printSegment(.{
            .text = "  (loading...)",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        return;
    };

    var count_buf: [64]u8 = undefined;
    const count = snap.conflicts.len;
    const count_line = std.fmt.bufPrint(
        &count_buf,
        "  {d} unresolved",
        .{count},
    ) catch "  (count error)";
    _ = win.printSegment(.{
        .text = count_line,
        .style = if (count > 0) .{ .bold = true } else .{ .dim = true },
    }, .{ .row_offset = row, .col_offset = 0 });
}

/// Render the detail pane in conflicts mode.
///
/// Layout:
///   "Unresolved conflicts:" — section header (bold dim)
///   N rows                  — one per conflict:
///     "  scope:direction [at-prefix]"
///     "    detail: <text>" (if detail is set)
///   OR "(none — all conflicts resolved)"
///
/// INVARIANT (task 4031): BOTH the clean-state "(none)" AND the conflict
/// rows are rendered — the operator must be able to confirm the check ran.
fn renderConflictsDetail(state: *const ExtOpsState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    var row: u16 = 0;

    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "Unresolved conflicts:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    const snap = state.snapshot orelse {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (data not loaded)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
        return;
    };

    if (snap.conflicts.len == 0) {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (none — all conflicts resolved)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
        return;
    }

    // One row (+ optional detail line) per unresolved conflict.
    // All strings are heap-allocated — stable grapheme pointers through render.
    for (snap.conflicts) |conflict| {
        if (row >= win.height) break;

        // Use the pre-formatted heap-allocated display_text.
        _ = win.printSegment(.{
            .text = conflict.display_text,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 2 });
        row += 1;

        // Optional detail line.
        if (conflict.detail) |det| {
            if (row < win.height) {
                _ = win.printSegment(.{
                    .text = "  detail: ",
                    .style = .{ .dim = true },
                }, .{ .row_offset = row, .col_offset = 2 });
                _ = win.printSegment(.{
                    .text = det,
                    .style = .{ .dim = true },
                }, .{ .row_offset = row, .col_offset = 12 });
                row += 1;
            }
        }
    }
}

/// Return a one-line legend string for the key legend bar.
pub fn legendLabel(buf: []u8) []const u8 {
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Select  c Conflicts/Systems  Tab Focus  1-8 View",
        .{},
    ) catch "  q Quit  j/k Select  c Conflicts/Systems";
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
/// single flat string. Used by render-level tests.
fn collectScreenText(screen: *const vaxis.Screen, out: *std.ArrayList(u8)) !void {
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) {
            try out.appendSlice(testing.allocator, g);
        }
    }
}

// -------------------------------------------------------------------------
// State lifecycle tests
// -------------------------------------------------------------------------

test "external_ops: init and deinit are clean" {
    var state = ExtOpsState.init(testing.allocator);
    defer state.deinit();
    try testing.expect(state.snapshot == null);
    try testing.expectEqual(DisplayMode.systems, state.mode);
}

test "external_ops: reload on empty DB yields empty snapshot (empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = ExtOpsState.init(a);
    defer state.deinit();

    try state.reload(&d);

    try testing.expect(state.snapshot != null);
    const snap = state.snapshot.?;
    try testing.expectEqual(@as(usize, 0), snap.systems.len);
    try testing.expectEqual(@as(usize, 0), snap.links.len);
    try testing.expectEqual(@as(usize, 0), snap.conflicts.len);
}

test "external_ops: reload populates systems and links (task 4029)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('github-issues','gh-org','gh-cli','gh')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Ext Task','todo')",
        &.{},
    );
    _ = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-77', 'ok')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );

    var state = ExtOpsState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const snap = state.snapshot.?;
    try testing.expectEqual(@as(usize, 1), snap.systems.len);
    try testing.expectEqualStrings("github-issues", snap.systems[0].kind);
    try testing.expectEqualStrings("gh-org", snap.systems[0].slug);
    try testing.expectEqual(@as(i64, 1), snap.systems[0].link_count);

    try testing.expectEqual(@as(usize, 1), snap.links.len);
    try testing.expectEqualStrings("task", snap.links[0].entity_kind);
    try testing.expectEqualStrings("I-77", snap.links[0].external_id);
    try testing.expectEqualStrings("ok", snap.links[0].last_sync_status);
    // Entity title resolved.
    try testing.expectEqualStrings("Ext Task", snap.links[0].entity_title);
}

test "external_ops: reload populates sync_events (task 4030)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-sync','token-env','J')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Sync Task','doing')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'JI-5', 'ok')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'push', 'ok', '2025-06-10T08:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    var state = ExtOpsState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const snap = state.snapshot.?;
    try testing.expectEqual(@as(usize, 1), snap.system_sync.len);
    try testing.expectEqual(sys_id, snap.system_sync[0].system_id);
    try testing.expectEqualStrings("ok", snap.system_sync[0].last_outcome);
    try testing.expectEqualStrings("push", snap.system_sync[0].last_direction);
    try testing.expect(std.mem.indexOf(u8, snap.system_sync[0].last_sync_at, "2025") != null);

    // selectedSystemSyncStatus must return this entry.
    const ss_opt = state.selectedSystemSyncStatus();
    try testing.expect(ss_opt != null);
    try testing.expectEqualStrings("ok", ss_opt.?.last_outcome);
}

test "external_ops: reload populates unresolved conflicts (task 4031)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-cf','token-env','J')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','CF Task','doing')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'JI-9', 'conflict')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, detail, at) values (?, 'external', 'pull', 'conflict', 'title conflict', '2025-03-01T09:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    var state = ExtOpsState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const snap = state.snapshot.?;
    try testing.expectEqual(@as(usize, 1), snap.conflicts.len);
    try testing.expectEqualStrings("external", snap.conflicts[0].scope);
    try testing.expectEqualStrings("pull", snap.conflicts[0].direction);
    try testing.expect(snap.conflicts[0].detail != null);
    try testing.expectEqualStrings("title conflict", snap.conflicts[0].detail.?);
}

test "external_ops: handleKey j/k moves selection" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Two systems.
    _ = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','s1','token-env','J1')",
        &.{},
    );
    _ = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('github-issues','s2','gh-cli','gh')",
        &.{},
    );

    var state = ExtOpsState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 0), state.selected_idx);

    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    _ = state.handleKey(j_key);
    try testing.expectEqual(@as(usize, 1), state.selected_idx);

    const k_key = Key{ .codepoint = 'k', .mods = .{} };
    _ = state.handleKey(k_key);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "external_ops: handleKey c toggles display mode (task 4031)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = ExtOpsState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(DisplayMode.systems, state.mode);

    const c_key = Key{ .codepoint = 'c', .mods = .{} };
    _ = state.handleKey(c_key);
    try testing.expectEqual(DisplayMode.conflicts, state.mode);

    _ = state.handleKey(c_key);
    try testing.expectEqual(DisplayMode.systems, state.mode);
}

// =========================================================================
// RENDER-LEVEL TESTS (rule (b): render into vaxis.Screen and assert text)
// =========================================================================

test "external_ops: renderSystemsNavigator shows system slug (task 4029 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('github-issues','my-org','gh-cli','gh')",
        &.{},
    );

    var state = ExtOpsState.init(a);
    defer state.deinit();
    try state.reload(&d);

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

    renderSystemsNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4029):
    // slug must appear.
    try testing.expect(std.mem.indexOf(u8, text, "my-org") != null);
    // kind must appear.
    try testing.expect(std.mem.indexOf(u8, text, "github-issues") != null);
    // Header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Systems") != null);
    // Link count ("0 links" or "links") must appear.
    try testing.expect(std.mem.indexOf(u8, text, "links") != null);
}

test "external_ops: renderSystemsNavigator shows empty state (render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = ExtOpsState.init(a);
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

    renderSystemsNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    try testing.expect(std.mem.indexOf(u8, text, "no external systems") != null);
}

test "external_ops: renderSystemsDetail renders system identity + links + sync status (tasks 4029/4030 render-level)" {
    // Critical render-level test: all queried data must appear in the rendered output.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('jira','corp-jira','https://corp.atlassian.net','MYPROJ','token-env','JIRA_TOKEN')",
        &.{},
    );
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Big Feature','big-feat','active')",
        &.{},
    );
    const link_id = try d.execParams(
        \\insert into external_links
        \\  (entity_kind, entity_id, system_id, external_id, link_role, sync_direction, last_sync_status)
        \\values ('plan', ?, ?, 'MYPROJ-100', 'mirror', 'two-way', 'ok')
    , &.{ .{ .int = pid }, .{ .int = sys_id } });
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'push', 'ok', '2025-05-01T11:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    var state = ExtOpsState.init(a);
    defer state.deinit();
    try state.reload(&d);

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

    try renderSystemsDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4029):
    // System identity.
    try testing.expect(std.mem.indexOf(u8, text, "jira") != null);
    try testing.expect(std.mem.indexOf(u8, text, "corp-jira") != null);
    try testing.expect(std.mem.indexOf(u8, text, "corp.atlassian.net") != null);
    try testing.expect(std.mem.indexOf(u8, text, "MYPROJ") != null);
    // Links section header.
    try testing.expect(std.mem.indexOf(u8, text, "Links:") != null);
    // Entity ref (plan:id).
    try testing.expect(std.mem.indexOf(u8, text, "plan:") != null);
    // Entity title.
    try testing.expect(std.mem.indexOf(u8, text, "Big Feature") != null);
    // External id.
    try testing.expect(std.mem.indexOf(u8, text, "MYPROJ-100") != null);
    // Link role.
    try testing.expect(std.mem.indexOf(u8, text, "mirror") != null);
    // Sync direction.
    try testing.expect(std.mem.indexOf(u8, text, "two-way") != null);
    // Last sync status.
    try testing.expect(std.mem.indexOf(u8, text, "ok") != null);

    // RENDER-LEVEL ASSERTIONS (task 4030): sync event data.
    try testing.expect(std.mem.indexOf(u8, text, "Latest sync:") != null);
    try testing.expect(std.mem.indexOf(u8, text, "push") != null);
    try testing.expect(std.mem.indexOf(u8, text, "2025") != null);
}

test "external_ops: renderSystemsDetail shows '(never synced)' when no sync events (task 4030 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('linear','linear-test','token-env','LINEAR_KEY')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    _ = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'LIN-1', 'never')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    // No sync_events rows.

    var state = ExtOpsState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const win_w: u16 = 80;
    const win_h: u16 = 24;
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

    try renderSystemsDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // "Latest sync:" header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Latest sync:") != null);
    // "(never synced)" must appear.
    try testing.expect(std.mem.indexOf(u8, text, "never synced") != null);
    // Link must still appear (task 4029).
    try testing.expect(std.mem.indexOf(u8, text, "LIN-1") != null);
}

test "external_ops: renderConflictsDetail shows unresolved conflict (task 4031 render-level)" {
    // Critical render-level test for task 4031.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-cf-render','token-env','J')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','CF Task','doing')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'JI-11', 'conflict')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, detail, at) values (?, 'external', 'pull', 'conflict', 'status mismatch', '2025-04-01T07:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    var state = ExtOpsState.init(a);
    defer state.deinit();
    try state.reload(&d);
    state.mode = .conflicts;

    const win_w: u16 = 80;
    const win_h: u16 = 24;
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

    renderConflictsDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4031):
    // Section header.
    try testing.expect(std.mem.indexOf(u8, text, "Unresolved conflicts:") != null);
    // conflict row: scope and direction from display_text.
    try testing.expect(std.mem.indexOf(u8, text, "external") != null);
    try testing.expect(std.mem.indexOf(u8, text, "pull") != null);
    // timestamp prefix.
    try testing.expect(std.mem.indexOf(u8, text, "2025") != null);
    // detail text.
    try testing.expect(std.mem.indexOf(u8, text, "status mismatch") != null);
}

test "external_ops: renderConflictsDetail shows (none) when no conflicts (task 4031 render-level)" {
    // Verifies clean fixture shows explicit "(none — all conflicts resolved)".
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Resolved conflict: conflict event followed by resolved-fs event.
    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-clean','token-env','J')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Clean Task','done')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'JI-R', 'ok')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'pull', 'conflict', '2025-01-01T10:00:00.000Z')",
        &.{.{ .int = link_id }},
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'push', 'resolved-fs', '2025-01-02T10:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    var state = ExtOpsState.init(a);
    defer state.deinit();
    try state.reload(&d);
    state.mode = .conflicts;

    const win_w: u16 = 80;
    const win_h: u16 = 24;
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

    renderConflictsDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // Section header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Unresolved conflicts:") != null);
    // Clean state: "(none — all conflicts resolved)" must appear.
    try testing.expect(std.mem.indexOf(u8, text, "none") != null);
    try testing.expect(std.mem.indexOf(u8, text, "resolved") != null);
}

test "external_ops: legendLabel fits in buf" {
    var buf: [128]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Quit") != null);
    try testing.expect(std.mem.indexOf(u8, label, "Conflicts") != null);
}

test "external_ops compiles" {
    std.testing.refAllDecls(@This());
}
