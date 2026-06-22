//! engine/planning/strategy — parallelizability-rules engine.
//!
//! Single source of truth for the six parallel-eligibility rules locked
//! by decision 370 (accepted). Consumed by `planar plan
//! recommend-strategy` (the operator-facing verb) and, downstream, by
//! the fan-out eligibility gate and the
//! orchestrator skill. The rules are NOT re-derived externally
//! — they live here, once.
//!
//! READ-ONLY: this module only SELECTs. No writes, no migration.
//!
//! ## The six rules
//!
//! Two open (todo) tasks in the same plan are parallel-eligible iff ALL
//! six hold. A task that fails any rule serializes (drops out of the
//! parallel batch); the per-task exclusion reasons name every rule it
//! tripped so the operator can fix the right thing.
//!
//!   1. No `blocked_by` chain to another not-yet-done task in the plan.
//!      The transitive closure of outgoing `blocks` edges (task -[blocks]->
//!      task, where from_id is blocked BY to_id — see task.markBlocked)
//!      intersected with the plan's not-done tasks must be empty.
//!   2. Disjoint touch sets. A task's touch set = repo slugs it touches
//!      (entity_links task -[touches]-> repo) ∪ file paths it touches
//!      (task_touch_paths rows where task_touch_paths.task_id = task,
//!      seeded by `planar task touches add <task> <repo> --path <p>`). An
//!      EMPTY touch set is treated as "touches everything" → NOT eligible
//!      (forces touches-declaration discipline). Two tasks whose touch
//!      sets intersect BOTH drop (drop-both-on-tie) — the result is a
//!      maximal mutually-non-conflicting subset, not an arbitrary winner.
//!   3. No schema migration touched. Any task whose touch set contains a
//!      path matching `migrations/*.sql` serializes against the whole
//!      plan (migration numbering is linear). UNILATERAL drop: such a
//!      task can never be in a parallel batch, so it is dropped on its
//!      own regardless of whether another task also touches a migration.
//!   4. No singleton authoritative file touched. Any task touching a file
//!      in `singleton_files` serializes. UNILATERAL drop, same reasoning
//!      as rule 3 — these are coordination points, the list may grow.
//!   5. No unresolved open question linked to the task (any entity_links
//!      edge between the task and a question whose status = 'open').
//!   6. No unresolved decision dependency (any entity_links edge between
//!      the task and a decision whose status = 'proposed').
//!
//! ## Touch-granularity note
//!
//! Rule 2 is "disjoint task_touches (repo + path)". Each touch is a
//! `{ repo_id, path }` pair (see the `Touch` type) sourced from two
//! structures:
//!   - repo touches: entity_links(from='task', to='repo', rel='touches')
//!     — seeded by `planar task touches add <task> <repo>`. These produce a
//!     whole-repo touch `{ repo_id, path = null }`.
//!   - path touches: task_touch_paths rows joined on task_touch_paths.task_id
//!     — seeded by `planar task touches add <task> <repo> --path <path>`
//!     (migration 00019). These produce `{ repo_id, path }` and carry the
//!     file-path precision rules 2/3/4 need.
//! Because BOTH granularities carry repo identity, a whole-repo touch
//! conflicts with ANY same-repo touch (coarse or path) — two whole-repo
//! claims on R collide, and a whole-repo claim on R collides with a path
//! touch on R, while two distinct path touches on R do not. Cross-repo
//! touches never collide. Eligibility is only as complete as the declared
//! touches: a task that under-declares its touches may be marked eligible
//! against another task it actually conflicts with — declaring touches
//! accurately is operator/orchestrator hygiene, the same discipline any
//! touches-based system requires. An EMPTY touch set is treated as
//! "touches everything" → never eligible, so the failure mode of omission
//! is safe (serialize), not unsafe (false-parallel).

const std = @import("std");
const db = @import("db");

// =========================================================================
// Singleton authoritative files (rule 4)
// =========================================================================

/// Canonical coordination files. A task touching any of these serializes
/// (rule 4). Named constant so it is greppable and maintainable — the
/// tech-spec notes "the list may grow".
pub const singleton_files = [_][]const u8{
    "agents/methodology.md",
    "CLAUDE.md",
    "AGENTS.md",
    "docs/cli-reference.md",
    "docs/architecture.md",
};

/// Return true when `path` is one of the canonical singleton files.
pub fn isSingletonFile(path: []const u8) bool {
    for (singleton_files) |s| {
        if (std.mem.eql(u8, path, s)) return true;
    }
    return false;
}

/// Return true when `path` is a schema migration (`migrations/*.sql`).
/// (rule 3). Matches a `migrations/` prefix and a `.sql` suffix.
pub fn isMigrationPath(path: []const u8) bool {
    const prefix = "migrations/";
    if (!std.mem.startsWith(u8, path, prefix)) return false;
    if (!std.mem.endsWith(u8, path, ".sql")) return false;
    return true;
}

// =========================================================================
// Types
// =========================================================================

pub const Error = error{
    NotFound,
    QueryFailed,
} || std.mem.Allocator.Error;

/// Which signal rule 2's overlap test reads (decision D4, plan 634).
///
/// The baseline (`declared`) is the existing, behavior-preserving default:
/// rule 2 disjointness is computed from a task's DECLARED touches
/// (`task_touch_paths` + the coarse `entity_links` whole-repo edges). The
/// derived variant (`derived`) reads the COMPUTED symbol-level closure
/// (`closures`, the M2 extractor's output) instead: two tasks overlap when
/// their effective closures (role `modify`/`reference`; `transitive` is
/// excluded) share a symbol, even when their declared file touches are
/// disjoint. The gap between the two verdicts is the divergence the
/// extractor exists to surface — see `divergence`.
///
/// ONLY rule 2's touch signal switches. Rules 1/3/4/5/6 are unchanged
/// across sources (rules 3/4 still read declared paths — a migration or
/// singleton touch is a declared-path property, not a closure property).
pub const ClosureSource = enum {
    /// Baseline: declared `task_touch_paths` + whole-repo edges (default).
    declared,
    /// Derived: the computed symbol-level closure in `closures`.
    derived,

    /// Parse the operator-facing flag value; returns null on an unknown
    /// token so the handler can emit a precise error.
    pub fn parse(s: []const u8) ?ClosureSource {
        if (std.mem.eql(u8, s, "declared")) return .declared;
        if (std.mem.eql(u8, s, "derived")) return .derived;
        return null;
    }
};

/// One exclusion reason for a serialized task: the rule number (1..6)
/// and an operator-actionable reason string. Owned by the report's
/// allocator.
pub const Exclusion = struct {
    rule: u8,
    reason: []const u8,
};

