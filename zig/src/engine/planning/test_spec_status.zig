//! engine/planning/test_spec_status — `planar test-spec status` compute.
//!
//! Mirrors Go's `computeTestSpecStatus` in
//! `src/cmd/planar/internal/planning/test_spec.go`. The verb is the
//! human-facing complement to the `spec ingest --strict` coverage gate:
//! at ingest time, `engine/ingestor/coverage.zig` checks the parsed
//! Diff; here we walk the live DB to surface per-milestone coverage
//! AFTER ingest has run.
//!
//! Coverage classification rules (taken verbatim from Go):
//!   * For an anchor plan `A`, "milestones" = `A` itself + every plan
//!     P where there's a `plan -> plan derives-from` edge from P to A.
//!   * Scenarios attached to `A` are walked via
//!     `test_scenario -> plan derives-from` edges.
//!   * A task is "covered" iff some attached scenario has a
//!     `test_scenario -> task verifies` edge pointing at the task.
//!   * Each covered task's bucket counts are aggregated from the
//!     scenarios that verify it; buckets are derived from the
//!     scenario title prefix via `classifyBucket`.

const std = @import("std");
const db = @import("db");

/// MilestoneStatus is one row of the per-milestone coverage report.
/// Field order matches Go's `milestoneStatus` so both binaries emit
/// compatible NDJSON.
pub const MilestoneStatus = struct {
    plan_id: i64,
    title: []const u8,
    total_tasks: i64,
    tasks_with_slug: i64,
    tasks_covered: i64,
    happy: i64,
    empty: i64,
    @"error": i64,
    edge: i64,
    other: i64,
};

/// PlanSummary is the trailing roll-up across all milestones.
pub const PlanSummary = struct {
    anchor_plan_id: i64,
    total_tasks: i64,
    tasks_with_slug: i64,
    tasks_covered: i64,
    total_scenarios: i64,
};

/// Status is the value returned by `compute` — owned by the allocator
/// passed in; release with `deinit`.
pub const Status = struct {
    milestones: []MilestoneStatus,
    summary: PlanSummary,
};

pub fn deinit(s: Status, allocator: std.mem.Allocator) void {
    for (s.milestones) |m| allocator.free(m.title);
    allocator.free(s.milestones);
}

pub const Error = error{
    OutOfMemory,
    QueryFailed,
};

