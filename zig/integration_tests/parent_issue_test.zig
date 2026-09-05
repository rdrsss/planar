//! integration_tests/parent_issue_test.zig
//!
//! CLI-level smoke checks for the zig `--github-strategy` overrides on
//! plan 314 (tasks 2348 parent-issue + 2349 projects-v2). The full happy
//! path against a real GitHub remote is covered by the engine unit tests
//! in `src/engine/extsync/parent_issue.zig` and
//! `src/engine/extsync/projects_v2.zig`; these tests assert dispatch
//! wiring between the CLI handler and each engine.
//!
//! Two invariants:
//!
//!   1. `--github-strategy parent-issue` no longer exits with
//!      `NotImplemented` (exit 64). On a feature with no touched repos it
//!      now fails with the engine's "needs a touched repo" guidance.
//!
//!   2. `--github-strategy projects-v2` no longer exits with
//!      `NotImplemented`. Under dry-run with no touched repos it succeeds
//!      and reports the anchor as a planned creation under the new engine.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
};

test "ext propagate --github-strategy parent-issue dispatches to the new engine path (no NotImplemented)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Register a github-issues system.
    const reg_out = suite.mustRunExt(&.{
        "ext",       "register",  "github",     "gh-pi",
        "--project", "acme/demo", "--auth-env", "PLANAR_TEST_GH_TOKEN",
    });
    gpa.free(reg_out);

    // Create an anchor plan with no touched repos.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "parent-issue smoke plan",
    });
    const plan_id_s = try std.fmt.allocPrint(arena, "{d}", .{plan.id});

    // Run with --github-strategy parent-issue. The override path now
    // dispatches to the engine; the engine fails fast on the missing
    // touched repo with an InvalidInput-mapped exit instead of the legacy
    // NotImplemented (exit 64).
    const stderr = suite.expectFailureExtWith(
        &.{
            "ext",          "propagate", plan_id_s,
            "--system",     "gh-pi",     "--github-strategy",
            "parent-issue", "--dry-run",
        },
        &.{.{ .key = "PLANAR_TEST_GH_TOKEN", .value = "dummy-token" }},
    );
    defer gpa.free(stderr);

    // Negative assertion: we must NOT see the legacy "not yet ported"
    // message — that would mean the dispatch never reached the engine.
    if (std.mem.indexOf(u8, stderr, "not yet ported") != null) {
        std.debug.print("parent-issue still rejected as not-yet-ported:\n{s}\n", .{stderr});
        try std.testing.expect(false);
    }
    // Positive assertion: the engine surfaces the "needs a touched repo"
    // guidance, confirming we reached `propagateParentIssue` and got the
    // `NoTouchedRepos` branch translated.
    try std.testing.expect(std.mem.indexOf(u8, stderr, "needs a touched repo") != null);
}

test "ext propagate --github-strategy projects-v2 dry-run runs the engine (task 2349)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const reg_out = suite.mustRunExt(&.{
        "ext",       "register",  "github",     "gh-p2",
        "--project", "acme/demo", "--auth-env", "PLANAR_TEST_GH_TOKEN",
    });
    gpa.free(reg_out);

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "projects-v2 smoke plan",
    });
    const plan_id_s = try std.fmt.allocPrint(arena, "{d}", .{plan.id});

    // Dry-run with no touched repos: the projects-v2 engine still runs and
    // reports the anchor as a planned creation. We must NOT see the legacy
    // NotImplemented message (Cycle B''' closure).
    const out = suite.mustRunExtWith(
        &.{
            "ext",         "propagate", plan_id_s,
            "--system",    "gh-p2",     "--github-strategy",
            "projects-v2", "--dry-run",
        },
        &.{.{ .key = "PLANAR_TEST_GH_TOKEN", .value = "dummy-token" }},
    );
    defer gpa.free(out);

    if (std.mem.indexOf(u8, out, "not yet ported") != null) {
        std.debug.print("projects-v2 still rejected as not-yet-ported:\n{s}\n", .{out});
        try std.testing.expect(false);
    }
    try std.testing.expect(std.mem.indexOf(u8, out, "github-projects-v2") != null);
}
