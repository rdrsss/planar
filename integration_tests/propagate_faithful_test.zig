//! integration_tests/propagate_faithful_test.zig
//!
//! Faithfulness / regression test for the reimplemented `ext propagate`.
//! Verifies: task:seam-propagate-faithful
//!
//! Proves that:
//!   1. `ext propagate --dry-run` returns results with the same shape as
//!      iterating `ext propagate-one --dry-run` over each entry from
//!      `plan descendants --json`.
//!   2. The results are byte-comparable: the set of (entity_kind, entity_id,
//!      op) tuples is identical between the two paths.
//!
//! This is the correct faithfulness proof under the constraint that both
//! paths share the same `propagateOneEntity` call (the shared body), so
//! structural identity of the call graph is the primary proof; this test
//! closes the loop at the CLI surface.
//!
//! Since the reimplemented propagate loop in propagate.zig now calls
//! propagate_one.propagateOneEntity directly, any divergence between
//! `ext propagate` and manually iterating `ext propagate-one` would
//! require a bug in the propagate.zig loop body (wrong role assignment,
//! wrong strategy selection, etc.), which this test would catch.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64, title: []const u8, status: []const u8 };
const TaskJSON = struct { id: i64, title: []const u8 };
const RegisterJSON = struct { id: i64, slug: []const u8, kind: []const u8, ok: bool };

const PropagateResult = struct {
    ok: bool,
    plan_id: i64,
    system: []const u8,
    strategy: []const u8,
    created: i64,
    skipped: i64,
    failed: i64,
    results: []EntityResult,
};

const EntityResult = struct {
    entity_kind: []const u8,
    entity_id: i64,
    title: []const u8,
    op: []const u8,
    external_id: []const u8,
};

const PropagateOneJSON = struct {
    ok: bool,
    entity_kind: []const u8,
    entity_id: i64,
    op: []const u8,
    external_id: []const u8,
};

const DescendantEntry = struct {
    kind: []const u8,
    role: []const u8,
    id: i64,
    title: []const u8,
};

