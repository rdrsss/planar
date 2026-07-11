//! engine/planning/task — Task entity: CRUD + status transitions
//! against the `tasks` table.
//!
//! Status: {todo, doing, blocked, done, cancelled} — different from
//! plan's set (planning vs work-item life cycles diverge).
//!
//! Scope handling: when `scope` is provided in CreateArgs/ListFilter/UpdateArgs,
//! it is resolved via engine.identity.scope.resolveSlug. See plan.zig for the
//! full design rationale.
//!
//! State-transition verbs (`markDone`, `markCancelled`, `markBlocked`,
//! `reopen`) are first-class for the common moves. `update(status=…)`
//! still works and goes through the same policy.status check; the
//! dedicated verbs just save a couple of CLI flags and let the verb
//! name carry the intent into the audit log.

const std = @import("std");
const db = @import("db");
const identity = @import("../identity.zig");
const policy = @import("../policy.zig");
const plan = @import("plan.zig");

// =========================================================================
// Types
// =========================================================================

pub const ScopeKind = enum {
    repo,
    association,
    global,

    pub fn fromText(s: []const u8) ?ScopeKind {
        if (std.mem.eql(u8, s, "repo")) return .repo;
        if (std.mem.eql(u8, s, "association")) return .association;
        if (std.mem.eql(u8, s, "global")) return .global;
        return null;
    }
};

pub const Status = enum {
    todo,
    doing,
    blocked,
    done,
    cancelled,

    pub fn fromText(s: []const u8) ?Status {
        if (std.mem.eql(u8, s, "todo")) return .todo;
        if (std.mem.eql(u8, s, "doing")) return .doing;
        if (std.mem.eql(u8, s, "blocked")) return .blocked;
        if (std.mem.eql(u8, s, "done")) return .done;
        if (std.mem.eql(u8, s, "cancelled")) return .cancelled;
        return null;
    }
};

/// One row from the `tasks` table. All string fields owned by the
/// allocator passed to the function that produced this Task.
pub const Task = struct {
    id: i64,
    scope_kind: ScopeKind,
    scope_id: ?i64,
    plan_id: ?i64,
    parent_task_id: ?i64,
    title: []const u8,
    body: ?[]const u8,
    /// Optional stable slug (migration 00011). When non-null, scenarios
    /// in the test-spec can cite this task as `task:<slug>` and the
    /// ingestor's coverage gate resolves the slug to this row.
    slug: ?[]const u8,
    status: Status,
    priority: i64,
    next_action: ?[]const u8,
    due_at: ?[]const u8,
    created_at: []const u8,
    updated_at: []const u8,
};

pub fn deinit(task: Task, allocator: std.mem.Allocator) void {
    allocator.free(task.title);
    if (task.body) |s| allocator.free(s);
    if (task.slug) |s| allocator.free(s);
    if (task.next_action) |s| allocator.free(s);
    if (task.due_at) |s| allocator.free(s);
    allocator.free(task.created_at);
    allocator.free(task.updated_at);
}

pub fn deinitMany(tasks: []const Task, allocator: std.mem.Allocator) void {
    for (tasks) |t| deinit(t, allocator);
    allocator.free(tasks);
}

pub const CreateArgs = struct {
    title: []const u8,
    body: ?[]const u8 = null,
    status: Status = .todo,
    priority: i64 = 100,
    plan_id: ?i64 = null,
    parent_task_id: ?i64 = null,
    next_action: ?[]const u8 = null,
    due_at: ?[]const u8 = null,
    /// Stable slug — see `Task.slug`. The ingestor sets this from
    /// `[slug: foo-bar]` roadmap annotations; the CLI surface accepts
    /// it via the `--slug` flag on `planar task add`.
    slug: ?[]const u8 = null,
    /// Skip plan-status auto-promotion recompute for this operation.
    no_auto_promote: bool = false,
    /// Scope slug accepted by identity.scope.resolveSlug.
    scope: ?[]const u8 = null,
};

pub const UpdateArgs = struct {
    title: ?[]const u8 = null,
    body: ?[]const u8 = null,
    status: ?Status = null,
    priority: ?i64 = null,
    plan_id: ?i64 = null,
    next_action: ?[]const u8 = null,
    due_at: ?[]const u8 = null,
    /// When non-null, sets the task's slug. The ingestor's re-ingest
    /// pass uses this to backfill a slug onto a previously-unannotated
    /// task. Setting to a value that conflicts with another task's
    /// slug surfaces as Error.SlugConflict at apply time.
    slug: ?[]const u8 = null,
    /// Clear plan_id (set to NULL) when true.
    clear_plan: bool = false,
    /// Skip plan-status auto-promotion recompute for this operation.
    no_auto_promote: bool = false,
    /// Scope slug accepted by identity.scope.resolveSlug.
    scope: ?[]const u8 = null,
    /// When true and the status transition is a reopen (from done/cancelled
    /// to todo/doing/blocked), records a task_reopens row with
    /// source='task-update-force'. Paired with `reason` for the audit trail.
    force: bool = false,
    /// Optional reason stored in task_reopens when `force` triggers a reopen.
    reason: ?[]const u8 = null,
};

pub const ListFilter = struct {
    status: ?Status = null,
    plan_id: ?i64 = null,
    /// Inclusive upper bound on priority value (lower = more urgent).
    priority_max: ?i64 = null,
    /// Legacy single-scope filter retained for handler compatibility.
    scope: ?[]const u8 = null,
    /// Multi-scope filter. Values are scope slugs accepted by resolveSlug().
    scopes: []const []const u8 = &.{},
};

pub const Error =
    error{
        NotFound,
        UnsupportedScope,
        SlugNotFound,
        SlugConflict,
        InvalidStatus,
        InvalidDueAt,
        InvalidParentCycle,
        QueryFailed,
        /// The task has an active, unexpired work claim. The operator must
        /// release/complete the claim via the agent path, or pass force=true
        /// to override (decision 533 / task 4165).
        TaskClaimed,
    } ||
    std.mem.Allocator.Error ||
    policy.scope_guard.Error ||
    policy.status.Error ||
    policy.audit.Error;

/// parseDueAt validates `s` as YYYY-MM-DD or a basic RFC3339 timestamp.
/// Returns s unchanged when valid.
pub fn parseDueAt(s: []const u8) Error![]const u8 {
    if (s.len == 0) return s;
    if (isDateOnly(s)) return s;
    if (isRfc3339Like(s)) return s;
    return Error.InvalidDueAt;
}

fn isDateOnly(s: []const u8) bool {
    if (s.len != 10) return false;
    if (s[4] != '-' or s[7] != '-') return false;
    if (!allDigits(s[0..4]) or !allDigits(s[5..7]) or !allDigits(s[8..10])) return false;
    const year = std.fmt.parseInt(u32, s[0..4], 10) catch return false;
    const month = std.fmt.parseInt(u32, s[5..7], 10) catch return false;
    const day = std.fmt.parseInt(u32, s[8..10], 10) catch return false;
    return validDate(year, month, day);
}

fn isRfc3339Like(s: []const u8) bool {
    if (s.len < 20) return false;
    if (!isDateOnly(s[0..10])) return false;
    if (s[10] != 'T') return false;
    if (!allDigits(s[11..13]) or s[13] != ':' or !allDigits(s[14..16]) or s[16] != ':') return false;
    if (!allDigits(s[17..19])) return false;
    const hour = std.fmt.parseInt(u32, s[11..13], 10) catch return false;
    const minute = std.fmt.parseInt(u32, s[14..16], 10) catch return false;
    const second = std.fmt.parseInt(u32, s[17..19], 10) catch return false;
    if (hour > 23 or minute > 59 or second > 59) return false;

    var idx: usize = 19;
    if (idx < s.len and s[idx] == '.') {
        idx += 1;
        const start = idx;
        while (idx < s.len and std.ascii.isDigit(s[idx])) : (idx += 1) {}
        if (idx == start) return false;
    }
    if (idx >= s.len) return false;
    if (s[idx] == 'Z') return idx + 1 == s.len;
    if (s[idx] != '+' and s[idx] != '-') return false;
    if (idx + 6 != s.len) return false;
    if (!allDigits(s[idx + 1 .. idx + 3]) or s[idx + 3] != ':' or !allDigits(s[idx + 4 .. idx + 6])) return false;
    const tz_hour = std.fmt.parseInt(u32, s[idx + 1 .. idx + 3], 10) catch return false;
    const tz_minute = std.fmt.parseInt(u32, s[idx + 4 .. idx + 6], 10) catch return false;
    return tz_hour <= 23 and tz_minute <= 59;
}

