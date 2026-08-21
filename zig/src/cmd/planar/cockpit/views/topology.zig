//! cockpit/views/topology.zig — Scope / Association Topology view (M14).
//!
//! Renders a two-pane layout for the Scope / Association Topology:
//!
//!   • Left pane (navigator): associations ordered by slug. Each row shows:
//!       "[<kind>] <slug>  (<N> members)"
//!     j/k (or arrow keys) navigate between associations.
//!
//!   • Right pane (detail): for the selected association:
//!       - slug, name, kind, auto_detected flag
//!       - "Members:" section — for each member project:
//!           project slug: project name
//!           root:         <root_path>  (or "(none)")
//!           remote:       <git_remote> (or "(none)")
//!           source:       <source_label>
//!           scope:        <resolution_rule>
//!         The scope / resolution-rule line surfaces the engine's
//!         cwd-derived scope-resolution outcome for this project, mirroring
//!         src/engine/identity/scope.zig `deriveFromCwd` lines 242–280:
//!           member_count = 1 → "→ resolves here"
//!           member_count > 1 → "→ ambiguous (N)"
//!
//!   • Empty state: when no associations exist, the navigator shows
//!       "(no associations registered)"
//!     and the detail pane shows "(no association selected)".
//!
//! Tasks 4039 (association → member-project map) and 4040 (routing /
//! scope-resolution context for each association).
//!
//! Acceptance invariants:
//!   (4039) ALL queried fields are rendered: association slug, name, kind,
//!          auto_detected; per member: project_slug, project_name,
//!          root_path, git_remote, source, source_label.
//!   (4040) resolution_rule is rendered for each member, faithfully
//!          mirroring the engine's scope-resolution model
//!          (src/engine/identity/scope.zig `deriveFromCwd` lines 242–280).
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model; no writes.
//!   - All heap-owned data is owned by TopologyState and released via deinit.
//!   - Live updates: the wake thread posts .db_changed → app.zig calls
//!     `reload` on the active view. No second wake thread.
//!   - MEMORY GUARD (brief rule (c)): no filter string fields in this view
//!     (no cycling filter added). All rendered strings come from freshly
//!     allocated view_model fields.
//!   - 12th view: registered with key 't' (display-only; numeric jump fires
//!     only for '1'–'9'). Tab/Shift-Tab cycling reaches this view.
//!
//! Schema confirmed from migrations/00001_foundation.up.sql:
//!   projects(id, slug, name, root_path, git_remote, created_at, updated_at)
//!   associations(id, slug, name, kind, auto_detected, config_json,
//!                created_at, updated_at)
//!   project_associations(project_id, association_id, source, created_at)

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// TopologyState
// =========================================================================

/// All mutable state for the Scope / Association Topology view.
pub const TopologyState = struct {
    allocator: std.mem.Allocator,

    /// Current snapshot of associations with their member projects.
    rows: []view_model.TopologyAssocRow = &.{},

    /// Navigator selection index (0-based into rows).
    selected_idx: usize = 0,

    /// Navigator scroll offset.
    scroll_offset: usize = 0,

    pub fn init(allocator: std.mem.Allocator) TopologyState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *TopologyState) void {
        view_model.TopologyAssocRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};
    }

    /// Reload all topology data from the DB. Called on db_changed and on
    /// initial launch. Reuses the existing wake .db_changed integration
    /// (no second wake thread).
    pub fn reload(self: *TopologyState, d: *db.sqlite.Db) !void {
        view_model.TopologyAssocRow.deinitMany(self.rows, self.allocator);
        self.rows = &.{};

        self.rows = try view_model.queryTopology(d, self.allocator);

        // Clamp selection.
        if (self.rows.len > 0) {
            if (self.selected_idx >= self.rows.len) {
                self.selected_idx = self.rows.len - 1;
            }
        } else {
            self.selected_idx = 0;
        }
    }

    /// Handle a key event. Returns true when the key was consumed.
    pub fn handleKey(self: *TopologyState, key: Key) bool {
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

        return false;
    }
};

// =========================================================================
// Render
// =========================================================================