test "[happy] reimplemented ext propagate yields same results as iterating propagate-one" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Register a jira system (dry-run works without HTTP).
    _ = suite.mustRunJSON(RegisterJSON, arena, &.{
        "ext",                    "register",   "jira",
        "jira-faithful",          "--base-url", "https://test.atlassian.net",
        "--project",              "FAITH",      "--auth-env",
        "PLANAR_TEST_JIRA_TOKEN", "--json",
    });

    // Build a fixture plan with a child plan and two tasks.
    const anchor = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Faithful anchor plan",
    });
    const anchor_id_s = try std.fmt.allocPrint(arena, "{d}", .{anchor.id});

    const child = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--parent", anchor_id_s, "Child plan",
    });
    _ = child;

    // Add tasks linked via derives-from entity links (walkTree uses entity_links,
    // not the plan_id FK). We add two tasks and link them to the anchor plan.
    const task1 = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "Task one",
    });
    const task1_id_s = try std.fmt.allocPrint(arena, "{d}", .{task1.id});
    const anchor_plan_ref = try std.fmt.allocPrint(arena, "plan:{s}", .{anchor_id_s});
    const l1_out = suite.mustRun(&.{
        "task", "link", task1_id_s, anchor_plan_ref, "--relationship", "derives-from",
    });
    gpa.free(l1_out);

    const task2 = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "Task two",
    });
    const task2_id_s = try std.fmt.allocPrint(arena, "{d}", .{task2.id});
    const l2_out = suite.mustRun(&.{
        "task", "link", task2_id_s, anchor_plan_ref, "--relationship", "derives-from",
    });
    gpa.free(l2_out);

    // --- Path A: `ext propagate --dry-run --json` ----------------------------
    const propagate_raw = suite.mustRun(&.{
        "ext",      "propagate",     anchor_id_s,
        "--system", "jira-faithful", "--dry-run",
        "--json",
    });
    defer gpa.free(propagate_raw);

    const propagate_result = std.json.parseFromSlice(PropagateResult, arena, propagate_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("propagate JSON parse failed: {s}\nraw: {s}\n", .{ @errorName(e), propagate_raw });
        try std.testing.expect(false);
        unreachable;
    };
    const propagate_entries = propagate_result.value.results;
    try std.testing.expect(propagate_result.value.ok);
    try std.testing.expect(propagate_entries.len >= 1);

    // --- Path B: `plan descendants | for each ext propagate-one --dry-run` ---
    const descendants_raw = suite.mustRun(&.{ "plan", "descendants", "--json", anchor_id_s });
    defer gpa.free(descendants_raw);

    const descendants_parsed = std.json.parseFromSlice([]DescendantEntry, arena, descendants_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("descendants JSON parse failed: {s}\nraw: {s}\n", .{ @errorName(e), descendants_raw });
        try std.testing.expect(false);
        unreachable;
    };
    const descendants = descendants_parsed.value;
    try std.testing.expect(descendants.len >= 1);

    // --- Compare path A vs path B -------------------------------------------
    // Same number of entities.
    try std.testing.expectEqual(propagate_entries.len, descendants.len);

    // For each descendant, run propagate-one and compare against the
    // corresponding propagate result entry.
    for (propagate_entries, descendants) |a, entry| {
        const from_ref = try std.fmt.allocPrint(arena, "{s}:{d}", .{ entry.kind, entry.id });
        const one_raw = suite.mustRun(&.{
            "ext",    "propagate-one", "jira-faithful",
            "--from", from_ref,        "--dry-run",
            "--json",
        });
        defer gpa.free(one_raw);

        const one = std.json.parseFromSlice(PropagateOneJSON, arena, one_raw, .{
            .allocate = .alloc_always,
            .ignore_unknown_fields = true,
        }) catch |e| {
            std.debug.print("propagate-one JSON parse failed for {s}: {s}\nraw: {s}\n", .{ from_ref, @errorName(e), one_raw });
            try std.testing.expect(false);
            unreachable;
        };
        const b = one.value;

        // Same (entity_kind, entity_id, op) for each position.
        try std.testing.expectEqualStrings(a.entity_kind, b.entity_kind);
        try std.testing.expectEqual(a.entity_id, b.entity_id);
        // Under dry-run both paths should emit "planned" (propagate.zig maps
        // propagateOneEntity's "planned" to the results_buf, so the shapes match).
        try std.testing.expectEqualStrings(a.op, b.op);
    }
}

test "[happy] ext propagate idempotency: second dry-run after first dry-run still works" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    _ = suite.mustRunJSON(RegisterJSON, arena, &.{
        "ext",                    "register",   "jira",
        "jira-idem",              "--base-url", "https://test.atlassian.net",
        "--project",              "IDEM",       "--auth-env",
        "PLANAR_TEST_JIRA_TOKEN", "--json",
    });

    const anchor = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Idempotency test plan",
    });
    const anchor_id_s = try std.fmt.allocPrint(arena, "{d}", .{anchor.id});

    // First dry-run.
    const r1_raw = suite.mustRun(&.{
        "ext",      "propagate", anchor_id_s,
        "--system", "jira-idem", "--dry-run",
        "--json",
    });
    defer gpa.free(r1_raw);
    const r1 = std.json.parseFromSlice(PropagateResult, arena, r1_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expect(r1.value.ok);
    const first_created = r1.value.created;

    // Second dry-run: same counts (dry-run never writes links so idempotency
    // only matters for real runs; this confirms the verb remains stable).
    const r2_raw = suite.mustRun(&.{
        "ext",      "propagate", anchor_id_s,
        "--system", "jira-idem", "--dry-run",
        "--json",
    });
    defer gpa.free(r2_raw);
    const r2 = std.json.parseFromSlice(PropagateResult, arena, r2_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expect(r2.value.ok);
    try std.testing.expectEqual(first_created, r2.value.created);
}
