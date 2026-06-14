//! cockpit/views/scope_explorer.zig — Scope Explorer view (M3).
//!
//! The default landing view for the cockpit. Renders a collapsible
//! association → plan → subplan → task tree in the navigator pane, with
//! the selected node's content in the split detail pane.
//!
//! Tasks 3966 (plan-tree), 3967 (scope-filter), 3968 (task-drill),
//! 3969 (split-detail), 3970 (collapse-expand).
//!
//! Design invariants:
//!   - Pure view: reads from DB via view_model.zig; no writes.
//!   - All heap-owned data is owned by the ExplorerState and released
//!     via `deinit`.
//!   - The navigator pane is driven by the tree_navigator widget; the
//!     detail pane by the markdown_detail widget.
//!   - Scope filter: cwd-derived on launch; 'a' key toggles all-scopes.
//!     Falls back to all-scopes when launched outside a registered repo.
//!   - Collapse/expand: Enter on a plan node collapses/expands its
//!     children. j/k or arrow keys move the selection.

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");
const tree_nav = @import("../widgets/tree_navigator.zig");
const markdown_detail = @import("../widgets/markdown_detail.zig");
const split_layout = @import("../widgets/split_layout.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// ExplorerNode: flat list node for the tree-navigator
// =========================================================================

/// The kind of a node in the explorer's flat tree.
pub const NodeKind = enum {
    plan,
    task,
    decision,
    question,
    scenario,
    artifact,
};

/// One node in the Scope Explorer flat tree (navigator pane).
pub const ExplorerNode = struct {
    kind: NodeKind,
    /// The database entity id.
    entity_id: i64,
    /// Parent plan id (for determining sub-item visibility).
    parent_plan_id: ?i64,
    /// Display label. Owned by the ExplorerState arena.
    label: []const u8,
    /// Status badge from view_model.
    badge: view_model.StatusBadge,
    /// Depth in the tree (0 = top-level plan).
    depth: u32,
    /// Whether this node is collapsed (only applies to plan nodes
    /// that have children).
    collapsed: bool,
    /// Whether this node is currently hidden (parent collapsed).
    hidden: bool,
    /// Summary count for plan nodes (task_count).
    count_badge: ?u32,
};

// =========================================================================
// ExplorerState
// =========================================================================

/// All mutable state for the Scope Explorer view. Lives in the caller
/// (app.zig) and is passed by pointer to render/handleKey.
pub const ExplorerState = struct {
    allocator: std.mem.Allocator,

    /// Flat list of visible nodes, rebuilt on DB-changed or scope toggle.
    nodes: []ExplorerNode = &.{},

    /// Navigator selection/scroll state.
    nav: tree_nav.Navigator = .{},

    /// Current scope filter.
    filter: view_model.ScopeFilter = .all,

    /// True when filter was explicitly set to cwd-derived (vs. fallen
    /// back to .all).
    filter_is_cwd: bool = false,

    /// Cached detail pane content for the currently selected node.
    detail: ?view_model.DetailPane = null,

    /// Labels buffer. All node label slices point into allocations from
    /// this allocator (same as ExplorerState.allocator).
    pub fn init(allocator: std.mem.Allocator) ExplorerState {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *ExplorerState) void {
        self.freeNodes();
        if (self.detail) |d| d.deinit(self.allocator);
    }

    fn freeNodes(self: *ExplorerState) void {
        for (self.nodes) |n| self.allocator.free(n.label);
        self.allocator.free(self.nodes);
        self.nodes = &.{};
    }

    /// Reload all nodes from the DB. Called on db_changed and on launch.
    /// Resets nav selection if the node count changes.
    pub fn reload(
        self: *ExplorerState,
        d: *db.sqlite.Db,
    ) !void {
        // Free old data.
        self.freeNodes();
        if (self.detail) |det| det.deinit(self.allocator);
        self.detail = null;

        // Build the new flat node list.
        var new_nodes: std.ArrayList(ExplorerNode) = .empty;
        errdefer {
            for (new_nodes.items) |n| self.allocator.free(n.label);
            new_nodes.deinit(self.allocator);
        }

        try self.buildPlanTree(d, &new_nodes);

        const prev_count = self.nodes.len;
        self.nodes = try new_nodes.toOwnedSlice(self.allocator);

        // Reset selection when the list shrinks below current selection.
        if (self.nodes.len != prev_count) {
            if (self.nav.selected_idx >= self.nodes.len) {
                self.nav.selected_idx = if (self.nodes.len > 0) self.nodes.len - 1 else 0;
            }
        }

        // Update detail pane for the selected node.
        try self.refreshDetail(d);
    }

    /// Build the flat node list from plans (filtered) and their drill children.
    /// Plan nodes that are collapsed hide their children. Uses
    /// queryPlanNodesFiltered for the plan tier and queryPlanDrillRows for the
    /// task/decision/question/scenario/artifact tier under each plan.
    fn buildPlanTree(
        self: *ExplorerState,
        d: *db.sqlite.Db,
        out: *std.ArrayList(ExplorerNode),
    ) !void {
        // Fetch filtered plan nodes from the view-model (includes parent_plan_id).
        const plan_rows = try view_model.queryPlanNodesFiltered(
            d,
            self.allocator,
            self.filter,
        );
        defer {
            for (plan_rows) |p| p.deinit(self.allocator);
            self.allocator.free(plan_rows);
        }

        // Compute depth for each plan using a parent-id → depth map.
        // queryPlanNodesFiltered orders rows by coalesce(parent_plan_id, id)
        // so parents appear before their children — depth is resolved in one pass.
        var id_to_depth = std.AutoHashMap(i64, u32).init(self.allocator);
        defer id_to_depth.deinit();

        for (plan_rows) |p| {
            const depth: u32 = if (p.parent_plan_id) |ppid|
                (id_to_depth.get(ppid) orelse 0) + 1
            else
                0;
            try id_to_depth.put(p.id, depth);

            // Preserve collapsed state across reloads.
            var was_collapsed = false;
            for (self.nodes) |old| {
                if (old.kind == .plan and old.entity_id == p.id) {
                    was_collapsed = old.collapsed;
                    break;
                }
            }

            const label = try self.allocator.dupe(u8, p.title);
            errdefer self.allocator.free(label);

            try out.append(self.allocator, .{
                .kind = .plan,
                .entity_id = p.id,
                .parent_plan_id = p.parent_plan_id,
                .label = label,
                .badge = p.status_badge,
                .depth = depth,
                .collapsed = was_collapsed,
                .hidden = false, // computed in second pass below
                .count_badge = if (p.task_count > 0) p.task_count else null,
            });

            // Append drill children (tasks, decisions, questions, scenarios,
            // artifacts) under this plan node at depth+1. Children are fetched
            // unconditionally; the hidden flag in the second pass will gate
            // visibility when the plan is collapsed.
            const drill_rows = try view_model.queryPlanDrillRows(d, self.allocator, p.id);
            defer view_model.DrillRow.deinitMany(drill_rows, self.allocator);

            for (drill_rows) |dr| {
                const child_kind: NodeKind = switch (dr.kind) {
                    .task => .task,
                    .decision => .decision,
                    .question => .question,
                    .scenario => .scenario,
                    .artifact => .artifact,
                };
                const child_label = try self.allocator.dupe(u8, dr.title);
                errdefer self.allocator.free(child_label);

                try out.append(self.allocator, .{
                    .kind = child_kind,
                    .entity_id = dr.id,
                    // parent_plan_id for drill children is the plan they belong
                    // to; used by the hidden-flag pass below.
                    .parent_plan_id = p.id,
                    .label = child_label,
                    .badge = dr.status_badge,
                    .depth = depth + 1,
                    .collapsed = false,
                    .hidden = false, // computed in second pass below
                    .count_badge = null,
                });
            }
        }

        // Second pass: mark nodes as hidden when their parent plan is collapsed
        // or when the plan itself is hidden (ancestor collapsed). This applies
        // to both subplan nodes (kind == .plan with a parent_plan_id) and drill
        // child nodes (kind != .plan with parent_plan_id = their plan's id).
        var collapsed_set = std.AutoHashMap(i64, void).init(self.allocator);
        defer collapsed_set.deinit();

        for (out.items) |*node| {
            if (node.kind == .plan) {
                if (node.collapsed) {
                    try collapsed_set.put(node.entity_id, {});
                }
                if (node.parent_plan_id) |ppid| {
                    if (collapsed_set.contains(ppid)) {
                        node.hidden = true;
                        // A hidden plan also collapses its children implicitly;
                        // add it to the collapsed set so its children are hidden.
                        try collapsed_set.put(node.entity_id, {});
                    }
                }
            } else {
                // Drill children: hidden when their parent plan is in the
                // collapsed set (i.e. collapsed or itself hidden).
                if (node.parent_plan_id) |ppid| {
                    if (collapsed_set.contains(ppid)) {
                        node.hidden = true;
                    }
                }
            }
        }
    }

    /// Count visible nodes (non-hidden).
    pub fn visibleCount(self: *const ExplorerState) usize {
        var count: usize = 0;
        for (self.nodes) |n| {
            if (!n.hidden) count += 1;
        }
        return count;
    }

    /// Refresh the detail pane for the currently selected visible node.
    pub fn refreshDetail(
        self: *ExplorerState,
        d: *db.sqlite.Db,
    ) !void {
        if (self.detail) |det| {
            det.deinit(self.allocator);
            self.detail = null;
        }

        const sel = self.selectedNode() orelse {
            self.detail = try view_model.DetailPane.empty(self.allocator);
            return;
        };

        self.detail = switch (sel.kind) {
            .plan => try view_model.queryPlanDetail(d, self.allocator, sel.entity_id),
            .task => try view_model.queryTaskDetail(d, self.allocator, sel.entity_id),
            .decision => try view_model.queryDecisionDetail(d, self.allocator, sel.entity_id),
            .question => try view_model.queryQuestionDetail(d, self.allocator, sel.entity_id),
            .scenario => try view_model.queryScenarioDetail(d, self.allocator, sel.entity_id),
            .artifact => try view_model.queryArtifactDetail(d, self.allocator, sel.entity_id),
        };
    }

    /// The currently selected visible node, or null if empty.
    pub fn selectedNode(self: *const ExplorerState) ?*const ExplorerNode {
        var visible: usize = 0;
        for (self.nodes) |*n| {
            if (n.hidden) continue;
            if (visible == self.nav.selected_idx) return n;
            visible += 1;
        }
        return null;
    }

    /// Handle a key event for the Scope Explorer. Returns true when the
    /// key was consumed.
    pub fn handleKey(
        self: *ExplorerState,
        key: Key,
        d: *db.sqlite.Db,
    ) bool {
        const visible = self.visibleCount();

        // Movement: j / arrow-down / k / arrow-up.
        if (key.matches('j', .{}) or key.matches(Key.down, .{})) {
            self.nav.moveDown(visible);
            self.nav.ensureVisible(30); // generous viewport estimate
            self.refreshDetail(d) catch {};
            return true;
        }
        if (key.matches('k', .{}) or key.matches(Key.up, .{})) {
            self.nav.moveUp();
            self.nav.ensureVisible(30);
            self.refreshDetail(d) catch {};
            return true;
        }

        // Collapse/expand: Enter on a plan node.
        if (key.matches(Key.enter, .{})) {
            if (self.selectedNode()) |sel| {
                if (sel.kind == .plan) {
                    // Find the mutable version and toggle.
                    self.toggleCollapse(sel.entity_id);
                    self.refreshDetail(d) catch {};
                    return true;
                }
            }
            return false;
        }

        // Selecting a non-plan node (task, decision, question, scenario,
        // artifact) automatically updates the detail pane via refreshDetail;
        // no separate drill key action is needed.

        // Scope toggle: 'a' = all scopes.
        if (key.matches('a', .{})) {
            self.filter = .all;
            self.filter_is_cwd = false;
            self.reload(d) catch {};
            return true;
        }

        return false;
    }

    /// Toggle the collapsed state of the plan node with `plan_id`.
    fn toggleCollapse(self: *ExplorerState, plan_id: i64) void {
        for (self.nodes) |*n| {
            if (n.kind == .plan and n.entity_id == plan_id) {
                n.collapsed = !n.collapsed;
                break;
            }
        }
        // Recompute hidden flags for all node kinds. Drill children (kind !=
        // .plan) are hidden when their parent_plan_id is in the collapsed set.
        var collapsed_set = std.AutoHashMap(i64, void).init(self.allocator);
        defer collapsed_set.deinit();
        for (self.nodes) |*n| {
            if (n.kind == .plan) {
                if (n.collapsed) collapsed_set.put(n.entity_id, {}) catch {};
                if (n.parent_plan_id) |ppid| {
                    n.hidden = collapsed_set.contains(ppid);
                    // A hidden plan's subtree must also be collapsed so its
                    // children (subplans and drill rows) are hidden.
                    if (n.hidden) collapsed_set.put(n.entity_id, {}) catch {};
                } else {
                    n.hidden = false;
                }
            } else {
                // Drill children: hide when their parent plan is collapsed.
                if (n.parent_plan_id) |ppid| {
                    n.hidden = collapsed_set.contains(ppid);
                } else {
                    n.hidden = false;
                }
            }
        }
    }
};

// =========================================================================
// Render
// =========================================================================

/// Build a TreeNode slice from ExplorerState.nodes for the tree-navigator
/// widget. The returned slice is allocated; caller frees it (but NOT the
/// label slices — those are owned by ExplorerState).
pub fn buildTreeNodes(
    state: *const ExplorerState,
    allocator: std.mem.Allocator,
) ![]tree_nav.TreeNode {
    var out: std.ArrayList(tree_nav.TreeNode) = .empty;
    defer out.deinit(allocator);

    for (state.nodes) |*n| {
        try out.append(allocator, .{
            .label = n.label,
            .depth = n.depth,
            .badge = n.badge,
            .count_badge = n.count_badge,
            .collapsed = n.collapsed,
            .hidden = n.hidden,
        });
    }
    return try out.toOwnedSlice(allocator);
}

/// Render the Scope Explorer into the navigator and detail windows.
pub fn render(
    state: *const ExplorerState,
    nav_win: Window,
    detail_win: Window,
    allocator: std.mem.Allocator,
) !void {
    // ---- Navigator pane ------------------------------------------------
    {
        const tree_nodes = try buildTreeNodes(state, allocator);
        defer allocator.free(tree_nodes);

        tree_nav.render(nav_win, tree_nodes, &state.nav);
    }

    // ---- Header: scope indicator (top row of nav) ----------------------
    // Draw scope label in dim style above the tree (row 0 of nav_win is
    // used by the tree; we'd need to shift — skip for now and show scope
    // label in the legend bar managed by app.zig).

    // ---- Detail pane ---------------------------------------------------
    if (state.detail) |det| {
        // Title row.
        if (det.title.len > 0 and detail_win.height > 0) {
            const title_style: Style = .{ .bold = true };
            var col: usize = 0;
            for (det.title) |byte| {
                if (col >= detail_win.width) break;
                if (byte & 0x80 == 0) {
                    const ch: [1]u8 = .{byte};
                    detail_win.writeCell(@intCast(col), 0, .{
                        .char = .{ .grapheme = &ch, .width = 1 },
                        .style = title_style,
                    });
                    col += 1;
                }
            }
        }

        // Body (markdown) starting at row 1.
        if (detail_win.height > 1) {
            const body_win = detail_win.child(.{
                .x_off = 0,
                .y_off = 1,
                .width = detail_win.width,
                .height = detail_win.height - 1,
            });
            try markdown_detail.render(body_win, allocator, det.body);
        }
    } else {
        // Nothing selected.
        _ = detail_win.printSegment(.{
            .text = "(no selection)",
            .style = .{ .dim = true },
        }, .{ .row_offset = 0, .col_offset = 0 });
    }
}

/// Return a one-line scope indicator string for the key legend bar.
/// Does not allocate — writes into buf and returns the used slice.
pub fn scopeLabel(state: *const ExplorerState, buf: []u8) []const u8 {
    return switch (state.filter) {
        .all => std.fmt.bufPrint(buf, "all-scopes", .{}) catch "all-scopes",
        .repo => std.fmt.bufPrint(buf, "cwd-scope", .{}) catch "cwd-scope",
    };
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

test "scope_explorer: ExplorerState init and deinit are clean" {
    var state = ExplorerState.init(testing.allocator);
    defer state.deinit();
    try testing.expectEqual(@as(usize, 0), state.nodes.len);
}

test "scope_explorer: reload on empty DB produces zero nodes" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);
    try testing.expectEqual(@as(usize, 0), state.nodes.len);
    try testing.expectEqual(@as(usize, 0), state.visibleCount());
}

