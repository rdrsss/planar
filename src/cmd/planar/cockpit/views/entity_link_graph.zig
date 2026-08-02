//! cockpit/views/entity_link_graph.zig — Entity-Link Graph view (M9).
//!
//! Renders a two-pane layout for the Entity-Link Graph:
//!
//!   • Left pane (navigator): list of all entity_links edges for the
//!     currently focused entity, in BOTH directions (outbound + inbound),
//!     across ALL relationship kinds (derives-from, depends-on, addresses,
//!     verifies, cites, supersedes, touches). Each row is formatted as:
//!
//!       [out] derives-from  artifact:5 — FoundingSpec
//!       [in]  blocks        task:12 — Implement parser
//!
//!     j/k (or arrow keys) navigate. Enter re-centers on the selected
//!     entity (task 4028: navigate along a link).
//!
//!   • Right pane (detail): for the focused entity, shows:
//!       - Header: "kind:id — title" (bold)
//!       - "Links: N" count
//!       - Currently-selected-link detail: direction, relationship,
//!         and the other entity's kind:id — title
//!       - Navigation hint: "Enter to re-center on selected entity"
//!
//! Tasks 4027 (related-entities panel) and 4028 (navigate along a link).
//!
//! Acceptance invariants:
//!   (4027) The navigator lists ALL entity_links rows in BOTH directions
//!          across ALL relationship kinds for the focus entity.
//!          Every datum queried (direction, relationship, kind:id, title)
//!          is rendered — hard rule (a) from the brief.
//!   (4028) Pressing Enter on a row re-centers the view on the selected
//!          entity (re-queries its neighborhood). The cross-view focus
//!          mechanism in app.zig also allows jumping to the Scope Explorer
//!          focused on the target entity via app.FocusRequest.
//!
//! Cross-view focus mechanism (task 4028):
//!   EntityLinkState.handleKey returns a FocusRequest when Enter is pressed
//!   on a selected row. The caller (app.zig) inspects the return value and:
//!     1. Re-queries the entity_link_graph for the new focus entity.
//!     2. Optionally switches to another view (e.g. Scope Explorer) if the
//!        FocusRequest.switch_view flag is set (future extension point for M7
//!        open_questions 'g' jump — not wired here, just not precluded).
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by EntityLinkState and released via deinit.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread.
//!   - MEMORY GUARD (brief rule (c)): OOM/error fallback in view_model helpers
//!     never aliases kind/id pointers into label/title fields that deinit frees.
//!     The EntityFocus.title field uses an owned empty string on failure (never
//!     aliasing EntityFocus.kind). Enforced in view_model.queryEntityLinkGraph.

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// FocusRequest (task 4028 cross-view focus mechanism)
// =========================================================================

/// Returned by handleKey when a navigation action requires a view or focus
/// change. The minimal cross-view focus mechanism for M9.
///
/// Design: EntityLinkState.handleKey returns `?FocusRequest`. When non-null,
/// app.zig:
///   1. Re-loads the entity_link_graph for the target entity (always).
///   2. If switch_to_view != null, switches the active view to that id and
///      optionally sets a pending focus on it (reserved for future use by
///      M7 open_questions 'g' jump — not wired here, not precluded).
///
/// The struct is kept minimal: kind and id are the target entity; switch_to_view
/// is the optional cross-view jump. No heap allocation inside FocusRequest —
/// the kind string is a slice into the EntityLinkRow.other_kind field which
/// is owned by the EntityLinkState and outlives the key handler call.
pub const FocusRequest = struct {
    /// Target entity kind string (e.g. "task", "plan"). Points into
    /// EntityLinkRow.other_kind; valid until the next reload().
    kind: []const u8,
    /// Target entity id.
    id: i64,
    /// When non-null, also switch to this view after reloading the
    /// entity_link_graph. Extension point for cross-view navigation.
    switch_to_view: ?view_model.ViewId,
};

