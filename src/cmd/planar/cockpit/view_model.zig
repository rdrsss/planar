//! Compatibility facade for the Planar cockpit view-model modules.
//!
//! Public names remain re-exported here so cockpit views have one stable
//! import. Domain queries and their owned row types live under `view_model/`.
//! This layer also keeps cross-domain regression tests that exercise the
//! facade exactly as consumers see it.
//!
//! Design invariants:
//!   - Every public struct is allocator-owned: string slices come from
//!     the allocator passed to the producing function; release via the
//!     per-type `deinit` helper.
//!   - No libvaxis / Window / Cell imports. The driver layer (app.zig)
//!     reads these structs each frame.
//!   - Query functions take `*db.sqlite.Db`; the view-model owns no
//!     DB handle.
//!
const std = @import("std");
const db = @import("db");
const common = @import("view_model/common.zig");

// =========================================================================
// Shared primitives
// =========================================================================

pub const StatusBadge = common.StatusBadge;

// Agent-monitor view-model facade.
const agent_monitor = @import("view_model/agent_monitor.zig");
pub const ClaimRow = agent_monitor.ClaimRow;
pub const HeartbeatAge = agent_monitor.HeartbeatAge;
pub const AgentMonitorSnapshot = agent_monitor.AgentMonitorSnapshot;
pub const AgentActionRow = agent_monitor.AgentActionRow;
pub const queryAgentMonitor = agent_monitor.queryAgentMonitor;
pub const classifyHeartbeatAge = agent_monitor.classifyHeartbeatAge;
pub const parseIso8601Unix = agent_monitor.parseIso8601Unix;
pub const queryAgentActionStream = agent_monitor.queryAgentActionStream;

// =========================================================================
// Scope-explorer view-model facade.
const scope_explorer = @import("view_model/scope_explorer.zig");
pub const PlanNode = scope_explorer.PlanNode;
pub const TaskRow = scope_explorer.TaskRow;
pub const DetailKind = scope_explorer.DetailKind;
pub const DetailPane = scope_explorer.DetailPane;
pub const ScopeFilter = scope_explorer.ScopeFilter;
pub const DrillKind = scope_explorer.DrillKind;
pub const DrillRow = scope_explorer.DrillRow;
pub const planStatusBadge = scope_explorer.planStatusBadge;
pub const taskStatusBadge = scope_explorer.taskStatusBadge;
pub const queryPlanNodes = scope_explorer.queryPlanNodes;
pub const queryTaskRows = scope_explorer.queryTaskRows;
pub const queryPlanDetail = scope_explorer.queryPlanDetail;
pub const queryTaskDetail = scope_explorer.queryTaskDetail;
pub const queryPlanNodesFiltered = scope_explorer.queryPlanNodesFiltered;
pub const queryPlanDrillRows = scope_explorer.queryPlanDrillRows;
pub const queryDecisionDetail = scope_explorer.queryDecisionDetail;
pub const queryQuestionDetail = scope_explorer.queryQuestionDetail;
pub const queryScenarioDetail = scope_explorer.queryScenarioDetail;
pub const queryArtifactDetail = scope_explorer.queryArtifactDetail;
const decisionStatusBadge = scope_explorer.decisionStatusBadge;
const questionStatusBadge = scope_explorer.questionStatusBadge;
const scenarioStatusBadge = scope_explorer.scenarioStatusBadge;

// Task-board view-model facade.
const task_board = @import("view_model/task_board.zig");
pub const BoardTaskRow = task_board.BoardTaskRow;
pub const TaskBoardSnapshot = task_board.TaskBoardSnapshot;
pub const TaskReopenRow = task_board.TaskReopenRow;
pub const TaskTouchPathRow = task_board.TaskTouchPathRow;
pub const TaskLinkRow = task_board.TaskLinkRow;
pub const LinkDirection = task_board.LinkDirection;
pub const TaskBoardDetail = task_board.TaskBoardDetail;
pub const queryTaskBoard = task_board.queryTaskBoard;
pub const queryTaskReopens = task_board.queryTaskReopens;
pub const queryTaskTouchPaths = task_board.queryTaskTouchPaths;
pub const queryTaskBlockingLinks = task_board.queryTaskBlockingLinks;
pub const queryTaskBoardDetail = task_board.queryTaskBoardDetail;
pub const cwdScopeProjectId = task_board.cwdScopeProjectId;

// Decision-log view-model facade.
const decision_log = @import("view_model/decision_log.zig");
pub const DecisionLogRow = decision_log.DecisionLogRow;
pub const DecisionDerivesFromRow = decision_log.DecisionDerivesFromRow;
pub const DecisionLogDetail = decision_log.DecisionLogDetail;
pub const queryDecisionLog = decision_log.queryDecisionLog;
pub const queryDecisionDerivesFrom = decision_log.queryDecisionDerivesFrom;
pub const queryDecisionLogDetail = decision_log.queryDecisionLogDetail;

// Open-questions view-model facade.
const open_questions = @import("view_model/open_questions.zig");
pub const QuestionStatusFilter = open_questions.QuestionStatusFilter;
pub const OpenQuestionsRow = open_questions.OpenQuestionsRow;
pub const QuestionLinkedEntity = open_questions.QuestionLinkedEntity;
pub const OpenQuestionsDetail = open_questions.OpenQuestionsDetail;
pub const queryOpenQuestions = open_questions.queryOpenQuestions;
pub const queryQuestionLinkedEntities = open_questions.queryQuestionLinkedEntities;
pub const queryOpenQuestionsDetail = open_questions.queryOpenQuestionsDetail;
pub const ViewId = common.ViewId;