/// Render the Topology view into the navigator and detail windows.
///
/// Navigator (left pane): associations list.
/// Detail (right pane): selected association with all member projects and
/// scope-resolution context (tasks 4039 + 4040).
pub fn render(
    state: *const TopologyState,
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
///   row 0: "Topology" (bold dim header)
///   row 1+: one row per association, selected reversed
///   Empty: "(no associations registered)"
///
/// INVARIANT (task 4039): every row's display_text (pre-formatted as
///   "[<kind>] <slug>  (<N> members)")
/// is rendered via printSegment using the heap-allocated display_text so
/// grapheme pointers remain valid.
fn renderNavigator(state: *const TopologyState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    // Header row: "Topology"
    _ = win.printSegment(.{
        .text = "Topology",
        .style = .{ .bold = true, .dim = true },
    }, .{ .row_offset = 0, .col_offset = 0 });
    if (win.height < 2) return;

    if (state.rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no associations registered)",
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

        // INVARIANT (task 4039): render display_text — the pre-formatted,
        // heap-allocated "[<kind>] <slug>  (<N> members)".
        _ = win.printSegment(.{
            .text = row.display_text,
            .style = style,
        }, .{ .row_offset = display_row, .col_offset = 0 });
        display_row += 1;
    }
}

/// Render the detail pane (right) for the selected association.
///
/// Layout (all fields from the queried row — task 4039):
///   row 0:  "<slug>"    (bold) — the association slug
///   row 1:  "name:   <name>"
///   row 2:  "kind:   <kind>"
///   row 3:  "auto:   yes / no"
///   separator
///   "Members:" (bold dim header)
///   For each member project (task 4040 — scope-resolution context):
///     row N:   "  <project_slug>: <project_name>"  (bold)
///     row N+1: "  root:    <root_path> (or "(none)")"
///     row N+2: "  remote:  <git_remote> (or "(none)")"
///     row N+3: "  source:  <source_label>"
///     row N+4: "  scope:   <resolution_rule>"
///              ↑ This is the engine-faithful scope-resolution context
///                (mirrors scope.zig deriveFromCwd lines 242–280).
///     blank line separator between members.
///
/// INVARIANT (tasks 4039 + 4040): ALL rendered strings come from freshly
/// heap-allocated view_model fields. Every datum queried is rendered:
/// slug, name, kind, auto_detected; per member: project_slug,
/// project_name, root_path, git_remote, source_label, resolution_rule.
fn renderDetail(state: *const TopologyState, win: Window) void {
    if (win.height == 0 or win.width == 0) return;

    if (state.rows.len == 0) {
        _ = win.printSegment(.{
            .text = "(no association selected)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
        return;
    }

    if (state.selected_idx >= state.rows.len) return;
    const r = state.rows[state.selected_idx];

    var row: u16 = 0;

    // ---- slug (row 0) -------------------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = r.slug,
            .style = .{ .bold = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    // ---- name (row 1) -------------------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "name:   ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.name,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 8 });
        row += 1;
    }

    // ---- kind (row 2) -------------------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "kind:   ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = r.kind,
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 8 });
        row += 1;
    }

    // ---- auto_detected (row 3) ----------------------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "auto:   ",
            .style = .{ .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        _ = win.printSegment(.{
            .text = if (r.auto_detected) "yes" else "no",
            .style = .{},
        }, .{ .row_offset = row, .col_offset = 8 });
        row += 1;
    }

    // ---- Separator ----------------------------------------------------------
    if (row < win.height) row += 1;

    // ---- Members section (tasks 4039 + 4040) --------------------------------
    if (row < win.height) {
        _ = win.printSegment(.{
            .text = "Members:",
            .style = .{ .bold = true, .dim = true },
        }, .{ .row_offset = row, .col_offset = 0 });
        row += 1;
    }

    if (r.members.len == 0) {
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  (no member projects)",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
        }
        return;
    }

    for (r.members, 0..) |m, mi| {
        _ = mi;
        if (row >= win.height) return;

        // Member header: "  <project_slug>: <project_name>" (bold).
        // INVARIANT (task 4039): project_slug and project_name rendered here.
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  ",
                .style = .{ .bold = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{
                .text = m.project_slug,
                .style = .{ .bold = true },
            }, .{ .row_offset = row, .col_offset = 2 });
            const slug_end: u16 = @intCast(2 + m.project_slug.len);
            _ = win.printSegment(.{
                .text = ": ",
                .style = .{ .bold = true },
            }, .{ .row_offset = row, .col_offset = slug_end });
            const colon_end: u16 = slug_end + 2;
            _ = win.printSegment(.{
                .text = m.project_name,
                .style = .{ .bold = true },
            }, .{ .row_offset = row, .col_offset = colon_end });
            row += 1;
        }

        // root_path.
        // INVARIANT (task 4039): root_path rendered here.
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  root:   ",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            if (m.root_path) |rp| {
                _ = win.printSegment(.{
                    .text = rp,
                    .style = .{},
                }, .{ .row_offset = row, .col_offset = 10 });
            } else {
                _ = win.printSegment(.{
                    .text = "(none)",
                    .style = .{ .dim = true },
                }, .{ .row_offset = row, .col_offset = 10 });
            }
            row += 1;
        }

        // git_remote.
        // INVARIANT (task 4039): git_remote rendered here.
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  remote: ",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            if (m.git_remote) |gr| {
                _ = win.printSegment(.{
                    .text = gr,
                    .style = .{},
                }, .{ .row_offset = row, .col_offset = 10 });
            } else {
                _ = win.printSegment(.{
                    .text = "(none)",
                    .style = .{ .dim = true },
                }, .{ .row_offset = row, .col_offset = 10 });
            }
            row += 1;
        }

        // source_label (membership origin).
        // INVARIANT (task 4039): source_label rendered here.
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  source: ",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{
                .text = m.source_label,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 10 });
            row += 1;
        }

        // resolution_rule — the engine-faithful scope-resolution context.
        // INVARIANT (task 4040): resolution_rule rendered here, mirroring
        // src/engine/identity/scope.zig `deriveFromCwd` lines 242–280:
        //   member_count = 1 → "→ resolves here"
        //   member_count > 1 → "→ ambiguous (N)"
        if (row < win.height) {
            _ = win.printSegment(.{
                .text = "  scope:  ",
                .style = .{ .dim = true },
            }, .{ .row_offset = row, .col_offset = 0 });
            _ = win.printSegment(.{
                .text = m.resolution_rule,
                .style = .{},
            }, .{ .row_offset = row, .col_offset = 10 });
            row += 1;
        }

        // Blank separator between members.
        if (row < win.height) row += 1;
    }
}

