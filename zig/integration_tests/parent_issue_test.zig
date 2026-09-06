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

// RETIRED against decision 1001 (accepted; plan 1006, task 6451).
//
// This test asserted `ext propagate --github-strategy projects-v2 --dry-run`
// RUNS the engine and reports the anchor as a planned creation -- the
// pre-cutover oracle contract from task 2349. Decision 1001 permanently cut
// the multi-repo `projects_v2` GitHub strategy from the C++ rewrite: Planar
// does not replicate the context plane into a GitHub Projects board. The
// C++ `planar-ext ext propagate` (`src/cmd/planar-ext/handlers/
// propagate.cpp`) now refuses `github-strategy projects-v2` explicitly and
// unconditionally, citing decision 1001 in its own error text -- that
// refusal is the CORRECT, deliberate behavior, not a gap to close. This
// test is not "fixed" to assert the refusal instead, because the point of
// retiring it here (rather than converting it) is that the ORIGINAL claim
// -- "the engine runs" -- is no longer a claim this codebase makes about
// itself at all; asserting the negative in its place would silently reuse
// a name that used to mean something else.
//
// The refusal itself IS covered, by
// `ext_propagate_leaf.t.cpp`'s "ext propagate --github-strategy projects-v2
// refuses by decision 1001, never reaching the engine (task 6451)" Catch2
// case, run under `ctest -L cmd_planar_ext`. `select_strategy` still
// REPORTS the `github-projects-v2` bucket name for a >=2-repo GitHub
// feature (see `ext_strategy.cppm`'s header) precisely so `propagate.cpp`
// can name the refusal reason instead of mis-executing or silently
// downgrading the feature -- that reporting path is preserved and is
// itself exercised by `ext_strategy_leaves.t.cpp`.
test "ext propagate --github-strategy projects-v2 dry-run runs the engine (task 2349) -- RETIRED, decision 1001" {
    return error.SkipZigTest;
}