// =========================================================================
// EntityLinkState
// =========================================================================

/// All mutable state for the Entity-Link Graph view.
pub const EntityLinkState = struct {
    allocator: std.mem.Allocator,

    /// Current neighborhood data for the focus entity.
    /// NOTE: always initialised via EntityLinkState.init — never use a struct
    /// literal default here because EntityLinkGraphData.links_count_label must
    /// be heap-owned (deinit calls allocator.free on it).
    data: view_model.EntityLinkGraphData,

    /// Navigator selection index (0-based into data.rows).
    selected_idx: usize = 0,

    /// Navigator scroll offset.
    scroll_offset: usize = 0,

    pub fn init(allocator: std.mem.Allocator) EntityLinkState {
        return .{
            .allocator = allocator,
            // Safe sentinel: zero-len rows and links_count_label are static slices.
            // allocator.free on a zero-len slice is always a no-op in Zig's std
            // allocators, so EntityLinkGraphData.deinit is safe even before any
            // reload has replaced these with heap-owned slices.
            .data = .{
                .focus = null,
                .rows = &.{},
                .links_count_label = &.{},
            },
            .selected_idx = 0,
            .scroll_offset = 0,
        };
    }

    pub fn deinit(self: *EntityLinkState) void {
        self.data.deinit(self.allocator);
    }

    /// Reload using the default entity heuristic. Called on initial launch
    /// when no explicit focus has been set.
    pub fn reloadDefault(self: *EntityLinkState, d: *db.sqlite.Db) !void {
        const old_data = self.data;
        self.data = try view_model.queryEntityLinkGraphDefault(d, self.allocator);
        old_data.deinit(self.allocator);
        self.clampSelection();
    }

    /// Reload for a specific focus entity. Used by task 4028 re-center and
    /// by app.zig on db_changed when a focus is already set.
    pub fn reloadFor(
        self: *EntityLinkState,
        d: *db.sqlite.Db,
        kind: []const u8,
        id: i64,
    ) !void {
        const old_data = self.data;
        self.data = try view_model.queryEntityLinkGraph(d, self.allocator, kind, id);
        old_data.deinit(self.allocator);
        self.clampSelection();
    }

    /// Reload keeping the current focus (re-query on db_changed).
    pub fn reload(self: *EntityLinkState, d: *db.sqlite.Db) !void {
        if (self.data.focus) |f| {
            // Keep the focus kind/id across the reload.
            // We must copy kind before freeing old_data.
            const kind_copy = try self.allocator.dupe(u8, f.kind);
            defer self.allocator.free(kind_copy);
            const id = f.id;
            try self.reloadFor(d, kind_copy, id);
        } else {
            try self.reloadDefault(d);
        }
    }

    fn clampSelection(self: *EntityLinkState) void {
        if (self.data.rows.len > 0) {
            if (self.selected_idx >= self.data.rows.len) {
                self.selected_idx = self.data.rows.len - 1;
            }
        } else {
            self.selected_idx = 0;
        }
    }

    /// Handle a key event. Returns:
    ///   - `{ .consumed = true, .focus = null }` — key consumed, no navigation.
    ///   - `{ .consumed = true, .focus = some }` — key consumed, re-center needed.
    ///   - `{ .consumed = false, .focus = null }` — key not consumed.
    pub const HandleKeyResult = struct {
        consumed: bool,
        focus: ?FocusRequest,
    };

    pub fn handleKey(self: *EntityLinkState, key: Key) HandleKeyResult {
        const count = self.data.rows.len;

        // j / arrow-down: move selection down.
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            if (count > 0 and self.selected_idx + 1 < count) {
                self.selected_idx += 1;
            }
            return .{ .consumed = true, .focus = null };
        }
        // k / arrow-up: move selection up.
        if (key.matches('k', .{}) or key.matches(Key.up, .{})) {
            if (self.selected_idx > 0) {
                self.selected_idx -= 1;
            }
            return .{ .consumed = true, .focus = null };
        }

        // Enter: re-center on the selected entity (task 4028).
        if (key.matches(Key.enter, .{})) {
            if (count > 0) {
                const row = self.data.rows[self.selected_idx];
                return .{
                    .consumed = true,
                    .focus = .{
                        .kind = row.other_kind,
                        .id = row.other_id,
                        .switch_to_view = null,
                    },
                };
            }
            return .{ .consumed = true, .focus = null };
        }

        return .{ .consumed = false, .focus = null };
    }
};