test "scope_explorer: reload with plans populates nodes" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Slug uniqueness required per the schema unique indexes.
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Plan A','pa-x','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Plan B','pb-x','draft')",
        &.{},
    );

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);
    try testing.expectEqual(@as(usize, 2), state.nodes.len);
    try testing.expectEqual(@as(usize, 2), state.visibleCount());
    try testing.expectEqual(NodeKind.plan, state.nodes[0].kind);
}

test "scope_explorer: collapse/expand hides children" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const parent_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Parent','par','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global','Child','chi','draft',?)",
        &.{.{ .int = parent_id }},
    );

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);
    // Initially both visible.
    try testing.expectEqual(@as(usize, 2), state.visibleCount());

    // Collapse the parent.
    state.toggleCollapse(parent_id);
    // Child should now be hidden.
    try testing.expectEqual(@as(usize, 1), state.visibleCount());

    // Expand again.
    state.toggleCollapse(parent_id);
    try testing.expectEqual(@as(usize, 2), state.visibleCount());
}

test "scope_explorer: selectedNode returns first node at idx=0" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Only Plan','op','active')",
        &.{},
    );

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);
    const sel = state.selectedNode();
    try testing.expect(sel != null);
    try testing.expectEqual(NodeKind.plan, sel.?.kind);
}

