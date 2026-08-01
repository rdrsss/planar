//! engine/ingestor/apply — commit a Diff against the database.
//!
//! Mirrors Go's `internal/ingestor/apply.go`. Two-phase semantics: parse
//! → diff → apply. Preview (`opts.apply = false`) writes nothing; apply
//! commits additions and updates, optionally also removals.
//!
//! The anchor plan is flipped from `draft` → `active` on the first
//! successful apply (subsequent applies are no-ops on the status flip).
//!
//! Removals (orphan tasks / orphan plans) are committed only when both
//! `opts.apply` and `opts.apply_removals` are true. `apply_removals` alone
//! is a user error and is rejected by the handler before reaching this
//! module.
//!
//! Apply mode is atomic per anchor plan. All derived graph writes for one
//! anchor run inside a SQLite savepoint; a failed write rolls back the whole
//! apply while preserving the original failure when cleanup succeeds.

const std = @import("std");
const db = @import("db");
const diff_mod = @import("diff.zig");
const parse = @import("parse.zig");
const scenarios_mod = @import("scenarios.zig");
const materialize = @import("materialize.zig");
const entitylink = @import("../entitylink.zig");
const plan_mod = @import("../planning/plan.zig");
const task_mod = @import("../planning/task.zig");
const decision_mod = @import("../planning/decision.zig");
const question_mod = @import("../planning/question.zig");
const scenario_mod = @import("../planning/scenario.zig");
const session_mod = @import("../runtime/session.zig");

// =========================================================================
// Options + result
// =========================================================================

pub const Options = struct {
    apply: bool = false,
    apply_removals: bool = false,
    /// Scope slug for child plans, tasks, decisions, scenarios created
    /// during apply. `null` = global. The handler supplies the anchor plan's
    /// stored scope so every derived entity preserves anchor provenance;
    /// the operator's resolved write scope only authorizes the apply.
    scope: ?[]const u8 = null,
};

pub const Result = struct {
    plans_created: usize = 0,
    plans_updated: usize = 0,
    tasks_created: usize = 0,
    tasks_updated: usize = 0,
    tasks_cancelled: usize = 0,
    decisions_added: usize = 0,
    scenarios_added: usize = 0,
    questions_added: usize = 0,
    questions_answered: usize = 0,
    anchor_activated: bool = false,
};

// =========================================================================
// Error set
// =========================================================================

pub const Error =
    error{
        NotFound,
        QueryFailed,
        ResolveSlugFailed,
        ApplyRemovalsRequiresApply,
    } ||
    plan_mod.Error ||
    task_mod.Error ||
    decision_mod.Error ||
    question_mod.Error ||
    scenario_mod.Error ||
    entitylink.Error ||
    scenarios_mod.Error ||
    materialize.Error ||
    std.mem.Allocator.Error;

// =========================================================================
// Apply
// =========================================================================

/// apply commits the Diff to the database according to opts.
///
/// Preview mode (`opts.apply == false`) writes nothing and returns an
/// empty Result; the caller has already rendered the Diff for display.
///
/// On real apply, child plans + tasks land first (so verifies-link
/// resolution against task slugs works), then decisions, then scenarios,
/// then question status flips, and finally — when the anchor was in
/// `draft` — the anchor plan is flipped to `active`. That write sequence is
/// wrapped in one savepoint so a failed apply leaves no derived rows behind.
pub fn apply(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    diff: diff_mod.Diff,
    opts: Options,
) Error!Result {
    if (opts.apply_removals and !opts.apply) return Error.ApplyRemovalsRequiresApply;

    var res: Result = .{};
    if (!opts.apply) {
        // Preview: emit a read-only audit entry and return. Best-effort —
        // a missing active session must NOT fail the preview.
        appendReadEntry(d, allocator, diff.anchor_plan_id) catch {};
        return res;
    }

    const savepoint_name = "spec_ingest_apply";
    d.savepoint(allocator, savepoint_name) catch return Error.QueryFailed;

    res = applyWithinSavepoint(d, allocator, diff, opts) catch |apply_err| {
        d.rollbackToSavepoint(allocator, savepoint_name) catch return Error.QueryFailed;
        d.releaseSavepoint(allocator, savepoint_name) catch return Error.QueryFailed;
        return apply_err;
    };

    d.releaseSavepoint(allocator, savepoint_name) catch return Error.QueryFailed;
    return res;
}