// =========================================================================
// Render
// =========================================================================

/// Render the Entity-Link Graph view into the navigator and detail windows.
///
/// Navigator (left): list of entity_links rows for the focus entity.
/// Detail (right): focus entity header + selected-link detail.
pub fn render(
    state: *const EntityLinkState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    _ = allocator;
    renderNavigator(state, nav_win);
    renderDetail(state, detail_win);
}

/// Render the navigator pane (left): entity_links list for the focus entity.
///
/// Each row: "[out] relationship  kind:id — title"
/// The selected row is rendered in reverse-video.
///
/// INVARIANT (task 4027): EVERY queried datum is rendered:
///   - direction ([out]/[in])
///   - relationship (derives-from, depends-on, etc.)
///   - kind:id of the other entity
///   - resolved title (when available)
fn renderNavigator(state: *const EntityLinkState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    // Header: focus entity.
    if (state.data.focus) |f| {
        _ = win.printSegment(.{
            .text = f.header,
            .style = .{ .bold = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
    } else {
        _ = win.printSegment(.{
            .text = "(no entity selected)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
    }

    if (win.height < 2) return;

    if (state.data.rows.len == 0) {
        // Empty state: show a clear message, not just silence.
        _ = win.printSegment(.{
            .text = "(no entity links)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 1, .col_offset = 0 });
        return;
    }

    // Subheader: link count.
    // Use the heap-owned links_count_label — NOT a local bufPrint buffer —
    // because printSegment stores grapheme pointers into the window cell buffer,
    // and those pointers must remain valid after this function returns (for
    // render-level tests using collectScreenText).
    _ = win.printSegment(.{
        .text = state.data.links_count_label,
        .style = .{ .dim = true },
    }, .{ .row_offset = 1, .col_offset = 0 });

    if (win.height < 3) return;

    // Rows: viewport starts at row 2 (below header + subheader).
    const viewport_h: usize = if (win.height > 2) @as(usize, @intCast(win.height)) - 2 else 0;
    const scroll: usize = if (state.selected_idx >= viewport_h)
        state.selected_idx - viewport_h + 1
    else
        0;

    var display_row: u16 = 2;
    for (state.data.rows, 0..) |row, i| {
        if (i < scroll) continue;
        if (display_row >= win.height) break;

        const is_selected = (i == state.selected_idx);
        const style: Style = if (is_selected)
            .{ .bold = true, .reverse = true }
        else
            .{};

        // HARD RULE (a): render the full display_text so all queried data appears.
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
///   row 0       — focus entity header (bold)
///   row 1       — "Links: N" count
///   row 2       — blank separator
///   row 3       — "Selected link:" header (bold dim) [when rows non-empty]
///   row 4       — "  Direction: [out]/[in]"
///   row 5       — "  Relationship: <rel>"
///   row 6       — "  Entity: <kind>:<id> — <title>"
///   row 7       — blank
///   row 8       — "  Enter: re-center on this entity"
///
/// INVARIANT (task 4027): ALL datum fields (direction, relationship,
/// kind:id, title) must be rendered in the detail pane for the selected row.
fn renderDetail(state: *const EntityLinkState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    var row: u16 = 0;

    // ---- Focus entity header (row 0) ----------------------------------------
    if (state.data.focus) |f| {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = f.header,
                .style = .{ .bold = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
    } else {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "(no entity selected — navigate to an entity first)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
        return;
    }
    row += 1;

    // ---- Link count (row 1) -------------------------------------------------
    // Use the heap-owned links_count_label for the same reason as renderNavigator:
    // printSegment stores grapheme pointers that must outlive this function.
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = state.data.links_count_label,
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
    }
    row += 1;

    if (state.data.rows.len == 0) {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "(no entity links for this entity)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
        return;
    }

    // ---- Blank separator (row 2) --------------------------------------------
    row += 1;
    if (row >= win.height) return;

    // ---- Selected link detail (rows 3–8) ------------------------------------
    _ = win.printSegment(.{
        .text = "Selected link:",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = row, .col_offset = 0 });
    row += 1;
    if (row >= win.height) return;

    const sel = state.data.rows[state.selected_idx];

    // Direction line.
    const dir_str: []const u8 = switch (sel.direction) {
        .outbound => "[out]  (this entity → other)",
        .inbound => "[in]   (other entity → this)",
    };
    _ = win.printSegment(.{
        .text = "  Direction: ",
        .style = .{},
    }, .{ .row_offset = row, .col_offset = 0 });
    _ = win.printSegment(.{
        .text = dir_str,
        .style = .{ .bold = true },
    }, .{ .row_offset = row, .col_offset = 13 });
    row += 1;
    if (row >= win.height) return;

    // Relationship line.
    _ = win.printSegment(.{
        .text = "  Relationship: ",
        .style = .{},
    }, .{ .row_offset = row, .col_offset = 0 });
    _ = win.printSegment(.{
        .text = sel.relationship.toText(),
        .style = .{ .bold = true },
    }, .{ .row_offset = row, .col_offset = 16 });
    row += 1;
    if (row >= win.height) return;

    // Entity line: full display_text (includes direction, rel, kind:id, title).
    _ = win.printSegment(.{
        .text = "  ",
        .style = .{},
    }, .{ .row_offset = row, .col_offset = 0 });
    _ = win.printSegment(.{
        .text = sel.display_text,
        .style = .{},
    }, .{ .row_offset = row, .col_offset = 2 });
    row += 1;
    if (row >= win.height) return;

    // Blank separator.
    row += 1;
    if (row >= win.height) return;

    // Navigation hint (task 4028).
    _ = win.printSegment(.{
        .text = "  Enter: re-center on this entity",
        .style = .{ .dim = true },
    }, .{ .row_offset = row, .col_offset = 0 });
}

/// Return a one-line legend string for the key legend bar.
pub fn legendLabel(buf: []u8) []const u8 {
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Select  Enter Re-center  Tab Focus  1-9 View",
        .{},
    ) catch "  q Quit  j/k Select  Enter Re-center";
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

// =========================================================================
// EntityLinkState lifecycle tests
// =========================================================================

test "entity_link_graph: init and deinit are clean" {
    var state = EntityLinkState.init(testing.allocator);
    defer state.deinit();
    try testing.expect(state.data.focus == null);
    try testing.expectEqual(@as(usize, 0), state.data.rows.len);
}

test "entity_link_graph: reloadDefault on empty DB yields null focus (empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = EntityLinkState.init(a);
    defer state.deinit();

    try state.reloadDefault(&d);

    try testing.expect(state.data.focus == null);
    try testing.expectEqual(@as(usize, 0), state.data.rows.len);
}

test "entity_link_graph: reloadFor queries outbound edges (task 4027)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert a decision and an artifact; link decision derives-from artifact.
    const did = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Focus Decision','body','accepted')",
        &.{},
    );
    const aid = try d.execParams(
        "insert into artifacts (scope_kind, title, body, kind) values ('global','Source Artifact','abody','tech_spec')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', ?, 'artifact', ?, 'derives-from')",
        &.{ .{ .int = did }, .{ .int = aid } },
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();
    try state.reloadFor(&d, "decision", did);

    try testing.expect(state.data.focus != null);
    try testing.expectEqualStrings("decision", state.data.focus.?.kind);
    try testing.expectEqual(did, state.data.focus.?.id);
    // Title must be resolved.
    try testing.expectEqualStrings("Focus Decision", state.data.focus.?.title);
    // Header must contain kind:id.
    try testing.expect(std.mem.indexOf(u8, state.data.focus.?.header, "decision:") != null);

    // One outbound edge: decision → artifact via derives-from.
    try testing.expectEqual(@as(usize, 1), state.data.rows.len);
    try testing.expectEqual(view_model.LinkEdgeDirection.outbound, state.data.rows[0].direction);
    try testing.expectEqual(view_model.LinkRelationship.derives_from, state.data.rows[0].relationship);
    try testing.expectEqualStrings("artifact", state.data.rows[0].other_kind);
    try testing.expectEqual(aid, state.data.rows[0].other_id);
    // display_text must contain [out], derives-from, artifact:id, and title.
    const dt = state.data.rows[0].display_text;
    try testing.expect(std.mem.indexOf(u8, dt, "[out]") != null);
    try testing.expect(std.mem.indexOf(u8, dt, "derives-from") != null);
    try testing.expect(std.mem.indexOf(u8, dt, "artifact:") != null);
    try testing.expect(std.mem.indexOf(u8, dt, "Source Artifact") != null);
}

test "entity_link_graph: reloadFor queries inbound edges (task 4027)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert a task and a scenario; scenario verifies task (inbound to task).
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Target Task','todo')",
        &.{},
    );
    const sid = try d.execParams(
        "insert into test_scenarios (scope_kind, title, status) values ('global','Source Scenario','ready')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('test_scenario', ?, 'task', ?, 'verifies')",
        &.{ .{ .int = sid }, .{ .int = tid } },
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();
    // Focus on the task — the scenario's verifies edge is inbound to the task.
    try state.reloadFor(&d, "task", tid);

    try testing.expect(state.data.focus != null);
    try testing.expectEqual(@as(usize, 1), state.data.rows.len);

    const row = state.data.rows[0];
    try testing.expectEqual(view_model.LinkEdgeDirection.inbound, row.direction);
    try testing.expectEqual(view_model.LinkRelationship.verifies, row.relationship);
    // display_text must contain [in] and the scenario name.
    try testing.expect(std.mem.indexOf(u8, row.display_text, "[in]") != null);
    try testing.expect(std.mem.indexOf(u8, row.display_text, "verifies") != null);
    try testing.expect(std.mem.indexOf(u8, row.display_text, "Source Scenario") != null);
}

test "entity_link_graph: multiple relationship kinds across both directions (task 4027)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert a plan as the focus.
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Center Plan','center-plan','active')",
        &.{},
    );
    // Insert a task that this plan's task blocks.
    const tid1 = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Blocked Task','blocked')",
        &.{},
    );
    // Insert another task that is the from side (touches).
    const tid2 = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Touching Task','doing')",
        &.{},
    );
    // Insert a decision deriving from this plan.
    const did = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Derived Decision','body','proposed')",
        &.{},
    );

    // Edge 1: task:tid1 is blocked by plan:pid  →  plan→task via 'depends-on' (outbound from plan).
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('plan', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = pid }, .{ .int = tid1 } },
    );
    // Edge 2: task:tid2 touches plan:pid  →  inbound to plan.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'plan', ?, 'touches')",
        &.{ .{ .int = tid2 }, .{ .int = pid } },
    );
    // Edge 3: decision derives-from plan  →  inbound to plan.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', ?, 'plan', ?, 'derives-from')",
        &.{ .{ .int = did }, .{ .int = pid } },
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();
    try state.reloadFor(&d, "plan", pid);

    try testing.expectEqual(@as(usize, 3), state.data.rows.len);

    // Verify that all three relationship kinds appear in display_text.
    var found_blocks = false;
    var found_touches = false;
    var found_derives = false;
    for (state.data.rows) |row| {
        if (std.mem.indexOf(u8, row.display_text, "depends-on") != null) found_blocks = true;
        if (std.mem.indexOf(u8, row.display_text, "touches") != null) found_touches = true;
        if (std.mem.indexOf(u8, row.display_text, "derives-from") != null) found_derives = true;
    }
    try testing.expect(found_blocks);
    try testing.expect(found_touches);
    try testing.expect(found_derives);

    // Outbound edge (depends-on) must be [out]; inbound edges (touches, derives-from) must be [in].
    for (state.data.rows) |row| {
        if (row.relationship == .depends_on) {
            try testing.expectEqual(view_model.LinkEdgeDirection.outbound, row.direction);
            try testing.expect(std.mem.indexOf(u8, row.display_text, "[out]") != null);
        }
        if (row.relationship == .touches or row.relationship == .derives_from) {
            try testing.expectEqual(view_model.LinkEdgeDirection.inbound, row.direction);
            try testing.expect(std.mem.indexOf(u8, row.display_text, "[in]") != null);
        }
    }
}

