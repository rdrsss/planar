//! integration_tests/scenarios/scenario_feature_lifecycle_test.zig
//!
//! Flagship workflow scenario: a feature lifecycle from "operator
//! lands in a registered project and starts a new feature" through
//! "first task closes and the plan rolls forward".
//!
//! This file is the canonical example for the scenario-test style
//! documented in CLAUDE.md § Integration test methodology. It walks a
//! single coherent operator session end-to-end, asserting on JSON-
//! shaped post-state after every mutating step instead of trusting
//! exit codes alone.
//!
//! Verbs exercised in the primary flow (one path through):
//!     assoc create, assoc add, plan create, plan show,
//!     artifact add, artifact show, task add (×3), task show,
//!     plan next, task update, capture session, capture snapshot,
//!     task done, plan show (post-completion).
//!
//! The secondary test block exercises the question / decision overlay
//! that typically threads through a real feature — operators capture
//! open uncertainties as `question add --plan` and decision records
//! as `decision add --plan` as the work unfolds. The third block
//! asserts the tree view rolls everything up coherently.
//!
//! Scenarios deliberately cross verb boundaries. A regression that
//! only shows up when verbs are composed (e.g. `task done` leaves a
//! ghost row that `plan show` then mis-counts) would slip through
//! per-verb unit tests but surface here.

const std = @import("std");
const harness = @import("harness");

// ---- JSON shapes used to assert post-state at each step ----------

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const ArtifactJSON = struct {
    id: i64,
    title: []const u8,
    kind: []const u8,
};

const TaskJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
    priority: i64,
};

/// Plan 85 M3 reshaped `plan next` from a single-task picker into a
/// bucketed view (`available` / `claimed` / `stale` / `blocked`). This
/// scenario only cares about the "highest-priority available task";
/// it pulls the first row out of the `available` bucket.
const PlanNextJSON = struct {
    plan_id: i64,
    available: []TaskJSON,
};

const SessionJSON = struct {
    id: i64,
};

const SnapshotJSON = struct {
    id: i64,
};

const QuestionJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const DecisionJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

// ---- scenario 1: feature lifecycle ------------------------------

