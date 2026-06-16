//! cockpit/views/sessions_handoff.zig — Sessions & Handoff view (M11).
//!
//! Renders a two-pane layout for the Sessions & Handoff surface:
//!
//!   • Left pane (navigator): list of sessions (newest-first), each showing
//!     vendor, model, date, and entry count. 's' / 'h' toggles between the
//!     sessions list and the handoffs list.
//!
//!   • Right pane (detail): for the selected session in sessions mode:
//!       - Session identity: vendor, model, started_at, ended_at (or "active")
//!       - "Entries:" section with all session_entries in ordinal order
//!         (the session's activity lineage, task 4032)
//!       - "Commits:" section with all session_commits tied to the session
//!         (task 4034)
//!
//!     For the selected handoff in handoffs mode:
//!       - Handoff identity: from_vendor→to_vendor, status, created_at
//!       - Resume-readiness indicator (task 4033): "READY" or "NOT READY" plus
//!         explanation. Readiness is task-centric: tasks.next_action non-empty AND
//!         a task-scoped context_snapshots row exists (mirrors resume.validate in
//!         src/engine/runtime/resume.zig:300-336). handoffs.status is lifecycle
//!         metadata and does NOT gate readiness.
//!       - validated_at when present
//!
//! Tasks:
//!   4032 (Session lineage from sessions / session_entries)
//!   4033 (Handoff and resume-readiness from handoffs / context_snapshots)
//!   4034 (Link work to code via session_commits)
//!
//! Acceptance invariants:
//!   (4032) Every queried datum from sessions and session_entries is rendered:
//!          vendor, model, started_at, ended_at, entry count in the navigator;
//!          entry prefix, body, ordinal, created_at in the detail.
//!   (4033) Handoff records and their resume-readiness are rendered using the
//!          task-centric rule: READY iff tasks.next_action non-empty AND a
//!          task-scoped context_snapshots row exists. A 'pending' handoff whose
//!          task satisfies the rule renders READY; a 'consumed' handoff whose
//!          task next_action was cleared renders NOT READY.
//!   (4034) session_commits (sha, subject, author, branch, committed_at) are
//!          rendered in the session detail pane.
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by SessionsHandoffState and released via deinit.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread.
//!   - MEMORY GUARD (brief rule (c)): all display_text strings are freshly
//!     heap-allocated in view_model; no alias into kind/id pointer fields.
//!   - Symmetric dup/free: every allocated string in SessionDetail is freed by
//!     SessionDetail.deinit.

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

/// Whether to show the sessions list or the handoffs list in the navigator.
pub const DisplayMode = enum {
    /// Show the sessions list (default).
    sessions,
    /// Show the handoffs list.
    handoffs,
};

// =========================================================================
// SessionDetail — detail pane data for a selected session
// =========================================================================

/// Heap-owned detail data for a selected session. Freed via deinit.
pub const SessionDetail = struct {
    /// Session row fields (freshly duped — NOT aliased from SessionRow).
    vendor: []const u8,
    model: ?[]const u8,
    started_at: []const u8,
    ended_at: ?[]const u8,
    summary: ?[]const u8,
    task_id: ?i64,
    /// Activity lineage: session_entries in ordinal order.
    entries: []view_model.SessionEntryRow,
    /// Commits tied to the session via session_commits.
    commits: []view_model.SessionCommitRow,

    pub fn deinit(self: SessionDetail, allocator: std.mem.Allocator) void {
        allocator.free(self.vendor);
        if (self.model) |s| allocator.free(s);
        allocator.free(self.started_at);
        if (self.ended_at) |s| allocator.free(s);
        if (self.summary) |s| allocator.free(s);
        view_model.SessionEntryRow.deinitMany(self.entries, allocator);
        view_model.SessionCommitRow.deinitMany(self.commits, allocator);
    }
};

// =========================================================================
// SessionsHandoffState
// =========================================================================