fn applyWithinSavepoint(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    diff: diff_mod.Diff,
    opts: Options,
) Error!Result {
    var res: Result = .{};

    // Anchor scope. Threaded from the handler via opts.scope so child plans,
    // tasks, decisions, and scenarios inherit the anchor plan's stored
    // provenance (or global when null), independent of the operator scope
    // that authorized the write.
    const scope_slug: ?[]const u8 = opts.scope;

    // ---- removals -----------------------------------------------------
    //
    // Apply removals before additions so a spec refresh can replace an
    // orphaned plan/task with a new row that reuses the same derived slug.
    // Cancelled tasks and abandoned plans remain in the audit trail, but
    // their slugs must be moved out of the live namespace first because the
    // schema's slug indexes are global for non-null slugs.
    if (opts.apply_removals) {
        for (diff.orphan_tasks) |ot| {
            try retireTaskForSpecRemoval(d, allocator, ot.existing_id);
            res.tasks_cancelled += 1;
        }
        for (diff.orphan_plans) |op| {
            for (op.tasks) |ot| {
                try retireTaskForSpecRemoval(d, allocator, ot.existing_id);
                res.tasks_cancelled += 1;
            }
            try retirePlanForSpecRemoval(d, allocator, op.existing_id);
        }
    }

    // ---- child plans + tasks ------------------------------------------
    for (diff.child_plans) |cp| {
        var child_plan_id: i64 = 0;
        switch (cp.op) {
            .add => {
                const created = plan_mod.create(d, allocator, .{
                    .title = cp.title,
                    .parent_plan_id = diff.anchor_plan_id,
                    .scope = scope_slug,
                }) catch |e| return mapPlanErr(e);
                defer plan_mod.deinit(created, allocator);
                child_plan_id = created.id;
                ensureLink(d, allocator, .plan, created.id, .plan, diff.anchor_plan_id, .@"derives-from") catch |e| return e;
                res.plans_created += 1;
            },
            .update => {
                child_plan_id = cp.existing_id;
                res.plans_updated += 1;
            },
            .remove => continue, // orphan path below.
        }

        for (cp.tasks) |te| {
            switch (te.op) {
                .add => {
                    const t = task_mod.create(d, allocator, .{
                        .title = te.title,
                        .body = if (te.body.len > 0) te.body else null,
                        .plan_id = child_plan_id,
                        // Generated by the differ from the roadmap position.
                        // The old hardcoded "Implement per acceptance
                        // criteria." was rejected by routing's
                        // genericNextAction, so every freshly ingested task
                        // was born unroutable.
                        .next_action = if (te.next_action.len > 0)
                            te.next_action
                        else
                            "Implement per acceptance criteria.",
                        .slug = if (te.slug.len > 0) te.slug else null,
                        .scope = scope_slug,
                    }) catch |e| return mapTaskErr(e);
                    defer task_mod.deinit(t, allocator);

                    ensureLink(d, allocator, .task, t.id, .plan, child_plan_id, .@"derives-from") catch |e| return e;
                    try applyTouchesLinks(d, allocator, t.id, te.touches);

                    if (diff_mod.isNonTrivial(te.body)) {
                        scenarios_mod.draftScenario(d, allocator, t.id, te.title, scope_slug) catch |e| return e;
                        res.scenarios_added += 1;
                    }
                    res.tasks_created += 1;
                },
                .update => {
                    // An EMPTY body means "the operator enriched this; leave it
                    // alone". Writing unconditionally is what made enrichment
                    // impossible: the only way to refresh facts was to destroy
                    // the content that made the task dispatchable.
                    if (te.body.len > 0) {
                        _ = d.execParams(
                            \\update tasks set body = ?,
                            \\                  updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
                            \\where id = ?
                        , &.{
                            .{ .text = te.body },
                            .{ .int = te.existing_id },
                        }) catch return Error.QueryFailed;
                    }
                    // Same rule for the next action: only a still-generated one
                    // is replaced.
                    if (te.next_action.len > 0) {
                        _ = d.execParams(
                            \\update tasks set next_action = ?,
                            \\                  updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
                            \\where id = ?
                        , &.{
                            .{ .text = te.next_action },
                            .{ .int = te.existing_id },
                        }) catch return Error.QueryFailed;
                    }

                    try applyTouchesLinks(d, allocator, te.existing_id, te.touches);

                    // Plan 286 M1 slug back-fill: set tasks.slug only when
                    // currently NULL. Never overwrite an existing slug
                    // silently — a mismatch surfaces as a stderr warning.
                    if (te.slug.len > 0) {
                        const current = try readTaskSlug(d, allocator, te.existing_id);
                        defer if (current) |s| allocator.free(s);
                        const set_slug = (current == null) or current.?.len == 0;
                        if (set_slug) {
                            const updated = task_mod.update(d, allocator, te.existing_id, .{
                                .slug = te.slug,
                            }) catch |e| return mapTaskErr(e);
                            task_mod.deinit(updated, allocator);
                        } else if (!std.mem.eql(u8, current.?, te.slug)) {
                            // Mismatch: emit a warning but keep the DB value.
                            std.log.warn(
                                "task {d}: slug '{s}' on disk but '{s}' in DB; keeping DB value",
                                .{ te.existing_id, te.slug, current.? },
                            );
                        }
                    }

                    res.tasks_updated += 1;
                },
                .remove => continue,
            }
        }
    }

    // Every ingested task has one explicit citation to the exact roadmap
    // bullet that created it. Reconcile these links before deriving facts so
    // artifact facts follow task citations rather than anchor membership.
    const roadmap_citations = try reconcileRoadmapCitations(d, allocator, diff);
    defer allocator.free(roadmap_citations);

    // ---- decisions ----------------------------------------------------
    for (diff.decisions) |de| {
        switch (de.op) {
            .add => {
                const body = if (de.body.len > 0) de.body else "(no body)";
                const created = decision_mod.create(d, allocator, .{
                    .title = de.title,
                    .body = body,
                    .scope = scope_slug,
                }) catch |e| return mapDecisionErr(e);
                defer decision_mod.deinit(created, allocator);
                ensureLink(d, allocator, .decision, created.id, .plan, diff.anchor_plan_id, .@"derives-from") catch |e| return e;
                res.decisions_added += 1;
            },
            .update => {
                _ = d.execParams(
                    \\update decisions set body = ?,
                    \\                     updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
                    \\where id = ?
                , &.{
                    .{ .text = de.body },
                    .{ .int = de.existing_id },
                }) catch return Error.QueryFailed;
            },
            .remove => continue,
        }
    }

    // ---- test-spec scenarios -----------------------------------------
    for (diff.scenarios) |se| {
        // Resolve slug refs first; an unresolvable slug aborts the apply.
        const resolved = try resolveSlugRefs(d, allocator, se.verifies, se.title, diff.anchor_plan_id);
        defer freeRefs(allocator, resolved);

        switch (se.op) {
            .add => {
                const created = scenario_mod.create(d, allocator, .{
                    .title = se.title,
                    .body = if (se.body.len > 0) se.body else null,
                    .scope = scope_slug,
                }) catch |e| return mapScenarioErr(e);
                defer scenario_mod.deinit(created, allocator);
                ensureLink(d, allocator, .test_scenario, created.id, .plan, diff.anchor_plan_id, .@"derives-from") catch |e| return e;
                for (resolved) |r| {
                    ensureLink(d, allocator, .test_scenario, created.id, .task, r.id, .verifies) catch |e| return e;
                }
                res.scenarios_added += 1;
            },
            .update => {
                _ = d.execParams(
                    \\update test_scenarios set body = ?,
                    \\                          updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
                    \\where id = ?
                , &.{
                    .{ .text = se.body },
                    .{ .int = se.existing_id },
                }) catch return Error.QueryFailed;
                for (resolved) |r| {
                    ensureLink(d, allocator, .test_scenario, se.existing_id, .task, r.id, .verifies) catch |e| return e;
                }
            },
            .remove => continue,
        }
    }

    // ---- new questions ------------------------------------------------
    for (diff.new_questions) |q| {
        const created = question_mod.create(d, allocator, .{
            .title = q.title,
            .body = if (q.body.len > 0) q.body else null,
            .scope = scope_slug,
            .plan_id = diff.anchor_plan_id,
        }) catch |e| return mapQuestionErr(e);
        defer question_mod.deinit(created, allocator);

        if (q.resolution.len > 0) {
            const answered = question_mod.answer(d, allocator, created.id, q.resolution) catch |e| return mapQuestionErr(e);
            defer question_mod.deinit(answered, allocator);
            res.questions_answered += 1;
        }
        res.questions_added += 1;
    }

    // ---- question status flips ----------------------------------------
    for (diff.updated_question_status) |sc| {
        const answered = question_mod.answer(d, allocator, sc.question_id, sc.answer) catch |e| return mapQuestionErr(e);
        defer question_mod.deinit(answered, allocator);
        res.questions_answered += 1;
    }

    // Replace the complete provenance-bearing fact set inside the same
    // savepoint as entity reconciliation. A failure therefore leaves the
    // previous authoritative set intact and exposes no partial rows.
    try materialize.reconcile(d, allocator, diff.anchor_plan_id, roadmap_citations);

    // ---- flip anchor draft → active -----------------------------------
    if (std.mem.eql(u8, diff.current_status, "draft")) {
        const active: plan_mod.Status = .active;
        const updated = plan_mod.update(d, allocator, diff.anchor_plan_id, .{ .status = active }) catch |e| return mapPlanErr(e);
        plan_mod.deinit(updated, allocator);
        res.anchor_activated = true;
    }

    // ---- session capture (best-effort) --------------------------------
    appendActionEntry(d, allocator, diff.anchor_plan_id) catch {};

    return res;
}

