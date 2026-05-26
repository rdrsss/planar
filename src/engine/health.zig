//! engine/health — database + runtime-health introspection.
//!
//! Returns a `Report` describing the state of the local Planar store.
//! Goes alongside the `planar health` CLI verb but is independent of it
//! — integration tests and future read-side viewers can call `check`
//! directly.
//!
//! Cluster C-health-content-loss (plan 351, Q235): mirrors Go's rich
//! field set. Each in-flight task (status in {doing, blocked}) is
//! classified resumable vs not (needs non-empty next_action + at least
//! one context_snapshot row), pending handoffs are bucketed into
//! stale vs fresh, the schema-migration ledger is compared against
//! embedded_max, and a final `overall` classification ("ok" /
//! "degraded") rolls everything up. The CLI handler exits 1 on
//! "degraded" so oncall / CI scrapes get a meaningful non-zero.

const std = @import("std");
const db = @import("db");

pub const Error = error{
    SchemaTableMissing,
    QueryFailed,
};

/// Snapshot of database + handoff health. Field order is the wire
/// format — `std.json.stringify` picks it up directly when the handler
/// runs in `--json` mode, and the text renderer prints fields in
/// declaration order.
pub const Report = struct {
    db_path: []const u8,
    db_ok: bool,
    schema_version: i64,
    schema_target: i64,
    schema_current: bool,
    migration_count: i64,
    integrity_ok: bool,
    inflight_tasks: i64,
    resumable_tasks: i64,
    not_resumable_tasks: i64,
    pending_handoffs: i64,
    stale_handoffs: i64,
    overall: []const u8,
};

/// Stale-handoff cutoff. A pending / validated handoff older than this
/// is flagged in `stale_handoffs`. 24 hours mirrors the Go default.
pub const stale_handoff_threshold_hours: i64 = 24;

/// Snapshot the current health of the supplied database. Pure read:
/// issues single-row queries and assembles the struct. No IO, no
/// formatting — the caller emits the result.
///
/// `db_path` is borrowed from the runtime context for the duration of
/// the report; it is not freed by the engine.
pub fn check(d: *db.sqlite.Db, db_path: []const u8) Error!Report {
    const schema_version = d.intQuery(
        "select coalesce(max(version), 0) from schema_migrations",
    ) catch return Error.SchemaTableMissing;

    const migration_count = d.intQuery(
        "select count(*) from schema_migrations",
    ) catch return Error.QueryFailed;

    const schema_target: i64 = @intCast(db.migrate.embedded_max);

    const integrity_ok = checkIntegrity(d);

    // In-flight tasks (status in {doing, blocked}) bucketed into
    // resumable vs not-resumable. Resumable requires a non-empty
    // next_action and at least one context_snapshot row.
    const inflight_tasks = d.intQuery(
        "select count(*) from tasks where status in ('doing','blocked')",
    ) catch 0;

    const resumable_tasks = d.intQuery(
        "select count(*) from tasks t" ++
            " where t.status in ('doing','blocked')" ++
            "   and coalesce(t.next_action,'') != ''" ++
            "   and exists (select 1 from context_snapshots cs where cs.task_id = t.id)",
    ) catch 0;

    const not_resumable_tasks = inflight_tasks - resumable_tasks;

    const pending_handoffs = d.intQuery(
        "select count(*) from handoffs where status in ('pending', 'validated')",
    ) catch 0;

    var stale_buf: [256]u8 = undefined;
    const stale_sql = std.fmt.bufPrintZ(
        &stale_buf,
        "select count(*) from handoffs" ++
            " where status in ('pending','validated')" ++
            "   and (julianday('now') - julianday(created_at)) * 24 > {d}",
        .{stale_handoff_threshold_hours},
    ) catch return Error.QueryFailed;
    const stale_handoffs = d.intQuery(stale_sql) catch 0;

    const db_ok = true;
    const schema_current = schema_version == schema_target;
    const degraded = !db_ok or !integrity_ok or not_resumable_tasks > 0 or stale_handoffs > 0;

    return .{
        .db_path = db_path,
        .db_ok = db_ok,
        .schema_version = schema_version,
        .schema_target = schema_target,
        .schema_current = schema_current,
        .migration_count = migration_count,
        .integrity_ok = integrity_ok,
        .inflight_tasks = inflight_tasks,
        .resumable_tasks = resumable_tasks,
        .not_resumable_tasks = not_resumable_tasks,
        .pending_handoffs = pending_handoffs,
        .stale_handoffs = stale_handoffs,
        .overall = if (degraded) "degraded" else "ok",
    };
}

/// Run `PRAGMA integrity_check` and return true when SQLite reports
/// the single "ok" row. Any other outcome (locked DB, corruption,
/// errors) is treated as a fail-safe `false`.
fn checkIntegrity(d: *db.sqlite.Db) bool {
    var stmt = d.prepare("PRAGMA integrity_check") catch return false;
    defer stmt.finalize();
    const step_result = stmt.step() catch return false;
    switch (step_result) {
        .done => return false,
        .row => {
            var name_buf: [64]u8 = undefined;
            var fba = std.heap.FixedBufferAllocator.init(&name_buf);
            const txt = stmt.columnTextAlloc(0, fba.allocator()) catch return false;
            return std.mem.eql(u8, txt, "ok");
        },
    }
}

/// Render the report as a multi-line plain-text block. The CLI handler
/// calls this for non-JSON mode; tests can also call it to assert on
/// the rendered shape. Label set mirrors Go's `planar health` text
/// output.
pub fn renderText(report: Report, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("db:               {s} ({s})\n", .{
        if (report.db_ok) "ok" else "ERROR",
        report.db_path,
    });
    try writer.print("schema:           v{d} of v{d} ({s})\n", .{
        report.schema_version,
        report.schema_target,
        if (report.schema_current) "current" else "behind",
    });
    try writer.print("integrity:        {s}\n", .{
        if (report.integrity_ok) "ok" else "FAIL",
    });
    try writer.print("in-flight tasks:  {d} ({d} resumable, {d} NOT resumable)\n", .{
        report.inflight_tasks,
        report.resumable_tasks,
        report.not_resumable_tasks,
    });
    try writer.print("pending handoffs: {d} ({d} stale > {d}h)\n", .{
        report.pending_handoffs,
        report.stale_handoffs,
        stale_handoff_threshold_hours,
    });
    try writer.print("overall:          {s}\n", .{report.overall});
}

// ---- tests ----

test "check returns SchemaTableMissing against a freshly-opened in-memory DB" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try std.testing.expectError(Error.SchemaTableMissing, check(&d, "/tmp/test.db"));
}

test "check reports the rich field set after migrations are applied" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);
    const report = try check(&d, "/tmp/test.db");
    try std.testing.expect(report.db_ok);
    try std.testing.expect(report.integrity_ok);
    try std.testing.expectEqualStrings("ok", report.overall);
    try std.testing.expectEqual(@as(i64, 0), report.inflight_tasks);
    try std.testing.expectEqual(@as(i64, 0), report.not_resumable_tasks);
}

test "check classifies a doing task with no next_action as not-resumable + degraded" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'doing', 100)",
        &.{},
    );
    const report = try check(&d, "/tmp/test.db");
    try std.testing.expectEqual(@as(i64, 1), report.inflight_tasks);
    try std.testing.expectEqual(@as(i64, 1), report.not_resumable_tasks);
    try std.testing.expectEqualStrings("degraded", report.overall);
}
