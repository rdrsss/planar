//! engine/ingestor/diff — Compute a proposed Diff from parsed spec data + DB state.
//!
//! Mirrors Go's `internal/ingestor/diff.go`. Two-phase ingest semantics:
//!   1. parse markdown (see parse.zig) → produces typed structs.
//!   2. `compute` reads existing DB rows and produces a Diff of OpAdd /
//!      OpUpdate / OpRemove entries.
//!   3. (separate) `apply` (apply.zig) commits the Diff.
//!
//! Reconciliation key is the entity's title (case-insensitive, trimmed):
//!   * milestone H2 → child plan, matched by title;
//!   * roadmap bullet → task under that child plan, matched by title;
//!   * tech-spec H3 under `## Decisions` → decision, matched by title;
//!   * tech-spec H3 under `## Open Questions` → question, matched by title
//!     scoped to the anchor plan's scope;
//!   * test-spec H3 under `## Scenarios` → test_scenarios row, matched by
//!     title against scenarios linked to the anchor plan.
//!
//! The Slug back-fill on existing tasks (plan 286 M1 policy) only fires
//! when the roadmap bullet has gained a `[slug:]` annotation AND the DB
//! row's slug is still NULL — we never overwrite a non-null slug
//! silently.

const std = @import("std");
const db = @import("db");
const parse = @import("parse.zig");

// =========================================================================
// Types
// =========================================================================

pub const Op = enum {
    add,
    update,
    remove,

    pub fn toText(self: Op) []const u8 {
        return @tagName(self);
    }
};

/// Proposed change for one child plan.
pub const PlanEntry = struct {
    op: Op,
    title: []const u8,
    /// Set for OpUpdate / OpRemove — the id of the matching plan.
    existing_id: i64 = 0,
    /// Tasks belonging to this child plan, in source order.
    tasks: []const TaskEntry = &.{},
};

/// Proposed change for one task.
pub const TaskEntry = struct {
    op: Op,
    title: []const u8,
    body: []const u8,
    touches: []const []const u8 = &.{},
    /// Slug from the roadmap `[slug:]` annotation. Empty when absent.
    slug: []const u8 = "",
    existing_id: i64 = 0,
    /// Parent child plan title (display only).
    child_plan_title: []const u8 = "",
};

pub const DecisionEntry = struct {
    op: Op,
    title: []const u8,
    body: []const u8,
    existing_id: i64 = 0,
};

/// open → answered transition for an existing question row, produced
/// when the parsed Question has a `Resolution:` and the DB row is still
/// status='open' under the same title (case-insensitive).
pub const QuestionStatusChange = struct {
    question_id: i64,
    question_title: []const u8,
    old_status: []const u8,
    new_status: []const u8,
    answer: []const u8,
};

pub const ScenarioEntry = struct {
    op: Op,
    title: []const u8,
    body: []const u8,
    kind: []const u8,
    acceptance: []const u8,
    verifies: []const parse.TaskRef,
    existing_id: i64 = 0,
};

/// A proposed ADD task slug that already exists on a task NOT part of this
/// ingest. `tasks.slug` carries a global partial-unique index (migration
/// 00011 `ux_tasks_slug`), so this collision would cause `SlugConflict` at
/// apply time. The preview surfaces it before --apply is attempted.
///
/// Note: child-plan slugs use per-parent uniqueness and are not globally
/// unique, so they are not checked here. Decisions/questions/scenarios
/// created by ingest have null slugs and are also not checked.
pub const SlugCollision = struct {
    /// The slug string that collides.
    slug: []const u8,
    /// ID of the existing task that holds the slug.
    existing_task_id: i64,
    /// Plan ID the existing task belongs to (via derives-from link).
    existing_plan_id: i64,
};

/// Full proposed change set for one ingestion pass.
pub const Diff = struct {
    anchor_plan_id: i64,
    anchor_slug: []const u8,
    assoc_slug: []const u8,
    current_status: []const u8,

    child_plans: []const PlanEntry = &.{},
    decisions: []const DecisionEntry = &.{},
    new_questions: []const parse.Question = &.{},
    updated_question_status: []const QuestionStatusChange = &.{},
    orphan_tasks: []const TaskEntry = &.{},
    orphan_plans: []const PlanEntry = &.{},
    scenarios: []const ScenarioEntry = &.{},
    /// Task slugs proposed as ADD that already exist globally on a task
    /// outside this ingest's anchor subtree. Non-empty means --apply would
    /// fail with SlugConflict; the preview surfaces these ahead of time.
    slug_collisions: []const SlugCollision = &.{},

    pub fn totalAdditions(self: Diff) usize {
        var n: usize = 0;
        for (self.child_plans) |cp| {
            if (cp.op == .add) n += 1;
            for (cp.tasks) |t| if (t.op == .add) {
                n += 1;
            };
        }
        for (self.decisions) |d| if (d.op == .add) {
            n += 1;
        };
        n += self.new_questions.len;
        return n;
    }

    pub fn totalUpdates(self: Diff) usize {
        var n: usize = 0;
        for (self.child_plans) |cp| {
            for (cp.tasks) |t| if (t.op == .update) {
                n += 1;
            };
        }
        for (self.decisions) |d| if (d.op == .update) {
            n += 1;
        };
        n += self.updated_question_status.len;
        return n;
    }

    pub fn totalRemovals(self: Diff) usize {
        return self.orphan_tasks.len + self.orphan_plans.len;
    }

    pub fn isEmpty(self: Diff) bool {
        return self.totalAdditions() == 0 and self.totalUpdates() == 0 and self.totalRemovals() == 0;
    }
};

