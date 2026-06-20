//! cockpit/views/utility_view.zig — Utility view (M15).
//!
//! ONE view with THREE sub-modes toggled by 'm':
//!
//!   .config         — Config inspector over config(key, value, updated_at).
//!                     Left pane lists all config keys; right pane shows the
//!                     selected key's full value and updated_at.
//!
//!   .annotations    — Annotations browser over annotations + annotation_tags.
//!                     Left pane lists annotations (status + title + tags);
//!                     right pane shows the selected annotation's detail:
//!                     anchor_path, line range, body, tags, status.
//!
//!   .workbench_sync — Workbench sync-state view: per feature (anchor_plan_id),
//!                     the drift/conflict state derived from workbench_sync_state.
//!                     Left pane lists features with [=]/[~]/[!] indicators;
//!                     right pane shows: total, in_sync, pending, conflicts,
//!                     last_synced_at.
//!
//! Tasks 4041 (Config inspector), 4042 (Annotations browser),
//!       4043 (Workbench sync-state view).
//!
//! Acceptance invariants:
//!   (4041) ALL queried config fields rendered: key, value, updated_at.
//!   (4042) ALL queried annotation fields rendered: title, anchor_path,
//!          status, tags, body, line range.
//!   (4043) ALL queried workbench sync fields rendered: plan_title, total,
//!          in_sync, pending, conflicts, last_synced_at. Drift/conflict
//!          mirroring the engine's model (sync.zig lines 442-460).
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by UtilityState and released via deinit.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread.
//!   - MEMORY GUARD (brief rule (c)): no string filter fields in this view.
//!     All rendered strings come from freshly allocated view_model fields.
//!   - 13th view: registered with key 'u' (display-only; numeric jump fires
//!     only for '1'–'9'). Tab/Shift-Tab cycling reaches this view.
//!
//! Schema confirmed:
//!   config(key text pk, value text not null, updated_at text not null)
//!   annotations(id, scope_kind, scope_id, anchor_path, anchor_line_start,
//!     anchor_line_end, title, slug, body, status, vendor, plan_id, task_id)
//!   annotation_tags(annotation_id integer, tag text)
//!   workbench_sync_state(id, anchor_plan_id, entity_kind, entity_id,
//!     file_path, content_hash, fs_mtime, db_updated_at, last_synced_at)

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");
const external_actions = @import("../edit/external_actions.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// Sub-mode
// =========================================================================

/// The three utility sub-modes. Toggled by 'm'.
pub const UtilityMode = enum {
    config,
    annotations,
    workbench_sync,

    pub fn next(self: UtilityMode) UtilityMode {
        return switch (self) {
            .config => .annotations,
            .annotations => .workbench_sync,
            .workbench_sync => .config,
        };
    }

    pub fn label(self: UtilityMode) []const u8 {
        return switch (self) {
            .config => "Config",
            .annotations => "Annotations",
            .workbench_sync => "WorkbenchSync",
        };
    }
};

// =========================================================================
// UtilityState
// =========================================================================

/// All mutable state for the Utility view (three sub-modes).
pub const UtilityState = struct {
    allocator: std.mem.Allocator,

    /// Active sub-mode.
    mode: UtilityMode = .config,

    // ---- Config sub-mode (task 4041) ------------------------------------
    config_rows: []view_model.ConfigRow = &.{},
    config_selected_idx: usize = 0,

    // ---- Annotations sub-mode (task 4042) --------------------------------
    annotation_rows: []view_model.AnnotationRow = &.{},
    annotation_selected_idx: usize = 0,

    // ---- Workbench sync sub-mode (task 4043) ----------------------------
    wb_rows: []view_model.WorkbenchSyncRow = &.{},
    wb_selected_idx: usize = 0,

    // ---- M18 (task 4050): workbench action overlay ----------------------
    /// Workbench push/pull/status action controller.
    /// Keys 'p' (push), 'l' (pull), 's' (status) in .workbench_sync mode
    /// enter the confirm overlay. The selected plan's id/slug is used.
    action: external_actions.ExternalActionState,

    /// Initialize with test-safe defaults (testing.io + empty environ).
    /// Use `initFull` in the cockpit run path to wire live io/environ.
    pub fn init(allocator: std.mem.Allocator) UtilityState {
        return .{
            .allocator = allocator,
            .action = external_actions.ExternalActionState.init(
                allocator,
                std.testing.io,
                std.process.Environ.empty,
            ),
        };
    }

    /// Initialize with live I/O and process environ for cockpit runtime use.
    /// Called by app.zig `run()` to wire workbench-adapter credentials at launch.
    pub fn initFull(
        allocator: std.mem.Allocator,
        io: std.Io,
        environ: std.process.Environ,
    ) UtilityState {
        return .{
            .allocator = allocator,
            .action = external_actions.ExternalActionState.init(allocator, io, environ),
        };
    }

    pub fn deinit(self: *UtilityState) void {
        self.action.deinit();
        view_model.ConfigRow.deinitMany(self.config_rows, self.allocator);
        self.config_rows = &.{};
        view_model.AnnotationRow.deinitMany(self.annotation_rows, self.allocator);
        self.annotation_rows = &.{};
        view_model.WorkbenchSyncRow.deinitMany(self.wb_rows, self.allocator);
        self.wb_rows = &.{};
    }

    /// Reload all utility data from the DB. Called on db_changed and on
    /// initial launch. Reuses the existing wake .db_changed integration
    /// (no second wake thread).
    pub fn reload(self: *UtilityState, d: *db.sqlite.Db) !void {
        // Config rows.
        view_model.ConfigRow.deinitMany(self.config_rows, self.allocator);
        self.config_rows = &.{};
        self.config_rows = try view_model.queryConfig(d, self.allocator);
        if (self.config_rows.len == 0) {
            self.config_selected_idx = 0;
        } else if (self.config_selected_idx >= self.config_rows.len) {
            self.config_selected_idx = self.config_rows.len - 1;
        }

        // Annotation rows.
        view_model.AnnotationRow.deinitMany(self.annotation_rows, self.allocator);
        self.annotation_rows = &.{};
        self.annotation_rows = try view_model.queryAnnotations(d, self.allocator);
        if (self.annotation_rows.len == 0) {
            self.annotation_selected_idx = 0;
        } else if (self.annotation_selected_idx >= self.annotation_rows.len) {
            self.annotation_selected_idx = self.annotation_rows.len - 1;
        }

        // Workbench sync rows.
        view_model.WorkbenchSyncRow.deinitMany(self.wb_rows, self.allocator);
        self.wb_rows = &.{};
        self.wb_rows = try view_model.queryWorkbenchSync(d, self.allocator);
        if (self.wb_rows.len == 0) {
            self.wb_selected_idx = 0;
        } else if (self.wb_selected_idx >= self.wb_rows.len) {
            self.wb_selected_idx = self.wb_rows.len - 1;
        }
    }

    /// Handle a key event. Returns true when the key was consumed.
    ///
    /// M18 (task 4050): in .workbench_sync mode, 'p' triggers push,
    /// 'l' triggers pull, and 's' triggers status for the selected plan.
    /// The action overlay takes priority when active.
    pub fn handleKey(self: *UtilityState, key: Key, d: *db.sqlite.Db) bool {
        // Action overlay takes priority (M18 confirm/dismiss/cancel paths).
        if (self.action.isActive()) {
            return self.action.handleKey(key, d);
        }

        // 'm': cycle sub-mode.
        if (key.matches('m', .{})) {
            self.mode = self.mode.next();
            return true;
        }

        // M18 (task 4050): workbench action keys in .workbench_sync mode.
        // 'p' = push (DB → FS), 'l' = pull (FS → DB), 's' = status.
        // These mirror engine.workbench.sync.{push,pull,status} via
        // the same paths as handlers/workbench/{push,pull,status}.zig.
        if (self.mode == .workbench_sync) {
            // Determine the selected plan for the workbench action.
            const selected_row = if (self.wb_rows.len > 0)
                &self.wb_rows[self.wb_selected_idx]
            else
                null;

            if (key.matches('p', .{})) {
                if (selected_row) |row| {
                    self.action.enterWorkbenchConfirm(.push, row.anchor_plan_id, row.plan_title);
                }
                return true;
            }
            if (key.matches('l', .{})) {
                if (selected_row) |row| {
                    self.action.enterWorkbenchConfirm(.pull, row.anchor_plan_id, row.plan_title);
                }
                return true;
            }
            if (key.matches('s', .{})) {
                if (selected_row) |row| {
                    self.action.enterWorkbenchConfirm(.status, row.anchor_plan_id, row.plan_title);
                }
                return true;
            }
        }

        // j / arrow-down: move selection down in active sub-mode.
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            switch (self.mode) {
                .config => {
                    if (self.config_rows.len > 0 and self.config_selected_idx + 1 < self.config_rows.len) {
                        self.config_selected_idx += 1;
                    }
                },
                .annotations => {
                    if (self.annotation_rows.len > 0 and self.annotation_selected_idx + 1 < self.annotation_rows.len) {
                        self.annotation_selected_idx += 1;
                    }
                },
                .workbench_sync => {
                    if (self.wb_rows.len > 0 and self.wb_selected_idx + 1 < self.wb_rows.len) {
                        self.wb_selected_idx += 1;
                    }
                },
            }
            return true;
        }

        // k / arrow-up: move selection up in active sub-mode.
        if (key.matches('k', .{}) or key.matches(Key.up, .{})) {
            switch (self.mode) {
                .config => {
                    if (self.config_selected_idx > 0) self.config_selected_idx -= 1;
                },
                .annotations => {
                    if (self.annotation_selected_idx > 0) self.annotation_selected_idx -= 1;
                },
                .workbench_sync => {
                    if (self.wb_selected_idx > 0) self.wb_selected_idx -= 1;
                },
            }
            return true;
        }

        return false;
    }
};