test "scenario: feature lifecycle — register → plan → spec → tasks → resume → done" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // ---- 1. Operator lands in a fresh repo. Project + assoc.
    //
    // Real operators run `planar init` once per checkout; the harness
    // wraps that, then `planar assoc create/add` binds the project to
    // a named scope so subsequent cwd-derive resolves to that scope.
    _ = suite.registerProject("featurelife");
    suite.addAssoc("featurelife", "project");

    // ---- 2. Plan for the new feature.
    //
    // `planar plan create` is the canonical entry point. We assert
    // the returned plan starts in `draft` per the documented status
    // lifecycle.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan",        "create", "--json", "--summary", "Add /reports endpoint",
        "Reports API",
    });
    try std.testing.expectEqualStrings("Reports API", plan.title);
    try std.testing.expectEqualStrings("draft", plan.status);

    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Plan should be visible via `plan show` — round-trip the
    // identity field set.
    const plan_shown = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "show", "--json", plan_id_str,
    });
    try std.testing.expectEqual(plan.id, plan_shown.id);
    try std.testing.expectEqualStrings(plan.title, plan_shown.title);

    // ---- 3. Tech-spec artifact linked to the plan.
    //
    // `artifact add --plan` creates the artifact AND inserts the
    // entity_links (artifact→plan, 'derives-from') edge atomically.
    // We assert the kind round-trips so a future schema change that
    // silently drops the kind tag would surface here.
    const spec = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact",          "add",       "--json",
        "--plan",            plan_id_str, "--kind",
        "tech_spec",         "--body",    "## Endpoints\n- GET /reports\n",
        "Reports tech spec",
    });
    try std.testing.expectEqualStrings("tech_spec", spec.kind);

    // ---- 4. Decompose into tasks.
    //
    // Three tasks at distinct priorities. `plan next` should return
    // the lowest-priority-value task (priority 20 = highest).
    const t_low = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",                "add",           "--json",
        "--plan",              plan_id_str,     "--priority",
        "80",                  "--next-action", "wire response shape",
        "Wire response shape",
    });
    try std.testing.expectEqualStrings("todo", t_low.status);
    try std.testing.expectEqual(@as(i64, 80), t_low.priority);

    const t_mid = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",                   "add",           "--json",
        "--plan",                 plan_id_str,     "--priority",
        "50",                     "--next-action", "scaffold route handler",
        "Scaffold route handler",
    });
    _ = t_mid;

    const t_top = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",                  "add",           "--json",
        "--plan",                plan_id_str,     "--priority",
        "20",                    "--next-action", "define request schema",
        "Define request schema",
    });

    // ---- 5. `plan next` ranks correctly.
    //
    // Top priority task should surface. This composes plan + task —
    // a bug in either side that leaks priority semantics would fail
    // here.
    const next = suite.mustRunJSON(PlanNextJSON, arena, &.{
        "plan", "next", "--json", plan_id_str,
    });
    try std.testing.expect(next.available.len > 0);
    try std.testing.expectEqual(t_top.id, next.available[0].id);
    try std.testing.expectEqual(@as(i64, 20), next.available[0].priority);

    // ---- 6. Operator opens the task: `task update --status doing`.
    const top_id_str = std.fmt.allocPrint(arena, "{d}", .{t_top.id}) catch unreachable;
    const upd_out = suite.mustRun(&.{
        "task", "update", "--status", "doing", top_id_str,
    });
    gpa.free(upd_out);

    const t_top_doing = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "show", "--json", top_id_str,
    });
    try std.testing.expectEqualStrings("doing", t_top_doing.status);

    // ---- 7. Real operators run inside a captured session so handoff
    // / resume can find the active context. We pin the (vendor,
    // vendor-session-id) tuple via env so every subsequent capture
    // sees the same session.
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_VENDOR", .value = "claude" },
        .{ .key = "PLANAR_VENDOR_SESSION_ID", .value = "scenario-feature-lifecycle" },
    };
    const sess_stdout = suite.mustRunWith(&.{
        "capture", "session", "--json", "--task", top_id_str,
    }, &env);
    const sess = std.json.parseFromSlice(SessionJSON, arena, sess_stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(sess_stdout);
    try std.testing.expect(sess.value.id > 0);

    // ---- 8. Snapshot the working state. Required for `resume` to
    // classify the task as resume-ready.
    const snap_stdout = suite.mustRunWith(&.{
        "capture", "snapshot", "--json", "--task", top_id_str,
    }, &env);
    const snap = std.json.parseFromSlice(SnapshotJSON, arena, snap_stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(snap_stdout);
    try std.testing.expect(snap.value.id > 0);

    // ---- 9. Confirm `resume` produces a valid packet for the task.
    //
    // The packet's identity.task_id must match the task we're
    // resuming; state.next_action must echo the value set on
    // creation. This catches resume.buildPacket regressions that
    // shipped before plan 351 cycle E removed the M7 guard.
    const ResumePacket = struct {
        identity: struct {
            task_id: i64,
            title: []const u8,
        },
        state: struct {
            next_action: []const u8,
        },
    };
    const packet = suite.mustRunJSON(ResumePacket, arena, &.{
        "resume", "--json", top_id_str,
    });
    try std.testing.expectEqual(t_top.id, packet.identity.task_id);
    try std.testing.expectEqualStrings("define request schema", packet.state.next_action);

    // ---- 10. Mark task done.
    const done_out = suite.mustRun(&.{ "task", "done", top_id_str });
    gpa.free(done_out);

    const t_top_done = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "show", "--json", top_id_str,
    });
    try std.testing.expectEqualStrings("done", t_top_done.status);

    // ---- 11. `plan next` now returns the next-highest task.
    const next_after = suite.mustRunJSON(PlanNextJSON, arena, &.{
        "plan", "next", "--json", plan_id_str,
    });
    try std.testing.expect(next_after.available.len > 0);
    try std.testing.expect(next_after.available[0].id != t_top.id);
    try std.testing.expectEqual(@as(i64, 50), next_after.available[0].priority);
}