// Coverage view-model facade.
const coverage = @import("view_model/coverage.zig");
pub const ScenarioVerifiesRow = coverage.ScenarioVerifiesRow;
pub const ScenarioCoverageRow = coverage.ScenarioCoverageRow;
pub const CoverageGap = coverage.CoverageGap;
pub const OrphanScenarioRow = coverage.OrphanScenarioRow;
pub const UncoveredTaskRow = coverage.UncoveredTaskRow;
pub const queryScenarioCoverage = coverage.queryScenarioCoverage;
pub const queryCoverageGap = coverage.queryCoverageGap;

// Entity-link graph view-model facade.
const entity_link_graph = @import("view_model/entity_link_graph.zig");
pub const LinkRelationship = entity_link_graph.LinkRelationship;
pub const LinkEdgeDirection = entity_link_graph.LinkEdgeDirection;
pub const EntityLinkRow = entity_link_graph.EntityLinkRow;
pub const EntityFocus = entity_link_graph.EntityFocus;
pub const EntityLinkGraphData = entity_link_graph.EntityLinkGraphData;
pub const queryEntityLinkGraph = entity_link_graph.queryEntityLinkGraph;
pub const queryEntityLinkGraphDefault = entity_link_graph.queryEntityLinkGraphDefault;

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

test "view_model: StatusBadge.glyph returns expected chars" {
    try testing.expectEqualStrings("A", StatusBadge.active.glyph());
    try testing.expectEqualStrings("S", StatusBadge.stale.glyph());
    try testing.expectEqualStrings("D", StatusBadge.done.glyph());
    try testing.expectEqualStrings(">", StatusBadge.doing.glyph());
    try testing.expectEqualStrings("B", StatusBadge.blocked.glyph());
    try testing.expectEqualStrings(" ", StatusBadge.none.glyph());
    try testing.expectEqualStrings("^", StatusBadge.superseded.glyph());
    try testing.expectEqualStrings("~", StatusBadge.abandoned.glyph());
    try testing.expectEqualStrings("d", StatusBadge.draft.glyph());
}

test "view_model: queryAgentMonitor on empty DB returns empty slices" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const snap = try queryAgentMonitor(&d, a);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 0), snap.active.len);
    try testing.expectEqual(@as(usize, 0), snap.stale.len);
}

test "view_model: queryAgentMonitor surfaces active claims" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert a session, a task, and acquire a claim.
    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','vm-test-task','todo')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\  status, vendor, lease_expires_at
        \\) values (
        \\  'test-tok', ?, 'task', ?, 'exclusive', 'active', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '+600 seconds')
        \\)
    ,
        &.{
            .{ .int = sid },
            .{ .int = tid },
        },
    );

    const snap = try queryAgentMonitor(&d, a);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 1), snap.active.len);
    try testing.expectEqual(@as(usize, 0), snap.stale.len);
    try testing.expectEqual(StatusBadge.active, snap.active[0].badge);
    try testing.expectEqualStrings("claude", snap.active[0].vendor);
    // Verify entity_ref has "task:<id>" format.
    try testing.expect(std.mem.startsWith(u8, snap.active[0].entity_ref, "task:"));
}

test "view_model: queryAgentMonitor surfaces stale claims" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','stale-task','todo')",
        &.{},
    );
    // Insert an already-expired (stale) active claim.
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\  status, vendor, lease_expires_at
        \\) values (
        \\  'stale-tok', ?, 'task', ?, 'exclusive', 'stale', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '-60 seconds')
        \\)
    ,
        &.{
            .{ .int = sid },
            .{ .int = tid },
        },
    );

    const snap = try queryAgentMonitor(&d, a);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 0), snap.active.len);
    try testing.expectEqual(@as(usize, 1), snap.stale.len);
    try testing.expectEqual(StatusBadge.stale, snap.stale[0].badge);
}

// =========================================================================
// HeartbeatAge / parseIso8601Unix tests (tasks 4015, 4017)
// =========================================================================

test "view_model: classifyHeartbeatAge returns fresh when well within TTL" {
    // now=100, claimed=0, expires=200, last_hb=90 → age=10, TTL=200, TTL/2=100 → fresh
    const age = classifyHeartbeatAge(100, 90, 200, 0);
    try testing.expectEqual(HeartbeatAge.fresh, age);
}

test "view_model: classifyHeartbeatAge returns warning when past TTL/2" {
    // now=160, claimed=0, expires=200, last_hb=10 → age=150, TTL=200, TTL/2=100 → warning
    const age = classifyHeartbeatAge(160, 10, 200, 0);
    try testing.expectEqual(HeartbeatAge.warning, age);
}

test "view_model: classifyHeartbeatAge returns stale when lease expired" {
    // now=300, expires=200 → stale regardless of heartbeat
    const age = classifyHeartbeatAge(300, 190, 200, 0);
    try testing.expectEqual(HeartbeatAge.stale, age);
}

test "view_model: classifyHeartbeatAge stale with status=stale record" {
    // Simulates an already-stale claim: expires in past
    const age = classifyHeartbeatAge(1000, 500, 600, 0);
    try testing.expectEqual(HeartbeatAge.stale, age);
}

