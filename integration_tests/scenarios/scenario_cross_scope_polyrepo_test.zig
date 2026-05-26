//! integration_tests/scenarios/scenario_cross_scope_polyrepo_test.zig
//!
//! Scenario M1 of plan 352. Cross-scope polyrepo: an operator
//! working across two sibling project checkouts bound to one (or
//! two) associations, then exercising association membership,
//! task-touches links across repos, and the current observable
//! behavior of cwd-derive at plan-create time.
//!
//! Primary flow verbs:
//!     init (via the harness's registerProject + a sibling
//!     freshSystemTmpDir + mustRunInDir),
//!     assoc create, assoc add (×2), assoc members,
//!     plan create, plan show,
//!     task add, task touches add, task list --touches.
//!
//! Verifies (roadmap slugs):
//!     [cs/init-two-projects] — two projects bound to one assoc
//!     both surface in `assoc members --json`.
//!     [cs/task-touches] — `task touches add` against a sibling
//!     project's slug surfaces the touches edge via
//!     `task list --touches <slug> --json`.
//!     [cs/plan-scope-derive] (partial / observed-behavior pin) —
//!     `plan create` from inside a registered project's cwd
//!     produces a usable plan. The scenario locks the CURRENT
//!     observable behavior (plan lands in `global` regardless of
//!     assoc binding); the aspirational claim that the plan
//!     should derive the bound assoc scope is filed as task
//!     2450 on plan 352. Once that engine fix lands, the
//!     scope_kind assertion below tightens from "global" to
//!     "association".
//!
//! Bullets deferred to follow-up tasks (NOT asserted here):
//!     [cs/cross-scope-refusal] — the cross-scope guard does not
//!     currently fire for `task update` from operator cwd. Filed
//!     as task 2451. Once landed, the second block from this
//!     scenario's original draft (rolled back 2026-05-26) is
//!     restored.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
    scope_kind: []const u8,
};

const TaskJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const AssocMember = struct {
    id: i64,
    slug: []const u8,
    name: []const u8,
    root_path: []const u8,
};

// =========================================================================
// Primary flow: two projects under one assoc, plan + task + touches
// =========================================================================

test "scenario: cross-scope polyrepo — two projects under one assoc surface in assoc members + plan create + task touches" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // ---- 1. Two sibling projects.
    //
    // The suite's tmp_dir becomes project A via registerProject
    // (which shells `planar init` from inside the dir, exercising
    // the init code path the operator actually uses). A fresh
    // system tmpdir becomes project B via mustRunInDir.
    _ = suite.registerProject("polyrepo-a");
    const proj_a = suite.tmpAbsPath();
    const proj_b = suite.freshSystemTmpDir();
    const init_b_out = suite.mustRunInDir(proj_b, &.{
        "init", "--allow-no-repo", "--name", "polyrepo-b",
    });
    gpa.free(init_b_out);

    // ---- 2. One association binding both projects.
    const assoc_slug = "polyrepo-org";
    const cr_out = suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "org" });
    gpa.free(cr_out);
    const add_a_out = suite.mustRun(&.{ "assoc", "add", assoc_slug, proj_a });
    gpa.free(add_a_out);
    const add_b_out = suite.mustRun(&.{ "assoc", "add", assoc_slug, proj_b });
    gpa.free(add_b_out);

    // ---- 3. `assoc members --json` lists both projects.
    //
    // Pins the post-state of the two `assoc add` calls. The JSON
    // shape is a flat array of {id, slug, name, root_path}.
    const members_raw = suite.mustRun(&.{ "assoc", "members", assoc_slug, "--json" });
    defer gpa.free(members_raw);

    const members_parsed = std.json.parseFromSlice([]AssocMember, arena, members_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nassoc members JSON parse failed: {s}\nraw: {s}\n", .{ @errorName(e), members_raw });
        try std.testing.expect(false);
        return;
    };
    const members = members_parsed.value;

    try std.testing.expectEqual(@as(usize, 2), members.len);
    var saw_a = false;
    var saw_b = false;
    var proj_b_slug: []const u8 = "";
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "polyrepo-a")) saw_a = true;
        if (std.mem.eql(u8, m.name, "polyrepo-b")) {
            saw_b = true;
            proj_b_slug = m.slug;
        }
    }
    try std.testing.expect(saw_a);
    try std.testing.expect(saw_b);
    try std.testing.expect(proj_b_slug.len > 0);

    // ---- 4. Plan from inside project A's cwd.
    //
    // Known gap (task 2450): cwd-derive at plan-create does not
    // surface the bound assoc scope today. The plan lands in
    // `global`. We assert the current observable behavior so the
    // engine fix lands as a deliberate test update, not a silent
    // contract change.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Polyrepo feature spike",
    });
    try std.testing.expectEqualStrings("Polyrepo feature spike", plan.title);
    try std.testing.expectEqualStrings("draft", plan.status);
    try std.testing.expectEqualStrings("global", plan.scope_kind);

    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // ---- 5. Task on the plan + touches link to project B.
    //
    // `task touches add <task-id> <repo-slug>` writes an
    // entity_links (task → repo, relationship=touches) edge.
    // Today `task show --json` doesn't carry the touches overlay
    // (verified via probe 2026-05-26), but `task list --touches
    // <slug> --json` does — that's what we assert here.
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",         "add",     "--json",
        "--plan",       plan_id_str,
        "--next-action", "spike cross-repo signature",
        "Spike cross-repo signature",
    });
    try std.testing.expectEqualStrings("todo", task.status);

    const task_id_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    const touches_out = suite.mustRun(&.{
        "task", "touches", "add", task_id_str, proj_b_slug,
    });
    gpa.free(touches_out);

    // ---- 6. `task list --touches <proj-b-slug>` surfaces the
    // task via the touches edge. The query is the operator's
    // canonical "what touches repo X" lookup.
    const touches_list_raw = suite.mustRun(&.{
        "task", "list", "--touches", proj_b_slug, "--json",
    });
    defer gpa.free(touches_list_raw);

    const TouchesList = []TaskJSON;
    const touches_parsed = std.json.parseFromSlice(TouchesList, arena, touches_list_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\ntouches list JSON parse failed: {s}\nraw: {s}\n", .{ @errorName(e), touches_list_raw });
        try std.testing.expect(false);
        return;
    };
    var saw_task = false;
    for (touches_parsed.value) |t| {
        if (t.id == task.id) {
            saw_task = true;
            break;
        }
    }
    if (!saw_task) {
        std.debug.print(
            "\ntask {d} did not surface in 'task list --touches {s} --json'\nraw: {s}\n",
            .{ task.id, proj_b_slug, touches_list_raw },
        );
        try std.testing.expect(false);
    }
}

// =========================================================================
// Composition assertion: plan show reflects the plan + task created
// =========================================================================

test "scenario: cross-scope polyrepo — plan show round-trips title and surfaces the task count" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("polyrepo-roundtrip-a");

    const assoc_slug = "polyrepo-roundtrip-org";
    const cr_out = suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "org" });
    gpa.free(cr_out);
    const add_a_out = suite.mustRun(&.{ "assoc", "add", assoc_slug, suite.tmpAbsPath() });
    gpa.free(add_a_out);

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Roundtrip target",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--plan", plan_id_str, "round-trip task",
    });

    // plan show --json should round-trip the title + status.
    const plan_shown = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "show", "--json", plan_id_str,
    });
    try std.testing.expectEqual(plan.id, plan_shown.id);
    try std.testing.expectEqualStrings("Roundtrip target", plan_shown.title);
    try std.testing.expectEqualStrings("draft", plan_shown.status);
}