test "scope_explorer: buildTreeNodes produces matching count" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','P1','p1','active')",
        &.{},
    );

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);

    const nodes = try buildTreeNodes(&state, a);
    defer a.free(nodes);
    try testing.expectEqual(state.nodes.len, nodes.len);
}

test "scope_explorer: scopeLabel returns all-scopes for filter=.all" {
    var state = ExplorerState.init(testing.allocator);
    defer state.deinit();
    state.filter = .all;
    var buf: [64]u8 = undefined;
    const label = scopeLabel(&state, &buf);
    try testing.expectEqualStrings("all-scopes", label);
}

test "scope_explorer: scopeLabel returns cwd-scope for filter=.repo" {
    var state = ExplorerState.init(testing.allocator);
    defer state.deinit();
    state.filter = .{ .repo = 1 };
    var buf: [64]u8 = undefined;
    const label = scopeLabel(&state, &buf);
    try testing.expectEqualStrings("cwd-scope", label);
}

// -------------------------------------------------------------------------
// Task 3966/3968/3969 tests: drill children, collapse, and detail pane.
// -------------------------------------------------------------------------

/// Seed a plan with one task, one linked decision, one linked question, one
/// linked test_scenario, and one linked artifact. Returns the plan_id.
fn seedPlanWithDrillData(d: *db.sqlite.Db) !i64 {
    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','DrillPlan','drill-plan','active')",
        &.{},
    );
    // Task directly on the plan.
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 'DrillTask','doing')",
        &.{.{ .int = plan_id }},
    );
    // Decision linked via entity_links.
    const dec_id = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','DrillDec','Dec body','accepted')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('plan', ?, 'decision', ?, 'derives-from')",
        &.{ .{ .int = plan_id }, .{ .int = dec_id } },
    );
    // Question linked via entity_links.
    const q_id = try d.execParams(
        "insert into questions (scope_kind, title, status) values ('global','DrillQ','open')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('plan', ?, 'question', ?, 'addresses')",
        &.{ .{ .int = plan_id }, .{ .int = q_id } },
    );
    // Test scenario linked via entity_links.
    const sc_id = try d.execParams(
        "insert into test_scenarios (scope_kind, title, status) values ('global','DrillScenario','verified')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('plan', ?, 'test_scenario', ?, 'verifies')",
        &.{ .{ .int = plan_id }, .{ .int = sc_id } },
    );
    // Artifact linked via entity_links.
    const art_id = try d.execParams(
        "insert into artifacts (scope_kind, kind, title, body) values ('global','tech_spec','DrillArtifact','Artifact body')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('plan', ?, 'artifact', ?, 'cites')",
        &.{ .{ .int = plan_id }, .{ .int = art_id } },
    );
    return plan_id;
}