/// One task in the recommendation, identified just enough for the
/// operator/orchestrator to act. `slug` / `title` borrow from the
/// task row; both are heap-owned by the report allocator.
pub const TaskRef = struct {
    id: i64,
    slug: ?[]const u8,
    title: []const u8,
    /// Populated only for serialized tasks; empty for eligible ones.
    excluded_by: []const Exclusion,
};

/// The full recommendation for a plan.
pub const Recommendation = struct {
    plan_id: i64,
    parallel_eligible: []const TaskRef,
    serialized: []const TaskRef,
    open_tasks: usize,
    /// fan_out_available == (parallel_eligible.len >= 2).
    fan_out_available: bool,

    pub fn deinit(self: Recommendation, allocator: std.mem.Allocator) void {
        freeRefs(self.parallel_eligible, allocator);
        freeRefs(self.serialized, allocator);
    }
};

fn freeRefs(refs: []const TaskRef, allocator: std.mem.Allocator) void {
    for (refs) |r| {
        allocator.free(r.title);
        if (r.slug) |s| allocator.free(s);
        for (r.excluded_by) |e| allocator.free(e.reason);
        allocator.free(r.excluded_by);
    }
    allocator.free(refs);
}

// =========================================================================
// Internal working representation
// =========================================================================

/// A single declared touch. `repo_id` identifies the repo; `path` is the
/// repo-relative file path for a path-level touch, or `null` for a coarse
/// whole-repo touch (`task touches add <task> <repo>` with no `--path`).
///
/// Two touches CONFLICT iff they target the same repo AND at least one is
/// whole-repo OR they name the same path:
///
///   - same repo, both specific paths, different paths  -> NO conflict
///     (this is what enables intra-repo parallelism);
///   - same repo, both the same path                    -> conflict;
///   - same repo, at least one whole-repo (path = null) -> conflict
///     (a whole-repo claim subsumes any same-repo path-touch);
///   - different repos                                  -> no conflict.
///
/// Repo identity is carried EXPLICITLY (not folded into a flat string
/// token) so a coarse whole-repo claim can collide with a same-repo
/// path-touch — a bare path string carries no repo identity and so could
/// not express that overlap (the iter-2 false-positive this fixes).
const Touch = struct {
    repo_id: i64,
    /// Heap-owned (report allocator) when present.
    path: ?[]const u8,

    /// True when this touch conflicts with `other` per the rule above.
    fn conflicts(self: Touch, other: Touch) bool {
        if (self.repo_id != other.repo_id) return false;
        const sp = self.path orelse return true; // self is whole-repo
        const op = other.path orelse return true; // other is whole-repo
        return std.mem.eql(u8, sp, op);
    }
};

const WorkTask = struct {
    id: i64,
    slug: ?[]const u8,
    title: []const u8,
    /// DECLARED touch set: declared repo + path touches (path heap-owned).
    /// Rules 2 (empty-touches branch), 3, and 4 always read THIS set,
    /// regardless of `ClosureSource` — migration/singleton detection and the
    /// touches-everything guard are declared-path properties.
    touches: []Touch,
    /// DERIVED closure touch set: one `Touch{ repo_id, path = symbol }` per
    /// effective-closure unit (role modify/reference) from `closures`. Empty
    /// under `.declared`. Rule 2's pairwise OVERLAP test reads this set when
    /// the source is `.derived`; otherwise it reads `touches`.
    closure_touches: []Touch,
    /// Accumulated exclusions (heap-owned reason strings).
    exclusions: std.ArrayList(Exclusion),
    /// Set once any rule drops this task.
    dropped: bool,

    /// The touch set rule 2's OVERLAP pass should read for `source`.
    fn overlapTouches(self: WorkTask, source: ClosureSource) []const Touch {
        return switch (source) {
            .declared => self.touches,
            .derived => self.closure_touches,
        };
    }
};

// =========================================================================
// Public entry point
// =========================================================================

/// Compute the parallel-eligibility recommendation for a plan using the
/// baseline DECLARED closure source. Behavior-preserving wrapper over
/// `recommendWith(.declared)`. Caller owns the returned Recommendation and
/// must call `deinit`. Returns `Error.NotFound` when the plan does not exist.
pub fn recommend(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) Error!Recommendation {
    return recommendWith(d, allocator, plan_id, .declared);
}