// =========================================================================
// Helpers
// =========================================================================

/// ResolvedRef is a TaskRef whose slug (if any) has been resolved to a
/// numeric task id. Numeric refs pass through unchanged.
const ResolvedRef = struct {
    kind: []const u8,
    id: i64,
};

fn freeRefs(allocator: std.mem.Allocator, refs: []const ResolvedRef) void {
    for (refs) |r| allocator.free(r.kind);
    allocator.free(refs);
}

fn reconcileRoadmapCitations(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    diff: diff_mod.Diff,
) Error![]const materialize.RoadmapCitation {
    var artifact_stmt = d.prepare(
        \\select a.id
        \\from artifacts a
        \\join entity_links el on el.from_kind = 'artifact' and el.from_id = a.id
        \\  and el.to_kind = 'plan' and el.to_id = ?
        \\  and el.relationship = 'derives-from'
        \\where a.kind = 'roadmap'
        \\order by a.id limit 1
    ) catch return Error.QueryFailed;
    defer artifact_stmt.finalize();
    artifact_stmt.bind(&.{.{ .int = diff.anchor_plan_id }}) catch return Error.QueryFailed;
    switch (artifact_stmt.step() catch return Error.QueryFailed) {
        .done => return try allocator.alloc(materialize.RoadmapCitation, 0),
        .row => {},
    }
    const artifact_id = artifact_stmt.columnInt(0);

    d.exec(
        \\create temp table if not exists spec_ingest_roadmap_citations (
        \\  task_id integer primary key
        \\)
    ) catch return Error.QueryFailed;
    d.exec("delete from temp.spec_ingest_roadmap_citations") catch return Error.QueryFailed;

    var citations = try allocator.alloc(materialize.RoadmapCitation, diff.roadmap_citations.len);
    errdefer allocator.free(citations);
    for (diff.roadmap_citations, 0..) |citation, i| {
        const task_id = if (citation.existing_task_id > 0)
            citation.existing_task_id
        else
            try resolveTaskFromRoadmapMapping(
                d,
                diff.anchor_plan_id,
                citation.child_plan_title,
                citation.task_title,
            );
        citations[i] = .{
            .task_id = task_id,
            .artifact_id = artifact_id,
            .source_locator = citation.source_locator,
            .source_text = citation.source_text,
        };
        _ = d.execParams(
            "insert into temp.spec_ingest_roadmap_citations (task_id) values (?)",
            &.{.{ .int = task_id }},
        ) catch return Error.QueryFailed;
        ensureLink(d, allocator, .task, task_id, .artifact, artifact_id, .cites) catch |e| return e;
    }

    _ = d.execParams(
        \\delete from entity_links
        \\where from_kind = 'task' and to_kind = 'artifact' and to_id = ?
        \\  and relationship = 'cites'
        \\  and from_id in (
        \\    select task_id from routing_task_facts
        \\    where fact_kind = 'cited_artifact_section'
        \\      and source_entity_kind = 'artifact' and source_entity_id = ?
        \\      and (
        \\        source_locator like 'roadmap#milestone:%'
        \\        or source_locator like 'roadmap#task:%'
        \\      )
        \\  )
        \\  and from_id not in (
        \\    select task_id from temp.spec_ingest_roadmap_citations
        \\  )
    , &.{ .{ .int = artifact_id }, .{ .int = artifact_id } }) catch return Error.QueryFailed;
    return citations;
}

fn resolveTaskFromRoadmapMapping(
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
    child_plan_title: []const u8,
    task_title: []const u8,
) Error!i64 {
    var stmt = d.prepare(
        \\select t.id
        \\from tasks t join plans p on p.id = t.plan_id
        \\where p.parent_plan_id = ?
        \\  and lower(trim(p.title)) = lower(trim(?))
        \\  and lower(trim(t.title)) = lower(trim(?))
        \\  and t.status != 'cancelled'
        \\order by t.id limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .int = anchor_plan_id },
        .{ .text = child_plan_title },
        .{ .text = task_title },
    }) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => stmt.columnInt(0),
    };
}

/// resolveSlugRefs maps every `task:<slug>` ref to a numeric task id by
/// looking up the slug among tasks linked to the anchor plan (directly
/// or via child plan derives-from edges). Numeric refs pass through.
///
/// An unresolvable slug aborts the apply with `Error.ResolveSlugFailed`
/// after logging the offending slug + scenario title. This implements
/// the "slug-mandatory-when-cited" rule from plan 286 M2.
fn resolveSlugRefs(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    refs: []const parse.TaskRef,
    scenario_title: []const u8,
    anchor_plan_id: i64,
) Error![]const ResolvedRef {
    var out = try allocator.alloc(ResolvedRef, refs.len);
    // Track how many slots have been initialized so errdefer only frees those;
    // freeing uninitialized slots on OOM mid-resolve would crash with garbage
    // pointers. `i` is the loop index below; freeing `[0..i]` is correct.
    var i: usize = 0;
    errdefer {
        for (out[0..i]) |r| allocator.free(r.kind);
        allocator.free(out);
    }
    while (i < refs.len) : (i += 1) {
        if (refs[i].slug.len == 0) {
            out[i] = .{
                .kind = try allocator.dupe(u8, refs[i].kind),
                .id = refs[i].id,
            };
            continue;
        }
        var stmt = d.prepare(
            \\select t.id from tasks t
            \\where t.slug = ?
            \\  and (
            \\    t.plan_id = ?
            \\    or t.plan_id in (
            \\      select from_id from entity_links
            \\       where from_kind = 'plan'
            \\         and to_kind = 'plan'
            \\         and to_id = ?
            \\         and relationship = 'derives-from'
            \\    )
            \\  )
            \\limit 1
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{
            .{ .text = refs[i].slug },
            .{ .int = anchor_plan_id },
            .{ .int = anchor_plan_id },
        }) catch return Error.QueryFailed;

        switch (stmt.step() catch return Error.QueryFailed) {
            .done => {
                std.log.err(
                    "unresolvable slug {s}:{s} cited by scenario '{s}': no task with that slug under anchor plan {d}",
                    .{ refs[i].kind, refs[i].slug, scenario_title, anchor_plan_id },
                );
                return Error.ResolveSlugFailed;
            },
            .row => {
                out[i] = .{
                    .kind = try allocator.dupe(u8, refs[i].kind),
                    .id = stmt.columnInt(0),
                };
            },
        }
    }
    return out;
}

