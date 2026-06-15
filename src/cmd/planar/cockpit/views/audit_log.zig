//! cockpit/views/audit_log.zig — Audit Log view (M12).
//!
//! Renders a two-pane layout for the Audit Log:
//!
//!   • Left pane (navigator): chronological list of audit_log rows,
//!     newest-first. Each row shows:
//!       "[verb] entity_kind:entity_id  actor  timestamp"
//!     j/k (or arrow keys) navigate. 'f' cycles the entity filter.
//!
//!   • Right pane (detail): for the selected row, shows all queried fields:
//!       - verb
//!       - entity reference (entity_kind:entity_id)
//!       - actor (or "—" when null)
//!       - scope (or "(none)" when null)
//!       - summary (or "(none)" when null)
//!       - recorded_at (full timestamp)
//!
//!   • Status bar: shows the active entity filter ("Filter: all entities"
//!     or "Filter: task:42") so the operator always sees which entity is
//!     being viewed.
//!
//! Tasks 4035 (chronological audit_log list with actor+action) and
//! 4036 (filter by entity).
//!
//! Acceptance invariants:
//!   (4035) ALL queried audit_log row fields are rendered: verb,
//!          entity_kind, entity_id, actor, recorded_at, summary.
//!   (4036) Entity filter narrows the rendered set; the active filter is
//!          visible in the navigator header; 'f' cycles to the next entity
//!          in the distinct entity list (entity:all → entity:0 → entity:1
//!          → … → entity:N → all). Rows outside the active filter are
//!          NOT present in the rendered output.
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by AuditLogState and released via deinit.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread.
//!   - MEMORY GUARD (brief rule (c)): all strings are freshly allocated in
//!     view_model; no alias into kind/id pointer fields that deinit frees.
//!   - 10th view: registered with key '0' (display-only; numeric jump logic
//!     in view_switcher only fires for '1'–'9'). Tab/Shift-Tab cycling
//!     reaches this view.
//!
//! Schema confirmed from migrations/00014_audit_log.up.sql:
//!   audit_log(id, verb, entity_kind, entity_id, actor, scope, summary,
//!             recorded_at)

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// AuditLogState
// =========================================================================

