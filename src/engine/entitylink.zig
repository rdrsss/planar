//! engine/entitylink — Entity links: generic cross-cutting relationships
//! between any two Planar entities.
//!
//! entity_links stores pairs (from_kind:from_id, to_kind:to_id, relationship)
//! with a UNIQUE constraint that prevents duplicates. The from_kind / to_kind
//! CHECK constraint covers every kind currently in the schema (widened in
//! migrations 0004 and 0012). The relationship CHECK covers:
//!   derives-from, depends-on, addresses, verifies, cites, supersedes, touches
//!
//! Link verbs are deliberately UNGUARDED — they create cross-scope edges by
//! design. The per-entity `link` handlers in Cycle B (not in this module)
//! hold the guard decision. This module exposes the raw primitive.
//!
//! `parseRef` accepts a "kind:id-or-slug" string and returns a ParsedRef
//! (either a numeric id or a slug). Callers that need to resolve a slug to
//! a numeric id perform a per-table lookup directly (matching the
//! parseRef + custom-lookup pattern used by M2.B handlers). There is no
//! generic resolveRef helper in this module — per-table lookups are preferred
//! because the entity-specific handler already knows the table.
//!
//! First-cut scope handling: the `scope` field on AddArgs / ListFilter is
//! accepted for forward compatibility; only `.global` (null scope) resolves
//! until the association lookup lands in M3. Passing a non-null scope returns
//! Error.UnsupportedScope.

const std = @import("std");
const db = @import("db");
const policy = @import("policy.zig");

// =========================================================================
// Enums
// =========================================================================

/// EntityKind covers all kinds accepted by the entity_links CHECK constraint.
/// The set was widened in migration 0012 to include 'annotation'.
pub const EntityKind = enum {
    plan,
    plan_step,
    task,
    question,
    test_scenario,
    artifact,
    decision,
    session,
    repo,
    annotation,

    pub fn fromText(s: []const u8) ?EntityKind {
        if (std.mem.eql(u8, s, "plan")) return .plan;
        if (std.mem.eql(u8, s, "plan_step")) return .plan_step;
        if (std.mem.eql(u8, s, "task")) return .task;
        if (std.mem.eql(u8, s, "question")) return .question;
        if (std.mem.eql(u8, s, "test_scenario")) return .test_scenario;
        if (std.mem.eql(u8, s, "artifact")) return .artifact;
        if (std.mem.eql(u8, s, "decision")) return .decision;
        if (std.mem.eql(u8, s, "session")) return .session;
        if (std.mem.eql(u8, s, "repo")) return .repo;
        if (std.mem.eql(u8, s, "annotation")) return .annotation;
        return null;
    }

    pub fn toText(self: EntityKind) []const u8 {
        return @tagName(self);
    }
};

/// Relationship covers all values accepted by the entity_links CHECK
/// constraint on the `relationship` column.
pub const Relationship = enum {
    @"derives-from",
    @"depends-on",
    addresses,
    verifies,
    cites,
    supersedes,
    touches,

    pub fn fromText(s: []const u8) ?Relationship {
        if (std.mem.eql(u8, s, "derives-from")) return .@"derives-from";
        if (std.mem.eql(u8, s, "depends-on")) return .@"depends-on";
        // `blocks` was the old spelling for this edge and is deliberately NOT
        // accepted as an alias. `A --blocks--> B` stored "A depends on B", so
        // silently mapping the old word would preserve exactly the inversion
        // the rename exists to remove. Rejecting it forces the author to
        // re-read the direction, and the CLI names the replacement.
        if (std.mem.eql(u8, s, "blocks")) return null;
        if (std.mem.eql(u8, s, "addresses")) return .addresses;
        if (std.mem.eql(u8, s, "verifies")) return .verifies;
        if (std.mem.eql(u8, s, "cites")) return .cites;
        if (std.mem.eql(u8, s, "supersedes")) return .supersedes;
        if (std.mem.eql(u8, s, "touches")) return .touches;
        return null;
    }

    pub fn toText(self: Relationship) []const u8 {
        return switch (self) {
            .@"derives-from" => "derives-from",
            .@"depends-on" => "depends-on",
            .addresses => "addresses",
            .verifies => "verifies",
            .cites => "cites",
            .supersedes => "supersedes",
            .touches => "touches",
        };
    }
};

// =========================================================================
// Types
// =========================================================================