/// ensureLink adds an entity_links row; existing rows are tolerated as
/// idempotent no-ops. Other errors propagate.
fn ensureLink(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    from_kind: entitylink.EntityKind,
    from_id: i64,
    to_kind: entitylink.EntityKind,
    to_id: i64,
    rel: entitylink.Relationship,
) entitylink.Error!void {
    const link = entitylink.add(d, allocator, .{
        .from_kind = from_kind,
        .from_id = from_id,
        .to_kind = to_kind,
        .to_id = to_id,
        .relationship = rel,
    }) catch |e| switch (e) {
        entitylink.Error.LinkExists => return,
        else => return e,
    };
    entitylink.deinit(link, allocator);
}

/// applyTouchesLinks inserts `entity_links(relationship='touches')` for
/// each repo slug. A slug that doesn't resolve to a known projects.slug
/// row is skipped silently — matches Go's behavior (the slug may not be
/// registered in this DB).
fn applyTouchesLinks(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
    slugs: []const []const u8,
) Error!void {
    for (slugs) |slug| {
        const repo_id = repoIdBySlug(d, slug) catch |err| switch (err) {
            error.NotFound => null,
            else => return error.QueryFailed,
        };
        if (repo_id) |rid| {
            ensureLink(d, allocator, .task, task_id, .repo, rid, .touches) catch |e| return e;
        }
    }
}

fn repoIdBySlug(d: *db.sqlite.Db, slug: []const u8) !i64 {
    var stmt = try d.prepare("select id from projects where slug = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .text = slug }});
    switch (try stmt.step()) {
        .done => return error.NotFound,
        .row => return stmt.columnInt(0),
    }
}

fn readTaskSlug(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
) Error!?[]const u8 {
    var stmt = d.prepare("select slug from tasks where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => return Error.NotFound,
        .row => return stmt.columnTextOpt(0, allocator) catch return Error.QueryFailed,
    }
}

fn retireTaskForSpecRemoval(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
) Error!void {
    d.savepoint(allocator, "spec_retire_task") catch return Error.QueryFailed;
    var released = false;
    defer {
        if (!released) {
            d.rollbackToSavepoint(allocator, "spec_retire_task") catch {};
            d.releaseSavepoint(allocator, "spec_retire_task") catch {};
        }
    }

    const cancelled = task_mod.markCancelled(d, allocator, id) catch |e| return mapTaskErr(e);
    task_mod.deinit(cancelled, allocator);
    _ = d.execParams(
        \\update tasks
        \\set slug = null,
        \\    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\where id = ?
    , &.{.{ .int = id }}) catch return Error.QueryFailed;

    d.releaseSavepoint(allocator, "spec_retire_task") catch return Error.QueryFailed;
    released = true;
}

fn retirePlanForSpecRemoval(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
) Error!void {
    d.savepoint(allocator, "spec_retire_plan") catch return Error.QueryFailed;
    var released = false;
    defer {
        if (!released) {
            d.rollbackToSavepoint(allocator, "spec_retire_plan") catch {};
            d.releaseSavepoint(allocator, "spec_retire_plan") catch {};
        }
    }

    const current = plan_mod.show(d, allocator, id) catch |e| return mapPlanErr(e);
    defer plan_mod.deinit(current, allocator);
    const stale_slug = try std.fmt.allocPrint(allocator, "stale-{d}-{s}", .{ id, current.slug });
    defer allocator.free(stale_slug);

    // Rename the slug first (frees it for slug-reuse of an incoming replacement
    // with the same derived slug). This uses plan_mod.update so the audit
    // record lands; it does NOT set status here because plan_mod.update routes
    // through policy.status.check.
    const renamed = plan_mod.update(d, allocator, id, .{ .slug = stale_slug }) catch |e| return mapPlanErr(e);
    plan_mod.deinit(renamed, allocator);

    // INTENTIONAL bypass of policy.status.check: spec-ingest retirement is an
    // engine-internal operation, not an operator transition. The plan may be in
    // any lifecycle state (draft, active, done, etc.) when its spec milestone is
    // removed, and in all cases it must become `abandoned`. The operator
    // transition matrix correctly refuses `done → abandoned` and `draft →
    // abandoned`, but those rules apply to operator-driven moves, not engine
    // retirements. This mirrors the recomputeStatus and closeout apply paths.
    _ = d.execParams(
        "update plans set status = 'abandoned', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
        &.{.{ .int = id }},
    ) catch return Error.QueryFailed;

    d.releaseSavepoint(allocator, "spec_retire_plan") catch return Error.QueryFailed;
    released = true;
}

/// appendReadEntry tries to append a `prefix='read'` session entry; a
/// missing active session is tolerated.
fn appendReadEntry(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) !void {
    const summary = try std.fmt.allocPrint(allocator, "spec ingest preview plan:{d}", .{anchor_plan_id});
    defer allocator.free(summary);
    const sid = session_mod.ensureActive(d, allocator, "ingestor", null) catch return;
    if (sid == 0) return;
    session_mod.appendEntry(d, sid, "read", summary) catch return;
}

/// appendActionEntry tries to append a `prefix='action'` session entry;
/// a missing active session is tolerated.
fn appendActionEntry(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) !void {
    const summary = try std.fmt.allocPrint(allocator, "spec ingest apply plan:{d}", .{anchor_plan_id});
    defer allocator.free(summary);
    const sid = session_mod.ensureActive(d, allocator, "ingestor", null) catch return;
    if (sid == 0) return;
    session_mod.appendEntry(d, sid, "action", summary) catch return;
}

// =========================================================================
// Error mappers
// =========================================================================
//
// Engine modules have richer error sets than `apply` exposes. Map narrow
// engine-side errors onto our broader Error to keep `apply` callable from
// the handler without per-error switch noise.

fn mapPlanErr(e: plan_mod.Error) Error {
    return e;
}
fn mapTaskErr(e: task_mod.Error) Error {
    return e;
}
fn mapDecisionErr(e: decision_mod.Error) Error {
    return e;
}
fn mapQuestionErr(e: question_mod.Error) Error {
    return e;
}
fn mapScenarioErr(e: scenario_mod.Error) Error {
    return e;
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

test "apply preview: writes nothing, returns empty result" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anch', 'draft')",
        &.{},
    );
    const anchor_id = try d.intQuery("select id from plans where slug = 'anch'");
    const diff = diff_mod.Diff{
        .anchor_plan_id = anchor_id,
        .anchor_slug = try a.dupe(u8, "anch"),
        .assoc_slug = try a.dupe(u8, ""),
        .current_status = try a.dupe(u8, "draft"),
    };
    defer diff_mod.deinitDiff(diff, a);

    const res = try apply(&d, a, diff, .{ .apply = false });
    try testing.expectEqual(@as(usize, 0), res.plans_created);
    try testing.expectEqual(@as(usize, 0), res.tasks_created);
    try testing.expect(!res.anchor_activated);
    // Anchor is still draft.
    const status_txt = try readPlanStatus(&d, a, anchor_id);
    defer a.free(status_txt);
    try testing.expectEqualStrings("draft", status_txt);
}