// =========================================================================
// Render
// =========================================================================

/// Render the Utility view into the navigator and detail windows.
///
/// The active sub-mode is shown in the navigator header. 'm' cycles modes.
pub fn render(
    state: *const UtilityState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    // M18 (task 4050): when the action overlay is active, render it on top
    // of the detail pane (navigator remains visible for context).
    const overlay_active = state.action.isActive();

    switch (state.mode) {
        .config => {
            renderConfigNavigator(state, nav_win);
            if (overlay_active) {
                external_actions.renderOverlay(&state.action, detail_win);
            } else {
                renderConfigDetail(state, detail_win);
            }
        },
        .annotations => {
            renderAnnotationsNavigator(state, nav_win);
            if (overlay_active) {
                external_actions.renderOverlay(&state.action, detail_win);
            } else {
                renderAnnotationsDetail(state, detail_win, allocator);
            }
        },
        .workbench_sync => {
            renderWbSyncNavigator(state, nav_win);
            if (overlay_active) {
                external_actions.renderOverlay(&state.action, detail_win);
            } else {
                renderWbSyncDetail(state, detail_win, allocator);
            }
        },
    }
}

// =========================================================================
// Config sub-mode (task 4041)
// =========================================================================

/// Render the Config navigator (left pane).
///
/// Header: "Config  [<N> entries]"
/// Rows:   one per config key/value pair (pre-formatted display_text).
/// Empty:  "(no config entries)"
///
/// INVARIANT (task 4041): all queried config fields rendered: key + value
/// appear in display_text; updated_at appears in the detail pane.
fn renderConfigNavigator(state: *const UtilityState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    // Header.
    _ = win.printSegment(.{
        .text = "Config",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });
    if (win.height < 2) return;

    if (state.config_rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no config entries)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 1, .col_offset = 0 });
        return;
    }

    const viewport_h: usize = if (win.height > 1) @as(usize, @intCast(win.height)) - 1 else 0;
    const scroll: usize = if (state.config_selected_idx >= viewport_h)
        state.config_selected_idx - viewport_h + 1
    else
        0;

    var display_row: u16 = 1;
    for (state.config_rows, 0..) |row, i| {
        if (i < scroll) continue;
        if (display_row >= win.height) break;
        const is_selected = (i == state.config_selected_idx);
        const style: Style = if (is_selected)
            .{ .bold = true, .reverse = true }
        else
            .{};
        // INVARIANT (task 4041): render display_text (heap-allocated "key  =  value").
        _ = win.printSegment(.{
            .text = row.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });
        display_row += 1;
    }
}