test "entity_link_graph: empty entity shows no links (empty state, task 4027)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert a task with no entity_links at all.
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Isolated Task','todo')",
        &.{},
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();
    try state.reloadFor(&d, "task", tid);

    try testing.expect(state.data.focus != null);
    try testing.expectEqual(@as(usize, 0), state.data.rows.len);
    try testing.expectEqualStrings("Isolated Task", state.data.focus.?.title);
}

test "entity_link_graph: handleKey j/k moves selection" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const did = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','D','body','accepted')",
        &.{},
    );
    const aid1 = try d.execParams(
        "insert into artifacts (scope_kind, title, body, kind) values ('global','A1','b','tech_spec')",
        &.{},
    );
    const aid2 = try d.execParams(
        "insert into artifacts (scope_kind, title, body, kind) values ('global','A2','b','tech_spec')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', ?, 'artifact', ?, 'derives-from')",
        &.{ .{ .int = did }, .{ .int = aid1 } },
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', ?, 'artifact', ?, 'cites')",
        &.{ .{ .int = did }, .{ .int = aid2 } },
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();
    try state.reloadFor(&d, "decision", did);
    try testing.expectEqual(@as(usize, 2), state.data.rows.len);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);

    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    const r1 = state.handleKey(j_key);
    try testing.expect(r1.consumed);
    try testing.expect(r1.focus == null);
    try testing.expectEqual(@as(usize, 1), state.selected_idx);

    const k_key = Key{ .codepoint = 'k', .mods = .{} };
    const r2 = state.handleKey(k_key);
    try testing.expect(r2.consumed);
    try testing.expect(r2.focus == null);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "entity_link_graph: handleKey Enter returns FocusRequest for re-center (task 4028)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Task A','todo')",
        &.{},
    );
    const tid2 = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Task B','doing')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = tid }, .{ .int = tid2 } },
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();
    try state.reloadFor(&d, "task", tid);
    try testing.expectEqual(@as(usize, 1), state.data.rows.len);

    const enter_key = Key{ .codepoint = Key.enter, .mods = .{} };
    const result = state.handleKey(enter_key);
    try testing.expect(result.consumed);
    try testing.expect(result.focus != null);

    const focus_req = result.focus.?;
    try testing.expectEqualStrings("task", focus_req.kind);
    try testing.expectEqual(tid2, focus_req.id);
    // switch_to_view is null for the simple re-center case.
    try testing.expect(focus_req.switch_to_view == null);
}

