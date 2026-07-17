//! engine/health — database + runtime-health introspection.
//!
//! Returns a `Report` describing the state of the local Planar store.
//! Goes alongside the `planar health` CLI verb but is independent of it
//! — integration tests and future read-side viewers can call `check`
//! directly.
//!
//! Cluster C-health-content-loss (plan 351, Q235): mirrors Go's rich
//! field set. Each in-flight task (status in {doing, blocked}) is
//! classified resumable vs not (needs non-empty next_action + at least
//! one context_snapshot row), pending handoffs are bucketed into
//! stale vs fresh, the schema-migration ledger is compared against
//! embedded_max, and a final `overall` classification ("ok" /
//! "degraded") rolls everything up. The CLI handler exits 1 on
//! "degraded" so oncall / CI scrapes get a meaningful non-zero.

const std = @import("std");
const db = @import("db");
const installedsurface = @import("installedsurface.zig");
const identity = @import("identity.zig");

pub const Error = error{
    SchemaTableMissing,
    QueryFailed,
};

pub const ProjectionFreshness = struct {
    state: []const u8,
    manifest_status: installedsurface.ManifestState,
    managed: usize,
    fresh: usize,
    stale: usize,
    missing: usize,
    unmanaged: usize,
    unselected_vendors: usize,
    evidence: ?[]const u8,
    repair_command: ?[]const u8,
};

/// Snapshot of database + handoff health. Field order is the wire
/// format — `std.json.stringify` picks it up directly when the handler
/// runs in `--json` mode, and the text renderer prints fields in
/// declaration order.
pub const Report = struct {
    db_path: []const u8,
    db_ok: bool,
    schema_version: i64,
    schema_target: i64,
    schema_current: bool,
    migration_count: i64,
    integrity_ok: bool,
    inflight_tasks: i64,
    resumable_tasks: i64,
    not_resumable_tasks: i64,
    pending_handoffs: i64,
    stale_handoffs: i64,
    projection_freshness: ProjectionFreshness,
    overall: []const u8,
};

/// Stale-handoff cutoff. A pending / validated handoff older than this
/// is flagged in `stale_handoffs`. 24 hours mirrors the Go default.
pub const stale_handoff_threshold_hours: i64 = 24;

/// Per-status task counts attached to a draft plan.
pub const TaskCounts = struct { todo: i64, doing: i64, blocked: i64, done: i64, cancelled: i64 };

/// A draft plan which is either an empty wrapper or has only terminal tasks.
pub const StaleDraftPlan = struct {
    id: i64,
    parent_plan_id: ?i64,
    title: []const u8,
    reason: []const u8,
    task_counts: TaskCounts,
    suggestion: []const u8,
};

/// A task which has remained in `doing` beyond the configured threshold.
pub const StaleDoingTask = struct { id: i64, plan_id: i64, scope: []const u8, title: []const u8, age_days: i64, suggestion: []const u8 };

/// A question which has remained open beyond the configured threshold.
pub const StaleOpenQuestion = struct { id: i64, title: []const u8, age_days: i64, suggestion: []const u8 };

/// Thresholds echoed in the hygiene JSON response.
pub const HygieneThresholds = struct { stale_doing_days: i64, stale_open_days: i64 };

/// Read-only lifecycle-drift report emitted by `planar health hygiene`.
pub const HygieneReport = struct {
    thresholds: HygieneThresholds,
    stale_draft_plans: []StaleDraftPlan,
    stale_doing_tasks: []StaleDoingTask,
    stale_open_questions: []StaleOpenQuestion,

    /// Release all allocations owned by the report.
    pub fn deinit(self: HygieneReport, allocator: std.mem.Allocator) void {
        deinitStalePlans(self.stale_draft_plans, allocator);
        deinitStaleTasks(self.stale_doing_tasks, allocator);
        deinitStaleQuestions(self.stale_open_questions, allocator);
    }
};

/// Options for lifecycle hygiene collection. A null scope reports all scopes.
pub const HygieneOptions = struct {
    scope: ?[]const u8 = null,
    stale_doing_days: i64 = 7,
    stale_open_days: i64 = 30,
    /// Optional fixed clock for deterministic engine tests.
    now: ?[]const u8 = null,
};

pub const HygieneError = error{ InvalidThreshold, UnsupportedScope, SlugNotFound, QueryFailed } || std.mem.Allocator.Error;