/// All mutable state for the Audit Log view.
pub const AuditLogState = struct {
    allocator: std.mem.Allocator,

    /// Current snapshot of audit_log rows, ordered newest-first.
    rows: []view_model.AuditLogRow = &.{},

    /// Distinct entity refs present in the audit_log (for filter cycling).
    entities: []view_model.AuditEntityRef = &.{},

    /// Active entity filter. .all shows all rows; .entity narrows to one
    /// entity. Default: .all.
    ///
    /// OWNERSHIP: when the tag is .entity, the kind slice is an independently
    /// heap-allocated duplicate (see setFilterEntity / resetFilterToAll).  It
    /// must NOT alias into entities[i].kind — the entities list is freed and
    /// reallocated on every reload(), and the filter must survive that.
    filter: view_model.AuditEntityFilter = .all,

    /// Whether the filter's .entity.kind string is owned by this state and
    /// must be freed on replacement or deinit.
    filter_kind_owned: bool = false,

    /// Pre-formatted filter label string ("all entities" or "task:55").
    /// Heap-allocated so the navigator renderer can use it safely without
    /// stack-local format buffers whose grapheme pointers would dangle after
    /// the render function returns (MEMORY GUARD rule (c)).
    filter_label: []const u8 = "all entities",

    /// Whether filter_label was heap-allocated (i.e. not the static literal).
    filter_label_owned: bool = false,

    /// Navigator selection index (0-based into rows).
    selected_idx: usize = 0,

    /// Navigator scroll offset.
    scroll_offset: usize = 0,

    pub fn init(allocator: std.mem.Allocator) AuditLogState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *AuditLogState) void {
        view_model.AuditLogRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        view_model.AuditEntityRef.deinitMany(self.entities, self.allocator);
        self.entities = &.{};
        // The filter owns its kind string independently of entities, so we
        // can free it here without any ordering dependency on entities.
        self.resetFilterToAll();
        if (self.filter_label_owned) {
            self.allocator.free(self.filter_label);
        }
        self.filter_label = "all entities";
        self.filter_label_owned = false;
    }

    /// Set the active filter to .entity with an independently owned copy of
    /// `kind`. Frees the previously-owned kind string if one was set.
    /// On allocation failure the filter remains unchanged and the error is
    /// returned — no partial state is written.
    fn setFilterEntity(self: *AuditLogState, kind: []const u8, id: i64) !void {
        const owned_kind = try self.allocator.dupe(u8, kind);
        // Only release the old kind after the allocation succeeds (no partial state).
        if (self.filter_kind_owned) {
            // We are replacing an existing owned kind — free the old one first.
            self.allocator.free(self.filter.entity.kind);
        }
        self.filter = .{ .entity = .{ .kind = owned_kind, .id = id } };
        self.filter_kind_owned = true;
    }

    /// Reset the active filter to .all, freeing the owned kind string if one
    /// was set.
    fn resetFilterToAll(self: *AuditLogState) void {
        if (self.filter_kind_owned) {
            self.allocator.free(self.filter.entity.kind);
            self.filter_kind_owned = false;
        }
        self.filter = .all;
    }

    /// Update the filter_label field to match the current filter.
    /// MEMORY GUARD: allocates a fresh heap string for .entity; uses a
    /// static literal for .all. Old heap string is freed before replacement.
    fn updateFilterLabel(self: *AuditLogState) !void {
        if (self.filter_label_owned) {
            self.allocator.free(self.filter_label);
            self.filter_label_owned = false;
            self.filter_label = "all entities";
        }
        switch (self.filter) {
            .all => {
                self.filter_label = "all entities";
                self.filter_label_owned = false;
            },
            .entity => |e| {
                self.filter_label = try std.fmt.allocPrint(
                    self.allocator,
                    "{s}:{d}",
                    .{ e.kind, e.id },
                );
                self.filter_label_owned = true;
            },
        }
    }

    /// Reload all audit_log data from the DB. Called on db_changed and on
    /// initial launch.
    ///
    /// MEMORY SAFETY: the filter now owns its kind string independently of the
    /// entities list (see setFilterEntity / resetFilterToAll), so the order in
    /// which we free entities vs. read the filter does not matter — no UAF.
    pub fn reload(self: *AuditLogState, d: *db.sqlite.Db) !void {
        // Free old rows.
        view_model.AuditLogRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};

        // Reload distinct entity refs (for filter cycling).
        // Safe to free entities before reading self.filter because the filter
        // owns its kind string independently (not aliased from entities[i].kind).
        view_model.AuditEntityRef.deinitMany(self.entities, self.allocator);
        self.entities = &.{};
        self.entities = try view_model.queryAuditEntities(d, self.allocator);

        // Rebuild the filter: if the active filter was .entity, validate
        // that the entity still exists in the freshly loaded list; if not,
        // reset to .all.  Reading self.filter.entity.kind here is safe because
        // the kind string is owned by the filter (duped in setFilterEntity),
        // not borrowed from the now-freed old entities list.
        switch (self.filter) {
            .all => {},
            .entity => |e| {
                var still_valid = false;
                for (self.entities) |ref| {
                    if (ref.id == e.id and std.mem.eql(u8, ref.kind, e.kind)) {
                        still_valid = true;
                        break;
                    }
                }
                if (!still_valid) self.resetFilterToAll();
            },
        }

        // Query rows under the (possibly reset) filter.
        self.rows = try view_model.queryAuditLog(d, self.allocator, self.filter);

        // Update the pre-formatted filter label so renderNavigator can use it
        // without stack-local format buffers (MEMORY GUARD rule (c)).
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

    /// Cycle the entity filter to the next entity in the distinct entity list.
    ///
    ///   all → entities[0] → entities[1] → … → entities[N-1] → all → …
    ///
    /// After cycling, rows are re-queried from the DB.
    ///
    /// OWNERSHIP: uses setFilterEntity (which dupes the kind string) so the
    /// filter never borrows from entities[i].kind — entities can be freed or
    /// reallocated without dangling the filter.
    pub fn cycleFilter(self: *AuditLogState, d: *db.sqlite.Db) !void {
        if (self.entities.len == 0) {
            // No entities to cycle to; stay at .all.
            self.resetFilterToAll();
        } else {
            switch (self.filter) {
                .all => {
                    // Advance to the first entity (dupe kind — no alias).
                    const ref = self.entities[0];
                    try self.setFilterEntity(ref.kind, ref.id);
                },
                .entity => |e| {
                    // Find the current entity index and advance to the next.
                    var idx: ?usize = null;
                    for (self.entities, 0..) |ref, i| {
                        if (ref.id == e.id and std.mem.eql(u8, ref.kind, e.kind)) {
                            idx = i;
                            break;
                        }
                    }
                    if (idx) |i| {
                        const next_i = i + 1;
                        if (next_i >= self.entities.len) {
                            // Wrap back to .all (frees the owned kind).
                            self.resetFilterToAll();
                        } else {
                            const next_ref = self.entities[next_i];
                            // setFilterEntity frees the old owned kind, then dupes the new one.
                            try self.setFilterEntity(next_ref.kind, next_ref.id);
                        }
                    } else {
                        // Current entity no longer in list — reset.
                        self.resetFilterToAll();
                    }
                },
            }
        }

        // Re-query under the new filter.
        view_model.AuditLogRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
        self.rows = try view_model.queryAuditLog(d, self.allocator, self.filter);

        // Update the pre-formatted filter label (MEMORY GUARD rule (c)).
        try self.updateFilterLabel();

        // Reset selection on filter change.
        self.selected_idx = 0;
    }

    /// Handle a key event. Returns true when the key was consumed.
    pub fn handleKey(self: *AuditLogState, key: Key, d: *db.sqlite.Db) bool {
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
        // 'f': cycle entity filter (task 4036).
        if (key.matches('f', .{})) {
            self.cycleFilter(d) catch {};
            return true;
        }

        return false;
    }
};