fn allDigits(s: []const u8) bool {
    for (s) |c| {
        if (!std.ascii.isDigit(c)) return false;
    }
    return true;
}

fn validDate(year: u32, month: u32, day: u32) bool {
    if (month < 1 or month > 12) return false;
    if (day < 1) return false;
    const max_day = daysInMonth(year, month);
    return day <= max_day;
}

fn daysInMonth(year: u32, month: u32) u32 {
    return switch (month) {
        1, 3, 5, 7, 8, 10, 12 => 31,
        4, 6, 9, 11 => 30,
        2 => if (isLeapYear(year)) 29 else 28,
        else => 0,
    };
}

fn isLeapYear(year: u32) bool {
    if (year % 400 == 0) return true;
    if (year % 100 == 0) return false;
    return year % 4 == 0;
}

/// Return true when `task_id` has an active, unexpired work claim.
///
/// Inlined from engine.runtime.agentactivity.store.hasActiveClaim — importing
/// that module from planning/task.zig would create a compile cycle (atomic.zig
/// already imports planning/plan.zig). The SQL is identical; cite decision 533
/// and task 4165 as the authoritative source for this invariant.
fn hasActiveClaimOnTask(d: *db.sqlite.Db, task_id: i64) Error!bool {
    var stmt = d.prepare(
        \\select 1 from agent_work_claims
        \\where entity_kind = 'task'
        \\  and entity_id = ?
        \\  and status = 'active'
        \\  and lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => false,
        .row => true,
    };
}

fn beginSavepoint(d: *db.sqlite.Db, allocator: std.mem.Allocator, name: []const u8) Error!void {
    d.savepoint(allocator, name) catch |e| switch (e) {
        error.OutOfMemory => return error.OutOfMemory,
        else => return Error.WriteFailed,
    };
}

fn finishSavepoint(d: *db.sqlite.Db, allocator: std.mem.Allocator, name: []const u8) Error!void {
    d.releaseSavepoint(allocator, name) catch |e| switch (e) {
        error.OutOfMemory => return error.OutOfMemory,
        else => return Error.WriteFailed,
    };
}

/// Begin an IMMEDIATE transaction, acquiring the writer lock right away.
/// Used by the status-transition verbs (markDone, markBlocked, reopen,
/// update) so the claim check and the UPDATE run inside the same
/// write-locked transaction — no other writer can interleave between
/// the SELECT on agent_work_claims and the UPDATE on tasks.
fn beginImmediate(d: *db.sqlite.Db) Error!void {
    d.exec("BEGIN IMMEDIATE") catch return Error.QueryFailed;
}

fn commitTx(d: *db.sqlite.Db) Error!void {
    d.exec("COMMIT") catch return Error.QueryFailed;
}

fn rollbackTx(d: *db.sqlite.Db) void {
    d.exec("ROLLBACK") catch |e| {
        std.log.warn("task: rollback failed: {s}", .{@errorName(e)});
    };
}

// =========================================================================
// CRUD
// =========================================================================

pub fn create(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: CreateArgs) Error!Task {
    const scope_ref = if (args.scope) |s|
        identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        }
    else
        identity.scope.ScopeRef{ .kind = .global, .id = null };

    const scope_kind_str: []const u8 = switch (scope_ref.kind) {
        .global => "global",
        .association => "association",
        .repo => "repo",
    };

    if (args.due_at) |due| _ = try parseDueAt(due);

    try policy.scope_guard.check(null, null);

    try beginSavepoint(d, allocator, "task_create");
    var savepoint_released = false;
    defer {
        if (!savepoint_released) {
            d.rollbackToSavepoint(allocator, "task_create") catch {};
            d.releaseSavepoint(allocator, "task_create") catch {};
        }
    }

    const insert_sql: [:0]const u8 =
        \\insert into tasks (scope_kind, scope_id, plan_id, parent_task_id,
        \\                   title, body, slug, status, priority, next_action, due_at)
        \\values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ;
    const id = d.execParams(insert_sql, &.{
        .{ .text = scope_kind_str },
        if (scope_ref.id) |sid| .{ .int = sid } else .{ .null = {} },
        if (args.plan_id) |p| .{ .int = p } else .{ .null = {} },
        if (args.parent_task_id) |p| .{ .int = p } else .{ .null = {} },
        .{ .text = args.title },
        if (args.body) |s| .{ .text = s } else .{ .null = {} },
        if (args.slug) |s| .{ .text = s } else .{ .null = {} },
        .{ .text = @tagName(args.status) },
        .{ .int = args.priority },
        if (args.next_action) |s| .{ .text = s } else .{ .null = {} },
        if (args.due_at) |s| .{ .text = s } else .{ .null = {} },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.SlugConflict;
        std.log.err("task.create exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const summary = try std.fmt.allocPrint(allocator, "create task '{s}'", .{args.title});
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "task", .id = id },
        .scope = null,
        .summary = summary,
    });

    if (!args.no_auto_promote) {
        if (args.plan_id) |pid| {
            const recompute = try plan.recomputeStatus(d, allocator, pid);
            recompute.deinit(allocator);
        }
    }

    try finishSavepoint(d, allocator, "task_create");
    savepoint_released = true;

    const created = try show(d, allocator, id);
    return created;
}

pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Task {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => return Error.NotFound,
        .row => return try readRow(&stmt, allocator),
    }
}

pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: ListFilter) Error![]Task {
    var scope_refs: std.ArrayList(identity.scope.ScopeRef) = .empty;
    defer scope_refs.deinit(allocator);
    if (filter.scope) |s| {
        try scope_refs.append(allocator, identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        });
    }
    for (filter.scopes) |s| {
        try scope_refs.append(allocator, identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        });
    }

    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, select_all_prefix);

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    if (filter.status) |s| {
        try sql_buf.appendSlice(allocator, " and status = ?");
        try params.append(allocator, .{ .text = @tagName(s) });
    } else {
        try sql_buf.appendSlice(allocator, " and status in ('todo','doing','blocked')");
    }
    if (filter.plan_id) |p| {
        try sql_buf.appendSlice(allocator, " and plan_id = ?");
        try params.append(allocator, .{ .int = p });
    }
    if (filter.priority_max) |p| {
        try sql_buf.appendSlice(allocator, " and priority <= ?");
        try params.append(allocator, .{ .int = p });
    }
    if (scope_refs.items.len > 0) {
        try sql_buf.appendSlice(allocator, " and (");
        for (scope_refs.items, 0..) |ref, i| {
            if (i > 0) try sql_buf.appendSlice(allocator, " or ");
            switch (ref.kind) {
                .global => try sql_buf.appendSlice(allocator, "scope_kind = 'global'"),
                .association => {
                    try sql_buf.appendSlice(allocator, "(scope_kind = 'association' and scope_id = ?)");
                    try params.append(allocator, .{ .int = ref.id.? });
                },
                .repo => {
                    try sql_buf.appendSlice(allocator, "(scope_kind = 'repo' and scope_id = ?)");
                    try params.append(allocator, .{ .int = ref.id.? });
                },
            }
        }
        try sql_buf.appendSlice(allocator, ")");
    }
    // Default order: open tasks by priority ascending, then by updated_at desc,
    // then by id. Matches the ix_tasks_open_priority partial index.
    try sql_buf.appendSlice(allocator, " order by priority, updated_at desc, id");

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var out: std.ArrayList(Task) = .empty;
    errdefer {
        for (out.items) |t| deinit(t, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// touchedRepoIDs returns repo ids linked from this task via relationship='touches'.
pub fn touchedRepoIDs(d: *db.sqlite.Db, allocator: std.mem.Allocator, task_id: i64) Error![]i64 {
    var stmt = d.prepare(
        \\select to_id
        \\from entity_links
        \\where from_kind='task' and from_id=? and to_kind='repo' and relationship='touches'
        \\order by id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(i64) = .empty;
    errdefer out.deinit(allocator);
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, stmt.columnInt(0)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// One path-level touch declaration for a task: the repo it belongs to
/// (projects.id) and the repo-relative file path. `path` is heap-owned by
/// the caller-supplied allocator and must be freed via `deinitTouchPaths`.
pub const TouchPath = struct {
    repo_id: i64,
    path: []const u8,
};

/// Free a slice of TouchPath rows returned by `touchedPaths`.
pub fn deinitTouchPaths(paths: []TouchPath, allocator: std.mem.Allocator) void {
    for (paths) |p| allocator.free(p.path);
    allocator.free(paths);
}

/// addTouchPath records a path-level touch for a task in `task_touch_paths`.
/// `repo_id` is a projects.id; `path` is a repo-relative file path. Idempotent
/// against the `unique(task_id, repo_id, path)` constraint — a duplicate
/// declaration is a no-op (insert or ignore), so callers can re-declare freely.
pub fn addTouchPath(
    d: *db.sqlite.Db,
    task_id: i64,
    repo_id: i64,
    path: []const u8,
) Error!void {
    _ = d.execParams(
        \\insert or ignore into task_touch_paths (task_id, repo_id, path)
        \\values (?, ?, ?)
    , &.{
        .{ .int = task_id },
        .{ .int = repo_id },
        .{ .text = path },
    }) catch return Error.QueryFailed;
}

/// touchedPaths returns the path-level touch declarations for a task
/// (rows in `task_touch_paths`), ordered by repo then path. Caller owns the
/// returned slice and must free it via `deinitTouchPaths`.
pub fn touchedPaths(d: *db.sqlite.Db, allocator: std.mem.Allocator, task_id: i64) Error![]TouchPath {
    var stmt = d.prepare(
        \\select repo_id, path
        \\from task_touch_paths
        \\where task_id = ?
        \\order by repo_id, path
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(TouchPath) = .empty;
    errdefer {
        for (out.items) |p| allocator.free(p.path);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const repo_id = stmt.columnInt(0);
                const path = stmt.columnTextAlloc(1, allocator) catch return Error.QueryFailed;
                try out.append(allocator, .{ .repo_id = repo_id, .path = path });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// listTouching returns tasks scoped to repo_id OR linked via touches to repo_id.
pub fn listTouching(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    repo_id: i64,
    filter: ListFilter,
) Error![]Task {
    var scope_refs: std.ArrayList(identity.scope.ScopeRef) = .empty;
    defer scope_refs.deinit(allocator);
    if (filter.scope) |s| {
        try scope_refs.append(allocator, identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        });
    }
    for (filter.scopes) |s| {
        try scope_refs.append(allocator, identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        });
    }

    var branch1_active = true;
    if (scope_refs.items.len > 0) {
        branch1_active = false;
        for (scope_refs.items) |ref| {
            if (ref.kind == .repo and (ref.id == null or ref.id.? == repo_id)) {
                branch1_active = true;
                break;
            }
        }
    }

    var sql: std.ArrayList(u8) = .empty;
    defer sql.deinit(allocator);

    try sql.appendSlice(allocator, "select * from (");
    if (branch1_active) {
        try sql.appendSlice(allocator, select_all_prefix);
        try sql.appendSlice(allocator, " and scope_kind='repo' and scope_id=?");
        if (filter.status) |_| try sql.appendSlice(allocator, " and status=?") else try sql.appendSlice(allocator, " and status in ('todo','doing','blocked')");
        if (filter.plan_id) |_| try sql.appendSlice(allocator, " and plan_id=?");
        if (filter.priority_max) |_| try sql.appendSlice(allocator, " and priority<=?");
    } else {
        try sql.appendSlice(allocator, select_all_prefix);
        try sql.appendSlice(allocator, " and 1=0");
    }

    try sql.appendSlice(allocator, " union ");
    try sql.appendSlice(allocator, select_all_prefix);
    try sql.appendSlice(allocator, " and id in (select from_id from entity_links where from_kind='task' and to_kind='repo' and to_id=? and relationship='touches')");
    if (filter.status) |_| try sql.appendSlice(allocator, " and status=?") else try sql.appendSlice(allocator, " and status in ('todo','doing','blocked')");
    if (scope_refs.items.len > 0) {
        try sql.appendSlice(allocator, " and (");
        for (scope_refs.items, 0..) |ref, i| {
            if (i > 0) try sql.appendSlice(allocator, " or ");
            switch (ref.kind) {
                .global => try sql.appendSlice(allocator, "scope_kind='global'"),
                .association => try sql.appendSlice(allocator, "(scope_kind='association' and scope_id=?)"),
                .repo => try sql.appendSlice(allocator, "(scope_kind='repo' and scope_id=?)"),
            }
        }
        try sql.appendSlice(allocator, ")");
    }
    if (filter.plan_id) |_| try sql.appendSlice(allocator, " and plan_id=?");
    if (filter.priority_max) |_| try sql.appendSlice(allocator, " and priority<=?");
    try sql.appendSlice(allocator, ") order by id");

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);
    if (branch1_active) {
        try params.append(allocator, .{ .int = repo_id });
        if (filter.status) |s| try params.append(allocator, .{ .text = @tagName(s) });
        if (filter.plan_id) |p| try params.append(allocator, .{ .int = p });
        if (filter.priority_max) |p| try params.append(allocator, .{ .int = p });
    }
    try params.append(allocator, .{ .int = repo_id });
    if (filter.status) |s| try params.append(allocator, .{ .text = @tagName(s) });
    for (scope_refs.items) |ref| switch (ref.kind) {
        .global => {},
        .association, .repo => try params.append(allocator, .{ .int = ref.id.? }),
    };
    if (filter.plan_id) |p| try params.append(allocator, .{ .int = p });
    if (filter.priority_max) |p| try params.append(allocator, .{ .int = p });

    const sql_z = try allocator.dupeZ(u8, sql.items);
    defer allocator.free(sql_z);
    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var out: std.ArrayList(Task) = .empty;
    errdefer {
        for (out.items) |t| deinit(t, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn update(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    patch: UpdateArgs,
) Error!Task {
    const scope_ref: ?identity.scope.ScopeRef = if (patch.scope) |s|
        identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        }
    else
        null;

    if (patch.due_at) |due| _ = try parseDueAt(due);

    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, "update tasks set ");

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    var first = true;
    const appendSep = struct {
        fn call(buf: *std.ArrayList(u8), is_first: *bool, alloc: std.mem.Allocator) !void {
            if (!is_first.*) try buf.appendSlice(alloc, ", ");
            is_first.* = false;
        }
    }.call;

    if (scope_ref) |ref| {
        const sk_str: []const u8 = switch (ref.kind) {
            .global => "global",
            .association => "association",
            .repo => "repo",
        };
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "scope_kind = ?");
        try params.append(allocator, .{ .text = sk_str });
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "scope_id = ?");
        try params.append(allocator, if (ref.id) |sid| .{ .int = sid } else .{ .null = {} });
    }
    if (patch.title) |t| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "title = ?");
        try params.append(allocator, .{ .text = t });
    }
    if (patch.body) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "body = ?");
        try params.append(allocator, .{ .text = s });
    }
    if (patch.status) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "status = ?");
        try params.append(allocator, .{ .text = @tagName(s) });
    }
    if (patch.priority) |p| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "priority = ?");
        try params.append(allocator, .{ .int = p });
    }
    if (patch.plan_id) |p| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "plan_id = ?");
        try params.append(allocator, .{ .int = p });
    } else if (patch.clear_plan) {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "plan_id = null");
    }
    if (patch.next_action) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "next_action = ?");
        try params.append(allocator, .{ .text = s });
    }
    if (patch.due_at) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "due_at = ?");
        try params.append(allocator, .{ .text = s });
    }
    if (patch.slug) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "slug = ?");
        try params.append(allocator, .{ .text = s });
    }

    if (first) return try show(d, allocator, id);

    try appendSep(&sql_buf, &first, allocator);
    try sql_buf.appendSlice(allocator, "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
    try params.append(allocator, .{ .int = id });

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    // When the patch includes a status change, open a BEGIN IMMEDIATE so the
    // claim check and the UPDATE are in one write-locked transaction
    // (decision 533 / task 4165 correctness fix). Non-status-change updates
    // use an ordinary savepoint — no claim guard applies.
    const has_status_change = patch.status != null;
    if (has_status_change) {
        try beginImmediate(d);
    } else {
        try beginSavepoint(d, allocator, "task_update");
    }
    var tx_done = false;
    defer {
        if (!tx_done) {
            if (has_status_change) {
                rollbackTx(d);
            } else {
                d.rollbackToSavepoint(allocator, "task_update") catch {};
                d.releaseSavepoint(allocator, "task_update") catch {};
            }
        }
    }

    // Claim guard executes inside the write-locked transaction (status path).
    if (has_status_change and !patch.force) {
        if (try hasActiveClaimOnTask(d, id)) {
            rollbackTx(d);
            tx_done = true;
            return Error.TaskClaimed;
        }
    }

    // Read and validate after the writer lock is held. Otherwise a concurrent
    // terminal transition can invalidate the status snapshot before UPDATE.
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    if (patch.status) |new_status| {
        try policy.status.check(.task, @tagName(current.status), @tagName(new_status), patch.force);
    }

    _ = d.execParams(sql_z, params.items) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.SlugConflict;
        std.log.err("task.update exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const verb: policy.audit.Verb = if (patch.status != null) .status_change else .update;
    try policy.audit.record(d, .{
        .verb = verb,
        .entity = .{ .kind = "task", .id = id },
        .scope = null,
        .summary = null,
    });

    // Insert a task_reopens row when --force moves a task out of a terminal
    // status (done/cancelled) into one of the allowed reopen targets
    // (todo/doing/blocked). This is the 'task-update-force' source path.
    if (patch.force) {
        if (patch.status) |new_status| {
            const from_is_terminal = current.status == .done or current.status == .cancelled;
            const to_is_reopen = new_status == .todo or new_status == .doing or new_status == .blocked;
            if (from_is_terminal and to_is_reopen) {
                _ = d.execParams(
                    \\insert into task_reopens (task_id, from_status, to_status, source, reason)
                    \\values (?, ?, ?, 'task-update-force', ?)
                , &.{
                    .{ .int = id },
                    .{ .text = @tagName(current.status) },
                    .{ .text = @tagName(new_status) },
                    if (patch.reason) |r| .{ .text = r } else .{ .null = {} },
                }) catch return Error.QueryFailed;
            }
        }
    }

    const updated = try show(d, allocator, id);
    errdefer deinit(updated, allocator);
    if (!patch.no_auto_promote) {
        if (current.plan_id) |pid| {
            const recompute = try plan.recomputeStatus(d, allocator, pid);
            recompute.deinit(allocator);
        }
        if (updated.plan_id) |pid| if (current.plan_id == null or current.plan_id.? != pid) {
            const recompute = try plan.recomputeStatus(d, allocator, pid);
            recompute.deinit(allocator);
        };
    }
    if (has_status_change) {
        try commitTx(d);
    } else {
        try finishSavepoint(d, allocator, "task_update");
    }
    tx_done = true;
    return updated;
}

// =========================================================================
// State transitions
// =========================================================================

/// Mark a task as done. Records a `status_change` audit row with a
/// "done" summary so the trail shows which verb the operator used,
/// not just that the status changed.
///
/// Returns `error.TaskClaimed` when an active, unexpired work claim exists on
/// the task and `force` is false (decision 533 / task 4165 TOCTOU guard).
/// Pass `force=true` to override the guard and flip the status anyway.
///
/// Atomicity: the claim check and the UPDATE are executed inside one
/// BEGIN IMMEDIATE transaction, so no concurrent process can acquire a
/// claim between the check and the flip (task 4165 correctness fix).
pub fn markDone(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64, force: bool) Error!Task {
    try beginImmediate(d);
    var committed = false;
    errdefer if (!committed) rollbackTx(d);

    // Claim guard executes inside the write-locked transaction.
    if (!force) {
        if (try hasActiveClaimOnTask(d, id)) {
            rollbackTx(d);
            committed = true;
            return Error.TaskClaimed;
        }
    }

    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    try policy.status.check(.task, @tagName(current.status), "done", force);

    _ = d.execParams(
        "update tasks set status = 'done', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
        &.{.{ .int = id }},
    ) catch return Error.QueryFailed;

    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "task", .id = id },
        .scope = null,
        .summary = "done",
    });

    const updated = try show(d, allocator, id);
    errdefer deinit(updated, allocator);
    if (updated.plan_id) |pid| {
        const recompute = try plan.recomputeStatus(d, allocator, pid);
        recompute.deinit(allocator);
    }

    try commitTx(d);
    committed = true;
    return updated;
}

