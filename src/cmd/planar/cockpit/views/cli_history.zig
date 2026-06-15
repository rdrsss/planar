//! cockpit/views/cli_history.zig — CLI Invocation History view (M13).
//!
//! Renders a two-pane layout for the CLI Invocation History:
//!
//!   • Left pane (navigator): cli_invocations rows, newest-first. Each row
//!     shows:
//!       "[OK]  verb_path  args_shape  ts_short"  (exit_code = 0)
//!       "[ERR] verb_path  args_shape  ts_short"  (exit_code ≠ 0)
//!     j/k (or arrow keys) navigate.
//!     'v' cycles the verb filter.
//!     's' cycles the scope filter.
//!     't' cycles the time-bucket filter (all → last hour → last 24h → all).
//!
//!   • Right pane (detail): all queried fields for the selected row:
//!       - verb_path
//!       - args_shape
//!       - outcome: "OK" (green dim) or "ERR: error_category" (when failed)
//!       - scope_slug (or "(none)")
//!       - duration_ms (or "(none)")
//!       - recorded_at
//!
//!   • Status bar: the active filter summary
//!       "Verb: <verb>  Scope: <scope>  Time: <bucket>"
//!     is rendered in the navigator header so the operator always sees
//!     which filters are active.
//!
//! Tasks 4037 (list cli_invocations with verb/args/outcome) and
//!      4038 (filter by verb / scope / time).
//!
//! Acceptance invariants:
//!   (4037) ALL queried cli_invocations row fields are rendered:
//!          verb_path, args_shape, exit_code/outcome, error_category,
//!          scope_slug, duration_ms, recorded_at.
//!   (4038) Verb, scope, and time filters narrow the rendered set. The
//!          active filter state is visible in the navigator header.
//!          Rows outside the active filter are NOT present in the
//!          rendered output.
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by CliHistoryState and released via deinit.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread.
//!   - MEMORY GUARD (brief rule (c) — critical for this view):
//!       * filter.verb  is an independently heap-allocated duplicate when
//!         set (setFilterVerb dupes; resetVerbFilter frees). It NEVER
//!         aliases into verbs[] or rows[].verb_path — those lists are freed
//!         and reallocated on every reload().
//!       * filter.scope is an independently heap-allocated duplicate when
//!         set (setFilterScope dupes; resetScopeFilter frees). It NEVER
//!         aliases into scopes[] or rows[].scope_slug.
//!       * The reload→setFilter→reload regression test exercises this path
//!         under testing.allocator which detects UAF/double-free/leak.
//!   - 11th view: registered with key 'h' (display-only; numeric jump fires
//!     only for '1'–'9'). Tab/Shift-Tab cycling reaches this view.
//!
//! Schema confirmed from migrations/00020_cli_invocations.up.sql:
//!   cli_invocations(id, verb_path, args_shape, exit_code, error_category,
//!                   scope_slug, duration_ms, recorded_at)

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// CliHistoryState
// =========================================================================

