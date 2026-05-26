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
//! Per-statement DB ops; no mega-transaction. A failure mid-apply leaves
//! the DB in a partially-applied state that is resumable by re-running
//! the ingestor (the Diff is recomputed against the new DB state and only
//! the still-missing entities are inserted).

const std = @import("std");
const db = @import("db");
const diff_mod = @import("diff.zig");
const parse = @import("parse.zig");
const scenarios_mod = @import("scenarios.zig");
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
    /// during apply. `null` = global. Mirrors Go's
    /// `runSpecIngestOne` behavior: child entities inherit the operator's
    /// resolved write scope (anchor.AssocSlug or the explicit `--scope`).
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
/// `draft` — the anchor plan is flipped to `active`.
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

    // Anchor scope. Threaded from the handler via opts.scope so child
    // plans, tasks, decisions, and scenarios land in the operator's
    // resolved write scope (or global when null). Mirrors Go's
    // runSpecIngestOne which uses anchor.AssocSlug / --scope.
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
                        .next_action = "Implement per acceptance criteria.",
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
                    _ = d.execParams(
                        \\update tasks set body = ?,
                        \\                  updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
                        \\where id = ?
                    , &.{
                        .{ .text = te.body },
                        .{ .int = te.existing_id },
                    }) catch return Error.QueryFailed;

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
        if (q.resolution.len > 0) {
            // Insert directly as answered: schema CHECK requires both
            // answer_body and answered_at to be set atomically with
            // status='answered'.
            const scope_kind_str: []const u8 = "global";
            _ = d.execParams(
                \\insert into questions (scope_kind, scope_id, title, body, status,
                \\                       answer_body, answered_at)
                \\values (?, null, ?, ?, 'answered', ?,
                \\        strftime('%Y-%m-%dT%H:%M:%fZ','now'))
            , &.{
                .{ .text = scope_kind_str },
                .{ .text = q.title },
                if (q.body.len > 0) .{ .text = q.body } else .{ .null = {} },
                .{ .text = q.resolution },
            }) catch return Error.QueryFailed;
        } else {
            const created = question_mod.create(d, allocator, .{
                .title = q.title,
                .body = if (q.body.len > 0) q.body else null,
                .scope = scope_slug,
            }) catch |e| return mapQuestionErr(e);
            defer question_mod.deinit(created, allocator);
        }
        res.questions_added += 1;
    }

    // ---- question status flips ----------------------------------------
    for (diff.updated_question_status) |sc| {
        const answered = question_mod.answer(d, allocator, sc.question_id, sc.answer) catch |e| return mapQuestionErr(e);
        defer question_mod.deinit(answered, allocator);
        res.questions_answered += 1;
    }

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
        const repo_id = repoIdBySlug(d, slug) catch null;
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

    const abandoned: plan_mod.Status = .abandoned;
    const updated = plan_mod.update(d, allocator, id, .{
        .slug = stale_slug,
        .status = abandoned,
    }) catch |e| switch (e) {
        error.IllegalTransition => blk: {
            const renamed = plan_mod.update(d, allocator, id, .{ .slug = stale_slug }) catch |rename_err| return mapPlanErr(rename_err);
            break :blk renamed;
        },
        else => return mapPlanErr(e),
    };
    plan_mod.deinit(updated, allocator);

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

test "apply: creates child plan + task and flips anchor draft → active" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anch2', 'draft')",
        &.{},
    );
    const anchor_id = try d.intQuery("select id from plans where slug = 'anch2'");

    var tasks = try a.alloc(diff_mod.TaskEntry, 1);
    tasks[0] = .{
        .op = .add,
        .title = try a.dupe(u8, "Implement foo"),
        .body = try a.dupe(u8, "## Acceptance Criteria\n\n- foo\n"),
        .touches = &.{},
        .slug = try a.dupe(u8, "imp-foo"),
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
    const diff = diff_mod.Diff{
        .anchor_plan_id = anchor_id,
        .anchor_slug = try a.dupe(u8, "anch2"),
        .assoc_slug = try a.dupe(u8, ""),
        .current_status = try a.dupe(u8, "draft"),
        .child_plans = plans,
    };
    defer diff_mod.deinitDiff(diff, a);

    const res = try apply(&d, a, diff, .{ .apply = true });
    try testing.expectEqual(@as(usize, 1), res.plans_created);
    try testing.expectEqual(@as(usize, 1), res.tasks_created);
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