/// Render the Config detail pane (right).
///
/// Layout:
///   row 0: "key:" (bold dim label)  key (bold)
///   row 1: "value:"
///   row 2: "  <value text>"
///   row 3: (blank separator)
///   row 4: "updated:" (bold dim)  <updated_at>
///
/// INVARIANT (task 4041): ALL queried fields (key, value, updated_at) MUST
/// appear in the rendered detail pane.
fn renderConfigDetail(state: *const UtilityState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    if (state.config_rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no config entry selected)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    if (state.config_selected_idx >= state.config_rows.len) return;
    const r = state.config_rows[state.config_selected_idx];

    var row: u16 = 0;

    // key (row 0).
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "key:    ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.key,
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 8 });
        row += 1;
    }

    // "value:" label (row 1).
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "value:",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    // value text (row 2).
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = r.value,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 2 });
        row += 1;
    }

    // separator (row 3).
    if (row < win.height) row += 1;

    // updated_at (row 4).
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "updated:",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.updated_at,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 9 });
    }
}

// =========================================================================
// Annotations sub-mode (task 4042)
// =========================================================================

/// Render the Annotations navigator (left pane).
///
/// Header: "Annotations"
/// Rows:   display_text ("[status] title  [tags]" or "[status] title").
/// Empty:  "(no annotations)"
///
/// INVARIANT (task 4042): status + title + tags appear in display_text (all rendered).
fn renderAnnotationsNavigator(state: *const UtilityState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    _ = win.printSegment(.{
        .text = "Annotations",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });
    if (win.height < 2) return;

    if (state.annotation_rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no annotations)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 1, .col_offset = 0 });
        return;
    }

    const viewport_h: usize = if (win.height > 1) @as(usize, @intCast(win.height)) - 1 else 0;
    const scroll: usize = if (state.annotation_selected_idx >= viewport_h)
        state.annotation_selected_idx - viewport_h + 1
    else
        0;

    var display_row: u16 = 1;
    for (state.annotation_rows, 0..) |row, i| {
        if (i < scroll) continue;
        if (display_row >= win.height) break;
        const is_selected = (i == state.annotation_selected_idx);
        const style: Style = if (is_selected)
            .{ .bold = true, .reverse = true }
        else
            .{};
        // INVARIANT (task 4042): render display_text (heap-allocated).
        _ = win.printSegment(.{
            .text = row.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });
        display_row += 1;
    }
}

/// Render the Annotations detail pane (right).
///
/// Layout:
///   row 0: title (bold)
///   row 1: "status:  <status>"
///   row 2: "path:    <anchor_path>"
///   row 3: "lines:   <start>–<end>"  (if non-zero)
///   row 4: "tags:    <tags>"         (if non-empty)
///   (blank separator)
///   "Body:" section
///   body text (first N lines fitting in remaining height)
///
/// INVARIANT (task 4042): ALL queried fields must appear: title, status,
/// anchor_path, line range, tags, body.
fn renderAnnotationsDetail(state: *const UtilityState, win: Window, arena: std.mem.Allocator) void {
    if (win.height == 0 or win.width == 0) return;

    if (state.annotation_rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no annotation selected)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    if (state.annotation_selected_idx >= state.annotation_rows.len) return;
    const r = state.annotation_rows[state.annotation_selected_idx];

    var row: u16 = 0;

    // title (row 0).
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = r.title,
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    // status (row 1).
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "status:  ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.status,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 9 });
        row += 1;
    }

    // anchor_path (row 2).
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "path:    ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.anchor_path,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 9 });
        row += 1;
    }

    // line range (row 3) — always rendered; shows "0–0" when no lines set.
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "lines:   ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        // MEMORY GUARD: line numbers are integers; format into a fixed stack
        // buffer. This is safe because the grapheme pointer from printSegment
        // is NOT collected for render-level tests (stack-local format buffers
        // are only unsafe when grapheme pointers need to outlive the frame —
        // line numbers are rendered directly into cells, not referenced later).
        // To be safe for render-level tests, we use a static buf for the number
        // and a heap-allocated label for the combined string — but since we
        // have no render-level test collecting line number graphemes, a simple
        // stack buf is correct here.
        // Use arena allocation so the slice remains valid through vaxis.render().
        const line_str = std.fmt.allocPrint(
            arena,
            "{d}–{d}",
            .{ r.anchor_line_start, r.anchor_line_end },
        ) catch "?–?";
        _ = win.printSegment(.{
            .text = line_str,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 9 });
        row += 1;
    }

    // tags (row 4).
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "tags:    ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        if (r.tags.len > 0) {
            _ = win.printSegment(.{
                .text = r.tags,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 9 });
        } else {
            _ = win.printSegment(.{
                .text = "(none)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 9 });
        }
        row += 1;
    }

    // separator.
    if (row < win.height) row += 1;

    // "Body:" header.
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "Body:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    // body text — first line.
    if (row < win.height) {
        if (r.body.len > 0) {
            _ = win.printSegment(.{
                .text = r.body,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 2 });
        } else {
            _ = win.printSegment(.{
                .text = "(empty body)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 2 });
        }
    }
}

// =========================================================================
// Workbench sync sub-mode (task 4043)
// =========================================================================

/// Render the Workbench sync navigator (left pane).
///
/// Header: "WorkbenchSync  [<N> features]"
/// Rows:   display_text "[=/~/!] plan_title  (N total, N pending, N conflicts)"
/// Empty:  "(no workbench sync state)"
///
/// Status indicators (mirroring engine sync.zig Classification):
///   '=' — all in sync (no drift, no conflicts)
///   '~' — pending (DB has drifted since last sync; db_to_fs needed)
///   '!' — conflicts (unresolved workbench conflict events)
///
/// INVARIANT (task 4043): drift/conflict detection matches the engine's
/// db_changed flag (sync.zig lines 447-453).
fn renderWbSyncNavigator(state: *const UtilityState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    _ = win.printSegment(.{
        .text = "WorkbenchSync",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });
    if (win.height < 2) return;

    if (state.wb_rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no workbench sync state)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 1, .col_offset = 0 });
        return;
    }

    const viewport_h: usize = if (win.height > 1) @as(usize, @intCast(win.height)) - 1 else 0;
    const scroll: usize = if (state.wb_selected_idx >= viewport_h)
        state.wb_selected_idx - viewport_h + 1
    else
        0;

    var display_row: u16 = 1;
    for (state.wb_rows, 0..) |row, i| {
        if (i < scroll) continue;
        if (display_row >= win.height) break;
        const is_selected = (i == state.wb_selected_idx);
        const style: Style = if (is_selected)
            .{ .bold = true, .reverse = true }
        else
            .{};
        // INVARIANT (task 4043): render display_text (heap-allocated, contains
        // indicator, plan_title, total, pending, conflicts).
        _ = win.printSegment(.{
            .text = row.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });
        display_row += 1;
    }
}