/// All mutable state for the CLI Invocation History view.
pub const CliHistoryState = struct {
    allocator: std.mem.Allocator,

    /// Current snapshot of cli_invocations rows, ordered newest-first.
    rows: []view_model.CliInvocationRow = &.{},

    /// Distinct verb_path values present in cli_invocations (for verb-filter
    /// cycling). Each element is a freshly heap-allocated string.
    verbs: [][]const u8 = &.{},

    /// Distinct scope_slug values present in cli_invocations (for scope-filter
    /// cycling). Each element is a freshly heap-allocated string.
    scopes: [][]const u8 = &.{},

    /// Active composite filter. The .verb and .scope fields (when non-null)
    /// are independently heap-allocated duplicates owned by this state —
    /// they NEVER alias into verbs[], scopes[], or rows[] (see MEMORY GUARD).
    filter: view_model.CliHistoryFilter = .{},

    /// Whether filter.verb is an owned (duped) string that we must free.
    filter_verb_owned: bool = false,

    /// Whether filter.scope is an owned (duped) string that we must free.
    filter_scope_owned: bool = false,

    /// Pre-formatted filter header string, e.g.
    ///   "Verb: task add  Scope: myrepo  Time: last hour"
    /// Heap-allocated so the navigator renderer uses a stable pointer.
    /// Freed by updateFilterLabel / deinit.
    filter_label: []const u8 = "Verb: (all)  Scope: (all)  Time: all time",

    /// Whether filter_label is heap-allocated (must be freed on replace/deinit).
    filter_label_owned: bool = false,

    /// Navigator selection index (0-based into rows).
    selected_idx: usize = 0,

    /// Navigator scroll offset.
    scroll_offset: usize = 0,

    pub fn init(allocator: std.mem.Allocator) CliHistoryState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *CliHistoryState) void {
        view_model.CliInvocationRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        self.freeVerbs();
        self.freeScopes();
        // Free owned filter strings before clearing.
        self.resetVerbFilter();
        self.resetScopeFilter();
        if (self.filter_label_owned) {
            self.allocator.free(self.filter_label);
        }
        self.filter_label = "Verb: (all)  Scope: (all)  Time: all time";
        self.filter_label_owned = false;
    }

    // ---- Internal helpers to free verbs/scopes lists ----------------------

    fn freeVerbs(self: *CliHistoryState) void {
        for (self.verbs) |v| self.allocator.free(v);
        self.allocator.free(self.verbs);
        self.verbs = &.{};
    }

    fn freeScopes(self: *CliHistoryState) void {
        for (self.scopes) |s| self.allocator.free(s);
        self.allocator.free(self.scopes);
        self.scopes = &.{};
    }

    // ---- Filter ownership helpers (MEMORY GUARD rule (c)) -----------------

    /// Set the verb filter to an independently owned copy of `verb`.
    /// Frees the previously-owned verb string if one was set.
    /// On allocation failure the filter remains unchanged and the error is
    /// returned — no partial state is written.
    fn setFilterVerb(self: *CliHistoryState, verb: []const u8) !void {
        const owned = try self.allocator.dupe(u8, verb);
        // Allocation succeeded: free the old owned verb (if any) before
        // replacing it. This ordering prevents leaks on replace.
        if (self.filter_verb_owned) {
            self.allocator.free(self.filter.verb.?);
        }
        self.filter.verb = owned;
        self.filter_verb_owned = true;
    }

    /// Reset the verb filter to null, freeing the owned string if one was set.
    fn resetVerbFilter(self: *CliHistoryState) void {
        if (self.filter_verb_owned) {
            self.allocator.free(self.filter.verb.?);
            self.filter_verb_owned = false;
        }
        self.filter.verb = null;
    }

    /// Set the scope filter to an independently owned copy of `scope`.
    /// Frees the previously-owned scope string if one was set.
    /// On allocation failure the filter remains unchanged.
    fn setFilterScope(self: *CliHistoryState, scope: []const u8) !void {
        const owned = try self.allocator.dupe(u8, scope);
        if (self.filter_scope_owned) {
            self.allocator.free(self.filter.scope.?);
        }
        self.filter.scope = owned;
        self.filter_scope_owned = true;
    }

    /// Reset the scope filter to null, freeing the owned string if one was set.
    fn resetScopeFilter(self: *CliHistoryState) void {
        if (self.filter_scope_owned) {
            self.allocator.free(self.filter.scope.?);
            self.filter_scope_owned = false;
        }
        self.filter.scope = null;
    }

    /// Rebuild the pre-formatted filter_label from the current filter state.
    /// MEMORY GUARD: allocates a fresh heap string; frees the old one first.
    fn updateFilterLabel(self: *CliHistoryState) !void {
        const verb_display: []const u8 = self.filter.verb orelse "(all)";
        const scope_display: []const u8 = self.filter.scope orelse "(all)";
        const time_display: []const u8 = self.filter.time.label();

        const new_label = try std.fmt.allocPrint(
            self.allocator,
            "Verb: {s}  Scope: {s}  Time: {s}",
            .{ verb_display, scope_display, time_display },
        );

        // Free the old label only after the allocation succeeds (no partial state).
        if (self.filter_label_owned) {
            self.allocator.free(self.filter_label);
        }
        self.filter_label = new_label;
        self.filter_label_owned = true;
    }

    /// Reload all cli_invocations data from the DB. Called on db_changed and on
    /// initial launch.
    ///
    /// MEMORY SAFETY: filter.verb and filter.scope are independently owned
    /// strings (see setFilterVerb / setFilterScope), so the order in which we
    /// free verbs/scopes vs. read the filter does not matter — no UAF.
    pub fn reload(self: *CliHistoryState, d: *db.sqlite.Db) !void {
        // Free old rows.
        view_model.CliInvocationRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};

        // Reload distinct verbs (for verb-filter cycling).
        // Safe to free verbs before reading self.filter.verb because the filter
        // owns its verb string independently (not aliased from verbs[i]).
        self.freeVerbs();
        self.verbs = try view_model.queryCliVerbs(d, self.allocator);

        // Reload distinct scopes (for scope-filter cycling).
        self.freeScopes();
        self.scopes = try view_model.queryCliScopes(d, self.allocator);

        // Validate the active verb filter: if the verb is set but no longer
        // present in the freshly loaded verbs list, reset to null.
        // Reading self.filter.verb here is safe — it is an owned dup (not a
        // pointer into the now-freed old verbs list).
        if (self.filter.verb) |fv| {
            var still_valid = false;
            for (self.verbs) |v| {
                if (std.mem.eql(u8, v, fv)) {
                    still_valid = true;
                    break;
                }
            }
            if (!still_valid) self.resetVerbFilter();
        }

        // Validate the active scope filter similarly.
        if (self.filter.scope) |fs| {
            var still_valid = false;
            for (self.scopes) |s| {
                if (std.mem.eql(u8, s, fs)) {
                    still_valid = true;
                    break;
                }
            }
            if (!still_valid) self.resetScopeFilter();
        }

        // Query rows under the (possibly narrowed) filter.
        self.rows = try view_model.queryCliHistory(d, self.allocator, self.filter);

        // Update the pre-formatted filter label (MEMORY GUARD rule (c)).
        try self.updateFilterLabel();

        // Clamp selection.
        if (self.rows.len > 0) {
            if (self.selected_idx >= self.rows.len) {
                self.selected_idx = self.rows.len - 1;
            }
        } else {
            self.selected_idx = 0;
        }
    }

    /// Cycle the verb filter through the distinct verb list, then back to null.
    ///
    ///   null → verbs[0] → verbs[1] → … → verbs[N-1] → null → …
    ///
    /// OWNERSHIP: uses setFilterVerb (which dupes the verb string) so the
    /// filter never borrows from verbs[i] — verbs can be freed or reallocated
    /// without dangling the filter.
    pub fn cycleVerbFilter(self: *CliHistoryState, d: *db.sqlite.Db) !void {
        if (self.verbs.len == 0) {
            self.resetVerbFilter();
        } else if (self.filter.verb == null) {
            // Advance to the first verb.
            try self.setFilterVerb(self.verbs[0]);
        } else {
            // Find the current verb index and advance to the next.
            var idx: ?usize = null;
            for (self.verbs, 0..) |v, i| {
                if (std.mem.eql(u8, v, self.filter.verb.?)) {
                    idx = i;
                    break;
                }
            }
            if (idx) |i| {
                const next_i = i + 1;
                if (next_i >= self.verbs.len) {
                    // Wrap back to null (no verb filter).
                    self.resetVerbFilter();
                } else {
                    try self.setFilterVerb(self.verbs[next_i]);
                }
            } else {
                // Current verb no longer in list — reset.
                self.resetVerbFilter();
            }
        }

        // Re-query under the new filter.
        view_model.CliInvocationRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        self.rows = try view_model.queryCliHistory(d, self.allocator, self.filter);

        try self.updateFilterLabel();
        self.selected_idx = 0;
    }

    /// Cycle the scope filter through the distinct scope list, then back to null.
    ///
    ///   null → scopes[0] → scopes[1] → … → scopes[N-1] → null → …
    ///
    /// OWNERSHIP: uses setFilterScope (which dupes the scope string) so the
    /// filter never borrows from scopes[i].
    pub fn cycleScopeFilter(self: *CliHistoryState, d: *db.sqlite.Db) !void {
        if (self.scopes.len == 0) {
            self.resetScopeFilter();
        } else if (self.filter.scope == null) {
            try self.setFilterScope(self.scopes[0]);
        } else {
            var idx: ?usize = null;
            for (self.scopes, 0..) |s, i| {
                if (std.mem.eql(u8, s, self.filter.scope.?)) {
                    idx = i;
                    break;
                }
            }
            if (idx) |i| {
                const next_i = i + 1;
                if (next_i >= self.scopes.len) {
                    self.resetScopeFilter();
                } else {
                    try self.setFilterScope(self.scopes[next_i]);
                }
            } else {
                self.resetScopeFilter();
            }
        }

        view_model.CliInvocationRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        self.rows = try view_model.queryCliHistory(d, self.allocator, self.filter);

        try self.updateFilterLabel();
        self.selected_idx = 0;
    }

    /// Cycle the time-bucket filter: all → hour → day → all → …
    pub fn cycleTimeFilter(self: *CliHistoryState, d: *db.sqlite.Db) !void {
        self.filter.time = self.filter.time.next();

        view_model.CliInvocationRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        self.rows = try view_model.queryCliHistory(d, self.allocator, self.filter);

        try self.updateFilterLabel();
        self.selected_idx = 0;
    }

    /// Handle a key event. Returns true when the key was consumed.
    pub fn handleKey(self: *CliHistoryState, key: Key, d: *db.sqlite.Db) bool {
        const count = self.rows.len;

        // j / arrow-down: move selection down.
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            if (count > 0 and self.selected_idx + 1 < count) {
                self.selected_idx += 1;
            }
            return true;
        }
        // k / arrow-up: move selection up.
        if (key.matches('k', .{}) or key.matches(Key.up, .{})) {
            if (self.selected_idx > 0) {
                self.selected_idx -= 1;
            }
            return true;
        }
        // 'v': cycle verb filter (task 4038).
        if (key.matches('v', .{})) {
            self.cycleVerbFilter(d) catch {};
            return true;
        }
        // 's': cycle scope filter (task 4038).
        if (key.matches('s', .{})) {
            self.cycleScopeFilter(d) catch {};
            return true;
        }
        // 't': cycle time-bucket filter (task 4038).
        if (key.matches('t', .{})) {
            self.cycleTimeFilter(d) catch {};
            return true;
        }

        return false;
    }
};