test "view_model: classifyHeartbeatAge zero TTL falls back to warning" {
    // claimed_at == lease_expires_at → TTL=0 → guard triggers → warning
    const age = classifyHeartbeatAge(100, 100, 200, 200);
    try testing.expectEqual(HeartbeatAge.warning, age);
}

test "view_model: parseIso8601Unix round-trips a known epoch" {
    // 1970-01-01T00:00:00.000Z = Unix epoch 0
    const result = parseIso8601Unix("1970-01-01T00:00:00.000Z");
    try testing.expect(result != null);
    try testing.expectEqual(@as(i64, 0), result.?);
}

test "view_model: parseIso8601Unix returns null for short string" {
    try testing.expectEqual(@as(?i64, null), parseIso8601Unix("short"));
}

test "view_model: parseIso8601Unix handles 2026-06-14T12:00:00Z" {
    // Sanity-check a date in the project's active timeframe.
    const result = parseIso8601Unix("2026-06-14T12:00:00.000Z");
    try testing.expect(result != null);
    // Should be substantially past 2020 (> 1577836800).
    try testing.expect(result.? > 1_577_836_800);
}

// =========================================================================
// AgentActionStream tests (task 4016)
// =========================================================================

test "view_model: queryAgentActionStream on empty DB returns empty slice" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const rows = try queryAgentActionStream(&d, a, 50);
    defer AgentActionRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryAgentActionStream returns action rows" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    // Insert a task and a claim (needed for action FK).
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','stream-task','doing')",
        &.{},
    );
    _ = tid;
    // Insert a completed action directly.
    _ = try d.execParams(
        \\insert into agent_actions (
        \\  session_id, action_kind, vendor, started_at, ended_at, outcome, summary
        \\) values (
        \\  ?, 'coder', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '-10 seconds'),
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now'),
        \\  'ok', 'wrote the code'
        \\)
    , &.{.{ .int = sid }});

    const rows = try queryAgentActionStream(&d, a, 50);
    defer AgentActionRow.deinitMany(rows, a);

    try testing.expect(rows.len >= 1);
    // The first row should be the action we inserted.
    try testing.expectEqualStrings("coder", rows[0].kind_label);
    try testing.expect(rows[0].summary != null);
    try testing.expectEqualStrings("wrote the code", rows[0].summary.?);
}

test "view_model: queryAgentActionStream includes claim transitions" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','trans-task','done')",
        &.{},
    );
    // Insert a completed claim (terminal status).
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\  status, vendor, lease_expires_at,
        \\  released_at, release_reason
        \\) values (
        \\  'comp-tok', ?, 'task', ?, 'exclusive', 'completed', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '+600 seconds'),
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now'),
        \\  'work done'
        \\)
    , &.{
        .{ .int = sid },
        .{ .int = tid },
    });

    const rows = try queryAgentActionStream(&d, a, 50);
    defer AgentActionRow.deinitMany(rows, a);

    // Should contain the claim transition row.
    var found_claim = false;
    for (rows) |r| {
        if (std.mem.startsWith(u8, r.kind_label, "claim:")) {
            found_claim = true;
            try testing.expectEqualStrings("claim:completed", r.kind_label);
        }
    }
    try testing.expect(found_claim);
}

test "view_model: queryAgentActionStream limit is respected" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor) values ('test')",
        &.{},
    );
    // Insert 5 action rows.
    var i: usize = 0;
    while (i < 5) : (i += 1) {
        _ = try d.execParams(
            \\insert into agent_actions (session_id, action_kind, vendor, started_at)
            \\values (?, 'other', 'test', strftime('%Y-%m-%dT%H:%M:%fZ','now'))
        , &.{.{ .int = sid }});
    }

    const rows = try queryAgentActionStream(&d, a, 3);
    defer AgentActionRow.deinitMany(rows, a);
    try testing.expect(rows.len <= 3);
}

test "view_model: queryPlanNodes on empty DB returns empty slice" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const nodes = try queryPlanNodes(&d, a);
    defer {
        for (nodes) |n| n.deinit(a);
        a.free(nodes);
    }
    try testing.expectEqual(@as(usize, 0), nodes.len);
}

test "view_model: queryPlanNodes populates badge and counts" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert an active plan.
    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Test Plan','test-plan','active')",
        &.{},
    );
    // Insert two tasks, one done.
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 'task-a','done')",
        &.{.{ .int = plan_id }},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 'task-b','todo')",
        &.{.{ .int = plan_id }},
    );

    const nodes = try queryPlanNodes(&d, a);
    defer {
        for (nodes) |n| n.deinit(a);
        a.free(nodes);
    }

    try testing.expectEqual(@as(usize, 1), nodes.len);
    try testing.expectEqual(StatusBadge.active, nodes[0].status_badge);
    try testing.expectEqual(@as(u32, 2), nodes[0].task_count);
    try testing.expectEqual(@as(u32, 1), nodes[0].done_count);
    try testing.expectEqualStrings("Test Plan", nodes[0].title);
    try testing.expectEqualStrings("test-plan", nodes[0].slug);
}