pub fn markCancelled(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Task {
    const nested = d.inTransaction();
    if (nested) {
        try beginSavepoint(d, allocator, "task_cancel");
    } else {
        try beginImmediate(d);
    }
    var committed = false;
    errdefer if (!committed) {
        if (nested) {
            d.rollbackToSavepoint(allocator, "task_cancel") catch {};
            d.releaseSavepoint(allocator, "task_cancel") catch {};
        } else {
            rollbackTx(d);
        }
    };

    if (try hasActiveClaimOnTask(d, id)) {
        if (nested) {
            d.rollbackToSavepoint(allocator, "task_cancel") catch return Error.QueryFailed;
            d.releaseSavepoint(allocator, "task_cancel") catch return Error.QueryFailed;
        } else {
            rollbackTx(d);
        }
        committed = true;
        return Error.TaskClaimed;
    }

    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    try policy.status.check(.task, @tagName(current.status), "cancelled", false);
    _ = d.execParams(
        "update tasks set status = 'cancelled', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?",
        &.{.{ .int = id }},
    ) catch return Error.QueryFailed;
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "task", .id = id },
        .scope = null,
        .summary = "cancelled",
    });
    const updated = try show(d, allocator, id);
    errdefer deinit(updated, allocator);
    if (updated.plan_id) |pid| {
        const recompute = try plan.recomputeStatus(d, allocator, pid);
        recompute.deinit(allocator);
    }
    if (nested) {
        try finishSavepoint(d, allocator, "task_cancel");
    } else {
        try commitTx(d);
    }
    committed = true;
    return updated;
}

