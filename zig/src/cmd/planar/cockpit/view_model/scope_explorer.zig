//! Plan tree, entity detail, and scope-explorer cockpit queries.

const std = @import("std");
const db = @import("db");
const common = @import("common.zig");
const StatusBadge = common.StatusBadge;

// Plan tree view-model
// =========================================================================

/// A plan node in the Scope Explorer / Task Board tree.
pub const PlanNode = struct {
    id: i64,
    title: []const u8,
    slug: []const u8,
    status_badge: StatusBadge,
    task_count: u32,
    done_count: u32,
    /// Parent plan id for depth computation. Null for top-level plans.
    parent_plan_id: ?i64,
    /// Depth in the tree (0 = root plan). Computed by the caller.
    depth: u32,
    /// Whether this node is currently expanded.
    expanded: bool,

    pub fn deinit(self: PlanNode, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.slug);
    }
};

/// A task row in the Task Board / plan tree.
pub const TaskRow = struct {
    id: i64,
    plan_id: ?i64,
    title: []const u8,
    status_badge: StatusBadge,
    priority: i64,
    /// Active claim token if one exists.
    claim_token: ?[]const u8,

    pub fn deinit(self: TaskRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        if (self.claim_token) |s| allocator.free(s);
    }

    pub fn deinitMany(rows: []TaskRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

pub fn planStatusBadge(status_text: []const u8) StatusBadge {
    if (std.mem.eql(u8, status_text, "draft")) return .draft;
    if (std.mem.eql(u8, status_text, "active")) return .active;
    if (std.mem.eql(u8, status_text, "paused")) return .paused;
    if (std.mem.eql(u8, status_text, "done")) return .done;
    if (std.mem.eql(u8, status_text, "abandoned")) return .abandoned;
    return .none;
}

pub fn taskStatusBadge(status_text: []const u8) StatusBadge {
    if (std.mem.eql(u8, status_text, "todo")) return .todo;
    if (std.mem.eql(u8, status_text, "doing")) return .doing;
    if (std.mem.eql(u8, status_text, "blocked")) return .blocked;
    if (std.mem.eql(u8, status_text, "done")) return .done;
    if (std.mem.eql(u8, status_text, "cancelled")) return .cancelled;
    return .none;
}

/// Query a flat list of plans, ordered by parent_plan_id then id so the
/// caller can build a tree. Result is caller-owned; free each element
/// via `node.deinit(allocator)` then `allocator.free(slice)`.
pub fn queryPlanNodes(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]PlanNode {
    var stmt = d.prepare(
        \\select p.id, p.title, p.slug, p.status, p.parent_plan_id,
        \\       (select count(*) from tasks t where t.plan_id = p.id) as task_count,
        \\       (select count(*) from tasks t where t.plan_id = p.id and t.status = 'done') as done_count
        \\from plans p
        \\order by coalesce(p.parent_plan_id, p.id), p.id
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(PlanNode) = .empty;
    errdefer {
        for (out.items) |n| n.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const slug = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(slug);
                const status_text = try stmt.columnTextAlloc(3, allocator);
                defer allocator.free(status_text);
                const parent_plan_id = stmt.columnIntOpt(4);
                const task_count: u32 = @intCast(@max(0, stmt.columnInt(5)));
                const done_count: u32 = @intCast(@max(0, stmt.columnInt(6)));

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .slug = slug,
                    .status_badge = planStatusBadge(status_text),
                    .task_count = task_count,
                    .done_count = done_count,
                    .parent_plan_id = parent_plan_id,
                    .depth = 0,
                    .expanded = true,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query tasks for a specific plan, enriched with active claim info.
/// Result slice is caller-owned; free via `TaskRow.deinitMany`.
pub fn queryTaskRows(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) ![]TaskRow {
    var stmt = d.prepare(
        \\select t.id, t.plan_id, t.title, t.status, t.priority,
        \\       (select c.claim_token from agent_work_claims c
        \\        where c.entity_kind = 'task' and c.entity_id = t.id
        \\          and c.status = 'active'
        \\          and c.lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\        order by c.id desc limit 1) as active_claim_token
        \\from tasks t
        \\where t.plan_id = ?
        \\order by t.priority asc, t.id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(TaskRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

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

                try out.append(allocator, .{
                    .id = id,
                    .plan_id = pid,
                    .title = title,
                    .status_badge = taskStatusBadge(status_text),
                    .priority = priority,
                    .claim_token = claim_token,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Detail pane view-model
// =========================================================================

/// Kind of entity shown in the detail pane.
pub const DetailKind = enum {
    plan,
    task,
    claim,
    empty,
};

/// Content for the detail pane. The `body` slice is markdown text to
/// render via the markdown detail pane widget.
pub const DetailPane = struct {
    kind: DetailKind,
    title: []const u8,
    body: []const u8,

    pub fn deinit(self: DetailPane, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.body);
    }

    /// Empty detail pane (nothing selected).
    pub fn empty(allocator: std.mem.Allocator) !DetailPane {
        return .{
            .kind = .empty,
            .title = try allocator.dupe(u8, ""),
            .body = try allocator.dupe(u8, ""),
        };
    }
};

/// Build a DetailPane for a plan row from the DB.
pub fn queryPlanDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, summary, status from plans where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const summary_opt = try stmt.columnTextOpt(1, allocator);
            defer if (summary_opt) |s| allocator.free(s);
            const status_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status_text);

            const body = if (summary_opt) |s|
                try std.fmt.allocPrint(allocator, "**Status:** {s}\n\n{s}", .{ status_text, s })
            else
                try std.fmt.allocPrint(allocator, "**Status:** {s}", .{status_text});

            return .{
                .kind = .plan,
                .title = title,
                .body = body,
            };
        },
    }
}

/// Build a DetailPane for a task from the DB.
pub fn queryTaskDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, body, status, priority, next_action from tasks where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
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

            var body_parts: std.ArrayList(u8) = .empty;
            errdefer body_parts.deinit(allocator);
            {
                const first_line = try std.fmt.allocPrint(
                    allocator,
                    "**Status:** {s}  **Priority:** {d}",
                    .{ status_text, priority },
                );
                defer allocator.free(first_line);
                try body_parts.appendSlice(allocator, first_line);
            }
            if (next_action_opt) |na| {
                const part = try std.fmt.allocPrint(allocator, "\n\n**Next action:** {s}", .{na});
                defer allocator.free(part);
                try body_parts.appendSlice(allocator, part);
            }
            if (body_opt) |b| {
                const part = try std.fmt.allocPrint(allocator, "\n\n{s}", .{b});
                defer allocator.free(part);
                try body_parts.appendSlice(allocator, part);
            }

            return .{
                .kind = .task,
                .title = title,
                .body = try body_parts.toOwnedSlice(allocator),
            };
        },
    }
}

// =========================================================================
// Scope Explorer view-model  (tasks 3966–3970)
// =========================================================================

/// A scope filter for the Scope Explorer: either a specific repo scope
/// (by project.id) or all scopes.
pub const ScopeFilter = common.ScopeFilter;

/// Query plans visible under `filter`, ordered by parent then id so that
/// child plans (subplans) follow their parent in the flat list. The caller
/// must build depth annotations from the parent_plan_id column.
///
/// Returns a flat slice; caller frees each element via `node.deinit(allocator)`
/// then `allocator.free(slice)`.
pub fn queryPlanNodesFiltered(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: ScopeFilter,
) ![]PlanNode {
    const sql_all =
        \\select p.id, p.title, p.slug, p.status, p.parent_plan_id,
        \\       (select count(*) from tasks t where t.plan_id = p.id) as task_count,
        \\       (select count(*) from tasks t where t.plan_id = p.id and t.status = 'done') as done_count
        \\from plans p
        \\order by coalesce(p.parent_plan_id, p.id), p.id
    ;
    const sql_repo =
        \\select p.id, p.title, p.slug, p.status, p.parent_plan_id,
        \\       (select count(*) from tasks t where t.plan_id = p.id) as task_count,
        \\       (select count(*) from tasks t where t.plan_id = p.id and t.status = 'done') as done_count
        \\from plans p
        \\where (p.scope_kind = 'repo' and p.scope_id = ?) or p.scope_kind = 'global'
        \\order by coalesce(p.parent_plan_id, p.id), p.id
    ;

    var stmt = switch (filter) {
        .all => blk: {
            var s = d.prepare(sql_all) catch return error.QueryFailed;
            s.bind(&.{}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
        .repo => blk: {
            var s = d.prepare(sql_repo) catch return error.QueryFailed;
            s.bind(&.{.{ .int = filter.repo }}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
    };
    defer stmt.finalize();

    var out: std.ArrayList(PlanNode) = .empty;
    errdefer {
        for (out.items) |n| n.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const slug = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(slug);
                const status_text = try stmt.columnTextAlloc(3, allocator);
                defer allocator.free(status_text);
                const parent_plan_id = stmt.columnIntOpt(4);
                const task_count: u32 = @intCast(@max(0, stmt.columnInt(5)));
                const done_count: u32 = @intCast(@max(0, stmt.columnInt(6)));

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .slug = slug,
                    .status_badge = planStatusBadge(status_text),
                    .task_count = task_count,
                    .done_count = done_count,
                    .parent_plan_id = parent_plan_id,
                    .depth = 0, // caller computes depth from parent_plan_id
                    .expanded = true,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// One child-entity row under a drilled plan (task, decision, question,
/// scenario, or artifact).
pub const DrillKind = enum {
    task,
    decision,
    question,
    scenario,
    artifact,
};

pub const DrillRow = struct {
    id: i64,
    kind: DrillKind,
    title: []const u8,
    status_badge: StatusBadge,
    /// plan_id the row belongs to.
    plan_id: i64,

    pub fn deinit(self: DrillRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
    }

    pub fn deinitMany(rows: []DrillRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

pub fn decisionStatusBadge(status_text: []const u8) StatusBadge {
    // decisions status: proposed, accepted, superseded, withdrawn.
    if (std.mem.eql(u8, status_text, "accepted")) return .done;
    if (std.mem.eql(u8, status_text, "withdrawn")) return .cancelled;
    // superseded uses .superseded (glyph '^') — distinct from .abandoned ('~')
    // and .draft ('d') so the operator can immediately tell a superseded decision
    // apart from a merely-proposed or abandoned one.
    if (std.mem.eql(u8, status_text, "superseded")) return .superseded;
    return .draft; // proposed
}

pub fn questionStatusBadge(status_text: []const u8) StatusBadge {
    // questions status: open, answered, wontfix.
    if (std.mem.eql(u8, status_text, "open")) return .todo;
    if (std.mem.eql(u8, status_text, "answered")) return .done;
    if (std.mem.eql(u8, status_text, "wontfix")) return .cancelled;
    return .none;
}

pub fn scenarioStatusBadge(status_text: []const u8) StatusBadge {
    // test_scenarios status: draft, ready, verified, failing, retired.
    if (std.mem.eql(u8, status_text, "draft")) return .draft;
    if (std.mem.eql(u8, status_text, "verified")) return .done;
    if (std.mem.eql(u8, status_text, "failing")) return .blocked;
    if (std.mem.eql(u8, status_text, "retired")) return .abandoned;
    return .none; // ready
}

/// Query all child entities for a plan: tasks, decisions, questions,
/// scenarios, artifacts linked to the plan. Returns a flat list ordered
/// by kind then id. Caller owns result; free via `DrillRow.deinitMany`.
pub fn queryPlanDrillRows(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) ![]DrillRow {
    var out: std.ArrayList(DrillRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    // Tasks.
    {
        var stmt = d.prepare(
            "select id, title, status from tasks where plan_id = ? order by priority asc, id asc",
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const status = try stmt.columnTextAlloc(2, allocator);
                    defer allocator.free(status);
                    try out.append(allocator, .{
                        .id = id,
                        .kind = .task,
                        .title = title,
                        .status_badge = taskStatusBadge(status),
                        .plan_id = plan_id,
                    });
                },
            }
        }
    }

    // Decisions linked to this plan via entity_links.
    // entity_links uses from_kind/from_id/to_kind/to_id/relationship.
    // Check both directions: plan→decision and decision→plan.
    {
        var stmt = d.prepare(
            \\select d.id, d.title, d.status
            \\from decisions d
            \\where d.id in (
            \\  select el.to_id from entity_links el
            \\  where el.from_kind = 'plan' and el.from_id = ? and el.to_kind = 'decision'
            \\  union
            \\  select el.from_id from entity_links el
            \\  where el.to_kind = 'plan' and el.to_id = ? and el.from_kind = 'decision'
            \\)
            \\order by d.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const status = try stmt.columnTextAlloc(2, allocator);
                    defer allocator.free(status);
                    try out.append(allocator, .{
                        .id = id,
                        .kind = .decision,
                        .title = title,
                        .status_badge = decisionStatusBadge(status),
                        .plan_id = plan_id,
                    });
                },
            }
        }
    }

    // Questions linked to this plan via entity_links (both directions).
    {
        var stmt = d.prepare(
            \\select q.id, q.title, q.status
            \\from questions q
            \\where q.id in (
            \\  select el.to_id from entity_links el
            \\  where el.from_kind = 'plan' and el.from_id = ? and el.to_kind = 'question'
            \\  union
            \\  select el.from_id from entity_links el
            \\  where el.to_kind = 'plan' and el.to_id = ? and el.from_kind = 'question'
            \\)
            \\order by q.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const status = try stmt.columnTextAlloc(2, allocator);
                    defer allocator.free(status);
                    try out.append(allocator, .{
                        .id = id,
                        .kind = .question,
                        .title = title,
                        .status_badge = questionStatusBadge(status),
                        .plan_id = plan_id,
                    });
                },
            }
        }
    }

    // Test scenarios linked to this plan via entity_links (both directions).
    {
        var stmt = d.prepare(
            \\select ts.id, ts.title, ts.status
            \\from test_scenarios ts
            \\where ts.id in (
            \\  select el.to_id from entity_links el
            \\  where el.from_kind = 'plan' and el.from_id = ? and el.to_kind = 'test_scenario'
            \\  union
            \\  select el.from_id from entity_links el
            \\  where el.to_kind = 'plan' and el.to_id = ? and el.from_kind = 'test_scenario'
            \\)
            \\order by ts.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    const status = try stmt.columnTextAlloc(2, allocator);
                    defer allocator.free(status);
                    try out.append(allocator, .{
                        .id = id,
                        .kind = .scenario,
                        .title = title,
                        .status_badge = scenarioStatusBadge(status),
                        .plan_id = plan_id,
                    });
                },
            }
        }
    }

    // Artifacts linked to this plan via entity_links (both directions).
    // artifacts.kind is the column (not artifact_kind).
    {
        var stmt = d.prepare(
            \\select a.id, a.title, a.kind
            \\from artifacts a
            \\where a.id in (
            \\  select el.to_id from entity_links el
            \\  where el.from_kind = 'plan' and el.from_id = ? and el.to_kind = 'artifact'
            \\  union
            \\  select el.from_id from entity_links el
            \\  where el.to_kind = 'plan' and el.to_id = ? and el.from_kind = 'artifact'
            \\)
            \\order by a.id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(title);
                    try out.append(allocator, .{
                        .id = id,
                        .kind = .artifact,
                        .title = title,
                        .status_badge = .none,
                        .plan_id = plan_id,
                    });
                },
            }
        }
    }

    return try out.toOwnedSlice(allocator);
}

/// Build a DetailPane for a decision from the DB (for the split detail pane).
/// Note: decisions.body is NOT NULL in the schema.
pub fn queryDecisionDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    decision_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, body, status from decisions where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = decision_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            // decisions.body is NOT NULL.
            const body_raw = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(body_raw);
            const status = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status);

            const body = try std.fmt.allocPrint(
                allocator,
                "**Status:** {s}\n\n{s}",
                .{ status, body_raw },
            );

            return .{ .kind = .plan, .title = title, .body = body };
        },
    }
}