test "view_model: queryTaskRows on missing plan returns empty" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const rows = try queryTaskRows(&d, a, 9999);
    defer TaskRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryTaskRows returns tasks with correct status badges" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','P1','p1','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 'T1','doing',10)",
        &.{.{ .int = plan_id }},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 'T2','blocked',20)",
        &.{.{ .int = plan_id }},
    );

    const rows = try queryTaskRows(&d, a, plan_id);
    defer TaskRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 2), rows.len);
    try testing.expectEqual(StatusBadge.doing, rows[0].status_badge);
    try testing.expectEqual(StatusBadge.blocked, rows[1].status_badge);
    try testing.expectEqual(@as(?[]const u8, null), rows[0].claim_token);
}

test "view_model: queryTaskRows surfaces active claim token" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','ClaimPlan','cp','active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 'claimtask','doing')",
        &.{.{ .int = plan_id }},
    );
    const sid = try d.execParams(
        "insert into sessions (vendor) values ('claude')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\  status, vendor, lease_expires_at
        \\) values (
        \\  'active-claim-tok', ?, 'task', ?, 'exclusive', 'active', 'claude',
        \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '+600 seconds')
        \\)
    ,
        &.{
            .{ .int = sid },
            .{ .int = task_id },
        },
    );

    const rows = try queryTaskRows(&d, a, plan_id);
    defer TaskRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expect(rows[0].claim_token != null);
    try testing.expectEqualStrings("active-claim-tok", rows[0].claim_token.?);
}

test "view_model: queryPlanDetail returns empty pane for missing plan" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const detail = try queryPlanDetail(&d, a, 9999);
    defer detail.deinit(a);
    try testing.expectEqual(DetailKind.empty, detail.kind);
}

test "view_model: queryPlanDetail returns plan title and status" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status, summary) values ('global','Detail Plan','dp','active','A plan summary')",
        &.{},
    );

    const detail = try queryPlanDetail(&d, a, pid);
    defer detail.deinit(a);

    try testing.expectEqual(DetailKind.plan, detail.kind);
    try testing.expectEqualStrings("Detail Plan", detail.title);
    // Body should contain the status and summary.
    try testing.expect(std.mem.indexOf(u8, detail.body, "active") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "A plan summary") != null);
}

test "view_model: queryTaskDetail returns empty pane for missing task" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const detail = try queryTaskDetail(&d, a, 9999);
    defer detail.deinit(a);
    try testing.expectEqual(DetailKind.empty, detail.kind);
}

test "view_model: queryTaskDetail returns task fields" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, body, status, priority, next_action) values ('global','My Task','Task body text','doing',5,'Write the code')",
        &.{},
    );

    const detail = try queryTaskDetail(&d, a, task_id);
    defer detail.deinit(a);

    try testing.expectEqual(DetailKind.task, detail.kind);
    try testing.expectEqualStrings("My Task", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "doing") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "Write the code") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "Task body text") != null);
}

test "view_model: DetailPane.empty is valid" {
    const a = testing.allocator;
    const pane = try DetailPane.empty(a);
    defer pane.deinit(a);
    try testing.expectEqual(DetailKind.empty, pane.kind);
    try testing.expectEqualStrings("", pane.title);
    try testing.expectEqualStrings("", pane.body);
}

// =========================================================================
// Scope Explorer tests (tasks 3966–3970)
// =========================================================================

test "view_model: queryPlanNodesFiltered all-scopes on empty DB returns empty" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const nodes = try queryPlanNodesFiltered(&d, a, .all);
    defer {
        for (nodes) |n| n.deinit(a);
        a.free(nodes);
    }
    try testing.expectEqual(@as(usize, 0), nodes.len);
}

test "view_model: queryPlanNodesFiltered all-scopes returns plans" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Alpha','alpha','active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Beta','beta','draft')",
        &.{},
    );

    const nodes = try queryPlanNodesFiltered(&d, a, .all);
    defer {
        for (nodes) |n| n.deinit(a);
        a.free(nodes);
    }
    try testing.expectEqual(@as(usize, 2), nodes.len);
}

test "view_model: queryPlanNodesFiltered repo filter excludes mismatched scope" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Insert a project and a repo-scoped plan.
    const proj_id = try d.execParams(
        "insert into projects (slug, name, root_path) values ('proj1','Proj1','/work/proj1')",
        &.{},
    );
    _ = try d.execParams(
        "insert into plans (scope_kind, scope_id, title, slug, status) values ('repo', ?, 'Repo Plan','rp','active')",
        &.{.{ .int = proj_id }},
    );
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Global Plan','gp','active')",
        &.{},
    );

    // Filter to proj_id: should include both the repo-scoped plan and
    // global plans (global is always included per the SQL filter).
    const nodes = try queryPlanNodesFiltered(&d, a, .{ .repo = proj_id });
    defer {
        for (nodes) |n| n.deinit(a);
        a.free(nodes);
    }
    // Repo plan + global plan = 2.
    try testing.expectEqual(@as(usize, 2), nodes.len);
}

test "view_model: queryPlanDrillRows returns tasks and linked decisions" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','DrillPlan','dp','active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 'T1','todo')",
        &.{.{ .int = plan_id }},
    );
    _ = task_id;
    // Insert a decision and link it to the plan.
    // decisions.body is NOT NULL — provide a non-empty body.
    const dec_id = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','D1','Decision body','accepted')",
        &.{},
    );
    // entity_links uses from_kind/from_id/to_kind/to_id/relationship.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('plan', ?, 'decision', ?, 'derives-from')",
        &.{ .{ .int = plan_id }, .{ .int = dec_id } },
    );

    const rows = try queryPlanDrillRows(&d, a, plan_id);
    defer DrillRow.deinitMany(rows, a);

    // At minimum: 1 task + 1 decision.
    try testing.expect(rows.len >= 2);
    // First rows should be tasks.
    try testing.expectEqual(DrillKind.task, rows[0].kind);
    // Find the decision in the result.
    var found_dec = false;
    for (rows) |r| {
        if (r.kind == .decision) {
            found_dec = true;
            try testing.expectEqual(StatusBadge.done, r.status_badge);
        }
    }
    try testing.expect(found_dec);
}