// =========================================================================
// Render
// =========================================================================

/// Render the Audit Log into the navigator and detail windows.
///
/// Navigator (left pane): chronological list of rows, newest-first.
/// Detail (right pane): all fields for the selected row.
pub fn render(
    state: *const AuditLogState,
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
///   row 0: "Audit Log  [Filter: <filter_label>]" (bold dim header)
///   row 1+: one row per audit_log entry, newest-first, selected reversed
///   Empty: "(no audit_log entries)" or "(no entries for <filter>)"
///
/// INVARIANT (task 4035): every row's display_text (pre-formatted as
///   "[verb] entity_kind:entity_id  actor  ts_short")
/// is rendered via printSegment using the heap-allocated display_text so
/// grapheme pointers remain valid.
///
/// INVARIANT (task 4036): the active filter label is shown in the header.
fn renderNavigator(state: *const AuditLogState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    // Header row: "Audit Log  [Filter: <label>]"
    // Three separate printSegment calls using only static literals and heap-stable
    // state.filter_label so no grapheme pointer dangles after this function returns
    // (MEMORY GUARD rule (c): no stack-local format buffers here).
    _ = win.printSegment(.{
        .text = "Audit Log  [Filter: ",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });
    _ = win.printSegment(.{
        .text = state.filter_label,
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 20 });
    _ = win.printSegment(.{
        .text = "]",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = @intCast(20 + state.filter_label.len) });
    if (win.height < 2) return;

    if (state.rows.len == 0) {
        const empty_msg: []const u8 = switch (state.filter) {
            .all => "(no audit_log entries)",
            .entity => "(no entries for this entity)",
        };
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

        // INVARIANT (task 4035): render display_text — the pre-formatted,
        // heap-allocated "[verb] entity_kind:entity_id  actor  ts_short".
        // Using the heap-allocated slice means grapheme pointers are stable.
        _ = win.printSegment(.{
            .text = row.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });
        display_row += 1;
    }
}

/// Render the detail pane (right) for the selected audit_log row.
///
/// Layout (all fields from the queried row):
///   row 0:  "[verb]" (bold) — the mutation verb
///   row 1:  "entity:  entity_kind:entity_id"
///   row 2:  "actor:   actor  (or "—")"
///   row 3:  "scope:   scope  (or "(none)")"
///   row 4:  "at:      recorded_at"
///   separator
///   "Summary:" section (bold dim header)
///   row N:  "  summary text"  (or "  (none)")
///
/// INVARIANT (task 4035): ALL rendered strings come from freshly
/// heap-allocated view_model fields. Every datum queried is rendered
/// (verb, entity_kind, entity_id, actor, scope, recorded_at, summary).
fn renderDetail(state: *const AuditLogState, win: Window) void {
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

    // ---- verb (row 0) -------------------------------------------------------
    // Wrap in brackets for visual clarity: "[verb]"
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "[",
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.verb,
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 1 });
        const verb_end: u16 = @intCast(1 + r.verb.len);
        _ = win.printSegment(.{
            .text = "]",
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = verb_end });
        row += 1;
    }

    // ---- entity_kind:entity_id (row 1) ------------------------------------
    // MEMORY GUARD (rule (c)): use r.entity_ref (heap-allocated "kind:id" string)
    // instead of stack-local format buffers. Grapheme pointers into r.entity_ref
    // remain valid for the lifetime of the state.
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "entity:  ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.entity_ref,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 9 });
        row += 1;
    }

    // ---- actor (row 2) -----------------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "actor:   ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        if (r.actor) |ac| {
            _ = win.printSegment(.{
                .text = ac,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 9 });
        } else {
            _ = win.printSegment(.{
                .text = "\u{2014}", // em-dash "—"
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 9 });
        }
        row += 1;
    }

    // ---- scope (row 3) -----------------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "scope:   ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        if (r.scope) |sc| {
            _ = win.printSegment(.{
                .text = sc,
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

    // ---- recorded_at (row 4) -----------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "at:      ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.recorded_at,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 9 });
        row += 1;
    }

    // ---- Separator -------------------------------------------------------
    if (row < win.height) row += 1;

    // ---- Summary section (task 4035 — must always render the summary field)
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "Summary:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    if (row < win.height) {
        if (r.summary) |s| {
            _ = win.printSegment(.{
                .text = s,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 2 });
        } else {
            _ = win.printSegment(.{
                .text = "(none)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 2 });
        }
    }
}

/// Return a one-line legend string for the key legend bar.
/// Note: this is the 10th view; there is no numeric key jump (only Tab/Shift-Tab).
pub fn legendLabel(buf: []u8) []const u8 {
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Select  f Filter-entity  Tab Focus  Tab/S-Tab View",
        .{},
    ) catch "  q Quit  j/k Select  f Filter  Tab Focus";
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
/// grapheme pointers remain valid for the duration of the test when the
/// strings are heap-allocated (display_text, actor, recorded_at, etc.).
fn collectScreenText(screen: *const vaxis.Screen, out: *std.ArrayList(u8)) !void {
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) {
            try out.appendSlice(testing.allocator, g);
        }
    }
}