/// Compute the parallel-eligibility recommendation for a plan, choosing the
/// signal rule 2 reads via `source` (decision D4). `.declared` is the
/// baseline and is byte-for-byte identical to the pre-D4 behavior; `.derived`
/// swaps rule 2's touch signal for the computed symbol-level closure
/// (`closures`). Caller owns the returned Recommendation and must call
/// `deinit`. Returns `Error.NotFound` when the plan does not exist.
pub fn recommendWith(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
    source: ClosureSource,
) Error!Recommendation {
    try ensurePlanExists(d, plan_id);

    // Plan's not-done task ids (for rule 1 closure intersection). A task
    // is "not done" when status not in ('done','cancelled').
    var not_done = std.AutoHashMap(i64, void).init(allocator);
    defer not_done.deinit();
    try loadNotDoneIds(d, allocator, plan_id, &not_done);

    // Open (todo) tasks are the candidate set.
    var tasks = try loadOpenTasks(d, allocator, plan_id, source);
    defer {
        for (tasks.items) |*t| deinitWorkTask(t, allocator);
        tasks.deinit(allocator);
    }

    // ---- Rule 1: blocked_by chain to a not-done task -------------------
    for (tasks.items) |*t| {
        if (try blockedByNotDone(d, allocator, t.id, &not_done)) |blocker| {
            try addExclusion(allocator, t, 1, try std.fmt.allocPrint(
                allocator,
                "excluded by rule 1: blocked_by not-done task {d}",
                .{blocker},
            ));
        }
    }

    // ---- Rule 5: open question linked ---------------------------------
    for (tasks.items) |*t| {
        if (try linkedToUnresolved(d, "question", "open", t.id)) {
            try addExclusion(allocator, t, 5, try allocator.dupe(
                u8,
                "excluded by rule 5: linked to an open question",
            ));
        }
    }

    // ---- Rule 6: proposed decision linked ----------------------------
    for (tasks.items) |*t| {
        if (try linkedToUnresolved(d, "decision", "proposed", t.id)) {
            try addExclusion(allocator, t, 6, try allocator.dupe(
                u8,
                "excluded by rule 6: linked to a proposed decision",
            ));
        }
    }

    // ---- Rule 2 (empty-touches branch): no declared touches ----------
    // Empty touch set == "touches everything" -> not eligible.
    for (tasks.items) |*t| {
        if (t.touches.len == 0) {
            try addExclusion(allocator, t, 2, try allocator.dupe(
                u8,
                "excluded by rule 2: no task_touches declared (treated as touches-everything)",
            ));
        }
    }

    // ---- Rule 3: migration touched (unilateral) ----------------------
    // Reads the RAW declared path (not a qualified token) so the
    // `migrations/*.sql` match still works after repo-qualification.
    for (tasks.items) |*t| {
        for (t.touches) |touch| {
            const path = touch.path orelse continue;
            if (isMigrationPath(path)) {
                try addExclusion(allocator, t, 3, try std.fmt.allocPrint(
                    allocator,
                    "excluded by rule 3: touches {s}",
                    .{path},
                ));
                break;
            }
        }
    }

    // ---- Rule 4: singleton authoritative file (unilateral) -----------
    // Also reads the RAW declared path so singleton detection survives
    // repo-qualification.
    for (tasks.items) |*t| {
        for (t.touches) |touch| {
            const path = touch.path orelse continue;
            if (isSingletonFile(path)) {
                try addExclusion(allocator, t, 4, try std.fmt.allocPrint(
                    allocator,
                    "excluded by rule 4: touches singleton authoritative file {s}",
                    .{path},
                ));
                break;
            }
        }
    }

    // ---- Rule 2 (overlap branch): drop-both-on-tie -------------------
    // Pairwise: if two tasks share any touch, BOTH drop. The pairwise
    // overlap is evaluated ONLY over tasks that survived the unilateral
    // rules (1, 3, 4, 5, 6, and the rule-2 empty-touches branch). A task
    // already dropped by a unilateral rule is removed from the eligible
    // set BEFORE this pass, so it does NOT cascade rule-2 overlap onto a
    // peer whose ONLY conflict was with the already-dropped task. The
    // drop-both-on-tie semantics is preserved within the survivor set.
    //
    // Pre-fix (PR #17 finding 4): the loop ran across ALL open tasks
    // including the unilateral drops; a task B whose only overlap was
    // with a migration-touching peer A (rule 3) got wrongly serialized
    // by rule 2 even though A was already removed from the eligible set.
    // Safe direction (over-serialization, never false-eligible) but real.
    const n = tasks.items.len;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        if (tasks.items[i].dropped) continue;
        var j: usize = i + 1;
        while (j < n) : (j += 1) {
            if (tasks.items[j].dropped) continue;
            const overlap = sharedTouch(
                tasks.items[i].overlapTouches(source),
                tasks.items[j].overlapTouches(source),
            );
            if (overlap) |shared| {
                const desc = try describeTouch(allocator, shared);
                defer allocator.free(desc);
                try addExclusion(allocator, &tasks.items[i], 2, try std.fmt.allocPrint(
                    allocator,
                    "excluded by rule 2: overlaps task {d} on {s}",
                    .{ tasks.items[j].id, desc },
                ));
                try addExclusion(allocator, &tasks.items[j], 2, try std.fmt.allocPrint(
                    allocator,
                    "excluded by rule 2: overlaps task {d} on {s}",
                    .{ tasks.items[i].id, desc },
                ));
            }
        }
    }

    // ---- Partition into eligible / serialized ------------------------
    var eligible: std.ArrayList(TaskRef) = .empty;
    errdefer freeRefsList(&eligible, allocator);
    var serialized: std.ArrayList(TaskRef) = .empty;
    errdefer freeRefsList(&serialized, allocator);

    for (tasks.items) |*t| {
        const title = try allocator.dupe(u8, t.title);
        errdefer allocator.free(title);
        const slug = if (t.slug) |s| try allocator.dupe(u8, s) else null;
        errdefer if (slug) |s| allocator.free(s);

        if (t.dropped) {
            const excl = try t.exclusions.toOwnedSlice(allocator);
            try serialized.append(allocator, .{
                .id = t.id,
                .slug = slug,
                .title = title,
                .excluded_by = excl,
            });
        } else {
            try eligible.append(allocator, .{
                .id = t.id,
                .slug = slug,
                .title = title,
                .excluded_by = &.{},
            });
        }
    }

    const eligible_slice = try eligible.toOwnedSlice(allocator);
    const serialized_slice = try serialized.toOwnedSlice(allocator);

    return .{
        .plan_id = plan_id,
        .parallel_eligible = eligible_slice,
        .serialized = serialized_slice,
        .open_tasks = tasks.items.len,
        .fan_out_available = eligible_slice.len >= 2,
    };
}

fn freeRefsList(list: *std.ArrayList(TaskRef), allocator: std.mem.Allocator) void {
    for (list.items) |r| {
        allocator.free(r.title);
        if (r.slug) |s| allocator.free(s);
        for (r.excluded_by) |e| allocator.free(e.reason);
        allocator.free(r.excluded_by);
    }
    list.deinit(allocator);
}

// =========================================================================
// Divergence: the gap that justifies the extractor (decision D4, M2)
// =========================================================================

/// The derived-vs-declared rule-2 overlap divergence for a plan — the
/// concrete number the closure extractor exists to surface.
///
/// For every unordered pair of the plan's open (todo) tasks we evaluate
/// rule 2's pairwise overlap verdict TWICE: once over each task's DECLARED
/// touch set and once over its DERIVED closure set. The verdict is binary
/// (overlap / disjoint). A pair whose verdict differs between the two
/// sources is a FLIP — the declared baseline and the derived closure
/// DISAGREE about whether those two tasks can run in parallel. `flips` is
/// the count of such pairs; `jaccard` is the Jaccard distance between the
/// two verdict sets (flips / union-of-overlapping-pairs), a normalized
/// [0,1] measure of how far apart the two sources are. `flips > 0` means
/// the derived closure caught a conflict (or freed a pair) the declared
/// touches missed — the gap the extractor is for.
pub const Divergence = struct {
    /// Open tasks considered (the pair universe is `pairs` over these).
    open_tasks: usize,
    /// Unordered task-pairs evaluated (`open_tasks choose 2`).
    pairs: usize,
    /// Pairs that overlap under the DECLARED source.
    declared_overlaps: usize,
    /// Pairs that overlap under the DERIVED source.
    derived_overlaps: usize,
    /// Pairs whose overlap verdict FLIPS between the two sources.
    flips: usize,
    /// Jaccard distance between the declared and derived overlap-pair sets:
    /// `flips / |declared_overlaps ∪ derived_overlaps|`. 0.0 when the two
    /// sources agree on every pair; 1.0 when they share no overlapping pair.
    jaccard: f64,
};

