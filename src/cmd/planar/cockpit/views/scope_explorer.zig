//! cockpit/views/scope_explorer.zig — Scope Explorer view (M3 + M16 + M17).
//!
//! The default landing view for the cockpit. Renders a collapsible
//! association → plan → subplan → task tree in the navigator pane, with
//! the selected node's content in the split detail pane.
//!
//! Tasks 3966 (plan-tree), 3967 (scope-filter), 3968 (task-drill),
//! 3969 (split-detail), 3970 (collapse-expand).
//!
//! M16 edit additions (tasks 4044, 4045, 4046):
//!   - 'e' on any entity node enters inline title-edit mode.
//!   - The input buffer is displayed in the detail pane header.
//!   - On Enter: if the old title is non-empty, transitions to
//!     confirmation mode (task 4046). On confirm ('y'/'Y'), calls the
//!     edit action (task 4044). Escape cancels at any point.
//!   - All writes go through cockpit/edit/actions.editTitle which
//!     uses the engine write path + scope guard (task 4045).
//!
//! M17 task lifecycle additions (tasks 4047, 4048):
//!   - 'L' on a task node enters the lifecycle overlay.
//!   - The overlay first shows live claim state (task 4048).
//!   - If an active unexpired claim exists, the overlay shows a clear
//!     refusal ("CLAIM ACTIVE — STATUS CHANGE REFUSED") and blocks any
//!     status flip (the never-strand invariant).
//!   - If no active claim, Enter advances to the action menu where the
//!     operator can: start, done, reopen, block, adjust priority.
//!   - All status transitions route through engine.planning.task functions
//!     (markDone, reopen, update, markBlocked) — no raw SQL.
//!   - The lifecycle controller (TaskLifecycleState.handleKey) is
//!     unit-testable from testing.allocator (see task_lifecycle.zig).
//!
//! Design invariants:
//!   - All heap-owned data is owned by the ExplorerState and released
//!     via `deinit`.
//!   - The navigator pane is driven by the tree_navigator widget; the
//!     detail pane by the markdown_detail widget.
//!   - Scope filter: cwd-derived on launch; 'a' key toggles all-scopes.
//!     Falls back to all-scopes when launched outside a registered repo.
//!   - Collapse/expand: Enter on a plan node collapses/expands its
//!     children. j/k or arrow keys move the selection.
//!   - Edit mode: 'e' on any selected node begins inline editing.
//!     Escape aborts at any stage; Enter in edit mode transitions to
//!     confirm (if destructive) or commits directly (if non-destructive).

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const view_model = @import("../view_model.zig");
const tree_nav = @import("../widgets/tree_navigator.zig");
const markdown_detail = @import("../widgets/markdown_detail.zig");
const split_layout = @import("../widgets/split_layout.zig");
const edit_actions = @import("../edit/actions.zig");
const task_lifecycle = @import("../edit/task_lifecycle.zig");

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
// Edit mode state (M16, tasks 4044/4045/4046)
// =========================================================================

/// Maximum length of the inline edit input buffer. Entity titles are
/// bounded to 512 bytes in practice; 256 is a safe UI limit.
pub const EDIT_BUF_MAX = 256;

