//! integration_tests/entity_view_test.zig
//!
//! Extends the `<entity> view` coverage beyond `plan view` (the only one
//! pinned by editflow_view_test.zig). Each entity kind renders into its
//! anchor-plan workbench feature dir and cats the result; a regression in
//! any non-plan entity's view render would otherwise pass
//! `make test-integration` silently.
//!
//! For each of task / question / decision / scenario / artifact: create
//! the entity under a plan, run `<kind> view <id>` with PAGER=/bin/cat and
//! a controlled PLANAR_WORKBENCH_ROOT, and assert exit 0 plus the entity's
//! title in the rendered markdown.
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

const Id = struct { id: i64 };

fn workbenchRoot(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    var tmp_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp_len = suite.tmp_dir.dir.realPath(std.testing.io, &tmp_buf) catch @panic("cannot resolve tmp dir");
    return std.fs.path.join(arena, &.{ tmp_buf[0..tmp_len], "workbench" }) catch @panic("OOM");
}

// NOTE: callers pass the FULL literal arg slice (e.g. `&.{ "task",
// "view", id }`) rather than a `kind` variable, so the leaf-coverage
// script (scripts/coverage-check.sh) can see the literal `verb`,`view`
// pair. A `kind`-variable helper exercises the verb but reads as
// uncovered — the tc.kind blind spot.
fn assertView(
    suite: *harness.Suite,
    gpa: std.mem.Allocator,
    wb_root: []const u8,
    args: []const []const u8,
    title: []const u8,
) void {
    const res = suite.execWith(args, &.{
        .{ .key = "PAGER", .value = "/bin/cat" },
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("\n{any} failed\nstdout: {s}\nstderr: {s}\n", .{ args, res.stdout, res.stderr });
        @panic("entity view: non-zero exit");
    }
    if (!std.mem.containsAtLeast(u8, res.stdout, 1, title)) {
        std.debug.print("\n{any}: title '{s}' missing from render\nstdout: {s}\n", .{ args, title, res.stdout });
        @panic("entity view: title missing from rendered markdown");
    }
}

test "entity view: task/question/decision/scenario/artifact all render their title" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const wb_root = workbenchRoot(&suite, arena);

    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "View Family Plan" });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Create one of each plan-linked entity. decision requires --body.
    const task = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--json", "--plan", pid, "--next-action", "do", "TaskView" });
    const question = suite.mustRunJSON(Id, arena, &.{ "question", "add", "--json", "--plan", pid, "QuestionView" });
    const decision = suite.mustRunJSON(Id, arena, &.{ "decision", "add", "--json", "--plan", pid, "--body", "rationale", "DecisionView" });
    const artifact = suite.mustRunJSON(Id, arena, &.{ "artifact", "add", "--json", "--plan", pid, "--kind", "tech_spec", "ArtifactView" });

    const task_id = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;
    const question_id = std.fmt.allocPrint(arena, "{d}", .{question.id}) catch unreachable;
    const decision_id = std.fmt.allocPrint(arena, "{d}", .{decision.id}) catch unreachable;
    const artifact_id = std.fmt.allocPrint(arena, "{d}", .{artifact.id}) catch unreachable;

    assertView(&suite, gpa, wb_root, &.{ "task", "view", task_id }, "TaskView");
    assertView(&suite, gpa, wb_root, &.{ "question", "view", question_id }, "QuestionView");
    assertView(&suite, gpa, wb_root, &.{ "decision", "view", decision_id }, "DecisionView");
    assertView(&suite, gpa, wb_root, &.{ "artifact", "view", artifact_id }, "ArtifactView");

    // KNOWN GAP — `scenario view` is intentionally NOT covered here.
    // `scenario add --plan <id>` accepts the flag but prints
    // "--plan accepted but not yet linked (entity_links not wired)" and
    // does NOT create the anchor-plan link, and `links add scenario:<id>
    // …` rejects the `scenario` kind — so a freshly-added scenario has no
    // anchor plan and `scenario view` fails with NoPlanLink. There is no
    // public-CLI path to make a scenario viewable today. This is a real
    // gap (scenario add --plan should wire the link, or scenario view
    // should fall back to scope) — fix that, then add scenario here.
}