/// compute walks `anchor_plan_id`'s milestones, scenarios, and the
/// `verifies` edges between them. Returns a `Status` owned by the
/// caller (free with `deinit`). The slice ordering is `(plan_id asc,
/// title asc)`.
pub fn compute(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
) Error!Status {
    // -- step 1: list anchor + child plans --
    var plan_rows: std.ArrayList(PlanRow) = .empty;
    errdefer {
        for (plan_rows.items) |p| allocator.free(p.title);
        plan_rows.deinit(allocator);
    }
    {
        var stmt = d.prepare(
            \\select p.id, p.title
            \\  from plans p
            \\ where p.id = ?
            \\    or p.id in (
            \\      select from_id from entity_links
            \\       where from_kind = 'plan'
            \\         and to_kind = 'plan'
            \\         and to_id = ?
            \\         and relationship = 'derives-from'
            \\    )
            \\ order by p.id
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = anchor_plan_id } }) catch return Error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = stmt.columnTextAlloc(1, allocator) catch return Error.OutOfMemory;
                    plan_rows.append(allocator, .{ .id = id, .title = title }) catch {
                        allocator.free(title);
                        return Error.OutOfMemory;
                    };
                },
            }
        }
    }

    // -- step 2: list scenarios attached to anchor + their buckets --
    var scenarios: std.ArrayList(ScenarioRow) = .empty;
    errdefer {
        for (scenarios.items) |s| allocator.free(s.title);
        scenarios.deinit(allocator);
    }
    {
        var stmt = d.prepare(
            \\select s.id, s.title from test_scenarios s
            \\ where s.id in (
            \\   select from_id from entity_links
            \\    where from_kind = 'test_scenario'
            \\      and to_kind = 'plan'
            \\      and to_id = ?
            \\      and relationship = 'derives-from'
            \\ )
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return Error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => break,
                .row => {
                    const id = stmt.columnInt(0);
                    const title = stmt.columnTextAlloc(1, allocator) catch return Error.OutOfMemory;
                    scenarios.append(allocator, .{ .id = id, .title = title }) catch {
                        allocator.free(title);
                        return Error.OutOfMemory;
                    };
                },
            }
        }
    }

    // -- step 3: tasksCovered + per-task bucket counts --
    var tasks_covered: std.AutoHashMap(i64, void) = .init(allocator);
    defer tasks_covered.deinit();
    // task_id -> TaskBuckets struct of 5 counters.
    var task_buckets: std.AutoHashMap(i64, TaskBuckets) = .init(allocator);
    defer task_buckets.deinit();

    for (scenarios.items) |sc| {
        const bucket = classifyBucket(sc.title);
        var v_stmt = d.prepare(
            \\select to_id from entity_links
            \\ where from_kind = 'test_scenario'
            \\   and from_id = ?
            \\   and to_kind = 'task'
            \\   and relationship = 'verifies'
        ) catch return Error.QueryFailed;
        defer v_stmt.finalize();
        v_stmt.bind(&.{.{ .int = sc.id }}) catch return Error.QueryFailed;
        while (true) {
            switch (v_stmt.step() catch return Error.QueryFailed) {
                .done => break,
                .row => {
                    const task_id = v_stmt.columnInt(0);
                    tasks_covered.put(task_id, {}) catch return Error.OutOfMemory;
                    const gop = task_buckets.getOrPut(task_id) catch return Error.OutOfMemory;
                    if (!gop.found_existing) gop.value_ptr.* = .{};
                    gop.value_ptr.add(bucket, 1);
                },
            }
        }
    }

    // -- step 4: per-milestone task aggregates + summary --
    var milestones_out = allocator.alloc(MilestoneStatus, plan_rows.items.len) catch return Error.OutOfMemory;
    errdefer allocator.free(milestones_out);
    var milestones_filled: usize = 0;
    errdefer for (milestones_out[0..milestones_filled]) |m| allocator.free(m.title);

    var summary = PlanSummary{
        .anchor_plan_id = anchor_plan_id,
        .total_tasks = 0,
        .tasks_with_slug = 0,
        .tasks_covered = 0,
        .total_scenarios = @intCast(scenarios.items.len),
    };

    for (plan_rows.items) |pr| {
        var ms = MilestoneStatus{
            .plan_id = pr.id,
            .title = pr.title, // ownership transferred from plan_rows
            .total_tasks = 0,
            .tasks_with_slug = 0,
            .tasks_covered = 0,
            .happy = 0,
            .empty = 0,
            .@"error" = 0,
            .edge = 0,
            .other = 0,
        };
        var t_stmt = d.prepare("select id, slug from tasks where plan_id = ?") catch {
            // free the title we just adopted before returning.
            allocator.free(pr.title);
            return Error.QueryFailed;
        };
        defer t_stmt.finalize();
        t_stmt.bind(&.{.{ .int = pr.id }}) catch {
            allocator.free(pr.title);
            return Error.QueryFailed;
        };
        while (true) {
            switch (t_stmt.step() catch {
                allocator.free(pr.title);
                return Error.QueryFailed;
            }) {
                .done => break,
                .row => {
                    const tid = t_stmt.columnInt(0);
                    const slug_opt = t_stmt.columnTextOpt(1, allocator) catch {
                        allocator.free(pr.title);
                        return Error.OutOfMemory;
                    };
                    defer if (slug_opt) |s| allocator.free(s);
                    ms.total_tasks += 1;
                    if (slug_opt) |s| if (s.len > 0) {
                        ms.tasks_with_slug += 1;
                    };
                    if (tasks_covered.contains(tid)) {
                        ms.tasks_covered += 1;
                        if (task_buckets.get(tid)) |bk| {
                            ms.happy += bk.happy;
                            ms.empty += bk.empty;
                            ms.@"error" += bk.@"error";
                            ms.edge += bk.edge;
                            ms.other += bk.other;
                        }
                    }
                },
            }
        }
        milestones_out[milestones_filled] = ms;
        milestones_filled += 1;
        summary.total_tasks += ms.total_tasks;
        summary.tasks_with_slug += ms.tasks_with_slug;
        summary.tasks_covered += ms.tasks_covered;
    }

    // ownership transferred — clear the title pointers without freeing
    // since milestones_out now owns them.
    plan_rows.deinit(allocator);

    // scenarios titles were dupes used only inside compute; release them.
    for (scenarios.items) |sc| allocator.free(sc.title);
    scenarios.deinit(allocator);

    // Stable ordering (plan id ascending, then title).
    std.sort.block(MilestoneStatus, milestones_out, {}, struct {
        fn lt(_: void, a: MilestoneStatus, b: MilestoneStatus) bool {
            if (a.plan_id != b.plan_id) return a.plan_id < b.plan_id;
            return std.mem.lessThan(u8, a.title, b.title);
        }
    }.lt);

    return .{ .milestones = milestones_out, .summary = summary };
}