// =========================================================================
// Render
// =========================================================================

/// Render the CLI History view into the navigator and detail windows.
///
/// Navigator (left pane): cli_invocations rows, newest-first.
/// Detail (right pane): all fields for the selected row.
pub fn render(
    state: *const CliHistoryState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    _ = allocator;
    renderNavigator(state, nav_win);
    renderDetail(state, detail_win);
}

/// Render the navigator pane (left).
///
/// Layout:
///   row 0: "CLI History  [<filter_label>]" (bold dim header)
///   row 1+: one row per cli_invocations entry, newest-first, selected reversed
///   Empty: "(no cli_invocations — enable [introspection].cli_log in config)"
///
/// INVARIANT (task 4037): every row's display_text (pre-formatted as
///   "[OK]  verb_path  args_shape  ts_short" or "[ERR] …")
/// is rendered via printSegment using the heap-allocated display_text so
/// grapheme pointers remain valid.
///
/// INVARIANT (task 4038): the active filter label is shown in the header.
fn renderNavigator(state: *const CliHistoryState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    // Header row: "CLI History  [<filter_label>]"
    // Three separate printSegment calls using only static literals and
    // heap-stable state.filter_label (MEMORY GUARD rule (c)).
    const prefix = "CLI History  [";
    const suffix = "]";
    _ = win.printSegment(.{
        .text = prefix,
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });
    _ = win.printSegment(.{
        .text = state.filter_label,
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = @intCast(prefix.len) });
    _ = win.printSegment(.{
        .text = suffix,
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = @intCast(prefix.len + state.filter_label.len) });

    if (win.height < 2) return;

    if (state.rows.len == 0) {
        const empty_msg = "(no cli_invocations — enable [introspection].cli_log in config)";
        _ = win.printSegment(.{
            .text = empty_msg,
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
    for (state.rows, 0..) |row, i| {
        if (i < scroll) continue;
        if (display_row >= win.height) break;

        const is_selected = (i == state.selected_idx);
        const style: Style = if (is_selected)
            .{ .bold = true, .reverse = true }
        else
            .{};

        // INVARIANT (task 4037): render display_text — the pre-formatted,
        // heap-allocated "[OK]  verb_path  args_shape  ts_short" string.
        _ = win.printSegment(.{
            .text = row.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });
        display_row += 1;
    }
}

/// Render the detail pane (right) for the selected cli_invocations row.
///
/// Layout (all fields from the queried row):
///   row 0:  "verb:      verb_path" (bold)
///   row 1:  "args:      args_shape"
///   row 2:  "outcome:   OK" or "outcome:   ERR (error_category)"
///   row 3:  "scope:     scope_slug  (or "(none)")"
///   row 4:  "duration:  NNms  (or "(none)")"
///   row 5:  "at:        recorded_at"
///
/// INVARIANT (task 4037): ALL rendered strings come from freshly
/// heap-allocated view_model fields. Every datum queried is rendered
/// (verb_path, args_shape, exit_code, error_category, scope_slug,
/// duration_ms, recorded_at).
fn renderDetail(state: *const CliHistoryState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    if (state.rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no row selected)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    if (state.selected_idx >= state.rows.len) return;
    const r = state.rows[state.selected_idx];

    var row: u16 = 0;

    // ---- verb_path (row 0) -----------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "verb:      ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.verb_path,
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 11 });
        row += 1;
    }

    // ---- args_shape (row 1) -----------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "args:      ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        if (r.args_shape.len > 0) {
            _ = win.printSegment(.{
                .text = r.args_shape,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 11 });
        } else {
            _ = win.printSegment(.{
                .text = "(none)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 11 });
        }
        row += 1;
    }

    // ---- outcome: exit_code + error_category (row 2) ---------------------
    // INVARIANT (task 4037): render exit_code and error_category so both
    // queried fields are visible in the detail pane.
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "outcome:   ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        if (r.exit_code == 0) {
            _ = win.printSegment(.{
                .text = "OK",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 11 });
        } else {
            _ = win.printSegment(.{
                .text = "ERR",
                .style = .{ .bold = true },
            }, .{ .row_offset = row, .col_offset = 11 });
            if (r.error_category) |ec| {
                _ = win.printSegment(.{
                    .text = " (",
                    .style = .{},
                }, .{ .row_offset = row, .col_offset = 14 });
                _ = win.printSegment(.{
                    .text = ec,
                    .style = .{},
                }, .{ .row_offset = row, .col_offset = 16 });
                _ = win.printSegment(.{
                    .text = ")",
                    .style = .{},
                }, .{ .row_offset = row, .col_offset = @intCast(16 + ec.len) });
            }
        }
        row += 1;
    }

    // ---- scope_slug (row 3) -----------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "scope:     ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        if (r.scope_slug) |sc| {
            _ = win.printSegment(.{
                .text = sc,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 11 });
        } else {
            _ = win.printSegment(.{
                .text = "(none)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 11 });
        }
        row += 1;
    }

    // ---- duration_ms (row 4) ---------------------------------------------
    // INVARIANT (task 4037): duration_ms is a queried field and must be rendered.
    // MEMORY GUARD (rule (c)): use r.duration_display (heap-allocated in
    // readCliInvocationRow) instead of a stack-local format buffer here.
    // printSegment stores a grapheme pointer into the text slice; a stack buffer
    // would dangle after renderDetail returns.
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "duration:  ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        if (r.duration_display) |dd| {
            _ = win.printSegment(.{
                .text = dd,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 11 });
        } else {
            _ = win.printSegment(.{
                .text = "(none)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 11 });
        }
        row += 1;
    }

    // ---- recorded_at (row 5) ---------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "at:        ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.recorded_at,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 11 });
    }
}

