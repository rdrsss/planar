//! engine/ingestor/coverage — `--strict` gate semantics.
//!
//! Coverage measures how many tasks in a Diff are verified by at least
//! one test-spec scenario. Slug-form `**Verifies:** task:<slug>` is the
//! citation chain — numeric refs are ignored for coverage purposes
//! because at preview time we cannot tie a numeric id to a roadmap
//! bullet, and at apply time the task may not yet exist.
//!
//! The gate is informational by default. Passing `--strict` on
//! `planar spec ingest` promotes uncovered tasks and orphan scenarios
//! (scenarios whose `**Verifies:**` line is missing or fails to parse)
//! from a printed warning into a non-zero exit.

const std = @import("std");
const diff_mod = @import("diff.zig");

/// Coverage summarises test-spec coverage of the tasks in a Diff. The
/// slices are owned by the allocator passed to `compute`.
pub const Coverage = struct {
    total_tasks: usize,
    tasks_with_slug: usize,
    tasks_without_slug: usize,
    uncovered_task_slugs: []const []const u8,
    orphan_scenarios: []const []const u8,

    pub fn hasGaps(self: Coverage) bool {
        return self.uncovered_task_slugs.len > 0 or self.orphan_scenarios.len > 0;
    }
};

pub fn deinitCoverage(c: Coverage, allocator: std.mem.Allocator) void {
    for (c.uncovered_task_slugs) |s| allocator.free(s);
    allocator.free(c.uncovered_task_slugs);
    for (c.orphan_scenarios) |s| allocator.free(s);
    allocator.free(c.orphan_scenarios);
}

/// Compute coverage for the diff. Mirrors Go's `ComputeCoverage`.
pub fn compute(
    allocator: std.mem.Allocator,
    d: diff_mod.Diff,
) std.mem.Allocator.Error!Coverage {
    // Collect slugs cited by scenarios.
    var cited = std.StringHashMap(void).init(allocator);
    defer cited.deinit();
    for (d.scenarios) |sc| {
        for (sc.verifies) |r| {
            if (r.slug.len > 0) try cited.put(r.slug, {});
        }
    }

    var total: usize = 0;
    var with_slug: usize = 0;
    var without_slug: usize = 0;
    var uncovered = std.StringHashMap(void).init(allocator);
    defer uncovered.deinit();

    for (d.child_plans) |cp| {
        for (cp.tasks) |t| {
            if (t.op != .add and t.op != .update) continue;
            total += 1;
            if (t.slug.len == 0) {
                without_slug += 1;
                continue;
            }
            with_slug += 1;
            if (!cited.contains(t.slug)) try uncovered.put(t.slug, {});
        }
    }

    // Orphan scenarios: zero Verifies entries.
    var orphan_buf: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (orphan_buf.items) |s| allocator.free(s);
        orphan_buf.deinit(allocator);
    }
    for (d.scenarios) |sc| {
        if (sc.verifies.len == 0) try orphan_buf.append(allocator, try allocator.dupe(u8, sc.title));
    }
    const orphans_sorted = try orphan_buf.toOwnedSlice(allocator);
    sortInPlace(orphans_sorted);

    var uncovered_buf: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (uncovered_buf.items) |s| allocator.free(s);
        uncovered_buf.deinit(allocator);
    }
    var it = uncovered.iterator();
    while (it.next()) |e| {
        try uncovered_buf.append(allocator, try allocator.dupe(u8, e.key_ptr.*));
    }
    const uncovered_sorted = try uncovered_buf.toOwnedSlice(allocator);
    sortInPlace(uncovered_sorted);

    return .{
        .total_tasks = total,
        .tasks_with_slug = with_slug,
        .tasks_without_slug = without_slug,
        .uncovered_task_slugs = uncovered_sorted,
        .orphan_scenarios = orphans_sorted,
    };
}

fn sortInPlace(items: [][]const u8) void {
    std.sort.block([]const u8, items, {}, struct {
        fn lt(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.lessThan(u8, a, b);
        }
    }.lt);
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

test "hasGaps: empty coverage is gap-free" {
    const c = Coverage{
        .total_tasks = 0,
        .tasks_with_slug = 0,
        .tasks_without_slug = 0,
        .uncovered_task_slugs = &.{},
        .orphan_scenarios = &.{},
    };
    try testing.expect(!c.hasGaps());
}

test "hasGaps: non-empty uncovered triggers" {
    const c = Coverage{
        .total_tasks = 1,
        .tasks_with_slug = 1,
        .tasks_without_slug = 0,
        .uncovered_task_slugs = &.{"foo"},
        .orphan_scenarios = &.{},
    };
    try testing.expect(c.hasGaps());
}

test "compute: counts tasks-with/without-slug correctly" {
    const a = testing.allocator;
    // Hand-build a Diff with two child plans, mixed slugs.
    var tasks_a = try a.alloc(diff_mod.TaskEntry, 2);
    tasks_a[0] = .{
        .op = .add,
        .title = try a.dupe(u8, "a"),
        .body = try a.dupe(u8, ""),
        .touches = &.{},
        .slug = try a.dupe(u8, "task-a"),
        .existing_id = 0,
        .child_plan_title = try a.dupe(u8, ""),
    };
    tasks_a[1] = .{
        .op = .add,
        .title = try a.dupe(u8, "b"),
        .body = try a.dupe(u8, ""),
        .touches = &.{},
        .slug = try a.dupe(u8, ""),
        .existing_id = 0,
        .child_plan_title = try a.dupe(u8, ""),
    };
    var plans = try a.alloc(diff_mod.PlanEntry, 1);
    plans[0] = .{
        .op = .add,
        .title = try a.dupe(u8, "M1"),
        .existing_id = 0,
        .tasks = tasks_a,
    };
    const d = diff_mod.Diff{
        .anchor_plan_id = 1,
        .anchor_slug = try a.dupe(u8, "x"),
        .assoc_slug = try a.dupe(u8, ""),
        .current_status = try a.dupe(u8, "draft"),
        .child_plans = plans,
    };
    defer diff_mod.deinitDiff(d, a);

    const cov = try compute(a, d);
    defer deinitCoverage(cov, a);
    try testing.expectEqual(@as(usize, 2), cov.total_tasks);
    try testing.expectEqual(@as(usize, 1), cov.tasks_with_slug);
    try testing.expectEqual(@as(usize, 1), cov.tasks_without_slug);
    try testing.expectEqual(@as(usize, 1), cov.uncovered_task_slugs.len);
    try testing.expectEqualStrings("task-a", cov.uncovered_task_slugs[0]);
}