/// Release every allocator-owned string and slice on a Diff.
pub fn deinitDiff(d: Diff, allocator: std.mem.Allocator) void {
    allocator.free(d.anchor_slug);
    allocator.free(d.assoc_slug);
    allocator.free(d.current_status);
    for (d.child_plans) |cp| deinitPlanEntry(cp, allocator);
    allocator.free(d.child_plans);
    for (d.decisions) |de| deinitDecisionEntry(de, allocator);
    allocator.free(d.decisions);
    for (d.new_questions) |q| parse.deinitQuestion(q, allocator);
    allocator.free(d.new_questions);
    for (d.updated_question_status) |s| deinitStatusChange(s, allocator);
    allocator.free(d.updated_question_status);
    for (d.orphan_tasks) |t| deinitTaskEntry(t, allocator);
    allocator.free(d.orphan_tasks);
    for (d.orphan_plans) |op| deinitPlanEntry(op, allocator);
    allocator.free(d.orphan_plans);
    for (d.scenarios) |s| deinitScenarioEntry(s, allocator);
    allocator.free(d.scenarios);
    for (d.slug_collisions) |sc| allocator.free(sc.slug);
    allocator.free(d.slug_collisions);
}

fn deinitSlugCollision(sc: SlugCollision, allocator: std.mem.Allocator) void {
    allocator.free(sc.slug);
}

fn deinitPlanEntry(p: PlanEntry, allocator: std.mem.Allocator) void {
    allocator.free(p.title);
    for (p.tasks) |t| deinitTaskEntry(t, allocator);
    allocator.free(p.tasks);
}

fn deinitTaskEntry(t: TaskEntry, allocator: std.mem.Allocator) void {
    allocator.free(t.title);
    allocator.free(t.body);
    for (t.touches) |s| allocator.free(s);
    allocator.free(t.touches);
    allocator.free(t.slug);
    allocator.free(t.child_plan_title);
}

fn deinitDecisionEntry(d: DecisionEntry, allocator: std.mem.Allocator) void {
    allocator.free(d.title);
    allocator.free(d.body);
}

fn deinitStatusChange(s: QuestionStatusChange, allocator: std.mem.Allocator) void {
    allocator.free(s.question_title);
    allocator.free(s.old_status);
    allocator.free(s.new_status);
    allocator.free(s.answer);
}

fn deinitScenarioEntry(s: ScenarioEntry, allocator: std.mem.Allocator) void {
    allocator.free(s.title);
    allocator.free(s.body);
    allocator.free(s.kind);
    allocator.free(s.acceptance);
    for (s.verifies) |r| parse.deinitTaskRef(r, allocator);
    allocator.free(s.verifies);
}

// =========================================================================
// Error set
// =========================================================================

pub const Error = error{
    NotFound,
    QueryFailed,
} || std.mem.Allocator.Error;

// =========================================================================
// Compute
// =========================================================================