test "entity_link_graph: re-center changes neighborhood (task 4028)" {
    // Verifies that after pressing Enter (re-center), reloadFor changes the
    // focus entity and the rows represent the new entity's neighborhood.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid1 = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Task Alpha','todo')",
        &.{},
    );
    const tid2 = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Task Beta','doing')",
        &.{},
    );
    const tid3 = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Task Gamma','done')",
        &.{},
    );
    // tid1 blocks tid2; tid2 addresses tid3.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = tid1 }, .{ .int = tid2 } },
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'addresses')",
        &.{ .{ .int = tid2 }, .{ .int = tid3 } },
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();

    // Start on tid1: 1 outbound (blocks tid2).
    try state.reloadFor(&d, "task", tid1);
    try testing.expectEqual(@as(usize, 1), state.data.rows.len);
    try testing.expectEqualStrings("Task Alpha", state.data.focus.?.title);

    // Press Enter to re-center on tid2.
    const enter_key = Key{ .codepoint = Key.enter, .mods = .{} };
    const result = state.handleKey(enter_key);
    try testing.expect(result.focus != null);
    const fr = result.focus.?;

    // Simulate app.zig handling the FocusRequest by reloading for the target.
    try state.reloadFor(&d, fr.kind, fr.id);

    // Now focused on tid2: inbound from tid1 (blocks) + outbound to tid3 (addresses).
    try testing.expectEqual(@as(usize, 2), state.data.rows.len);
    try testing.expectEqualStrings("Task Beta", state.data.focus.?.title);

    var found_in = false;
    var found_out = false;
    for (state.data.rows) |row| {
        if (row.direction == .inbound) found_in = true;
        if (row.direction == .outbound) found_out = true;
    }
    try testing.expect(found_in);
    try testing.expect(found_out);
}

