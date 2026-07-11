//! Scenario coverage, gap, and orphan-analysis cockpit queries.

const std = @import("std");
const db = @import("db");
const common = @import("common.zig");
const StatusBadge = common.StatusBadge;
const ScopeFilter = common.ScopeFilter;
const scenarioStatusBadge = @import("scope_explorer.zig").scenarioStatusBadge;

// Test Scenario & Coverage view-model  (tasks 4025, 4026)
// =========================================================================

/// One task that a scenario verifies (from entity_links).
///
/// Direction confirmed from engine/planning/test_spec_status.zig and
/// engine/ingestor/apply.zig:
///   from_kind='test_scenario', from_id=scenario_id,
///   to_kind='task', to_id=task_id, relationship='verifies'
///
/// The scenario "verifies" the task (scenario is the source/from side).
pub const ScenarioVerifiesRow = struct {
    /// task id that the scenario verifies.
    task_id: i64,
    /// Human-readable label: "task:<id> — <title>".
    label: []const u8,

    pub fn deinit(self: ScenarioVerifiesRow, allocator: std.mem.Allocator) void {
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []ScenarioVerifiesRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One row in the Coverage view navigator list: a test scenario with its
/// verifies→task links.
///
/// Schema reference: migrations/00003_work_items.up.sql
///   test_scenarios(id, scope_kind, scope_id, title, body, status,
///                  related_artifact_id, last_run_at, last_outcome,
///                  created_at, updated_at)
///   status check: ('draft','ready','verified','failing','retired')
pub const ScenarioCoverageRow = struct {
    id: i64,
    /// Scenario title.
    title: []const u8,
    /// Status badge.
    badge: StatusBadge,
    /// Status string for display.
    status: []const u8,
    /// Pre-formatted display string: "[badge] status  title" — heap-allocated
    /// so grapheme pointers from printSegment remain valid after render returns.
    display_text: []const u8,
    /// Scenario body (markdown), or null when the column is NULL in the DB.
    /// Owned by this struct; freed via deinit.
    body: ?[]const u8,
    /// Tasks this scenario verifies (may be empty = orphan scenario).
    verifies: []ScenarioVerifiesRow,

    pub fn deinit(self: ScenarioCoverageRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.status);
        allocator.free(self.display_text);
        if (self.body) |b| allocator.free(b);
        ScenarioVerifiesRow.deinitMany(self.verifies, allocator);
    }

    pub fn deinitMany(rows: []ScenarioCoverageRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Coverage gap data for the gap surface (task 4026).
///
/// Two gap classes:
///   (i)  Orphan scenarios: test_scenarios with NO verifies edge from them.
///   (ii) Uncovered tasks: tasks with NO scenario verifying them.
///
/// All fields are caller-owned; free via `CoverageGap.deinit`.
pub const CoverageGap = struct {
    /// Scenario rows with zero verifies edges (orphan scenarios).
    orphan_scenarios: []OrphanScenarioRow,
    /// Task rows not verified by any scenario (uncovered tasks).
    uncovered_tasks: []UncoveredTaskRow,

    pub fn deinit(self: CoverageGap, allocator: std.mem.Allocator) void {
        OrphanScenarioRow.deinitMany(self.orphan_scenarios, allocator);
        UncoveredTaskRow.deinitMany(self.uncovered_tasks, allocator);
    }
};

/// One orphan scenario: a test_scenario with no outgoing verifies edge.
pub const OrphanScenarioRow = struct {
    id: i64,
    /// Scenario title.
    title: []const u8,
    /// Human-readable label: "scenario:<id> — <title>".
    label: []const u8,

    pub fn deinit(self: OrphanScenarioRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []OrphanScenarioRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One uncovered task: a task with no test_scenario verifying it.
pub const UncoveredTaskRow = struct {
    id: i64,
    /// Task title.
    title: []const u8,
    /// Task status.
    status: []const u8,
    /// Human-readable label: "task:<id> — <title>".
    label: []const u8,

    pub fn deinit(self: UncoveredTaskRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.status);
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []UncoveredTaskRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Query test scenarios with their verifies→task links (task 4025).
///
/// Returns all test_scenarios (within the scope filter) ordered by
/// created_at desc, id desc. For each scenario, the verifies slice
/// lists the tasks it verifies via entity_links.
///
/// verifies direction (confirmed from engine/planning/test_spec_status.zig
/// and engine/ingestor/apply.zig):
///   from_kind='test_scenario', from_id=scenario_id,
///   to_kind='task', to_id=task_id, relationship='verifies'
///
/// Caller owns the result; free via `ScenarioCoverageRow.deinitMany`.
pub fn queryScenarioCoverage(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: ScopeFilter,
) ![]ScenarioCoverageRow {
    const sql_all =
        \\select id, title, status, body
        \\from test_scenarios
        \\order by created_at desc, id desc
    ;
    const sql_repo =
        \\select id, title, status, body
        \\from test_scenarios
        \\where (scope_kind = 'repo' and scope_id = ?) or scope_kind = 'global'
        \\order by created_at desc, id desc
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

    var out: std.ArrayList(ScenarioCoverageRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const status_text = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(status_text);
                // body is nullable (test_scenarios.body text without NOT NULL).
                const body_opt = try stmt.columnTextOpt(3, allocator);
                errdefer if (body_opt) |b| allocator.free(b);

                const badge = scenarioStatusBadge(status_text);

                // Pre-format display string so grapheme pointers remain valid.
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s}  {s}",
                    .{ badge.glyph(), status_text, title },
                );
                errdefer allocator.free(display_text);

                // Query verifies→task links for this scenario.
                const verifies = try queryScenarioVerifies(d, allocator, id);
                errdefer ScenarioVerifiesRow.deinitMany(verifies, allocator);

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .badge = badge,
                    .status = status_text,
                    .display_text = display_text,
                    .body = body_opt,
                    .verifies = verifies,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query the tasks that a scenario verifies (outgoing verifies edges).
///
/// verifies direction:
///   from_kind='test_scenario', from_id=scenario_id,
///   to_kind='task', to_id=task_id, relationship='verifies'
///
/// Caller owns the result; free via `ScenarioVerifiesRow.deinitMany`.
fn queryScenarioVerifies(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    scenario_id: i64,
) ![]ScenarioVerifiesRow {
    var stmt = d.prepare(
        \\select el.to_id, t.title
        \\from entity_links el
        \\left join tasks t on t.id = el.to_id
        \\where el.from_kind = 'test_scenario'
        \\  and el.from_id = ?
        \\  and el.to_kind = 'task'
        \\  and el.relationship = 'verifies'
        \\order by el.to_id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = scenario_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(ScenarioVerifiesRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const task_id = stmt.columnInt(0);
                const task_title_opt = try stmt.columnTextOpt(1, allocator);
                defer if (task_title_opt) |t| allocator.free(t);

                // Build label: "task:<id> — <title>" or "task:<id>".
                const label = if (task_title_opt) |t|
                    try std.fmt.allocPrint(allocator, "task:{d} — {s}", .{ task_id, t })
                else
                    try std.fmt.allocPrint(allocator, "task:{d}", .{task_id});
                errdefer allocator.free(label);

                try out.append(allocator, .{
                    .task_id = task_id,
                    .label = label,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query coverage gaps within the given scope filter (task 4026).
///
/// Gap class (i): Orphan scenarios — test_scenarios with NO outgoing
/// verifies edge (no tasks they verify).
///
/// Gap class (ii): Uncovered tasks — tasks in non-cancelled status with
/// NO test_scenario verifying them (no incoming verifies edge).
///
/// Caller owns the result; free via `CoverageGap.deinit`.
pub fn queryCoverageGap(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: ScopeFilter,
) !CoverageGap {
    // ---- Orphan scenarios (gap class i) ----------------------------------
    // Scenarios with no outgoing 'verifies' edge from them.
    const orphan_sql_all =
        \\select ts.id, ts.title
        \\from test_scenarios ts
        \\where not exists (
        \\  select 1 from entity_links el
        \\  where el.from_kind = 'test_scenario'
        \\    and el.from_id = ts.id
        \\    and el.to_kind = 'task'
        \\    and el.relationship = 'verifies'
        \\)
        \\order by ts.created_at desc, ts.id desc
    ;
    const orphan_sql_repo =
        \\select ts.id, ts.title
        \\from test_scenarios ts
        \\where ((ts.scope_kind = 'repo' and ts.scope_id = ?) or ts.scope_kind = 'global')
        \\  and not exists (
        \\    select 1 from entity_links el
        \\    where el.from_kind = 'test_scenario'
        \\      and el.from_id = ts.id
        \\      and el.to_kind = 'task'
        \\      and el.relationship = 'verifies'
        \\  )
        \\order by ts.created_at desc, ts.id desc
    ;

    var orphan_stmt = switch (filter) {
        .all => blk: {
            var s = d.prepare(orphan_sql_all) catch return error.QueryFailed;
            s.bind(&.{}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
        .repo => blk: {
            var s = d.prepare(orphan_sql_repo) catch return error.QueryFailed;
            s.bind(&.{.{ .int = filter.repo }}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
    };
    defer orphan_stmt.finalize();

    var orphans: std.ArrayList(OrphanScenarioRow) = .empty;
    errdefer {
        for (orphans.items) |r| r.deinit(allocator);
        orphans.deinit(allocator);
    }

    while (true) {
        switch (orphan_stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = orphan_stmt.columnInt(0);
                const title = try orphan_stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);

                // Build label heap-owned; keep dup/free symmetric.
                const label = try std.fmt.allocPrint(
                    allocator,
                    "scenario:{d} — {s}",
                    .{ id, title },
                );
                errdefer allocator.free(label);

                try orphans.append(allocator, .{
                    .id = id,
                    .title = title,
                    .label = label,
                });
            },
        }
    }

    // ---- Uncovered tasks (gap class ii) ----------------------------------
    // Tasks (non-cancelled) with no incoming 'verifies' edge pointing at them.
    const uncov_sql_all =
        \\select t.id, t.title, t.status
        \\from tasks t
        \\where t.status not in ('cancelled')
        \\  and not exists (
        \\    select 1 from entity_links el
        \\    where el.from_kind = 'test_scenario'
        \\      and el.to_kind = 'task'
        \\      and el.to_id = t.id
        \\      and el.relationship = 'verifies'
        \\  )
        \\order by t.priority asc, t.id asc
    ;
    const uncov_sql_repo =
        \\select t.id, t.title, t.status
        \\from tasks t
        \\where t.status not in ('cancelled')
        \\  and ((t.scope_kind = 'repo' and t.scope_id = ?) or t.scope_kind = 'global')
        \\  and not exists (
        \\    select 1 from entity_links el
        \\    where el.from_kind = 'test_scenario'
        \\      and el.to_kind = 'task'
        \\      and el.to_id = t.id
        \\      and el.relationship = 'verifies'
        \\  )
        \\order by t.priority asc, t.id asc
    ;

    var uncov_stmt = switch (filter) {
        .all => blk: {
            var s = d.prepare(uncov_sql_all) catch return error.QueryFailed;
            s.bind(&.{}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
        .repo => blk: {
            var s = d.prepare(uncov_sql_repo) catch return error.QueryFailed;
            s.bind(&.{.{ .int = filter.repo }}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
    };
    defer uncov_stmt.finalize();

    var uncovered: std.ArrayList(UncoveredTaskRow) = .empty;
    errdefer {
        for (uncovered.items) |r| r.deinit(allocator);
        uncovered.deinit(allocator);
    }

    while (true) {
        switch (uncov_stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = uncov_stmt.columnInt(0);
                const title = try uncov_stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const status_text = try uncov_stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(status_text);

                // Build label heap-owned; keep dup/free symmetric.
                const label = try std.fmt.allocPrint(
                    allocator,
                    "task:{d} — {s}",
                    .{ id, title },
                );
                errdefer allocator.free(label);

                try uncovered.append(allocator, .{
                    .id = id,
                    .title = title,
                    .status = status_text,
                    .label = label,
                });
            },
        }
    }

    return .{
        .orphan_scenarios = try orphans.toOwnedSlice(allocator),
        .uncovered_tasks = try uncovered.toOwnedSlice(allocator),
    };
}

// =========================================================================