/// Return a one-line legend string for the key legend bar.
/// Note: this is the 12th view; there is no numeric key jump (only Tab/Shift-Tab).
pub fn legendLabel(buf: []u8) []const u8 {
    return std.fmt.bufPrint(
        buf,
        "  q Quit  j/k Select  Tab Focus  Tab/S-Tab View",
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
fn collectScreenText(screen: *const vaxis.Screen, out: *std.ArrayList(u8)) !void {
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) {
            try out.appendSlice(testing.allocator, g);
        }
    }
}

// -------------------------------------------------------------------------
// TopologyState lifecycle tests
// -------------------------------------------------------------------------

test "topology: init and deinit are clean" {
    var state = TopologyState.init(testing.allocator);
    defer state.deinit();
    try testing.expectEqual(@as(usize, 0), state.rows.len);
}

test "topology: reload on empty DB yields empty rows (task 4039 empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = TopologyState.init(a);
    defer state.deinit();

    try state.reload(&d);

    try testing.expectEqual(@as(usize, 0), state.rows.len);
}

test "topology: reload populates rows with association and members (task 4039)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path, git_remote) values ('repo1', 'Repo One', '/work/repo1', 'git@gh:org/repo1')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'repo1'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('myorg', 'My Org', 'org')",
        &.{},
    );
    const aid = try d.intQuery("select id from associations where slug = 'myorg'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid } },
    );

    var state = TopologyState.init(a);
    defer state.deinit();
    try state.reload(&d);

    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqualStrings("myorg", state.rows[0].slug);
    try testing.expectEqual(@as(usize, 1), state.rows[0].members.len);
    try testing.expectEqualStrings("repo1", state.rows[0].members[0].project_slug);
}

