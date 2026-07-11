//! Audit-log cockpit queries and entity filters.

const std = @import("std");
const db = @import("db");
const testing = std.testing;

// =========================================================================
// Audit Log view-model  (tasks 4035, 4036, M12)
//
// Reads: audit_log (id, verb, entity_kind, entity_id, actor, scope,
//        summary, recorded_at). Schema confirmed from
//        migrations/00014_audit_log.up.sql.
//
// Filter: optional entity_kind+entity_id pair. When set, only rows for
//         that entity are returned. When null, all rows are returned.
// Order:  newest-first (recorded_at DESC).
// =========================================================================

/// One row in the Audit Log navigator list. All string fields are
/// heap-allocated and owned by the caller; release via deinit.
pub const AuditLogRow = struct {
    id: i64,
    /// Mutation verb: "create" | "update" | "delete" | "status_change" |
    /// "link" | "unlink". Confirmed CHECK constraint from migration 00014.
    verb: []const u8,
    /// Entity kind string, e.g. "task", "plan", "decision".
    entity_kind: []const u8,
    entity_id: i64,
    /// Optional actor label (free-text, e.g. "claude" or CLI username).
    actor: ?[]const u8,
    /// Optional scope string.
    scope: ?[]const u8,
    /// Optional human-readable mutation summary.
    summary: ?[]const u8,
    /// ISO-8601 timestamp when the mutation was recorded.
    recorded_at: []const u8,
    /// Pre-formatted display text: "[verb] entity_kind:entity_id  actor  recorded_at".
    /// Heap-allocated. Used by the navigator renderer directly so that
    /// grapheme pointers remain valid for the duration of the render.
    display_text: []const u8,
    /// Pre-formatted entity reference: "entity_kind:entity_id" (e.g. "task:55").
    /// Heap-allocated. Used by the detail pane renderer to avoid stack-local
    /// format buffers whose grapheme pointers dangle after the render function
    /// returns (MEMORY GUARD rule (c)).
    entity_ref: []const u8,

    pub fn deinit(self: AuditLogRow, allocator: std.mem.Allocator) void {
        allocator.free(self.verb);
        allocator.free(self.entity_kind);
        if (self.actor) |s| allocator.free(s);
        if (self.scope) |s| allocator.free(s);
        if (self.summary) |s| allocator.free(s);
        allocator.free(self.recorded_at);
        allocator.free(self.display_text);
        allocator.free(self.entity_ref);
    }

    pub fn deinitMany(rows: []AuditLogRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Entity filter for the Audit Log view.
///
/// When filter is .all, all rows are returned. When filter is .entity,
/// only rows where entity_kind=kind AND entity_id=id are returned.
/// This is the entity-narrowing affordance for task 4036.
pub const AuditEntityFilter = union(enum) {
    all,
    entity: struct {
        kind: []const u8,
        id: i64,
    },

    /// Return a human-readable label for display in the filter bar.
    /// Uses a fixed stack buffer; the result is valid for the call duration.
    pub fn label(self: AuditEntityFilter, buf: []u8) []const u8 {
        return switch (self) {
            .all => std.fmt.bufPrint(buf, "all entities", .{}) catch "all entities",
            .entity => |e| std.fmt.bufPrint(buf, "{s}:{d}", .{ e.kind, e.id }) catch "entity",
        };
    }
};

/// Query audit_log rows, newest-first. When `filter` is `.entity`, narrows
/// to the given entity_kind+entity_id pair. Returns a heap-allocated slice;
/// caller owns and must release via `AuditLogRow.deinitMany`.
///
/// Column order: id(0), verb(1), entity_kind(2), entity_id(3),
///               actor(4), scope(5), summary(6), recorded_at(7).
///
/// MEMORY GUARD (brief rule (c)): all string fields are freshly duped from
/// the statement columns. display_text is independently allocated via
/// allocPrint. deinit frees every field symmetrically.
pub fn queryAuditLog(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: AuditEntityFilter,
) ![]AuditLogRow {
    var rows: std.ArrayList(AuditLogRow) = .empty;
    errdefer {
        for (rows.items) |r| r.deinit(allocator);
        rows.deinit(allocator);
    }

    switch (filter) {
        .all => {
            var stmt = try d.prepare(
                \\select id, coalesce(verb,''), coalesce(entity_kind,''), entity_id,
                \\       actor, scope, summary, coalesce(recorded_at,'')
                \\from audit_log
                \\order by recorded_at desc, id desc
            );
            defer stmt.finalize();
            try stmt.bind(&.{});
            while (true) {
                switch (try stmt.step()) {
                    .done => break,
                    .row => {
                        const row = try readAuditLogRow(&stmt, allocator);
                        errdefer row.deinit(allocator);
                        try rows.append(allocator, row);
                    },
                }
            }
        },
        .entity => |e| {
            var stmt = try d.prepare(
                \\select id, coalesce(verb,''), coalesce(entity_kind,''), entity_id,
                \\       actor, scope, summary, coalesce(recorded_at,'')
                \\from audit_log
                \\where entity_kind = ? and entity_id = ?
                \\order by recorded_at desc, id desc
            );
            defer stmt.finalize();
            try stmt.bind(&.{ .{ .text = e.kind }, .{ .int = e.id } });
            while (true) {
                switch (try stmt.step()) {
                    .done => break,
                    .row => {
                        const row = try readAuditLogRow(&stmt, allocator);
                        errdefer row.deinit(allocator);
                        try rows.append(allocator, row);
                    },
                }
            }
        },
    }

    return rows.toOwnedSlice(allocator);
}

/// Read one AuditLogRow from the current statement row (called while stmt
/// is in the `.row` state). Takes `*db.sqlite.Stmt` so column methods
/// receive a mutable receiver, matching the Stmt API.
fn readAuditLogRow(
    stmt: *db.sqlite.Stmt,
    allocator: std.mem.Allocator,
) !AuditLogRow {
    const id = stmt.columnInt(0);

    const verb = try stmt.columnTextAlloc(1, allocator);
    errdefer allocator.free(verb);

    const entity_kind = try stmt.columnTextAlloc(2, allocator);
    errdefer allocator.free(entity_kind);

    const entity_id = stmt.columnInt(3);

    const actor: ?[]const u8 = if (stmt.columnIsNull(4)) null else try stmt.columnTextAlloc(4, allocator);
    errdefer if (actor) |s| allocator.free(s);

    const scope: ?[]const u8 = if (stmt.columnIsNull(5)) null else try stmt.columnTextAlloc(5, allocator);
    errdefer if (scope) |s| allocator.free(s);

    const summary: ?[]const u8 = if (stmt.columnIsNull(6)) null else try stmt.columnTextAlloc(6, allocator);
    errdefer if (summary) |s| allocator.free(s);

    const recorded_at = try stmt.columnTextAlloc(7, allocator);
    errdefer allocator.free(recorded_at);

    // Build pre-formatted display_text independently; no alias into any
    // field above. Format: "[verb] entity_kind:entity_id  actor  ts_short"
    // ts_short = first 19 chars of recorded_at (YYYY-MM-DDTHH:MM:SS).
    const ts_short = if (recorded_at.len >= 19) recorded_at[0..19] else recorded_at;
    const actor_display: []const u8 = actor orelse "\u{2014}";
    const display_text = try std.fmt.allocPrint(
        allocator,
        "[{s}] {s}:{d}  {s}  {s}",
        .{ verb, entity_kind, entity_id, actor_display, ts_short },
    );
    errdefer allocator.free(display_text);

    // Build pre-formatted entity_ref ("entity_kind:entity_id") independently.
    // MEMORY GUARD (rule (c)): used by renderDetail instead of a stack-local
    // format buffer so grapheme pointers remain valid after the render function
    // returns (stack buffers dangle; heap slices are stable).
    const entity_ref = try std.fmt.allocPrint(
        allocator,
        "{s}:{d}",
        .{ entity_kind, entity_id },
    );
    errdefer allocator.free(entity_ref);

    return .{
        .id = id,
        .verb = verb,
        .entity_kind = entity_kind,
        .entity_id = entity_id,
        .actor = actor,
        .scope = scope,
        .summary = summary,
        .recorded_at = recorded_at,
        .display_text = display_text,
        .entity_ref = entity_ref,
    };
}

/// Query all unique (entity_kind, entity_id) pairs present in audit_log,
/// sorted by entity_kind ASC then entity_id ASC. Used by the filter-cycle
/// affordance in the Audit Log view so the operator can tab through entities.
/// Returns a heap-allocated slice of (kind, id) tuples; caller must free.
pub const AuditEntityRef = struct {
    kind: []const u8,
    id: i64,

    pub fn deinit(self: AuditEntityRef, allocator: std.mem.Allocator) void {
        allocator.free(self.kind);
    }

    pub fn deinitMany(refs: []AuditEntityRef, allocator: std.mem.Allocator) void {
        for (refs) |r| r.deinit(allocator);
        allocator.free(refs);
    }
};

/// Query distinct entity refs in audit_log (for filter cycling).
pub fn queryAuditEntities(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]AuditEntityRef {
    var refs: std.ArrayList(AuditEntityRef) = .empty;
    errdefer {
        for (refs.items) |r| r.deinit(allocator);
        refs.deinit(allocator);
    }

    var stmt = try d.prepare(
        \\select distinct entity_kind, entity_id
        \\from audit_log
        \\order by entity_kind asc, entity_id asc
    );
    defer stmt.finalize();
    try stmt.bind(&.{});
    while (true) {
        const step = try stmt.step();
        if (step == .done) break;
        const kind = try stmt.columnTextAlloc(0, allocator);
        errdefer allocator.free(kind);
        const eid = stmt.columnInt(1);
        try refs.append(allocator, .{ .kind = kind, .id = eid });
    }

    return refs.toOwnedSlice(allocator);
}

// =========================================================================
// Audit Log view-model tests (task 4035, 4036)
// =========================================================================

fn setupTestDbAudit(a: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, a);
    return d;
}

test "view_model: queryAuditLog on empty DB returns empty slice (task 4035 empty)" {
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    const rows = try queryAuditLog(&d, a, .all);
    defer AuditLogRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryAuditLog returns rows newest-first (task 4035)" {
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 1, 'alice', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('update', 'task', 1, 'bob', '2025-06-01T00:00:00.000Z')",
        &.{},
    );

    const rows = try queryAuditLog(&d, a, .all);
    defer AuditLogRow.deinitMany(rows, a);

    // Two rows, newest-first.
    try testing.expectEqual(@as(usize, 2), rows.len);
    try testing.expectEqualStrings("update", rows[0].verb);
    try testing.expectEqualStrings("create", rows[1].verb);
}