/// EntityLink is a row from the entity_links table.
pub const EntityLink = struct {
    id: i64,
    from_kind: EntityKind,
    from_id: i64,
    to_kind: EntityKind,
    to_id: i64,
    relationship: Relationship,
    created_at: []const u8,
};

pub fn deinit(link: EntityLink, allocator: std.mem.Allocator) void {
    allocator.free(link.created_at);
}

pub fn deinitMany(items: []const EntityLink, allocator: std.mem.Allocator) void {
    for (items) |l| deinit(l, allocator);
    allocator.free(items);
}

/// AuditRow is a row from the audit_log table as returned by `trail`.
pub const AuditRow = struct {
    id: i64,
    verb: []const u8,
    entity_kind: []const u8,
    entity_id: i64,
    actor: ?[]const u8,
    scope: ?[]const u8,
    summary: ?[]const u8,
    recorded_at: []const u8,
};

pub fn deinitAuditRow(row: AuditRow, allocator: std.mem.Allocator) void {
    allocator.free(row.verb);
    allocator.free(row.entity_kind);
    if (row.actor) |s| allocator.free(s);
    if (row.scope) |s| allocator.free(s);
    if (row.summary) |s| allocator.free(s);
    allocator.free(row.recorded_at);
}

pub fn deinitAuditRows(rows: []const AuditRow, allocator: std.mem.Allocator) void {
    for (rows) |r| deinitAuditRow(r, allocator);
    allocator.free(rows);
}

/// ParsedRef is the decoded form of a "kind:id-or-slug" reference string.
pub const ParsedRef = union(enum) {
    /// Reference resolved to a numeric id directly.
    id: struct { kind: EntityKind, id: i64 },
    /// Reference contains a slug that requires a DB lookup to resolve.
    slug: struct { kind: EntityKind, slug: []const u8 },
};

// =========================================================================
// Argument structs
// =========================================================================

pub const AddArgs = struct {
    from_kind: EntityKind,
    from_id: i64,
    to_kind: EntityKind,
    to_id: i64,
    relationship: Relationship,
    /// When true the (future) scope guard is bypassed — legacy escape hatch
    /// matching Go's SkipScopeCheck. Link verbs are documented unguarded;
    /// this field is present for API parity and Cycle B handler symmetry.
    skip_scope_check: bool = false,
    /// Accepted for forward compatibility; only null (global) resolves
    /// until the association lookup lands in M3.
    scope: ?[]const u8 = null,
};

pub const ListFilter = struct {
    /// Return links where from_kind = this kind and from_id = this id.
    from_kind: ?EntityKind = null,
    from_id: ?i64 = null,
    /// Return links where to_kind = this kind and to_id = this id.
    to_kind: ?EntityKind = null,
    to_id: ?i64 = null,
    /// Filter by relationship type.
    relationship: ?Relationship = null,
    /// Accepted for forward compatibility; only null (global) resolves.
    scope: ?[]const u8 = null,
};

// =========================================================================
// Error set
// =========================================================================

pub const Error =
    error{
        NotFound,
        LinkExists,
        UnsupportedScope,
        InvalidRef,
        QueryFailed,
    } ||
    std.mem.Allocator.Error ||
    policy.audit.Error;

// =========================================================================
// Verbs
// =========================================================================