// -------------------------------------------------------------------------
// AuditLogState lifecycle tests
// -------------------------------------------------------------------------

test "audit_log: init and deinit are clean" {
    var state = AuditLogState.init(testing.allocator);
    defer state.deinit();
    try testing.expectEqual(@as(usize, 0), state.rows.len);
    try testing.expectEqual(@as(usize, 0), state.entities.len);
}

test "audit_log: reload on empty DB yields empty rows (task 4035 empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = AuditLogState.init(a);
    defer state.deinit();

    try state.reload(&d);

    try testing.expectEqual(@as(usize, 0), state.rows.len);
    try testing.expectEqual(@as(usize, 0), state.entities.len);
}

test "audit_log: reload populates rows newest-first (task 4035)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 1, 'alice', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('update', 'task', 1, 'alice', '2025-06-01T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 2), state.rows.len);
    // Newest-first: update before create.
    try testing.expectEqualStrings("update", state.rows[0].verb);
    try testing.expectEqualStrings("create", state.rows[1].verb);
}

test "audit_log: reload populates entities list (task 4036)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 1, 'alice', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'plan', 5, 'bob', '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Two distinct entity refs (plan:5, task:1 sorted by kind asc).
    try testing.expectEqual(@as(usize, 2), state.entities.len);
    try testing.expectEqualStrings("plan", state.entities[0].kind);
    try testing.expectEqualStrings("task", state.entities[1].kind);
}