/// Mark a task as blocked. `blocked_on_id` records which task is
/// blocking it; the audit summary mentions both so the trail is
/// self-describing. A future iteration will also insert a row into
/// entity_links (`task -> task` "blocked-by") once that module exists.
///
/// Returns `error.TaskClaimed` when an active, unexpired work claim exists on
/// the task and `force` is false (decision 533 / task 4165 TOCTOU guard).
/// Pass `force=true` to override.
///
/// Atomicity: the claim check and the UPDATE are executed inside one
/// BEGIN IMMEDIATE transaction (task 4165 correctness fix).
pub fn markBlocked(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    blocked_on_id: i64,
    reason: ?[]const u8,
    force: bool,
) Error!Task {
    // Verify the blocker exists before opening the transaction (read-only,
    // no write-lock needed).
    const blocker = try show(d, allocator, blocked_on_id);
    deinit(blocker, allocator);

    const summary = if (reason) |r|
        try std.fmt.allocPrint(allocator, "blocked on task {d}: {s}", .{ blocked_on_id, r })
    else
        try std.fmt.allocPrint(allocator, "blocked on task {d}", .{blocked_on_id});
    defer allocator.free(summary);

    try beginImmediate(d);
    var committed = false;
    errdefer if (!committed) rollbackTx(d);

    // Claim guard executes inside the write-locked transaction.
    if (!force) {
        if (try hasActiveClaimOnTask(d, id)) {
            rollbackTx(d);
            committed = true;
            return Error.TaskClaimed;
        }
    }

    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    try policy.status.check(.task, @tagName(current.status), "blocked", false);

    _ = d.execParams(
        "update tasks set status = 'blocked', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
        &.{.{ .int = id }},
    ) catch return Error.QueryFailed;

    _ = d.execParams(
        \\insert into entity_links (from_kind, from_id, to_kind, to_id, relationship)
        \\values ('task', ?, 'task', ?, 'blocks')
    , &.{ .{ .int = id }, .{ .int = blocked_on_id } }) catch return Error.QueryFailed;

    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "task", .id = id },
        .scope = null,
        .summary = summary,
    });

    const updated = try show(d, allocator, id);
    errdefer deinit(updated, allocator);
    if (updated.plan_id) |pid| {
        const recompute = try plan.recomputeStatus(d, allocator, pid);
        recompute.deinit(allocator);
    }
    try commitTx(d);
    committed = true;
    return updated;
}

/// Reopen a done/cancelled task. `reason` is required at the CLI; the
/// caller has already validated that. Stored in the audit summary so
/// the trail explains why the task came back to life.
///
/// Inserts a `task_reopens` row with source='task-reopen' in the same
/// transaction as the status change so the audit table is always consistent
/// with the tasks row.
///
/// Returns `error.TaskClaimed` when an active, unexpired work claim exists on
/// the task and `force` is false (decision 533 / task 4165 TOCTOU guard).
/// Pass `force=true` to override.
///
/// Atomicity: the claim check and the UPDATE are executed inside one
/// BEGIN IMMEDIATE transaction (task 4165 correctness fix).
pub fn reopen(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    new_status: Status,
    reason: []const u8,
    force: bool,
) Error!Task {
    const summary = try std.fmt.allocPrint(
        allocator,
        "reopen to {s}: {s}",
        .{ @tagName(new_status), reason },
    );
    defer allocator.free(summary);

    try beginImmediate(d);
    var committed = false;
    errdefer if (!committed) rollbackTx(d);

    // Claim guard executes inside the write-locked transaction.
    if (!force) {
        if (try hasActiveClaimOnTask(d, id)) {
            rollbackTx(d);
            committed = true;
            return Error.TaskClaimed;
        }
    }

    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    // `reopen` is the verb-gated escape from terminal status; it bypasses the
    // bare-update matrix (force=true) because the dedicated verb already
    // carries its own authorization and records the task_reopens audit row.
    try policy.status.check(.task, @tagName(current.status), @tagName(new_status), true);

    _ = d.execParams(
        "update tasks set status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
        &.{ .{ .text = @tagName(new_status) }, .{ .int = id } },
    ) catch return Error.QueryFailed;

    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "task", .id = id },
        .scope = null,
        .summary = summary,
    });

    // Record the reopen in the task_reopens audit table. Only insert when
    // moving FROM a terminal status (done/cancelled) — the CHECK constraints
    // on the table enforce this too, but we guard here to avoid a noisy
    // error on non-reopen transitions that happen to call this function.
    const from_is_terminal = current.status == .done or current.status == .cancelled;
    const to_is_reopen = new_status == .todo or new_status == .doing or new_status == .blocked;
    if (from_is_terminal and to_is_reopen) {
        _ = d.execParams(
            \\insert into task_reopens (task_id, from_status, to_status, source, reason)
            \\values (?, ?, ?, 'task-reopen', ?)
        , &.{
            .{ .int = id },
            .{ .text = @tagName(current.status) },
            .{ .text = @tagName(new_status) },
            .{ .text = reason },
        }) catch return Error.QueryFailed;
    }

    const updated = try show(d, allocator, id);
    errdefer deinit(updated, allocator);
    if (updated.plan_id) |pid| {
        const recompute = try plan.recomputeStatus(d, allocator, pid);
        recompute.deinit(allocator);
    }
    try commitTx(d);
    committed = true;
    return updated;
}

fn transition(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    new_status: Status,
    short_summary: []const u8,
) Error!Task {
    return try transitionWithSummary(d, allocator, id, new_status, short_summary);
}

fn transitionWithSummary(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    new_status: Status,
    summary: []const u8,
) Error!Task {
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    try policy.status.check(.task, @tagName(current.status), @tagName(new_status), false);

    try beginSavepoint(d, allocator, "task_transition");
    var savepoint_released = false;
    defer {
        if (!savepoint_released) {
            d.rollbackToSavepoint(allocator, "task_transition") catch {};
            d.releaseSavepoint(allocator, "task_transition") catch {};
        }
    }

    _ = d.execParams(
        "update tasks set status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
        &.{ .{ .text = @tagName(new_status) }, .{ .int = id } },
    ) catch return Error.QueryFailed;

    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "task", .id = id },
        .scope = null,
        .summary = summary,
    });
    const updated = try show(d, allocator, id);
    errdefer deinit(updated, allocator);
    if (updated.plan_id) |pid| {
        const recompute = try plan.recomputeStatus(d, allocator, pid);
        recompute.deinit(allocator);
    }
    try finishSavepoint(d, allocator, "task_transition");
    savepoint_released = true;
    return updated;
}

// =========================================================================
// Output rendering
// =========================================================================

pub fn renderText(task: Task, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("id:          {d}\n", .{@as(u64, @intCast(task.id))});
    try writer.print("title:       {s}\n", .{task.title});
    try writer.print("status:      {s}\n", .{@tagName(task.status)});
    try writer.print("priority:    {d}\n", .{@as(u64, @intCast(@max(task.priority, 0)))});
    try writer.print("scope:       {s}", .{@tagName(task.scope_kind)});
    if (task.scope_id) |sid| try writer.print(":{d}", .{sid});
    try writer.print("\n", .{});
    if (task.plan_id) |p| try writer.print("plan:        {d}\n", .{p});
    if (task.parent_task_id) |p| try writer.print("parent:      {d}\n", .{p});
    if (task.next_action) |s| try writer.print("next action: {s}\n", .{s});
    if (task.due_at) |s| try writer.print("due:         {s}\n", .{s});
    if (task.body) |s| try writer.print("body:        {s}\n", .{s});
    try writer.print("created:     {s}\n", .{task.created_at});
    try writer.print("updated:     {s}\n", .{task.updated_at});
}

pub fn renderListText(tasks: []const Task, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    if (tasks.len == 0) {
        try writer.print("(no tasks)\n", .{});
        return;
    }
    for (tasks) |t| {
        // Cast signed values to unsigned for display so {d:>N} doesn't
        // emit a leading '+' on positive ids/priorities.
        try writer.print("{d:>5}  pri {d:>3}  {s:<10}  {s}\n", .{
            @as(u64, @intCast(t.id)),
            @as(u64, @intCast(@max(t.priority, 0))),
            @tagName(t.status),
            t.title,
        });
    }
}

// =========================================================================
// Internals
// =========================================================================

const select_columns =
    "id, scope_kind, scope_id, plan_id, parent_task_id, title, body, slug, " ++
    "status, priority, next_action, due_at, created_at, updated_at";

