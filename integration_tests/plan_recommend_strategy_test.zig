//! integration_tests/plan_recommend_strategy_test.zig —
//!   `planar plan recommend-strategy <plan> --json`
//!
//! Black-box coverage for the parallelizability-rules engine verb
//! (decision 370, M5 task 3186). Walks a realistic operator workflow:
//! seed a plan with several open tasks, declare path-level touches (via
//! `task touches add <task> <repo> --path <p>`, migration 00019), link an
//! open question and a proposed decision, and set up a blocked_by edge,
//! then assert the eligibility partition + per-rule exclusion reasons.
//!
//! Touch-granularity note: rules 2/3/4 are path-shaped (overlap on a file,
//! migrations/*.sql, singleton files), so this test declares per-file
//! touches via the `--path` flag. Path-level detail refines the coarse
//! repo signal: two tasks editing different files in the SAME repo are
//! disjoint (parallel-eligible), which is the whole point of path
//! precision. All tasks here touch one registered repo at distinct paths.
//!
//! Asserts:
//!   - genuinely-disjoint tasks land in parallel_eligible;
//!   - each rule-violating task lands in serialized with the correct
//!     excluded_by rule number;
//!   - drop-both-on-tie: two tasks overlapping on the same path BOTH
//!     serialize, neither stays eligible;
//!   - summary.fan_out_available matches (eligible >= 2).

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64 };
const TaskAddJSON = struct { id: i64 };
const QuestionJSON = struct { id: i64 };
const DecisionJSON = struct { id: i64 };

const Exclusion = struct { rule: i64, reason: []const u8 };

const EligibleTask = struct {
    id: i64,
    slug: ?[]const u8,
    title: []const u8,
};

const SerializedTask = struct {
    id: i64,
    slug: ?[]const u8,
    title: []const u8,
    excluded_by: []Exclusion,
};

const Summary = struct {
    open_tasks: i64,
    eligible: i64,
    serialized: i64,
    fan_out_available: bool,
};

const RecommendJSON = struct {
    plan_id: i64,
    closure_source: []const u8,
    parallel_eligible: []EligibleTask,
    serialized: []SerializedTask,
    summary: Summary,
    recommended_note: []const u8,
};

fn addTask(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    pid: []const u8,
    title: []const u8,
) i64 {
    const t = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", title,
    });
    return t.id;
}

/// Declare a path-level touch on a task via the real CLI surface:
/// `task touches add <task-id> <repo-slug> --path <p>`. This writes a
/// task_touch_paths row (and the coarse repo edge), exactly the data
/// `recommend-strategy` reads for rules 2/3/4.
fn touchPath(
    suite: *harness.Suite,
    repo_slug: []const u8,
    task_id: i64,
    path: []const u8,
) void {
    const gpa = suite.allocator;
    const tid = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch unreachable;
    defer gpa.free(tid);
    const out = suite.mustRun(&.{
        "task", "touches", "add", tid, repo_slug, "--path", path,
    });
    gpa.free(out);
}

/// Register a project at the suite tmp dir and return its slug (read back
/// via `assoc members --json`, the same surface the polyrepo scenario uses).
fn registerRepoSlug(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    _ = suite.registerProject("rs-repo");
    const assoc_slug = "rs-org";
    const cr = suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "org" });
    suite.allocator.free(cr);
    const root = suite.tmpAbsPath();
    const ad = suite.mustRun(&.{ "assoc", "add", assoc_slug, root });
    suite.allocator.free(ad);

    const Member = struct { id: i64, slug: []const u8, name: []const u8 };
    const members = suite.mustRunJSON([]Member, arena, &.{ "assoc", "members", assoc_slug, "--json" });
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "rs-repo")) return m.slug;
    }
    std.debug.panic("registered repo slug not found in assoc members", .{});
}

/// Find a serialized task by id; panic if absent (test contract).
fn serializedById(rec: RecommendJSON, id: i64) SerializedTask {
    for (rec.serialized) |s| {
        if (s.id == id) return s;
    }
    std.debug.panic("task {d} not in serialized set", .{id});
}

fn hasRule(s: SerializedTask, rule: i64) bool {
    for (s.excluded_by) |e| {
        if (e.rule == rule) return true;
    }
    return false;
}

fn isEligible(rec: RecommendJSON, id: i64) bool {
    for (rec.parallel_eligible) |t| {
        if (t.id == id) return true;
    }
    return false;
}