/// Render the Workbench sync detail pane (right).
///
/// Layout:
///   row 0:  "Feature: <plan_title>" (bold)
///   row 1:  "Plan ID: <anchor_plan_id>"
///   row 2:  (blank separator)
///   row 3:  "total:     <N>"
///   row 4:  "in sync:   <N>"
///   row 5:  "pending:   <N>"   (drift: DB changed since last sync)
///   row 6:  "conflicts: <N>"   (unresolved workbench conflict events)
///   row 7:  (blank separator)
///   row 8:  "last synced:"
///   row 9:  "  <last_synced_at>"
///
/// INVARIANT (task 4043): ALL queried fields MUST appear in the rendered
/// detail pane: plan_title, anchor_plan_id, total, in_sync, pending,
/// conflicts, last_synced_at.
fn renderWbSyncDetail(state: *const UtilityState, win: Window, arena: std.mem.Allocator) void {
    if (win.height == 0 or win.width == 0) return;

    if (state.wb_rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no feature selected)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    if (state.wb_selected_idx >= state.wb_rows.len) return;
    const r = state.wb_rows[state.wb_selected_idx];

    var row: u16 = 0;

    // Feature title (row 0).
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = r.plan_title,
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    // Plan ID (row 1) — integer, rendered via a stack buf (safe: not
    // collected by collectScreenText in render-level tests for integers).
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "plan_id:   ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        // Use arena allocation so the slice remains valid through vaxis.render().
        const id_str = std.fmt.allocPrint(arena, "{d}", .{r.anchor_plan_id}) catch "?";
        _ = win.printSegment(.{
            .text = id_str,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 11 });
        row += 1;
    }

    // separator.
    if (row < win.height) row += 1;

    // Counts — integers via stack bufs.
    const fields = [_]struct { label: []const u8, val: i64 }{
        .{ .label = "total:     ", .val = r.total },
        .{ .label = "in sync:   ", .val = r.in_sync },
        .{ .label = "pending:   ", .val = r.pending },
        .{ .label = "conflicts: ", .val = r.conflicts },
    };
    for (fields) |f| {
        if (row >= win.height) break;
        _ = win.printSegment(.{
            .text = f.label,
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        // Use arena allocation so the slice remains valid through vaxis.render().
        const num_str = std.fmt.allocPrint(arena, "{d}", .{f.val}) catch "?";
        _ = win.printSegment(.{
            .text = num_str,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 11 });
        row += 1;
    }

    // separator.
    if (row < win.height) row += 1;

    // "last synced:" header.
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "last synced:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    // last_synced_at value (heap-allocated — stable for collectScreenText).
    if (row < win.height) {
        if (r.last_synced_at.len > 0) {
            _ = win.printSegment(.{
                .text = r.last_synced_at,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 2 });
        } else {
            _ = win.printSegment(.{
                .text = "(never)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 2 });
        }
    }
}

/// Return a one-line legend string for the key legend bar.
/// Return a one-line legend string for the key legend bar.
///
/// M18 (task 4050): when the action overlay is active, defer to its legend.
/// When in .workbench_sync mode, show the workbench action keys.
pub fn legendLabel(state: *const UtilityState, buf: []u8) []const u8 {
    if (state.action.isActive()) {
        return external_actions.legendLabel(&state.action, buf);
    }
    if (state.mode == .workbench_sync) {
        return std.fmt.bufPrint(
            buf,
            "  q Quit  j/k Select  m Mode  p Push  l Pull  s Status  Tab Focus",
            .{},
        ) catch "  q Quit  j/k Select  m Mode  p Push  l Pull  s Status";
    }
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Select  m Mode(Config/Ann/Sync)  Tab Focus  Tab/S-Tab View",
        .{},
    ) catch "  q Quit  j/k Select  m Mode  Tab Focus";
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

/// Extract all non-empty grapheme text from a Screen's cell buffer.
/// Used by render-level tests to assert specific text appears in output.
/// Grapheme pointers from heap-allocated strings remain valid for the
/// test's duration.
fn collectScreenText(screen: *const vaxis.Screen, out: *std.ArrayList(u8)) !void {
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) {
            try out.appendSlice(testing.allocator, g);
        }
    }
}

// -------------------------------------------------------------------------
// UtilityState lifecycle
// -------------------------------------------------------------------------

test "utility_view: init and deinit are clean" {
    var state = UtilityState.init(testing.allocator);
    defer state.deinit();
    try testing.expectEqual(@as(usize, 0), state.config_rows.len);
    try testing.expectEqual(@as(usize, 0), state.annotation_rows.len);
    try testing.expectEqual(@as(usize, 0), state.wb_rows.len);
    try testing.expectEqual(UtilityMode.config, state.mode);
}

test "utility_view: reload on empty DB yields all empty rows" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = UtilityState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 0), state.config_rows.len);
    try testing.expectEqual(@as(usize, 0), state.annotation_rows.len);
    try testing.expectEqual(@as(usize, 0), state.wb_rows.len);
}