test "view_model: queryPlanDrillRows empty plan returns empty" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','Empty','ep','draft')",
        &.{},
    );
    const rows = try queryPlanDrillRows(&d, a, plan_id);
    defer DrillRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryDecisionDetail returns body and status" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // decisions.body is NOT NULL and decisions.status values: proposed/accepted/superseded/withdrawn.
    const did = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global','Dec Title','Dec body text','accepted')",
        &.{},
    );
    const detail = try queryDecisionDetail(&d, a, did);
    defer detail.deinit(a);

    try testing.expectEqualStrings("Dec Title", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "accepted") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "Dec body text") != null);
}

test "view_model: queryQuestionDetail returns empty for missing" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const detail = try queryQuestionDetail(&d, a, 9999);
    defer detail.deinit(a);
    try testing.expectEqual(DetailKind.empty, detail.kind);
}

test "view_model: queryScenarioDetail returns body and status" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // test_scenarios status: draft, ready, verified, failing, retired.
    const sid = try d.execParams(
        "insert into test_scenarios (scope_kind, title, body, status) values ('global','Scenario Title','Scenario body','verified')",
        &.{},
    );
    const detail = try queryScenarioDetail(&d, a, sid);
    defer detail.deinit(a);

    try testing.expectEqualStrings("Scenario Title", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "verified") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "Scenario body") != null);
}

test "view_model: queryArtifactDetail returns kind and body" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // artifacts.kind is the column name (not artifact_kind).
    const aid = try d.execParams(
        "insert into artifacts (scope_kind, kind, title, body) values ('global','tech_spec','Artifact Title','Artifact body text')",
        &.{},
    );
    const detail = try queryArtifactDetail(&d, a, aid);
    defer detail.deinit(a);

    try testing.expectEqualStrings("Artifact Title", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "tech_spec") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "Artifact body text") != null);
}

test "view_model: cwdScopeProjectId returns null on empty DB" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const result = cwdScopeProjectId(&d, a, "/some/path");
    try testing.expectEqual(@as(?i64, null), result);
}

test "view_model: cwdScopeProjectId returns project id when path matches" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // projects requires name (NOT NULL).
    const proj_id = try d.execParams(
        "insert into projects (slug, name, root_path) values ('myrepo','MyRepo','/work/myrepo')",
        &.{},
    );
    // Add an association so the count >= 1 path is satisfied.
    const assoc_id = try d.execParams(
        "insert into associations (slug, name, kind) values ('myorg','MyOrg','org')",
        &.{},
    );
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = proj_id }, .{ .int = assoc_id } },
    );

    const result = cwdScopeProjectId(&d, a, "/work/myrepo/src/foo");
    try testing.expectEqual(proj_id, result.?);
}

test "view_model: decisionStatusBadge maps known statuses" {
    // decisions status: proposed, accepted, superseded, withdrawn.
    try testing.expectEqual(StatusBadge.done, decisionStatusBadge("accepted"));
    try testing.expectEqual(StatusBadge.cancelled, decisionStatusBadge("withdrawn"));
    // superseded now maps to .superseded (glyph '^'), not .abandoned (glyph '~').
    try testing.expectEqual(StatusBadge.superseded, decisionStatusBadge("superseded"));
    try testing.expectEqual(StatusBadge.draft, decisionStatusBadge("proposed"));
}

test "view_model: StatusBadge.superseded has distinct glyph from draft and abandoned" {
    // task 4100: 'superseded' must be visually distinct from 'proposed'/'draft'
    // and from 'abandoned'. All three must have different glyphs.
    const sup = StatusBadge.superseded.glyph();
    const dra = StatusBadge.draft.glyph();
    const aba = StatusBadge.abandoned.glyph();
    try testing.expect(!std.mem.eql(u8, sup, dra));
    try testing.expect(!std.mem.eql(u8, sup, aba));
    // Confirm the exact glyph so a future change is caught explicitly.
    try testing.expectEqualStrings("^", sup);
}

test "view_model: questionStatusBadge maps known statuses" {
    // questions status: open, answered, wontfix.
    try testing.expectEqual(StatusBadge.todo, questionStatusBadge("open"));
    try testing.expectEqual(StatusBadge.done, questionStatusBadge("answered"));
    try testing.expectEqual(StatusBadge.cancelled, questionStatusBadge("wontfix"));
}

test "view_model: scenarioStatusBadge maps known statuses" {
    // test_scenarios status: draft, ready, verified, failing, retired.
    try testing.expectEqual(StatusBadge.draft, scenarioStatusBadge("draft"));
    try testing.expectEqual(StatusBadge.done, scenarioStatusBadge("verified"));
    try testing.expectEqual(StatusBadge.blocked, scenarioStatusBadge("failing"));
    try testing.expectEqual(StatusBadge.abandoned, scenarioStatusBadge("retired"));
}