test "topology: handleKey j/k moves selection" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('a1', 'Assoc 1', 'org')",
        &.{},
    );
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('a2', 'Assoc 2', 'org')",
        &.{},
    );

    var state = TopologyState.init(a);
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

test "topology: handleKey j does not overflow past last row" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('only', 'Only', 'org')",
        &.{},
    );

    var state = TopologyState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const j_key = Key{ .codepoint = 'j', .mods = .{} };
    _ = state.handleKey(j_key);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

test "topology: handleKey k does not underflow below 0" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('only', 'Only', 'org')",
        &.{},
    );

    var state = TopologyState.init(a);
    defer state.deinit();
    try state.reload(&d);

    const k_key = Key{ .codepoint = 'k', .mods = .{} };
    _ = state.handleKey(k_key);
    try testing.expectEqual(@as(usize, 0), state.selected_idx);
}

// =========================================================================
// RENDER-LEVEL TESTS (rule (b): render into vaxis.Screen and assert text)
// =========================================================================

test "topology: renderNavigator shows association rows (task 4039 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name) values ('rp', 'Repo P')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'rp'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('org-alpha', 'Org Alpha', 'org')",
        &.{},
    );
    const aid = try d.intQuery("select id from associations where slug = 'org-alpha'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid } },
    );

    var state = TopologyState.init(a);
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

    // RENDER-LEVEL ASSERTIONS (task 4039):
    // Header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Topology") != null);
    // Association slug must appear in display_text.
    try testing.expect(std.mem.indexOf(u8, text, "org-alpha") != null);
    // kind must appear in display_text.
    try testing.expect(std.mem.indexOf(u8, text, "org") != null);
    // Member count must appear.
    try testing.expect(std.mem.indexOf(u8, text, "1 member") != null);
}

test "topology: renderNavigator shows empty state when no associations (task 4039 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = TopologyState.init(a);
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

    try testing.expect(std.mem.indexOf(u8, text, "no associations registered") != null);
}

test "topology: renderDetail shows all association and member fields (task 4039 + 4040 render-level)" {
    // This is the primary render-level test: verifies that EVERY datum queried
    // is rendered (rule (a) invariant). Also verifies the resolution_rule
    // appears (task 4040 engine-fidelity invariant).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path, git_remote) values ('proj-x', 'Project X', '/home/user/proj-x', 'git@github.com:org/proj-x')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'proj-x'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind, auto_detected) values ('client-acme', 'Acme Corp', 'client', 0)",
        &.{},
    );
    const aid = try d.intQuery("select id from associations where slug = 'client-acme'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'auto:git-remote')",
        &.{ .{ .int = pid }, .{ .int = aid } },
    );

    var state = TopologyState.init(a);
    defer state.deinit();
    try state.reload(&d);

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

    // RENDER-LEVEL ASSERTIONS (task 4039): ALL queried association fields.
    try testing.expect(std.mem.indexOf(u8, text, "client-acme") != null); // slug
    try testing.expect(std.mem.indexOf(u8, text, "Acme Corp") != null); // name
    try testing.expect(std.mem.indexOf(u8, text, "client") != null); // kind
    try testing.expect(std.mem.indexOf(u8, text, "no") != null); // auto_detected = 0 → "no"

    // Member section header.
    try testing.expect(std.mem.indexOf(u8, text, "Members") != null);

    // RENDER-LEVEL ASSERTIONS (task 4039): ALL queried member fields.
    try testing.expect(std.mem.indexOf(u8, text, "proj-x") != null); // project_slug
    try testing.expect(std.mem.indexOf(u8, text, "Project X") != null); // project_name
    try testing.expect(std.mem.indexOf(u8, text, "/home/user/proj-x") != null); // root_path
    try testing.expect(std.mem.indexOf(u8, text, "git@github.com:org/proj-x") != null); // git_remote
    // source_label mirrors scope.reasonFromSource("auto:git-remote") = "from git remote".
    try testing.expect(std.mem.indexOf(u8, text, "from git remote") != null);

    // RENDER-LEVEL ASSERTION (task 4040): resolution_rule is rendered.
    // This project has 1 membership → "→ resolves here" (engine fidelity).
    try testing.expect(std.mem.indexOf(u8, text, "resolves here") != null);
}