test "view_model: queryAuditLog row fields populated (task 4035)" {
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, scope, summary, recorded_at) values ('status_change', 'task', 42, 'claude', 'project/myrepo', 'moved to doing', '2026-01-15T10:30:00.000Z')",
        &.{},
    );

    const rows = try queryAuditLog(&d, a, .all);
    defer AuditLogRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    const r = rows[0];
    try testing.expectEqualStrings("status_change", r.verb);
    try testing.expectEqualStrings("task", r.entity_kind);
    try testing.expectEqual(@as(i64, 42), r.entity_id);
    try testing.expect(r.actor != null);
    try testing.expectEqualStrings("claude", r.actor.?);
    try testing.expect(r.scope != null);
    try testing.expectEqualStrings("project/myrepo", r.scope.?);
    try testing.expect(r.summary != null);
    try testing.expectEqualStrings("moved to doing", r.summary.?);
    // Timestamp must start with 2026.
    try testing.expect(std.mem.startsWith(u8, r.recorded_at, "2026"));
    // display_text must contain verb, entity_kind, entity_id, actor, and ts.
    try testing.expect(std.mem.indexOf(u8, r.display_text, "status_change") != null);
    try testing.expect(std.mem.indexOf(u8, r.display_text, "task") != null);
    try testing.expect(std.mem.indexOf(u8, r.display_text, "42") != null);
    try testing.expect(std.mem.indexOf(u8, r.display_text, "claude") != null);
}