const select_one_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from tasks where id = ?";

const select_all_prefix =
    "select " ++ select_columns ++ " from tasks where 1 = 1";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Task {
    const scope_kind_text = try stmt.columnTextAlloc(1, allocator);
    defer allocator.free(scope_kind_text);
    const scope_kind = ScopeKind.fromText(scope_kind_text) orelse return Error.QueryFailed;

    const status_text = try stmt.columnTextAlloc(8, allocator);
    defer allocator.free(status_text);
    const status = Status.fromText(status_text) orelse return Error.QueryFailed;

    return .{
        .id = stmt.columnInt(0),
        .scope_kind = scope_kind,
        .scope_id = stmt.columnIntOpt(2),
        .plan_id = stmt.columnIntOpt(3),
        .parent_task_id = stmt.columnIntOpt(4),
        .title = try stmt.columnTextAlloc(5, allocator),
        .body = try stmt.columnTextOpt(6, allocator),
        .slug = try stmt.columnTextOpt(7, allocator),
        .status = status,
        .priority = stmt.columnInt(9),
        .next_action = try stmt.columnTextOpt(10, allocator),
        .due_at = try stmt.columnTextOpt(11, allocator),
        .created_at = try stmt.columnTextAlloc(12, allocator),
        .updated_at = try stmt.columnTextAlloc(13, allocator),
    };
}

// =========================================================================
// Tests
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "create + show round-trip a global task" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{
        .title = "Write the test",
        .body = "And run it.",
        .priority = 50,
        .next_action = "open editor",
    });
    defer deinit(t, a);

    try std.testing.expectEqualStrings("Write the test", t.title);
    try std.testing.expectEqual(Status.todo, t.status);
    try std.testing.expectEqual(@as(i64, 50), t.priority);
    try std.testing.expect(t.body != null);
    try std.testing.expectEqualStrings("And run it.", t.body.?);
    try std.testing.expect(t.next_action != null);
    try std.testing.expectEqualStrings("open editor", t.next_action.?);
}

test "create with known association scope writes scope_kind='association'" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'acme'");
    const t = try create(&d, a, .{ .title = "scoped task", .scope = "acme" });
    defer deinit(t, a);
    try std.testing.expectEqual(ScopeKind.association, t.scope_kind);
    try std.testing.expectEqual(assoc_id, t.scope_id.?);
}

test "create with scope='global' writes scope_kind='global', scope_id=null" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const t = try create(&d, a, .{ .title = "global task", .scope = "global" });
    defer deinit(t, a);
    try std.testing.expectEqual(ScopeKind.global, t.scope_kind);
    try std.testing.expect(t.scope_id == null);
}

test "create with unknown scope slug returns SlugNotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(
        Error.SlugNotFound,
        create(&d, a, .{ .title = "x", .scope = "no-such-slug" }),
    );
}

test "create with repo: scope writes scope_kind='repo'" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('foo', 'Foo', '/work/foo')",
        &.{},
    );
    const repo_id = try d.intQuery("select id from projects where slug = 'foo'");
    const t = try create(&d, a, .{ .title = "repo task", .scope = "repo:foo" });
    defer deinit(t, a);
    try std.testing.expectEqual(ScopeKind.repo, t.scope_kind);
    try std.testing.expectEqual(repo_id, t.scope_id.?);
}

test "parseDueAt rejects impossible dates/times" {
    try std.testing.expectError(Error.InvalidDueAt, parseDueAt("2026-99-99"));
    try std.testing.expectError(Error.InvalidDueAt, parseDueAt("2026-02-30"));
    try std.testing.expectError(Error.InvalidDueAt, parseDueAt("2026-01-01T25:00:00Z"));
    try std.testing.expectError(Error.InvalidDueAt, parseDueAt("2026-01-01T23:61:00Z"));
    try std.testing.expectError(Error.InvalidDueAt, parseDueAt("2026-01-01T23:59:61Z"));
    try std.testing.expectError(Error.InvalidDueAt, parseDueAt("2026-01-01T10:00:00+24:00"));
}

test "list orders by priority then updated_at desc" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const low_pri = try create(&d, a, .{ .title = "later", .priority = 200 });
    defer deinit(low_pri, a);
    const high_pri = try create(&d, a, .{ .title = "urgent", .priority = 10 });
    defer deinit(high_pri, a);

    const all = try list(&d, a, .{});
    defer deinitMany(all, a);
    try std.testing.expectEqual(@as(usize, 2), all.len);
    try std.testing.expectEqualStrings("urgent", all[0].title);
    try std.testing.expectEqualStrings("later", all[1].title);
}

test "list filters by status, plan_id, priority_max" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t1 = try create(&d, a, .{ .title = "todo task" });
    defer deinit(t1, a);
    const t2 = try create(&d, a, .{ .title = "doing task", .status = .doing, .priority = 50 });
    defer deinit(t2, a);
    const t3 = try create(&d, a, .{ .title = "low pri", .priority = 500 });
    defer deinit(t3, a);

    const doing = try list(&d, a, .{ .status = .doing });
    defer deinitMany(doing, a);
    try std.testing.expectEqual(@as(usize, 1), doing.len);
    try std.testing.expectEqualStrings("doing task", doing[0].title);

    const urgent = try list(&d, a, .{ .priority_max = 100 });
    defer deinitMany(urgent, a);
    try std.testing.expectEqual(@as(usize, 2), urgent.len);
}

test "list defaults to todo/doing/blocked statuses" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t1 = try create(&d, a, .{ .title = "todo task", .status = .todo });
    defer deinit(t1, a);
    const t2 = try create(&d, a, .{ .title = "doing task", .status = .doing });
    defer deinit(t2, a);
    const t3 = try create(&d, a, .{ .title = "blocked task", .status = .blocked });
    defer deinit(t3, a);
    const t4 = try create(&d, a, .{ .title = "done task", .status = .done });
    defer deinit(t4, a);
    const t5 = try create(&d, a, .{ .title = "cancelled task", .status = .cancelled });
    defer deinit(t5, a);

    const items = try list(&d, a, .{});
    defer deinitMany(items, a);
    try std.testing.expectEqual(@as(usize, 3), items.len);
    for (items) |it| {
        try std.testing.expect(it.status == .todo or it.status == .doing or it.status == .blocked);
    }
}

test "markDone transitions doing → done and records status_change audit" {
    // The matrix requires todo → doing before done; markDone from todo is now illegal.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "finish it" });
    defer deinit(t, a);
    // Advance to doing first (todo → doing is legal per the matrix).
    const doing_task = try update(&d, a, t.id, .{ .status = .doing });
    defer deinit(doing_task, a);
    const done_task = try markDone(&d, a, t.id, false);
    defer deinit(done_task, a);

    try std.testing.expectEqual(Status.done, done_task.status);
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='status_change' and entity_kind='task' and summary='done'"),
    );
}

test "markCancelled transitions to cancelled" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "won't do" });
    defer deinit(t, a);
    const c = try markCancelled(&d, a, t.id);
    defer deinit(c, a);
    try std.testing.expectEqual(Status.cancelled, c.status);
}

test "markBlocked stores summary referencing the blocker" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const blocker = try create(&d, a, .{ .title = "the blocker" });
    defer deinit(blocker, a);
    const target = try create(&d, a, .{ .title = "the blocked" });
    defer deinit(target, a);

    const blocked = try markBlocked(&d, a, target.id, blocker.id, null, false);
    defer deinit(blocked, a);

    try std.testing.expectEqual(Status.blocked, blocked.status);
    const expected_summary = try std.fmt.allocPrint(a, "blocked on task {d}", .{blocker.id});
    defer a.free(expected_summary);
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery(
            "select count(*) from audit_log where verb='status_change' and entity_kind='task' " ++
                "and summary like 'blocked on task %'",
        ),
    );
}