/// State machine for inline title editing in the Scope Explorer.
///
///   .none       — no edit in progress.
///   .typing     — user is typing a new title; `input_buf[0..input_len]`
///                 holds the current input. Pressing Enter advances to
///                 .confirming (if old title non-empty) or commits directly.
///                 Escape cancels.
///   .confirming — new title ready; old title was non-empty, so we ask
///                 the user to confirm before overwriting (task 4046).
///                 'y'/'Y' commits the write; any other key cancels.
///   .error_msg  — a write or scope-guard error; shown in the detail pane
///                 for one keystroke then reverts to .none.
pub const EditMode = union(enum) {
    none,
    typing: struct {
        kind: edit_actions.EntityKind,
        entity_id: i64,
        /// Current title before the edit (owned by edit mode; freed on exit).
        old_title: []const u8,
        /// Mutable input buffer. Fixed-capacity; content is input_buf[0..input_len].
        input_buf: [EDIT_BUF_MAX]u8,
        input_len: usize,
    },
    confirming: struct {
        kind: edit_actions.EntityKind,
        entity_id: i64,
        old_title: []const u8,
        /// The new title to write on confirm. Owned by edit mode.
        new_title: []const u8,
    },
    error_msg: struct {
        /// Error message string. Owned by edit mode allocator.
        msg: []const u8,
    },
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

    /// M16: inline edit mode (task 4044/4045/4046).
    edit_mode: EditMode = .none,

    /// M16: explicit scope override supplied by the operator. When non-null,
    /// this overrides the filter-derived write scope in the guard check.
    /// The cockpit does not yet expose a UI for setting this; it is set
    /// programmatically (e.g. from an explicit --scope flag on launch) or
    /// left null. When null and filter is .all, writes to scoped entities
    /// will be refused by the guard (task 4045).
    explicit_scope: ?[]const u8 = null,

    /// M17: task lifecycle overlay controller (tasks 4047, 4048).
    /// Active only when a task node is selected and the operator pressed 'L'.
    /// Deinit'd via lifecycle.deinit() in ExplorerState.deinit.
    lifecycle: task_lifecycle.TaskLifecycleState = undefined,
    lifecycle_inited: bool = false,

    /// Labels buffer. All node label slices point into allocations from
    /// this allocator (same as ExplorerState.allocator).
    pub fn init(allocator: std.mem.Allocator) ExplorerState {
        var s: ExplorerState = .{ .allocator = allocator };
        s.lifecycle = task_lifecycle.TaskLifecycleState.init(allocator);
        s.lifecycle_inited = true;
        return s;
    }

    pub fn deinit(self: *ExplorerState) void {
        self.freeNodes();
        if (self.detail) |d| d.deinit(self.allocator);
        self.clearEditMode();
        if (self.lifecycle_inited) self.lifecycle.deinit();
    }

    /// Free any heap-allocated strings owned by the current edit mode and
    /// reset to .none.
    fn clearEditMode(self: *ExplorerState) void {
        switch (self.edit_mode) {
            .none => {},
            .typing => |*t| self.allocator.free(t.old_title),
            .confirming => |*c| {
                self.allocator.free(c.old_title);
                self.allocator.free(c.new_title);
            },
            .error_msg => |*e| self.allocator.free(e.msg),
        }
        self.edit_mode = .none;
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

    /// Derive the write scope for use with lifecycle transitions (same
    /// logic as commitEdit uses for entity-field edits).
    fn lifecycleWriteScope(self: *ExplorerState, d: *db.sqlite.Db) ?[]const u8 {
        const vm_filter: edit_actions.view_model_ScopeFilter = switch (self.filter) {
            .repo => |pid| .{ .repo = pid },
            .all => .all,
        };
        return edit_actions.cockpitWriteScope(d, self.allocator, vm_filter, self.explicit_scope) catch null;
    }

    /// Handle a key event for the Scope Explorer. Returns true when the
    /// key was consumed.
    pub fn handleKey(
        self: *ExplorerState,
        key: Key,
        d: *db.sqlite.Db,
    ) bool {
        // ---- M17: lifecycle overlay keys take highest priority (task 4047/4048) ----
        if (self.lifecycle_inited and self.lifecycle.isActive()) {
            // Derive write scope for lifecycle transitions.
            const write_scope = self.lifecycleWriteScope(d);
            defer if (write_scope) |s| self.allocator.free(s);

            const consumed = self.lifecycle.handleKey(key, d, write_scope);
            if (consumed) {
                // After the overlay is dismissed, reload if needed.
                if (!self.lifecycle.isActive() and self.lifecycle.wantsReload()) {
                    self.reload(d) catch {};
                }
                return true;
            }
            return true; // lifecycle overlay always consumes all keys
        }

        // ---- M16: edit mode keys (task 4044/4045/4046) take priority ----
        switch (self.edit_mode) {
            .typing => |*t| {
                // Escape: cancel edit.
                if (key.matches(Key.escape, .{})) {
                    self.clearEditMode();
                    return true;
                }
                // Enter: attempt to commit (or transition to confirmation).
                if (key.matches(Key.enter, .{})) {
                    const new_title = t.input_buf[0..t.input_len];
                    if (new_title.len == 0) {
                        // Empty title — refuse silently; stay in typing mode.
                        return true;
                    }
                    // Task 4046: if old title is non-empty, require confirmation.
                    if (edit_actions.editIsDestructive(t.old_title)) {
                        // Transition to confirming state. Transfer ownership
                        // of old_title and new_title to the confirming variant.
                        const new_title_owned = self.allocator.dupe(u8, new_title) catch {
                            self.setErrorMsg("out of memory");
                            return true;
                        };
                        const confirming: EditMode = .{ .confirming = .{
                            .kind = t.kind,
                            .entity_id = t.entity_id,
                            .old_title = t.old_title,
                            .new_title = new_title_owned,
                        } };
                        // Do NOT free t.old_title here — ownership transferred.
                        self.edit_mode = confirming;
                        return true;
                    }
                    // Non-destructive: commit directly.
                    self.commitEdit(d, t.kind, t.entity_id, new_title);
                    return true;
                }
                // Backspace: remove last character.
                if (key.matches(Key.backspace, .{})) {
                    if (t.input_len > 0) t.input_len -= 1;
                    return true;
                }
                // Printable ASCII: append to buffer.
                if (key.text) |text| {
                    for (text) |byte| {
                        if (t.input_len < EDIT_BUF_MAX) {
                            t.input_buf[t.input_len] = byte;
                            t.input_len += 1;
                        }
                    }
                    return true;
                }
                return true;
            },
            .confirming => |c| {
                // Escape or anything except 'y'/'Y': cancel.
                if (key.matches(Key.escape, .{})) {
                    self.clearEditMode();
                    return true;
                }
                if (key.matches('y', .{}) or key.matches('Y', .{})) {
                    // Commit the write. Transfer fields before clearEditMode.
                    const kind = c.kind;
                    const eid = c.entity_id;
                    const new_title = c.new_title;
                    // Temporarily take ownership; commitEditOwned will free new_title.
                    self.commitEditOwned(d, kind, eid, new_title);
                    // old_title was already in confirming; clearEditMode would free it
                    // but commitEditOwned cleared the mode itself.
                    return true;
                }
                // Any other key = cancel.
                self.clearEditMode();
                return true;
            },
            .error_msg => {
                // Any key dismisses the error.
                self.clearEditMode();
                return true;
            },
            .none => {},
        }

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

        // M16: 'e' starts inline title editing on any selected entity node.
        if (key.matches('e', .{})) {
            if (self.selectedNode()) |sel| {
                const ek: edit_actions.EntityKind = switch (sel.kind) {
                    .plan => .plan,
                    .task => .task,
                    .question => .question,
                    .decision => .decision,
                    .scenario => .scenario,
                    .artifact => .artifact,
                };
                const old_title = edit_actions.fetchCurrentTitle(d, self.allocator, ek, sel.entity_id) catch {
                    self.setErrorMsg("could not fetch title");
                    return true;
                };
                var input_buf: [EDIT_BUF_MAX]u8 = undefined;
                // Pre-fill input with current title.
                const prefill_len = @min(old_title.len, EDIT_BUF_MAX);
                @memcpy(input_buf[0..prefill_len], old_title[0..prefill_len]);
                self.edit_mode = .{ .typing = .{
                    .kind = ek,
                    .entity_id = sel.entity_id,
                    .old_title = old_title,
                    .input_buf = input_buf,
                    .input_len = prefill_len,
                } };
                return true;
            }
            return false;
        }

        // M17: 'L' on a task node enters the lifecycle overlay (tasks 4047/4048).
        if (key.matches('L', .{})) {
            if (self.lifecycle_inited) {
                if (self.selectedNode()) |sel| {
                    if (sel.kind == .task) {
                        self.lifecycle.enter(d, sel.entity_id);
                        return true;
                    }
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

    /// Commit an edit where the new title is a borrowed slice (not yet owned).
    /// Used for the non-destructive direct-commit path.
    fn commitEdit(
        self: *ExplorerState,
        d: *db.sqlite.Db,
        kind: edit_actions.EntityKind,
        entity_id: i64,
        new_title: []const u8,
    ) void {
        // Derive the cockpit write scope from the current filter (task 4045).
        const vm_filter: edit_actions.view_model_ScopeFilter = switch (self.filter) {
            .repo => |pid| .{ .repo = pid },
            .all => .all,
        };
        const write_scope = edit_actions.cockpitWriteScope(
            d,
            self.allocator,
            vm_filter,
            self.explicit_scope,
        ) catch null;
        defer if (write_scope) |s| self.allocator.free(s);

        // Call the engine write path (task 4044).
        edit_actions.editTitle(d, self.allocator, kind, entity_id, new_title, write_scope) catch |e| {
            const msg = switch (e) {
                edit_actions.EditError.ScopeMismatch => "scope mismatch: entity scope differs from cockpit scope; set explicit_scope to override",
                edit_actions.EditError.EmptyValueNotAllowed => "title must not be empty",
                edit_actions.EditError.EntityNotFound => "entity not found",
                else => "write failed",
            };
            self.clearEditMode();
            self.setErrorMsg(msg);
            return;
        };

        self.clearEditMode();
        self.reload(d) catch {};
    }

    /// Commit an edit where `new_title` is already heap-allocated and owned
    /// by the caller. This function frees it (and old_title via clearEditMode).
    /// Used for the confirmation-path commit.
    fn commitEditOwned(
        self: *ExplorerState,
        d: *db.sqlite.Db,
        kind: edit_actions.EntityKind,
        entity_id: i64,
        new_title: []const u8,
    ) void {
        // We need to free new_title after use. Borrow it, then free.
        defer self.allocator.free(new_title);
        // Derive write scope.
        const vm_filter: edit_actions.view_model_ScopeFilter = switch (self.filter) {
            .repo => |pid| .{ .repo = pid },
            .all => .all,
        };
        const write_scope = edit_actions.cockpitWriteScope(
            d,
            self.allocator,
            vm_filter,
            self.explicit_scope,
        ) catch null;
        defer if (write_scope) |s| self.allocator.free(s);

        // Before calling the engine, we need to clear the edit mode so that
        // old_title (owned by confirming variant) is freed. We already
        // have new_title in a defer-freed local.
        // Free only old_title from confirming, then clear mode.
        switch (self.edit_mode) {
            .confirming => |*c| self.allocator.free(c.old_title),
            else => {},
        }
        self.edit_mode = .none;

        edit_actions.editTitle(d, self.allocator, kind, entity_id, new_title, write_scope) catch |e| {
            const msg = switch (e) {
                edit_actions.EditError.ScopeMismatch => "scope mismatch: entity scope differs from cockpit scope; set explicit_scope to override",
                edit_actions.EditError.EmptyValueNotAllowed => "title must not be empty",
                edit_actions.EditError.EntityNotFound => "entity not found",
                else => "write failed",
            };
            self.setErrorMsg(msg);
            return;
        };

        self.reload(d) catch {};
    }

    /// Set an error message in edit mode. Allocates a copy of `msg`.
    fn setErrorMsg(self: *ExplorerState, msg: []const u8) void {
        self.clearEditMode();
        const owned = self.allocator.dupe(u8, msg) catch return;
        self.edit_mode = .{ .error_msg = .{ .msg = owned } };
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

    // ---- M17: Lifecycle overlay in the detail pane (task 4047/4048) ------
    // When the lifecycle overlay is active, it takes over the detail pane.
    if (state.lifecycle_inited and state.lifecycle.isActive()) {
        try task_lifecycle.renderOverlay(&state.lifecycle, detail_win, allocator);
        return;
    }

    // ---- M16: Edit-mode overlay in the detail pane --------------------
    // When in edit/confirm/error mode, the detail pane shows the edit UI
    // instead of the normal entity content.
    switch (state.edit_mode) {
        .typing => |*t| {
            if (detail_win.height >= 1) {
                _ = detail_win.printSegment(.{
                    .text = "Edit title (Enter=commit, Esc=cancel):",
                    .style = .{ .bold = true },
                }, .{ .row_offset = 0, .col_offset = 0 });
            }
            if (detail_win.height >= 2) {
                // Render the editable line as two segments so that the input
                // text slice (which lives in the EditMode union on the heap)
                // is passed directly without being copied into a stack-local
                // format buffer. Stack-local buffers produce dangling grapheme
                // pointers in the Screen cell array after render() returns.
                _ = detail_win.print(&.{
                    .{ .text = "> ", .style = .{ .ul_style = .single } },
                    .{ .text = t.input_buf[0..t.input_len], .style = .{ .ul_style = .single } },
                }, .{ .row_offset = 1, .col_offset = 0 });
            }
            if (detail_win.height >= 3) {
                _ = detail_win.printSegment(.{
                    .text = "(old title will be overwritten on confirm)",
                    .style = .{ .dim = true },
                }, .{ .row_offset = 2, .col_offset = 0 });
            }
            return;
        },
        .confirming => |*c| {
            if (detail_win.height >= 1) {
                _ = detail_win.printSegment(.{
                    .text = "Overwrite existing title? [y/N]",
                    .style = .{ .bold = true },
                }, .{ .row_offset = 0, .col_offset = 0 });
            }
            if (detail_win.height >= 2) {
                // Use two-segment print so old_title (heap-owned) is passed
                // directly; avoids dangling grapheme pointers from a
                // stack-local format buffer.
                _ = detail_win.print(&.{
                    .{ .text = "  Old: ", .style = .{ .dim = true } },
                    .{ .text = c.old_title, .style = .{ .dim = true } },
                }, .{ .row_offset = 1, .col_offset = 0 });
            }
            if (detail_win.height >= 3) {
                _ = detail_win.print(&.{
                    .{ .text = "  New: ", .style = .{} },
                    .{ .text = c.new_title, .style = .{} },
                }, .{ .row_offset = 2, .col_offset = 0 });
            }
            if (detail_win.height >= 4) {
                _ = detail_win.printSegment(.{
                    .text = "Press y to confirm, any other key to cancel.",
                    .style = .{ .dim = true },
                }, .{ .row_offset = 3, .col_offset = 0 });
            }
            return;
        },
        .error_msg => |*e| {
            if (detail_win.height >= 1) {
                _ = detail_win.printSegment(.{
                    .text = "Error:",
                    .style = .{ .bold = true },
                }, .{ .row_offset = 0, .col_offset = 0 });
            }
            if (detail_win.height >= 2) {
                _ = detail_win.printSegment(.{
                    .text = e.msg,
                    .style = .{},
                }, .{ .row_offset = 1, .col_offset = 0 });
            }
            if (detail_win.height >= 3) {
                _ = detail_win.printSegment(.{
                    .text = "(press any key to dismiss)",
                    .style = .{ .dim = true },
                }, .{ .row_offset = 2, .col_offset = 0 });
            }
            return;
        },
        .none => {},
    }

    // ---- Normal detail pane (no edit mode) -----------------------------
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

/// Return the legend string for the key legend bar when the Explorer is active.
/// Shows lifecycle-mode hint when lifecycle overlay is active, otherwise edit-mode hint.
pub fn legendLabel(state: *const ExplorerState, buf: []u8) []const u8 {
    // M17: lifecycle overlay legend takes priority.
    if (state.lifecycle_inited and state.lifecycle.isActive()) {
        return task_lifecycle.legendLabel(&state.lifecycle, buf);
    }
    return switch (state.edit_mode) {
        .typing => std.fmt.bufPrint(buf, "  Typing title — Enter=commit  Esc=cancel", .{}) catch
            "  Typing title — Enter=commit  Esc=cancel",
        .confirming => std.fmt.bufPrint(buf, "  Confirm overwrite? y=yes  any=cancel", .{}) catch
            "  Confirm overwrite? y=yes  any=cancel",
        .error_msg => std.fmt.bufPrint(buf, "  Edit error — press any key to dismiss", .{}) catch
            "  Edit error — press any key to dismiss",
        .none => std.fmt.bufPrint(buf, "  q Quit  j/k Move  Enter Expand  e Edit  L Lifecycle  a All-scopes  Tab Focus", .{}) catch
            "  q Quit  j/k Move  Enter Expand  e Edit  L Lifecycle  a All-scopes  Tab Focus",
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

// =========================================================================
// M16 controller tests: handleKey edit path (tasks 4044/4045/4046)
// =========================================================================
//
// These tests drive the handleKey state machine with synthetic Key events
// under testing.allocator so the GPA leak detector exercises the EditMode
// ownership across typing→confirming→commit/cancel.

/// Helper: query the current title of a plan from the DB. Caller must free.
fn fetchPlanTitle(d: *db.sqlite.Db, plan_id: i64) ![]const u8 {
    var stmt = try d.prepare("select title from plans where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = plan_id }});
    switch (try stmt.step()) {
        .done => return error.NotFound,
        .row => return stmt.columnTextAlloc(0, testing.allocator),
    }
}

/// Helper: make a Key event for a printable ASCII character with `text` set.
fn keyChar(comptime ch: u8) Key {
    return .{
        .codepoint = ch,
        .text = &.{ch},
    };
}

/// Helper: make a Key event for a special (non-printable) key.
fn keySpecial(cp: u21) Key {
    return .{ .codepoint = cp };
}

test "scope_explorer handleKey: 'e' on selected plan node enters typing mode with title prefilled" {
    // (a) Pressing 'e' on a selected node enters typing mode with the title
    // pre-filled in the input buffer. GPA leak-detector validates ownership.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','My Plan','my-plan','active')",
        &.{},
    );

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);

    // Select the plan (idx 0 is the only visible node).
    state.nav.selected_idx = 0;

    const consumed = state.handleKey(keyChar('e'), &d);
    try testing.expect(consumed);

    // Must be in typing mode.
    switch (state.edit_mode) {
        .typing => |*t| {
            try testing.expectEqual(edit_actions.EntityKind.plan, t.kind);
            try testing.expectEqual(plan_id, t.entity_id);
            // old_title must match the DB value.
            try testing.expectEqualStrings("My Plan", t.old_title);
            // input_buf must be pre-filled with the current title.
            try testing.expectEqualStrings("My Plan", t.input_buf[0..t.input_len]);
        },
        else => {
            try testing.expect(false); // expected .typing
        },
    }
    // GPA will detect any leak on state.deinit() via defer above.
}

test "scope_explorer handleKey: Enter on non-empty old title transitions to .confirming without writing" {
    // (b) Enter while typing a new title for an entity with a non-empty old
    // title transitions to .confirming — NOT a direct commit. The DB must be
    // UNCHANGED at this point (no write before confirm — task 4046 contract).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Original Title','orig-title','active')",
        &.{},
    );

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);
    state.nav.selected_idx = 0;

    // Enter typing mode.
    _ = state.handleKey(keyChar('e'), &d);
    try testing.expect(state.edit_mode == .typing);

    // Clear the prefilled buffer and type a new title.
    // Backspace 14 times ("Original Title" = 14 chars), then type "New Title".
    for (0..14) |_| _ = state.handleKey(keySpecial(Key.backspace), &d);
    for ("New Title") |ch| _ = state.handleKey(.{ .codepoint = ch, .text = &.{ch} }, &d);

    // Press Enter — destructive (old title was non-empty), should go to confirming.
    _ = state.handleKey(keySpecial(Key.enter), &d);

    // MUST be in .confirming now.
    switch (state.edit_mode) {
        .confirming => |c| {
            try testing.expectEqualStrings("Original Title", c.old_title);
            try testing.expectEqualStrings("New Title", c.new_title);
        },
        else => {
            try testing.expect(false); // expected .confirming
        },
    }

    // DB must be UNCHANGED — no write before confirm.
    const title_now = try fetchPlanTitle(&d, plan_id);
    defer a.free(title_now);
    try testing.expectEqualStrings("Original Title", title_now);
}

test "scope_explorer handleKey: 'y' from .confirming commits and DB reflects new title" {
    // (c) Pressing 'y' from .confirming commits the write. The DB must reflect
    // the new title after. GPA validates the double-free risk in commitEditOwned.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Before Confirm','bef-confirm','active')",
        &.{},
    );

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);
    state.nav.selected_idx = 0;

    // Enter typing mode, clear buffer, type new title, press Enter.
    _ = state.handleKey(keyChar('e'), &d);
    for (0.."Before Confirm".len) |_| _ = state.handleKey(keySpecial(Key.backspace), &d);
    for ("After Confirm") |ch| _ = state.handleKey(.{ .codepoint = ch, .text = &.{ch} }, &d);
    _ = state.handleKey(keySpecial(Key.enter), &d);

    // Verify we're in .confirming.
    try testing.expect(state.edit_mode == .confirming);

    // Press 'y' to confirm.
    _ = state.handleKey(keyChar('y'), &d);

    // Must be back to .none.
    try testing.expectEqual(EditMode.none, state.edit_mode);

    // DB must now have the new title.
    const title_now = try fetchPlanTitle(&d, plan_id);
    defer a.free(title_now);
    try testing.expectEqualStrings("After Confirm", title_now);
}

test "scope_explorer handleKey: Escape from .confirming cancels — DB unchanged" {
    // (d-1) Pressing Escape from .confirming cancels the write. The DB must
    // remain unchanged. GPA validates no leak on the cancelled path.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Cancel Me','cancel-me','active')",
        &.{},
    );

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);
    state.nav.selected_idx = 0;

    _ = state.handleKey(keyChar('e'), &d);
    for (0.."Cancel Me".len) |_| _ = state.handleKey(keySpecial(Key.backspace), &d);
    for ("Should Not Land") |ch| _ = state.handleKey(.{ .codepoint = ch, .text = &.{ch} }, &d);
    _ = state.handleKey(keySpecial(Key.enter), &d);
    try testing.expect(state.edit_mode == .confirming);

    // Escape cancels.
    _ = state.handleKey(keySpecial(Key.escape), &d);
    try testing.expectEqual(EditMode.none, state.edit_mode);

    // DB must be unchanged.
    const title_now = try fetchPlanTitle(&d, plan_id);
    defer a.free(title_now);
    try testing.expectEqualStrings("Cancel Me", title_now);
}

test "scope_explorer handleKey: non-'y' key from .confirming cancels — DB unchanged" {
    // (d-2) Any key other than 'y'/'Y' from .confirming cancels. DB unchanged.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Stay Intact','stay-intact','active')",
        &.{},
    );

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);
    state.nav.selected_idx = 0;

    _ = state.handleKey(keyChar('e'), &d);
    for (0.."Stay Intact".len) |_| _ = state.handleKey(keySpecial(Key.backspace), &d);
    for ("Never Lands") |ch| _ = state.handleKey(.{ .codepoint = ch, .text = &.{ch} }, &d);
    _ = state.handleKey(keySpecial(Key.enter), &d);
    try testing.expect(state.edit_mode == .confirming);

    // Press 'n' (non-'y') — should cancel.
    _ = state.handleKey(keyChar('n'), &d);
    try testing.expectEqual(EditMode.none, state.edit_mode);

    // DB must be unchanged.
    const title_now = try fetchPlanTitle(&d, plan_id);
    defer a.free(title_now);
    try testing.expectEqualStrings("Stay Intact", title_now);
}