test "view_model: queryAuditLog entity filter narrows rows (task 4036)" {
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 10, 'alice', '2026-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('update', 'task', 10, 'alice', '2026-01-02T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'plan', 5, 'bob', '2026-01-03T00:00:00.000Z')",
        &.{},
    );

    // Filter to task:10 — should return 2 rows.
    const filtered = try queryAuditLog(&d, a, .{ .entity = .{ .kind = "task", .id = 10 } });
    defer AuditLogRow.deinitMany(filtered, a);
    try testing.expectEqual(@as(usize, 2), filtered.len);
    for (filtered) |r| {
        try testing.expectEqualStrings("task", r.entity_kind);
        try testing.expectEqual(@as(i64, 10), r.entity_id);
    }

    // Filter to plan:5 — should return 1 row.
    const plan_rows = try queryAuditLog(&d, a, .{ .entity = .{ .kind = "plan", .id = 5 } });
    defer AuditLogRow.deinitMany(plan_rows, a);
    try testing.expectEqual(@as(usize, 1), plan_rows.len);
    try testing.expectEqualStrings("plan", plan_rows[0].entity_kind);

    // All — should return 3 rows.
    const all_rows = try queryAuditLog(&d, a, .all);
    defer AuditLogRow.deinitMany(all_rows, a);
    try testing.expectEqual(@as(usize, 3), all_rows.len);
}

