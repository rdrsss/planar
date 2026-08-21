//! integration_tests/plan_descendants_test.zig
//!
//! Focused per-verb tests for `planar plan descendants <plan-id> --json`.
//! Verifies: task:seam-plan-descendants
//!
//! Scenarios:
//!   [happy] descendants returns the subtree in topological order
//!   [empty] descendants on a leaf plan returns an empty subtree

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64, title: []const u8, status: []const u8 };
const TaskJSON = struct { id: i64, title: []const u8 };

const DescendantEntry = struct {
    kind: []const u8,
    role: []const u8,
    id: i64,
    title: []const u8,
};

test "[happy] plan descendants returns the subtree in topological order" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Create anchor plan.
    const anchor = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Anchor plan",
    });
    const anchor_id_s = try std.fmt.allocPrint(arena, "{d}", .{anchor.id});

    // Create a child plan under the anchor.
    const child = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--parent", anchor_id_s, "Child plan",
    });
    const child_id_s = try std.fmt.allocPrint(arena, "{d}", .{child.id});

    // Add a task and link it to the anchor plan via derives-from entity link.
    // walkTree walks entity_links(derives-from), not the plan_id FK.
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "Task on anchor",
    });
    const task_id_s = try std.fmt.allocPrint(arena, "{d}", .{task.id});
    const anchor_plan_ref = try std.fmt.allocPrint(arena, "plan:{s}", .{anchor_id_s});
    const link_out = suite.mustRun(&.{
        "task", "link", task_id_s, anchor_plan_ref, "--relationship", "derives-from",
    });
    gpa.free(link_out);

    // Add a task linked to the child plan.
    const child_task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "Task on child",
    });
    const child_task_id_s = try std.fmt.allocPrint(arena, "{d}", .{child_task.id});
    const child_plan_ref = try std.fmt.allocPrint(arena, "plan:{s}", .{child_id_s});
    const link_out2 = suite.mustRun(&.{
        "task", "link", child_task_id_s, child_plan_ref, "--relationship", "derives-from",
    });
    gpa.free(link_out2);

    // Run plan descendants.
    const raw = suite.mustRun(&.{ "plan", "descendants", "--json", anchor_id_s });
    defer gpa.free(raw);

    const parsed = std.json.parseFromSlice([]DescendantEntry, arena, raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("plan descendants JSON parse failed: {s}\nraw: {s}\n", .{ @errorName(e), raw });
        try std.testing.expect(false);
        unreachable;
    };
    const entries = parsed.value;

    // Must have at least 4 entries: anchor, child plan, 2 tasks.
    try std.testing.expect(entries.len >= 4);

    // First entry must be the anchor plan.
    try std.testing.expectEqualStrings("plan", entries[0].kind);
    try std.testing.expectEqualStrings("anchor", entries[0].role);
    try std.testing.expectEqual(anchor.id, entries[0].id);

    // All entries must have a valid kind.
    for (entries) |entry| {
        const kind_ok = std.mem.eql(u8, entry.kind, "plan") or std.mem.eql(u8, entry.kind, "task");
        try std.testing.expect(kind_ok);
    }

    // The child plan and both tasks must appear.
    var saw_child = false;
    var saw_task = false;
    var saw_child_task = false;
    for (entries) |entry| {
        if (entry.id == child.id and std.mem.eql(u8, entry.kind, "plan")) saw_child = true;
        if (entry.id == task.id and std.mem.eql(u8, entry.kind, "task")) saw_task = true;
        if (entry.id == child_task.id and std.mem.eql(u8, entry.kind, "task")) saw_child_task = true;
    }
    try std.testing.expect(saw_child);
    try std.testing.expect(saw_task);
    try std.testing.expect(saw_child_task);

    // Topological invariant: ancestor entries come before descendant entries.
    // The anchor (entries[0]) must appear before the child plan.
    var anchor_pos: usize = 0;
    var child_pos: usize = 0;
    for (entries, 0..) |entry, i| {
        if (entry.id == anchor.id and std.mem.eql(u8, entry.role, "anchor")) anchor_pos = i;
        if (entry.id == child.id and std.mem.eql(u8, entry.kind, "plan")) child_pos = i;
    }
    try std.testing.expect(anchor_pos < child_pos);
}

test "[empty] plan descendants on a leaf plan returns an empty subtree" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Create a leaf plan (no children, no tasks).
    const leaf = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Leaf plan",
    });
    const leaf_id_s = try std.fmt.allocPrint(arena, "{d}", .{leaf.id});

    const raw = suite.mustRun(&.{ "plan", "descendants", "--json", leaf_id_s });
    defer gpa.free(raw);

    const parsed = std.json.parseFromSlice([]DescendantEntry, arena, raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("plan descendants (empty) JSON parse failed: {s}\nraw: {s}\n", .{ @errorName(e), raw });
        try std.testing.expect(false);
        unreachable;
    };
    const entries = parsed.value;

    // Only the anchor itself is returned; no children.
    try std.testing.expectEqual(@as(usize, 1), entries.len);
    try std.testing.expectEqualStrings("anchor", entries[0].role);
    try std.testing.expectEqual(leaf.id, entries[0].id);
}

test "[error] plan descendants on a non-existent plan returns not-found" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const stderr = suite.expectFailure(&.{ "plan", "descendants", "--json", "999999" });
    defer gpa.free(stderr);
    try std.testing.expect(stderr.len > 0);
}