/// Collect plan, task, and question lifecycle drift without mutating state.
pub fn hygiene(d: *db.sqlite.Db, allocator: std.mem.Allocator, options: HygieneOptions) HygieneError!HygieneReport {
    if (options.stale_doing_days < 0 or options.stale_open_days < 0) return error.InvalidThreshold;
    const association_id: ?i64 = if (options.scope) |raw| blk: {
        const resolved = identity.scope.resolveSlug(d, allocator, raw) catch |err| switch (err) {
            error.SlugNotFound => return error.SlugNotFound,
            else => return error.QueryFailed,
        };
        if (resolved.kind != .association or resolved.id == null) return error.UnsupportedScope;
        break :blk resolved.id.?;
    } else null;

    const plans = try queryStaleDraftPlans(d, allocator, association_id);
    errdefer deinitStalePlans(plans, allocator);
    const tasks = try queryStaleDoingTasks(d, allocator, association_id, options.stale_doing_days, options.now);
    errdefer deinitStaleTasks(tasks, allocator);
    const questions = try queryStaleOpenQuestions(d, allocator, association_id, options.stale_open_days, options.now);
    errdefer deinitStaleQuestions(questions, allocator);
    return .{
        .thresholds = .{ .stale_doing_days = options.stale_doing_days, .stale_open_days = options.stale_open_days },
        .stale_draft_plans = plans,
        .stale_doing_tasks = tasks,
        .stale_open_questions = questions,
    };
}

fn queryStaleDraftPlans(d: *db.sqlite.Db, allocator: std.mem.Allocator, association_id: ?i64) HygieneError![]StaleDraftPlan {
    const sql: [:0]const u8 =
        \\select p.id, p.parent_plan_id, p.title,
        \\ sum(case when t.status='todo' then 1 else 0 end),
        \\ sum(case when t.status='doing' then 1 else 0 end),
        \\ sum(case when t.status='blocked' then 1 else 0 end),
        \\ sum(case when t.status='done' then 1 else 0 end),
        \\ sum(case when t.status='cancelled' then 1 else 0 end), count(t.id)
        \\from plans p left join tasks t on t.plan_id=p.id
        \\where p.status='draft' and (? is null or (p.scope_kind='association' and p.scope_id=?))
        \\group by p.id, p.parent_plan_id, p.title
        \\having count(t.id)=0 or sum(case when t.status not in ('done','cancelled') then 1 else 0 end)=0
        \\order by p.id
    ;
    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ scopeParam(association_id), scopeParam(association_id) }) catch return error.QueryFailed;
    var rows: std.ArrayList(StaleDraftPlan) = .empty;
    errdefer deinitStalePlanList(&rows, allocator);
    while (true) switch (stmt.step() catch return error.QueryFailed) {
        .done => return try rows.toOwnedSlice(allocator),
        .row => {
            const id = stmt.columnInt(0);
            const task_count = stmt.columnInt(8);
            const title = try stmt.columnTextAlloc(2, allocator);
            errdefer allocator.free(title);
            const suggestion = try std.fmt.allocPrint(allocator, "planar plan update {d} --status {s}", .{ id, if (task_count == 0) "abandoned" else "done" });
            errdefer allocator.free(suggestion);
            try rows.append(allocator, .{
                .id = id,
                .parent_plan_id = stmt.columnIntOpt(1),
                .title = title,
                .reason = if (task_count == 0) "zero_tasks" else "all_tasks_terminal",
                .task_counts = .{ .todo = stmt.columnInt(3), .doing = stmt.columnInt(4), .blocked = stmt.columnInt(5), .done = stmt.columnInt(6), .cancelled = stmt.columnInt(7) },
                .suggestion = suggestion,
            });
        },
    };
}

