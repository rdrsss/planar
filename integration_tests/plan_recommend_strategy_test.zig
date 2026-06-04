//! integration_tests/plan_recommend_strategy_test.zig —
//!   `planar plan recommend-strategy <plan> --json`
//!
//! Black-box coverage for the parallelizability-rules engine verb
//! (decision 370, M5 task 3186). Walks a realistic operator workflow:
//! seed a plan with several open tasks, declare touches (via annotations'
//! anchor_path — the path-level touch surface the engine reads), link an
//! open question and a proposed decision, and set up a blocked_by edge,
//! then assert the eligibility partition + per-rule exclusion reasons.
//!
//! Touch-granularity note: the live schema models path-level touches via
//! annotations(anchor_path, task_id); `planar task touches add` records
//! only repo-level (entity_links task -> repo). Both feed the engine's
//! touch set. This test uses `annotate add --task --anchor-path` because
//! rules 2/3/4 are path-shaped (overlap on a file, migrations/*.sql,
//! singleton files), which repo-slug touches cannot express.
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

fn annotate(
    suite: *harness.Suite,
    task_id: i64,
    path: []const u8,
) void {
    const gpa = suite.allocator;
    const tid = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch unreachable;
    defer gpa.free(tid);
    const out = suite.mustRun(&.{
        "annotate", "add",   "--task", tid,     "--anchor-path", path,
        "--title",  "touch", "--body", "touch",
    });
    gpa.free(out);
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

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "rs-mixed", "--json", "RS_MIXED",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Two genuinely-disjoint tasks -> both eligible.
    const t_a = addTask(&suite, arena, pid, "alpha");
    annotate(&suite, t_a, "src/alpha.zig");
    const t_b = addTask(&suite, arena, pid, "beta");
    annotate(&suite, t_b, "src/beta.zig");

    // Drop-both-on-tie: two tasks overlap on the same path.
    const t_o1 = addTask(&suite, arena, pid, "overlap-one");
    annotate(&suite, t_o1, "src/shared.zig");
    const t_o2 = addTask(&suite, arena, pid, "overlap-two");
    annotate(&suite, t_o2, "src/shared.zig");

    // Rule 3: touches a migration.
    const t_mig = addTask(&suite, arena, pid, "migrator");
    annotate(&suite, t_mig, "migrations/00099_widget.sql");

    // Rule 4: touches a singleton authoritative file.
    const t_sing = addTask(&suite, arena, pid, "doc-toucher");
    annotate(&suite, t_sing, "CLAUDE.md");

    // Rule 5: linked to an open question. Also give it a distinct touch
    // so it is not also caught by rule 2 (isolate the rule-5 signal).
    const t_q = addTask(&suite, arena, pid, "questioned");
    annotate(&suite, t_q, "src/questioned.zig");
    const q = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "add", "--json", "open question",
    });
    const tq_ref = std.fmt.allocPrint(arena, "task:{d}", .{t_q}) catch unreachable;
    const q_ref = std.fmt.allocPrint(arena, "question:{d}", .{q.id}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "links", "add", tq_ref, q_ref, "--relationship", "addresses" }));

    // Rule 6: linked to a proposed decision (with a distinct touch).
    const t_d = addTask(&suite, arena, pid, "decided");
    annotate(&suite, t_d, "src/decided.zig");
    const dec = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision", "add", "--body", "rationale", "--json", "proposed decision",
    });
    const td_ref = std.fmt.allocPrint(arena, "task:{d}", .{t_d}) catch unreachable;
    const d_ref = std.fmt.allocPrint(arena, "decision:{d}", .{dec.id}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "links", "add", td_ref, d_ref, "--relationship", "addresses" }));

    // Rule 1: a task blocked_by another not-done task in the plan. Give
    // both distinct touches so only rule 1 fires on the blocked task.
    const t_blocker = addTask(&suite, arena, pid, "blocker");
    annotate(&suite, t_blocker, "src/blocker.zig");
    const t_blocked = addTask(&suite, arena, pid, "blocked");
    annotate(&suite, t_blocked, "src/blocked.zig");
    // `task block <id> --on <blocker>` records the blocks edge AND flips
    // status to blocked (which removes it from the open/todo candidate
    // set). To exercise rule 1 on a STILL-OPEN task we insert the
    // blocks edge via `links add` and leave status = todo.
    const tblk_ref = std.fmt.allocPrint(arena, "task:{d}", .{t_blocked}) catch unreachable;
    const tbkr_ref = std.fmt.allocPrint(arena, "task:{d}", .{t_blocker}) catch unreachable;
    gpa.free(suite.mustRun(&.{ "links", "add", tblk_ref, tbkr_ref, "--relationship", "blocks" }));

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

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "rs-empty", "--json", "RS_EMPTY",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // One task declares a touch; one declares nothing (touches-everything).
    const t_decl = addTask(&suite, arena, pid, "declared");
    annotate(&suite, t_decl, "src/declared.zig");
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

test "recommend-strategy on a missing plan exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const stderr = suite.expectFailure(&.{ "plan", "recommend-strategy", "9999", "--json" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "9999") != null);
}