/// Compute the Diff for an anchor plan against parsed spec data.
///
/// `milestones`, `decisions`, `questions`, and `scenarios` are typically
/// produced by parse.zig. The function reads existing DB rows linked to
/// `anchor_plan_id` and emits the additions / updates / removals that
/// `apply.zig` will commit.
pub fn compute(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
    milestones: []const parse.Milestone,
    decisions: []const parse.Decision,
    questions: []const parse.Question,
    scenarios: []const parse.Scenario,
) Error!Diff {
    var result = Diff{
        .anchor_plan_id = anchor_plan_id,
        .anchor_slug = try allocator.dupe(u8, ""),
        .assoc_slug = try allocator.dupe(u8, ""),
        .current_status = try allocator.dupe(u8, ""),
    };
    errdefer deinitDiff(result, allocator);

    // ---- anchor plan --------------------------------------------------
    const anchor = try loadAnchorPlan(d, allocator, anchor_plan_id);
    defer anchor.deinit(allocator);
    allocator.free(result.anchor_slug);
    allocator.free(result.assoc_slug);
    allocator.free(result.current_status);
    result.anchor_slug = try allocator.dupe(u8, anchor.slug);
    result.assoc_slug = try allocator.dupe(u8, anchor.assoc_slug);
    result.current_status = try allocator.dupe(u8, anchor.status);

    // ---- child plans + tasks ------------------------------------------
    var child_plans_out: std.ArrayList(PlanEntry) = .empty;
    errdefer {
        for (child_plans_out.items) |cp| deinitPlanEntry(cp, allocator);
        child_plans_out.deinit(allocator);
    }
    var orphan_tasks_out: std.ArrayList(TaskEntry) = .empty;
    errdefer {
        for (orphan_tasks_out.items) |t| deinitTaskEntry(t, allocator);
        orphan_tasks_out.deinit(allocator);
    }

    const existing_plans = try loadChildPlans(d, allocator, anchor_plan_id);
    defer {
        for (existing_plans) |cp| allocator.free(cp.title);
        allocator.free(existing_plans);
    }
    var used_plan_ids: std.AutoHashMap(i64, void) = .init(allocator);
    defer used_plan_ids.deinit();

    for (milestones) |ms| {
        const ms_title = std.mem.trim(u8, ms.name, " \t");
        const existing = planByTitle(existing_plans, ms_title);

        var pe = PlanEntry{
            .op = if (existing) |_| Op.update else Op.add,
            .title = try allocator.dupe(u8, ms_title),
            .existing_id = if (existing) |e| e.id else 0,
        };
        if (existing) |e| try used_plan_ids.put(e.id, {});

        // Resolve tasks for this child plan.
        var existing_tasks: []dbTask = &.{};
        if (existing) |e| {
            existing_tasks = try loadTasksForPlan(d, allocator, e.id);
        }
        defer {
            for (existing_tasks) |t| {
                allocator.free(t.title);
                allocator.free(t.body);
                allocator.free(t.slug);
            }
            allocator.free(existing_tasks);
        }
        var used_task_ids: std.AutoHashMap(i64, void) = .init(allocator);
        defer used_task_ids.deinit();

        var tasks_out: std.ArrayList(TaskEntry) = .empty;
        errdefer {
            for (tasks_out.items) |t| deinitTaskEntry(t, allocator);
            tasks_out.deinit(allocator);
        }

        for (ms.work_items) |wi| {
            const wi_title = std.mem.trim(u8, wi.title, " \t");
            const task_body = try buildTaskBody(allocator, wi);
            const existing_t = taskByTitle(existing_tasks, wi_title);

            if (existing_t) |et| {
                try used_task_ids.put(et.id, {});
                const body_changed = !std.mem.eql(u8, et.body, task_body);
                const slug_needs_backfill = wi.slug.len != 0 and et.slug.len == 0;
                if (!body_changed and !slug_needs_backfill) {
                    allocator.free(task_body);
                    continue;
                }
                try tasks_out.append(allocator, .{
                    .op = .update,
                    .title = try allocator.dupe(u8, wi_title),
                    .body = task_body,
                    .touches = try dupeStrings(allocator, wi.touches),
                    .slug = try allocator.dupe(u8, wi.slug),
                    .existing_id = et.id,
                    .child_plan_title = try allocator.dupe(u8, ms_title),
                });
            } else {
                try tasks_out.append(allocator, .{
                    .op = .add,
                    .title = try allocator.dupe(u8, wi_title),
                    .body = task_body,
                    .touches = try dupeStrings(allocator, wi.touches),
                    .slug = try allocator.dupe(u8, wi.slug),
                    .existing_id = 0,
                    .child_plan_title = try allocator.dupe(u8, ms_title),
                });
            }
        }

        // Orphan tasks: in DB but not in current roadmap.
        for (existing_tasks) |et| {
            if (used_task_ids.contains(et.id)) continue;
            try orphan_tasks_out.append(allocator, .{
                .op = .remove,
                .title = try allocator.dupe(u8, et.title),
                .body = try allocator.dupe(u8, et.body),
                .touches = &.{},
                .slug = try allocator.dupe(u8, ""),
                .existing_id = et.id,
                .child_plan_title = try allocator.dupe(u8, ms_title),
            });
        }

        pe.tasks = try tasks_out.toOwnedSlice(allocator);
        try child_plans_out.append(allocator, pe);
    }

    // Orphan child plans.
    var orphan_plans_out: std.ArrayList(PlanEntry) = .empty;
    errdefer {
        for (orphan_plans_out.items) |op| deinitPlanEntry(op, allocator);
        orphan_plans_out.deinit(allocator);
    }
    for (existing_plans) |ep| {
        if (used_plan_ids.contains(ep.id)) continue;
        var op_entry = PlanEntry{
            .op = .remove,
            .title = try allocator.dupe(u8, ep.title),
            .existing_id = ep.id,
        };
        const orphan_kid_tasks = try loadTasksForPlan(d, allocator, ep.id);
        defer {
            for (orphan_kid_tasks) |t| {
                allocator.free(t.title);
                allocator.free(t.body);
                allocator.free(t.slug);
            }
            allocator.free(orphan_kid_tasks);
        }
        var kid_out: std.ArrayList(TaskEntry) = .empty;
        errdefer {
            for (kid_out.items) |t| deinitTaskEntry(t, allocator);
            kid_out.deinit(allocator);
        }
        for (orphan_kid_tasks) |kt| {
            try kid_out.append(allocator, .{
                .op = .remove,
                .title = try allocator.dupe(u8, kt.title),
                .body = try allocator.dupe(u8, ""),
                .touches = &.{},
                .slug = try allocator.dupe(u8, ""),
                .existing_id = kt.id,
                .child_plan_title = try allocator.dupe(u8, ep.title),
            });
        }
        op_entry.tasks = try kid_out.toOwnedSlice(allocator);
        try orphan_plans_out.append(allocator, op_entry);
    }

    // ---- decisions -----------------------------------------------------
    const existing_decisions = try loadDecisionsForAnchor(d, allocator, anchor_plan_id);
    defer {
        for (existing_decisions) |de| {
            allocator.free(de.title);
            allocator.free(de.body);
        }
        allocator.free(existing_decisions);
    }
    var decisions_out: std.ArrayList(DecisionEntry) = .empty;
    errdefer {
        for (decisions_out.items) |de| deinitDecisionEntry(de, allocator);
        decisions_out.deinit(allocator);
    }
    for (decisions) |de| {
        const dec_title = std.mem.trim(u8, de.title, " \t");
        if (decisionByTitle(existing_decisions, dec_title)) |existing| {
            if (std.mem.eql(u8, existing.body, de.body)) continue;
            try decisions_out.append(allocator, .{
                .op = .update,
                .title = try allocator.dupe(u8, dec_title),
                .body = try allocator.dupe(u8, de.body),
                .existing_id = existing.id,
            });
        } else {
            try decisions_out.append(allocator, .{
                .op = .add,
                .title = try allocator.dupe(u8, dec_title),
                .body = try allocator.dupe(u8, de.body),
                .existing_id = 0,
            });
        }
    }

    // ---- questions -----------------------------------------------------
    const existing_questions = try loadQuestionsForAnchor(d, allocator, anchor_plan_id);
    defer {
        for (existing_questions) |q| {
            allocator.free(q.title);
            allocator.free(q.status);
        }
        allocator.free(existing_questions);
    }
    var new_questions_out: std.ArrayList(parse.Question) = .empty;
    errdefer {
        for (new_questions_out.items) |q| parse.deinitQuestion(q, allocator);
        new_questions_out.deinit(allocator);
    }
    var status_changes_out: std.ArrayList(QuestionStatusChange) = .empty;
    errdefer {
        for (status_changes_out.items) |s| deinitStatusChange(s, allocator);
        status_changes_out.deinit(allocator);
    }
    for (questions) |q| {
        const q_title = std.mem.trim(u8, q.title, " \t");
        if (questionByTitle(existing_questions, q_title)) |existing| {
            if (q.resolution.len > 0 and std.mem.eql(u8, existing.status, "open")) {
                try status_changes_out.append(allocator, .{
                    .question_id = existing.id,
                    .question_title = try allocator.dupe(u8, existing.title),
                    .old_status = try allocator.dupe(u8, "open"),
                    .new_status = try allocator.dupe(u8, "answered"),
                    .answer = try allocator.dupe(u8, q.resolution),
                });
                // Also emit derived decision unless one exists with that title.
                if (decisionByTitle(existing_decisions, q_title) == null and
                    !decisionAlreadyQueued(decisions_out.items, q_title))
                {
                    try decisions_out.append(allocator, .{
                        .op = .add,
                        .title = try allocator.dupe(u8, q_title),
                        .body = try allocator.dupe(u8, q.resolution),
                    });
                }
            }
            continue;
        }
        // New question: copy strings.
        try new_questions_out.append(allocator, .{
            .title = try allocator.dupe(u8, q.title),
            .body = try allocator.dupe(u8, q.body),
            .resolution = try allocator.dupe(u8, q.resolution),
        });
        if (q.resolution.len > 0 and
            decisionByTitle(existing_decisions, q_title) == null and
            !decisionAlreadyQueued(decisions_out.items, q_title))
        {
            try decisions_out.append(allocator, .{
                .op = .add,
                .title = try allocator.dupe(u8, q_title),
                .body = try allocator.dupe(u8, q.resolution),
            });
        }
    }

    // ---- test-spec scenarios ------------------------------------------
    var scenarios_out: std.ArrayList(ScenarioEntry) = .empty;
    errdefer {
        for (scenarios_out.items) |s| deinitScenarioEntry(s, allocator);
        scenarios_out.deinit(allocator);
    }
    if (scenarios.len > 0) {
        const existing_scenarios = try loadScenariosForAnchor(d, allocator, anchor_plan_id);
        defer {
            for (existing_scenarios) |s| {
                allocator.free(s.title);
                allocator.free(s.body);
            }
            allocator.free(existing_scenarios);
        }
        for (scenarios) |s| {
            const title = std.mem.trim(u8, s.title, " \t");
            if (title.len == 0) continue;
            const body_text = try buildScenarioBody(allocator, s);
            const existing = scenarioByTitle(existing_scenarios, title);
            if (existing) |ex| {
                if (std.mem.eql(u8, ex.body, body_text)) {
                    allocator.free(body_text);
                    continue;
                }
                try scenarios_out.append(allocator, .{
                    .op = .update,
                    .title = try allocator.dupe(u8, title),
                    .body = body_text,
                    .kind = try allocator.dupe(u8, s.kind),
                    .acceptance = try allocator.dupe(u8, s.acceptance),
                    .verifies = try dupeTaskRefs(allocator, s.verifies),
                    .existing_id = ex.id,
                });
            } else {
                try scenarios_out.append(allocator, .{
                    .op = .add,
                    .title = try allocator.dupe(u8, title),
                    .body = body_text,
                    .kind = try allocator.dupe(u8, s.kind),
                    .acceptance = try allocator.dupe(u8, s.acceptance),
                    .verifies = try dupeTaskRefs(allocator, s.verifies),
                    .existing_id = 0,
                });
            }
        }
    }

    // ---- global slug-collision check ------------------------------------
    // For each proposed ADD task that carries a non-null slug, query whether
    // that slug is already held by a task outside the current anchor subtree.
    // A hit means --apply would blow up with SlugConflict; we surface it
    // here so the preview tells the operator before --apply is attempted.
    var slug_collisions_out: std.ArrayList(SlugCollision) = .empty;
    errdefer {
        for (slug_collisions_out.items) |sc| deinitSlugCollision(sc, allocator);
        slug_collisions_out.deinit(allocator);
    }
    // Collect the set of task IDs that ARE part of this ingest (existing
    // tasks that will be updated) so we can exclude them from the collision
    // check. A slug held by a task being updated in this very ingest is NOT
    // a collision — the update path back-fills the slug only when the DB row
    // has slug = NULL, so it won't touch an already-set slug.
    var ingest_task_ids: std.AutoHashMap(i64, void) = .init(allocator);
    defer ingest_task_ids.deinit();
    for (child_plans_out.items) |cp| {
        for (cp.tasks) |t| {
            if (t.op == .update and t.existing_id > 0) {
                try ingest_task_ids.put(t.existing_id, {});
            }
        }
    }
    for (child_plans_out.items) |cp| {
        for (cp.tasks) |t| {
            if (t.op != .add) continue;
            if (t.slug.len == 0) continue;
            if (try findGlobalSlugCollision(d, allocator, t.slug, &ingest_task_ids)) |collision| {
                try slug_collisions_out.append(allocator, collision);
            }
        }
    }

    result.child_plans = try child_plans_out.toOwnedSlice(allocator);
    result.orphan_tasks = try orphan_tasks_out.toOwnedSlice(allocator);
    result.orphan_plans = try orphan_plans_out.toOwnedSlice(allocator);
    result.decisions = try decisions_out.toOwnedSlice(allocator);
    result.new_questions = try new_questions_out.toOwnedSlice(allocator);
    result.updated_question_status = try status_changes_out.toOwnedSlice(allocator);
    result.scenarios = try scenarios_out.toOwnedSlice(allocator);
    result.slug_collisions = try slug_collisions_out.toOwnedSlice(allocator);

    return result;
}