fn queryStaleDoingTasks(d: *db.sqlite.Db, allocator: std.mem.Allocator, association_id: ?i64, threshold_days: i64, now: ?[]const u8) HygieneError![]StaleDoingTask {
    const sql: [:0]const u8 =
        \\select t.id, t.plan_id, t.title,
        \\ cast(julianday(coalesce(?, 'now'))-julianday(t.updated_at) as integer),
        \\ case t.scope_kind
        \\   when 'global' then 'global'
        \\   when 'association' then 'assoc:' || a.slug
        \\   when 'repo' then 'repo:' || p.slug
        \\ end
        \\from tasks t
        \\left join associations a on t.scope_kind='association' and a.id=t.scope_id
        \\left join projects p on t.scope_kind='repo' and p.id=t.scope_id
        \\where t.status='doing' and julianday(coalesce(?, 'now'))-julianday(t.updated_at)>?
        \\ and (? is null or (t.scope_kind='association' and t.scope_id=?)) order by t.id
    ;
    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ timeParam(now), timeParam(now), .{ .int = threshold_days }, scopeParam(association_id), scopeParam(association_id) }) catch return error.QueryFailed;
    var rows: std.ArrayList(StaleDoingTask) = .empty;
    errdefer deinitStaleTaskList(&rows, allocator);
    while (true) switch (stmt.step() catch return error.QueryFailed) {
        .done => return try rows.toOwnedSlice(allocator),
        .row => {
            const id = stmt.columnInt(0);
            const title = try stmt.columnTextAlloc(2, allocator);
            errdefer allocator.free(title);
            const scope = try stmt.columnTextAlloc(4, allocator);
            errdefer allocator.free(scope);
            const suggestion = try std.fmt.allocPrint(allocator, "planar task update {d} --scope {s} --status done OR planar task update {d} --scope {s} --status blocked", .{ id, scope, id, scope });
            errdefer allocator.free(suggestion);
            try rows.append(allocator, .{ .id = id, .plan_id = stmt.columnInt(1), .scope = scope, .title = title, .age_days = stmt.columnInt(3), .suggestion = suggestion });
        },
    };
}

fn queryStaleOpenQuestions(d: *db.sqlite.Db, allocator: std.mem.Allocator, association_id: ?i64, threshold_days: i64, now: ?[]const u8) HygieneError![]StaleOpenQuestion {
    const sql: [:0]const u8 =
        \\select id, title, cast(julianday(coalesce(?, 'now'))-julianday(updated_at) as integer)
        \\from questions where status='open' and julianday(coalesce(?, 'now'))-julianday(updated_at)>?
        \\ and (? is null or (scope_kind='association' and scope_id=?)) order by id
    ;
    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ timeParam(now), timeParam(now), .{ .int = threshold_days }, scopeParam(association_id), scopeParam(association_id) }) catch return error.QueryFailed;
    var rows: std.ArrayList(StaleOpenQuestion) = .empty;
    errdefer deinitStaleQuestionList(&rows, allocator);
    while (true) switch (stmt.step() catch return error.QueryFailed) {
        .done => return try rows.toOwnedSlice(allocator),
        .row => {
            const id = stmt.columnInt(0);
            const title = try stmt.columnTextAlloc(1, allocator);
            errdefer allocator.free(title);
            const suggestion = try std.fmt.allocPrint(allocator, "planar question answer {d} --answer \"<resolution>\" OR planar question wontfix {d}", .{ id, id });
            errdefer allocator.free(suggestion);
            try rows.append(allocator, .{ .id = id, .title = title, .age_days = stmt.columnInt(2), .suggestion = suggestion });
        },
    };
}

fn scopeParam(id: ?i64) db.sqlite.Param {
    return if (id) |value| .{ .int = value } else .{ .null = {} };
}

fn timeParam(now: ?[]const u8) db.sqlite.Param {
    return if (now) |value| .{ .text = value } else .{ .null = {} };
}

fn deinitStalePlanList(rows: *std.ArrayList(StaleDraftPlan), allocator: std.mem.Allocator) void {
    for (rows.items) |row| {
        allocator.free(row.title);
        allocator.free(row.suggestion);
    }
    rows.deinit(allocator);
}
fn deinitStaleTaskList(rows: *std.ArrayList(StaleDoingTask), allocator: std.mem.Allocator) void {
    for (rows.items) |row| {
        allocator.free(row.scope);
        allocator.free(row.title);
        allocator.free(row.suggestion);
    }
    rows.deinit(allocator);
}
fn deinitStaleQuestionList(rows: *std.ArrayList(StaleOpenQuestion), allocator: std.mem.Allocator) void {
    for (rows.items) |row| {
        allocator.free(row.title);
        allocator.free(row.suggestion);
    }
    rows.deinit(allocator);
}
fn deinitStalePlans(rows: []StaleDraftPlan, allocator: std.mem.Allocator) void {
    for (rows) |row| {
        allocator.free(row.title);
        allocator.free(row.suggestion);
    }
    allocator.free(rows);
}
fn deinitStaleTasks(rows: []StaleDoingTask, allocator: std.mem.Allocator) void {
    for (rows) |row| {
        allocator.free(row.scope);
        allocator.free(row.title);
        allocator.free(row.suggestion);
    }
    allocator.free(rows);
}
fn deinitStaleQuestions(rows: []StaleOpenQuestion, allocator: std.mem.Allocator) void {
    for (rows) |row| {
        allocator.free(row.title);
        allocator.free(row.suggestion);
    }
    allocator.free(rows);
}