/// Compute the rule-2 overlap divergence between the declared and derived
/// closure sources for a plan (decision D4). Loads the plan's open tasks
/// with BOTH touch sets populated and compares the pairwise overlap verdict
/// under each source. Returns `Error.NotFound` when the plan does not exist.
///
/// This is a pure measurement: it applies ONLY rule 2's pairwise overlap
/// test (no unilateral rules, no drop-both partitioning) so the number
/// isolates exactly the signal D4 swaps — the gap between declared touches
/// and the derived closure. It writes nothing.
pub fn divergence(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) Error!Divergence {
    try ensurePlanExists(d, plan_id);

    // Load every open task with BOTH touch sets so we can compare in one pass.
    var tasks = try loadOpenTasks(d, allocator, plan_id, .declared);
    defer {
        for (tasks.items) |*t| deinitWorkTask(t, allocator);
        tasks.deinit(allocator);
    }
    // loadOpenTasks(.declared) leaves closure_touches empty; fill it now so a
    // single task list carries both sources for the pairwise comparison.
    for (tasks.items) |*t| {
        t.closure_touches = try loadClosureTouches(d, allocator, t.id);
    }

    const n = tasks.items.len;
    var pairs: usize = 0;
    var declared_overlaps: usize = 0;
    var derived_overlaps: usize = 0;
    var flips: usize = 0;
    var union_overlaps: usize = 0;

    var i: usize = 0;
    while (i < n) : (i += 1) {
        var j: usize = i + 1;
        while (j < n) : (j += 1) {
            pairs += 1;
            const decl = sharedTouch(tasks.items[i].touches, tasks.items[j].touches) != null;
            const der = sharedTouch(tasks.items[i].closure_touches, tasks.items[j].closure_touches) != null;
            if (decl) declared_overlaps += 1;
            if (der) derived_overlaps += 1;
            if (decl or der) union_overlaps += 1;
            if (decl != der) flips += 1;
        }
    }

    const jaccard: f64 = if (union_overlaps == 0)
        0.0
    else
        @as(f64, @floatFromInt(flips)) / @as(f64, @floatFromInt(union_overlaps));

    return .{
        .open_tasks = n,
        .pairs = pairs,
        .declared_overlaps = declared_overlaps,
        .derived_overlaps = derived_overlaps,
        .flips = flips,
        .jaccard = jaccard,
    };
}

/// Return the first conflicting touch from touch-set `a` (the touch whose
/// conflict with some touch of `b` made the pair overlap), or null if the
/// two touch-sets are disjoint. The returned touch carries the repo + path
/// context for the exclusion reason. The caller passes whichever touch set
/// (declared or derived-closure) the active `ClosureSource` selects.
fn sharedTouch(a: []const Touch, b: []const Touch) ?Touch {
    for (a) |ta| {
        for (b) |tb| {
            if (ta.conflicts(tb)) return ta;
        }
    }
    return null;
}

/// Render a touch for an operator-facing exclusion reason. A path-touch
/// shows the path; a whole-repo touch shows `repo:<id> (whole repo)`.
/// Caller owns the returned string.
fn describeTouch(allocator: std.mem.Allocator, t: Touch) Error![]const u8 {
    if (t.path) |p| return allocator.dupe(u8, p);
    return std.fmt.allocPrint(allocator, "repo:{d} (whole repo)", .{t.repo_id});
}

/// Add an exclusion to a task and mark it dropped. Takes ownership of
/// `reason` (heap-owned).
fn addExclusion(
    allocator: std.mem.Allocator,
    t: *WorkTask,
    rule: u8,
    reason: []const u8,
) Error!void {
    try t.exclusions.append(allocator, .{ .rule = rule, .reason = reason });
    t.dropped = true;
}

// =========================================================================
// DB helpers (read-only)
// =========================================================================

fn ensurePlanExists(d: *db.sqlite.Db, plan_id: i64) Error!void {
    var stmt = d.prepare("select count(*) from plans where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
    const n = switch (stmt.step() catch return Error.QueryFailed) {
        .done => @as(i64, 0),
        .row => stmt.columnInt(0),
    };
    if (n == 0) return Error.NotFound;
}

fn loadNotDoneIds(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
    out: *std.AutoHashMap(i64, void),
) Error!void {
    _ = allocator;
    var stmt = d.prepare(
        "select id from tasks where plan_id = ? and status not in ('done','cancelled')",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => out.put(stmt.columnInt(0), {}) catch return Error.OutOfMemory,
        }
    }
}

/// Load the plan's open (todo) tasks as WorkTasks with touch sets filled.
fn loadOpenTasks(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
    source: ClosureSource,
) Error!std.ArrayList(WorkTask) {
    var out: std.ArrayList(WorkTask) = .empty;
    errdefer {
        for (out.items) |*t| deinitWorkTask(t, allocator);
        out.deinit(allocator);
    }

    var stmt = d.prepare(
        "select id, slug, title from tasks where plan_id = ? and status = 'todo' order by priority, id",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const slug = stmt.columnTextOpt(1, allocator) catch return Error.QueryFailed;
                errdefer if (slug) |s| allocator.free(s);
                const title = stmt.columnTextAlloc(2, allocator) catch return Error.QueryFailed;
                errdefer allocator.free(title);
                const touches = try loadTouches(d, allocator, id);
                errdefer {
                    for (touches) |t| if (t.path) |p| allocator.free(p);
                    allocator.free(touches);
                }
                // The derived overlap set is only loaded (and only consulted)
                // under `.derived`; under `.declared` it stays empty so the
                // baseline path does ZERO extra DB work and is byte-for-byte
                // identical to the pre-D4 behavior.
                const closure_touches: []Touch = switch (source) {
                    .declared => &.{},
                    .derived => try loadClosureTouches(d, allocator, id),
                };
                try out.append(allocator, .{
                    .id = id,
                    .slug = slug,
                    .title = title,
                    .touches = touches,
                    .closure_touches = closure_touches,
                    .exclusions = .empty,
                    .dropped = false,
                });
            },
        }
    }
    return out;
}

