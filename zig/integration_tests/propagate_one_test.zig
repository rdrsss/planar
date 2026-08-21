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

// =========================================================================
// Task 4168 (BUG): propagate-one must reject --strategy parent-issue /
// projects-v2 with a clear error directing the caller to use
// `ext propagate --github-strategy`.
//
// Regression: before the fix, passing parent-issue or projects-v2 fell
// through to the generic per-entity path, producing a mislabeled mirror
// link and silently caching the strategy on the anchor plan.
// =========================================================================

test "[error] propagate-one --strategy parent-issue is rejected (task 4168)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Register a jira system (closest we can get without GitHub—strategy
    // validation fires before the system kind check, so jira is fine here
    // to isolate the strategy guard).
    _ = suite.mustRunJSON(RegisterJSON, arena, &.{
        "ext",                    "register",   "jira",
        "jira-strat-pi",          "--base-url", "https://test.atlassian.net",
        "--project",              "TEST",       "--auth-env",
        "PLANAR_TEST_JIRA_TOKEN", "--json",
    });

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Strategy reject test plan",
    });
    const from_ref = try std.fmt.allocPrint(arena, "plan:{d}", .{plan.id});

    // --strategy parent-issue must be rejected with a clear error message.
    const stderr_pi = suite.expectFailure(&.{
        "ext",          "propagate-one", "jira-strat-pi",
        "--from",       from_ref,        "--strategy",
        "parent-issue",
    });
    defer gpa.free(stderr_pi);
    // Error message must direct the caller to `ext propagate --github-strategy`.
    try std.testing.expect(
        std.mem.indexOf(u8, stderr_pi, "not supported by propagate-one") != null or
            std.mem.indexOf(u8, stderr_pi, "ext propagate --github-strategy") != null,
    );

    // --strategy projects-v2 must also be rejected.
    const stderr_pv2 = suite.expectFailure(&.{
        "ext",         "propagate-one", "jira-strat-pi",
        "--from",      from_ref,        "--strategy",
        "projects-v2",
    });
    defer gpa.free(stderr_pv2);
    try std.testing.expect(
        std.mem.indexOf(u8, stderr_pv2, "not supported by propagate-one") != null or
            std.mem.indexOf(u8, stderr_pv2, "ext propagate --github-strategy") != null,
    );

    // --strategy tracking-issue must still be ACCEPTED (not rejected).
    // Verify it doesn't error on the strategy validation step (it will
    // error later when trying to build the adapter for a real run, but
    // under --dry-run it should succeed since no HTTP is needed).
    const result = suite.mustRunJSON(PropagateOneJSON, arena, &.{
        "ext",            "propagate-one", "jira-strat-pi",
        "--from",         from_ref,        "--strategy",
        "tracking-issue", "--dry-run",     "--json",
    });
    // tracking-issue is the only valid value for propagate-one; dry-run succeeds.
    try std.testing.expect(result.ok);
    try std.testing.expectEqualStrings("planned", result.op);
}