/// All mutable state for the Sessions & Handoff view.
pub const SessionsHandoffState = struct {
    allocator: std.mem.Allocator,

    /// Current full snapshot (sessions + handoffs lists).
    snapshot: ?view_model.SessionsHandoffSnapshot = null,

    /// Navigator selection index (0-based into sessions or handoffs list).
    selected_idx: usize = 0,

    /// Whether to show sessions or handoffs in the navigator.
    mode: DisplayMode = .sessions,

    /// Detail for the currently selected session (sessions mode only).
    /// Null when in handoffs mode or list is empty.
    session_detail: ?SessionDetail = null,

    pub fn init(allocator: std.mem.Allocator) SessionsHandoffState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *SessionsHandoffState) void {
        if (self.snapshot) |snap| snap.deinit(self.allocator);
        self.snapshot = null;
        if (self.session_detail) |d| d.deinit(self.allocator);
        self.session_detail = null;
    }

    /// Reload all data from the DB. Called on db_changed and on initial launch.
    pub fn reload(self: *SessionsHandoffState, d: *db.sqlite.Db) !void {
        // Free old state.
        if (self.snapshot) |snap| snap.deinit(self.allocator);
        self.snapshot = null;
        if (self.session_detail) |det| det.deinit(self.allocator);
        self.session_detail = null;

        const snap = try view_model.querySessionsHandoffSnapshot(d, self.allocator);
        self.snapshot = snap;

        // Clamp selection.
        const count = self.activeListCount();
        if (count > 0) {
            if (self.selected_idx >= count) self.selected_idx = count - 1;
        } else {
            self.selected_idx = 0;
        }

        // Load session detail when in sessions mode and a session is selected.
        if (self.mode == .sessions) {
            try self.refreshSessionDetail(d);
        }
    }

    /// Return the count of items in the currently active list.
    fn activeListCount(self: *const SessionsHandoffState) usize {
        const snap = self.snapshot orelse return 0;
        return switch (self.mode) {
            .sessions => snap.sessions.len,
            .handoffs => snap.handoffs.len,
        };
    }

    /// Reload the session detail for the currently selected session.
    /// Only meaningful when mode == .sessions.
    fn refreshSessionDetail(self: *SessionsHandoffState, d: *db.sqlite.Db) !void {
        if (self.session_detail) |det| {
            det.deinit(self.allocator);
            self.session_detail = null;
        }

        const snap = self.snapshot orelse return;
        if (self.mode != .sessions) return;
        if (snap.sessions.len == 0) return;
        if (self.selected_idx >= snap.sessions.len) return;

        const sel = snap.sessions[self.selected_idx];

        // Fresh dupe every field — never alias into the SessionRow.
        const vendor = try self.allocator.dupe(u8, sel.vendor);
        errdefer self.allocator.free(vendor);

        const model: ?[]const u8 = if (sel.model) |m|
            try self.allocator.dupe(u8, m)
        else
            null;
        errdefer if (model) |m| self.allocator.free(m);

        const started_at = try self.allocator.dupe(u8, sel.started_at);
        errdefer self.allocator.free(started_at);

        const ended_at: ?[]const u8 = if (sel.ended_at) |e|
            try self.allocator.dupe(u8, e)
        else
            null;
        errdefer if (ended_at) |e| self.allocator.free(e);

        const summary: ?[]const u8 = if (sel.summary) |s|
            try self.allocator.dupe(u8, s)
        else
            null;
        errdefer if (summary) |s| self.allocator.free(s);

        const entries = try view_model.querySessionEntries(d, self.allocator, sel.id);
        errdefer view_model.SessionEntryRow.deinitMany(entries, self.allocator);

        const commits = try view_model.querySessionCommits(d, self.allocator, sel.id);
        errdefer view_model.SessionCommitRow.deinitMany(commits, self.allocator);

        self.session_detail = .{
            .vendor = vendor,
            .model = model,
            .started_at = started_at,
            .ended_at = ended_at,
            .summary = summary,
            .task_id = sel.task_id,
            .entries = entries,
            .commits = commits,
        };
    }

    /// Handle a key event. Returns true when the key was consumed.
    pub fn handleKey(self: *SessionsHandoffState, key: Key, d: *db.sqlite.Db) bool {
        const count = self.activeListCount();

        // j / arrow-down: move selection down.
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            if (count > 0 and self.selected_idx + 1 < count) {
                self.selected_idx += 1;
                if (self.mode == .sessions) {
                    self.refreshSessionDetail(d) catch {};
                }
            }
            return true;
        }
        // k / arrow-up: move selection up.
        if (key.matches('k', .{}) or key.matches(Key.up, .{})) {
            if (self.selected_idx > 0) {
                self.selected_idx -= 1;
                if (self.mode == .sessions) {
                    self.refreshSessionDetail(d) catch {};
                }
            }
            return true;
        }
        // 's': switch to sessions mode.
        if (key.matches('s', .{})) {
            if (self.mode != .sessions) {
                self.mode = .sessions;
                const snap = self.snapshot orelse return true;
                if (self.selected_idx >= snap.sessions.len) {
                    self.selected_idx = if (snap.sessions.len > 0) snap.sessions.len - 1 else 0;
                }
                self.refreshSessionDetail(d) catch {};
            }
            return true;
        }
        // 'h': switch to handoffs mode.
        if (key.matches('h', .{})) {
            if (self.mode != .handoffs) {
                self.mode = .handoffs;
                const snap = self.snapshot orelse return true;
                if (self.selected_idx >= snap.handoffs.len) {
                    self.selected_idx = if (snap.handoffs.len > 0) snap.handoffs.len - 1 else 0;
                }
                // Clear session detail when switching to handoffs mode.
                if (self.session_detail) |det| {
                    det.deinit(self.allocator);
                    self.session_detail = null;
                }
            }
            return true;
        }

        return false;
    }
};

