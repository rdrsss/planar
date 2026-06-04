//! engine/planning/strategy — parallelizability-rules engine.
//!
//! Single source of truth for the six parallel-eligibility rules locked
//! by decision 370 (accepted). Consumed by `planar plan
//! recommend-strategy` (the operator-facing verb) and, downstream, by
//! planar-execute's fan-out eligibility gate (M5 task 3185) and the
//! orchestrator skill. The rules are NOT re-derived in planar-execute
//! Zig — they live here, once.
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
//! Rule 2 is "disjoint task_touches (repo + path)". The touch dimension is
//! sourced from two structures:
//!   - repo touches: entity_links(from='task', to='repo', rel='touches')
//!     — seeded by `planar task touches add <task> <repo>`.
//!   - path touches: task_touch_paths rows joined on task_touch_paths.task_id
//!     — seeded by `planar task touches add <task> <repo> --path <path>`
//!     (migration 00018). These carry the file-path precision rules 2/3/4
//!     need; the repo edge remains the coarse signal.
//! Repo slugs and file paths never false-collide (a slug like "r1" never
//! equals "migrations/x.sql"). Eligibility is only as complete as the
//! declared touches: a task that under-declares its touches may be marked
//! eligible against another task it actually conflicts with — declaring
//! touches accurately is operator/orchestrator hygiene, the same discipline
//! any touches-based system requires. An EMPTY touch set is treated as
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

const WorkTask = struct {
    id: i64,
    slug: ?[]const u8,
    title: []const u8,
    /// Touch set: repo slugs ∪ declared path touches (heap-owned strings).
    touches: [][]const u8,
    /// Accumulated exclusions (heap-owned reason strings).
    exclusions: std.ArrayList(Exclusion),
    /// Set once any rule drops this task.
    dropped: bool,

    fn hasTouch(self: WorkTask, t: []const u8) bool {
        for (self.touches) |x| {
            if (std.mem.eql(u8, x, t)) return true;
        }
        return false;
    }
};

// =========================================================================
// Public entry point
// =========================================================================

/// Compute the parallel-eligibility recommendation for a plan. Caller
/// owns the returned Recommendation and must call `deinit`. Returns
/// `Error.NotFound` when the plan does not exist.
pub fn recommend(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) Error!Recommendation {
    try ensurePlanExists(d, plan_id);

    // Plan's not-done task ids (for rule 1 closure intersection). A task
    // is "not done" when status not in ('done','cancelled').
    var not_done = std.AutoHashMap(i64, void).init(allocator);
    defer not_done.deinit();
    try loadNotDoneIds(d, allocator, plan_id, &not_done);

    // Open (todo) tasks are the candidate set.
    var tasks = try loadOpenTasks(d, allocator, plan_id);
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
    for (tasks.items) |*t| {
        for (t.touches) |path| {
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
    for (tasks.items) |*t| {
        for (t.touches) |path| {
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
    // Pairwise: if two tasks share any touch, BOTH drop. We compute the
    // overlapping pairs across ALL open tasks (not only currently
    // surviving ones) so that the exclusion reason is complete — but a
    // task already dropped by rules 1/3/4/5/6 still gets the rule-2
    // overlap reason added if it genuinely overlaps another task. That
    // matches "list all rules that apply".
    const n = tasks.items.len;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        var j: usize = i + 1;
        while (j < n) : (j += 1) {
            const overlap = sharedTouch(tasks.items[i], tasks.items[j]);
            if (overlap) |shared| {
                try addExclusion(allocator, &tasks.items[i], 2, try std.fmt.allocPrint(
                    allocator,
                    "excluded by rule 2: overlaps task {d} on {s}",
                    .{ tasks.items[j].id, shared },
                ));
                try addExclusion(allocator, &tasks.items[j], 2, try std.fmt.allocPrint(
                    allocator,
                    "excluded by rule 2: overlaps task {d} on {s}",
                    .{ tasks.items[i].id, shared },
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

/// Return the first touch shared between a and b, or null if disjoint.
fn sharedTouch(a: WorkTask, b: WorkTask) ?[]const u8 {
    for (a.touches) |t| {
        if (b.hasTouch(t)) return t;
    }
    return null;
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
                try out.append(allocator, .{
                    .id = id,
                    .slug = slug,
                    .title = title,
                    .touches = touches,
                    .exclusions = .empty,
                    .dropped = false,
                });
            },
        }
    }
    return out;
}

/// Touch set for a task. Two granularities feed the set, and path-level
/// detail REFINES the coarse repo signal:
///
///   - For a repo a task touches at the file level (task_touch_paths rows,
///     migration 00018), the touch tokens are the declared FILE PATHS for
///     that repo — NOT the repo slug. Two tasks editing different files in
///     the SAME repo are therefore disjoint (parallel-eligible), which is
///     the whole point of path-level precision.
///   - For a repo a task touches only via the coarse entity_links edge
///     (`task touches add <task> <repo>` with no --path), the touch token
///     is the REPO SLUG — a whole-repo claim that conflicts with any other
///     task touching that repo (coarse or fine). This preserves the
///     conservative behavior when an operator under-declares.
///
/// Rules 3/4 match on the raw repo-relative path (`migrations/*.sql`,
/// singleton files), so paths enter the set verbatim. Repo slugs and file
/// paths never false-collide (a slug never equals a `migrations/x.sql`
/// path). Returns heap-owned strings.
fn loadTouches(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error![][]const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (out.items) |s| allocator.free(s);
        out.deinit(allocator);
    }

    // Repos with path-level declarations: those repos contribute their
    // FILE PATHS, not the coarse slug. Collect the set of refined repo ids
    // so the coarse pass below can skip them.
    var refined = std.AutoHashMap(i64, void).init(allocator);
    defer refined.deinit();

    // Declared file paths via task_touch_paths (migration 00018).
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
                    refined.put(stmt.columnInt(0), {}) catch return Error.OutOfMemory;
                    const s = stmt.columnTextAlloc(1, allocator) catch return Error.QueryFailed;
                    try appendUnique(allocator, &out, s);
                },
            }
        }
    }

    // Coarse repo slugs via entity_links — only for repos WITHOUT any
    // path-level declaration (path detail wins over the coarse slug).
    {
        var stmt = d.prepare(
            \\select p.id, p.slug
            \\from entity_links el
            \\join projects p on p.id = el.to_id
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
                    const s = stmt.columnTextAlloc(1, allocator) catch return Error.QueryFailed;
                    if (refined.contains(repo_id)) {
                        allocator.free(s);
                        continue;
                    }
                    try appendUnique(allocator, &out, s);
                },
            }
        }
    }

    return try out.toOwnedSlice(allocator);
}

/// Append `s` to `out` if not already present; free `s` when duplicate.
fn appendUnique(
    allocator: std.mem.Allocator,
    out: *std.ArrayList([]const u8),
    s: []const u8,
) Error!void {
    for (out.items) |existing| {
        if (std.mem.eql(u8, existing, s)) {
            allocator.free(s);
            return;
        }
    }
    try out.append(allocator, s);
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
    for (t.touches) |s| allocator.free(s);
    allocator.free(t.touches);
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

test "recommend returns NotFound for a missing plan" {
    const a = testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try testing.expectError(Error.NotFound, recommend(&d, a, 9999));
}