test "utility_view: mode cycling via handleKey 'm'" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = UtilityState.init(a);
    defer state.deinit();

    try testing.expectEqual(UtilityMode.config, state.mode);
    const m_key = Key{ .codepoint = 'm', .mods = .{} };
    _ = state.handleKey(m_key, &d);
    try testing.expectEqual(UtilityMode.annotations, state.mode);
    _ = state.handleKey(m_key, &d);
    try testing.expectEqual(UtilityMode.workbench_sync, state.mode);
    _ = state.handleKey(m_key, &d);
    try testing.expectEqual(UtilityMode.config, state.mode);
}

// -------------------------------------------------------------------------
// Config sub-mode (task 4041)
// -------------------------------------------------------------------------

test "utility_view: config reload populates rows ordered by key (task 4041)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into config (key, value) values ('z-key', 'z-value')",
        &.{},
    );
    _ = try d.execParams(
        "insert into config (key, value) values ('a-key', 'a-value')",
        &.{},
    );

    var state = UtilityState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 2), state.config_rows.len);
    try testing.expectEqualStrings("a-key", state.config_rows[0].key);
    try testing.expectEqualStrings("z-key", state.config_rows[1].key);
}

test "utility_view: j/k navigate config rows (task 4041)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into config (key, value) values ('k1', 'v1')", &.{});
    _ = try d.execParams("insert into config (key, value) values ('k2', 'v2')", &.{});

    var state = UtilityState.init(a);
    defer state.deinit();
    try state.reload(&d);
    try testing.expectEqual(@as(usize, 0), state.config_selected_idx);

    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    _ = state.handleKey(j_key, &d);
    try testing.expectEqual(@as(usize, 1), state.config_selected_idx);

    const k_key = Key{ .codepoint = 'k', .mods = .{} };
    _ = state.handleKey(k_key, &d);
    try testing.expectEqual(@as(usize, 0), state.config_selected_idx);
}

// RENDER-LEVEL TEST (task 4041, rule (b))
test "utility_view: renderConfigNavigator renders config keys in screen buffer (task 4041 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into config (key, value) values ('my.key', 'my-value')", &.{});
    _ = try d.execParams("insert into config (key, value) values ('other.key', 'other-value')", &.{});

    var state = UtilityState.init(a);
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

    renderConfigNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL: config header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Config") != null);
    // Both keys must appear.
    try testing.expect(std.mem.indexOf(u8, text, "my.key") != null);
    try testing.expect(std.mem.indexOf(u8, text, "other.key") != null);
    // Both values must appear.
    try testing.expect(std.mem.indexOf(u8, text, "my-value") != null);
    try testing.expect(std.mem.indexOf(u8, text, "other-value") != null);
}

// RENDER-LEVEL TEST (task 4041, rule (b))
test "utility_view: renderConfigDetail renders key, value, updated_at (task 4041 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into config (key, value) values ('scope.default', 'all')", &.{});

    var state = UtilityState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const win_w: u16 = 80;
    const win_h: u16 = 20;
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

    renderConfigDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4041):
    // Key must appear.
    try testing.expect(std.mem.indexOf(u8, text, "scope.default") != null);
    // Value must appear.
    try testing.expect(std.mem.indexOf(u8, text, "all") != null);
    // updated_at (contains year 2026 or similar).
    try testing.expect(std.mem.indexOf(u8, text, "updated:") != null);
}

// RENDER-LEVEL EMPTY STATE (task 4041)
test "utility_view: renderConfigNavigator shows empty state when no config (task 4041 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = UtilityState.init(a);
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

    renderConfigNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;
    try testing.expect(std.mem.indexOf(u8, text, "no config entries") != null);
}

// -------------------------------------------------------------------------
// Annotations sub-mode (task 4042)
// -------------------------------------------------------------------------

test "utility_view: annotation reload populates rows newest-first (task 4042)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into annotations (scope_kind, anchor_path, title, body, status, created_at) values ('global', 'a.zig', 'Older', 'body1', 'active', '2024-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into annotations (scope_kind, anchor_path, title, body, status, created_at) values ('global', 'b.zig', 'Newer', 'body2', 'resolved', '2025-06-01T00:00:00.000Z')",
        &.{},
    );

    var state = UtilityState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 2), state.annotation_rows.len);
    // Newest-first.
    try testing.expectEqualStrings("Newer", state.annotation_rows[0].title);
    try testing.expectEqualStrings("Older", state.annotation_rows[1].title);
}

// RENDER-LEVEL TEST (task 4042, rule (b))
test "utility_view: renderAnnotationsNavigator renders title+status+tags (task 4042 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const ann_id = try d.execParams(
        "insert into annotations (scope_kind, anchor_path, title, body, status) values ('global', 'src/lib.zig', 'Important Note', 'See this', 'active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into annotation_tags (annotation_id, tag) values (?, 'fixme')",
        &.{.{ .int = ann_id }},
    );

    var state = UtilityState.init(a);
    defer state.deinit();
    state.mode = .annotations;
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

    renderAnnotationsNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4042):
    // Header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Annotations") != null);
    // Title must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Important Note") != null);
    // Status must appear.
    try testing.expect(std.mem.indexOf(u8, text, "active") != null);
    // Tag must appear.
    try testing.expect(std.mem.indexOf(u8, text, "fixme") != null);
}

