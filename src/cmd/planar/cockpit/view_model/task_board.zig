//! Task-board, lifecycle history, touch-path, and blocking-link queries.

const std = @import("std");
const db = @import("db");
const common = @import("common.zig");
const StatusBadge = common.StatusBadge;
const ScopeFilter = common.ScopeFilter;
const taskStatusBadge = @import("scope_explorer.zig").taskStatusBadge;

// Task Board view-model  (tasks 4018, 4019, 4020)
// =========================================================================

/// One row in a Task Board column.
///
/// `status_badge` reflects the task's actual status. The board groups rows
/// into four columns by status:
///
///   open    — tasks with status = 'todo'
///   doing   — tasks with status = 'doing'
///   blocked — tasks with status = 'blocked'
///   done    — tasks with status = 'done'
///
/// Cancelled tasks are excluded from the board (terminal, not actionable).
pub const BoardTaskRow = struct {
    id: i64,
    plan_id: ?i64,
    title: []const u8,
    status_badge: StatusBadge,
    priority: i64,
    /// Active claim token if one exists (for the claiming-agent indicator).
    claim_token: ?[]const u8,

    pub fn deinit(self: BoardTaskRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        if (self.claim_token) |s| allocator.free(s);
    }

    pub fn deinitMany(rows: []BoardTaskRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Grouped task board: tasks bucketed into the four board columns.
/// All slices are caller-owned; free via `TaskBoardSnapshot.deinit`.
pub const TaskBoardSnapshot = struct {
    /// Tasks with status = 'todo' (board column: open).
    open: []BoardTaskRow,
    /// Tasks with status = 'doing'.
    doing: []BoardTaskRow,
    /// Tasks with status = 'blocked'.
    blocked: []BoardTaskRow,
    /// Tasks with status = 'done'.
    done: []BoardTaskRow,

    pub fn deinit(self: TaskBoardSnapshot, allocator: std.mem.Allocator) void {
        BoardTaskRow.deinitMany(self.open, allocator);
        BoardTaskRow.deinitMany(self.doing, allocator);
        BoardTaskRow.deinitMany(self.blocked, allocator);
        BoardTaskRow.deinitMany(self.done, allocator);
    }

    /// Total row count across all columns.
    pub fn totalCount(self: *const TaskBoardSnapshot) usize {
        return self.open.len + self.doing.len + self.blocked.len + self.done.len;
    }
};

/// One reopen record for a task (from task_reopens).
pub const TaskReopenRow = struct {
    id: i64,
    /// Status before the reopen (done or cancelled).
    from_status: []const u8,
    /// Status after the reopen (todo, doing, or blocked).
    to_status: []const u8,
    /// Source verb (task-reopen or task-update-force).
    source: []const u8,
    /// Optional operator-provided reason.
    reason: ?[]const u8,
    /// ISO-8601 created_at.
    created_at: []const u8,

    pub fn deinit(self: TaskReopenRow, allocator: std.mem.Allocator) void {
        allocator.free(self.from_status);
        allocator.free(self.to_status);
        allocator.free(self.source);
        if (self.reason) |s| allocator.free(s);
        allocator.free(self.created_at);
    }

    pub fn deinitMany(rows: []TaskReopenRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One touch-path record for a task (from task_touch_paths).
pub const TaskTouchPathRow = struct {
    id: i64,
    /// Repo-relative file path.
    path: []const u8,
    /// ISO-8601 created_at.
    created_at: []const u8,

    pub fn deinit(self: TaskTouchPathRow, allocator: std.mem.Allocator) void {
        allocator.free(self.path);
        allocator.free(self.created_at);
    }

    pub fn deinitMany(rows: []TaskTouchPathRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One blocking/dependency link row for a task (from entity_links).
///
/// The entity_links `blocks` relationship encodes:
///
///   from_kind=task, from_id=A, to_kind=task, to_id=B, relationship='blocks'
///   → task A blocks task B.
///
/// For the board detail pane we surface:
///   • what blocks this task: where this task is the `to_id` (blocked-by set)
///   • what this task blocks: where this task is the `from_id` (blocks set)
pub const TaskLinkRow = struct {
    id: i64,
    /// Human-readable label: "task:<id> — <title>".
    label: []const u8,
    /// Direction from the perspective of the selected task:
    ///   .blocks_this = another task blocks the selected task
    ///   .this_blocks = the selected task blocks another task
    direction: LinkDirection,

    pub fn deinit(self: TaskLinkRow, allocator: std.mem.Allocator) void {
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []TaskLinkRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Direction of a blocking link relative to the selected task.
pub const LinkDirection = enum {
    /// Another task blocks this task (entity_links: other→this, relationship='blocks').
    blocks_this,
    /// This task blocks another task (entity_links: this→other, relationship='blocks').
    this_blocks,
};

/// Detail pane data for a selected task on the Task Board. Combines the
/// basic task fields with reopen history, touch paths, and blocking links.
///
/// All fields are caller-owned; free via `TaskBoardDetail.deinit`.
pub const TaskBoardDetail = struct {
    /// Task id.
    id: i64,
    /// Task title.
    title: []const u8,
    /// Rendered markdown body (status, priority, next_action, body).
    body: []const u8,
    /// Reopen history (chronological, oldest-first).
    reopens: []TaskReopenRow,
    /// Touch paths (alphabetical).
    touch_paths: []TaskTouchPathRow,
    /// Blocking links (blocked-by first, then blocks).
    links: []TaskLinkRow,

    pub fn deinit(self: TaskBoardDetail, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.body);
        TaskReopenRow.deinitMany(self.reopens, allocator);
        TaskTouchPathRow.deinitMany(self.touch_paths, allocator);
        TaskLinkRow.deinitMany(self.links, allocator);
    }
};

/// Query all non-cancelled tasks and group them into board columns.
///
/// Filters to the current scope if `scope_filter` is `.repo`; shows all
/// scopes when `.all`. Ordered within each column by priority asc, id asc.
///
/// Status values confirmed from migration 00003_work_items.up.sql:
///   check(status in ('todo','doing','blocked','done','cancelled'))
///
/// 'cancelled' tasks are excluded (terminal, not board-actionable).
pub fn queryTaskBoard(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    scope_filter: ScopeFilter,
) !TaskBoardSnapshot {
    var open_rows: std.ArrayList(BoardTaskRow) = .empty;
    errdefer {
        for (open_rows.items) |r| r.deinit(allocator);
        open_rows.deinit(allocator);
    }
    var doing_rows: std.ArrayList(BoardTaskRow) = .empty;
    errdefer {
        for (doing_rows.items) |r| r.deinit(allocator);
        doing_rows.deinit(allocator);
    }
    var blocked_rows: std.ArrayList(BoardTaskRow) = .empty;
    errdefer {
        for (blocked_rows.items) |r| r.deinit(allocator);
        blocked_rows.deinit(allocator);
    }
    var done_rows: std.ArrayList(BoardTaskRow) = .empty;
    errdefer {
        for (done_rows.items) |r| r.deinit(allocator);
        done_rows.deinit(allocator);
    }

    // SQL: select all non-cancelled tasks plus their active claim token
    // (sub-select same as queryTaskRows). Filter to scope when repo.
    const sql_all =
        \\select t.id, t.plan_id, t.title, t.status, t.priority,
        \\       (select c.claim_token from agent_work_claims c
        \\        where c.entity_kind = 'task' and c.entity_id = t.id
        \\          and c.status = 'active'
        \\          and c.lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\        order by c.id desc limit 1) as active_claim_token
        \\from tasks t
        \\where t.status in ('todo','doing','blocked','done')
        \\order by t.priority asc, t.id asc
    ;
    const sql_repo =
        \\select t.id, t.plan_id, t.title, t.status, t.priority,
        \\       (select c.claim_token from agent_work_claims c
        \\        where c.entity_kind = 'task' and c.entity_id = t.id
        \\          and c.status = 'active'
        \\          and c.lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\        order by c.id desc limit 1) as active_claim_token
        \\from tasks t
        \\where t.status in ('todo','doing','blocked','done')
        \\  and (t.scope_kind = 'repo' and t.scope_id = ? or t.scope_kind = 'global')
        \\order by t.priority asc, t.id asc
    ;

    var stmt = switch (scope_filter) {
        .all => blk: {
            var s = d.prepare(sql_all) catch return error.QueryFailed;
            s.bind(&.{}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
        .repo => |pid| blk: {
            var s = d.prepare(sql_repo) catch return error.QueryFailed;
            s.bind(&.{.{ .int = pid }}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
    };
    defer stmt.finalize();

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const pid = stmt.columnIntOpt(1);
                const title = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(title);
                const status_text = try stmt.columnTextAlloc(3, allocator);
                defer allocator.free(status_text);
                const priority = stmt.columnInt(4);
                const claim_token = try stmt.columnTextOpt(5, allocator);

                const row: BoardTaskRow = .{
                    .id = id,
                    .plan_id = pid,
                    .title = title,
                    .status_badge = taskStatusBadge(status_text),
                    .priority = priority,
                    .claim_token = claim_token,
                };

                // Route to the appropriate column.
                if (std.mem.eql(u8, status_text, "todo")) {
                    try open_rows.append(allocator, row);
                } else if (std.mem.eql(u8, status_text, "doing")) {
                    try doing_rows.append(allocator, row);
                } else if (std.mem.eql(u8, status_text, "blocked")) {
                    try blocked_rows.append(allocator, row);
                } else if (std.mem.eql(u8, status_text, "done")) {
                    try done_rows.append(allocator, row);
                } else {
                    // Unknown status — skip (should not happen given the WHERE clause).
                    row.deinit(allocator);
                }
            },
        }
    }

    return .{
        .open = try open_rows.toOwnedSlice(allocator),
        .doing = try doing_rows.toOwnedSlice(allocator),
        .blocked = try blocked_rows.toOwnedSlice(allocator),
        .done = try done_rows.toOwnedSlice(allocator),
    };
}

/// Query reopen history for a task. Rows are ordered chronologically
/// (oldest-first). Caller owns the result; free via `TaskReopenRow.deinitMany`.
pub fn queryTaskReopens(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) ![]TaskReopenRow {
    var stmt = d.prepare(
        \\select id, from_status, to_status, source, reason, created_at
        \\from task_reopens
        \\where task_id = ?
        \\order by created_at asc, id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(TaskReopenRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const from_s = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(from_s);
                const to_s = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(to_s);
                const source = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(source);
                const reason = try stmt.columnTextOpt(4, allocator);
                errdefer if (reason) |s| allocator.free(s);
                const created = try stmt.columnTextAlloc(5, allocator);
                errdefer allocator.free(created);

                try out.append(allocator, .{
                    .id = id,
                    .from_status = from_s,
                    .to_status = to_s,
                    .source = source,
                    .reason = reason,
                    .created_at = created,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query touch paths for a task. Rows are ordered by path (alphabetical).
/// Caller owns the result; free via `TaskTouchPathRow.deinitMany`.
pub fn queryTaskTouchPaths(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) ![]TaskTouchPathRow {
    var stmt = d.prepare(
        \\select id, path, created_at
        \\from task_touch_paths
        \\where task_id = ?
        \\order by path asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(TaskTouchPathRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const path = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(path);
                const created = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(created);

                try out.append(allocator, .{
                    .id = id,
                    .path = path,
                    .created_at = created,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query blocking links for a task from entity_links.
///
/// Surfaces two directions:
///   (1) tasks that block this task: entity_links rows where
///       to_kind='task', to_id=task_id, from_kind='task', relationship='blocks'
///       → direction = .blocks_this
///   (2) tasks this task blocks: entity_links rows where
///       from_kind='task', from_id=task_id, to_kind='task', relationship='blocks'
///       → direction = .this_blocks
///
/// The `blocks` relationship is the only one checked here per migration
/// 00004_entity_links.up.sql, which confirms the valid set:
///   ('derives-from','blocks','addresses','verifies','cites','supersedes','touches')
///
/// Caller owns the result; free via `TaskLinkRow.deinitMany`.
pub fn queryTaskBlockingLinks(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) ![]TaskLinkRow {
    var out: std.ArrayList(TaskLinkRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    // (1) Tasks that block this task (blocked-by set).
    {
        var stmt = d.prepare(
            \\select t.id, t.title
            \\from tasks t
            \\join entity_links el on el.from_kind = 'task'
            \\  and el.from_id = t.id
            \\  and el.to_kind = 'task'
            \\  and el.to_id = ?
            \\  and el.relationship = 'blocks'
            \\order by t.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const label = try std.fmt.allocPrint(
                        allocator,
                        "task:{d} — {s}",
                        .{ id, title },
                    );
                    allocator.free(title);
                    errdefer allocator.free(label);

                    try out.append(allocator, .{
                        .id = id,
                        .label = label,
                        .direction = .blocks_this,
                    });
                },
            }
        }
    }

    // (2) Tasks this task blocks (this_blocks set).
    {
        var stmt = d.prepare(
            \\select t.id, t.title
            \\from tasks t
            \\join entity_links el on el.to_kind = 'task'
            \\  and el.to_id = t.id
            \\  and el.from_kind = 'task'
            \\  and el.from_id = ?
            \\  and el.relationship = 'blocks'
            \\order by t.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const label = try std.fmt.allocPrint(
                        allocator,
                        "task:{d} — {s}",
                        .{ id, title },
                    );
                    allocator.free(title);
                    errdefer allocator.free(label);

                    try out.append(allocator, .{
                        .id = id,
                        .label = label,
                        .direction = .this_blocks,
                    });
                },
            }
        }
    }

    return try out.toOwnedSlice(allocator);
}

/// Query full Task Board detail for a selected task.
///
/// Combines the basic task fields (status, priority, next_action, body)
/// with reopen history (task_reopens), touch paths (task_touch_paths),
/// and blocking/dependency links (entity_links, relationship='blocks').
///
/// Returns null when the task does not exist.
/// Caller owns the result; free via `TaskBoardDetail.deinit`.
pub fn queryTaskBoardDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) !?TaskBoardDetail {
    var stmt = d.prepare(
        "select title, body, status, priority, next_action from tasks where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return null,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const status_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status_text);
            const priority = stmt.columnInt(3);
            const next_action_opt = try stmt.columnTextOpt(4, allocator);
            defer if (next_action_opt) |s| allocator.free(s);

            // Build the markdown body.
            var body_buf: std.ArrayList(u8) = .empty;
            errdefer body_buf.deinit(allocator);

            const first_line = try std.fmt.allocPrint(
                allocator,
                "**Status:** {s}  **Priority:** {d}",
                .{ status_text, priority },
            );
            defer allocator.free(first_line);
            try body_buf.appendSlice(allocator, first_line);

            if (next_action_opt) |na| {
                const part = try std.fmt.allocPrint(allocator, "\n\n**Next action:** {s}", .{na});
                defer allocator.free(part);
                try body_buf.appendSlice(allocator, part);
            }
            if (body_opt) |b| {
                const part = try std.fmt.allocPrint(allocator, "\n\n{s}", .{b});
                defer allocator.free(part);
                try body_buf.appendSlice(allocator, part);
            }

            const body = try body_buf.toOwnedSlice(allocator);
            errdefer allocator.free(body);

            // Sub-queries.
            const reopens = try queryTaskReopens(d, allocator, task_id);
            errdefer TaskReopenRow.deinitMany(reopens, allocator);

            const touch_paths = try queryTaskTouchPaths(d, allocator, task_id);
            errdefer TaskTouchPathRow.deinitMany(touch_paths, allocator);

            const links = try queryTaskBlockingLinks(d, allocator, task_id);
            errdefer TaskLinkRow.deinitMany(links, allocator);

            return .{
                .id = task_id,
                .title = title,
                .body = body,
                .reopens = reopens,
                .touch_paths = touch_paths,
                .links = links,
            };
        },
    }
}

/// Try to resolve the current working directory's scope from the DB.
/// Returns the project id if a single-association repo match is found,
/// or null if the cwd maps to multiple/no scopes (caller falls back to
/// all-scopes mode).
///
/// This is a best-effort lookup: failures (SQL error, cwd not found)
/// return null silently rather than crashing the cockpit.
pub fn cwdScopeProjectId(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    cwd: []const u8,
) ?i64 {
    // Find the project whose root_path is the longest prefix of cwd.
    var stmt = d.prepare(
        "select id, root_path from projects where root_path is not null order by length(root_path) desc",
    ) catch return null;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return null;

    while (true) {
        switch (stmt.step() catch return null) {
            .done => return null,
            .row => {
                const pid = stmt.columnInt(0);
                const root = stmt.columnTextOpt(1, allocator) catch return null;
                if (root) |r| {
                    defer allocator.free(r);
                    if (std.mem.startsWith(u8, cwd, r)) {
                        // Count associations for this project.
                        var count_stmt = d.prepare(
                            "select count(*) from project_associations where project_id = ?",
                        ) catch return pid; // conservative: return pid on error
                        defer count_stmt.finalize();
                        count_stmt.bind(&.{.{ .int = pid }}) catch return pid;
                        switch (count_stmt.step() catch return pid) {
                            .done => return pid,
                            .row => {
                                const cnt = count_stmt.columnInt(0);
                                // Only filter to this scope when the project
                                // has at least one association.
                                if (cnt >= 1) return pid;
                                return null;
                            },
                        }
                    }
                }
            },
        }
    }
}

// =========================================================================