test "view_model: queryAuditEntities returns distinct entity refs (task 4036)" {
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'task', 1, 'alice', '2026-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('update', 'task', 1, 'alice', '2026-01-02T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, actor, recorded_at) values ('create', 'plan', 5, 'bob', '2026-01-03T00:00:00.000Z')",
        &.{},
    );

    const refs = try queryAuditEntities(&d, a);
    defer AuditEntityRef.deinitMany(refs, a);

    // Two distinct (kind,id) pairs: plan:5 and task:1 (sorted by kind asc).
    try testing.expectEqual(@as(usize, 2), refs.len);
    try testing.expectEqualStrings("plan", refs[0].kind);
    try testing.expectEqual(@as(i64, 5), refs[0].id);
    try testing.expectEqualStrings("task", refs[1].kind);
    try testing.expectEqual(@as(i64, 1), refs[1].id);
}

test "view_model: AuditEntityFilter label all returns expected string" {
    var buf: [64]u8 = undefined;
    const f: AuditEntityFilter = .all;
    const lbl = f.label(&buf);
    try testing.expectEqualStrings("all entities", lbl);
}

test "view_model: AuditEntityFilter label entity returns kind:id" {
    var buf: [64]u8 = undefined;
    const f = AuditEntityFilter{ .entity = .{ .kind = "task", .id = 42 } };
    const lbl = f.label(&buf);
    try testing.expect(std.mem.indexOf(u8, lbl, "task") != null);
    try testing.expect(std.mem.indexOf(u8, lbl, "42") != null);
}

test "view_model: AuditLogRow deinit handles null optional fields" {
    // Verify that a row with null actor/scope/summary deinit cleanly.
    const a = testing.allocator;
    var d = try setupTestDbAudit(a);
    defer d.close();

    // Insert with nulls — no actor, no scope, no summary.
    _ = try d.execParams(
        "insert into audit_log (verb, entity_kind, entity_id, recorded_at) values ('delete', 'artifact', 99, '2026-03-01T00:00:00.000Z')",
        &.{},
    );

    const rows = try queryAuditLog(&d, a, .all);
    defer AuditLogRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expect(rows[0].actor == null);
    try testing.expect(rows[0].scope == null);
    try testing.expect(rows[0].summary == null);
    // display_text must use "—" placeholder when actor is null.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "\u{2014}") != null);
}