test "recommend-strategy partitions a realistic plan across all six rules" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "rs-mixed", "--json", "RS_MIXED",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Two genuinely-disjoint tasks -> both eligible.
    const t_a = addTask(&suite, arena, pid, "alpha");
    touchPath(&suite, repo, t_a, "src/alpha.zig");
    const t_b = addTask(&suite, arena, pid, "beta");
    touchPath(&suite, repo, t_b, "src/beta.zig");

    // Drop-both-on-tie: two tasks overlap on the same path.
    const t_o1 = addTask(&suite, arena, pid, "overlap-one");
    touchPath(&suite, repo, t_o1, "src/shared.zig");
    const t_o2 = addTask(&suite, arena, pid, "overlap-two");
    touchPath(&suite, repo, t_o2, "src/shared.zig");

    // Rule 3: touches a migration.
    const t_mig = addTask(&suite, arena, pid, "migrator");
    touchPath(&suite, repo, t_mig, "migrations/00099_widget.sql");

    // Rule 4: touches a singleton authoritative file.
    const t_sing = addTask(&suite, arena, pid, "doc-toucher");
    touchPath(&suite, repo, t_sing, "CLAUDE.md");

    // Rule 5: linked to an open question. Also give it a distinct touch
    // so it is not also caught by rule 2 (isolate the rule-5 signal).
    const t_q = addTask(&suite, arena, pid, "questioned");
    touchPath(&suite, repo, t_q, "src/questioned.zig");
    const q = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "add", "--json", "open question",
    });
    const tq_ref = std.fmt.allocPrint(arena, "task:{d}", .{t_q}) catch unreachable;
    const q_ref = std.fmt.allocPrint(arena, "question:{d}", .{q.id}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "links", "add", tq_ref, q_ref, "--relationship", "addresses" }));

    // Rule 6: linked to a proposed decision (with a distinct touch).
    const t_d = addTask(&suite, arena, pid, "decided");
    touchPath(&suite, repo, t_d, "src/decided.zig");
    const dec = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--body", "rationale", "--json", "proposed decision",
    });
    const td_ref = std.fmt.allocPrint(arena, "task:{d}", .{t_d}) catch unreachable;
    const d_ref = std.fmt.allocPrint(arena, "decision:{d}", .{dec.id}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "links", "add", td_ref, d_ref, "--relationship", "addresses" }));

    // Rule 1: a task blocked_by another not-done task in the plan. Give
    // both distinct touches so only rule 1 fires on the blocked task.
    const t_blocker = addTask(&suite, arena, pid, "blocker");
    touchPath(&suite, repo, t_blocker, "src/blocker.zig");
    const t_blocked = addTask(&suite, arena, pid, "blocked");
    touchPath(&suite, repo, t_blocked, "src/blocked.zig");
    // `task block <id> --on <blocker>` records the blocks edge AND flips
    // status to blocked (which removes it from the open/todo candidate
    // set). To exercise rule 1 on a STILL-OPEN task we insert the
    // blocks edge via `links add` and leave status = todo.
    const tblk_ref = std.fmt.allocPrint(arena, "task:{d}", .{t_blocked}) catch unreachable;
    const tbkr_ref = std.fmt.allocPrint(arena, "task:{d}", .{t_blocker}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "links", "add", tblk_ref, tbkr_ref, "--relationship", "depends-on" }));

    const rec = suite.mustRunJSON(RecommendJSON, arena, &.{
        "plan", "recommend-strategy", pid, "--json",
    });

    try std.testing.expectEqual(plan.id, rec.plan_id);

    // Disjoint tasks are eligible. The blocker also has a distinct touch
    // and no other rule violation, so it is eligible too.
    try std.testing.expect(isEligible(rec, t_a));
    try std.testing.expect(isEligible(rec, t_b));
    try std.testing.expect(isEligible(rec, t_blocker));

    // Eligible set is exactly {alpha, beta, blocker}.
    try std.testing.expectEqual(@as(usize, 3), rec.parallel_eligible.len);

    // Drop-both-on-tie: both overlap tasks serialized via rule 2, neither
    // eligible.
    try std.testing.expect(!isEligible(rec, t_o1));
    try std.testing.expect(!isEligible(rec, t_o2));
    try std.testing.expect(hasRule(serializedById(rec, t_o1), 2));
    try std.testing.expect(hasRule(serializedById(rec, t_o2), 2));

    // Per-rule exclusions land with the correct rule number.
    try std.testing.expect(hasRule(serializedById(rec, t_mig), 3));
    try std.testing.expect(hasRule(serializedById(rec, t_sing), 4));
    try std.testing.expect(hasRule(serializedById(rec, t_q), 5));
    try std.testing.expect(hasRule(serializedById(rec, t_d), 6));
    try std.testing.expect(hasRule(serializedById(rec, t_blocked), 1));

    // Summary tallies + fan-out availability.
    try std.testing.expectEqual(@as(i64, 3), rec.summary.eligible);
    try std.testing.expect(rec.summary.fan_out_available); // 3 >= 2
    try std.testing.expectEqual(
        @as(i64, @intCast(rec.parallel_eligible.len + rec.serialized.len)),
        rec.summary.open_tasks,
    );
}