// =========================================================================
// Task Board view-model tests (tasks 4018, 4019, 4020)
// =========================================================================

test "view_model: queryTaskBoard on empty DB yields empty snapshot" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const snap = try queryTaskBoard(&d, a, .all);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 0), snap.open.len);
    try testing.expectEqual(@as(usize, 0), snap.doing.len);
    try testing.expectEqual(@as(usize, 0), snap.blocked.len);
    try testing.expectEqual(@as(usize, 0), snap.done.len);
    try testing.expectEqual(@as(usize, 0), snap.totalCount());
}

test "view_model: queryTaskBoard groups tasks into four status columns (task 4018)" {
    // Verifies: tasks across all four status groups land in the right column.
    // Status enum confirmed from migration 00003_work_items.up.sql:
    //   check(status in ('todo','doing','blocked','done','cancelled'))
    // 'todo' → open, 'doing' → doing, 'blocked' → blocked, 'done' → done.
    // 'cancelled' is excluded.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','A','todo',1)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','B','doing',2)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','C','blocked',3)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','D','done',4)",
        &.{},
    );
    // Cancelled should be excluded.
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','E','cancelled',5)",
        &.{},
    );

    const snap = try queryTaskBoard(&d, a, .all);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 1), snap.open.len);
    try testing.expectEqual(@as(usize, 1), snap.doing.len);
    try testing.expectEqual(@as(usize, 1), snap.blocked.len);
    try testing.expectEqual(@as(usize, 1), snap.done.len);
    try testing.expectEqual(@as(usize, 4), snap.totalCount());

    // Verify badge mapping.
    try testing.expectEqual(StatusBadge.todo, snap.open[0].status_badge);
    try testing.expectEqual(StatusBadge.doing, snap.doing[0].status_badge);
    try testing.expectEqual(StatusBadge.blocked, snap.blocked[0].status_badge);
    try testing.expectEqual(StatusBadge.done, snap.done[0].status_badge);

    // Verify titles landed in the right column.
    try testing.expectEqualStrings("A", snap.open[0].title);
    try testing.expectEqualStrings("B", snap.doing[0].title);
    try testing.expectEqualStrings("C", snap.blocked[0].title);
    try testing.expectEqualStrings("D", snap.done[0].title);
}

test "view_model: queryTaskBoard multiple tasks per column ordered by priority (task 4018)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','High','todo',1)",
        &.{},
    );
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global','Low','todo',99)",
        &.{},
    );

    const snap = try queryTaskBoard(&d, a, .all);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 2), snap.open.len);
    // Priority 1 comes before priority 99.
    try testing.expectEqualStrings("High", snap.open[0].title);
    try testing.expectEqualStrings("Low", snap.open[1].title);
}

test "view_model: queryTaskBoard repo filter excludes cancelled global task (sql_repo precedence fix)" {
    // Regression test for the operator-precedence bug in sql_repo:
    //   BUGGY:  where status in (...) and (scope_kind='repo' and scope_id=?) or scope_kind='global'
    //           parsed as: (status filter AND repo clause) OR global clause
    //           → returns ALL global tasks regardless of status, including cancelled.
    //   FIXED:  where status in (...) and (scope_kind='repo' and scope_id=? or scope_kind='global')
    //           → status filter applies to both repo and global tasks.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('filter-repo','FilterRepo')",
        &.{},
    );
    // A cancelled global task — must be excluded under repo filter.
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','CancelledGlobal','cancelled')",
        &.{},
    );
    // An in-scope todo task — must appear.
    _ = try d.execParams(
        "insert into tasks (scope_kind, scope_id, title, status, priority) values ('repo', ?, 'RepoTodo','todo',1)",
        &.{.{ .int = proj_id }},
    );

    const snap = try queryTaskBoard(&d, a, .{ .repo = proj_id });
    defer snap.deinit(a);

    // Only the in-scope todo task should appear; cancelled global must be excluded.
    try testing.expectEqual(@as(usize, 1), snap.open.len);
    try testing.expectEqual(@as(usize, 0), snap.doing.len);
    try testing.expectEqual(@as(usize, 0), snap.blocked.len);
    try testing.expectEqual(@as(usize, 0), snap.done.len);
    try testing.expectEqualStrings("RepoTodo", snap.open[0].title);
}

test "view_model: queryTaskBoard only-cancelled tasks yields empty snapshot (task 4018 empty state)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','CancelledTask','cancelled')",
        &.{},
    );

    const snap = try queryTaskBoard(&d, a, .all);
    defer snap.deinit(a);
    try testing.expectEqual(@as(usize, 0), snap.totalCount());
}