test "scope_explorer handleKey: ScopeMismatch error sets error_msg and performs no write" {
    // (e) A scoped entity with no matching write scope triggers ScopeMismatch.
    // The controller must set .error_msg mode and perform NO write.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Seed a repo-scoped plan.
    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('acme/core', 'Core')",
        &.{},
    );
    // Plans don't have scope_id column — use a task instead (tasks have scope_id).
    // Seed a global plan first (needed for state.reload to have a node).
    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Parent Plan','pp-scope-test','active')",
        &.{},
    );
    // Seed a repo-scoped task under the plan.
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, scope_id, plan_id, title, status, priority) values ('repo', ?, ?, 'Scoped Task', 'todo', 100)",
        &.{ .{ .int = proj_id }, .{ .int = plan_id } },
    );
    var state = ExplorerState.init(a);
    defer state.deinit();

    // Reload so the task node is in the list.
    try state.reload(&d);

    // Find the task node (it's a drill child under the plan).
    var task_node_idx: ?usize = null;
    var vis_idx: usize = 0;
    for (state.nodes) |n| {
        if (n.hidden) continue;
        if (n.kind == .task) {
            task_node_idx = vis_idx;
            break;
        }
        vis_idx += 1;
    }
    try testing.expect(task_node_idx != null);
    state.nav.selected_idx = task_node_idx.?;

    // explicit_scope = null, filter = .all → write_scope = null → ScopeMismatch.
    state.explicit_scope = null;
    state.filter = .all;

    // Press 'e' to enter typing mode.
    _ = state.handleKey(keyChar('e'), &d);
    try testing.expect(state.edit_mode == .typing);

    // Type a new title and press Enter.
    // (Old title "Scoped Task" is non-empty → goes to .confirming.)
    for (0.."Scoped Task".len) |_| _ = state.handleKey(keySpecial(Key.backspace), &d);
    for ("New Scoped") |ch| _ = state.handleKey(.{ .codepoint = ch, .text = &.{ch} }, &d);
    _ = state.handleKey(keySpecial(Key.enter), &d);
    try testing.expect(state.edit_mode == .confirming);

    // Confirm with 'y' — this will call commitEditOwned which hits ScopeMismatch.
    _ = state.handleKey(keyChar('y'), &d);

    // Must be in .error_msg, NOT .none.
    switch (state.edit_mode) {
        .error_msg => |e| {
            // Error message must mention "scope".
            try testing.expect(std.mem.indexOf(u8, e.msg, "scope") != null);
        },
        else => {
            try testing.expect(false); // expected .error_msg
        },
    }

    // DB must be unchanged — no write was performed.
    var stmt = try d.prepare("select title from tasks where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    try testing.expect((try stmt.step()) == .row);
    const title_now = try stmt.columnTextAlloc(0, a);
    defer a.free(title_now);
    try testing.expectEqualStrings("Scoped Task", title_now);
}