/// add inserts a new entity_links row and records an audit row.
///
/// Returns the created EntityLink. Returns Error.LinkExists if the (from,
/// to, relationship) tuple is already recorded. Returns
/// Error.UnsupportedScope if args.scope is non-null (M3 forward-compat
/// guard; link verbs are unguarded so no scope_guard.check call is made).
pub fn add(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: AddArgs) Error!EntityLink {
    if (args.scope != null) return Error.UnsupportedScope;

    const insert_sql: [:0]const u8 =
        \\insert into entity_links (from_kind, from_id, to_kind, to_id, relationship)
        \\values (?, ?, ?, ?, ?)
    ;

    const id = d.execParams(insert_sql, &.{
        .{ .text = args.from_kind.toText() },
        .{ .int = args.from_id },
        .{ .text = args.to_kind.toText() },
        .{ .int = args.to_id },
        .{ .text = args.relationship.toText() },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.LinkExists;
        std.log.err("entitylink.add exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    try policy.audit.record(d, .{
        .verb = .link,
        .entity = .{ .kind = "entity_link", .id = id },
        .summary = null,
    });

    return try show(d, allocator, id);
}

/// remove deletes an entity_links row by primary key and records an audit row.
///
/// Returns Error.NotFound when the row does not exist.
pub fn remove(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!void {
    // Verify existence so we return NotFound rather than a silent no-op.
    const link = try show(d, allocator, id);
    deinit(link, allocator);

    _ = d.execParams("delete from entity_links where id = ?", &.{.{ .int = id }}) catch |e| {
        std.log.err("entitylink.remove exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    try policy.audit.record(d, .{
        .verb = .unlink,
        .entity = .{ .kind = "entity_link", .id = id },
        .summary = null,
    });
}

/// show fetches an entity_links row by primary key.
///
/// Returns Error.NotFound when the row does not exist.
pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!EntityLink {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => readRow(&stmt, allocator),
    };
}

/// list returns entity_links rows matching the given filter.
///
/// Filters are ANDed. An empty filter returns all rows. Results are ordered
/// by id. Never returns null — an empty result set yields an empty slice.
pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: ListFilter) Error![]EntityLink {
    if (filter.scope != null) return Error.UnsupportedScope;

    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, select_all_prefix);

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    if (filter.from_kind) |k| {
        try sql_buf.appendSlice(allocator, " and from_kind = ?");
        try params.append(allocator, .{ .text = k.toText() });
    }
    if (filter.from_id) |n| {
        try sql_buf.appendSlice(allocator, " and from_id = ?");
        try params.append(allocator, .{ .int = n });
    }
    if (filter.to_kind) |k| {
        try sql_buf.appendSlice(allocator, " and to_kind = ?");
        try params.append(allocator, .{ .text = k.toText() });
    }
    if (filter.to_id) |n| {
        try sql_buf.appendSlice(allocator, " and to_id = ?");
        try params.append(allocator, .{ .int = n });
    }
    if (filter.relationship) |r| {
        try sql_buf.appendSlice(allocator, " and relationship = ?");
        try params.append(allocator, .{ .text = r.toText() });
    }

    try sql_buf.appendSlice(allocator, " order by id");

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var out: std.ArrayList(EntityLink) = .empty;
    errdefer {
        for (out.items) |l| deinit(l, allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// trail returns audit_log rows for the given entity_link id, ordered by id.
///
/// Returns Error.NotFound when the link does not exist.
pub fn trail(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error![]AuditRow {
    // Verify the link exists before querying the audit log.
    const link = try show(d, allocator, id);
    deinit(link, allocator);

    const trail_sql: [:0]const u8 =
        \\select id, verb, entity_kind, entity_id, actor, scope, summary, recorded_at
        \\from audit_log
        \\where entity_kind = 'entity_link' and entity_id = ?
        \\order by id
    ;

    var stmt = d.prepare(trail_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(AuditRow) = .empty;
    errdefer {
        for (out.items) |r| deinitAuditRow(r, allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const row = AuditRow{
                    .id = stmt.columnInt(0),
                    .verb = try stmt.columnTextAlloc(1, allocator),
                    .entity_kind = try stmt.columnTextAlloc(2, allocator),
                    .entity_id = stmt.columnInt(3),
                    .actor = try stmt.columnTextOpt(4, allocator),
                    .scope = try stmt.columnTextOpt(5, allocator),
                    .summary = try stmt.columnTextOpt(6, allocator),
                    .recorded_at = try stmt.columnTextAlloc(7, allocator),
                };
                try out.append(allocator, row);
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Ref parsing and resolution
// =========================================================================

/// parseRef decodes a "kind:id-or-slug" reference string into a ParsedRef.
///
/// If the part after the colon is a pure positive integer it yields a
/// ParsedRef.id; otherwise it yields a ParsedRef.slug (slug string is a
/// slice into the input — NOT heap-allocated; the caller must keep the
/// input alive as long as the returned slug is in use).
///
/// Returns Error.InvalidRef for malformed input (no colon, empty kind,
/// empty id-or-slug, unknown kind, non-positive integer id).
pub fn parseRef(s: []const u8) Error!ParsedRef {
    const colon = std.mem.lastIndexOfScalar(u8, s, ':') orelse return Error.InvalidRef;
    const kind_text = s[0..colon];
    const ref_text = s[colon + 1 ..];

    if (kind_text.len == 0) return Error.InvalidRef;
    if (ref_text.len == 0) return Error.InvalidRef;

    const kind = EntityKind.fromText(kind_text) orelse return Error.InvalidRef;

    // Try to parse as an integer id.
    if (std.fmt.parseInt(i64, ref_text, 10)) |n| {
        if (n <= 0) return Error.InvalidRef;
        return ParsedRef{ .id = .{ .kind = kind, .id = n } };
    } else |_| {
        // Not an integer — treat as a slug.
        return ParsedRef{ .slug = .{ .kind = kind, .slug = ref_text } };
    }
}

// =========================================================================
// Internals
// =========================================================================

const select_columns =
    "id, from_kind, from_id, to_kind, to_id, relationship, created_at";

const select_one_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from entity_links where id = ?";

const select_all_prefix =
    "select " ++ select_columns ++ " from entity_links where 1 = 1";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!EntityLink {
    const from_kind_text = try stmt.columnTextAlloc(1, allocator);
    defer allocator.free(from_kind_text);
    const from_kind = EntityKind.fromText(from_kind_text) orelse return Error.QueryFailed;

    const to_kind_text = try stmt.columnTextAlloc(3, allocator);
    defer allocator.free(to_kind_text);
    const to_kind = EntityKind.fromText(to_kind_text) orelse return Error.QueryFailed;

    const rel_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(rel_text);
    const relationship = Relationship.fromText(rel_text) orelse return Error.QueryFailed;

    return EntityLink{
        .id = stmt.columnInt(0),
        .from_kind = from_kind,
        .from_id = stmt.columnInt(2),
        .to_kind = to_kind,
        .to_id = stmt.columnInt(4),
        .relationship = relationship,
        .created_at = try stmt.columnTextAlloc(6, allocator),
    };
}

// =========================================================================
// Tests
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

// Helper: insert a minimal plan row so we have a real entity to link.
// plans.slug is NOT NULL; we derive a unique slug from the title.
fn insertPlan(d: *db.sqlite.Db, title: []const u8) !i64 {
    const sql =
        \\insert into plans (scope_kind, title, slug, status)
        \\values ('global', ?, ?, 'draft')
    ;
    return d.execParams(sql, &.{ .{ .text = title }, .{ .text = title } });
}

// Helper: insert a minimal task row. status defaults to 'todo'.
fn insertTask(d: *db.sqlite.Db, plan_id: i64, title: []const u8) !i64 {
    const sql =
        \\insert into tasks (scope_kind, scope_id, plan_id, parent_task_id,
        \\                   title, body, status, priority, next_action, due_at)
        \\values ('global', null, ?, null, ?, null, 'todo', 100, null, null)
    ;
    return d.execParams(sql, &.{ .{ .int = plan_id }, .{ .text = title } });
}

// ---- EntityKind round-trip -----------------------------------------------

test "EntityKind.fromText / toText round-trip for every kind" {
    const kinds = [_][]const u8{
        "plan",     "plan_step", "task",    "question", "test_scenario",
        "artifact", "decision",  "session", "repo",     "annotation",
    };
    for (kinds) |k| {
        const ek = EntityKind.fromText(k) orelse {
            try std.testing.expect(false); // fail with a readable message
            continue;
        };
        try std.testing.expectEqualStrings(k, ek.toText());
    }
}

test "EntityKind.fromText returns null for unknown kind" {
    try std.testing.expect(EntityKind.fromText("bogus") == null);
    try std.testing.expect(EntityKind.fromText("") == null);
}

// ---- Relationship round-trip ----------------------------------------------

test "Relationship.fromText / toText round-trip for every relationship" {
    const rels = [_][]const u8{
        "derives-from", "depends-on", "addresses", "verifies",
        "cites",        "supersedes", "touches",
    };
    for (rels) |r| {
        const rel = Relationship.fromText(r) orelse {
            try std.testing.expect(false);
            continue;
        };
        try std.testing.expectEqualStrings(r, rel.toText());
    }
}

test "Relationship.fromText returns null for unknown relationship" {
    try std.testing.expect(Relationship.fromText("nope") == null);
}

// ---- parseRef ------------------------------------------------------------

test "parseRef: 'plan:42' yields id ref" {
    const ref = try parseRef("plan:42");
    switch (ref) {
        .id => |r| {
            try std.testing.expectEqual(EntityKind.plan, r.kind);
            try std.testing.expectEqual(@as(i64, 42), r.id);
        },
        .slug => return error.UnexpectedSlug,
    }
}

test "parseRef: 'plan:some-slug' yields slug ref" {
    const ref = try parseRef("plan:some-slug");
    switch (ref) {
        .slug => |r| {
            try std.testing.expectEqual(EntityKind.plan, r.kind);
            try std.testing.expectEqualStrings("some-slug", r.slug);
        },
        .id => return error.UnexpectedId,
    }
}

test "parseRef: malformed inputs return InvalidRef" {
    const bad = [_][]const u8{
        "", // empty
        "plan", // no colon
        ":42", // empty kind
        "plan:", // empty id / slug
        "bogus:42", // unknown kind
        "plan:0", // non-positive id
        "plan:-1", // negative id
    };
    for (bad) |s| {
        try std.testing.expectError(Error.InvalidRef, parseRef(s));
    }
}

// ---- add -----------------------------------------------------------------

test "add: happy path between two existing entities; audit row recorded" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try insertPlan(&d, "Plan A");
    const p2 = try insertPlan(&d, "Plan B");

    const link = try add(&d, a, .{
        .from_kind = .plan,
        .from_id = p1,
        .to_kind = .plan,
        .to_id = p2,
        .relationship = .@"derives-from",
    });
    defer deinit(link, a);

    try std.testing.expectEqual(EntityKind.plan, link.from_kind);
    try std.testing.expectEqual(p1, link.from_id);
    try std.testing.expectEqual(EntityKind.plan, link.to_kind);
    try std.testing.expectEqual(p2, link.to_id);
    try std.testing.expectEqual(Relationship.@"derives-from", link.relationship);

    // Audit row.
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='link' and entity_kind='entity_link'"),
    );
}

test "add: duplicate returns LinkExists" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try insertPlan(&d, "Plan A");
    const p2 = try insertPlan(&d, "Plan B");

    const link = try add(&d, a, .{
        .from_kind = .plan,
        .from_id = p1,
        .to_kind = .plan,
        .to_id = p2,
        .relationship = .cites,
    });
    defer deinit(link, a);

    try std.testing.expectError(Error.LinkExists, add(&d, a, .{
        .from_kind = .plan,
        .from_id = p1,
        .to_kind = .plan,
        .to_id = p2,
        .relationship = .cites,
    }));
}

test "add: unsupported scope (.repo / .association) returns UnsupportedScope" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(Error.UnsupportedScope, add(&d, a, .{
        .from_kind = .plan,
        .from_id = 1,
        .to_kind = .plan,
        .to_id = 2,
        .relationship = .@"depends-on",
        .scope = "some-assoc",
    }));
}

test "add: skip_scope_check=true is accepted without error (scope still null)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try insertPlan(&d, "Plan A");
    const p2 = try insertPlan(&d, "Plan B");

    // skip_scope_check=true with null scope: must succeed.
    const link = try add(&d, a, .{
        .from_kind = .plan,
        .from_id = p1,
        .to_kind = .plan,
        .to_id = p2,
        .relationship = .touches,
        .skip_scope_check = true,
    });
    defer deinit(link, a);
    try std.testing.expectEqual(EntityKind.plan, link.from_kind);
}

// ---- remove --------------------------------------------------------------

test "remove: happy path; audit row recorded" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try insertPlan(&d, "P1");
    const p2 = try insertPlan(&d, "P2");
    const link = try add(&d, a, .{
        .from_kind = .plan,
        .from_id = p1,
        .to_kind = .plan,
        .to_id = p2,
        .relationship = .@"depends-on",
    });
    const link_id = link.id;
    deinit(link, a);

    try remove(&d, a, link_id);

    // Gone.
    try std.testing.expectError(Error.NotFound, show(&d, a, link_id));

    // Audit row for the unlink.
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='unlink' and entity_kind='entity_link'"),
    );
}