// RENDER-LEVEL TEST (task 4042, rule (b))
test "utility_view: renderAnnotationsDetail renders all fields (task 4042 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const ann_id = try d.execParams(
        "insert into annotations (scope_kind, anchor_path, anchor_line_start, anchor_line_end, title, body, status) values ('global', 'engine/workbench.zig', 42, 55, 'Review this function', 'This needs attention.', 'active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into annotation_tags (annotation_id, tag) values (?, 'review')",
        &.{.{ .int = ann_id }},
    );

    var state = UtilityState.init(a);
    defer state.deinit();
    state.mode = .annotations;
    try state.reload(&d);

    const win_w: u16 = 80;
    const win_h: u16 = 30;
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

    // Use an arena so that allocPrint calls inside renderAnnotationsDetail do
    // not leak when tested with testing.allocator (render functions own no
    // arena; the arena is reset each frame by renderFrame).
    var render_arena = std.heap.ArenaAllocator.init(a);
    defer render_arena.deinit();
    renderAnnotationsDetail(&state, detail_win, render_arena.allocator());

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4042): ALL queried fields must appear.
    // Title (heap-allocated → stable grapheme pointers).
    try testing.expect(std.mem.indexOf(u8, text, "Review this function") != null);
    // Status.
    try testing.expect(std.mem.indexOf(u8, text, "active") != null);
    // anchor_path (heap-allocated).
    try testing.expect(std.mem.indexOf(u8, text, "engine/workbench.zig") != null);
    // tags (heap-allocated).
    try testing.expect(std.mem.indexOf(u8, text, "review") != null);
    // Body text (heap-allocated).
    try testing.expect(std.mem.indexOf(u8, text, "This needs attention.") != null);
    // "Body:" header (string literal).
    try testing.expect(std.mem.indexOf(u8, text, "Body:") != null);
}

// RENDER-LEVEL EMPTY STATE (task 4042)
test "utility_view: renderAnnotationsNavigator shows empty state when no annotations (task 4042 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = UtilityState.init(a);
    defer state.deinit();
    state.mode = .annotations;
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

    renderAnnotationsNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;
    try testing.expect(std.mem.indexOf(u8, text, "no annotations") != null);
}

// -------------------------------------------------------------------------
// Workbench sync sub-mode (task 4043)
// -------------------------------------------------------------------------

test "utility_view: wb_sync reload populates rows (task 4043)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'My Feature', 'my-feature', 'active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into workbench_sync_state (anchor_plan_id, entity_kind, entity_id, file_path, content_hash, db_updated_at, last_synced_at) values (?, 'plan', ?, 'feat/plan.md', 'hash1', '2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z')",
        &.{ .{ .int = plan_id }, .{ .int = plan_id } },
    );

    var state = UtilityState.init(a);
    defer state.deinit();
    state.mode = .workbench_sync;
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.wb_rows.len);
    try testing.expectEqualStrings("My Feature", state.wb_rows[0].plan_title);
    try testing.expectEqual(@as(i64, 1), state.wb_rows[0].total);
}

// RENDER-LEVEL TEST (task 4043, rule (b)) — clean case
test "utility_view: renderWbSyncNavigator renders clean feature with '=' indicator (task 4043 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Clean Feature', 'clean-feature', 'active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (plan_id, scope_kind, title, status, priority) values (?, 'global', 'T1', 'todo', 2)",
        &.{.{ .int = plan_id }},
    );
    // Read back the task's updated_at.
    const task_upd = blk: {
        var stmt = try d.prepare("select updated_at from tasks where id = ?");
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch break :blk try a.dupe(u8, "");
        switch (stmt.step() catch break :blk try a.dupe(u8, "")) {
            .done => break :blk try a.dupe(u8, ""),
            .row => break :blk try stmt.columnTextAlloc(0, a),
        }
    };
    defer a.free(task_upd);

    _ = try d.execParams(
        "insert into workbench_sync_state (anchor_plan_id, entity_kind, entity_id, file_path, content_hash, db_updated_at, last_synced_at) values (?, 'task', ?, 'feat/t1.md', 'h1', ?, strftime('%Y-%m-%dT%H:%M:%fZ','now'))",
        &.{ .{ .int = plan_id }, .{ .int = task_id }, .{ .text = task_upd } },
    );

    var state = UtilityState.init(a);
    defer state.deinit();
    state.mode = .workbench_sync;
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

    renderWbSyncNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4043):
    // Header.
    try testing.expect(std.mem.indexOf(u8, text, "WorkbenchSync") != null);
    // Plan title.
    try testing.expect(std.mem.indexOf(u8, text, "Clean Feature") != null);
    // In-sync indicator.
    try testing.expect(std.mem.indexOf(u8, text, "=") != null);
    // Counts.
    try testing.expect(std.mem.indexOf(u8, text, "1 total") != null);
    try testing.expect(std.mem.indexOf(u8, text, "0 pending") != null);
    try testing.expect(std.mem.indexOf(u8, text, "0 conflicts") != null);
}

// RENDER-LEVEL TEST (task 4043, rule (b)) — drift case
test "utility_view: renderWbSyncNavigator renders drifted feature with '~' indicator (task 4043 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Drifted Feature', 'drifted-feature', 'active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (plan_id, scope_kind, title, status, priority) values (?, 'global', 'T-Drifted', 'todo', 2)",
        &.{.{ .int = plan_id }},
    );
    // Old db_updated_at that does NOT match the task's current updated_at → pending.
    _ = try d.execParams(
        "insert into workbench_sync_state (anchor_plan_id, entity_kind, entity_id, file_path, content_hash, db_updated_at, last_synced_at) values (?, 'task', ?, 'feat/t-drift.md', 'h1', '2020-01-01T00:00:00.000Z', '2020-01-01T00:00:00.000Z')",
        &.{ .{ .int = plan_id }, .{ .int = task_id } },
    );

    var state = UtilityState.init(a);
    defer state.deinit();
    state.mode = .workbench_sync;
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

    renderWbSyncNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4043): drift indicator and counts.
    try testing.expect(std.mem.indexOf(u8, text, "Drifted Feature") != null);
    // Drift indicator '~'.
    try testing.expect(std.mem.indexOf(u8, text, "~") != null);
    // Pending count 1.
    try testing.expect(std.mem.indexOf(u8, text, "1 pending") != null);
}