test "recommend-strategy: empty-touches task is excluded by rule 2" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "rs-empty", "--json", "RS_EMPTY",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // One task declares a touch; one declares nothing (touches-everything).
    const t_decl = addTask(&suite, arena, pid, "declared");
    touchPath(&suite, repo, t_decl, "src/declared.zig");
    const t_none = addTask(&suite, arena, pid, "no-touches");

    const rec = suite.mustRunJSON(RecommendJSON, arena, &.{
        "plan", "recommend-strategy", pid, "--json",
    });

    // The no-touches task is serialized by rule 2; only one task is left
    // eligible, so fan-out is NOT available.
    try std.testing.expect(isEligible(rec, t_decl));
    try std.testing.expect(!isEligible(rec, t_none));
    try std.testing.expect(hasRule(serializedById(rec, t_none), 2));
    try std.testing.expectEqual(@as(i64, 1), rec.summary.eligible);
    try std.testing.expect(!rec.summary.fan_out_available);
}

/// Declare a coarse whole-repo touch (no `--path`) via the real CLI
/// surface: `task touches add <task-id> <repo-slug>`. Writes the coarse
/// entity_links edge only — the whole-repo claim that must conflict with
/// any same-repo path touch.
fn touchRepo(
    suite: *harness.Suite,
    repo_slug: []const u8,
    task_id: i64,
) void {
    const gpa = suite.allocator;
    const tid = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch unreachable;
    defer gpa.free(tid);
    const out = suite.mustRun(&.{ "task", "touches", "add", tid, repo_slug });
    gpa.free(out);
}

test "recommend-strategy: a whole-repo touch conflicts with a same-repo path touch (B.3)" {
    // Iter-3 regression guard. Task A touches repo R at a specific path;
    // task B touches repo R coarsely (no --path) = the whole repo, which
    // subsumes A's path. Pre-fix the bare-path token "src/a.zig" never
    // equalled the bare slug, so both were marked parallel-eligible — a
    // false positive that produces a real merge conflict on fan-out. They
    // MUST both serialize.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "rs-b3", "--json", "RS_B3",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_path = addTask(&suite, arena, pid, "path-toucher");
    touchPath(&suite, repo, t_path, "src/foo.zig");
    const t_repo = addTask(&suite, arena, pid, "whole-repo-toucher");
    touchRepo(&suite, repo, t_repo); // NO --path: whole repo

    const rec = suite.mustRunJSON(RecommendJSON, arena, &.{
        "plan", "recommend-strategy", pid, "--json",
    });

    // Neither eligible; both serialized via rule 2.
    try std.testing.expect(!isEligible(rec, t_path));
    try std.testing.expect(!isEligible(rec, t_repo));
    try std.testing.expectEqual(@as(usize, 0), rec.parallel_eligible.len);
    try std.testing.expect(hasRule(serializedById(rec, t_path), 2));
    try std.testing.expect(hasRule(serializedById(rec, t_repo), 2));
    try std.testing.expect(!rec.summary.fan_out_available);
}

test "task touches add --path: roundtrips both surfaces (transactional invariant)" {
    // PR #17 cycle B finding 5 regression pin. `task touches add --path`
    // performs TWO writes (the repo edge in entity_links + the
    // task_touch_paths row); they must commit together. After a single
    // call BOTH the repo and the path must show up in `task touches list`
    // — proving the two-write contract is intact. (A non-transactional
    // implementation where the second write silently no-opped would
    // surface as the path row missing here.)
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "tx-touches", "--json", "TX_TOUCHES",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const t = addTask(&suite, arena, pid, "T");
    touchPath(&suite, repo, t, "src/lib/foo.zig");

    // Roundtrip: list surfaces BOTH the repo and the path.
    const TouchesList = struct {
        task_id: i64,
        repos: []const []const u8,
        paths: []const struct { repo: []const u8, path: []const u8 },
    };
    const tid = std.fmt.allocPrint(arena, "{d}", .{t}) catch unreachable;
    const list = suite.mustRunJSON(TouchesList, arena, &.{
        "task", "touches", "list", tid, "--json",
    });
    try std.testing.expectEqual(t, list.task_id);

    // Repo edge present (the savepoint's first write committed).
    var has_repo = false;
    for (list.repos) |s| if (std.mem.eql(u8, s, repo)) {
        has_repo = true;
    };
    try std.testing.expect(has_repo);

    // Path row present (the savepoint's second write committed under the
    // same transaction → both visible).
    var has_path = false;
    for (list.paths) |row| {
        if (std.mem.eql(u8, row.repo, repo) and std.mem.eql(u8, row.path, "src/lib/foo.zig")) {
            has_path = true;
        }
    }
    try std.testing.expect(has_path);
}