test "remove: missing id returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(Error.NotFound, remove(&d, a, 9999));
}

// ---- show ----------------------------------------------------------------

test "show: happy path" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try insertPlan(&d, "P1");
    const p2 = try insertPlan(&d, "P2");
    const created = try add(&d, a, .{
        .from_kind = .plan,
        .from_id = p1,
        .to_kind = .plan,
        .to_id = p2,
        .relationship = .verifies,
    });
    defer deinit(created, a);

    const fetched = try show(&d, a, created.id);
    defer deinit(fetched, a);

    try std.testing.expectEqual(created.id, fetched.id);
    try std.testing.expectEqual(Relationship.verifies, fetched.relationship);
}

test "show: missing returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
}

// ---- list ----------------------------------------------------------------

test "list: filter by from_kind + from_id returns matches" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try insertPlan(&d, "P1");
    const p2 = try insertPlan(&d, "P2");
    const p3 = try insertPlan(&d, "P3");

    const l1 = try add(&d, a, .{ .from_kind = .plan, .from_id = p1, .to_kind = .plan, .to_id = p2, .relationship = .cites });
    defer deinit(l1, a);
    const l2 = try add(&d, a, .{ .from_kind = .plan, .from_id = p1, .to_kind = .plan, .to_id = p3, .relationship = .@"depends-on" });
    defer deinit(l2, a);
    // Different from_id — should not appear.
    const l3 = try add(&d, a, .{ .from_kind = .plan, .from_id = p2, .to_kind = .plan, .to_id = p3, .relationship = .addresses });
    defer deinit(l3, a);

    const results = try list(&d, a, .{ .from_kind = .plan, .from_id = p1 });
    defer deinitMany(results, a);

    try std.testing.expectEqual(@as(usize, 2), results.len);
}

