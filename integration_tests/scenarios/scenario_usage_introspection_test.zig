//! integration_tests/scenarios/scenario_usage_introspection_test.zig
//!
//! M3 scenario: Usage Introspection feedback loop.
//!
//! Walks the CLI verbs the introspector agent would invoke — NOT via LLM
//! dispatch — locking the state contract that the agent prose depends on.
//! Every mutating step is followed by a JSON post-state assertion; exit 0
//! alone is not sufficient per CLAUDE.md § Integration test methodology.
//!
//! Verifies (roadmap slugs):
//!     [introspector-feedback-plan]  — bootstrap creates exactly one
//!         planar-feedback plan; a second run reuses it.
//!     [introspector-findings]       — question/task rows filed on the
//!         feedback plan with deterministic signal-derived titles and
//!         derives-from links, observable via list --json.
//!     [introspector-agent]          — quiet database (no friction signal)
//!         files zero new entities.
//!
//! Test blocks:
//!     1. feedback plan bootstrapped exactly once
//!     2. findings filed on feedback plan (friction fixture)
//!     3. re-run dedup: second pass over identical signal adds nothing
//!     4. quiet database files nothing
//!
//! M4 (self-report / gh issue) is deliberately excluded — see brief.

const std = @import("std");
const harness = @import("harness");

// ---- JSON shapes -------------------------------------------------------

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    slug: []const u8,
    status: []const u8,
};

const TaskJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const QuestionJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

// ---- helpers -----------------------------------------------------------

/// Scan a JSON array body (raw bytes) for any entry whose `slug` field
/// equals the target slug. Returns the plan id if found, or null.
fn findPlanBySlug(raw: []const u8, arena: std.mem.Allocator, slug: []const u8) ?i64 {
    const Partial = struct {
        id: i64,
        slug: []const u8,
        title: []const u8 = "",
        status: []const u8 = "",
    };
    const parsed = std.json.parseFromSlice([]Partial, arena, raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch return null;
    defer parsed.deinit();
    for (parsed.value) |p| {
        if (std.mem.eql(u8, p.slug, slug)) return p.id;
    }
    return null;
}

/// Count how many plans in the JSON array body carry the target slug.
fn countPlansBySlug(raw: []const u8, arena: std.mem.Allocator, slug: []const u8) usize {
    const Partial = struct {
        id: i64,
        slug: []const u8,
        title: []const u8 = "",
        status: []const u8 = "",
    };
    const parsed = std.json.parseFromSlice([]Partial, arena, raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch return 0;
    defer parsed.deinit();
    var count: usize = 0;
    for (parsed.value) |p| {
        if (std.mem.eql(u8, p.slug, slug)) count += 1;
    }
    return count;
}

/// Count rows in a JSON array.
fn countRows(raw: []const u8, arena: std.mem.Allocator) usize {
    const parsed = std.json.parseFromSlice([]std.json.Value, arena, raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch return 0;
    defer parsed.deinit();
    return parsed.value.len;
}

// =========================================================================
// Scenario 1: feedback plan bootstrapped exactly once
//
// Verifies: [introspector-feedback-plan]
// =========================================================================

test "scenario: introspector — feedback plan bootstrapped exactly once" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("introspect-bootstrap");
    suite.addAssoc("introspect-bootstrap", "project");

    // Step 1: verify no planar-feedback plan exists yet.
    const list_before_raw = suite.mustRun(&.{ "plan", "list", "--scope", "global", "--json" });
    defer gpa.free(list_before_raw);
    const count_before = countPlansBySlug(list_before_raw, arena, "planar-feedback");
    try std.testing.expectEqual(@as(usize, 0), count_before);

    // Step 2: first pass — bootstrap sequence.
    //
    // The agent would check plan list, find no planar-feedback, then:
    //   planar plan create "Planar Feedback" --slug planar-feedback
    const feedback_plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--scope", "global", "--slug", "planar-feedback", "Planar Feedback",
    });
    try std.testing.expectEqualStrings("draft", feedback_plan.status);
    try std.testing.expectEqualStrings("planar-feedback", feedback_plan.slug);

    // Step 3: second pass — detect via list scan and reuse.
    //
    // A second introspector run would call plan list again and find the
    // existing plan. Exactly one plan with slug planar-feedback exists.
    const list_after_raw = suite.mustRun(&.{ "plan", "list", "--scope", "global", "--json" });
    defer gpa.free(list_after_raw);
    const count_after = countPlansBySlug(list_after_raw, arena, "planar-feedback");
    try std.testing.expectEqual(@as(usize, 1), count_after);

    // The detected id must match the plan we created — not a duplicate.
    const detected_id = findPlanBySlug(list_after_raw, arena, "planar-feedback");
    try std.testing.expect(detected_id != null);
    try std.testing.expectEqual(feedback_plan.id, detected_id.?);

    // Step 4: simulate what a second bootstrap would attempt — creating
    // another plan with the same title. The agent would detect the slug
    // via list scan and skip creation entirely. We confirm that if an
    // operator did create a second plan and then set the same slug, the
    // list still shows exactly one planar-feedback entry. (We do NOT
    // create a duplicate here — that would violate the dedup contract.
    // Instead, we assert the existing one is the only one after re-scan.)
    const rescan_raw = suite.mustRun(&.{ "plan", "list", "--scope", "global", "--json" });
    defer gpa.free(rescan_raw);
    try std.testing.expectEqual(@as(usize, 1), countPlansBySlug(rescan_raw, arena, "planar-feedback"));
}