const PlanRow = struct { id: i64, title: []const u8 };
const ScenarioRow = struct { id: i64, title: []const u8 };

const Bucket = enum { happy, empty, @"error", edge, other };

const TaskBuckets = struct {
    happy: i64 = 0,
    empty: i64 = 0,
    @"error": i64 = 0,
    edge: i64 = 0,
    other: i64 = 0,

    fn add(self: *TaskBuckets, b: Bucket, n: i64) void {
        switch (b) {
            .happy => self.happy += n,
            .empty => self.empty += n,
            .@"error" => self.@"error" += n,
            .edge => self.edge += n,
            .other => self.other += n,
        }
    }
};

/// classifyBucket maps a scenario title prefix to one of five
/// return-path buckets. Mirrors Go's `classifyBucket`: the prefixes are
/// the canonical bucket headings from the test-spec template.
pub fn classifyBucket(title: []const u8) Bucket {
    const trimmed = std.mem.trim(u8, title, " \t");
    // Match case-insensitively without allocating: compare lowercased
    // prefix bytes against fixed candidates.
    if (startsWithCi(trimmed, "happy path")) return .happy;
    if (startsWithCi(trimmed, "empty / null") or
        startsWithCi(trimmed, "empty") or
        startsWithCi(trimmed, "null")) return .empty;
    if (startsWithCi(trimmed, "error")) return .@"error";
    if (startsWithCi(trimmed, "edge")) return .edge;
    return .other;
}

fn startsWithCi(s: []const u8, prefix: []const u8) bool {
    if (s.len < prefix.len) return false;
    for (prefix, 0..) |c, i| {
        if (std.ascii.toLower(s[i]) != std.ascii.toLower(c)) return false;
    }
    return true;
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

test "classifyBucket: prefix matches" {
    try testing.expectEqual(Bucket.happy, classifyBucket("Happy path — basic"));
    try testing.expectEqual(Bucket.empty, classifyBucket("Empty / null return"));
    try testing.expectEqual(Bucket.empty, classifyBucket("empty input"));
    try testing.expectEqual(Bucket.empty, classifyBucket("null input"));
    try testing.expectEqual(Bucket.@"error", classifyBucket("Error return on bad input"));
    try testing.expectEqual(Bucket.edge, classifyBucket("Edge case: overflow"));
    try testing.expectEqual(Bucket.other, classifyBucket("Random other thing"));
}

test "classifyBucket: leading whitespace trimmed" {
    try testing.expectEqual(Bucket.happy, classifyBucket("   Happy path"));
}