test "list: filter by to_kind + to_id returns matches" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try insertPlan(&d, "P1");
    const p2 = try insertPlan(&d, "P2");
    const p3 = try insertPlan(&d, "P3");

    const l1 = try add(&d, a, .{ .from_kind = .plan, .from_id = p1, .to_kind = .plan, .to_id = p3, .relationship = .cites });
    defer deinit(l1, a);
    const l2 = try add(&d, a, .{ .from_kind = .plan, .from_id = p2, .to_kind = .plan, .to_id = p3, .relationship = .@"depends-on" });
    defer deinit(l2, a);

    const results = try list(&d, a, .{ .to_kind = .plan, .to_id = p3 });
    defer deinitMany(results, a);

    try std.testing.expectEqual(@as(usize, 2), results.len);
}

test "list: filter by relationship returns matches" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try insertPlan(&d, "P1");
    const p2 = try insertPlan(&d, "P2");
    const p3 = try insertPlan(&d, "P3");

    const l1 = try add(&d, a, .{ .from_kind = .plan, .from_id = p1, .to_kind = .plan, .to_id = p2, .relationship = .cites });
    defer deinit(l1, a);
    const l2 = try add(&d, a, .{ .from_kind = .plan, .from_id = p2, .to_kind = .plan, .to_id = p3, .relationship = .@"depends-on" });
    defer deinit(l2, a);

    const results = try list(&d, a, .{ .relationship = .cites });
    defer deinitMany(results, a);

    try std.testing.expectEqual(@as(usize, 1), results.len);
    try std.testing.expectEqual(Relationship.cites, results[0].relationship);
}