/// IsNonTrivial returns true when a task body warrants an auto-drafted
/// test scenario. The heuristic mirrors Go: ≥2 bullet lines in the body.
pub fn isNonTrivial(body: []const u8) bool {
    var count: usize = 0;
    var it = std.mem.splitScalar(u8, body, '\n');
    while (it.next()) |line| {
        const t = std.mem.trim(u8, line, " \t");
        if (std.mem.startsWith(u8, t, "- ") or std.mem.startsWith(u8, t, "* ")) {
            count += 1;
            if (count >= 2) return true;
        }
    }
    return false;
}

// =========================================================================
// Helpers (body builders, dupes)
// =========================================================================

/// buildTaskBody composes the task body from a roadmap WorkItem.
/// Mirrors Go's buildTaskBody exactly so reconciliation by-byte-equality
/// stays consistent between binaries.
pub fn buildTaskBody(allocator: std.mem.Allocator, wi: parse.WorkItem) std.mem.Allocator.Error![]const u8 {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);
    try buf.appendSlice(allocator, "## Acceptance Criteria\n\n- ");
    try buf.appendSlice(allocator, wi.title);
    try buf.appendSlice(allocator, " is implemented and tested.\n");
    if (wi.touches.len > 0) {
        try buf.appendSlice(allocator, "\n## Repository Scope\n\n");
        for (wi.touches) |slug| {
            try buf.appendSlice(allocator, "- touches: ");
            try buf.appendSlice(allocator, slug);
            try buf.append(allocator, '\n');
        }
    }
    return try allocator.dupe(u8, buf.items);
}

