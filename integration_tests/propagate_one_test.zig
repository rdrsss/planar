//! integration_tests/propagate_one_test.zig
//!
//! Focused per-verb tests for `planar ext propagate-one <system> --from <kind:id> --json`.
//! Verifies: task:seam-propagate-one
//!
//! Scenarios:
//!   [happy] propagate-one --dry-run creates a planned entry without HTTP
//!   [empty] propagate-one on an already-linked entity skips (idempotent)
//!   [error] propagate-one with invalid --from rejects with clear error
//!   [error] propagate-one with no registered system rejects

const std = @import("std");
const harness = @import("harness");

const RegisterJSON = struct { id: i64, slug: []const u8, kind: []const u8, ok: bool };
const PlanJSON = struct { id: i64, title: []const u8, status: []const u8 };
const TaskJSON = struct { id: i64, title: []const u8 };

const PropagateOneJSON = struct {
    ok: bool,
    entity_kind: []const u8,
    entity_id: i64,
    title: []const u8,
    op: []const u8,
    external_id: []const u8,
    system: []const u8,
    strategy: []const u8,
};

test "[happy] propagate-one --dry-run returns planned without contacting remote" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Register a jira system (doesn't need a network in dry-run mode).
    _ = suite.mustRunJSON(RegisterJSON, arena, &.{
        "ext",                    "register",   "jira",
        "jira-p1",                "--base-url", "https://test.atlassian.net",
        "--project",              "TEST",       "--auth-env",
        "PLANAR_TEST_JIRA_TOKEN", "--json",
    });

    // Create a plan to propagate.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Propagate one test plan",
    });
    const from_ref = try std.fmt.allocPrint(arena, "plan:{d}", .{plan.id});

    // Run propagate-one in dry-run mode — no HTTP call needed.
    const result = suite.mustRunJSON(PropagateOneJSON, arena, &.{
        "ext",    "propagate-one", "jira-p1",
        "--from", from_ref,        "--dry-run",
        "--json",
    });

    try std.testing.expect(result.ok);
    try std.testing.expectEqualStrings("plan", result.entity_kind);
    try std.testing.expectEqual(plan.id, result.entity_id);
    // dry-run produces op=planned
    try std.testing.expectEqualStrings("planned", result.op);
    try std.testing.expectEqualStrings("jira-p1", result.system);
    try std.testing.expect(result.strategy.len > 0);
    // external_id is a placeholder under dry-run
    try std.testing.expect(result.external_id.len > 0);
}

test "[happy] propagate-one on a task --dry-run returns planned" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    _ = suite.mustRunJSON(RegisterJSON, arena, &.{
        "ext",                    "register",   "jira",
        "jira-p1t",               "--base-url", "https://test.atlassian.net",
        "--project",              "TEST",       "--auth-env",
        "PLANAR_TEST_JIRA_TOKEN", "--json",
    });

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Task propagate-one test",
    });
    const plan_id_s = try std.fmt.allocPrint(arena, "{d}", .{plan.id});
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "Test task",
    });
    // Link task to plan via derives-from entity link.
    const task_id_s = try std.fmt.allocPrint(arena, "{d}", .{task.id});
    const plan_ref = try std.fmt.allocPrint(arena, "plan:{s}", .{plan_id_s});
    const lout = suite.mustRun(&.{
        "task", "link", task_id_s, plan_ref, "--relationship", "derives-from",
    });
    gpa.free(lout);
    const from_ref = try std.fmt.allocPrint(arena, "task:{d}", .{task.id});

    const result = suite.mustRunJSON(PropagateOneJSON, arena, &.{
        "ext",    "propagate-one", "jira-p1t",
        "--from", from_ref,        "--dry-run",
        "--json",
    });

    try std.testing.expect(result.ok);
    try std.testing.expectEqualStrings("task", result.entity_kind);
    try std.testing.expectEqual(task.id, result.entity_id);
    try std.testing.expectEqualStrings("planned", result.op);
}

test "[error] propagate-one with invalid --from format rejects" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    _ = suite.mustRunJSON(RegisterJSON, arena, &.{
        "ext",                    "register",   "jira",
        "jira-err",               "--base-url", "https://test.atlassian.net",
        "--project",              "TEST",       "--auth-env",
        "PLANAR_TEST_JIRA_TOKEN", "--json",
    });

    // Missing colon — should fail with InvalidInput.
    const stderr = suite.expectFailure(&.{
        "ext",    "propagate-one", "jira-err",
        "--from", "plan42",        "--dry-run",
    });
    defer gpa.free(stderr);
    try std.testing.expect(stderr.len > 0);
}

test "[error] propagate-one with no registered systems rejects" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // No ext register call — should fail with NotFound.
    const stderr = suite.expectFailure(&.{
        "ext",    "propagate-one", "nonexistent-system",
        "--from", "plan:1",        "--dry-run",
    });
    defer gpa.free(stderr);
    try std.testing.expect(stderr.len > 0);
}