test "list: filter by from-kind + to-kind pair returns matches" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try insertPlan(&d, "P1");
    const p2 = try insertPlan(&d, "P2");
    const t1 = try insertTask(&d, p1, "Task 1");

    // plan → plan link
    const l1 = try add(&d, a, .{ .from_kind = .plan, .from_id = p1, .to_kind = .plan, .to_id = p2, .relationship = .cites });
    defer deinit(l1, a);
    // plan → task link
    const l2 = try add(&d, a, .{ .from_kind = .plan, .from_id = p1, .to_kind = .task, .to_id = t1, .relationship = .touches });
    defer deinit(l2, a);

    const plan_to_plan = try list(&d, a, .{ .from_kind = .plan, .to_kind = .plan });
    defer deinitMany(plan_to_plan, a);
    try std.testing.expectEqual(@as(usize, 1), plan_to_plan.len);

    const plan_to_task = try list(&d, a, .{ .from_kind = .plan, .to_kind = .task });
    defer deinitMany(plan_to_task, a);
    try std.testing.expectEqual(@as(usize, 1), plan_to_task.len);
}

test "list: empty result returns empty slice (not null)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const results = try list(&d, a, .{});
    defer deinitMany(results, a);
    try std.testing.expectEqual(@as(usize, 0), results.len);
}

// ---- trail ---------------------------------------------------------------

test "trail: returns audit rows for the link" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try insertPlan(&d, "P1");
    const p2 = try insertPlan(&d, "P2");
    const link = try add(&d, a, .{
        .from_kind = .plan,
        .from_id = p1,
        .to_kind = .plan,
        .to_id = p2,
        .relationship = .supersedes,
    });
    const link_id = link.id;
    deinit(link, a);

    const rows = try trail(&d, a, link_id);
    defer deinitAuditRows(rows, a);

    // add() records one 'link' audit row.
    try std.testing.expectEqual(@as(usize, 1), rows.len);
    try std.testing.expectEqualStrings("link", rows[0].verb);
    try std.testing.expectEqual(link_id, rows[0].entity_id);
}

test "trail: NotFound for missing link id" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(Error.NotFound, trail(&d, a, 9999));
}