test "audit_log: cycleFilter cycles through entities and back to all (task 4036)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 10, 'alice', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'plan', 5, 'bob', '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Start at .all.
    try testing.expect(state.filter == .all);

    // Cycle → first entity (plan:5, sorted first by kind).
    try state.cycleFilter(&d);
    try testing.expect(state.filter == .entity);
    try testing.expectEqualStrings("plan", state.filter.entity.kind);
    try testing.expectEqual(@as(i64, 5), state.filter.entity.id);
    // Rows should now only contain plan:5 entries.
    try testing.expectEqual(@as(usize, 1), state.rows.len);

    // Cycle → second entity (task:10).
    try state.cycleFilter(&d);
    try testing.expect(state.filter == .entity);
    try testing.expectEqualStrings("task", state.filter.entity.kind);
    try testing.expectEqual(@as(i64, 10), state.filter.entity.id);
    try testing.expectEqual(@as(usize, 1), state.rows.len);

    // Cycle → wraps back to .all.
    try state.cycleFilter(&d);
    try testing.expect(state.filter == .all);
    // All rows (both entities) returned.
    try testing.expectEqual(@as(usize, 2), state.rows.len);
}

test "audit_log: entity filter shows only matching rows (task 4036)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 10, 'alice', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('update', 'task', 10, 'alice', '2025-01-02T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'plan', 5, 'bob', '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Cycle once to plan:5.
    try state.cycleFilter(&d);
    try testing.expect(state.filter == .entity);
    try testing.expectEqualStrings("plan", state.filter.entity.kind);
    // Only 1 plan:5 row.
    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqualStrings("plan", state.rows[0].entity_kind);
    try testing.expectEqual(@as(i64, 5), state.rows[0].entity_id);
}

test "audit_log: handleKey j/k moves selection" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, recorded_at) values ('create', 'task', 1, '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, recorded_at) values ('update', 'task', 1, '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
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

test "audit_log: handleKey j does not overflow past last row" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, recorded_at) values ('create', 'task', 1, '2025-01-01T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    _ = state.handleKey(j_key, &d);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "audit_log: handleKey k does not underflow below 0" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, recorded_at) values ('create', 'task', 1, '2025-01-01T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const k_key = Key{ .codepoint = 'k', .mods = .{} };
    _ = state.handleKey(k_key, &d);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "audit_log: handleKey f cycles filter (task 4036)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, recorded_at) values ('create', 'task', 1, '2025-01-01T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expect(state.filter == .all);

    const f_key = Key{ .codepoint = 'f', .mods = .{} };
    _ = state.handleKey(f_key, &d);
    // Should have cycled to task:1.
    try testing.expect(state.filter == .entity);

    _ = state.handleKey(f_key, &d);
    // Should have wrapped back to .all.
    try testing.expect(state.filter == .all);
}

// =========================================================================
// RENDER-LEVEL TESTS (rule (b): render into vaxis.Screen and assert text)
// =========================================================================
//
// These tests allocate a real vaxis.Screen, call the render functions,
// then scan the cell buffer for expected text. A struct-level assertion
// is NOT sufficient; these tests prove the rendered output contains data.