/// Render a hygiene report with actionable commands in each section.
pub fn renderHygieneText(report: HygieneReport, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("=== Stale draft plans (all tasks terminal) ===\n", .{});
    if (report.stale_draft_plans.len == 0) try writer.print("  none\n", .{});
    for (report.stale_draft_plans) |row| {
        if (row.parent_plan_id) |parent| try writer.print("  plan {d} (parent: plan:{d}): \"{s}\"\n", .{ row.id, parent, row.title }) else try writer.print("  plan {d}: \"{s}\"\n", .{ row.id, row.title });
        try writer.print("    tasks: {d} todo, {d} doing, {d} blocked, {d} done, {d} cancelled\n", .{ row.task_counts.todo, row.task_counts.doing, row.task_counts.blocked, row.task_counts.done, row.task_counts.cancelled });
        try writer.print("    suggest: {s}  ({s})\n", .{ row.suggestion, if (std.mem.eql(u8, row.reason, "zero_tasks")) "no tasks ever attached" else "all tasks terminal" });
    }
    try writer.print("\n=== Stale doing tasks (status=doing for >{d} days) ===\n", .{report.thresholds.stale_doing_days});
    if (report.stale_doing_tasks.len == 0) try writer.print("  none\n", .{});
    for (report.stale_doing_tasks) |row| {
        try writer.print("  task {d} (plan {d}, last touched {d}d ago): \"{s}\"\n", .{ row.id, row.plan_id, row.age_days, row.title });
        try writer.print("    suggest: {s}\n", .{row.suggestion});
    }
    try writer.print("\n=== Stale open questions (status=open for >{d} days) ===\n", .{report.thresholds.stale_open_days});
    if (report.stale_open_questions.len == 0) try writer.print("  none\n", .{});
    for (report.stale_open_questions) |row| {
        try writer.print("  question {d} ({d}d old): \"{s}\"\n", .{ row.id, row.age_days, row.title });
        try writer.print("    suggest: {s}\n", .{row.suggestion});
    }
}

/// Snapshot the current health of the supplied database. Pure read:
/// issues single-row queries and assembles the struct. No IO, no
/// formatting — the caller emits the result.
///
/// `db_path` is borrowed from the runtime context for the duration of
/// the report; it is not freed by the engine.
pub fn check(d: *db.sqlite.Db, db_path: []const u8) Error!Report {
    const schema_version = d.intQuery(
        "select coalesce(max(version), 0) from schema_migrations",
    ) catch return Error.SchemaTableMissing;

    const migration_count = d.intQuery(
        "select count(*) from schema_migrations",
    ) catch return Error.QueryFailed;

    const schema_target: i64 = @intCast(db.migrate.embedded_max);

    const integrity_ok = checkIntegrity(d);

    // In-flight tasks (status in {doing, blocked}) bucketed into
    // resumable vs not-resumable. Resumable requires a non-empty
    // next_action and at least one context_snapshot row.
    const inflight_tasks = d.intQuery(
        "select count(*) from tasks where status in ('doing','blocked')",
    ) catch 0;

    const resumable_tasks = d.intQuery(
        "select count(*) from tasks t" ++
            " where t.status in ('doing','blocked')" ++
            "   and coalesce(t.next_action,'') != ''" ++
            "   and exists (select 1 from context_snapshots cs where cs.task_id = t.id)",
    ) catch 0;

    const not_resumable_tasks = inflight_tasks - resumable_tasks;

    const pending_handoffs = d.intQuery(
        "select count(*) from handoffs where status in ('pending', 'validated')",
    ) catch 0;

    var stale_buf: [256]u8 = undefined;
    const stale_sql = std.fmt.bufPrintZ(
        &stale_buf,
        "select count(*) from handoffs" ++
            " where status in ('pending','validated')" ++
            "   and (julianday('now') - julianday(created_at)) * 24 > {d}",
        .{stale_handoff_threshold_hours},
    ) catch return Error.QueryFailed;
    const stale_handoffs = d.intQuery(stale_sql) catch 0;

    const db_ok = true;
    const schema_current = schema_version == schema_target;
    const degraded = !db_ok or !integrity_ok or not_resumable_tasks > 0 or stale_handoffs > 0;

    return .{
        .db_path = db_path,
        .db_ok = db_ok,
        .schema_version = schema_version,
        .schema_target = schema_target,
        .schema_current = schema_current,
        .migration_count = migration_count,
        .integrity_ok = integrity_ok,
        .inflight_tasks = inflight_tasks,
        .resumable_tasks = resumable_tasks,
        .not_resumable_tasks = not_resumable_tasks,
        .pending_handoffs = pending_handoffs,
        .stale_handoffs = stale_handoffs,
        .projection_freshness = .{
            .state = "not_installed",
            .manifest_status = .missing,
            .managed = 0,
            .fresh = 0,
            .stale = 0,
            .missing = 0,
            .unmanaged = 0,
            .unselected_vendors = installedsurface.supported_vendors.len,
            .evidence = "no managed Planar installation is recorded",
            .repair_command = null,
        },
        .overall = if (degraded) "degraded" else "ok",
    };
}

