//! Policy: append-only audit_log record per data-plane mutation.
//!
//! Every CRUD function in the data plane calls `record(...)` at the
//! end of its execution. The audit log is the "what happened, when,
//! by whom" trail — it underpins `planar audit trail`, session capture
//! correlation, and ext-sync reconciliation.
//!
//! Stubbed: the schema lands in a follow-up migration (`audit_log`
//! table). The write logic lands when the first entity CRUD calls
//! record(). The signature is the contract — callers can wire it now
//! and the real implementation slots in transparently.

const std = @import("std");
const db = @import("db");

pub const Verb = enum {
    create,
    update,
    delete,
    status_change,
    link,
    unlink,
};

pub const EntityRef = struct {
    /// Kind as a string ("plan", "task", "question", ...) — string
    /// rather than enum so the audit log survives entity-kind additions
    /// without a schema change.
    kind: []const u8,
    id: i64,
};

pub const Record = struct {
    verb: Verb,
    entity: EntityRef,
    /// Operator / vendor-session id, when known. Null on direct CLI
    /// invocations without a session.
    actor: ?[]const u8 = null,
    /// Scope under which the mutation was performed (post-resolver).
    scope: ?[]const u8 = null,
    /// One-line human-readable summary, e.g. "set status: active → done".
    summary: ?[]const u8 = null,
};

pub const Error = error{WriteFailed};

const insert_sql: [:0]const u8 =
    \\insert into audit_log (verb, entity_kind, entity_id, actor, scope, summary)
    \\values (?, ?, ?, ?, ?, ?)
;

/// Record one audit-log entry. Writes to `audit_log` (migration 14).
/// All param values are bound positionally so caller strings never
/// participate in SQL parsing.
pub fn record(d: *db.sqlite.Db, rec: Record) Error!void {
    _ = d.execParams(insert_sql, &.{
        .{ .text = @tagName(rec.verb) },
        .{ .text = rec.entity.kind },
        .{ .int = rec.entity.id },
        if (rec.actor) |a| .{ .text = a } else .{ .null = {} },
        if (rec.scope) |s| .{ .text = s } else .{ .null = {} },
        if (rec.summary) |s| .{ .text = s } else .{ .null = {} },
    }) catch return error.WriteFailed;
}

// ---- tests ----

test "record inserts a row into audit_log" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try @import("db").migrate.applyAll(&d, std.testing.allocator);

    try record(&d, .{
        .verb = .create,
        .entity = .{ .kind = "task", .id = 42 },
        .actor = "session:99",
        .scope = "acme",
        .summary = "create task #42",
    });

    try std.testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from audit_log"));
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery(
            "select count(*) from audit_log " ++
                "where verb = 'create' and entity_kind = 'task' and entity_id = 42 " ++
                "and actor = 'session:99' and scope = 'acme'",
        ),
    );
}

test "record accepts null actor/scope/summary" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try @import("db").migrate.applyAll(&d, std.testing.allocator);

    try record(&d, .{
        .verb = .delete,
        .entity = .{ .kind = "task", .id = 17 },
    });

    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery(
            "select count(*) from audit_log " ++
                "where actor is null and scope is null and summary is null",
        ),
    );
}

test "verb CHECK constraint catches bogus values" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try @import("db").migrate.applyAll(&d, std.testing.allocator);
    // Bypass the typed wrapper to prove the schema-level guard.
    try std.testing.expectError(
        db.sqlite.Error.StepFailed,
        d.execParams(
            insert_sql,
            &.{
                .{ .text = "drop_table" },
                .{ .text = "task" },
                .{ .int = 1 },
                .{ .null = {} },
                .{ .null = {} },
                .{ .null = {} },
            },
        ),
    );
}
