//! handlers/planfilter — shared `--plan <id>` filter widening used by
//! `feed`, `ps`, `claims`, and `actions`.
//!
//! `--plan <id>` matches any of:
//!   * (entity_kind='plan'      AND entity_id = <id>)
//!   * (entity_kind='task'      AND tasks.plan_id = <id>)
//!   * (entity_kind='plan_step' AND plan_steps.plan_id = <id>)
//!
//! The pre-fix behavior matched only the first row — silently dropping
//! every claim/action whose entity was a task on the plan. Plan 85's
//! multi-agent scenario test documented that quirk in a comment; with
//! this fix the scenario can drop the workaround.
//!
//! The lookups go through SQLite directly (read-only connection). One
//! prepared statement per call, finalized on return — fine for the
//! filter pass which iterates at most a couple of thousand rows under
//! the snapshot/incremental limits in feed.zig and siblings.

const std = @import("std");
const db = @import("db");
const engine = @import("engine");

const types = engine.runtime.agentactivity.types;

/// Returns true if a claim row with `(kind, id)` belongs to the plan
/// identified by `plan_id` under the widened match.
pub fn claimBelongsToPlan(d: *db.sqlite.Db, kind: types.EntityKind, entity_id: i64, plan_id: i64) bool {
    return switch (kind) {
        .plan => entity_id == plan_id,
        .task => taskPlan(d, entity_id) == plan_id,
        .plan_step => planStepPlan(d, entity_id) == plan_id,
    };
}

/// Returns true if an action row with `(kind, id)` belongs to the plan
/// identified by `plan_id` under the widened match. The action's
/// entity_kind covers a wider set than the claim's (question / artifact /
/// decision / scenario), but the same plan-rollup logic applies — for
/// those wider entity kinds we currently DON'T traverse them to a
/// plan_id (there isn't a direct link in the schema), so they don't
/// match `--plan`. Plan-id matching for actions is therefore the
/// triple {plan, task, plan_step}.
pub fn actionBelongsToPlan(d: *db.sqlite.Db, kind: types.ActionEntityKind, entity_id: i64, plan_id: i64) bool {
    return switch (kind) {
        .plan => entity_id == plan_id,
        .task => taskPlan(d, entity_id) == plan_id,
        .plan_step => planStepPlan(d, entity_id) == plan_id,
        .question, .test_scenario, .artifact, .decision => false,
    };
}

/// Returns the plan_id of a task, or -1 if the task has no plan binding
/// (e.g. scope=association/global tasks) or the lookup failed. The
/// sentinel keeps the call site branch-free — a real plan_id is always
/// >= 1.
fn taskPlan(d: *db.sqlite.Db, task_id: i64) i64 {
    var stmt = d.prepare("select coalesce(plan_id, -1) from tasks where id = ?") catch return -1;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return -1;
    const step = stmt.step() catch return -1;
    if (step != .row) return -1;
    return stmt.columnInt(0);
}

/// Returns the plan_id of a plan_step, or -1 if not found.
fn planStepPlan(d: *db.sqlite.Db, step_id: i64) i64 {
    var stmt = d.prepare("select plan_id from plan_steps where id = ?") catch return -1;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = step_id }}) catch return -1;
    const step = stmt.step() catch return -1;
    if (step != .row) return -1;
    return stmt.columnInt(0);
}