/// Touch set for a task as a list of `Touch{ repo_id, path }`. Two
/// granularities feed the set, and path-level detail REFINES the coarse
/// repo signal — but BOTH carry repo identity now, so a coarse whole-repo
/// touch correctly conflicts with a same-repo path-touch:
///
///   - For a repo a task touches at the file level (task_touch_paths rows,
///     migration 00019), each declared FILE PATH becomes a touch
///     `{ repo_id, path }`. Two tasks editing different files in the SAME
///     repo are therefore disjoint (parallel-eligible), which is the whole
///     point of path-level precision.
///   - For a repo a task touches only via the coarse entity_links edge
///     (`task touches add <task> <repo>` with no --path), the touch is a
///     whole-repo claim `{ repo_id, path = null }` that conflicts with any
///     other task touching that repo (coarse or fine — see
///     `Touch.conflicts`). This preserves the conservative behavior when an
///     operator under-declares.
///
/// A repo that has ANY path-level declaration does NOT also contribute a
/// coarse whole-repo touch — the path detail wins, which is what enables
/// intra-repo parallelism. (Adding a whole-repo touch alongside the paths
/// would re-introduce the conflict the path precision is meant to avoid.)
///
/// Rules 3/4 read the raw repo-relative `path` (`migrations/*.sql`,
/// singleton files) directly off each touch, so paths are stored verbatim.
/// Returns heap-owned path strings (whole-repo touches have null paths).
fn loadTouches(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error![]Touch {
    var out: std.ArrayList(Touch) = .empty;
    errdefer {
        for (out.items) |t| if (t.path) |p| allocator.free(p);
        out.deinit(allocator);
    }

    // Repos with path-level declarations: those repos contribute their
    // FILE PATHS, not a coarse whole-repo touch. Collect the set of
    // refined repo ids so the coarse pass below can skip them.
    var refined = std.AutoHashMap(i64, void).init(allocator);
    defer refined.deinit();

    // Declared file paths via task_touch_paths (migration 00019).
    {
        var stmt = d.prepare(
            "select repo_id, path from task_touch_paths where task_id = ?",
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => break,
                .row => {
                    const repo_id = stmt.columnInt(0);
                    refined.put(repo_id, {}) catch return Error.OutOfMemory;
                    const p = stmt.columnTextAlloc(1, allocator) catch return Error.QueryFailed;
                    try appendUniquePath(allocator, &out, repo_id, p);
                },
            }
        }
    }

    // Coarse whole-repo touches via entity_links — only for repos WITHOUT
    // any path-level declaration (path detail wins over the coarse signal).
    {
        var stmt = d.prepare(
            \\select el.to_id
            \\from entity_links el
            \\where el.from_kind = 'task' and el.from_id = ?
            \\  and el.to_kind = 'repo' and el.relationship = 'touches'
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => break,
                .row => {
                    const repo_id = stmt.columnInt(0);
                    if (refined.contains(repo_id)) continue;
                    try appendUniqueWholeRepo(allocator, &out, repo_id);
                },
            }
        }
    }

    return try out.toOwnedSlice(allocator);
}

/// Append a path-touch `{ repo_id, path }` if an identical one is not
/// already present; free `path` when it is a duplicate.
fn appendUniquePath(
    allocator: std.mem.Allocator,
    out: *std.ArrayList(Touch),
    repo_id: i64,
    path: []const u8,
) Error!void {
    for (out.items) |existing| {
        if (existing.repo_id == repo_id) {
            if (existing.path) |ep| {
                if (std.mem.eql(u8, ep, path)) {
                    allocator.free(path);
                    return;
                }
            }
        }
    }
    try out.append(allocator, .{ .repo_id = repo_id, .path = path });
}

/// Append a whole-repo touch `{ repo_id, path = null }` if one is not
/// already present for that repo.
fn appendUniqueWholeRepo(
    allocator: std.mem.Allocator,
    out: *std.ArrayList(Touch),
    repo_id: i64,
) Error!void {
    for (out.items) |existing| {
        if (existing.repo_id == repo_id and existing.path == null) return;
    }
    try out.append(allocator, .{ .repo_id = repo_id, .path = null });
}

/// Derived overlap set for a task: one `Touch{ repo_id, path = symbol }`
/// per row of the task's EFFECTIVE derived closure (decision D4, M2). The
/// effective closure is the `closures` rows with role `modify` or
/// `reference`; `transitive` is EXCLUDED (matching the schema's "excluded
/// from the effective closure by default" semantics — migration 00026).
///
/// The symbol (qualified name) is carried in the `path` field of `Touch`
/// so `Touch.conflicts` treats two tasks as overlapping when their derived
/// closures share a symbol within the same repo. A symbol-level overlap is
/// the finer signal the extractor produces: two tasks whose DECLARED file
/// touches are disjoint can still share a referenced symbol, and under
/// `.derived` that surfaces as a rule-2 overlap.
///
/// Returns heap-owned symbol strings (every derived touch carries a
/// non-null `path`). Empty when the task has no effective-closure rows.
fn loadClosureTouches(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error![]Touch {
    var out: std.ArrayList(Touch) = .empty;
    errdefer {
        for (out.items) |t| if (t.path) |p| allocator.free(p);
        out.deinit(allocator);
    }

    var stmt = d.prepare(
        \\select repo_id, symbol from closures
        \\where task_id = ? and role in ('modify', 'reference')
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const repo_id = stmt.columnInt(0);
                const symbol = stmt.columnTextAlloc(1, allocator) catch return Error.QueryFailed;
                try appendUniquePath(allocator, &out, repo_id, symbol);
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Rule 1: walk the transitive closure of outgoing `blocks` edges
/// (task -[blocks]-> task, meaning from_id is blocked BY to_id — see
/// task.markBlocked). Return the id of the first reached task that is
/// in the plan's not-done set, or null if none.
fn blockedByNotDone(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
    not_done: *const std.AutoHashMap(i64, void),
) Error!?i64 {
    var seen = std.AutoHashMap(i64, void).init(allocator);
    defer seen.deinit();
    var queue: std.ArrayList(i64) = .empty;
    defer queue.deinit(allocator);

    try queue.append(allocator, task_id);
    seen.put(task_id, {}) catch return Error.OutOfMemory;

    while (queue.pop()) |cur| {
        var stmt = d.prepare(
            \\select to_id from entity_links
            \\where from_kind = 'task' and from_id = ?
            \\  and to_kind = 'task' and relationship = 'blocks'
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = cur }}) catch return Error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => break,
                .row => {
                    const blocker = stmt.columnInt(0);
                    // A blocker that is itself a not-done plan task fails rule 1.
                    if (blocker != task_id and not_done.contains(blocker)) {
                        return blocker;
                    }
                    if (!seen.contains(blocker)) {
                        seen.put(blocker, {}) catch return Error.OutOfMemory;
                        try queue.append(allocator, blocker);
                    }
                },
            }
        }
    }
    return null;
}

/// Rules 5 & 6: return true when the task is linked (either direction)
/// to a `kind` entity whose status equals `status_value`.
fn linkedToUnresolved(
    d: *db.sqlite.Db,
    comptime kind: []const u8,
    status_value: []const u8,
    task_id: i64,
) Error!bool {
    const sql =
        "select 1 from entity_links el join " ++ kind ++ "s e on e.id = " ++
        "(case when el.from_kind = 'task' then el.to_id else el.from_id end) " ++
        "where ((el.from_kind = 'task' and el.from_id = ? and el.to_kind = '" ++ kind ++ "') " ++
        "or (el.to_kind = 'task' and el.to_id = ? and el.from_kind = '" ++ kind ++ "')) " ++
        "and e.status = ? limit 1";
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .int = task_id },
        .{ .int = task_id },
        .{ .text = status_value },
    }) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => false,
        .row => true,
    };
}

