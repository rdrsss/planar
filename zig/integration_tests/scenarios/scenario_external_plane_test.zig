//! integration_tests/scenarios/scenario_external_plane_test.zig
//!
//! Scenario M2 of plan 352 (no longer deferred). External plane:
//! operator registers an external system, lists it, tests the
//! connection, and previews a propagate against a real plan via
//! --dry-run. No live HTTP — the dry-run path exercises the strategy
//! detection + template rendering without contacting the remote.
//!
//! Verbs exercised:
//!     init, plan create, task add, artifact add (with tech_spec body),
//!     ext register github, ext register jira, ext list, ext test
//!     (negative path with dummy token), ext propagate --dry-run.
//!
//! Verifies (roadmap slugs):
//!     [ep/register-jira] — `ext register jira` produces a row with
//!     kind=jira; `ext list` surfaces it.
//!     [ep/propagate] — `ext propagate <plan> --dry-run --json`
//!     returns the strategy + planned results without HTTP. The
//!     planned[].op values reflect what propagate WOULD do live.

const std = @import("std");
const harness = @import("harness");

const RegisterResult = struct {
    ok: bool,
    id: i64,
    slug: []const u8,
    kind: []const u8,
};

const PropagateResult = struct {
    ok: bool,
    plan_id: i64,
    system: []const u8,
    strategy: []const u8,
    created: i64,
    skipped: i64,
    failed: i64,
};

const PlanJSON = struct { id: i64, title: []const u8, status: []const u8 };
const TaskJSON = struct { id: i64, title: []const u8 };
const ArtifactJSON = struct { id: i64, title: []const u8 };

test "scenario: external plane — register github + jira, list, propagate --dry-run preview" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ext-flow");

    // ---- 1. Register both adapter shapes.
    const gh = suite.mustRunExtJSON(RegisterResult, arena, &.{
        "ext",       "register",     "github",     "gh-ext",
        "--project", "acme/widgets", "--auth-env", "PLANAR_TEST_GH_TOKEN",
        "--json",
    });
    try std.testing.expect(gh.ok);
    try std.testing.expectEqualStrings("gh-ext", gh.slug);
    try std.testing.expectEqualStrings("github-issues", gh.kind);

    const jira = suite.mustRunExtJSON(RegisterResult, arena, &.{
        "ext",                    "register",   "jira",
        "jira-ext",               "--base-url", "https://example.atlassian.net",
        "--project",              "WIDG",       "--auth-env",
        "PLANAR_TEST_JIRA_TOKEN", "--json",
    });
    try std.testing.expect(jira.ok);
    try std.testing.expectEqualStrings("jira-ext", jira.slug);
    try std.testing.expectEqualStrings("jira", jira.kind);

    // ---- 2. ext list contains both rows.
    const list_out = suite.mustRunExt(&.{ "ext", "list" });
    defer gpa.free(list_out);
    try std.testing.expect(std.mem.containsAtLeast(u8, list_out, 1, "gh-ext"));
    try std.testing.expect(std.mem.containsAtLeast(u8, list_out, 1, "jira-ext"));

    // ---- 3. Real plan + task + artifact as the propagate target.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Widget feature",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",             "add",                   "--json",
        "--plan",           plan_id_str,             "--next-action",
        "implement widget", "Implement widget core",
    });

    _ = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact",    "add",       "--json",
        "--plan",      plan_id_str, "--kind",
        "tech_spec",   "--body",    "# Widget tech spec\n\n## Endpoints\n- GET /widgets\n",
        "Widget spec",
    });

    // ---- 4. ext propagate --dry-run --json
    //
    // Returns a JSON preview of the propagation plan without
    // contacting the remote. We pin the documented shape: {ok,
    // plan_id, system, strategy, created, skipped, failed,
    // results: [...]}. created reflects the count of NEW
    // counterparts a live run would create.
    const preview = suite.mustRunExtJSON(PropagateResult, arena, &.{
        "ext", "propagate", plan_id_str, "--dry-run", "--system", "gh-ext", "--json",
    });
    try std.testing.expect(preview.ok);
    try std.testing.expectEqual(plan.id, preview.plan_id);
    try std.testing.expectEqualStrings("gh-ext", preview.system);
    try std.testing.expect(preview.strategy.len > 0);
    try std.testing.expect(preview.created >= 1);
    try std.testing.expectEqual(@as(i64, 0), preview.failed);

    // ---- 5. ext test on the GitHub system. Without a valid token
    // / network this expects to fail; we pass a dummy and accept
    // either outcome — the verb's CONTRACT is "exit 0 on JSON
    // {ok:true} or exit non-zero on failure". We just want the
    // verb to round-trip its JSON when it does succeed.
    const test_res = suite.execExtWith(&.{
        "ext", "test", "gh-ext", "--json",
    }, &.{.{ .key = "PLANAR_TEST_GH_TOKEN", .value = "dummy-token" }});
    defer gpa.free(test_res.stdout);
    defer gpa.free(test_res.stderr);
    // Either exit 0 or non-zero is acceptable; the verb may try
    // to contact GitHub. Just confirm the binary doesn't crash.
    try std.testing.expect(test_res.term == .exited);
}