test "scope_explorer: drill children appear under a plan with seeded entities" {
    // (a) Asserts task 3966/3968: queryPlanDrillRows is wired into buildPlanTree.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try seedPlanWithDrillData(&d);

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);

    // Expect 1 plan + 5 drill children (task + decision + question + scenario + artifact).
    try testing.expectEqual(@as(usize, 6), state.nodes.len);
    try testing.expectEqual(@as(usize, 6), state.visibleCount());

    // First node must be the plan.
    try testing.expectEqual(NodeKind.plan, state.nodes[0].kind);

    // Remaining nodes must be child kinds.
    var found_task = false;
    var found_decision = false;
    var found_question = false;
    var found_scenario = false;
    var found_artifact = false;
    for (state.nodes[1..]) |n| {
        switch (n.kind) {
            .task => found_task = true,
            .decision => found_decision = true,
            .question => found_question = true,
            .scenario => found_scenario = true,
            .artifact => found_artifact = true,
            .plan => {},
        }
        // All drill children must have depth > 0.
        try testing.expect(n.depth > 0);
        // All drill children must have parent_plan_id set.
        try testing.expect(n.parent_plan_id != null);
    }
    try testing.expect(found_task);
    try testing.expect(found_decision);
    try testing.expect(found_question);
    try testing.expect(found_scenario);
    try testing.expect(found_artifact);
}