test "view_model: queryTaskReopens on task with no reopens returns empty (task 4019)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','NoReopenTask','done')",
        &.{},
    );

    const rows = try queryTaskReopens(&d, a, tid);
    defer TaskReopenRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryTaskReopens surfaces reopen history (task 4019)" {
    // Seeds a task with two reopens and asserts both appear in order.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','ReopenTask','doing')",
        &.{},
    );
    // First reopen: done → todo.
    _ = try d.execParams(
        \\insert into task_reopens (task_id, from_status, to_status, source, reason, created_at)
        \\values (?, 'done', 'todo', 'task-reopen', 'first reopen',
        \\        strftime('%Y-%m-%dT%H:%M:%fZ','now','-60 seconds'))
    , &.{.{ .int = tid }});
    // Second reopen: cancelled → doing.
    _ = try d.execParams(
        \\insert into task_reopens (task_id, from_status, to_status, source, reason, created_at)
        \\values (?, 'cancelled', 'doing', 'task-update-force', 'second reopen',
        \\        strftime('%Y-%m-%dT%H:%M:%fZ','now'))
    , &.{.{ .int = tid }});

    const rows = try queryTaskReopens(&d, a, tid);
    defer TaskReopenRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 2), rows.len);
    // Oldest-first: first reopen is done→todo.
    try testing.expectEqualStrings("done", rows[0].from_status);
    try testing.expectEqualStrings("todo", rows[0].to_status);
    try testing.expectEqualStrings("task-reopen", rows[0].source);
    try testing.expect(rows[0].reason != null);
    try testing.expectEqualStrings("first reopen", rows[0].reason.?);
    // Second reopen is cancelled→doing.
    try testing.expectEqualStrings("cancelled", rows[1].from_status);
    try testing.expectEqualStrings("doing", rows[1].to_status);
}

test "view_model: queryTaskTouchPaths on task with no paths returns empty (task 4019)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','NoPaths','todo')",
        &.{},
    );

    const rows = try queryTaskTouchPaths(&d, a, tid);
    defer TaskTouchPathRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryTaskTouchPaths surfaces touch paths alphabetically (task 4019)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','TouchTask','doing')",
        &.{},
    );
    // Insert a project for the repo_id FK.
    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('touch-repo','TouchRepo')",
        &.{},
    );
    // Insert two paths; verify alphabetical order.
    _ = try d.execParams(
        "insert into task_touch_paths (task_id, repo_id, path) values (?, ?, 'src/z.zig')",
        &.{ .{ .int = tid }, .{ .int = proj_id } },
    );
    _ = try d.execParams(
        "insert into task_touch_paths (task_id, repo_id, path) values (?, ?, 'src/a.zig')",
        &.{ .{ .int = tid }, .{ .int = proj_id } },
    );

    const rows = try queryTaskTouchPaths(&d, a, tid);
    defer TaskTouchPathRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 2), rows.len);
    // Alphabetical: src/a.zig before src/z.zig.
    try testing.expectEqualStrings("src/a.zig", rows[0].path);
    try testing.expectEqualStrings("src/z.zig", rows[1].path);
}

test "view_model: queryTaskBlockingLinks on task with no links returns empty (task 4020)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','NoLinks','todo')",
        &.{},
    );

    const rows = try queryTaskBlockingLinks(&d, a, tid);
    defer TaskLinkRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryTaskBlockingLinks surfaces blocks-this direction (task 4020)" {
    // Task A blocks task B. Query from B's perspective → direction=.blocks_this.
    // entity_links relationship 'depends-on' confirmed from migration 00004:
    //   check(relationship in ('derives-from','depends-on','addresses','verifies','cites','supersedes','touches'))
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_a = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','TaskA','done')",
        &.{},
    );
    const task_b = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','TaskB','blocked')",
        &.{},
    );
    // A blocks B: from_id=A, to_id=B.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = task_a }, .{ .int = task_b } },
    );

    // Query from B's perspective.
    const rows = try queryTaskBlockingLinks(&d, a, task_b);
    defer TaskLinkRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqual(LinkDirection.blocks_this, rows[0].direction);
    // Label format: "task:<id> — <title>".
    try testing.expect(std.mem.indexOf(u8, rows[0].label, "TaskA") != null);
}

test "view_model: queryTaskBlockingLinks surfaces blocked-by-this direction (task 4020)" {
    // Task A blocks task B. Query from A's perspective → direction=.this_blocks.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_a = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Blocker','doing')",
        &.{},
    );
    const task_b = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Blockee','blocked')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = task_a }, .{ .int = task_b } },
    );

    // Query from A's perspective.
    const rows = try queryTaskBlockingLinks(&d, a, task_a);
    defer TaskLinkRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqual(LinkDirection.this_blocks, rows[0].direction);
    try testing.expect(std.mem.indexOf(u8, rows[0].label, "Blockee") != null);
}

test "view_model: queryTaskBlockingLinks both directions simultaneously (task 4020)" {
    // Task X blocks task Y; task Z blocks task X.
    // From X's perspective: 1 blocks_this (Z blocks X) + 1 this_blocks (X blocks Y).
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const task_x = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','X','blocked')",
        &.{},
    );
    const task_y = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Y','blocked')",
        &.{},
    );
    const task_z = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Z','done')",
        &.{},
    );
    // Z blocks X.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = task_z }, .{ .int = task_x } },
    );
    // X blocks Y.
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = task_x }, .{ .int = task_y } },
    );

    const rows = try queryTaskBlockingLinks(&d, a, task_x);
    defer TaskLinkRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 2), rows.len);
    // First: blocks_this (Z blocks X).
    try testing.expectEqual(LinkDirection.blocks_this, rows[0].direction);
    // Second: this_blocks (X blocks Y).
    try testing.expectEqual(LinkDirection.this_blocks, rows[1].direction);
}

test "view_model: queryTaskBoardDetail returns null for missing task" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const result = try queryTaskBoardDetail(&d, a, 9999);
    try testing.expectEqual(@as(?TaskBoardDetail, null), result);
}