/// buildScenarioBody reconstructs the test_scenarios row body from a
/// parsed Scenario. Preserves `**Verifies:**` / `**Kind:**` / `**Acceptance:**`
/// field lines plus the prose so the row is human-readable.
pub fn buildScenarioBody(allocator: std.mem.Allocator, s: parse.Scenario) std.mem.Allocator.Error![]const u8 {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);
    if (s.verifies.len > 0) {
        try buf.appendSlice(allocator, "**Verifies:** ");
        for (s.verifies, 0..) |r, i| {
            if (i > 0) try buf.appendSlice(allocator, ", ");
            try buf.appendSlice(allocator, r.kind);
            try buf.append(allocator, ':');
            if (r.slug.len > 0) {
                try buf.appendSlice(allocator, r.slug);
            } else {
                const s_id = try std.fmt.allocPrint(allocator, "{d}", .{r.id});
                defer allocator.free(s_id);
                try buf.appendSlice(allocator, s_id);
            }
        }
        try buf.append(allocator, '\n');
    }
    if (s.kind.len > 0) {
        try buf.appendSlice(allocator, "**Kind:** ");
        try buf.appendSlice(allocator, s.kind);
        try buf.append(allocator, '\n');
    }
    if (s.acceptance.len > 0) {
        try buf.appendSlice(allocator, "**Acceptance:** ");
        try buf.appendSlice(allocator, s.acceptance);
        try buf.append(allocator, '\n');
    }
    if (s.body.len > 0) {
        if (buf.items.len > 0) try buf.append(allocator, '\n');
        try buf.appendSlice(allocator, s.body);
    }
    const trimmed = std.mem.trim(u8, buf.items, " \t\n\r");
    return try allocator.dupe(u8, trimmed);
}

