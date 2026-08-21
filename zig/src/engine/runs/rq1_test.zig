//! src/engine/runs/rq1_test.zig — fixture proof for the frozen RQ1
//! touch-accuracy metric definitions (preregistration §2, plan 635).
//!
//! WHAT THIS TEST DOES
//! ===================
//! 1. Builds a fully-migrated in-memory database.
//! 2. Seeds a hand-constructed `run_touches` fixture that exercises every
//!    branch of preregistration §2's edge-case rules:
//!
//!      Task A (ID 1001) — normal, partial overlap
//!        declared: {α, β}   actual: {β, γ}
//!        hit = {β}  → precision = 1/2 = 0.5, recall = 1/2 = 0.5
//!
//!      Task B (ID 1002) — undeclared (|declared| = 0)
//!        declared: {}   actual: {δ}
//!        → precision UNDEFINED (excluded; counted as undeclared_count)
//!        → recall = 0/1 = 0.0
//!
//!      Task C (ID 1003) — no-actual (|actual| = 0)
//!        declared: {ε}   actual: {}
//!        → precision = 0/1 = 0.0
//!        → recall UNDEFINED (excluded; counted as no_actual_count)
//!
//! 3. Reads metrics/rq1_touch_accuracy.sql via the `metrics_sql` build
//!    module (which embeds the file at compile time; metrics_sql.zig is the
//!    build bridge — the SQL files themselves are the analyst-facing source
//!    of truth). Executes both queries against the fixture, substituting
//!    the :run named parameter with the actual run_uid bound as '?'.
//!
//! 4. Asserts per-task results AND the aggregate macro-average AND the
//!    undeclared / no-actual counts match the hand-computed expected values.
//!
//! HAND-COMPUTED EXPECTED VALUES
//! =============================
//! Per preregistration §2 frozen rules:
//!
//!   precision macro-avg: avg over tasks with |declared| > 0
//!     = avg(precision_A, precision_C) = avg(0.5, 0.0) = 0.25
//!   recall macro-avg: avg over tasks with |actual| > 0
//!     = avg(recall_A, recall_B) = avg(0.5, 0.0) = 0.25
//!   undeclared_count = 1  (task B)
//!   no_actual_count  = 1  (task C)
//!   precision_task_count = 2 (A and C)
//!   recall_task_count    = 2 (A and B)
//!   total_task_count     = 3

const std = @import("std");
const db = @import("db");
const metrics_sql = @import("metrics_sql");

// The SQL file content, embedded at build time via metrics/metrics_sql.zig.
const rq1_sql_raw: []const u8 = metrics_sql.rq1_touch_accuracy;

// ---------------------------------------------------------------------------
// The separator between Query 1 and Query 2 in rq1_touch_accuracy.sql.
// ---------------------------------------------------------------------------
const query2_marker = "\n\n-- ---------------------------------------------------------------------------\n-- QUERY 2:";

// ---------------------------------------------------------------------------
// Split the raw SQL into two queries at runtime.
// Returns {query1, query2} or panics if the separator is not found
// (which would mean the SQL file structure changed without updating this marker).
// ---------------------------------------------------------------------------
fn splitQueries(raw: []const u8) struct { q1: []const u8, q2: []const u8 } {
    const idx = std.mem.indexOf(u8, raw, query2_marker) orelse
        @panic("QUERY 2 separator not found in rq1_touch_accuracy.sql — SQL file structure changed");
    return .{ .q1 = raw[0..idx], .q2 = raw[idx..] };
}

// ---------------------------------------------------------------------------
// Runtime helper: replace ':run' with '?' for the sqlite.Stmt prepare API.
// Returns a newly allocated sentinel-terminated string owned by `allocator`.
// ---------------------------------------------------------------------------
fn replaceRunParam(allocator: std.mem.Allocator, sql: []const u8) ![:0]u8 {
    const replaced = try std.mem.replaceOwned(u8, allocator, sql, ":run", "?");
    defer allocator.free(replaced);
    return allocator.dupeZ(u8, replaced);
}

// ---------------------------------------------------------------------------
// DB setup.
// ---------------------------------------------------------------------------

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

fn seedPlan(d: *db.sqlite.Db, slug: []const u8) !i64 {
    return d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', ?, ?, 'draft')",
        &.{ .{ .text = slug }, .{ .text = slug } },
    );
}

// ---------------------------------------------------------------------------
// Per-task row returned by Query 1.
// ---------------------------------------------------------------------------
const PerTaskRow = struct {
    task_id: i64,
    declared_count: i64,
    actual_count: i64,
    hit_count: i64,
    precision: ?f64,
    recall: ?f64,
};

// ---------------------------------------------------------------------------
// Aggregate row returned by Query 2.
// ---------------------------------------------------------------------------
const AggRow = struct {
    macro_avg_precision: ?f64,
    macro_avg_recall: ?f64,
    undeclared_count: i64,
    no_actual_count: i64,
    precision_task_count: i64,
    recall_task_count: i64,
    total_task_count: i64,
};