/// Fold the read-only installed-surface classifier into an existing database
/// health report. Classification remains owned by `installedsurface.status`;
/// this function only summarizes that result as a health contributor.
pub fn withProjectionFreshness(report_in: Report, status: installedsurface.StatusResult) Report {
    var report = report_in;
    const managed = status.summary.fresh + status.summary.stale + status.summary.missing;
    const manifest_degraded = switch (status.manifest_status) {
        .legacy, .invalid, .unsupported => true,
        .current, .missing => false,
    };
    const managed_degraded = status.summary.stale > 0 or status.summary.missing > 0;
    const degraded = manifest_degraded or managed_degraded;
    const state: []const u8 = if (degraded)
        "degraded"
    else if (status.manifest_status == .missing)
        "not_installed"
    else
        "fresh";
    const evidence: ?[]const u8 = if (status.reason) |reason|
        reason
    else if (managed_degraded)
        "managed projections differ from the staged installation authority"
    else
        null;
    report.projection_freshness = .{
        .state = state,
        .manifest_status = status.manifest_status,
        .managed = managed,
        .fresh = status.summary.fresh,
        .stale = status.summary.stale,
        .missing = status.summary.missing,
        .unmanaged = status.summary.unmanaged,
        .unselected_vendors = status.summary.unselected_vendors,
        .evidence = evidence,
        .repair_command = if (degraded) status.repair_command else null,
    };
    if (degraded) report.overall = "degraded";
    return report;
}

/// Run `PRAGMA integrity_check` and return true when SQLite reports
/// the single "ok" row. Any other outcome (locked DB, corruption,
/// errors) is treated as a fail-safe `false`.
fn checkIntegrity(d: *db.sqlite.Db) bool {
    var stmt = d.prepare("PRAGMA integrity_check") catch return false;
    defer stmt.finalize();
    const step_result = stmt.step() catch return false;
    switch (step_result) {
        .done => return false,
        .row => {
            var name_buf: [64]u8 = undefined;
            var fba = std.heap.FixedBufferAllocator.init(&name_buf);
            const txt = stmt.columnTextAlloc(0, fba.allocator()) catch return false;
            return std.mem.eql(u8, txt, "ok");
        },
    }
}

/// Render the report as a multi-line plain-text block. The CLI handler
/// calls this for non-JSON mode; tests can also call it to assert on
/// the rendered shape. Label set mirrors Go's `planar health` text
/// output.
pub fn renderText(report: Report, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("db:               {s} ({s})\n", .{
        if (report.db_ok) "ok" else "ERROR",
        report.db_path,
    });
    try writer.print("schema:           v{d} of v{d} ({s})\n", .{
        report.schema_version,
        report.schema_target,
        if (report.schema_current) "current" else "behind",
    });
    try writer.print("integrity:        {s}\n", .{
        if (report.integrity_ok) "ok" else "FAIL",
    });
    try writer.print("in-flight tasks:  {d} ({d} resumable, {d} NOT resumable)\n", .{
        report.inflight_tasks,
        report.resumable_tasks,
        report.not_resumable_tasks,
    });
    try writer.print("pending handoffs: {d} ({d} stale > {d}h)\n", .{
        report.pending_handoffs,
        report.stale_handoffs,
        stale_handoff_threshold_hours,
    });
    try writer.print("projection freshness: {s} ({d} managed: {d} fresh, {d} stale, {d} missing; {d} unmanaged; {d} unselected vendors)\n", .{
        report.projection_freshness.state,
        report.projection_freshness.managed,
        report.projection_freshness.fresh,
        report.projection_freshness.stale,
        report.projection_freshness.missing,
        report.projection_freshness.unmanaged,
        report.projection_freshness.unselected_vendors,
    });
    try writer.print("projection manifest:  {s}\n", .{@tagName(report.projection_freshness.manifest_status)});
    if (report.projection_freshness.evidence) |evidence| {
        try writer.print("projection evidence:  {s}\n", .{evidence});
    }
    if (report.projection_freshness.repair_command) |command| {
        try writer.print("projection repair:    {s}\n", .{command});
    }
    try writer.print("overall:          {s}\n", .{report.overall});
}