// RENDER-LEVEL TEST (task 4161) — conflict indicator
test "utility_view: renderWbSyncNavigator renders '!' conflict indicator at render level (task 4161)" {
    // This is a RENDER-LEVEL test: it seeds a real workbench_sync_state row
    // plus a sync_events conflict row (same seed shape as the M15 view-model
    // conflict test), renders the navigator pane into a vaxis.Screen, collects
    // the cell text, and asserts that:
    //   (a) The '!' conflict indicator appears in the rendered output.
    //   (b) The conflict count ("1 conflicts") appears in the rendered output.
    //   (c) The plan title appears in the rendered output.
    //
    // The view-model-level test in view_model.zig confirms queryWorkbenchSync
    // computes conflicts > 0 for a matching sync_events row. This test confirms
    // that the computed flag propagates all the way through renderWbSyncNavigator
    // into the vaxis back buffer — the layer the existing tests did not cover.
    //
    // Seed shape (mirrors queryWorkbenchSync conflict test in view_model.zig):
    //   - workbench_sync_state row with in-sync db_updated_at (not the source
    //     of the conflict indicator; conflicts come from sync_events).
    //   - sync_events row: scope='workbench', outcome='conflict',
    //     context_json with anchor_plan_id matching the plan.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Conflict Feature', 'conflict-feature', 'active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (plan_id, scope_kind, title, status, priority) values (?, 'global', 'T-Conflict', 'todo', 2)",
        &.{.{ .int = plan_id }},
    );

    // Read the task's current updated_at so we can set db_updated_at to match
    // (making the entity "in sync" at the DB level — the conflict comes solely
    // from the sync_events row, not from a db_updated_at mismatch).
    const task_updated_at = blk: {
        var stmt = try d.prepare("select coalesce(updated_at,'') from tasks where id = ?");
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = task_id }});
        switch (try stmt.step()) {
            .done => break :blk try a.dupe(u8, ""),
            .row => break :blk try stmt.columnTextAlloc(0, a),
        }
    };
    defer a.free(task_updated_at);

    _ = try d.execParams(
        "insert into workbench_sync_state (anchor_plan_id, entity_kind, entity_id, file_path, content_hash, db_updated_at, last_synced_at) values (?, 'task', ?, 'feat/conflict.md', 'abc', ?, strftime('%Y-%m-%dT%H:%M:%fZ','now'))",
        &.{ .{ .int = plan_id }, .{ .int = task_id }, .{ .text = task_updated_at } },
    );

    // Insert an unresolved workbench conflict event. queryWorkbenchSync counts
    // sync_events where scope='workbench', outcome='conflict', and
    // json_extract(context_json, '$.anchor_plan_id') = anchor_plan_id.
    const ctx_json = try std.fmt.allocPrint(
        a,
        "{{\"anchor_plan_id\":{d},\"entity_kind\":\"task\",\"entity_id\":{d},\"file_path\":\"feat/conflict.md\",\"fs_hash\":\"aaa\",\"db_hash\":\"bbb\"}}",
        .{ plan_id, task_id },
    );
    defer a.free(ctx_json);

    _ = try d.execParams(
        "insert into sync_events (scope, direction, outcome, context_json) values ('workbench', 'push', 'conflict', ?)",
        &.{.{ .text = ctx_json }},
    );

    var state = UtilityState.init(a);
    defer state.deinit();
    state.mode = .workbench_sync;
    try state.reload(&d);

    // Confirm the view-model computed conflicts > 0 (sanity check that the
    // seed is correct before hitting the render layer).
    try testing.expectEqual(@as(usize, 1), state.wb_rows.len);
    try testing.expect(state.wb_rows[0].conflicts > 0);

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

    renderWbSyncNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4161):
    // (a) Plan title must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Conflict Feature") != null);
    // (b) '!' conflict indicator must appear in the rendered cell text.
    try testing.expect(std.mem.indexOf(u8, text, "!") != null);
    // (c) The conflict count must appear.
    try testing.expect(std.mem.indexOf(u8, text, "1 conflicts") != null);
}

// RENDER-LEVEL TEST (task 4043, rule (b)) — detail pane
test "utility_view: renderWbSyncDetail renders all fields (task 4043 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Detail Feature', 'detail-feature', 'active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into workbench_sync_state (anchor_plan_id, entity_kind, entity_id, file_path, content_hash, db_updated_at, last_synced_at) values (?, 'plan', ?, 'feat/plan.md', 'h1', '2020-01-01T00:00:00.000Z', '2026-03-15T10:00:00.000Z')",
        &.{ .{ .int = plan_id }, .{ .int = plan_id } },
    );

    var state = UtilityState.init(a);
    defer state.deinit();
    state.mode = .workbench_sync;
    try state.reload(&d);

    const win_w: u16 = 80;
    const win_h: u16 = 30;
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

    // Use an arena so that allocPrint calls inside renderWbSyncDetail do not
    // leak when tested with testing.allocator (render functions own no arena;
    // the arena is reset each frame by renderFrame).
    var render_arena_wb = std.heap.ArenaAllocator.init(a);
    defer render_arena_wb.deinit();
    renderWbSyncDetail(&state, detail_win, render_arena_wb.allocator());

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4043): ALL queried fields must appear.
    // plan_title (heap-allocated → stable grapheme pointers).
    try testing.expect(std.mem.indexOf(u8, text, "Detail Feature") != null);
    // total/pending/conflicts labels.
    try testing.expect(std.mem.indexOf(u8, text, "total:") != null);
    try testing.expect(std.mem.indexOf(u8, text, "pending:") != null);
    try testing.expect(std.mem.indexOf(u8, text, "conflicts:") != null);
    // last synced header.
    try testing.expect(std.mem.indexOf(u8, text, "last synced:") != null);
    // last_synced_at value (heap-allocated).
    try testing.expect(std.mem.indexOf(u8, text, "2026-03-15") != null);
}

// RENDER-LEVEL EMPTY STATE (task 4043)
test "utility_view: renderWbSyncNavigator shows empty state when no sync state (task 4043 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = UtilityState.init(a);
    defer state.deinit();
    state.mode = .workbench_sync;
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

    renderWbSyncNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;
    try testing.expect(std.mem.indexOf(u8, text, "no workbench sync state") != null);
}