test "audit_log: renderNavigator shows rows with verb + entity + actor (task 4035 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 7, 'claude', '2025-01-01T10:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('status_change', 'task', 7, 'alice', '2025-06-01T12:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
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

    renderNavigator(&state, nav_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4035):
    // Header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Audit Log") != null);
    // Filter label must appear.
    try testing.expect(std.mem.indexOf(u8, text, "all entities") != null);
    // Verb must appear (newest-first row first: status_change).
    try testing.expect(std.mem.indexOf(u8, text, "status_change") != null);
    try testing.expect(std.mem.indexOf(u8, text, "create") != null);
    // entity_kind and entity_id must appear.
    try testing.expect(std.mem.indexOf(u8, text, "task") != null);
    try testing.expect(std.mem.indexOf(u8, text, "7") != null);
    // Actors must appear.
    try testing.expect(std.mem.indexOf(u8, text, "claude") != null);
    try testing.expect(std.mem.indexOf(u8, text, "alice") != null);
    // Timestamps must appear (years).
    try testing.expect(std.mem.indexOf(u8, text, "2025") != null);
}

test "audit_log: renderNavigator shows empty state when no rows (task 4035 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = AuditLogState.init(a);
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

    try testing.expect(std.mem.indexOf(u8, text, "no audit_log entries") != null);
}

test "audit_log: renderNavigator shows filter label when entity filter active (task 4036 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 42, 'bob', '2025-03-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'plan', 99, 'alice', '2025-04-01T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Cycle to plan:99 (plan sorts before task).
    try state.cycleFilter(&d);
    try testing.expect(state.filter == .entity);

    const win_w: u16 = 80;
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

    // RENDER-LEVEL ASSERTIONS (task 4036):
    // Filter label must show the active entity.
    try testing.expect(std.mem.indexOf(u8, text, "plan") != null);
    try testing.expect(std.mem.indexOf(u8, text, "99") != null);
    // Row for plan:99 must appear (alice).
    try testing.expect(std.mem.indexOf(u8, text, "alice") != null);
    // Row for task:42 must NOT appear (filtered out).
    try testing.expect(std.mem.indexOf(u8, text, "bob") == null);
}

test "audit_log: renderDetail shows all fields for selected row (task 4035 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, scope, summary, recorded_at) values ('status_change', 'task', 55, 'claude', 'project/foo', 'moved to doing', '2026-05-20T14:30:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();
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

    renderDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (task 4035): ALL queried fields must appear.
    // verb.
    try testing.expect(std.mem.indexOf(u8, text, "status_change") != null);
    // entity_kind:entity_id.
    try testing.expect(std.mem.indexOf(u8, text, "task") != null);
    try testing.expect(std.mem.indexOf(u8, text, "55") != null);
    // actor.
    try testing.expect(std.mem.indexOf(u8, text, "claude") != null);
    // scope (rendered via printSegment using heap-allocated slice).
    try testing.expect(std.mem.indexOf(u8, text, "project/foo") != null);
    // recorded_at.
    try testing.expect(std.mem.indexOf(u8, text, "2026") != null);
    // Summary header.
    try testing.expect(std.mem.indexOf(u8, text, "Summary:") != null);
    // summary text.
    try testing.expect(std.mem.indexOf(u8, text, "moved to doing") != null);
}

test "audit_log: renderDetail shows (none) placeholders when actor/scope/summary null (task 4035 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert with null actor, scope, summary.
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, recorded_at) values ('delete', 'artifact', 11, '2026-03-10T09:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
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

    // verb and entity fields must appear.
    try testing.expect(std.mem.indexOf(u8, text, "delete") != null);
    try testing.expect(std.mem.indexOf(u8, text, "artifact") != null);
    try testing.expect(std.mem.indexOf(u8, text, "11") != null);
    // em-dash for null actor.
    try testing.expect(std.mem.indexOf(u8, text, "\u{2014}") != null);
    // "(none)" for null scope.
    try testing.expect(std.mem.indexOf(u8, text, "(none)") != null);
    // Summary section must appear with "(none)".
    try testing.expect(std.mem.indexOf(u8, text, "Summary:") != null);
}