test "view_model: queryTaskBoardDetail full detail: body + reopens + touch_paths + links (tasks 4019, 4020)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Seed the task under test.
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, body, status, priority, next_action) values ('global','DetailTask','task body','doing',7,'next step')",
        &.{},
    );

    // Seed a reopen.
    _ = try d.execParams(
        \\insert into task_reopens (task_id, from_status, to_status, source)
        \\values (?, 'done', 'doing', 'task-reopen')
    , &.{.{ .int = tid }});

    // Seed a touch path.
    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('det-repo','DetRepo')",
        &.{},
    );
    _ = try d.execParams(
        "insert into task_touch_paths (task_id, repo_id, path) values (?, ?, 'src/detail.zig')",
        &.{ .{ .int = tid }, .{ .int = proj_id } },
    );

    // Seed a blocking link: another task blocks this one.
    const blocker_id = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Blocker','done')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'depends-on')",
        &.{ .{ .int = blocker_id }, .{ .int = tid } },
    );

    const detail_opt = try queryTaskBoardDetail(&d, a, tid);
    try testing.expect(detail_opt != null);
    const detail = detail_opt.?;
    defer detail.deinit(a);

    // Body must mention status and priority.
    try testing.expectEqualStrings("DetailTask", detail.title);
    try testing.expect(std.mem.indexOf(u8, detail.body, "doing") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "7") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "next step") != null);
    try testing.expect(std.mem.indexOf(u8, detail.body, "task body") != null);

    // Reopens.
    try testing.expectEqual(@as(usize, 1), detail.reopens.len);
    try testing.expectEqualStrings("done", detail.reopens[0].from_status);
    try testing.expectEqualStrings("doing", detail.reopens[0].to_status);

    // Touch paths.
    try testing.expectEqual(@as(usize, 1), detail.touch_paths.len);
    try testing.expectEqualStrings("src/detail.zig", detail.touch_paths[0].path);

    // Blocking links.
    try testing.expectEqual(@as(usize, 1), detail.links.len);
    try testing.expectEqual(LinkDirection.blocks_this, detail.links[0].direction);
    try testing.expect(std.mem.indexOf(u8, detail.links[0].label, "Blocker") != null);
}

// External operational-plane view-model facade.
const external_ops = @import("view_model/external_ops.zig");
pub const ExtSystemRow = external_ops.ExtSystemRow;
pub const ExtLinkRow = external_ops.ExtLinkRow;
pub const ExtSystemSyncStatus = external_ops.ExtSystemSyncStatus;
pub const UnresolvedConflictRow = external_ops.UnresolvedConflictRow;
pub const ExtOpsSnapshot = external_ops.ExtOpsSnapshot;
pub const queryExtSystems = external_ops.queryExtSystems;
pub const queryExtLinks = external_ops.queryExtLinks;
pub const queryExtSystemSyncStatus = external_ops.queryExtSystemSyncStatus;
pub const queryUnresolvedConflicts = external_ops.queryUnresolvedConflicts;
pub const queryExtOpsSnapshot = external_ops.queryExtOpsSnapshot;
// Sessions and handoff view-model facade.
const sessions = @import("view_model/sessions.zig");
pub const SessionRow = sessions.SessionRow;
pub const SessionEntryRow = sessions.SessionEntryRow;
pub const SessionCommitRow = sessions.SessionCommitRow;
pub const HandoffRow = sessions.HandoffRow;
pub const SessionsHandoffSnapshot = sessions.SessionsHandoffSnapshot;
pub const querySessions = sessions.querySessions;
pub const querySessionEntries = sessions.querySessionEntries;
pub const querySessionCommits = sessions.querySessionCommits;
pub const queryHandoffs = sessions.queryHandoffs;
pub const querySessionsHandoffSnapshot = sessions.querySessionsHandoffSnapshot;
// Audit-log view-model facade.
const audit = @import("view_model/audit.zig");
pub const AuditLogRow = audit.AuditLogRow;
pub const AuditEntityFilter = audit.AuditEntityFilter;
pub const AuditEntityRef = audit.AuditEntityRef;
pub const queryAuditLog = audit.queryAuditLog;
pub const queryAuditEntities = audit.queryAuditEntities;
// CLI-history view-model facade.
const cli_history = @import("view_model/cli_history.zig");
pub const CliInvocationRow = cli_history.CliInvocationRow;
pub const CliTimeFilter = cli_history.CliTimeFilter;
pub const CliHistoryFilter = cli_history.CliHistoryFilter;
pub const queryCliHistory = cli_history.queryCliHistory;
pub const queryCliVerbs = cli_history.queryCliVerbs;
pub const queryCliScopes = cli_history.queryCliScopes;
// Association-topology view-model facade.
const topology = @import("view_model/topology.zig");
pub const TopologyMemberRow = topology.TopologyMemberRow;
pub const TopologyAssocRow = topology.TopologyAssocRow;
pub const queryTopology = topology.queryTopology;
// Utility view-model facade.
const utility = @import("view_model/utility.zig");
pub const ConfigRow = utility.ConfigRow;
pub const AnnotationRow = utility.AnnotationRow;
pub const WorkbenchSyncRow = utility.WorkbenchSyncRow;
pub const queryConfig = utility.queryConfig;
pub const queryAnnotations = utility.queryAnnotations;
pub const queryWorkbenchSync = utility.queryWorkbenchSync;

test "view_model compiles" {
    std.testing.refAllDecls(@This());
}