test "recommend-strategy: rule-2 does NOT cascade through a migration-dropped peer (finding 4)" {
    // PR #17 cycle B finding 4 regression pin (CLI-level). Two tasks both
    // touch repo R coarsely. A also touches a migration → rule 3 drops A
    // unilaterally. Pre-fix the rule-2 pairwise overlap would cascade
    // through the dropped A and also serialize B (over-serialization,
    // safe direction but real lost parallelism). With the fix B is
    // eligible; A is serialized with rule 3 ONLY (no rule-2 cascade).
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "rs-f4", "--json", "RS_F4",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // A: coarse repo touch + migration path touch → rule 3 unilateral drop.
    const t_a = addTask(&suite, arena, pid, "migrator");
    touchRepo(&suite, repo, t_a);
    touchPath(&suite, repo, t_a, "migrations/00099_x.sql");
    // B: just a coarse repo touch; its ONLY rule-2 conflict is with A.
    const t_b = addTask(&suite, arena, pid, "B");
    touchRepo(&suite, repo, t_b);

    const rec = suite.mustRunJSON(RecommendJSON, arena, &.{
        "plan", "recommend-strategy", pid, "--json",
    });

    // B is eligible (the cascade through the dropped A no longer fires).
    try std.testing.expect(isEligible(rec, t_b));
    try std.testing.expectEqual(@as(usize, 1), rec.parallel_eligible.len);
    // A is serialized with rule 3, NOT rule 2.
    const a_serialized = serializedById(rec, t_a);
    try std.testing.expect(hasRule(a_serialized, 3));
    try std.testing.expect(!hasRule(a_serialized, 2));
}

test "recommend-strategy: --closure-source flag surface (D4)" {
    // D4 (plan 636 M2.6): the rule-2 overlap signal is selectable via
    // --closure-source. 'declared' is the default and is byte-for-byte the
    // pre-D4 behavior; 'derived' reads the computed closure. With no
    // `closures` rows seeded, the derived overlap set is empty so each task
    // is disjoint under derived — but the flag must be accepted, echoed in
    // the JSON, and produce a coherent partition. An invalid value errors.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "rs-d4", "--json", "RS_D4",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Two tasks overlapping on the SAME declared path → declared serializes
    // both (rule 2). Under derived with no closure rows the overlap set is
    // empty, so the same two tasks become eligible — proving the signal
    // actually switched sources.
    const t1 = addTask(&suite, arena, pid, "one");
    touchPath(&suite, repo, t1, "src/shared.zig");
    const t2 = addTask(&suite, arena, pid, "two");
    touchPath(&suite, repo, t2, "src/shared.zig");

    // Default = declared: both serialized on the shared path.
    const rec_default = suite.mustRunJSON(RecommendJSON, arena, &.{
        "plan", "recommend-strategy", pid, "--json",
    });
    try std.testing.expectEqualStrings("declared", rec_default.closure_source);
    try std.testing.expectEqual(@as(usize, 0), rec_default.parallel_eligible.len);
    try std.testing.expect(hasRule(serializedById(rec_default, t1), 2));
    try std.testing.expect(hasRule(serializedById(rec_default, t2), 2));

    // Explicit declared matches the default exactly.
    const rec_decl = suite.mustRunJSON(RecommendJSON, arena, &.{
        "plan", "recommend-strategy", pid, "--closure-source", "declared", "--json",
    });
    try std.testing.expectEqualStrings("declared", rec_decl.closure_source);
    try std.testing.expectEqual(@as(usize, 0), rec_decl.parallel_eligible.len);

    // Derived: no closure rows → empty derived overlap set → both eligible.
    const rec_der = suite.mustRunJSON(RecommendJSON, arena, &.{
        "plan", "recommend-strategy", pid, "--closure-source", "derived", "--json",
    });
    try std.testing.expectEqualStrings("derived", rec_der.closure_source);
    try std.testing.expect(isEligible(rec_der, t1));
    try std.testing.expect(isEligible(rec_der, t2));
    try std.testing.expectEqual(@as(usize, 2), rec_der.parallel_eligible.len);

    // Invalid value errors with an actionable message.
    const stderr = suite.expectFailure(&.{
        "plan", "recommend-strategy", pid, "--closure-source", "bogus", "--json",
    });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "closure-source") != null);
}

test "recommend-strategy on a missing plan exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const stderr = suite.expectFailure(&.{ "plan", "recommend-strategy", "9999", "--json" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "9999") != null);
}