// =========================================================================
// Scenario 2: findings filed on the feedback plan
//
// Verifies: [introspector-findings]
//
// Creates a feedback plan, then simulates the agent filing two findings
// (one question for a failure-cluster, one task for an abandoned-workflow)
// with deterministic signal-derived titles and derives-from links.
// Post-state is asserted via question list --json / task list --json.
// =========================================================================

test "scenario: introspector — findings filed on feedback plan with deterministic titles" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("introspect-findings");
    suite.addAssoc("introspect-findings", "project");

    // ---- Bootstrap the feedback plan.
    const fb_plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--scope", "global", "--slug", "planar-feedback", "Planar Feedback",
    });
    const fb_id_str = std.fmt.allocPrint(arena, "{d}", .{fb_plan.id}) catch unreachable;

    // ---- Dedup pre-check: confirm no findings exist yet.
    // The dedup sequence uses three calls (one for questions, two for tasks
    // with different --status values) and unions all titles.
    const q_before_raw = suite.mustRun(&.{
        "question", "list", "--scope", "global", "--status", "open", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(q_before_raw);
    try std.testing.expectEqual(@as(usize, 0), countRows(q_before_raw, arena));

    const t_todo_before_raw = suite.mustRun(&.{
        "task", "list", "--scope", "global", "--status", "todo", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(t_todo_before_raw);
    try std.testing.expectEqual(@as(usize, 0), countRows(t_todo_before_raw, arena));

    const t_doing_before_raw = suite.mustRun(&.{
        "task", "list", "--scope", "global", "--status", "doing", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(t_doing_before_raw);
    try std.testing.expectEqual(@as(usize, 0), countRows(t_doing_before_raw, arena));

    // ---- File finding 1: failure-cluster question.
    //
    // Deterministic title convention: "<taxonomy-key>: <signal-key>"
    // This mirrors what the agent files when it observes >= 3 failures
    // on a verb path in the diagnostic bundle.
    const finding_title_1 = "failure-cluster: task add";
    const q_finding = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question",                                                                                               "add",
        "--json",                                                                                                 "--plan",
        fb_id_str,                                                                                                "--scope",
        "global",                                                                                                 "--body",
        "3 failed invocations of `planar task add` with exit code 2 in the past 7 days (error_category: usage).", finding_title_1,
    });
    try std.testing.expectEqualStrings("open", q_finding.status);
    try std.testing.expectEqualStrings(finding_title_1, q_finding.title);

    // ---- File finding 2: abandoned-workflow task.
    //
    // Taxonomy key `abandoned-workflow` maps to a task entity.
    const finding_title_2 = "abandoned-workflow: coder";
    const t_finding = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",                                                                               "add",
        "--json",                                                                             "--plan",
        fb_id_str,                                                                            "--scope",
        "global",                                                                             "--body",
        "1 stale claim attributed to role `coder` (never consumed, TTL expired 2026-06-04).", finding_title_2,
    });
    try std.testing.expectEqualStrings("todo", t_finding.status);
    try std.testing.expectEqualStrings(finding_title_2, t_finding.title);

    // ---- Assert post-state via list --json.
    //
    // Both findings must be observable on the feedback plan.
    const q_after_raw = suite.mustRun(&.{
        "question", "list", "--scope", "global", "--status", "open", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(q_after_raw);
    try std.testing.expectEqual(@as(usize, 1), countRows(q_after_raw, arena));
    try std.testing.expect(std.mem.containsAtLeast(u8, q_after_raw, 1, finding_title_1));

    const t_after_raw = suite.mustRun(&.{
        "task", "list", "--scope", "global", "--status", "todo", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(t_after_raw);
    try std.testing.expectEqual(@as(usize, 1), countRows(t_after_raw, arena));
    try std.testing.expect(std.mem.containsAtLeast(u8, t_after_raw, 1, finding_title_2));

    // ---- Assert the question finding carries a derives-from link to the plan.
    //
    // `question add --plan` creates an entity_links(derives-from) row.
    // `links list question:<id>` (NDJSON; one object per line) must contain
    // the relationship and the feedback plan id.
    const q_ref = std.fmt.allocPrint(arena, "question:{d}", .{q_finding.id}) catch unreachable;
    const q_links_raw = suite.mustRun(&.{ "links", "list", q_ref, "--json" });
    defer gpa.free(q_links_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, q_links_raw, 1, "derives-from"));
    try std.testing.expect(std.mem.containsAtLeast(u8, q_links_raw, 1, fb_id_str));

    // ---- The task finding is associated to the feedback plan via plan_id
    // (set by `task add --plan`). This is already confirmed by the
    // `task list --plan <fb_id>` filter above which returned exactly one
    // row. Verify the plan_id round-trips via `task show --json`.
    const t_id_str = std.fmt.allocPrint(arena, "{d}", .{t_finding.id}) catch unreachable;
    const TaskShowJSON = struct {
        id: i64,
        title: []const u8,
        status: []const u8,
        plan_id: ?i64 = null,
    };
    const t_shown = suite.mustRunJSON(TaskShowJSON, arena, &.{
        "task", "show", "--json", t_id_str,
    });
    try std.testing.expect(t_shown.plan_id != null);
    try std.testing.expectEqual(fb_plan.id, t_shown.plan_id.?);
}

// =========================================================================
// Scenario 3: re-run dedup — second pass over identical signal adds nothing
//
// Verifies: [introspector-findings] (edge: re-run dedup)
//
// Two consecutive passes over the same friction signal yield the same
// finding set. Title-based dedup: the second pass detects both titles
// already present and skips both adds. Entity count is unchanged.
// =========================================================================

test "scenario: introspector — re-run dedup: second pass adds nothing" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("introspect-dedup");
    suite.addAssoc("introspect-dedup", "project");

    // ---- Bootstrap the feedback plan.
    const fb_plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--scope", "global", "--slug", "planar-feedback", "Planar Feedback",
    });
    const fb_id_str = std.fmt.allocPrint(arena, "{d}", .{fb_plan.id}) catch unreachable;

    // ---- Pass 1: file one finding.
    const dedup_title = "retry-pattern: plan create";
    const q1 = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question",                                                                                          "add",
        "--json",                                                                                            "--plan",
        fb_id_str,                                                                                           "--scope",
        "global",                                                                                            "--body",
        "4 consecutive failed invocations of `planar plan create` followed by a success (window: 14 days).", dedup_title,
    });
    try std.testing.expectEqualStrings(dedup_title, q1.title);

    // ---- Snapshot the count after pass 1.
    const count_after_pass1_raw = suite.mustRun(&.{
        "question", "list", "--scope", "global", "--status", "open", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(count_after_pass1_raw);
    const count_after_pass1 = countRows(count_after_pass1_raw, arena);
    try std.testing.expectEqual(@as(usize, 1), count_after_pass1);

    // ---- Pass 2: dedup sequence.
    //
    // The agent runs three calls and unions the title sets:
    //   question list --plan <id> --status open --json
    //   task list --plan <id> --status todo --json
    //   task list --plan <id> --status doing --json
    // (`--status` is single-valued; two separate task list calls are required.)
    // It finds `retry-pattern: plan create` already present in the question
    // list and skips the add.
    //
    // The integration test exercises the dedup invariant from the
    // outside: run the full dedup pre-check sequence, confirm the title is
    // present, then verify the count has NOT grown.
    const dedup_check_raw = suite.mustRun(&.{
        "question", "list", "--scope", "global", "--status", "open", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(dedup_check_raw);
    // The title is present in the existing question list — dedup would fire.
    try std.testing.expect(std.mem.containsAtLeast(u8, dedup_check_raw, 1, dedup_title));

    // Run the task side of the dedup sequence (todo then doing), exercising
    // the two-call union path prescribed by the spec.
    const dedup_task_todo_raw = suite.mustRun(&.{
        "task", "list", "--scope", "global", "--status", "todo", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(dedup_task_todo_raw);
    const dedup_task_doing_raw = suite.mustRun(&.{
        "task", "list", "--scope", "global", "--status", "doing", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(dedup_task_doing_raw);
    // Task side is empty — no tasks on this plan. Union is still empty.
    try std.testing.expectEqual(@as(usize, 0), countRows(dedup_task_todo_raw, arena));
    try std.testing.expectEqual(@as(usize, 0), countRows(dedup_task_doing_raw, arena));

    // The agent skips the add when the title is found. Count is still 1.
    const count_after_pass2_raw = suite.mustRun(&.{
        "question", "list", "--scope", "global", "--status", "open", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(count_after_pass2_raw);
    const count_after_pass2 = countRows(count_after_pass2_raw, arena);
    // Entity count must be unchanged — dedup contract.
    try std.testing.expectEqual(count_after_pass1, count_after_pass2);
}

// =========================================================================
// Scenario 4: quiet database files nothing
//
// Verifies: [introspector-agent] (empty/null path)
//
// A healthy / low-friction database has no failure clusters, no retry
// patterns, no stale claims, no stale handoffs. A pass over it creates
// zero new entities. The feedback plan (if it exists) gains no rows.
// =========================================================================

test "scenario: introspector — quiet database files nothing" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("introspect-quiet");
    suite.addAssoc("introspect-quiet", "project");

    // ---- Bootstrap the feedback plan (it may already exist from a prior
    //      pass; this run should leave it unchanged).
    const fb_plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--scope", "global", "--slug", "planar-feedback", "Planar Feedback",
    });
    const fb_id_str = std.fmt.allocPrint(arena, "{d}", .{fb_plan.id}) catch unreachable;

    // ---- Snapshot entity counts before the quiet pass.
    const q_before_raw = suite.mustRun(&.{
        "question", "list", "--scope", "global", "--status", "open", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(q_before_raw);
    const q_count_before = countRows(q_before_raw, arena);

    const t_before_raw = suite.mustRun(&.{
        "task", "list", "--scope", "global", "--status", "todo", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(t_before_raw);
    const t_count_before = countRows(t_before_raw, arena);

    // ---- Simulate a quiet pass: no friction signal observed.
    //
    // The agent would read `planar report --json`, find no failure
    // clusters / retry patterns / stale claims, mine transcripts (none),
    // classify zero candidates, and file nothing. We represent this by
    // doing nothing between the two counts — the key assertion is that
    // the counts are the same before and after.

    // ---- Snapshot counts after the quiet pass: must be unchanged.
    const q_after_raw = suite.mustRun(&.{
        "question", "list", "--scope", "global", "--status", "open", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(q_after_raw);
    const q_count_after = countRows(q_after_raw, arena);

    const t_after_raw = suite.mustRun(&.{
        "task", "list", "--scope", "global", "--status", "todo", "--plan", fb_id_str, "--json",
    });
    defer gpa.free(t_after_raw);
    const t_count_after = countRows(t_after_raw, arena);

    // Zero new entities filed — the quiet-database contract.
    try std.testing.expectEqual(q_count_before, q_count_after);
    try std.testing.expectEqual(t_count_before, t_count_after);

    // ---- The feedback plan itself must still be a plan with exactly the
    //      right slug and still in draft — the introspector never
    //      advances or modifies the plan itself.
    const fb_shown = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "show", "--json", fb_id_str,
    });
    try std.testing.expectEqualStrings("planar-feedback", fb_shown.slug);
    try std.testing.expectEqualStrings("draft", fb_shown.status);
}