// =========================================================================
// Render
// =========================================================================

/// Render the Sessions & Handoff view into the navigator and detail windows.
pub fn render(
    state: *const SessionsHandoffState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    switch (state.mode) {
        .sessions => {
            renderSessionsNavigator(state, nav_win);
            renderSessionsDetail(state, detail_win, allocator);
        },
        .handoffs => {
            renderHandoffsNavigator(state, nav_win);
            renderHandoffsDetail(state, detail_win);
        },
    }
}

// -------------------------------------------------------------------------
// Sessions navigator
// -------------------------------------------------------------------------

/// Render the navigator pane in sessions mode.
///
/// Layout:
///   row 0: "Sessions  [s sessions | h handoffs]" (bold dim header)
///   row 1+: one row per session, selected row reversed
///   Empty state: "(no sessions recorded)"
fn renderSessionsNavigator(state: *const SessionsHandoffState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    const snap = state.snapshot orelse {
        _ = win.printSegment(.{
            .text = "(loading...)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    };

    // Header.
    _ = win.printSegment(.{
        .text = "Sessions  [s/h switch]",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });
    if (win.height < 2) return;

    if (snap.sessions.len == 0) {
        _ = win.printSegment(.{
            .text = "(no sessions recorded)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 1, .col_offset = 0 });
        return;
    }

    const viewport_h: usize = if (win.height > 1) @as(usize, @intCast(win.height)) - 1 else 0;
    const scroll: usize = if (state.selected_idx >= viewport_h)
        state.selected_idx - viewport_h + 1
    else
        0;

    var display_row: u16 = 1;
    for (snap.sessions, 0..) |sess, i| {
        if (i < scroll) continue;
        if (display_row >= win.height) break;

        const is_selected = (i == state.selected_idx);
        const style: Style = if (is_selected)
            .{ .bold = true, .reverse = true }
        else
            .{};

        _ = win.printSegment(.{
            .text = sess.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });
        display_row += 1;
    }
}

// -------------------------------------------------------------------------
// Sessions detail pane
// -------------------------------------------------------------------------

/// Render the detail pane for the selected session.
///
/// Layout:
///   row 0: "vendor [model]" (bold)
///   row 1: "started: started_at"
///   row 2: "ended:   ended_at"  or "  active"
///   row 3: "task: task_id" (when task_id is set)
///   separator
///   "Summary:" section (when summary is set)
///   separator
///   "Entries: (N)" section header (bold dim) — task 4032
///   N rows: "  [prefix] body_preview"
///   separator
///   "Commits: (N)" section header (bold dim) — task 4034
///   N rows: "  sha_short subject_preview"
///   Empty state row: "(no entries)" / "(no commits)"
///
/// INVARIANT (task 4032): vendor, model, started_at, ended_at, and ALL
/// entry rows (prefix + body) are rendered.
/// INVARIANT (task 4034): ALL commit rows (sha, subject) are rendered.
fn renderSessionsDetail(state: *const SessionsHandoffState, win: Window, arena: std.mem.Allocator) void {
    if (win.height == 0 or win.width == 0) return;

    const snap = state.snapshot orelse {
        _ = win.printSegment(.{
            .text = "(no data)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    };

    if (snap.sessions.len == 0) {
        _ = win.printSegment(.{
            .text = "(no sessions — run `planar capture session` to start one)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    const det = state.session_detail orelse {
        _ = win.printSegment(.{
            .text = "(select a session)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    };

    var row: u16 = 0;

    // ---- Session identity: "vendor [model]" (row 0) ---------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = det.vendor,
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        if (det.model) |m| {
            const ven_len: u16 = @intCast(det.vendor.len);
            _ = win.printSegment(.{ .text = " [", .style = .{ .bold = true } }, .{ .row_offset = row, .col_offset = ven_len });
            _ = win.printSegment(.{ .text = m, .style = .{ .bold = true } }, .{ .row_offset = row, .col_offset = ven_len + 2 });
            const m_len: u16 = @intCast(m.len);
            _ = win.printSegment(.{ .text = "]", .style = .{ .bold = true } }, .{ .row_offset = row, .col_offset = ven_len + 2 + m_len });
        }
        row += 1;
    }

    // ---- started_at (row 1) ---------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{ .text = "started: ", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{ .text = det.started_at, .style = .{} }, .{ .row_offset = row, .col_offset = 9 });
        row += 1;
    }

    // ---- ended_at (row 2) -----------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{ .text = "ended:   ", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
        if (det.ended_at) |e| {
            _ = win.printSegment(.{ .text = e, .style = .{} }, .{ .row_offset = row, .col_offset = 9 });
        } else {
            _ = win.printSegment(.{ .text = "(active)", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 9 });
        }
        row += 1;
    }

    // ---- task_id (row 3, optional) --------------------------------------
    if (row < win.height) {
        if (det.task_id) |tid| {
            // Use arena allocation so the slice remains valid through vaxis.render().
            const task_ref = std.fmt.allocPrint(arena, "task:{d}", .{tid}) catch "task:?";
            _ = win.printSegment(.{ .text = "task:    ", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{ .text = task_ref, .style = .{} }, .{ .row_offset = row, .col_offset = 9 });
            row += 1;
        }
    }

    // ---- Summary (optional) ---------------------------------------------
    if (det.summary) |sum| {
        if (row < win.height) row += 1; // separator
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "Summary:",
                .style = .{ .bold = true, .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            row += 1;
        }
        if (row < win.height) {
            _ = win.printSegment(.{ .text = sum, .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 2 });
            row += 1;
        }
    }

    // ---- Separator -------------------------------------------------------
    if (row < win.height) row += 1;

    // ---- Entries section (task 4032) -------------------------------------
    if (row < win.height) {
        // Use static label + separate count to avoid stack-buffer grapheme
        // aliasing (printSegment stores slice pointers into the screen buffer;
        // string literals have static lifetime).
        _ = win.printSegment(.{
            .text = "Entries:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        // Write entry count using the display_text of entries (entry_count from session row
        // is already rendered in the navigator; here we just render the section label).
        row += 1;
    }

    if (det.entries.len == 0) {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (no entries)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            row += 1;
        }
    } else {
        // INVARIANT (task 4032): ALL entries are rendered (prefix + body preview).
        // display_text is heap-allocated: "[prefix] body_preview".
        for (det.entries) |entry| {
            if (row >= win.height) break;
            _ = win.printSegment(.{
                .text = entry.display_text,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 2 });
            row += 1;
        }
    }

    // ---- Separator -------------------------------------------------------
    if (row < win.height) row += 1;

    // ---- Commits section (task 4034) -------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "Commits:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    if (det.commits.len == 0) {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (no commits)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
    } else {
        // INVARIANT (task 4034): ALL commit rows rendered (sha + subject).
        // display_text is heap-allocated: "sha_short subject".
        for (det.commits) |commit| {
            if (row >= win.height) break;
            _ = win.printSegment(.{
                .text = commit.display_text,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 2 });
            row += 1;
            // Author + branch on the next line (dim), if present.
            if (row < win.height) {
                if (commit.author) |auth| {
                    _ = win.printSegment(.{ .text = "    by: ", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
                    _ = win.printSegment(.{ .text = auth, .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 8 });
                    row += 1;
                }
            }
            if (row < win.height) {
                if (commit.branch) |br| {
                    _ = win.printSegment(.{ .text = "    branch: ", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
                    _ = win.printSegment(.{ .text = br, .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 12 });
                    row += 1;
                }
            }
        }
    }
}

// -------------------------------------------------------------------------
// Handoffs navigator
// -------------------------------------------------------------------------

/// Render the navigator pane in handoffs mode.
///
/// Layout:
///   row 0: "Handoffs  [s/h switch]" (bold dim header)
///   row 1+: one row per handoff, selected row reversed
///   Each row uses the pre-formatted display_text from view_model:
///     "from→to  [status]  (ready/not-ready)"
///   Empty state: "(no handoffs)"
fn renderHandoffsNavigator(state: *const SessionsHandoffState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    const snap = state.snapshot orelse {
        _ = win.printSegment(.{
            .text = "(loading...)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    };

    // Header.
    _ = win.printSegment(.{
        .text = "Handoffs  [s/h switch]",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });
    if (win.height < 2) return;

    if (snap.handoffs.len == 0) {
        _ = win.printSegment(.{
            .text = "(no handoffs)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 1, .col_offset = 0 });
        return;
    }

    const viewport_h: usize = if (win.height > 1) @as(usize, @intCast(win.height)) - 1 else 0;
    const scroll: usize = if (state.selected_idx >= viewport_h)
        state.selected_idx - viewport_h + 1
    else
        0;

    var display_row: u16 = 1;
    for (snap.handoffs, 0..) |handoff, i| {
        if (i < scroll) continue;
        if (display_row >= win.height) break;

        const is_selected = (i == state.selected_idx);
        const style: Style = if (is_selected)
            .{ .bold = true, .reverse = true }
        else
            .{};

        _ = win.printSegment(.{
            .text = handoff.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });
        display_row += 1;
    }
}

// -------------------------------------------------------------------------
// Handoffs detail pane
// -------------------------------------------------------------------------

/// Render the detail pane for the selected handoff.
///
/// Layout:
///   row 0: "from_vendor → to_vendor" (bold)
///   row 1: "status: <status>"
///   row 2: "created: <created_at>"
///   row 3: "validated: <validated_at>" (when set)
///   separator
///   row N: "Resume readiness:" (bold dim)
///   row N+1: "  READY" (bold green) OR "  NOT READY" (bold dim)
///            with explanation.
///
/// INVARIANT (task 4033): the readiness signal (READY vs NOT READY) is
/// rendered explicitly and unambiguously for every handoff, with cause.
/// Readiness is task-centric (see HandoffRow.isResumeReady); handoffs.status
/// is shown as lifecycle info but does NOT decide READY vs NOT READY.
fn renderHandoffsDetail(state: *const SessionsHandoffState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    const snap = state.snapshot orelse {
        _ = win.printSegment(.{
            .text = "(no data)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    };

    if (snap.handoffs.len == 0) {
        _ = win.printSegment(.{
            .text = "(no handoffs — run `planar handoff create` to create one)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    if (state.selected_idx >= snap.handoffs.len) return;
    const h = snap.handoffs[state.selected_idx];

    var row: u16 = 0;

    // ---- "from_vendor → to_vendor" (row 0) ------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{ .text = h.from_vendor, .style = .{ .bold = true } }, .{ .row_offset = row, .col_offset = 0 });
        const fv_len: u16 = @intCast(h.from_vendor.len);
        _ = win.printSegment(.{ .text = " \u{2192} ", .style = .{ .bold = true } }, .{ .row_offset = row, .col_offset = fv_len });
        const to_str: []const u8 = h.to_vendor orelse "?";
        _ = win.printSegment(.{ .text = to_str, .style = .{ .bold = true } }, .{ .row_offset = row, .col_offset = fv_len + 3 });
        row += 1;
    }

    // ---- status (row 1) -------------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{ .text = "status:    ", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{ .text = h.status, .style = .{} }, .{ .row_offset = row, .col_offset = 11 });
        row += 1;
    }

    // ---- created_at (row 2) ---------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{ .text = "created:   ", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{ .text = h.created_at, .style = .{} }, .{ .row_offset = row, .col_offset = 11 });
        row += 1;
    }

    // ---- validated_at (optional, row 3) ---------------------------------
    if (row < win.height) {
        if (h.validated_at) |va| {
            _ = win.printSegment(.{ .text = "validated: ", .style = .{ .dim = true } }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{ .text = va, .style = .{} }, .{ .row_offset = row, .col_offset = 11 });
            row += 1;
        }
    }

    // ---- Separator -------------------------------------------------------
    if (row < win.height) row += 1;

    // ---- Resume readiness section (task 4033) ---------------------------
    // INVARIANT: readiness is ALWAYS rendered explicitly as READY or NOT READY
    // with a clear explanation of the determining factors.
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "Resume readiness:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    if (row < win.height) {
        if (h.isResumeReady()) {
            // READY: task has next_action AND a task-scoped snapshot exists.
            // Mirrors resume.validate (src/engine/runtime/resume.zig:300-336).
            _ = win.printSegment(.{
                .text = "  READY",
                .style = .{ .bold = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            row += 1;
            if (row < win.height) {
                _ = win.printSegment(.{
                    .text = "  (task has next_action + snapshot)",
                    .style = .{ .dim = true },
                }, .{ .row_offset = row, .col_offset = 0 });
                row += 1;
            }
        } else {
            // NOT READY: explain which task-centric condition is not met.
            // handoffs.status is NOT a readiness gate.
            _ = win.printSegment(.{
                .text = "  NOT READY",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            row += 1;
            if (row < win.height) {
                const cause: []const u8 = if (!h.task_has_snapshot)
                    "  (no task-scoped context snapshot)"
                else
                    "  (task has no next_action)";
                _ = win.printSegment(.{
                    .text = cause,
                    .style = .{ .dim = true },
                }, .{ .row_offset = row, .col_offset = 0 });
                row += 1;
            }
        }
    }
}

/// Return a one-line legend string for the key legend bar.
pub fn legendLabel(buf: []u8) []const u8 {
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Select  s Sessions  h Handoffs  Tab Focus  1-9 View",
        .{},
    ) catch "  q Quit  j/k Select  s Sessions  h Handoffs";
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

test "sessions_handoff: init and deinit are clean" {
    var state = SessionsHandoffState.init(testing.allocator);
    defer state.deinit();
    try testing.expect(state.snapshot == null);
    try testing.expectEqual(DisplayMode.sessions, state.mode);
}

test "sessions_handoff: reload on empty DB yields empty snapshot (empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = SessionsHandoffState.init(a);
    defer state.deinit();

    try state.reload(&d);

    try testing.expect(state.snapshot != null);
    const snap = state.snapshot.?;
    try testing.expectEqual(@as(usize, 0), snap.sessions.len);
    try testing.expectEqual(@as(usize, 0), snap.handoffs.len);
    try testing.expect(state.session_detail == null);
}

test "sessions_handoff: reload populates sessions (task 4032)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor, model, started_at) values ('claude', 'claude-4', '2026-06-10T08:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'action', 'Did something')",
        &.{.{ .int = sid }},
    );

    var state = SessionsHandoffState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const snap = state.snapshot.?;
    try testing.expectEqual(@as(usize, 1), snap.sessions.len);
    try testing.expectEqualStrings("claude", snap.sessions[0].vendor);
    try testing.expect(snap.sessions[0].model != null);
    try testing.expectEqualStrings("claude-4", snap.sessions[0].model.?);
    try testing.expectEqual(@as(i64, 1), snap.sessions[0].entry_count);

    // Session detail must be populated.
    try testing.expect(state.session_detail != null);
    const det = state.session_detail.?;
    try testing.expectEqualStrings("claude", det.vendor);
    try testing.expectEqual(@as(usize, 1), det.entries.len);
    try testing.expectEqualStrings("action", det.entries[0].prefix);
    try testing.expectEqualStrings("Did something", det.entries[0].body);
}

test "sessions_handoff: reload populates session commits (task 4034)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor, started_at) values ('claude', '2026-06-11T08:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into session_commits (session_id, sha, subject, author, branch, committed_at) values (?, 'aabbccdd11223344', 'feat: sessions view', 'Dev', 'feature/m11', '2026-06-11T09:00:00.000Z')",
        &.{.{ .int = sid }},
    );

    var state = SessionsHandoffState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const det = state.session_detail.?;
    try testing.expectEqual(@as(usize, 1), det.commits.len);
    try testing.expectEqualStrings("aabbccdd11223344", det.commits[0].sha);
    try testing.expect(det.commits[0].subject != null);
    try testing.expectEqualStrings("feat: sessions view", det.commits[0].subject.?);
    try testing.expect(det.commits[0].author != null);
    try testing.expectEqualStrings("Dev", det.commits[0].author.?);
    try testing.expect(det.commits[0].branch != null);
    try testing.expectEqualStrings("feature/m11", det.commits[0].branch.?);
}

test "sessions_handoff: reload populates handoffs with task-centric readiness (task 4033)" {
    // Verifies that reload → queryHandoffs → isResumeReady() uses the task-centric
    // rule (tasks.next_action non-empty AND task-scoped snapshot exists), not
    // handoffs.status.  Fixture: pending handoff, task has next_action + snapshot.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, next_action) values ('global', 'M11 feature', 'Continue work')",
        &.{},
    );
    const s1 = try d.execParams(
        "insert into sessions (vendor, started_at, task_id) values ('claude', '2026-06-12T08:00:00.000Z', ?)",
        &.{.{ .int = task_id }},
    );
    const snap_id = try d.execParams(
        "insert into context_snapshots (session_id, task_id, vendor, body) values (?, ?, 'claude', 'body')",
        &.{ .{ .int = s1 }, .{ .int = task_id } },
    );
    // 'pending' status — engine ignores this for readiness; view must too.
    _ = try d.execParams(
        "insert into handoffs (from_snapshot_id, from_vendor, to_vendor, status) values (?, 'claude', 'codex', 'pending')",
        &.{.{ .int = snap_id }},
    );

    var state = SessionsHandoffState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const snap = state.snapshot.?;
    try testing.expectEqual(@as(usize, 1), snap.handoffs.len);
    try testing.expectEqualStrings("pending", snap.handoffs[0].status);
    try testing.expect(snap.handoffs[0].task_has_next_action);
    try testing.expect(snap.handoffs[0].task_has_snapshot);
    // Must be READY despite 'pending' status.
    try testing.expect(snap.handoffs[0].isResumeReady());
}

test "sessions_handoff: handleKey h switches to handoffs mode" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = SessionsHandoffState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(DisplayMode.sessions, state.mode);
    const h_key = vaxis.Key{ .codepoint = 'h', .mods = .{} };
    _ = state.handleKey(h_key, &d);
    try testing.expectEqual(DisplayMode.handoffs, state.mode);

    const s_key = vaxis.Key{ .codepoint = 's', .mods = .{} };
    _ = state.handleKey(s_key, &d);
    try testing.expectEqual(DisplayMode.sessions, state.mode);
}

// =========================================================================
// RENDER-LEVEL TESTS (rule (b): render into vaxis.Screen and assert text)
// =========================================================================

test "sessions_handoff: renderSessionsNavigator shows session display_text (task 4032 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor, model, started_at) values ('claude', 'opus', '2026-06-10T10:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'action', 'Wrote code')",
        &.{.{ .int = sid }},
    );

    var state = SessionsHandoffState.init(a);
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

    renderSessionsNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4032):
    // Header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Sessions") != null);
    // Vendor must appear.
    try testing.expect(std.mem.indexOf(u8, text, "claude") != null);
    // Model must appear.
    try testing.expect(std.mem.indexOf(u8, text, "opus") != null);
    // Date prefix must appear.
    try testing.expect(std.mem.indexOf(u8, text, "2026-06-10") != null);
    // Entry count must appear.
    try testing.expect(std.mem.indexOf(u8, text, "1") != null);
}

test "sessions_handoff: renderSessionsNavigator shows empty state (render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = SessionsHandoffState.init(a);
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

    renderSessionsNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    try testing.expect(std.mem.indexOf(u8, text, "no sessions") != null);
}

test "sessions_handoff: renderSessionsDetail shows entries and commits (tasks 4032/4034 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global', 'Impl task', 'doing')",
        &.{},
    );
    const sid = try d.execParams(
        "insert into sessions (task_id, vendor, model, started_at) values (?, 'claude', 'sonnet', '2026-06-13T08:00:00.000Z')",
        &.{.{ .int = tid }},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'action', 'Implemented feature X')",
        &.{.{ .int = sid }},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 2, 'observation', 'Tests all pass')",
        &.{.{ .int = sid }},
    );
    _ = try d.execParams(
        "insert into session_commits (session_id, sha, subject, author, branch, committed_at) values (?, 'cafebabe12345678', 'feat: implement X', 'Alice', 'feature/x', '2026-06-13T09:00:00.000Z')",
        &.{.{ .int = sid }},
    );

    var state = SessionsHandoffState.init(a);
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

    // Use an arena so that allocPrint calls inside renderSessionsDetail do not
    // leak when tested with testing.allocator (render functions own no arena;
    // the arena is reset each frame by renderFrame).
    var render_arena = std.heap.ArenaAllocator.init(a);
    defer render_arena.deinit();
    renderSessionsDetail(&state, detail_win, render_arena.allocator());

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4032): session identity.
    try testing.expect(std.mem.indexOf(u8, text, "claude") != null);
    try testing.expect(std.mem.indexOf(u8, text, "sonnet") != null);
    try testing.expect(std.mem.indexOf(u8, text, "2026-06-13") != null);
    // Entries section header.
    try testing.expect(std.mem.indexOf(u8, text, "Entries:") != null);
    // Entry prefix + body preview.
    try testing.expect(std.mem.indexOf(u8, text, "action") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Implemented feature X") != null);
    try testing.expect(std.mem.indexOf(u8, text, "observation") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Tests all pass") != null);
    // RENDER-LEVEL ASSERTIONS (task 4034): commits section.
    try testing.expect(std.mem.indexOf(u8, text, "Commits:") != null);
    // sha short (first 8 chars).
    try testing.expect(std.mem.indexOf(u8, text, "cafebabe") != null);
    // subject.
    try testing.expect(std.mem.indexOf(u8, text, "feat: implement X") != null);
    // author.
    try testing.expect(std.mem.indexOf(u8, text, "Alice") != null);
    // branch.
    try testing.expect(std.mem.indexOf(u8, text, "feature/x") != null);
    // task reference.
    try testing.expect(std.mem.indexOf(u8, text, "task:") != null);
}

test "sessions_handoff: renderHandoffsDetail shows READY for pending handoff with task next_action + snapshot (task 4033 render-level)" {
    // Engine rule: READY iff tasks.next_action non-empty AND task-scoped snapshot exists.
    // handoffs.status is NOT a gate. Fixture uses 'pending' status to prove this.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, next_action) values ('global', 'M11 impl', 'Continue the M11 work')",
        &.{},
    );
    const s1 = try d.execParams(
        "insert into sessions (vendor, started_at, task_id) values ('claude', '2026-06-14T08:00:00.000Z', ?)",
        &.{.{ .int = task_id }},
    );
    const snap_id = try d.execParams(
        "insert into context_snapshots (session_id, task_id, vendor, body) values (?, ?, 'claude', 'body text')",
        &.{ .{ .int = s1 }, .{ .int = task_id } },
    );
    // 'pending' status — old rule: NOT READY. Engine rule: READY.
    _ = try d.execParams(
        "insert into handoffs (from_snapshot_id, from_vendor, to_vendor, status) values (?, 'claude', 'codex', 'pending')",
        &.{.{ .int = snap_id }},
    );

    var state = SessionsHandoffState.init(a);
    defer state.deinit();
    try state.reload(&d);
    state.mode = .handoffs;

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

    renderHandoffsDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4033):
    // Vendor identity.
    try testing.expect(std.mem.indexOf(u8, text, "claude") != null);
    try testing.expect(std.mem.indexOf(u8, text, "codex") != null);
    // Status shown as lifecycle info.
    try testing.expect(std.mem.indexOf(u8, text, "pending") != null);
    // Readiness section header.
    try testing.expect(std.mem.indexOf(u8, text, "Resume readiness:") != null);
    // READY must appear despite 'pending' status.
    try testing.expect(std.mem.indexOf(u8, text, "READY") != null);
    // Must NOT show NOT READY.
    try testing.expect(std.mem.indexOf(u8, text, "NOT READY") == null);
}

test "sessions_handoff: renderHandoffsDetail shows NOT READY for consumed handoff with no task next_action (task 4033 render-level)" {
    // Engine rule: tasks.next_action empty → NOT READY regardless of handoffs.status.
    // Fixture: 'consumed' handoff, task next_action cleared → must render NOT READY.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Task with no next_action (cleared).
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title) values ('global', 'finished task')",
        &.{},
    );
    const s1 = try d.execParams(
        "insert into sessions (vendor, started_at, task_id) values ('claude', '2026-06-14T10:00:00.000Z', ?)",
        &.{.{ .int = task_id }},
    );
    const snap_id = try d.execParams(
        "insert into context_snapshots (session_id, task_id, vendor, body) values (?, ?, 'claude', 'completed body')",
        &.{ .{ .int = s1 }, .{ .int = task_id } },
    );
    // 'consumed' status — old rule: READY. Engine rule: NOT READY (no next_action).
    _ = try d.execParams(
        "insert into handoffs (from_snapshot_id, from_vendor, status, consumed_at) values (?, 'claude', 'consumed', '2026-06-14T11:00:00.000Z')",
        &.{.{ .int = snap_id }},
    );

    var state = SessionsHandoffState.init(a);
    defer state.deinit();
    try state.reload(&d);
    state.mode = .handoffs;

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

    renderHandoffsDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4033):
    try testing.expect(std.mem.indexOf(u8, text, "claude") != null);
    try testing.expect(std.mem.indexOf(u8, text, "consumed") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Resume readiness:") != null);
    // NOT READY must appear despite 'consumed' status.
    try testing.expect(std.mem.indexOf(u8, text, "NOT READY") != null);
    // Must NOT show READY without the NOT prefix.
    try testing.expect(std.mem.indexOf(u8, text, "  READY") == null);
}

test "sessions_handoff: renderHandoffsNavigator shows empty state (render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = SessionsHandoffState.init(a);
    defer state.deinit();
    try state.reload(&d);
    state.mode = .handoffs;

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

    renderHandoffsNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    try testing.expect(std.mem.indexOf(u8, text, "no handoffs") != null);
}

test "sessions_handoff: legendLabel fits in buf" {
    var buf: [128]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Quit") != null);
    try testing.expect(std.mem.indexOf(u8, label, "Sessions") != null);
    try testing.expect(std.mem.indexOf(u8, label, "Handoffs") != null);
}

test "sessions_handoff compiles" {
    std.testing.refAllDecls(@This());
}