test "audit_log: renderNavigator entity-filtered: filtered-out rows absent from rendered output (task 4036 render-level)" {
    // This is the critical render-level test for task 4036:
    // Verifies that after filtering to task:10, rows for plan:5 are NOT
    // present in the rendered navigator output.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 10, 'alice', '2026-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'plan', 5, 'bob', '2026-01-02T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // entities[0] = plan:5 (sorts first), entities[1] = task:10.
    // Cycle twice to reach task:10.
    try state.cycleFilter(&d); // → plan:5
    try state.cycleFilter(&d); // → task:10
    try testing.expect(state.filter == .entity);
    try testing.expectEqualStrings("task", state.filter.entity.kind);
    try testing.expectEqual(@as(i64, 10), state.filter.entity.id);

    const win_w: u16 = 80;
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

    // task:10/alice must appear (filtered-in).
    try testing.expect(std.mem.indexOf(u8, text, "alice") != null);
    try testing.expect(std.mem.indexOf(u8, text, "10") != null);
    // plan:5/bob must NOT appear (filtered-out).
    // "bob" uniquely identifies plan:5 in this fixture.
    try testing.expect(std.mem.indexOf(u8, text, "bob") == null);
}

test "audit_log: legendLabel fits in buf and contains expected keys" {
    var buf: [256]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Quit") != null);
    try testing.expect(std.mem.indexOf(u8, label, "Filter") != null);
}

// -------------------------------------------------------------------------
// UAF regression test (task 4036 memory safety)
// -------------------------------------------------------------------------

test "audit_log: reload->cycleFilter->reload does not UAF or leak (regression for entity-filter-owned-kind)" {
    // This test exercises the path that previously triggered a use-after-free:
    //
    //   1. reload()       — entities list allocated; filter still .all.
    //   2. cycleFilter()  — filter set to .entity; previously the kind field
    //                       was borrowed from entities[i].kind (NOT duped).
    //   3. reload()       — entities list freed and reallocated; previously this
    //                       read self.filter.entity.kind which still pointed at
    //                       the freed entities[i].kind → UAF.
    //
    // With the fix, cycleFilter dupes the kind into an independently owned
    // string via setFilterEntity, so reload safely compares and potentially
    // resets the filter without any alias into the freed entities list.
    //
    // Run under testing.allocator: the allocator detects both double-frees and
    // leaks, so the test fails if the fix introduces either defect.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 77, 'alice', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'plan', 3, 'bob', '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    var state = AuditLogState.init(a);
    defer state.deinit();

    // Step 1: initial reload.
    try state.reload(&d);
    try testing.expectEqual(@as(usize, 2), state.rows.len);
    try testing.expectEqual(@as(usize, 2), state.entities.len);
    try testing.expect(state.filter == .all);

    // Step 2: cycle filter to the first entity (plan:3, sorts before task:77).
    try state.cycleFilter(&d);
    try testing.expect(state.filter == .entity);
    try testing.expectEqualStrings("plan", state.filter.entity.kind);
    // The filter now owns its own copy of "plan" — not a pointer into entities[].

    // Step 3: reload again. This frees and reallocates the entities list.
    // Previously, reading self.filter.entity.kind here would access freed memory.
    // With the fix, the kind string is independently owned and remains valid.
    try state.reload(&d);

    // The entity still exists so the filter should be preserved.
    try testing.expect(state.filter == .entity);
    try testing.expectEqualStrings("plan", state.filter.entity.kind);
    try testing.expectEqual(@as(i64, 3), state.filter.entity.id);
    // Only plan:3 rows returned (1 row).
    try testing.expectEqual(@as(usize, 1), state.rows.len);

    // Step 4: reset to .all via another reload after the entity is removed
    // from the DB — validates the "entity no longer exists → reset to .all" path
    // also frees the owned kind without a double-free.
    _ = try d.execParams("delete from audit_log where entity_kind = 'plan'", &.{});
    try state.reload(&d);
    try testing.expect(state.filter == .all);
    try testing.expectEqual(@as(usize, 1), state.rows.len);
}

test "audit_log compiles" {
    std.testing.refAllDecls(@This());
}