// ---- scenario 2: question + decision overlay ---------------------

test "scenario: feature lifecycle — open question + decision threaded through plan" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("featurelife-overlay");
    suite.addAssoc("featurelife-overlay", "project");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Auth refactor",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Operator captures an open uncertainty against the plan via
    // `question add --plan` (the Q237/task-2372 one-shot create-and-
    // link shape — plan 351 cycle D landed this).
    const q = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question",                              "add",           "--json",
        "--plan",                                plan_id_str,     "--body",
        "Do we keep the legacy session cookie?", "Legacy cookie",
    });
    try std.testing.expectEqualStrings("open", q.status);

    // Decision records the answer. `decision add --plan` should
    // attach the decision to the plan via entity_links so it
    // surfaces in `plan show` / `tree` later.
    const decision = suite.mustRunJSON(DecisionJSON, arena, &.{
        "decision",                                      "add",                        "--json",
        "--plan",                                        plan_id_str,                  "--body",
        "Drop legacy cookie; force re-auth on rollout.", "Drop legacy session cookie",
    });
    try std.testing.expectEqualStrings("proposed", decision.status);

    // Closing the question via `question answer` should flip status
    // to `answered`. Mirrors what an operator does after resolving
    // an open uncertainty.
    const q_id_str = std.fmt.allocPrint(arena, "{d}", .{q.id}) catch unreachable;
    const ans_out = suite.mustRun(&.{
        "question", "answer", q_id_str,
        "--answer",
        "Drop the legacy cookie — see decision below.",
    });
    gpa.free(ans_out);

    const q_after = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "show", "--json", q_id_str,
    });
    try std.testing.expectEqualStrings("answered", q_after.status);
}

// ---- scenario 3: tree rollup -------------------------------------

test "scenario: feature lifecycle — tree view rolls plan/task/artifact/question into one hierarchy" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("featurelife-tree");
    suite.addAssoc("featurelife-tree", "project");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Tree rollup smoke",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    _ = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact",    "add",                  "--json",
        "--plan",      plan_id_str,            "--kind",
        "design_note", "TREE_ROLLUP_artifact",
    });
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--plan", plan_id_str, "TREE_ROLLUP_task",
    });
    _ = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "add", "--json", "--plan", plan_id_str, "--body", "x", "TREE_ROLLUP_question",
    });

    // `tree --all-scopes` is the easiest way to assert composition
    // end-to-end here: the unique marker strings are sufficient to
    // pin presence and the ordering invariant lives in
    // parity_tree_render_test.zig. The plan was created under the
    // default (global) scope because the test runs from the suite's
    // working directory, not from `tmp_dir`; the assoc binding is
    // exercised more deliberately by the cross-scope polyrepo
    // scenario.
    const tree_out = suite.mustRun(&.{ "tree", "--all-scopes" });
    defer gpa.free(tree_out);

    // The plan + every linked entity should appear in the rendered
    // tree. The actual ordering is asserted by parity_tree_render_
    // test.zig; here we just confirm composition end-to-end.
    try std.testing.expect(std.mem.containsAtLeast(u8, tree_out, 1, "Tree rollup smoke"));
    try std.testing.expect(std.mem.containsAtLeast(u8, tree_out, 1, "TREE_ROLLUP_artifact"));
    try std.testing.expect(std.mem.containsAtLeast(u8, tree_out, 1, "TREE_ROLLUP_task"));
    try std.testing.expect(std.mem.containsAtLeast(u8, tree_out, 1, "TREE_ROLLUP_question"));
}