// =========================================================================
// RENDER-LEVEL TESTS (rule (b): assert rendered text in screen buffer)
// =========================================================================
//
// These tests allocate a real vaxis.Screen, call the render functions,
// then scan the cell buffer for expected text. A struct-level assertion
// that a field is populated is NOT sufficient; these tests prove the
// rendered output actually contains the data.

test "entity_link_graph: renderNavigator renders entity links with all data (task 4027 render-level)" {
    // Seeds an entity with links across multiple relationship kinds + both
    // directions, and verifies that ALL queried data appears in the rendered output.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Focus: a plan.
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Focus Plan','focus-plan','active')",
        &.{},
    );
    // Outbound: plan blocks a task.
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Blocked Task','blocked')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('plan', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = pid }, .{ .int = tid } },
    );
    // Inbound: a decision derives-from this plan.
    const did = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Derived Decision','body','accepted')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', ?, 'plan', ?, 'derives-from')",
        &.{ .{ .int = did }, .{ .int = pid } },
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();
    try state.reloadFor(&d, "plan", pid);
    try testing.expectEqual(@as(usize, 2), state.data.rows.len);

    const win_w: u16 = 100;
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

    // RENDER-LEVEL ASSERTIONS (hard rule (a): render every queried datum):
    // Focus header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Focus Plan") != null);
    // Link count must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Links:") != null);
    // Outbound row: [out], blocks, task, Blocked Task.
    try testing.expect(std.mem.indexOf(u8, text, "[out]") != null);
    try testing.expect(std.mem.indexOf(u8, text, "depends-on") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Blocked Task") != null);
    // Inbound row: [in], derives-from, Derived Decision.
    try testing.expect(std.mem.indexOf(u8, text, "[in]") != null);
    try testing.expect(std.mem.indexOf(u8, text, "derives-from") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Derived Decision") != null);
}

test "entity_link_graph: renderNavigator shows empty state for entity with no links (render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Lonely Task','todo')",
        &.{},
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();
    try state.reloadFor(&d, "task", tid);
    try testing.expectEqual(@as(usize, 0), state.data.rows.len);

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

    // Focus header must show the entity.
    try testing.expect(std.mem.indexOf(u8, text, "Lonely Task") != null);
    // Empty state message must appear.
    try testing.expect(std.mem.indexOf(u8, text, "no entity links") != null);
}