fn dupeStrings(allocator: std.mem.Allocator, items: []const []const u8) std.mem.Allocator.Error![]const []const u8 {
    var out = try allocator.alloc([]const u8, items.len);
    var i: usize = 0;
    errdefer {
        for (out[0..i]) |s| allocator.free(s);
        allocator.free(out);
    }
    while (i < items.len) : (i += 1) {
        out[i] = try allocator.dupe(u8, items[i]);
    }
    return out;
}

fn dupeTaskRefs(allocator: std.mem.Allocator, refs: []const parse.TaskRef) std.mem.Allocator.Error![]const parse.TaskRef {
    var out = try allocator.alloc(parse.TaskRef, refs.len);
    var i: usize = 0;
    errdefer {
        for (out[0..i]) |r| parse.deinitTaskRef(r, allocator);
        allocator.free(out);
    }
    while (i < refs.len) : (i += 1) {
        out[i] = .{
            .kind = try allocator.dupe(u8, refs[i].kind),
            .id = refs[i].id,
            .slug = try allocator.dupe(u8, refs[i].slug),
        };
    }
    return out;
}

fn decisionAlreadyQueued(items: []const DecisionEntry, title: []const u8) bool {
    for (items) |de| {
        if (titlesEqualIgnoreCase(de.title, title)) return true;
    }
    return false;
}

/// Case-insensitive, trim-aware title comparison. Avoids heap
/// allocation by streaming both strings in parallel.
fn titlesEqualIgnoreCase(a: []const u8, b: []const u8) bool {
    const ta = std.mem.trim(u8, a, " \t\n\r");
    const tb = std.mem.trim(u8, b, " \t\n\r");
    if (ta.len != tb.len) return false;
    for (ta, tb) |ca, cb| {
        if (std.ascii.toLower(ca) != std.ascii.toLower(cb)) return false;
    }
    return true;
}

// =========================================================================
// DB loaders
// =========================================================================

const AnchorRow = struct {
    slug: []const u8,
    assoc_slug: []const u8,
    status: []const u8,

    fn deinit(self: AnchorRow, allocator: std.mem.Allocator) void {
        allocator.free(self.slug);
        allocator.free(self.assoc_slug);
        allocator.free(self.status);
    }
};