/// Return a one-line legend string for the key legend bar.
/// Note: this is the 11th view; there is no numeric key jump (Tab/Shift-Tab only).
pub fn legendLabel(buf: []u8) []const u8 {
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Select  v Verb-filter  s Scope-filter  t Time-filter  Tab/S-Tab View",
        .{},
    ) catch "  q Quit  j/k Select  v/s/t Filter  Tab/S-Tab View";
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
fn collectScreenText(screen: *const vaxis.Screen, out: *std.ArrayList(u8)) !void {
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) {
            try out.appendSlice(testing.allocator, g);
        }
    }
}

// -------------------------------------------------------------------------
// CliHistoryState lifecycle tests
// -------------------------------------------------------------------------

test "cli_history: init and deinit are clean" {
    var state = CliHistoryState.init(testing.allocator);
    defer state.deinit();
    try testing.expectEqual(@as(usize, 0), state.rows.len);
    try testing.expectEqual(@as(usize, 0), state.verbs.len);
    try testing.expectEqual(@as(usize, 0), state.scopes.len);
}

test "cli_history: reload on empty DB yields empty rows (task 4037 empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = CliHistoryState.init(a);
    defer state.deinit();

    try state.reload(&d);

    try testing.expectEqual(@as(usize, 0), state.rows.len);
    try testing.expectEqual(@as(usize, 0), state.verbs.len);
    try testing.expectEqual(@as(usize, 0), state.scopes.len);
}