fn readPlanStatus(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) ![]const u8 {
    var stmt = try d.prepare("select status from plans where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = plan_id }});
    _ = try stmt.step();
    return try stmt.columnTextAlloc(0, allocator);
}

test "apply: preserves an explicit manual roadmap section citation and its fact" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try d.exec(
        \\insert into plans (
        \\  scope_kind, title, slug, status
        \\) values ('global', 'Anchor', 'manual-roadmap-anchor', 'active');
        \\insert into plans (
        \\  scope_kind, title, slug, status, parent_plan_id
        \\) values ('global', 'Child', 'manual-roadmap-child', 'active', 1);
        \\insert into artifacts (
        \\  scope_kind, kind, title, body
        \\) values (
        \\  'global', 'roadmap', 'Roadmap',
        \\  '## Manual Evidence
        \\
        \\MANUAL_ROADMAP_SENTINEL'
        \\);
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('artifact', 1, 'plan', 1, 'derives-from');
        \\insert into tasks (
        \\  scope_kind, plan_id, title, body, next_action
        \\) values (
        \\  'global', 2, 'Manual roadmap citation',
        \\  '## Acceptance Criteria
        \\
        \\- Preserve explicit manual roadmap evidence.
        \\
        \\## Spec Citations
        \\
        \\- artifact:1#Manual Evidence',
        \\  'Preserve the explicit manual roadmap citation.'
        \\);
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 1, 'artifact', 1, 'cites')
    );

    const ingest_diff = diff_mod.Diff{
        .anchor_plan_id = 1,
        .anchor_slug = try a.dupe(u8, "manual-roadmap-anchor"),
        .assoc_slug = try a.dupe(u8, ""),
        .current_status = try a.dupe(u8, "active"),
    };
    defer diff_mod.deinitDiff(ingest_diff, a);

    _ = try apply(&d, a, ingest_diff, .{ .apply = true });
    try testing.expectEqual(@as(i64, 1), try d.intQuery(
        \\select count(*) from entity_links
        \\where from_kind = 'task' and from_id = 1
        \\  and to_kind = 'artifact' and to_id = 1 and relationship = 'cites'
    ));
    try testing.expectEqual(@as(i64, 1), try d.intQuery(
        \\select count(*) from routing_task_facts
        \\where task_id = 1 and fact_kind = 'cited_artifact_section'
        \\  and value_text = 'MANUAL_ROADMAP_SENTINEL'
        \\  and source_entity_kind = 'artifact' and source_entity_id = 1
        \\  and source_locator = 'artifact:1#Manual Evidence'
        \\  and length(source_digest) = 64
        \\  and materializer_version = 'spec-ingest-v1'
    ));
    const initial_fact_id = try d.intQuery(
        \\select id from routing_task_facts
        \\where task_id = 1 and fact_kind = 'cited_artifact_section'
    );

    _ = try apply(&d, a, ingest_diff, .{ .apply = true });
    try testing.expectEqual(@as(i64, 1), try d.intQuery(
        \\select count(*) from routing_task_facts
        \\where task_id = 1 and fact_kind = 'cited_artifact_section'
    ));
    try testing.expectEqual(initial_fact_id, try d.intQuery(
        \\select id from routing_task_facts
        \\where task_id = 1 and fact_kind = 'cited_artifact_section'
    ));
}