fn deinitWorkTask(t: *WorkTask, allocator: std.mem.Allocator) void {
    allocator.free(t.title);
    if (t.slug) |s| allocator.free(s);
    for (t.touches) |touch| if (touch.path) |p| allocator.free(p);
    allocator.free(t.touches);
    for (t.closure_touches) |touch| if (touch.path) |p| allocator.free(p);
    allocator.free(t.closure_touches);
    for (t.exclusions.items) |e| allocator.free(e.reason);
    t.exclusions.deinit(allocator);
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

fn seedPlan(d: *db.sqlite.Db) !i64 {
    return d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')",
        &.{},
    );
}

fn seedTask(d: *db.sqlite.Db, plan_id: i64, title: []const u8) !i64 {
    return d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, ?, 'todo', 100)",
        &.{ .{ .int = plan_id }, .{ .text = title } },
    );
}

fn seedRepo(d: *db.sqlite.Db, slug: []const u8) !i64 {
    return d.execParams(
        "insert into projects (slug, name, root_path) values (?, ?, ?)",
        &.{ .{ .text = slug }, .{ .text = slug }, .{ .text = slug } },
    );
}

fn linkTouchesRepo(d: *db.sqlite.Db, task_id: i64, repo_id: i64) !void {
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'repo', ?, 'touches')",
        &.{ .{ .int = task_id }, .{ .int = repo_id } },
    );
}

fn touchPath(d: *db.sqlite.Db, task_id: i64, repo_id: i64, path: []const u8) !void {
    _ = try d.execParams(
        "insert into task_touch_paths (task_id, repo_id, path) values (?, ?, ?)",
        &.{ .{ .int = task_id }, .{ .int = repo_id }, .{ .text = path } },
    );
}

fn seedClosure(
    d: *db.sqlite.Db,
    task_id: i64,
    repo_id: i64,
    path: []const u8,
    symbol: []const u8,
    role: []const u8,
) !void {
    _ = try d.execParams(
        \\insert into closures
        \\  (task_id, repo_id, path, symbol, role, token_weight, extractor_version)
        \\values (?, ?, ?, ?, ?, 0, 'test')
    , &.{
        .{ .int = task_id },
        .{ .int = repo_id },
        .{ .text = path },
        .{ .text = symbol },
        .{ .text = role },
    });
}

test "isMigrationPath / isSingletonFile classify correctly" {
    try testing.expect(isMigrationPath("migrations/00016_foo.sql"));
    try testing.expect(!isMigrationPath("migrations/README.md"));
    try testing.expect(!isMigrationPath("src/migrations/x.sql"));
    try testing.expect(isSingletonFile("CLAUDE.md"));
    try testing.expect(isSingletonFile("docs/cli-reference.md"));
    try testing.expect(!isSingletonFile("docs/concepts.md"));
}

test "two tasks with disjoint repo touches are both eligible (fan-out available)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const t2 = try seedTask(&d, plan_id, "B");
    const r1 = try seedRepo(&d, "ra");
    const r2 = try seedRepo(&d, "rb");
    try linkTouchesRepo(&d, t1, r1);
    try linkTouchesRepo(&d, t2, r2);

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 2), rec.parallel_eligible.len);
    try testing.expectEqual(@as(usize, 0), rec.serialized.len);
    try testing.expect(rec.fan_out_available);
}

test "empty touches drops via rule 2 (touches-everything)" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const r1 = try seedRepo(&d, "ra");
    try linkTouchesRepo(&d, t1, r1);
    _ = try seedTask(&d, plan_id, "B-no-touches");

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 1), rec.parallel_eligible.len);
    try testing.expectEqual(@as(usize, 1), rec.serialized.len);
    try testing.expectEqual(@as(u8, 2), rec.serialized[0].excluded_by[0].rule);
    try testing.expect(!rec.fan_out_available);
}

test "drop-both-on-tie: overlapping touches drop BOTH tasks" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const t2 = try seedTask(&d, plan_id, "B");
    const r1 = try seedRepo(&d, "shared");
    try linkTouchesRepo(&d, t1, r1);
    try linkTouchesRepo(&d, t2, r1);

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 0), rec.parallel_eligible.len);
    try testing.expectEqual(@as(usize, 2), rec.serialized.len);
    for (rec.serialized) |s| {
        var has_rule2 = false;
        for (s.excluded_by) |e| {
            if (e.rule == 2) has_rule2 = true;
        }
        try testing.expect(has_rule2);
    }
}

test "mixed B.1: same repo, different paths -> both eligible" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const t2 = try seedTask(&d, plan_id, "B");
    const r = try seedRepo(&d, "r");
    try touchPath(&d, t1, r, "src/foo.zig");
    try touchPath(&d, t2, r, "src/bar.zig");

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 2), rec.parallel_eligible.len);
    try testing.expectEqual(@as(usize, 0), rec.serialized.len);
}

test "mixed B.2: same repo, same path -> both serialized" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const t2 = try seedTask(&d, plan_id, "B");
    const r = try seedRepo(&d, "r");
    try touchPath(&d, t1, r, "src/foo.zig");
    try touchPath(&d, t2, r, "src/foo.zig");

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 0), rec.parallel_eligible.len);
    try testing.expectEqual(@as(usize, 2), rec.serialized.len);
    for (rec.serialized) |s| {
        var has_rule2 = false;
        for (s.excluded_by) |e| if (e.rule == 2) {
            has_rule2 = true;
        };
        try testing.expect(has_rule2);
    }
}

test "mixed B.3: whole-repo touch conflicts with same-repo path touch -> both serialized" {
    // The false-positive iter-2 fix targets. Task A touches repo R at a
    // specific path; task B touches repo R coarsely (no --path) = the whole
    // repo, which subsumes A's path. They MUST both serialize, never be
    // marked parallel-eligible.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t_a = try seedTask(&d, plan_id, "A");
    const t_b = try seedTask(&d, plan_id, "B");
    const r = try seedRepo(&d, "r");
    try touchPath(&d, t_a, r, "src/foo.zig"); // A: path-level on R
    try linkTouchesRepo(&d, t_b, r); // B: whole-repo on R (no path)

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 0), rec.parallel_eligible.len);
    try testing.expectEqual(@as(usize, 2), rec.serialized.len);
    for (rec.serialized) |s| {
        var has_rule2 = false;
        for (s.excluded_by) |e| if (e.rule == 2) {
            has_rule2 = true;
        };
        try testing.expect(has_rule2);
    }
}

test "mixed: whole-repo touch on R does NOT conflict with a path touch on a different repo" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t_a = try seedTask(&d, plan_id, "A");
    const t_b = try seedTask(&d, plan_id, "B");
    const r1 = try seedRepo(&d, "r1");
    const r2 = try seedRepo(&d, "r2");
    try touchPath(&d, t_a, r1, "src/foo.zig"); // A: path on r1
    try linkTouchesRepo(&d, t_b, r2); // B: whole-repo on r2

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 2), rec.parallel_eligible.len);
    try testing.expectEqual(@as(usize, 0), rec.serialized.len);
}