test "cli_history: reload populates rows newest-first (task 4037)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('plan show', '', 0, '2025-06-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 2), state.rows.len);
    // Newest-first: plan show before task add.
    try testing.expectEqualStrings("plan show", state.rows[0].verb_path);
    try testing.expectEqualStrings("task add", state.rows[1].verb_path);
}

test "cli_history: cycleVerbFilter cycles through verbs and back to null (task 4038)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('plan show', '', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '', 0, '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Start with no verb filter.
    try testing.expect(state.filter.verb == null);
    try testing.expectEqual(@as(usize, 2), state.rows.len);

    // Cycle to first verb (alphabetically: "plan show").
    try state.cycleVerbFilter(&d);
    try testing.expectEqualStrings("plan show", state.filter.verb.?);
    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqualStrings("plan show", state.rows[0].verb_path);

    // Cycle to second verb ("task add").
    try state.cycleVerbFilter(&d);
    try testing.expectEqualStrings("task add", state.filter.verb.?);
    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqualStrings("task add", state.rows[0].verb_path);

    // Cycle wraps back to null (all verbs).
    try state.cycleVerbFilter(&d);
    try testing.expect(state.filter.verb == null);
    try testing.expectEqual(@as(usize, 2), state.rows.len);
}

test "cli_history: cycleScopeFilter cycles through scopes and back to null (task 4038)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task list', '', 0, 'alpha', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task list', '', 0, 'beta', '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expect(state.filter.scope == null);
    try testing.expectEqual(@as(usize, 2), state.rows.len);

    // Cycle to first scope ("alpha").
    try state.cycleScopeFilter(&d);
    try testing.expectEqualStrings("alpha", state.filter.scope.?);
    try testing.expectEqual(@as(usize, 1), state.rows.len);

    // Cycle to second scope ("beta").
    try state.cycleScopeFilter(&d);
    try testing.expectEqualStrings("beta", state.filter.scope.?);
    try testing.expectEqual(@as(usize, 1), state.rows.len);

    // Cycle wraps back to null.
    try state.cycleScopeFilter(&d);
    try testing.expect(state.filter.scope == null);
    try testing.expectEqual(@as(usize, 2), state.rows.len);
}