test "entity_link_graph: renderDetail renders all fields for selected link (task 4027 render-level)" {
    // Critical render-level test for task 4027: verifies that direction,
    // relationship, kind:id, and title ALL appear in the rendered detail pane.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const did = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Detail Decision','body','proposed')",
        &.{},
    );
    const aid = try d.execParams(
        "insert into artifacts (scope_kind, title, body, kind) values ('global','Detail Artifact','abody','tech_spec')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', ?, 'artifact', ?, 'derives-from')",
        &.{ .{ .int = did }, .{ .int = aid } },
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();
    try state.reloadFor(&d, "decision", did);
    try testing.expectEqual(@as(usize, 1), state.data.rows.len);

    const win_w: u16 = 100;
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

    renderDetail(&state, detail_win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // RENDER-LEVEL ASSERTIONS (hard rule (a): all queried data must render):
    // (a) Focus header with decision title.
    try testing.expect(std.mem.indexOf(u8, text, "Detail Decision") != null);
    // (b) Link count.
    try testing.expect(std.mem.indexOf(u8, text, "Links:") != null);
    // (c) "Selected link:" header.
    try testing.expect(std.mem.indexOf(u8, text, "Selected link:") != null);
    // (d) Direction label (outbound).
    try testing.expect(std.mem.indexOf(u8, text, "[out]") != null);
    // (e) Relationship name.
    try testing.expect(std.mem.indexOf(u8, text, "derives-from") != null);
    // (f) The artifact title in the display_text line.
    try testing.expect(std.mem.indexOf(u8, text, "Detail Artifact") != null);
    // (g) Navigation hint for task 4028.
    try testing.expect(std.mem.indexOf(u8, text, "Enter") != null);
    try testing.expect(std.mem.indexOf(u8, text, "re-center") != null);
}

test "entity_link_graph: renderNavigator shows no-entity-selected when focus is null (render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = EntityLinkState.init(a);
    defer state.deinit();
    // Do NOT reload — keep null focus.

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

    try testing.expect(std.mem.indexOf(u8, text, "no entity selected") != null);
}