test "rule 3: a task touching a migration is dropped unilaterally" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const r1 = try seedRepo(&d, "ra");
    try linkTouchesRepo(&d, t1, r1);
    const t2 = try seedTask(&d, plan_id, "migrator");
    const r2 = try seedRepo(&d, "rmig");
    try touchPath(&d, t2, r2, "migrations/00099_x.sql");

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    // t1 alone is eligible; t2 dropped by rule 3.
    try testing.expectEqual(@as(usize, 1), rec.parallel_eligible.len);
    try testing.expectEqual(t1, rec.parallel_eligible[0].id);
    try testing.expectEqual(@as(usize, 1), rec.serialized.len);
    var has_rule3 = false;
    for (rec.serialized[0].excluded_by) |e| {
        if (e.rule == 3) has_rule3 = true;
    }
    try testing.expect(has_rule3);
}

test "rule 4: a task touching a singleton file is dropped" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const r1 = try seedRepo(&d, "ra");
    try linkTouchesRepo(&d, t1, r1);
    const t2 = try seedTask(&d, plan_id, "doc-toucher");
    const r2 = try seedRepo(&d, "rdoc");
    try touchPath(&d, t2, r2, "CLAUDE.md");

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 1), rec.parallel_eligible.len);
    try testing.expectEqual(@as(usize, 1), rec.serialized.len);
    var has_rule4 = false;
    for (rec.serialized[0].excluded_by) |e| {
        if (e.rule == 4) has_rule4 = true;
    }
    try testing.expect(has_rule4);
}

test "rule 1: a task blocked_by a not-done plan task is dropped" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const r1 = try seedRepo(&d, "ra");
    try linkTouchesRepo(&d, t1, r1);
    const blocker = try seedTask(&d, plan_id, "blocker");
    const r2 = try seedRepo(&d, "rb");
    try linkTouchesRepo(&d, blocker, r2);
    const blocked = try seedTask(&d, plan_id, "blocked");
    const r3 = try seedRepo(&d, "rc");
    try linkTouchesRepo(&d, blocked, r3);
    // blocked -[blocks]-> blocker  (blocked is blocked BY blocker)
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'blocks')",
        &.{ .{ .int = blocked }, .{ .int = blocker } },
    );

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    // blocked is dropped by rule 1; t1 and blocker remain eligible.
    var blocked_serialized = false;
    for (rec.serialized) |s| {
        if (s.id == blocked) {
            blocked_serialized = true;
            var has_rule1 = false;
            for (s.excluded_by) |e| if (e.rule == 1) {
                has_rule1 = true;
            };
            try testing.expect(has_rule1);
        }
    }
    try testing.expect(blocked_serialized);
}

test "rule 5: a task linked to an open question is dropped" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const r1 = try seedRepo(&d, "ra");
    try linkTouchesRepo(&d, t1, r1);
    const q = try d.execParams(
        "insert into questions (scope_kind, title, status) values ('global', 'Q', 'open')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'question', ?, 'addresses')",
        &.{ .{ .int = t1 }, .{ .int = q } },
    );

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 0), rec.parallel_eligible.len);
    var has_rule5 = false;
    for (rec.serialized[0].excluded_by) |e| if (e.rule == 5) {
        has_rule5 = true;
    };
    try testing.expect(has_rule5);
}

test "rule 6: a task linked to a proposed decision is dropped" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const r1 = try seedRepo(&d, "ra");
    try linkTouchesRepo(&d, t1, r1);
    const dec = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global', 'D', 'b', 'proposed')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', ?, 'task', ?, 'blocks')",
        &.{ .{ .int = dec }, .{ .int = t1 } },
    );

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);
    try testing.expectEqual(@as(usize, 0), rec.parallel_eligible.len);
    var has_rule6 = false;
    for (rec.serialized[0].excluded_by) |e| if (e.rule == 6) {
        has_rule6 = true;
    };
    try testing.expect(has_rule6);
}

test "rule 2 does NOT cascade through a rule-3-dropped (migration) peer" {
    // PR #17 cycle B finding 4 regression pin: task A touches a migration
    // (rule 3 → unilateral drop). Task B's ONLY rule-2 overlap is with A
    // (both touch the same repo coarsely). Pre-fix, B was wrongly
    // serialized by rule 2 against the already-dropped A. Under the fix
    // the pairwise rule 2 only runs over survivors, so B is eligible.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t_a = try seedTask(&d, plan_id, "migrator");
    const t_b = try seedTask(&d, plan_id, "B");
    const r = try seedRepo(&d, "r");
    // A: path touch on migration (forces rule 3) + coarse repo edge.
    try linkTouchesRepo(&d, t_a, r);
    try touchPath(&d, t_a, r, "migrations/00099_x.sql");
    // B: coarse whole-repo touch on the SAME repo. Without the fix this
    // overlaps A's whole-repo claim (rule 2 drop-both-on-tie) and B is
    // serialized. With the fix A is rule-3-dropped FIRST, then rule 2
    // only iterates survivors → B is eligible.
    try linkTouchesRepo(&d, t_b, r);

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);

    try testing.expectEqual(@as(usize, 1), rec.parallel_eligible.len);
    try testing.expectEqual(t_b, rec.parallel_eligible[0].id);
    try testing.expectEqual(@as(usize, 1), rec.serialized.len);
    try testing.expectEqual(t_a, rec.serialized[0].id);

    // A carries rule 3 (the unilateral migration drop). It must NOT also
    // carry a rule-2 cascade onto B — the pairwise pass skipped this pair.
    var a_has_rule3 = false;
    var a_has_rule2 = false;
    for (rec.serialized[0].excluded_by) |e| {
        if (e.rule == 3) a_has_rule3 = true;
        if (e.rule == 2) a_has_rule2 = true;
    }
    try testing.expect(a_has_rule3);
    try testing.expect(!a_has_rule2);
}

test "rule 2 does NOT cascade through a rule-4-dropped (singleton) peer" {
    // Same shape as the rule-3 cascade test, but with a singleton-file
    // unilateral drop. Pins the same invariant for rule 4.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t_a = try seedTask(&d, plan_id, "doc-toucher");
    const t_b = try seedTask(&d, plan_id, "B");
    const r = try seedRepo(&d, "r");
    try linkTouchesRepo(&d, t_a, r);
    try touchPath(&d, t_a, r, "CLAUDE.md");
    try linkTouchesRepo(&d, t_b, r);

    const rec = try recommend(&d, a, plan_id);
    defer rec.deinit(a);

    try testing.expectEqual(@as(usize, 1), rec.parallel_eligible.len);
    try testing.expectEqual(t_b, rec.parallel_eligible[0].id);
    var a_has_rule4 = false;
    var a_has_rule2 = false;
    for (rec.serialized[0].excluded_by) |e| {
        if (e.rule == 4) a_has_rule4 = true;
        if (e.rule == 2) a_has_rule2 = true;
    }
    try testing.expect(a_has_rule4);
    try testing.expect(!a_has_rule2);
}