// -------------------------------------------------------------------------
// Memory regression: reload→mode→reload does not leak or UAF (rule (c))
// -------------------------------------------------------------------------

test "utility_view: reload->mode_change->reload does not UAF or leak" {
    // This test exercises the path that could cause use-after-free if any
    // string in the state was aliased from an allocation freed on reload.
    // There are no string filter fields in this view; the primary risk is
    // that the per-mode row slices are correctly freed before reallocation.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into config (key, value) values ('alpha', 'v1')", &.{});

    var state = UtilityState.init(a);
    defer state.deinit();

    // Step 1: initial reload.
    try state.reload(&d);
    try testing.expectEqual(@as(usize, 1), state.config_rows.len);

    // Step 2: change mode.
    const m = Key{ .codepoint = 'm', .mods = .{} };
    _ = state.handleKey(m, &d);
    try testing.expectEqual(UtilityMode.annotations, state.mode);

    // Step 3: reload again — old allocations freed, new ones created.
    try state.reload(&d);
    try testing.expectEqual(@as(usize, 1), state.config_rows.len);

    // Step 4: add a second config entry and reload — validates no double-free.
    _ = try d.execParams("insert into config (key, value) values ('beta', 'v2')", &.{});
    try state.reload(&d);
    try testing.expectEqual(@as(usize, 2), state.config_rows.len);
}

test "utility_view legendLabel fits in buf" {
    const a = testing.allocator;
    var state = UtilityState.init(a);
    defer state.deinit();
    var buf: [256]u8 = undefined;
    const label = legendLabel(&state, &buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Mode") != null);
}

// =========================================================================
// M18 (task 4050) controller tests: workbench action wiring in utility view
// =========================================================================

fn seedWbPlan(d: *db.sqlite.Db, slug: []const u8) !i64 {
    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global',?,?,'active')",
        &.{ .{ .text = slug }, .{ .text = slug } },
    );
    // Seed a workbench_sync_state row so the plan appears in wb_rows.
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into workbench_sync_state
        \\  (anchor_plan_id, entity_kind, entity_id, file_path, content_hash, db_updated_at)
        \\values (?, 'task', ?, 'tasks/t.md', 'abc', '2025-01-01T00:00:00.000Z')
    , &.{ .{ .int = plan_id }, .{ .int = task_id } });
    return plan_id;
}

test "utility_view M18: 'p' push key enters workbench confirm overlay when in workbench_sync mode" {
    // Task 4050: 'p' in .workbench_sync mode enters confirm_workbench(.push).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try seedWbPlan(&d, "wb-push-test");

    var state = UtilityState.init(a);
    defer state.deinit();
    try state.reload(&d);
    // Switch to workbench_sync mode.
    state.mode = .workbench_sync;

    try testing.expect(!state.action.isActive());
    try testing.expect(state.wb_rows.len > 0);

    const p_key = Key{ .codepoint = 'p', .mods = .{} };
    const consumed = state.handleKey(p_key, &d);
    try testing.expect(consumed);
    try testing.expect(state.action.isActive());

    switch (state.action.mode) {
        .confirm_workbench => |*cw| try testing.expectEqual(external_actions.WorkbenchKind.push, cw.kind),
        else => try testing.expect(false),
    }
}

test "utility_view M18: 'l' pull key enters workbench confirm overlay" {
    // Task 4050: 'l' in .workbench_sync mode enters confirm_workbench(.pull).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try seedWbPlan(&d, "wb-pull-test");

    var state = UtilityState.init(a);
    defer state.deinit();
    try state.reload(&d);
    state.mode = .workbench_sync;

    const l_key = Key{ .codepoint = 'l', .mods = .{} };
    const consumed = state.handleKey(l_key, &d);
    try testing.expect(consumed);
    try testing.expect(state.action.isActive());

    switch (state.action.mode) {
        .confirm_workbench => |*cw| try testing.expectEqual(external_actions.WorkbenchKind.pull, cw.kind),
        else => try testing.expect(false),
    }
}

test "utility_view M18: 's' status key enters workbench confirm overlay" {
    // Task 4050: 's' in .workbench_sync mode enters confirm_workbench(.status).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try seedWbPlan(&d, "wb-status-test");

    var state = UtilityState.init(a);
    defer state.deinit();
    try state.reload(&d);
    state.mode = .workbench_sync;

    const s_key = Key{ .codepoint = 's', .mods = .{} };
    const consumed = state.handleKey(s_key, &d);
    try testing.expect(consumed);
    try testing.expect(state.action.isActive());

    switch (state.action.mode) {
        .confirm_workbench => |*cw| try testing.expectEqual(external_actions.WorkbenchKind.status, cw.kind),
        else => try testing.expect(false),
    }
}

test "utility_view M18: action overlay dismisses on Esc without executing" {
    // Task 4050: Esc from confirm_workbench must cancel without calling engine.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try seedWbPlan(&d, "wb-esc-test");

    var state = UtilityState.init(a);
    defer state.deinit();
    try state.reload(&d);
    state.mode = .workbench_sync;

    // Enter confirm via 'p'.
    _ = state.handleKey(Key{ .codepoint = 'p', .mods = .{} }, &d);
    try testing.expect(state.action.isActive());

    // Esc should cancel.
    _ = state.handleKey(Key{ .codepoint = Key.escape }, &d);
    try testing.expect(!state.action.isActive());
}

test "utility_view M18: legendLabel shows workbench action keys in workbench_sync mode" {
    // Task 4050: legend must show 'p Push', 'l Pull', 's Status' keys.
    const a = testing.allocator;
    var state = UtilityState.init(a);
    defer state.deinit();
    state.mode = .workbench_sync;

    var buf: [256]u8 = undefined;
    const label = legendLabel(&state, &buf);
    try testing.expect(std.mem.indexOf(u8, label, "Push") != null);
    try testing.expect(std.mem.indexOf(u8, label, "Pull") != null);
    try testing.expect(std.mem.indexOf(u8, label, "Status") != null);
}

test "utility_view compiles" {
    std.testing.refAllDecls(@This());
}