// ---- tests ----

test "hygiene defaults use strict deterministic task and question age boundaries" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Active', 'active', 'active')",
        &.{},
    );
    const plan_id = try d.intQuery("select id from plans where title='Active'");
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, updated_at) values ('global', ?, 'Exactly seven days', 'doing', '2026-05-25T00:00:00.000Z')",
        &.{.{ .int = plan_id }},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, updated_at) values ('global', ?, 'Ten days old', 'doing', '2026-05-22T00:00:00.000Z')",
        &.{.{ .int = plan_id }},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, status, updated_at) values ('global', 'Exactly thirty days', 'open', '2026-05-02T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into questions (scope_kind, title, status, updated_at) values ('global', 'Forty-five days old', 'open', '2026-04-17T00:00:00.000Z')",
        &.{},
    );

    const report = try hygiene(&d, std.testing.allocator, .{ .now = "2026-06-01T00:00:00.000Z" });
    defer report.deinit(std.testing.allocator);

    try std.testing.expectEqual(@as(i64, 7), report.thresholds.stale_doing_days);
    try std.testing.expectEqual(@as(i64, 30), report.thresholds.stale_open_days);
    try std.testing.expectEqual(@as(usize, 1), report.stale_doing_tasks.len);
    try std.testing.expectEqualStrings("Ten days old", report.stale_doing_tasks[0].title);
    try std.testing.expectEqual(@as(i64, 10), report.stale_doing_tasks[0].age_days);
    try std.testing.expectEqualStrings("global", report.stale_doing_tasks[0].scope);
    try std.testing.expectEqual(@as(usize, 1), report.stale_open_questions.len);
    try std.testing.expectEqualStrings("Forty-five days old", report.stale_open_questions[0].title);
    try std.testing.expectEqual(@as(i64, 45), report.stale_open_questions[0].age_days);
}

test "check returns SchemaTableMissing against a freshly-opened in-memory DB" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try std.testing.expectError(Error.SchemaTableMissing, check(&d, "/tmp/test.db"));
}

test "check reports the rich field set after migrations are applied" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);
    const report = try check(&d, "/tmp/test.db");
    try std.testing.expect(report.db_ok);
    try std.testing.expect(report.integrity_ok);
    try std.testing.expectEqualStrings("ok", report.overall);
    try std.testing.expectEqual(@as(i64, 0), report.inflight_tasks);
    try std.testing.expectEqual(@as(i64, 0), report.not_resumable_tasks);
}

test "check classifies a doing task with no next_action as not-resumable + degraded" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'doing', 100)",
        &.{},
    );
    const report = try check(&d, "/tmp/test.db");
    try std.testing.expectEqual(@as(i64, 1), report.inflight_tasks);
    try std.testing.expectEqual(@as(i64, 1), report.not_resumable_tasks);
    try std.testing.expectEqualStrings("degraded", report.overall);
}

test "withProjectionFreshness degrades only managed drift and recovery manifest states" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    var current = try installedsurface.status(std.testing.allocator, .{
        .planar_home = "/definitely/not/a/planar/home",
        .home = "/definitely/not/a/home",
        .codex_home = "/definitely/not/a/codex/home",
    });
    defer current.deinit();
    const report = withProjectionFreshness(try check(&d, "/tmp/test.db"), current);
    try std.testing.expectEqualStrings("not_installed", report.projection_freshness.state);
    try std.testing.expectEqualStrings("ok", report.overall);
}