/// Build a DetailPane for a question from the DB.
pub fn queryQuestionDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    question_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, body, status from questions where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = question_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const status = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status);

            const body = if (body_opt) |b|
                try std.fmt.allocPrint(allocator, "**Status:** {s}\n\n{s}", .{ status, b })
            else
                try std.fmt.allocPrint(allocator, "**Status:** {s}", .{status});

            return .{ .kind = .plan, .title = title, .body = body };
        },
    }
}

/// Build a DetailPane for a test scenario from the DB.
pub fn queryScenarioDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    scenario_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, body, status from test_scenarios where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = scenario_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const status = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status);

            const body = if (body_opt) |b|
                try std.fmt.allocPrint(allocator, "**Status:** {s}\n\n{s}", .{ status, b })
            else
                try std.fmt.allocPrint(allocator, "**Status:** {s}", .{status});

            return .{ .kind = .plan, .title = title, .body = body };
        },
    }
}

/// Build a DetailPane for an artifact from the DB.
/// Note: artifacts.kind is the column name (not artifact_kind).
pub fn queryArtifactDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    artifact_id: i64,
) !DetailPane {
    var stmt = d.prepare(
        "select title, body, kind from artifacts where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = artifact_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return DetailPane.empty(allocator),
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const kind_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(kind_text);

            const body = if (body_opt) |b|
                try std.fmt.allocPrint(allocator, "**Kind:** {s}\n\n{s}", .{ kind_text, b })
            else
                try std.fmt.allocPrint(allocator, "**Kind:** {s}", .{kind_text});

            return .{ .kind = .plan, .title = title, .body = body };
        },
    }
}

// =========================================================================