test "reopen returns done task to todo with reason in audit" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "redo this" });
    defer deinit(t, a);
    // Advance to doing first so markDone is legal (todo → doing → done).
    const doing_t = try update(&d, a, t.id, .{ .status = .doing });
    defer deinit(doing_t, a);
    const done_t = try markDone(&d, a, t.id, false);
    defer deinit(done_t, a);

    const reopened = try reopen(&d, a, t.id, .todo, "scope changed", false);
    defer deinit(reopened, a);
    try std.testing.expectEqual(Status.todo, reopened.status);
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery(
            "select count(*) from audit_log where verb='status_change' and entity_kind='task' " ++
                "and summary like 'reopen to todo: scope changed%'",
        ),
    );
}

test "reopen from done writes exactly one task_reopens row with source task-reopen" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "reopen audit" });
    defer deinit(t, a);
    // Advance through doing so markDone is legal (todo → doing → done).
    const doing_t = try update(&d, a, t.id, .{ .status = .doing });
    defer deinit(doing_t, a);
    const done_t = try markDone(&d, a, t.id, false);
    defer deinit(done_t, a);

    const reopened = try reopen(&d, a, t.id, .todo, "reconsider", false);
    defer deinit(reopened, a);
    try std.testing.expectEqual(Status.todo, reopened.status);

    // Count task_reopens rows for this task with the correct source.
    const cnt_sql = try std.fmt.allocPrintSentinel(
        a,
        "select count(*) from task_reopens where task_id = {d} and source = 'task-reopen'",
        .{t.id},
        0,
    );
    defer a.free(cnt_sql);
    try std.testing.expectEqual(@as(i64, 1), try d.intQuery(cnt_sql));

    // No other-source rows should exist.
    const cnt_other_sql = try std.fmt.allocPrintSentinel(
        a,
        "select count(*) from task_reopens where task_id = {d} and source != 'task-reopen'",
        .{t.id},
        0,
    );
    defer a.free(cnt_other_sql);
    try std.testing.expectEqual(@as(i64, 0), try d.intQuery(cnt_other_sql));
}

test "update with force from done writes task_reopens row with source task-update-force" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "force reopen" });
    defer deinit(t, a);
    // Advance through doing so markDone is legal (todo → doing → done).
    const doing_t = try update(&d, a, t.id, .{ .status = .doing });
    defer deinit(doing_t, a);
    const done_t = try markDone(&d, a, t.id, false);
    defer deinit(done_t, a);

    const updated = try update(&d, a, t.id, .{
        .status = .todo,
        .force = true,
        .reason = "force path",
    });
    defer deinit(updated, a);
    try std.testing.expectEqual(Status.todo, updated.status);

    const cnt_sql = try std.fmt.allocPrintSentinel(
        a,
        "select count(*) from task_reopens where task_id = {d} and source = 'task-update-force'",
        .{t.id},
        0,
    );
    defer a.free(cnt_sql);
    try std.testing.expectEqual(@as(i64, 1), try d.intQuery(cnt_sql));
}

test "normal non-reopen transition does not write task_reopens" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "normal transition" });
    defer deinit(t, a);
    // Advance through doing so markDone is legal (todo → doing → done).
    // doing → done is a normal terminal transition, not a reopen.
    const doing_t = try update(&d, a, t.id, .{ .status = .doing });
    defer deinit(doing_t, a);
    const done_t = try markDone(&d, a, t.id, false);
    defer deinit(done_t, a);

    try std.testing.expectEqual(Status.done, done_t.status);
    const cnt_sql = try std.fmt.allocPrintSentinel(
        a,
        "select count(*) from task_reopens where task_id = {d}",
        .{t.id},
        0,
    );
    defer a.free(cnt_sql);
    try std.testing.expectEqual(@as(i64, 0), try d.intQuery(cnt_sql));
}

test "list with scope filter returns only matching rows" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const t_assoc = try create(&d, a, .{ .title = "assoc task", .scope = "acme" });
    defer deinit(t_assoc, a);
    const t_global = try create(&d, a, .{ .title = "global task" });
    defer deinit(t_global, a);

    const filtered = try list(&d, a, .{ .scope = "acme" });
    defer deinitMany(filtered, a);
    try std.testing.expectEqual(@as(usize, 1), filtered.len);
    try std.testing.expectEqualStrings("assoc task", filtered[0].title);
    try std.testing.expectEqual(ScopeKind.association, filtered[0].scope_kind);
}

test "listTouching suppresses direct-repo branch when scope excludes repo" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into projects (slug, name, root_path) values ('r1', 'R1', '/r1')", &.{});
    const repo_id = try d.intQuery("select id from projects where slug='r1'");
    _ = try d.execParams(
        \\insert into tasks (scope_kind, scope_id, title, status, priority)
        \\values ('repo', ?, 'repo-scoped', 'todo', 100)
    , &.{.{ .int = repo_id }});

    const all = try listTouching(&d, a, repo_id, .{});
    defer deinitMany(all, a);
    try std.testing.expectEqual(@as(usize, 1), all.len);

    const global_only = try listTouching(&d, a, repo_id, .{ .scope = "global" });
    defer deinitMany(global_only, a);
    try std.testing.expectEqual(@as(usize, 0), global_only.len);
}

test "update changes title and refreshes updated_at" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "Old" });
    defer deinit(t, a);
    const upd = try update(&d, a, t.id, .{ .title = "New", .priority = 25 });
    defer deinit(upd, a);
    try std.testing.expectEqualStrings("New", upd.title);
    try std.testing.expectEqual(@as(i64, 25), upd.priority);
}

test "update with scope moves task to association scope" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'acme'");
    const t = try create(&d, a, .{ .title = "initially global" });
    defer deinit(t, a);
    try std.testing.expectEqual(ScopeKind.global, t.scope_kind);

    const upd = try update(&d, a, t.id, .{ .scope = "acme" });
    defer deinit(upd, a);
    try std.testing.expectEqual(ScopeKind.association, upd.scope_kind);
    try std.testing.expectEqual(assoc_id, upd.scope_id.?);
}

test "show returns NotFound for missing id" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
}

test "addTouchPath + touchedPaths round-trip path-level touches" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const t = try create(&d, a, .{ .title = "toucher" });
    defer deinit(t, a);
    const repo_id = try d.execParams(
        "insert into projects (slug, name, root_path) values ('r1', 'r1', '/r1')",
        &.{},
    );

    try addTouchPath(&d, t.id, repo_id, "src/a.zig");
    try addTouchPath(&d, t.id, repo_id, "migrations/00099_x.sql");

    const paths = try touchedPaths(&d, a, t.id);
    defer deinitTouchPaths(paths, a);
    try std.testing.expectEqual(@as(usize, 2), paths.len);
    // Ordered by repo_id, path -> "migrations/..." sorts before "src/...".
    try std.testing.expectEqualStrings("migrations/00099_x.sql", paths[0].path);
    try std.testing.expectEqualStrings("src/a.zig", paths[1].path);
    try std.testing.expectEqual(repo_id, paths[0].repo_id);
}

test "addTouchPath is idempotent on the unique(task,repo,path) constraint" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const t = try create(&d, a, .{ .title = "toucher" });
    defer deinit(t, a);
    const repo_id = try d.execParams(
        "insert into projects (slug, name, root_path) values ('r1', 'r1', '/r1')",
        &.{},
    );

    try addTouchPath(&d, t.id, repo_id, "src/a.zig");
    try addTouchPath(&d, t.id, repo_id, "src/a.zig"); // duplicate is a no-op

    const paths = try touchedPaths(&d, a, t.id);
    defer deinitTouchPaths(paths, a);
    try std.testing.expectEqual(@as(usize, 1), paths.len);
}

test "touchedPaths returns empty slice for a task with no declared paths" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const t = try create(&d, a, .{ .title = "no-touches" });
    defer deinit(t, a);

    const paths = try touchedPaths(&d, a, t.id);
    defer deinitTouchPaths(paths, a);
    try std.testing.expectEqual(@as(usize, 0), paths.len);
}