test "apply: creates child plan + task and flips anchor draft → active" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anch2', 'draft')",
        &.{},
    );
    const anchor_id = try d.intQuery("select id from plans where slug = 'anch2'");
    try d.exec(
        \\insert into artifacts (scope_kind, kind, title, body)
        \\values (
        \\  'global', 'roadmap', 'Mutable display label',
        \\  '## M1
        \\
        \\- ROADMAP_SENTINEL [slug: imp-foo]
        \\- SLUGLESS_SENTINEL wraps
        \\  onto a continuation line'
        \\);
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('artifact', 1, 'plan', 1, 'derives-from')
    );

    var tasks = try a.alloc(diff_mod.TaskEntry, 2);
    tasks[0] = .{
        .op = .add,
        .title = try a.dupe(u8, "Implement foo"),
        .body = try a.dupe(u8, "## Acceptance Criteria\n\n- foo\n"),
        .touches = &.{},
        .slug = try a.dupe(u8, "imp-foo"),
        .existing_id = 0,
        .child_plan_title = try a.dupe(u8, "M1"),
    };
    tasks[1] = .{
        .op = .add,
        .title = try a.dupe(u8, "Implement slugless"),
        .body = try a.dupe(u8, "## Acceptance Criteria\n\n- slugless sentinel\n"),
        .touches = &.{},
        .slug = try a.dupe(u8, ""),
        .existing_id = 0,
        .child_plan_title = try a.dupe(u8, "M1"),
    };
    var plans = try a.alloc(diff_mod.PlanEntry, 1);
    plans[0] = .{
        .op = .add,
        .title = try a.dupe(u8, "M1"),
        .existing_id = 0,
        .tasks = tasks,
    };
    var decisions = try a.alloc(diff_mod.DecisionEntry, 1);
    decisions[0] = .{
        .op = .add,
        .title = try a.dupe(u8, "Decision display label"),
        .body = try a.dupe(u8, "DECISION_SENTINEL atomic transaction"),
    };
    var questions = try a.alloc(parse.Question, 1);
    questions[0] = .{
        .title = try a.dupe(u8, "Question display label"),
        .body = try a.dupe(u8, "QUESTION_SENTINEL"),
        .resolution = try a.dupe(u8, ""),
    };
    var verifies = try a.alloc(parse.TaskRef, 1);
    verifies[0] = .{
        .kind = try a.dupe(u8, "task"),
        .id = 0,
        .slug = try a.dupe(u8, "imp-foo"),
    };
    var scenarios = try a.alloc(diff_mod.ScenarioEntry, 1);
    scenarios[0] = .{
        .op = .add,
        .title = try a.dupe(u8, "Scenario display label"),
        .body = try a.dupe(u8, "**Acceptance:** SCENARIO_SENTINEL exits zero"),
        .kind = try a.dupe(u8, "integration"),
        .acceptance = try a.dupe(u8, "SCENARIO_SENTINEL exits zero"),
        .verifies = verifies,
    };
    var roadmap_citations = try a.alloc(diff_mod.RoadmapCitation, 2);
    roadmap_citations[0] = .{
        .child_plan_title = try a.dupe(u8, "M1"),
        .task_title = try a.dupe(u8, "Implement foo"),
        .source_locator = try a.dupe(u8, "roadmap#milestone:1/item:1"),
        .source_text = try a.dupe(u8, "- ROADMAP_SENTINEL [slug: imp-foo]"),
    };
    roadmap_citations[1] = .{
        .child_plan_title = try a.dupe(u8, "M1"),
        .task_title = try a.dupe(u8, "Implement slugless"),
        .source_locator = try a.dupe(u8, "roadmap#milestone:1/item:2"),
        .source_text = try a.dupe(u8, "- SLUGLESS_SENTINEL wraps onto a continuation line"),
    };
    const diff = diff_mod.Diff{
        .anchor_plan_id = anchor_id,
        .anchor_slug = try a.dupe(u8, "anch2"),
        .assoc_slug = try a.dupe(u8, ""),
        .current_status = try a.dupe(u8, "draft"),
        .child_plans = plans,
        .decisions = decisions,
        .new_questions = questions,
        .scenarios = scenarios,
        .roadmap_citations = roadmap_citations,
    };
    defer diff_mod.deinitDiff(diff, a);

    const preview = try apply(&d, a, diff, .{ .apply = false });
    try testing.expectEqual(@as(usize, 0), preview.tasks_created);
    try testing.expectEqual(@as(i64, 0), try d.intQuery("select count(*) from routing_task_facts"));

    const res = try apply(&d, a, diff, .{ .apply = true });
    try testing.expectEqual(@as(usize, 1), res.plans_created);
    try testing.expectEqual(@as(usize, 2), res.tasks_created);
    try testing.expect(res.anchor_activated);

    // Anchor is now active.
    const status_txt = try readPlanStatus(&d, a, anchor_id);
    defer a.free(status_txt);
    try testing.expectEqualStrings("active", status_txt);

    // Task exists with expected slug.
    const tcount = try d.intQuery("select count(*) from tasks where slug = 'imp-foo'");
    try testing.expectEqual(@as(i64, 1), tcount);

    // derives-from edges: plan→anchor and task→plan.
    var stmt = try d.prepare(
        \\select count(*) from entity_links
        \\where relationship = 'derives-from'
        \\  and ((from_kind='plan' and to_kind='plan' and to_id=?)
        \\    or (from_kind='task' and to_kind='plan'))
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = anchor_id }});
    _ = try stmt.step();
    try testing.expect(stmt.columnInt(0) >= 2);

    try testing.expectEqual(@as(i64, 1), try d.intQuery(
        \\select count(*) from entity_links
        \\where from_kind = 'task' and from_id = 1
        \\  and to_kind = 'artifact' and to_id = 1 and relationship = 'cites'
    ));
    try testing.expectEqual(@as(i64, 1), try d.intQuery(
        \\select count(*) from routing_task_facts
        \\where task_id = 2 and fact_kind = 'cited_artifact_section'
        \\  and value_text = '- SLUGLESS_SENTINEL wraps onto a continuation line'
        \\  and source_locator = 'roadmap#milestone:1/item:2'
    ));
    try testing.expectEqual(@as(i64, 1), try d.intQuery(
        \\select count(*) from routing_task_facts
        \\where fact_kind = 'cited_artifact_section'
        \\  and value_text = '- ROADMAP_SENTINEL [slug: imp-foo]'
        \\  and source_entity_kind = 'artifact' and source_entity_id = 1
        \\  and source_locator = 'roadmap#milestone:1/item:1'
    ));
    try testing.expectEqual(@as(i64, 1), try d.intQuery(
        \\select count(*) from routing_task_facts
        \\where fact_kind = 'validation_gate'
        \\  and value_text = 'SCENARIO_SENTINEL exits zero'
    ));
    try testing.expectEqual(@as(i64, 2), try d.intQuery(
        "select count(*) from routing_task_facts where fact_kind = 'unresolved_question'",
    ));
    const scenario_digest = try materialize.sourceDigestAlloc(
        a,
        "test_scenario",
        1,
        "body#acceptance",
        "SCENARIO_SENTINEL exits zero",
    );
    defer a.free(scenario_digest);
    try expectFactDigest(&d, "validation_gate", scenario_digest);
    const question_digest = try materialize.sourceDigestAlloc(
        a,
        "question",
        1,
        "status",
        "QUESTION_SENTINEL",
    );
    defer a.free(question_digest);
    try expectFactDigest(&d, "unresolved_question", question_digest);
    // Ingest creates proposed decisions. They are not locked facts.
    try testing.expectEqual(@as(i64, 0), try d.intQuery(
        "select count(*) from routing_task_facts where fact_kind = 'locked_decision'",
    ));

    const bullet = "- ROADMAP_SENTINEL [slug: imp-foo]";
    const expected_digest = try materialize.sourceDigestAlloc(a, "artifact", 1, "roadmap#milestone:1/item:1", bullet);
    defer a.free(expected_digest);
    const initial_digest = try readFactDigest(&d, a, "cited_artifact_section");
    defer a.free(initial_digest);
    try testing.expectEqualStrings(expected_digest, initial_digest);
    const initial_fact_id = try d.intQuery(
        "select id from routing_task_facts where fact_kind = 'cited_artifact_section'",
    );

    var replay_citations = try a.alloc(diff_mod.RoadmapCitation, 2);
    replay_citations[0] = .{
        .existing_task_id = 1,
        .child_plan_title = try a.dupe(u8, "M1"),
        .task_title = try a.dupe(u8, "Implement foo"),
        .source_locator = try a.dupe(u8, "roadmap#milestone:1/item:1"),
        .source_text = try a.dupe(u8, "- ROADMAP_SENTINEL [slug: imp-foo]"),
    };
    replay_citations[1] = .{
        .existing_task_id = 2,
        .child_plan_title = try a.dupe(u8, "M1"),
        .task_title = try a.dupe(u8, "Implement slugless"),
        .source_locator = try a.dupe(u8, "roadmap#milestone:1/item:2"),
        .source_text = try a.dupe(u8, "- SLUGLESS_SENTINEL wraps onto a continuation line"),
    };
    const replay_diff = diff_mod.Diff{
        .anchor_plan_id = anchor_id,
        .anchor_slug = try a.dupe(u8, "anch2"),
        .assoc_slug = try a.dupe(u8, ""),
        .current_status = try a.dupe(u8, "active"),
        .roadmap_citations = replay_citations,
    };
    defer diff_mod.deinitDiff(replay_diff, a);
    // Persisted slug drift does not affect the parsed work-item mapping.
    try d.exec("update tasks set slug = 'stale-slug' where id = 1");
    _ = try apply(&d, a, replay_diff, .{ .apply = true });
    try testing.expectEqual(initial_fact_id, try d.intQuery(
        "select id from routing_task_facts where fact_kind = 'cited_artifact_section'",
    ));

    // A manually authored roadmap citation with no prior ingestor-owned fact
    // is outside the reconciliation ownership boundary and remains intact.
    try d.exec(
        \\insert into tasks (
        \\  scope_kind, plan_id, title, body, next_action
        \\) values (
        \\  'global', 2, 'Manual citation task', 'manual', 'manual'
        \\);
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 3, 'artifact', 1, 'cites')
    );
    _ = try apply(&d, a, replay_diff, .{ .apply = true });
    try testing.expectEqual(@as(i64, 1), try d.intQuery(
        \\select count(*) from entity_links
        \\where from_kind = 'task' and from_id = 3
        \\  and to_kind = 'artifact' and to_id = 1 and relationship = 'cites'
    ));
    const fact_id_after_manual_citation = try d.intQuery(
        \\select id from routing_task_facts
        \\where task_id = 1 and fact_kind = 'cited_artifact_section'
    );

    // Display labels and timestamps are excluded from canonical lineage.
    try d.exec(
        \\update artifacts set title = 'Another display label',
        \\  updated_at = '2099-01-01T00:00:00Z' where id = 1
    );
    _ = try apply(&d, a, replay_diff, .{ .apply = true });
    try testing.expectEqual(fact_id_after_manual_citation, try d.intQuery(
        \\select id from routing_task_facts
        \\where task_id = 1 and fact_kind = 'cited_artifact_section'
    ));

    // A semantic source change changes the exact canonical digest.
    try d.exec(
        \\update artifacts set body = '## M1
        \\
        \\- ROADMAP_SENTINEL_CHANGED [slug: changed-source-slug]
        \\- SLUGLESS_SENTINEL wraps
        \\  onto a continuation line' where id = 1
    );
    // The parsed mapping, rather than persisted task slugs, carries the exact
    // current roadmap source into reconciliation.
    a.free(replay_citations[0].source_text);
    replay_citations[0].source_text =
        try a.dupe(u8, "- ROADMAP_SENTINEL_CHANGED [slug: changed-source-slug]");
    _ = try apply(&d, a, replay_diff, .{ .apply = true });
    const changed_digest = try readFactDigest(&d, a, "cited_artifact_section");
    defer a.free(changed_digest);
    try testing.expect(!std.mem.eql(u8, initial_digest, changed_digest));

    // Accepted decisions are materialized; proposed decisions above were not.
    try d.exec("update decisions set status = 'accepted' where id = 1");
    _ = try apply(&d, a, replay_diff, .{ .apply = true });
    try testing.expectEqual(@as(i64, 3), try d.intQuery(
        \\select count(*) from routing_task_facts
        \\where fact_kind = 'locked_decision'
        \\  and value_text = 'DECISION_SENTINEL atomic transaction'
    ));
    const decision_digest = try materialize.sourceDigestAlloc(
        a,
        "decision",
        1,
        "body",
        "DECISION_SENTINEL atomic transaction",
    );
    defer a.free(decision_digest);
    try expectFactDigest(&d, "locked_decision", decision_digest);
    try testing.expectEqual(@as(i64, 0), try d.intQuery(
        \\select count(*) from routing_task_facts
        \\where fact_kind like '%model%' or fact_kind like '%provider%'
        \\   or fact_kind like '%tier%' or fact_kind like '%recommend%'
    ));

    // Link insertion order does not affect the stable fact set.
    try d.exec(
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 1, 'task', 99, 'blocks');
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 1, 'task', 98, 'blocks')
    );
    _ = try apply(&d, a, replay_diff, .{ .apply = true });
    try testing.expectEqual(@as(i64, 2), try d.intQuery(
        "select value_integer from routing_task_facts where fact_kind = 'dependency_fanout'",
    ));
    const ordered_links = try readOrderedLinkFacts(&d, a);
    defer a.free(ordered_links);
    const max_fact_id = try d.intQuery("select max(id) from routing_task_facts");
    try d.exec(
        \\delete from entity_links where from_kind = 'task' and from_id = 1
        \\  and to_kind = 'task' and relationship = 'blocks';
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 1, 'task', 98, 'blocks');
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 1, 'task', 99, 'blocks')
    );
    _ = try apply(&d, a, replay_diff, .{ .apply = true });
    const reordered_links = try readOrderedLinkFacts(&d, a);
    defer a.free(reordered_links);
    try testing.expectEqualStrings(ordered_links, reordered_links);
    try testing.expectEqual(max_fact_id, try d.intQuery("select max(id) from routing_task_facts"));

    // Non-roadmap citations carry an explicit artifact+section locator in the
    // task body and digest only that named section.
    try d.exec(
        \\insert into artifacts (scope_kind, kind, title, body)
        \\values (
        \\  'global', 'tech_spec', 'Tech display label',
        \\  '## Transaction
        \\
        \\TECH_SENTINEL
        \\
        \\## Uncited
        \\
        \\IGNORED'
        \\);
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 1, 'artifact', 2, 'cites');
        \\update tasks set body = body || '
        \\
        \\## Spec Citations
        \\
        \\- artifact:2#Transaction' where id = 1
    );
    _ = try apply(&d, a, replay_diff, .{ .apply = true });
    try testing.expectEqual(@as(i64, 1), try d.intQuery(
        \\select count(*) from routing_task_facts
        \\where fact_kind = 'cited_artifact_section'
        \\  and source_entity_id = 2
        \\  and source_locator = 'artifact:2#Transaction'
        \\  and value_text = 'TECH_SENTINEL'
    ));
    const tech_digest = try materialize.sourceDigestAlloc(
        a,
        "artifact",
        2,
        "artifact:2#Transaction",
        "TECH_SENTINEL",
    );
    defer a.free(tech_digest);
    var tech_digest_stmt = try d.prepare(
        \\select source_digest from routing_task_facts
        \\where fact_kind = 'cited_artifact_section' and source_entity_id = 2
    );
    defer tech_digest_stmt.finalize();
    _ = try tech_digest_stmt.step();
    const actual_tech_digest = try tech_digest_stmt.columnTextAlloc(0, a);
    defer a.free(actual_tech_digest);
    try testing.expectEqualStrings(tech_digest, actual_tech_digest);

    // An explicit citation without an identifiable section is a real
    // derivation failure. The ingest savepoint rolls back graph and fact writes.
    try d.exec(
        \\insert into artifacts (scope_kind, kind, title, body)
        \\values ('global', 'tech_spec', 'Bad citation', 'no section locator');
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 1, 'artifact', 3, 'cites')
    );
    var bad_tasks = try a.alloc(diff_mod.TaskEntry, 1);
    bad_tasks[0] = .{
        .op = .update,
        .title = try a.dupe(u8, "Implement foo"),
        .body = try a.dupe(
            u8,
            "## Acceptance Criteria\n\n- changed in failed apply\n\n## Spec Citations\n\n- artifact:2#Transaction\n",
        ),
        .touches = &.{},
        .slug = try a.dupe(u8, "stale-slug"),
        .existing_id = 1,
        .child_plan_title = try a.dupe(u8, "M1"),
    };
    var bad_plans = try a.alloc(diff_mod.PlanEntry, 1);
    bad_plans[0] = .{
        .op = .update,
        .title = try a.dupe(u8, "M1"),
        .existing_id = 2,
        .tasks = bad_tasks,
    };
    var bad_citations = try a.alloc(diff_mod.RoadmapCitation, 2);
    bad_citations[0] = .{
        .existing_task_id = 1,
        .child_plan_title = try a.dupe(u8, "M1"),
        .task_title = try a.dupe(u8, "Implement foo"),
        .source_locator = try a.dupe(u8, "roadmap#milestone:1/item:1"),
        .source_text = try a.dupe(u8, "- ROADMAP_SENTINEL_CHANGED [slug: changed-source-slug]"),
    };
    bad_citations[1] = .{
        .existing_task_id = 2,
        .child_plan_title = try a.dupe(u8, "M1"),
        .task_title = try a.dupe(u8, "Implement slugless"),
        .source_locator = try a.dupe(u8, "roadmap#milestone:1/item:2"),
        .source_text = try a.dupe(u8, "- SLUGLESS_SENTINEL wraps onto a continuation line"),
    };
    const bad_diff = diff_mod.Diff{
        .anchor_plan_id = anchor_id,
        .anchor_slug = try a.dupe(u8, "anch2"),
        .assoc_slug = try a.dupe(u8, ""),
        .current_status = try a.dupe(u8, "active"),
        .child_plans = bad_plans,
        .roadmap_citations = bad_citations,
    };
    defer diff_mod.deinitDiff(bad_diff, a);
    const facts_before_failure = try d.intQuery("select count(*) from routing_task_facts");
    const fact_set_before_failure = try readAllFacts(&d, a);
    defer a.free(fact_set_before_failure);
    try testing.expectError(materialize.Error.InvalidCitation, apply(&d, a, bad_diff, .{ .apply = true }));
    try testing.expectEqual(@as(i64, 0), try d.intQuery(
        \\select count(*) from tasks
        \\where id = 1 and body like '%changed in failed apply%'
    ));
    try testing.expectEqual(facts_before_failure, try d.intQuery("select count(*) from routing_task_facts"));
    const fact_set_after_failure = try readAllFacts(&d, a);
    defer a.free(fact_set_after_failure);
    try testing.expectEqualStrings(fact_set_before_failure, fact_set_after_failure);
}