test "scope_explorer handleKey: Escape from .typing cancels — edit_mode reset and no leak" {
    // Extra: Escape from .typing must cleanly free old_title (GPA validates).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Typed Plan','typed-plan','active')",
        &.{},
    );

    var state = ExplorerState.init(a);
    defer state.deinit();

    try state.reload(&d);
    state.nav.selected_idx = 0;

    _ = state.handleKey(keyChar('e'), &d);
    try testing.expect(state.edit_mode == .typing);

    // Escape from typing must cancel cleanly.
    _ = state.handleKey(keySpecial(Key.escape), &d);
    try testing.expectEqual(EditMode.none, state.edit_mode);
    // GPA will detect any leak on defer state.deinit().
}

// =========================================================================
// M16 render-level tests: confirming overlay and error overlay text
// =========================================================================

/// Collect all non-empty grapheme text from a Screen's cell buffer.
fn collectScreenText(screen: *const vaxis.Screen, out: *std.ArrayList(u8)) !void {
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) {
            try out.appendSlice(testing.allocator, g);
        }
    }
}

test "scope_explorer render: .confirming overlay emits 'Overwrite existing title? [y/N]'" {
    // Render-level: the .confirming overlay must render the destructive-confirm
    // prompt text exactly as the code emits it.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = ExplorerState.init(a);
    defer state.deinit();

    // Set up confirming mode directly (no need to go through the full key flow).
    const old_title = try a.dupe(u8, "Old Plan");
    const new_title = try a.dupe(u8, "New Plan");
    state.edit_mode = .{ .confirming = .{
        .kind = .plan,
        .entity_id = 1,
        .old_title = old_title,
        .new_title = new_title,
    } };

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
        .width = 30,
        .height = win_h,
        .screen = &screen,
    };
    const detail_win: Window = .{
        .x_off = 30,
        .y_off = 0,
        .parent_x_off = 0,
        .parent_y_off = 0,
        .width = win_w - 30,
        .height = win_h,
        .screen = &screen,
    };

    try render(&state, nav_win, detail_win, a);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // The exact prompt string the code emits.
    try testing.expect(std.mem.indexOf(u8, text, "Overwrite existing title? [y/N]") != null);
    // The old/new titles must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Old Plan") != null);
    try testing.expect(std.mem.indexOf(u8, text, "New Plan") != null);
}