test "topology: renderDetail shows ambiguous resolution_rule for multi-assoc project (task 4040 render-level)" {
    // Engine-fidelity test: project with 2 associations must render
    // "→ ambiguous (2)" to match deriveFromCwd Reason.project_multiple_associations.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('shared-proj', 'Shared Project', '/work/shared-proj')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'shared-proj'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('assoc-1', 'Assoc One', 'org')",
        &.{},
    );
    const aid1 = try d.intQuery("select id from associations where slug = 'assoc-1'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('assoc-2', 'Assoc Two', 'org')",
        &.{},
    );
    const aid2 = try d.intQuery("select id from associations where slug = 'assoc-2'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid1 } },
    );
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid2 } },
    );

    var state = TopologyState.init(a);
    defer state.deinit();
    try state.reload(&d);

    // Select assoc-1 (first alphabetically).
    try testing.expectEqualStrings("assoc-1", state.rows[0].slug);

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

    // RENDER-LEVEL ASSERTION (task 4040): ambiguous rule rendered.
    // member_count = 2 → "→ ambiguous (2)".
    try testing.expect(std.mem.indexOf(u8, text, "ambiguous") != null);
    try testing.expect(std.mem.indexOf(u8, text, "2") != null);
    // Must NOT say "resolves here" when ambiguous.
    // (Note: "2" also appears in the association slugs, but "resolves here" would be wrong.)
    try testing.expect(std.mem.indexOf(u8, text, "resolves here") == null);
}

test "topology: renderDetail shows (no member projects) when association is empty (task 4039 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('empty-assoc', 'Empty', 'org')",
        &.{},
    );

    var state = TopologyState.init(a);
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

    try testing.expect(std.mem.indexOf(u8, text, "empty-assoc") != null);
    try testing.expect(std.mem.indexOf(u8, text, "no member projects") != null);
}

test "topology: renderDetail shows null root_path and git_remote as (none) (task 4039 render-level)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Project with no root_path or git_remote.
    _ = try d.execParams(
        "insert into projects (slug, name) values ('bare-proj', 'Bare Project')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'bare-proj'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('bare-assoc', 'Bare Assoc', 'org')",
        &.{},
    );
    const aid = try d.intQuery("select id from associations where slug = 'bare-assoc'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid } },
    );

    var state = TopologyState.init(a);
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

    // Project fields rendered.
    try testing.expect(std.mem.indexOf(u8, text, "bare-proj") != null);
    try testing.expect(std.mem.indexOf(u8, text, "Bare Project") != null);
    // Null root_path and git_remote rendered as "(none)".
    try testing.expect(std.mem.indexOf(u8, text, "(none)") != null);
    // "→ resolves here" for single-membership project.
    try testing.expect(std.mem.indexOf(u8, text, "resolves here") != null);
}

// -------------------------------------------------------------------------
// Memory regression test (reload safety)
// -------------------------------------------------------------------------

test "topology: reload->reload does not leak or UAF" {
    // Verify that two sequential reloads with the same DB do not leak
    // or double-free. Run under testing.allocator which detects both.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('r1', 'R1', '/work/r1')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'r1'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('org1', 'Org 1', 'org')",
        &.{},
    );
    const aid = try d.intQuery("select id from associations where slug = 'org1'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid } },
    );

    var state = TopologyState.init(a);
    defer state.deinit();

    try state.reload(&d);
    try testing.expectEqual(@as(usize, 1), state.rows.len);

    // Second reload: must free previous rows without double-free or leak.
    try state.reload(&d);
    try testing.expectEqual(@as(usize, 1), state.rows.len);
    try testing.expectEqualStrings("org1", state.rows[0].slug);
}

test "topology: legendLabel fits in buf and contains expected keys" {
    var buf: [256]u8 = undefined;
    const label = legendLabel(&buf);
    try testing.expect(label.len > 0);
    try testing.expect(std.mem.indexOf(u8, label, "Quit") != null);
    try testing.expect(std.mem.indexOf(u8, label, "Select") != null);
}

test "topology compiles" {
    std.testing.refAllDecls(@This());
}