test "scope_explorer: collapsing a plan hides its drill children" {
    // (b) Asserts that collapse still works after drill children are wired in.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlanWithDrillData(&d);

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);
    // 1 plan + 5 drill children all visible.
    try testing.expectEqual(@as(usize, 6), state.visibleCount());

    // Collapse the plan.
    state.toggleCollapse(plan_id);
    // Only the plan itself should be visible; all 5 drill children hidden.
    try testing.expectEqual(@as(usize, 1), state.visibleCount());

    // Expand again — all 6 back.
    state.toggleCollapse(plan_id);
    try testing.expectEqual(@as(usize, 6), state.visibleCount());
}

test "scope_explorer: selecting a non-plan node yields non-empty detail body" {
    // (c) Asserts task 3969: refreshDetail dispatches correctly for drill nodes.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try seedPlanWithDrillData(&d);

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);

    // Walk through all visible nodes. For each non-plan node, set the
    // selection to that position and assert the detail body is non-empty.
    var visible_idx: usize = 0;
    for (state.nodes) |n| {
        if (n.hidden) continue;
        if (n.kind != .plan) {
            state.nav.selected_idx = visible_idx;
            try state.refreshDetail(&d);
            try testing.expect(state.detail != null);
            // The detail body must mention the entity's content (non-empty body).
            try testing.expect(state.detail.?.body.len > 0);
        }
        visible_idx += 1;
    }
}

test "scope_explorer compiles" {
    std.testing.refAllDecls(@This());
}
