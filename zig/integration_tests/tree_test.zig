//! integration_tests/tree_test.zig
//!
//! Black-box integration tests for `planar tree`.
//!
//! Verifies:
//!   - Happy path (text): plan + task + linked question appear in `planar tree --depth 3`.
//!   - Happy path (JSON): same setup; `--json` emits a JSON object with kind = "scope"
//!     and children containing the plan node.
//!   - Exit code 0 always — an empty tree is not an error.
//!
//! Scenario: Happy path — Tree drills the plan hierarchy (test spec §M12,
//!   task:engine-tree, task:handler-tree).

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    slug: []const u8 = "",
    status: []const u8 = "",
};

const TaskJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8 = "",
};

const QuestionJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8 = "",
};

test "tree happy path text: plan → task → linked question appear in output" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Create a root plan.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "TREE_INTEG_Root Plan",
    });

    // Create a task on the plan (--plan <id> int flag).
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",   "add",       "--json",
        "--plan", plan_id_str, "TREE_INTEG_Top Task",
    });

    // Create a question.
    const question = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "add", "--json", "TREE_INTEG_Linked Question",
    });

    // Link the question to the plan via derives-from.
    // `planar links add <from-ref> <to-ref> --relationship <rel>`
    // from-ref = "question:<id>", to-ref = "plan:<id>"
    const from_ref = std.fmt.allocPrint(arena, "question:{d}", .{question.id}) catch unreachable;
    const to_ref = std.fmt.allocPrint(arena, "plan:{d}", .{plan.id}) catch unreachable;
    const link_out = suite.mustRun(&.{
        "links",          "add",
        from_ref,         to_ref,
        "--relationship", "derives-from",
    });
    gpa.free(link_out);

    // Run `planar tree --depth 3` (text mode).
    const stdout = suite.mustRun(&.{ "tree", "--depth", "3", "--scope", "global" });
    defer gpa.free(stdout);

    // All four entities must appear in the output.
    try std.testing.expect(std.mem.containsAtLeast(u8, stdout, 1, "TREE_INTEG_Root Plan"));
    try std.testing.expect(std.mem.containsAtLeast(u8, stdout, 1, "TREE_INTEG_Top Task"));
    try std.testing.expect(std.mem.containsAtLeast(u8, stdout, 1, "TREE_INTEG_Linked Question"));
}

test "tree JSON: --json output is valid JSON object with kind=scope and plan child" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Create a plan so the tree is non-empty.
    _ = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "TREE_JSON_INTEG_Plan",
    });

    // Run `planar tree --depth 3 --json`.
    const stdout = suite.mustRun(&.{ "tree", "--depth", "3", "--json", "--scope", "global" });
    defer gpa.free(stdout);

    // Parse as a JSON object — single scope root.
    const ScopeNode = struct {
        kind: []const u8,
        children: []const struct {
            kind: []const u8,
            title: []const u8,
        },
    };

    const trimmed = std.mem.trim(u8, stdout, " \n");
    const parsed = std.json.parseFromSlice(ScopeNode, arena, trimmed, .{
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print(
            "\ntree --json: failed to parse output: {s}\nraw: {s}\n",
            .{ @errorName(e), stdout },
        );
        try std.testing.expect(false);
        unreachable;
    };

    try std.testing.expectEqualStrings("scope", parsed.value.kind);
    try std.testing.expect(parsed.value.children.len >= 1);

    var found_plan = false;
    for (parsed.value.children) |c| {
        if (std.mem.eql(u8, c.kind, "plan") and
            std.mem.containsAtLeast(u8, c.title, 1, "TREE_JSON_INTEG_Plan"))
        {
            found_plan = true;
        }
    }
    try std.testing.expect(found_plan);
}
