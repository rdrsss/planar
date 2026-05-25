//! engine/health — database + runtime-health introspection.
//!
//! Returns a `Report` describing the state of the local Planar store.
//! Goes alongside the `planar health` CLI verb but is independent of it
//! — integration tests and future read-side viewers can call `check`
//! directly.

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
    schema_version: i64,
    migration_count: i64,
    handoffs_pending: i64,
    db_ok: bool,
};

/// Snapshot the current health of the supplied database. Pure read:
/// issues a handful of single-row queries and assembles the struct.
/// No IO, no formatting — the caller emits the result.
pub fn check(d: *db.sqlite.Db) Error!Report {
    const schema_version = d.intQuery(
        "select coalesce(max(version), 0) from schema_migrations",
    ) catch return Error.SchemaTableMissing;

    const migration_count = d.intQuery(
        "select count(*) from schema_migrations",
    ) catch return Error.QueryFailed;

    // Handoffs table may not exist yet on early-migration installs.
    // Treat that as "0 pending" rather than erroring — the schema_version
    // field already tells the operator whether the install is current.
    const handoffs_pending = d.intQuery(
        "select count(*) from handoffs where status in ('pending', 'validated')",
    ) catch 0;

    return .{
        .schema_version = schema_version,
        .migration_count = migration_count,
        .handoffs_pending = handoffs_pending,
        .db_ok = true,
    };
}

/// Render the report as a multi-line plain-text block. The CLI handler
/// calls this for non-JSON mode; tests can also call it to assert on
/// the rendered shape.
pub fn renderText(report: Report, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("schema version:   {d}\n", .{report.schema_version});
    try writer.print("migrations applied: {d}\n", .{report.migration_count});
    try writer.print("handoffs pending: {d}\n", .{report.handoffs_pending});
    try writer.print("db status:        {s}\n", .{if (report.db_ok) "ok" else "degraded"});
}

// ---- tests ----

test "check returns zero values against a freshly-opened in-memory DB" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    // No schema yet — check should report SchemaTableMissing.
    try std.testing.expectError(Error.SchemaTableMissing, check(&d));
}

test "check reports schema version after a migration row exists" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try d.exec(
        \\create table schema_migrations (version integer primary key, description text);
        \\insert into schema_migrations (version, description) values (3, 'test');
    );
    const report = try check(&d);
    try std.testing.expectEqual(@as(i64, 3), report.schema_version);
    try std.testing.expectEqual(@as(i64, 1), report.migration_count);
    try std.testing.expectEqual(@as(i64, 0), report.handoffs_pending);
    try std.testing.expectEqual(true, report.db_ok);
}