test "cli_history: cycleTimeFilter cycles through time buckets (task 4038)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task list', '', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expect(state.filter.time == .all);

    try state.cycleTimeFilter(&d);
    try testing.expect(state.filter.time == .hour);

    try state.cycleTimeFilter(&d);
    try testing.expect(state.filter.time == .day);

    try state.cycleTimeFilter(&d);
    try testing.expect(state.filter.time == .all);
}

test "cli_history: handleKey j/k moves selection" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('plan show', '', 0, '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 0), state.selected_idx);

    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    _ = state.handleKey(j_key, &d);
    try testing.expectEqual(@as(usize, 1), state.selected_idx);

    const k_key = Key{ .codepoint = 'k', .mods = .{} };
    _ = state.handleKey(k_key, &d);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "cli_history: handleKey v cycles verb filter (task 4038)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expect(state.filter.verb == null);

    const v_key = Key{ .codepoint = 'v', .mods = .{} };
    _ = state.handleKey(v_key, &d);
    try testing.expectEqualStrings("task add", state.filter.verb.?);

    _ = state.handleKey(v_key, &d);
    try testing.expect(state.filter.verb == null);
}

test "cli_history: handleKey s cycles scope filter (task 4038)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task add', '', 0, 'myrepo', '2025-01-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expect(state.filter.scope == null);

    const s_key = Key{ .codepoint = 's', .mods = .{} };
    _ = state.handleKey(s_key, &d);
    try testing.expectEqualStrings("myrepo", state.filter.scope.?);

    _ = state.handleKey(s_key, &d);
    try testing.expect(state.filter.scope == null);
}

test "cli_history: handleKey t cycles time filter (task 4038)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const t_key = Key{ .codepoint = 't', .mods = .{} };
    _ = state.handleKey(t_key, &d);
    try testing.expect(state.filter.time == .hour);

    _ = state.handleKey(t_key, &d);
    try testing.expect(state.filter.time == .day);

    _ = state.handleKey(t_key, &d);
    try testing.expect(state.filter.time == .all);
}