// ---------------------------------------------------------------------------
// The fixture test.
// ---------------------------------------------------------------------------

test "rq1_touch_accuracy.sql: per-task and aggregate results match hand-computed §2 values" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // --- Seed plan + run ---
    const plan_id = try seedPlan(&d, "rq1-test-plan");

    // Insert a runs row directly; plan_id FK must be valid.
    const run_id = try d.execParams(
        \\insert into runs (run_uid, plan_id, arm, base_sha, config_hash, status)
        \\values (?, ?, 'strict', 'deadbeef', 'cfg-test', 'completed')
    , &.{ .{ .text = "rq1-test-uid-001" }, .{ .int = plan_id } });

    // Task IDs (plain integers, no FK constraint on run_touches.task_id).
    const task_a: i64 = 1001;
    const task_b: i64 = 1002; // undeclared (|declared|=0)
    const task_c: i64 = 1003; // no-actual  (|actual|=0)

    // Helper to insert a touch row.
    const ins = struct {
        fn do(dd: *db.sqlite.Db, rid: i64, tid: i64, path: []const u8, kind: []const u8) void {
            _ = dd.execParams(
                \\insert into run_touches (run_id, task_id, path, kind)
                \\values (?, ?, ?, ?)
            , &.{
                .{ .int = rid },
                .{ .int = tid },
                .{ .text = path },
                .{ .text = kind },
            }) catch @panic("insert run_touches failed");
        }
    };

    // Task A: declared {α, β}, actual {β, γ}
    ins.do(&d, run_id, task_a, "src/alpha.zig", "declared");
    ins.do(&d, run_id, task_a, "src/beta.zig", "declared");
    ins.do(&d, run_id, task_a, "src/beta.zig", "actual");
    ins.do(&d, run_id, task_a, "src/gamma.zig", "actual");

    // Task B: no declared rows, actual {δ} — undeclared case (§2: |declared|=0)
    ins.do(&d, run_id, task_b, "src/delta.zig", "actual");

    // Task C: declared {ε}, no actual rows — no-actual case (§2: |actual|=0)
    ins.do(&d, run_id, task_c, "src/epsilon.zig", "declared");

    // --- Split and execute the two queries from rq1_touch_accuracy.sql ---
    const queries = splitQueries(rq1_sql_raw);

    const q1 = try replaceRunParam(a, queries.q1);
    defer a.free(q1);

    var stmt1 = d.prepare(q1) catch |err| {
        std.debug.print("Query 1 prepare failed: {s}\n", .{@errorName(err)});
        return err;
    };
    defer stmt1.finalize();
    // Query 1 has two active ':run' parameters (one in the 'd' CTE and one
    // in the 'a' CTE); both are replaced with '?' and must be bound to the
    // same run_uid. Comments also contain ':run' text but SQLite ignores '?'
    // inside comments when counting parameter slots.
    stmt1.bind(&.{
        .{ .text = "rq1-test-uid-001" },
        .{ .text = "rq1-test-uid-001" },
    }) catch |err| {
        std.debug.print("Query 1 bind failed: {s}\n", .{@errorName(err)});
        return err;
    };

    var rows: std.ArrayList(PerTaskRow) = .empty;
    defer rows.deinit(a);

    while (true) {
        const step_result = stmt1.step() catch |err| {
            std.debug.print("Query 1 step failed: {s}\n", .{@errorName(err)});
            return err;
        };
        if (step_result == .done) break;
        // Columns: task_id(0) declared_count(1) actual_count(2) hit_count(3) precision(4) recall(5)
        try rows.append(a, .{
            .task_id = stmt1.columnInt(0),
            .declared_count = stmt1.columnInt(1),
            .actual_count = stmt1.columnInt(2),
            .hit_count = stmt1.columnInt(3),
            .precision = if (stmt1.columnIsNull(4)) null else stmt1.columnDouble(4),
            .recall = if (stmt1.columnIsNull(5)) null else stmt1.columnDouble(5),
        });
    }

    // Expect exactly 3 rows (one per task), ordered by task_id.
    try std.testing.expectEqual(@as(usize, 3), rows.items.len);

    // Find each task's row by id.
    var row_a: ?PerTaskRow = null;
    var row_b: ?PerTaskRow = null;
    var row_c: ?PerTaskRow = null;
    for (rows.items) |r| {
        if (r.task_id == task_a) row_a = r;
        if (r.task_id == task_b) row_b = r;
        if (r.task_id == task_c) row_c = r;
    }
    try std.testing.expect(row_a != null);
    try std.testing.expect(row_b != null);
    try std.testing.expect(row_c != null);

    // --- Assert Task A: declared {α,β}, actual {β,γ} → hit={β} ---
    // precision = |{β}| / |{α,β}| = 1/2 = 0.5
    // recall    = |{β}| / |{β,γ}| = 1/2 = 0.5
    {
        const r = row_a.?;
        try std.testing.expectEqual(@as(i64, 2), r.declared_count);
        try std.testing.expectEqual(@as(i64, 2), r.actual_count);
        try std.testing.expectEqual(@as(i64, 1), r.hit_count);
        try std.testing.expect(r.precision != null);
        try std.testing.expectApproxEqRel(0.5, r.precision.?, 1e-9);
        try std.testing.expect(r.recall != null);
        try std.testing.expectApproxEqRel(0.5, r.recall.?, 1e-9);
    }

    // --- Assert Task B: undeclared (|declared|=0), actual={δ} ---
    // §2: "|declared|=0 has undefined precision → EXCLUDED from precision"
    // recall = |{}| / |{δ}| = 0/1 = 0.0
    {
        const r = row_b.?;
        try std.testing.expectEqual(@as(i64, 0), r.declared_count);
        try std.testing.expectEqual(@as(i64, 1), r.actual_count);
        try std.testing.expectEqual(@as(i64, 0), r.hit_count);
        // precision MUST be NULL (undefined, excluded from aggregate)
        try std.testing.expect(r.precision == null);
        // recall = 0.0 (task has actual touches, none hit)
        try std.testing.expect(r.recall != null);
        try std.testing.expectApproxEqRel(0.0, r.recall.?, 1e-9);
    }

    // --- Assert Task C: declared={ε}, no-actual (|actual|=0) ---
    // §2: "|actual|=0 has undefined recall → EXCLUDED from recall"
    // precision = |{}| / |{ε}| = 0/1 = 0.0
    {
        const r = row_c.?;
        try std.testing.expectEqual(@as(i64, 1), r.declared_count);
        try std.testing.expectEqual(@as(i64, 0), r.actual_count);
        try std.testing.expectEqual(@as(i64, 0), r.hit_count);
        // precision = 0.0 (task has declared touches, none hit)
        try std.testing.expect(r.precision != null);
        try std.testing.expectApproxEqRel(0.0, r.precision.?, 1e-9);
        // recall MUST be NULL (undefined, excluded from aggregate)
        try std.testing.expect(r.recall == null);
    }

    // --- Execute Query 2 (aggregate macro-average + edge-case counts) ---
    const q2 = try replaceRunParam(a, queries.q2);
    defer a.free(q2);

    var stmt2 = d.prepare(q2) catch |err| {
        std.debug.print("Query 2 prepare failed: {s}\n", .{@errorName(err)});
        return err;
    };
    defer stmt2.finalize();
    // Query 2 also has two active ':run' parameters (same pattern as Query 1).
    stmt2.bind(&.{
        .{ .text = "rq1-test-uid-001" },
        .{ .text = "rq1-test-uid-001" },
    }) catch |err| {
        std.debug.print("Query 2 bind failed: {s}\n", .{@errorName(err)});
        return err;
    };

    const step2 = stmt2.step() catch |err| {
        std.debug.print("Query 2 step failed: {s}\n", .{@errorName(err)});
        return err;
    };
    try std.testing.expectEqual(.row, step2);

    // Columns: macro_avg_precision(0) macro_avg_recall(1) undeclared_count(2)
    //          no_actual_count(3) precision_task_count(4) recall_task_count(5)
    //          total_task_count(6)
    const agg = AggRow{
        .macro_avg_precision = if (stmt2.columnIsNull(0)) null else stmt2.columnDouble(0),
        .macro_avg_recall = if (stmt2.columnIsNull(1)) null else stmt2.columnDouble(1),
        .undeclared_count = stmt2.columnInt(2),
        .no_actual_count = stmt2.columnInt(3),
        .precision_task_count = stmt2.columnInt(4),
        .recall_task_count = stmt2.columnInt(5),
        .total_task_count = stmt2.columnInt(6),
    };

    // §2: macro-avg precision = avg over tasks with |declared|>0 = avg(0.5, 0.0) = 0.25
    // Included tasks: A (0.5) and C (0.0); B excluded (undeclared).
    try std.testing.expect(agg.macro_avg_precision != null);
    try std.testing.expectApproxEqRel(0.25, agg.macro_avg_precision.?, 1e-9);

    // §2: macro-avg recall = avg over tasks with |actual|>0 = avg(0.5, 0.0) = 0.25
    // Included tasks: A (0.5) and B (0.0); C excluded (no-actual).
    try std.testing.expect(agg.macro_avg_recall != null);
    try std.testing.expectApproxEqRel(0.25, agg.macro_avg_recall.?, 1e-9);

    // §2: undeclared_count = 1 (task B — "its count reported separately as undeclared")
    try std.testing.expectEqual(@as(i64, 1), agg.undeclared_count);

    // §2: no_actual_count = 1 (task C — "excluded, count reported")
    try std.testing.expectEqual(@as(i64, 1), agg.no_actual_count);

    // Bookkeeping: precision included 2 tasks (A, C); recall included 2 tasks (A, B).
    try std.testing.expectEqual(@as(i64, 2), agg.precision_task_count);
    try std.testing.expectEqual(@as(i64, 2), agg.recall_task_count);

    // Total = 3 tasks in the union of declared and actual.
    try std.testing.expectEqual(@as(i64, 3), agg.total_task_count);
}