test "scope_explorer render: .error_msg overlay emits scope-mismatch refusal text" {
    // Render-level: the .error_msg overlay must render the scope-mismatch
    // refusal message set by the error path.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = ExplorerState.init(a);
    defer state.deinit();

    // Set error mode with the exact scope-mismatch message from the controller.
    const msg = try a.dupe(u8, "scope mismatch: entity scope differs from cockpit scope; set explicit_scope to override");
    state.edit_mode = .{ .error_msg = .{ .msg = msg } };

    const win_w: u16 = 120;
    const win_h: u16 = 6;
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
        .width = 40,
        .height = win_h,
        .screen = &screen,
    };
    const detail_win: Window = .{
        .x_off = 40,
        .y_off = 0,
        .parent_x_off = 0,
        .parent_y_off = 0,
        .width = win_w - 40,
        .height = win_h,
        .screen = &screen,
    };

    try render(&state, nav_win, detail_win, a);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // The "Error:" header must appear.
    try testing.expect(std.mem.indexOf(u8, text, "Error:") != null);
    // The scope-mismatch message text must appear.
    try testing.expect(std.mem.indexOf(u8, text, "scope mismatch") != null);
    // The dismiss hint must appear.
    try testing.expect(std.mem.indexOf(u8, text, "press any key to dismiss") != null);
}

test "scope_explorer compiles" {
    std.testing.refAllDecls(@This());
}