test "entity_link_graph: re-center changes rendered neighborhood (task 4028 render-level)" {
    // After re-centering from tid1 to tid2, the rendered navigator must show
    // tid2's neighborhood (not tid1's).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid1 = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Task Zeta','todo')",
        &.{},
    );
    const tid2 = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Task Eta','done')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'supersedes')",
        &.{ .{ .int = tid1 }, .{ .int = tid2 } },
    );

    var state = EntityLinkState.init(a);
    defer state.deinit();

    // Start on tid1.
    try state.reloadFor(&d, "task", tid1);
    try testing.expectEqual(@as(usize, 1), state.data.rows.len);

    // Trigger re-center via Enter key.
    const enter_key = Key{ .codepoint = Key.enter, .mods = .{} };
    const result = state.handleKey(enter_key);
    try testing.expect(result.focus != null);

    // Simulate app.zig: reload for the target.
    try state.reloadFor(&d, result.focus.?.kind, result.focus.?.id);

    // Now render the navigator for tid2's neighborhood.
    const win_w: u16 = 100;
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

    // The rendered output must now show tid2's header ("Task Eta"),
    // not tid1's ("Task Zeta" — tid1 should not appear as header).
    try testing.expect(std.mem.indexOf(u8, text, "Task Eta") != null);
    // The inbound link from tid1 must show supersedes and Task Zeta.
    try testing.expect(std.mem.indexOf(u8, text, "supersedes") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Task Zeta") != null);
}

test "entity_link_graph: legendLabel fits in buf" {
    var buf: [128]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Quit") != null);
    try testing.expect(std.mem.indexOf(u8, label, "Re-center") != null);
}

test "entity_link_graph compiles" {
    std.testing.refAllDecls(@This());
}