// =========================================================================
// RENDER-LEVEL TESTS (rule (b): render into vaxis.Screen and assert text)
// =========================================================================

test "cli_history: renderNavigator shows rows with verb/args/outcome (task 4037 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '<pos:1> --plan', 0, '2025-01-01T10:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at) values ('task show', '<pos:1>', 1, 'not_found', '2025-06-01T12:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const win_w: u16 = 120;
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

    // RENDER-LEVEL ASSERTIONS (task 4037):
    // Header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "CLI History") != null);
    // Filter label must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Verb: (all)") != null);
    // Verb paths must appear.
    try testing.expect(std.mem.indexOf(u8, text, "task add") != null);
    try testing.expect(std.mem.indexOf(u8, text, "task show") != null);
    // Args shape must appear.
    try testing.expect(std.mem.indexOf(u8, text, "--plan") != null);
    // Outcome tags: [OK] and [ERR].
    try testing.expect(std.mem.indexOf(u8, text, "[OK]") != null);
    try testing.expect(std.mem.indexOf(u8, text, "[ERR]") != null);
    // Timestamp year must appear.
    try testing.expect(std.mem.indexOf(u8, text, "2025") != null);
}

test "cli_history: renderNavigator shows empty state when no rows (task 4037 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const win_w: u16 = 120;
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

    try testing.expect(std.mem.indexOf(u8, text, "no cli_invocations") != null);
}

test "cli_history: renderNavigator shows filter label when verb filter active (task 4038 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('plan show', '', 0, '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Cycle verb filter to "plan show" (alphabetically first).
    try state.cycleVerbFilter(&d);
    try testing.expectEqualStrings("plan show", state.filter.verb.?);

    const win_w: u16 = 120;
    const win_h: u16 = 20;
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

    // RENDER-LEVEL ASSERTIONS (task 4038):
    // Active verb filter must appear in header.
    try testing.expect(std.mem.indexOf(u8, text, "plan show") != null);
    // "task add" must NOT appear (filtered out).
    try testing.expect(std.mem.indexOf(u8, text, "task add") == null);
}

test "cli_history: renderNavigator scope filter hides filtered-out rows (task 4038 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task list', '', 0, 'repo-keep', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task list', '', 0, 'repo-drop', '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Cycle scope filter to "repo-drop" (alphabetically "repo-drop" < "repo-keep").
    try state.cycleScopeFilter(&d);
    try testing.expectEqualStrings("repo-drop", state.filter.scope.?);

    const win_w: u16 = 120;
    const win_h: u16 = 20;
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

    // "repo-drop" (filtered-in) must appear in the filter label.
    try testing.expect(std.mem.indexOf(u8, text, "repo-drop") != null);
    // "repo-keep" (filtered-out) must NOT appear as a row.
    // We assert "repo-keep" is absent from the rendered row area.
    // (It appears only if the filter label showed it, which it doesn't here.)
    try testing.expect(std.mem.indexOf(u8, text, "repo-keep") == null);
}

test "cli_history: renderDetail shows all fields for selected row (task 4037 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, scope_slug, duration_ms, recorded_at) values ('workbench push', '--dry-run', 1, 'io', 'myproject', 123, '2026-05-20T14:30:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
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

    renderDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4037): ALL queried fields must appear.
    // verb_path.
    try testing.expect(std.mem.indexOf(u8, text, "workbench push") != null);
    // args_shape.
    try testing.expect(std.mem.indexOf(u8, text, "--dry-run") != null);
    // outcome: ERR (exit_code = 1).
    try testing.expect(std.mem.indexOf(u8, text, "ERR") != null);
    // error_category.
    try testing.expect(std.mem.indexOf(u8, text, "io") != null);
    // scope_slug.
    try testing.expect(std.mem.indexOf(u8, text, "myproject") != null);
    // duration_ms.
    try testing.expect(std.mem.indexOf(u8, text, "123ms") != null);
    // recorded_at.
    try testing.expect(std.mem.indexOf(u8, text, "2026") != null);
}

test "cli_history: renderDetail shows (none) placeholders for null optionals (task 4037 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('init', '', 0, '2026-03-10T09:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
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

    renderDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // verb_path must appear.
    try testing.expect(std.mem.indexOf(u8, text, "init") != null);
    // outcome: OK (exit_code = 0).
    try testing.expect(std.mem.indexOf(u8, text, "OK") != null);
    // "(none)" for null scope, duration, and empty args_shape.
    try testing.expect(std.mem.indexOf(u8, text, "(none)") != null);
}

