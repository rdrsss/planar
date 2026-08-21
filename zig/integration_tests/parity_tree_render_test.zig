//! integration_tests/parity_tree_render_test.zig
//!
//! Cluster K-tree-render-drift parity tests for plan 351 (2 audit
//! rows). See parity-triage.md §K-tree-render-drift.
//!
//! From a non-registered cwd both binaries fall back to global; both
//! produce large trees, but they diverge in two systematic ways:
//!   (a) zig groups all children of a kind together
//!       (artifact:1, artifact:2, ..., question:1, question:2, ...)
//!       while Go interleaves by creation order.
//!   (b) Go truncates long titles (~80 chars) with `…`, zig prints
//!       full titles.
//!
//! Recommendation 2026-05-26: match Go on both. Creation-order
//! interleaving preserves temporal narrative; ~80-char truncation
//! keeps the tree scannable.
//!
//! Two test blocks:
//! - Ordering: create mixed-kind entities and assert the tree
//!   renders them in creation order, not kind-grouped order.
//! - Truncation: a very long task title is truncated with `…`.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
};
const TaskJSON = struct {
    id: i64,
};
const ArtifactJSON = struct {
    id: i64,
};
const QuestionJSON = struct {
    id: i64,
};

test "parity: tree renders mixed-kind plan children in creation order, not kind-grouped (Cluster K ordering; red until renderer matches Go)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Create a plan, then interleave an artifact, a question, an
    // artifact, a question. Tree should render them in this order.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "TREE_ORDER_INTEG_root",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    _ = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--plan", plan_id_str, "--kind", "design_note", "TREE_ORDER_FIRST_artifact",
    });
    _ = suite.mustRunJSON(QuestionJSON, arena, &.{
        "question", "add", "--json", "--plan", plan_id_str, "--body", "x", "TREE_ORDER_SECOND_question",
    });
    _ = suite.mustRunJSON(ArtifactJSON, arena, &.{
        "artifact", "add", "--json", "--plan", plan_id_str, "--kind", "design_note", "TREE_ORDER_THIRD_artifact",
    });

    const stdout = suite.mustRun(&.{ "tree", "--all-scopes" });
    defer gpa.free(stdout);

    // Locate each marker in stdout; assert ordering by index.
    const idx_first = std.mem.indexOf(u8, stdout, "TREE_ORDER_FIRST_artifact");
    const idx_second = std.mem.indexOf(u8, stdout, "TREE_ORDER_SECOND_question");
    const idx_third = std.mem.indexOf(u8, stdout, "TREE_ORDER_THIRD_artifact");

    if (idx_first == null or idx_second == null or idx_third == null) {
        std.debug.print(
            "\ntree ordering: one or more markers missing from output (first={any} second={any} third={any})\nstdout:\n{s}\n",
            .{ idx_first, idx_second, idx_third, stdout },
        );
        try std.testing.expect(false);
        return;
    }

    // Creation order: artifact_first < question_second < artifact_third.
    // Kind-grouped order would put both artifacts before the question
    // (i.e. first < third < second), which is what zig does today.
    const ok = idx_first.? < idx_second.? and idx_second.? < idx_third.?;
    if (!ok) {
        std.debug.print(
            "\ntree ordering: expected creation order (first < second < third); got first={d} second={d} third={d}\nstdout:\n{s}\n",
            .{ idx_first.?, idx_second.?, idx_third.?, stdout },
        );
        try std.testing.expect(false);
    }
}

test "parity: tree truncates long task titles (Cluster K truncation; red until renderer matches Go's ~80-char limit)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Long title (well above any plausible truncation threshold).
    const long_title = "TRUNC_INTEG_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA_END";
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "TRUNC_INTEG_root",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--plan", plan_id_str, long_title,
    });

    const stdout = suite.mustRun(&.{ "tree", "--all-scopes" });
    defer gpa.free(stdout);

    // Go truncates with `…` (U+2026 horizontal ellipsis). Zig today
    // prints the full title. Either the ellipsis appears OR the full
    // _END marker is absent — both indicate truncation.
    const has_ellipsis = std.mem.containsAtLeast(u8, stdout, 1, "…");
    const has_full_title = std.mem.containsAtLeast(u8, stdout, 1, "_END");

    if (!has_ellipsis or has_full_title) {
        std.debug.print(
            "\ntree truncation: expected ellipsis present AND full '_END' absent; got has_ellipsis={} has_full_title={}\nstdout snippet:\n{s}\n",
            .{ has_ellipsis, has_full_title, stdout },
        );
        try std.testing.expect(false);
    }
}