fn readFactDigest(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    fact_kind: []const u8,
) ![]const u8 {
    var stmt = try d.prepare(
        "select source_digest from routing_task_facts where fact_kind = ? order by id limit 1",
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .text = fact_kind }});
    _ = try stmt.step();
    return try stmt.columnTextAlloc(0, allocator);
}

fn readOrderedLinkFacts(d: *db.sqlite.Db, allocator: std.mem.Allocator) ![]const u8 {
    var stmt = try d.prepare(
        \\select group_concat(row_text, '|') from (
        \\  select fact_kind || ':' || source_entity_kind || ':' ||
        \\         source_entity_id || ':' || source_locator || ':' ||
        \\         source_digest as row_text
        \\  from routing_task_facts
        \\  where fact_kind in ('blocks', 'blocked_by', 'dependency_fanout')
        \\  order by source_entity_kind, source_entity_id, source_locator, fact_kind
        \\)
    );
    defer stmt.finalize();
    _ = try stmt.step();
    return try stmt.columnTextAlloc(0, allocator);
}

fn readAllFacts(d: *db.sqlite.Db, allocator: std.mem.Allocator) ![]const u8 {
    var stmt = try d.prepare(
        \\select group_concat(row_text, '|') from (
        \\  select task_id || ':' || fact_kind || ':' || value_type || ':' ||
        \\         coalesce(value_bool, '') || ':' || coalesce(value_integer, '') || ':' ||
        \\         coalesce(value_text, '') || ':' || source_entity_kind || ':' ||
        \\         source_entity_id || ':' || source_locator || ':' || source_digest || ':' ||
        \\         materializer_version as row_text
        \\  from routing_task_facts
        \\  order by task_id, source_entity_kind, source_entity_id, source_locator,
        \\           fact_kind, value_type, coalesce(value_text, ''),
        \\           coalesce(value_integer, value_bool)
        \\)
    );
    defer stmt.finalize();
    _ = try stmt.step();
    return try stmt.columnTextAlloc(0, allocator);
}

fn expectFactDigest(d: *db.sqlite.Db, fact_kind: []const u8, expected: []const u8) !void {
    var stmt = try d.prepare(
        "select source_digest from routing_task_facts where fact_kind = ? order by id limit 1",
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .text = fact_kind }});
    _ = try stmt.step();
    const actual = try stmt.columnTextAlloc(0, testing.allocator);
    defer testing.allocator.free(actual);
    try testing.expectEqualStrings(expected, actual);
}

test "apply: --apply-removals without --apply is rejected" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const diff = diff_mod.Diff{
        .anchor_plan_id = 999,
        .anchor_slug = try a.dupe(u8, ""),
        .assoc_slug = try a.dupe(u8, ""),
        .current_status = try a.dupe(u8, "draft"),
    };
    defer diff_mod.deinitDiff(diff, a);
    try testing.expectError(Error.ApplyRemovalsRequiresApply, apply(&d, a, diff, .{ .apply = false, .apply_removals = true }));
}