test "cli_history: time filter label appears in rendered navigator header (task 4038 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task list', '', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Advance to "last hour" bucket.
    try state.cycleTimeFilter(&d);
    try testing.expect(state.filter.time == .hour);

    const win_w: u16 = 160;
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

    // The time filter label "last hour" must appear in the header.
    try testing.expect(std.mem.indexOf(u8, text, "last hour") != null);
}

// =========================================================================
// UAF regression test: reload → set filter → reload (hard rule (c))
// =========================================================================

test "cli_history: reload->cycleVerbFilter->reload does not UAF or leak (verb filter regression)" {
    // This test exercises the path that would trigger a use-after-free if
    // the verb filter held a borrowed pointer into verbs[]:
    //
    //   1. reload()          — verbs[] allocated; filter.verb = null.
    //   2. cycleVerbFilter() — filter.verb set; previously it would be a
    //                          pointer into verbs[i] (NOT duped) → UAF on step 3.
    //   3. reload()          — verbs[] freed and reallocated; previously
    //                          reading filter.verb would access freed memory.
    //
    // With the fix, cycleVerbFilter uses setFilterVerb which dupes the string
    // independently. reload() can safely compare and potentially reset the
    // filter without any alias into the freed verbs list.
    //
    // Under testing.allocator: detects double-frees and leaks.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '--plan', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('plan show', '', 0, '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();

    // Step 1: initial reload.
    try state.reload(&d);
    try testing.expectEqual(@as(usize, 2), state.rows.len);
    try testing.expectEqual(@as(usize, 2), state.verbs.len);
    try testing.expect(state.filter.verb == null);

    // Step 2: cycle verb filter to the first verb ("plan show" sorts before "task add").
    try state.cycleVerbFilter(&d);
    try testing.expectEqualStrings("plan show", state.filter.verb.?);
    // The filter now owns its own copy of "plan show" — not a pointer into verbs[].

    // Step 3: reload again. This frees and reallocates verbs[].
    // Previously, reading self.filter.verb here would access freed memory.
    // With the fix, the verb string is independently owned and remains valid.
    try state.reload(&d);

    // "plan show" still exists in the DB → filter preserved.
    try testing.expectEqualStrings("plan show", state.filter.verb.?);
    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqualStrings("plan show", state.rows[0].verb_path);

    // Step 4: delete the "plan show" rows and reload.
    // The verb is no longer in the DB → filter resets to null.
    _ = try d.execParams("delete from cli_invocations where verb_path = 'plan show'", &.{});
    try state.reload(&d);
    try testing.expect(state.filter.verb == null);
    try testing.expectEqual(@as(usize, 1), state.rows.len);
}

test "cli_history: reload->cycleScopeFilter->reload does not UAF or leak (scope filter regression)" {
    // Same UAF regression as above, but for the scope filter.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task add', '', 0, 'repo-a', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task add', '', 0, 'repo-b', '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = CliHistoryState.init(a);
    defer state.deinit();

    // Step 1.
    try state.reload(&d);
    try testing.expectEqual(@as(usize, 2), state.scopes.len);
    try testing.expect(state.filter.scope == null);

    // Step 2: cycle scope to "repo-a" (sorts first).
    try state.cycleScopeFilter(&d);
    try testing.expectEqualStrings("repo-a", state.filter.scope.?);
    // Independently owned; scopes[] can be freed safely.

    // Step 3: reload frees and reallocates scopes[].
    try state.reload(&d);
    try testing.expectEqualStrings("repo-a", state.filter.scope.?);
    try testing.expectEqual(@as(usize, 1), state.rows.len);

    // Step 4: remove scope "repo-a" rows → filter resets.
    _ = try d.execParams("delete from cli_invocations where scope_slug = 'repo-a'", &.{});
    try state.reload(&d);
    try testing.expect(state.filter.scope == null);
    try testing.expectEqual(@as(usize, 1), state.rows.len);
}

test "cli_history: legendLabel fits in buf and contains expected keys" {
    var buf: [256]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Quit") != null);
    try testing.expect(std.mem.indexOf(u8, label, "filter") != null or
        std.mem.indexOf(u8, label, "Filter") != null);
}

test "cli_history compiles" {
    std.testing.refAllDecls(@This());
}