// =========================================================================
// Claim-atomic unit tests (decision 533 / task 4165)
// =========================================================================
//
// These tests cover the TOCTOU guard added to markDone, markBlocked,
// and reopen. Claims are seeded via agentactivity.store.acquireClaim
// (the engine primitive, same as the planar-agent pull path), NOT raw SQL.
//
// Note on import cycle: store.zig imports only db + types.zig — no
// planning module is imported from the runtime side — so this import is
// safe here.

fn seedClaimForTask(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) ![]const u8 {
    // Import only from within the test helper to keep the production code
    // import-free of the runtime layer.
    const agentactivity_store = @import("../runtime/agentactivity/store.zig");

    // Seed a session row first (FK on agent_work_claims.session_id).
    const session_id = d.execParams(
        "insert into sessions (vendor) values ('test-claim')",
        &.{},
    ) catch return error.WriteFailed;

    d.exec("BEGIN IMMEDIATE") catch return error.WriteFailed;
    var committed = false;
    errdefer if (!committed) d.exec("ROLLBACK") catch {};

    const claim = try agentactivity_store.acquireClaim(d, allocator, .{
        .session_id = session_id,
        .entity_kind = .task,
        .entity_id = task_id,
        .vendor = "test",
        .ttl_secs = 3600,
    });
    d.exec("COMMIT") catch {
        claim.deinit(allocator);
        return error.WriteFailed;
    };
    committed = true;
    const token = try allocator.dupe(u8, claim.claim_token);
    claim.deinit(allocator);
    return token;
}

test "markDone: active claim returns TaskClaimed, leaves status doing" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "claimed task", .status = .doing });
    defer deinit(t, a);
    const token = try seedClaimForTask(&d, a, t.id);
    defer a.free(token);

    // Guard must refuse.
    try std.testing.expectError(Error.TaskClaimed, markDone(&d, a, t.id, false));

    // Status must be unchanged.
    const still = try show(&d, a, t.id);
    defer deinit(still, a);
    try std.testing.expectEqual(Status.doing, still.status);
}

test "markDone: force=true bypasses claim guard" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "force done", .status = .doing });
    defer deinit(t, a);
    const token = try seedClaimForTask(&d, a, t.id);
    defer a.free(token);

    const done_t = try markDone(&d, a, t.id, true);
    defer deinit(done_t, a);
    try std.testing.expectEqual(Status.done, done_t.status);
}

test "markDone: unclaimed task transitions normally" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "unclaimed done", .status = .doing });
    defer deinit(t, a);

    const done_t = try markDone(&d, a, t.id, false);
    defer deinit(done_t, a);
    try std.testing.expectEqual(Status.done, done_t.status);
}

test "markDone: expired claim does not block transition" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "expired claim", .status = .doing });
    defer deinit(t, a);

    // Insert an already-expired claim directly (lease_expires_at in the past).
    // Uses the exact schema from migration 00015_agent_activity.up.sql.
    const session_id = try d.execParams(
        "insert into sessions (vendor) values ('test-expired')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into agent_work_claims
        \\  (session_id, entity_kind, entity_id, claim_token, status, vendor,
        \\   lease_expires_at)
        \\values (?, 'task', ?, 'expired-token-4165', 'active', 'test',
        \\        strftime('%Y-%m-%dT%H:%M:%fZ','now','-1 hour'))
    , &.{ .{ .int = session_id }, .{ .int = t.id } });

    // Expired claim must NOT block the transition (lease_expires_at < now).
    const done_t = try markDone(&d, a, t.id, false);
    defer deinit(done_t, a);
    try std.testing.expectEqual(Status.done, done_t.status);
}

test "markBlocked: active claim returns TaskClaimed" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const blocker = try create(&d, a, .{ .title = "blocker" });
    defer deinit(blocker, a);
    const target = try create(&d, a, .{ .title = "target", .status = .doing });
    defer deinit(target, a);
    const token = try seedClaimForTask(&d, a, target.id);
    defer a.free(token);

    try std.testing.expectError(Error.TaskClaimed, markBlocked(&d, a, target.id, blocker.id, null, false));

    const still = try show(&d, a, target.id);
    defer deinit(still, a);
    try std.testing.expectEqual(Status.doing, still.status);
}

test "markBlocked: force=true bypasses claim guard" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const blocker = try create(&d, a, .{ .title = "blocker" });
    defer deinit(blocker, a);
    const target = try create(&d, a, .{ .title = "target", .status = .doing });
    defer deinit(target, a);
    const token = try seedClaimForTask(&d, a, target.id);
    defer a.free(token);

    const blocked = try markBlocked(&d, a, target.id, blocker.id, null, true);
    defer deinit(blocked, a);
    try std.testing.expectEqual(Status.blocked, blocked.status);
}

test "reopen: active claim on a done task returns TaskClaimed" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Create task, mark done without a claim, then add a claim (simulates a
    // claim acquired post-done or a claim on a doing task before it was done).
    const t = try create(&d, a, .{ .title = "reopen guard", .status = .done });
    defer deinit(t, a);
    const token = try seedClaimForTask(&d, a, t.id);
    defer a.free(token);

    try std.testing.expectError(Error.TaskClaimed, reopen(&d, a, t.id, .todo, "redo", false));

    const still = try show(&d, a, t.id);
    defer deinit(still, a);
    try std.testing.expectEqual(Status.done, still.status);
}

test "reopen: force=true bypasses claim guard" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "force reopen guard", .status = .done });
    defer deinit(t, a);
    const token = try seedClaimForTask(&d, a, t.id);
    defer a.free(token);

    const reopened = try reopen(&d, a, t.id, .todo, "override", true);
    defer deinit(reopened, a);
    try std.testing.expectEqual(Status.todo, reopened.status);
}

test "update with status change: active claim returns TaskClaimed" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "update guard", .status = .doing });
    defer deinit(t, a);
    const token = try seedClaimForTask(&d, a, t.id);
    defer a.free(token);

    try std.testing.expectError(
        Error.TaskClaimed,
        update(&d, a, t.id, .{ .status = .done, .force = false }),
    );

    const still = try show(&d, a, t.id);
    defer deinit(still, a);
    try std.testing.expectEqual(Status.doing, still.status);
}

test "update with status change: force=true bypasses claim guard" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "update force guard", .status = .doing });
    defer deinit(t, a);
    const token = try seedClaimForTask(&d, a, t.id);
    defer a.free(token);

    const updated = try update(&d, a, t.id, .{ .status = .done, .force = true });
    defer deinit(updated, a);
    try std.testing.expectEqual(Status.done, updated.status);
}

test "update without status change: active claim does NOT block non-status updates" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const t = try create(&d, a, .{ .title = "update no-status", .status = .doing });
    defer deinit(t, a);
    const token = try seedClaimForTask(&d, a, t.id);
    defer a.free(token);

    // Title-only update — no status change → no claim guard applies.
    const updated = try update(&d, a, t.id, .{ .title = "New Title" });
    defer deinit(updated, a);
    try std.testing.expectEqualStrings("New Title", updated.title);
    try std.testing.expectEqual(Status.doing, updated.status);
}

test "task_touch_paths cascade-deletes when the task is removed" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const t = try create(&d, a, .{ .title = "toucher" });
    const task_id = t.id;
    deinit(t, a);
    const repo_id = try d.execParams(
        "insert into projects (slug, name, root_path) values ('r1', 'r1', '/r1')",
        &.{},
    );
    try addTouchPath(&d, task_id, repo_id, "src/a.zig");
    _ = try d.execParams("delete from tasks where id = ?", &.{.{ .int = task_id }});

    const remaining = try d.intQuery("select count(*) from task_touch_paths");
    try std.testing.expectEqual(@as(i64, 0), remaining);
}