test "recommend returns NotFound for a missing plan" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try testing.expectError(Error.NotFound, recommend(&d, a, 9999));
}

// =========================================================================
// D4: derived closure source + divergence (decision D4, plan 636 M2.6)
// =========================================================================

test "ClosureSource.parse maps tokens; default declared preserved by recommend" {
    try testing.expectEqual(ClosureSource.declared, ClosureSource.parse("declared").?);
    try testing.expectEqual(ClosureSource.derived, ClosureSource.parse("derived").?);
    try testing.expect(ClosureSource.parse("bogus") == null);

    // `recommend` is the byte-for-byte declared wrapper over recommendWith.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const t2 = try seedTask(&d, plan_id, "B");
    const r1 = try seedRepo(&d, "ra");
    const r2 = try seedRepo(&d, "rb");
    try linkTouchesRepo(&d, t1, r1);
    try linkTouchesRepo(&d, t2, r2);

    const rec_default = try recommend(&d, a, plan_id);
    defer rec_default.deinit(a);
    const rec_declared = try recommendWith(&d, a, plan_id, .declared);
    defer rec_declared.deinit(a);
    try testing.expectEqual(rec_default.parallel_eligible.len, rec_declared.parallel_eligible.len);
    try testing.expectEqual(rec_default.serialized.len, rec_declared.serialized.len);
}

test "D4: declared-disjoint but derived-overlap flips rule 2 under .derived" {
    // Two tasks edit DIFFERENT files in the same repo (declared touches
    // disjoint → declared says parallel-eligible). But their DERIVED
    // closures both REFERENCE the same symbol → under .derived rule 2 sees
    // an overlap and serializes both. This is exactly the gap the extractor
    // surfaces.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const t2 = try seedTask(&d, plan_id, "B");
    const r = try seedRepo(&d, "r");

    // Declared: disjoint paths in the same repo.
    try touchPath(&d, t1, r, "src/foo.zig");
    try touchPath(&d, t2, r, "src/bar.zig");

    // Derived closures: each task modifies its own symbol AND both reference
    // the SAME shared symbol — the symbol-level conflict declared touches miss.
    try seedClosure(&d, t1, r, "src/foo.zig", "foo.run", "modify");
    try seedClosure(&d, t1, r, "src/shared.zig", "shared.helper", "reference");
    try seedClosure(&d, t2, r, "src/bar.zig", "bar.run", "modify");
    try seedClosure(&d, t2, r, "src/shared.zig", "shared.helper", "reference");

    // Declared: both eligible (different files).
    const rec_decl = try recommendWith(&d, a, plan_id, .declared);
    defer rec_decl.deinit(a);
    try testing.expectEqual(@as(usize, 2), rec_decl.parallel_eligible.len);
    try testing.expectEqual(@as(usize, 0), rec_decl.serialized.len);

    // Derived: both serialized on the shared symbol (rule 2).
    const rec_der = try recommendWith(&d, a, plan_id, .derived);
    defer rec_der.deinit(a);
    try testing.expectEqual(@as(usize, 0), rec_der.parallel_eligible.len);
    try testing.expectEqual(@as(usize, 2), rec_der.serialized.len);
    for (rec_der.serialized) |s| {
        var has_rule2 = false;
        for (s.excluded_by) |e| if (e.rule == 2) {
            has_rule2 = true;
        };
        try testing.expect(has_rule2);
    }

    // Divergence: exactly one pair flips (declared disjoint → derived overlap).
    const div = try divergence(&d, a, plan_id);
    try testing.expectEqual(@as(usize, 2), div.open_tasks);
    try testing.expectEqual(@as(usize, 1), div.pairs);
    try testing.expectEqual(@as(usize, 0), div.declared_overlaps);
    try testing.expectEqual(@as(usize, 1), div.derived_overlaps);
    try testing.expectEqual(@as(usize, 1), div.flips);
    try testing.expect(div.flips > 0);
    // The single pair flips and is the only overlapping pair → Jaccard 1.0.
    try testing.expectApproxEqAbs(@as(f64, 1.0), div.jaccard, 1e-9);
}

test "D4: transitive closure rows are excluded from the derived overlap set" {
    // The ONLY shared symbol between the two tasks is role 'transitive',
    // which the effective closure excludes. Derived must therefore NOT see
    // an overlap, and divergence must be zero.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const t2 = try seedTask(&d, plan_id, "B");
    const r = try seedRepo(&d, "r");
    try touchPath(&d, t1, r, "src/foo.zig");
    try touchPath(&d, t2, r, "src/bar.zig");

    try seedClosure(&d, t1, r, "src/foo.zig", "foo.run", "modify");
    try seedClosure(&d, t1, r, "src/deep.zig", "deep.thing", "transitive");
    try seedClosure(&d, t2, r, "src/bar.zig", "bar.run", "modify");
    try seedClosure(&d, t2, r, "src/deep.zig", "deep.thing", "transitive");

    const rec_der = try recommendWith(&d, a, plan_id, .derived);
    defer rec_der.deinit(a);
    // Shared symbol is transitive-only → no overlap → both eligible.
    try testing.expectEqual(@as(usize, 2), rec_der.parallel_eligible.len);

    const div = try divergence(&d, a, plan_id);
    try testing.expectEqual(@as(usize, 0), div.flips);
    try testing.expectEqual(@as(f64, 0.0), div.jaccard);
}

test "D4: divergence is zero when declared and derived agree" {
    // Both tasks share the same declared path AND the same derived symbol —
    // both sources say overlap → no flip.
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const plan_id = try seedPlan(&d);
    const t1 = try seedTask(&d, plan_id, "A");
    const t2 = try seedTask(&d, plan_id, "B");
    const r = try seedRepo(&d, "r");
    try touchPath(&d, t1, r, "src/foo.zig");
    try touchPath(&d, t2, r, "src/foo.zig");
    try seedClosure(&d, t1, r, "src/foo.zig", "foo.run", "modify");
    try seedClosure(&d, t2, r, "src/foo.zig", "foo.run", "modify");

    const div = try divergence(&d, a, plan_id);
    try testing.expectEqual(@as(usize, 1), div.declared_overlaps);
    try testing.expectEqual(@as(usize, 1), div.derived_overlaps);
    try testing.expectEqual(@as(usize, 0), div.flips);
    try testing.expectEqual(@as(f64, 0.0), div.jaccard);
}

test "divergence returns NotFound for a missing plan" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try testing.expectError(Error.NotFound, divergence(&d, a, 9999));
}