fn loadAnchorPlan(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!AnchorRow {
    var stmt = d.prepare(
        \\select p.slug, coalesce(a.slug, ''), p.status
        \\from plans p
        \\left join associations a on (p.scope_kind = 'association' and a.id = p.scope_id)
        \\where p.id = ? and p.parent_plan_id is null
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => return Error.NotFound,
        .row => {
            return .{
                .slug = try stmt.columnTextAlloc(0, allocator),
                .assoc_slug = try stmt.columnTextAlloc(1, allocator),
                .status = try stmt.columnTextAlloc(2, allocator),
            };
        },
    }
}

const dbChildPlan = struct {
    id: i64,
    title: []const u8,
};

fn loadChildPlans(d: *db.sqlite.Db, allocator: std.mem.Allocator, parent_id: i64) Error![]dbChildPlan {
    var stmt = d.prepare(
        \\select id, title from plans
        \\where parent_plan_id = ?
        \\  and status != 'abandoned'
        \\order by id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = parent_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(dbChildPlan) = .empty;
    errdefer {
        for (out.items) |cp| allocator.free(cp.title);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .title = try stmt.columnTextAlloc(1, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

const dbTask = struct {
    id: i64,
    title: []const u8,
    body: []const u8,
    slug: []const u8,
};

fn loadTasksForPlan(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) Error![]dbTask {
    var stmt = d.prepare(
        \\select t.id, t.title, coalesce(t.body, ''), coalesce(t.slug, '')
        \\from tasks t
        \\join entity_links el on (el.from_kind = 'task' and el.from_id = t.id
        \\                         and el.to_kind = 'plan' and el.to_id = ?
        \\                         and el.relationship = 'derives-from')
        \\where t.status != 'cancelled'
        \\order by t.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(dbTask) = .empty;
    errdefer {
        for (out.items) |t| {
            allocator.free(t.title);
            allocator.free(t.body);
            allocator.free(t.slug);
        }
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .title = try stmt.columnTextAlloc(1, allocator),
                .body = try stmt.columnTextAlloc(2, allocator),
                .slug = try stmt.columnTextAlloc(3, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

const dbDecision = struct {
    id: i64,
    title: []const u8,
    body: []const u8,
};

fn loadDecisionsForAnchor(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_id: i64) Error![]dbDecision {
    var stmt = d.prepare(
        \\select d.id, d.title, coalesce(d.body, '')
        \\from decisions d
        \\join entity_links el on (el.from_kind = 'decision' and el.from_id = d.id
        \\                         and el.to_kind = 'plan' and el.to_id = ?
        \\                         and el.relationship = 'derives-from')
        \\where d.status not in ('superseded', 'withdrawn')
        \\order by d.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(dbDecision) = .empty;
    errdefer {
        for (out.items) |de| {
            allocator.free(de.title);
            allocator.free(de.body);
        }
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .title = try stmt.columnTextAlloc(1, allocator),
                .body = try stmt.columnTextAlloc(2, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

const dbQuestion = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

fn loadQuestionsForAnchor(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_id: i64) Error![]dbQuestion {
    // Find the anchor's scope first.
    var scope_stmt = d.prepare("select scope_kind, scope_id from plans where id = ?") catch return Error.QueryFailed;
    defer scope_stmt.finalize();
    scope_stmt.bind(&.{.{ .int = anchor_id }}) catch return Error.QueryFailed;
    var scope_kind_buf: []const u8 = &.{};
    var scope_id_opt: ?i64 = null;
    switch (scope_stmt.step() catch return Error.QueryFailed) {
        .done => return Error.NotFound,
        .row => {
            scope_kind_buf = try scope_stmt.columnTextAlloc(0, allocator);
            scope_id_opt = scope_stmt.columnIntOpt(1);
        },
    }
    defer allocator.free(scope_kind_buf);

    var out: std.ArrayList(dbQuestion) = .empty;
    errdefer {
        for (out.items) |q| {
            allocator.free(q.title);
            allocator.free(q.status);
        }
        out.deinit(allocator);
    }

    var stmt = if (scope_id_opt == null)
        d.prepare(
            \\select id, title, status from questions
            \\where scope_kind = ? and scope_id is null
            \\order by id
        ) catch return Error.QueryFailed
    else
        d.prepare(
            \\select id, title, status from questions
            \\where scope_kind = ? and scope_id = ?
            \\order by id
        ) catch return Error.QueryFailed;
    defer stmt.finalize();

    if (scope_id_opt) |sid| {
        stmt.bind(&.{ .{ .text = scope_kind_buf }, .{ .int = sid } }) catch return Error.QueryFailed;
    } else {
        stmt.bind(&.{.{ .text = scope_kind_buf }}) catch return Error.QueryFailed;
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .title = try stmt.columnTextAlloc(1, allocator),
                .status = try stmt.columnTextAlloc(2, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

const dbScenario = struct {
    id: i64,
    title: []const u8,
    body: []const u8,
};

fn loadScenariosForAnchor(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_id: i64) Error![]dbScenario {
    var stmt = d.prepare(
        \\select ts.id, ts.title, coalesce(ts.body, '')
        \\from test_scenarios ts
        \\join entity_links el on el.from_kind = 'test_scenario' and el.from_id = ts.id
        \\   and el.to_kind = 'plan' and el.to_id = ?
        \\   and el.relationship = 'derives-from'
        \\where ts.status != 'retired'
        \\order by ts.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(dbScenario) = .empty;
    errdefer {
        for (out.items) |s| {
            allocator.free(s.title);
            allocator.free(s.body);
        }
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .title = try stmt.columnTextAlloc(1, allocator),
                .body = try stmt.columnTextAlloc(2, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// findGlobalSlugCollision checks whether `slug` is already held by a task
/// NOT in `excluded_task_ids`. Returns the collision descriptor when found,
/// null otherwise. Only non-null task slugs are globally unique (index
/// `ux_tasks_slug on tasks(slug) where slug is not null`, migration 00011).
///
/// The `existing_plan_id` in the returned collision is the task's `plan_id`
/// column (the direct plan association stored on the tasks row). Tasks
/// created via `spec ingest --apply` also get a `derives-from` entity_link,
/// but tasks created via `task add --plan` only set the column — so we read
/// `plan_id` from the tasks table directly for broadest coverage.
fn findGlobalSlugCollision(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    slug: []const u8,
    excluded_task_ids: *const std.AutoHashMap(i64, void),
) Error!?SlugCollision {
    // Find any task with this slug that is still live (not cancelled).
    // We exclude cancelled tasks because their slugs are cleared on
    // cancellation by the apply-removals path (nulled out), so they won't
    // hold the index slot.
    var stmt = d.prepare(
        \\select id, coalesce(plan_id, 0)
        \\from tasks
        \\where slug = ? and status != 'cancelled'
        \\order by id limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => return null,
        .row => {
            const task_id = stmt.columnInt(0);
            const plan_id = stmt.columnInt(1);
            // Not a collision if this task is part of the current ingest.
            if (excluded_task_ids.contains(task_id)) return null;
            return .{
                .slug = try allocator.dupe(u8, slug),
                .existing_task_id = task_id,
                .existing_plan_id = plan_id,
            };
        },
    }
}

// =========================================================================
// Lookup helpers
// =========================================================================

fn planByTitle(plans: []const dbChildPlan, title: []const u8) ?dbChildPlan {
    for (plans) |p| if (titlesEqualIgnoreCase(p.title, title)) return p;
    return null;
}

fn taskByTitle(tasks: []const dbTask, title: []const u8) ?dbTask {
    for (tasks) |t| if (titlesEqualIgnoreCase(t.title, title)) return t;
    return null;
}

fn decisionByTitle(decisions: []const dbDecision, title: []const u8) ?dbDecision {
    for (decisions) |de| if (titlesEqualIgnoreCase(de.title, title)) return de;
    return null;
}

fn questionByTitle(questions: []const dbQuestion, title: []const u8) ?dbQuestion {
    for (questions) |q| if (titlesEqualIgnoreCase(q.title, title)) return q;
    return null;
}

fn scenarioByTitle(scenarios: []const dbScenario, title: []const u8) ?dbScenario {
    for (scenarios) |s| if (titlesEqualIgnoreCase(s.title, title)) return s;
    return null;
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

test "isNonTrivial: ≥2 bullets" {
    try testing.expect(!isNonTrivial(""));
    try testing.expect(!isNonTrivial("- only one"));
    try testing.expect(isNonTrivial("- one\n- two"));
    try testing.expect(isNonTrivial("* one\n* two\n* three"));
}

test "compute: empty milestones, empty DB → empty diff" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    // Seed an anchor plan.
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'draft')",
        &.{},
    );
    const anchor_id = try d.intQuery("select id from plans where slug = 'anchor'");

    const diff_result = try compute(&d, a, anchor_id, &.{}, &.{}, &.{}, &.{});
    defer deinitDiff(diff_result, a);
    try testing.expect(diff_result.isEmpty());
    try testing.expectEqualStrings("anchor", diff_result.anchor_slug);
    try testing.expectEqualStrings("draft", diff_result.current_status);
}

test "compute: roadmap with one milestone → adds plan + tasks" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'draft')",
        &.{},
    );
    const anchor_id = try d.intQuery("select id from plans where slug = 'anchor'");

    const wi_a = parse.WorkItem{
        .title = try a.dupe(u8, "Add foo"),
        .touches = &.{},
        .slug = try a.dupe(u8, "add-foo"),
    };
    const wi_b = parse.WorkItem{
        .title = try a.dupe(u8, "Add bar"),
        .touches = &.{},
        .slug = try a.dupe(u8, ""),
    };
    var wis = try a.alloc(parse.WorkItem, 2);
    wis[0] = wi_a;
    wis[1] = wi_b;
    const ms = parse.Milestone{
        .name = try a.dupe(u8, "M1"),
        .intent = try a.dupe(u8, "first"),
        .work_items = wis,
    };
    var milestones = try a.alloc(parse.Milestone, 1);
    milestones[0] = ms;
    defer parse.deinitMilestones(milestones, a);

    const diff_result = try compute(&d, a, anchor_id, milestones, &.{}, &.{}, &.{});
    defer deinitDiff(diff_result, a);

    try testing.expectEqual(@as(usize, 1), diff_result.child_plans.len);
    try testing.expectEqual(Op.add, diff_result.child_plans[0].op);
    try testing.expectEqualStrings("M1", diff_result.child_plans[0].title);
    try testing.expectEqual(@as(usize, 2), diff_result.child_plans[0].tasks.len);
    try testing.expectEqualStrings("Add foo", diff_result.child_plans[0].tasks[0].title);
    try testing.expectEqualStrings("add-foo", diff_result.child_plans[0].tasks[0].slug);
    try testing.expectEqualStrings("Add bar", diff_result.child_plans[0].tasks[1].title);
    try testing.expectEqualStrings("", diff_result.child_plans[0].tasks[1].slug);
}

test "buildTaskBody: matches Go format" {
    const a = testing.allocator;
    const wi = parse.WorkItem{
        .title = "Add foo",
        .touches = &.{ "repo-a", "repo-b" },
        .slug = "",
    };
    const body = try buildTaskBody(a, wi);
    defer a.free(body);
    const expected =
        \\## Acceptance Criteria
        \\
        \\- Add foo is implemented and tested.
        \\
        \\## Repository Scope
        \\
        \\- touches: repo-a
        \\- touches: repo-b
        \\
    ;
    try testing.expectEqualStrings(expected, body);
}

test "buildTaskBody: no touches → no Repository Scope section" {
    const a = testing.allocator;
    const wi = parse.WorkItem{
        .title = "Add bar",
        .touches = &.{},
        .slug = "",
    };
    const body = try buildTaskBody(a, wi);
    defer a.free(body);
    const expected =
        \\## Acceptance Criteria
        \\
        \\- Add bar is implemented and tested.
        \\
    ;
    try testing.expectEqualStrings(expected, body);
}

test "buildScenarioBody: composes Verifies / Kind / Acceptance + prose" {
    const a = testing.allocator;
    var refs = try a.alloc(parse.TaskRef, 2);
    refs[0] = .{ .kind = "task", .id = 0, .slug = "foo" };
    refs[1] = .{ .kind = "task", .id = 5, .slug = "" };
    defer a.free(refs);
    const s = parse.Scenario{
        .title = "x",
        .kind = "integration",
        .acceptance = "exits 0",
        .verifies = refs,
        .body = "prose line",
    };
    const body = try buildScenarioBody(a, s);
    defer a.free(body);
    const expected =
        \\**Verifies:** task:foo, task:5
        \\**Kind:** integration
        \\**Acceptance:** exits 0
        \\
        \\prose line
    ;
    try testing.expectEqualStrings(expected, body);
}
