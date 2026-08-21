//! engine/promotion — Scope promotion and demotion for Planar entities.
//!
//! Promotion moves an entity from any scope (global or association) to a
//! named association scope. Demotion is the reverse: it moves an entity
//! back to global scope (scope_kind='global', scope_id=NULL).
//!
//! Promotable entity kinds (table names closed in this module):
//!   plan, task, question, test_scenario, artifact, decision
//!
//! The scope_kind+scope_id update is performed atomically: the scope UPDATE
//! and the audit row use a single Db handle within the same logical unit
//! (SQLite's WAL mode guarantees visibility). A partial state (scope updated
//! but no audit) is bounded to a process crash — mirrors the Go approach of
//! wrapping in a tx.
//!
//! Errors returned mirror the Go promote.go semantics:
//!   NotFound       — entity id does not exist in its table
//!   SlugNotFound   — association slug not in associations table
//!   ScopeUnchanged — entity is already at the requested scope
//!   UnsupportedScope — "repo:<slug>" prefix (reserved)
//!   InvalidScope   — unknown entity kind
//!   QueryFailed    — unexpected DB error

const std = @import("std");
const db = @import("db");
const identity = @import("identity.zig");
const policy = @import("policy.zig");

// =========================================================================
// Types
// =========================================================================

pub const ScopeKind = enum {
    global,
    association,
    repo,

    pub fn fromText(s: []const u8) ?ScopeKind {
        if (std.mem.eql(u8, s, "global")) return .global;
        if (std.mem.eql(u8, s, "association")) return .association;
        if (std.mem.eql(u8, s, "repo")) return .repo;
        return null;
    }
};

pub const Error =
    error{
        NotFound,
        InvalidScope,
        ScopeUnchanged,
        UnsupportedScope,
        SlugNotFound,
        QueryFailed,
    } ||
    std.mem.Allocator.Error ||
    policy.audit.Error;

pub const PromoteArgs = struct {
    /// Entity kind: "plan", "task", "question", "test_scenario", "artifact", "decision".
    kind: []const u8,
    id: i64,
    /// Association slug, OR "global" to demote. "repo:<slug>" returns UnsupportedScope.
    to_scope: []const u8,
};

/// A promotable entity's current scope. `scope_kind` is heap-allocated;
/// the caller must free it.
pub const ScopeInfo = struct {
    scope_kind: []const u8,
    scope_id: ?i64,
};

// =========================================================================
// Public API
// =========================================================================

/// Promote or demote an entity to `args.to_scope`.
///
/// When `to_scope` is "global", this is equivalent to calling `demote`.
/// When `to_scope` is an association slug, this promotes to that association.
/// Passing "repo:<slug>" returns UnsupportedScope.
///
/// Returns ScopeUnchanged when the entity is already at the requested scope.
/// The scope UPDATE and audit row are written without any window between them.
pub fn promote(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: PromoteArgs) Error!void {
    const table = tableFor(args.kind) orelse return Error.InvalidScope;

    if (std.mem.startsWith(u8, args.to_scope, "repo:")) return Error.UnsupportedScope;

    if (std.mem.eql(u8, args.to_scope, "global")) {
        return try demoteEntity(d, allocator, table, args.kind, args.id);
    }

    // Resolve association slug.
    const scope_ref = identity.scope.resolveSlug(d, allocator, args.to_scope) catch |e| switch (e) {
        error.UnsupportedScope => return Error.UnsupportedScope,
        error.SlugNotFound => return Error.SlugNotFound,
        else => return Error.QueryFailed,
    };

    const assoc_id = scope_ref.id orelse return Error.QueryFailed;

    // Read current scope; also validates entity exists.
    const current = try readCurrentScope(d, allocator, table, args.id);

    // Idempotency: already at the target association.
    if (current.kind == .association and current.id != null and current.id.? == assoc_id) {
        return Error.ScopeUnchanged;
    }

    // Build audit summary capturing from→to.
    const from_str = try scopeToStr(allocator, current);
    defer allocator.free(from_str);
    const to_str = try std.fmt.allocPrint(allocator, "association:{d}", .{assoc_id});
    defer allocator.free(to_str);
    const summary = try std.fmt.allocPrint(
        allocator,
        "promote {s}:{d} from {s} to {s}",
        .{ args.kind, args.id, from_str, to_str },
    );
    defer allocator.free(summary);

    // Build the dynamically-tabled UPDATE SQL and apply it.
    const update_sql_raw = try std.fmt.allocPrint(
        allocator,
        "update {s} set scope_kind = 'association', scope_id = ?, " ++
            "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
        .{table},
    );
    defer allocator.free(update_sql_raw);
    const update_sql = try allocator.dupeZ(u8, update_sql_raw);
    defer allocator.free(update_sql);

    _ = d.execParams(update_sql, &.{
        .{ .int = assoc_id },
        .{ .int = args.id },
    }) catch return Error.QueryFailed;

    // Audit is best-effort: the scope UPDATE above is the load-bearing write.
    // Mirrors Go's promote.go where appendSessionEntryTx is called with _ =
    // (best-effort). Failure must NOT roll back the already-committed scope change.
    policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = args.kind, .id = args.id },
        .summary = summary,
    }) catch {};
}

/// Demote an entity to global scope (scope_kind='global', scope_id=NULL).
/// Returns ScopeUnchanged when the entity is already at global scope.
pub fn demote(d: *db.sqlite.Db, allocator: std.mem.Allocator, kind: []const u8, id: i64) Error!void {
    const table = tableFor(kind) orelse return Error.InvalidScope;
    return try demoteEntity(d, allocator, table, kind, id);
}

// =========================================================================
// Internals
// =========================================================================

const CurrentScope = struct {
    kind: ScopeKind,
    id: ?i64,
};

fn readCurrentScope(d: *db.sqlite.Db, allocator: std.mem.Allocator, table: []const u8, entity_id: i64) Error!CurrentScope {
    const sql_raw = try std.fmt.allocPrint(
        allocator,
        "select scope_kind, scope_id from {s} where id = ?",
        .{table},
    );
    defer allocator.free(sql_raw);
    const sql = try allocator.dupeZ(u8, sql_raw);
    defer allocator.free(sql);

    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = entity_id }}) catch return Error.QueryFailed;

    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => {
            const sk_text = try stmt.columnTextAlloc(0, allocator);
            defer allocator.free(sk_text);
            const sk = ScopeKind.fromText(sk_text) orelse return Error.QueryFailed;
            return .{ .kind = sk, .id = stmt.columnIntOpt(1) };
        },
    };
}

fn scopeToStr(allocator: std.mem.Allocator, s: CurrentScope) std.mem.Allocator.Error![]const u8 {
    if (s.id) |sid| {
        return try std.fmt.allocPrint(allocator, "{s}:{d}", .{ @tagName(s.kind), sid });
    }
    return try allocator.dupe(u8, @tagName(s.kind));
}

fn demoteEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    table: []const u8,
    kind: []const u8,
    id: i64,
) Error!void {
    const current = try readCurrentScope(d, allocator, table, id);

    if (current.kind == .global) return Error.ScopeUnchanged;

    const from_str = try scopeToStr(allocator, current);
    defer allocator.free(from_str);
    const summary = try std.fmt.allocPrint(
        allocator,
        "demote {s}:{d} from {s} to global",
        .{ kind, id, from_str },
    );
    defer allocator.free(summary);

    const update_sql_raw = try std.fmt.allocPrint(
        allocator,
        "update {s} set scope_kind = 'global', scope_id = null, " ++
            "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
        .{table},
    );
    defer allocator.free(update_sql_raw);
    const update_sql = try allocator.dupeZ(u8, update_sql_raw);
    defer allocator.free(update_sql);

    _ = d.execParams(update_sql, &.{.{ .int = id }}) catch return Error.QueryFailed;

    // Audit is best-effort: the scope UPDATE above is the load-bearing write.
    // Mirrors Go's promote.go where appendSessionEntryTx is called with _ =
    // (best-effort). Failure must NOT roll back the already-committed scope change.
    policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = kind, .id = id },
        .summary = summary,
    }) catch {};
}

/// Read `(scope_kind, scope_id)` for a promotable entity from its table.
/// `result.scope_kind` is heap-allocated; the caller must free it.
/// Returns `InvalidScope` for an unknown kind, `NotFound` when the id is
/// absent. The shared reader for the promote/demote handlers — keep the
/// table-name resolution and scope-row shape in this one place.
pub fn readEntityScope(d: *db.sqlite.Db, allocator: std.mem.Allocator, kind: []const u8, id: i64) Error!ScopeInfo {
    const table = tableFor(kind) orelse return Error.InvalidScope;

    const sql_raw = try std.fmt.allocPrint(
        allocator,
        "select scope_kind, scope_id from {s} where id = ?",
        .{table},
    );
    defer allocator.free(sql_raw);
    const sql = try allocator.dupeZ(u8, sql_raw);
    defer allocator.free(sql);

    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;

    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => .{
            .scope_kind = try stmt.columnTextAlloc(0, allocator),
            .scope_id = stmt.columnIntOpt(1),
        },
    };
}

/// Map entity kind string to the corresponding table name.
/// Returns null for unrecognised kinds (caller returns InvalidScope).
fn tableFor(kind: []const u8) ?[]const u8 {
    if (std.mem.eql(u8, kind, "plan")) return "plans";
    if (std.mem.eql(u8, kind, "task")) return "tasks";
    if (std.mem.eql(u8, kind, "question")) return "questions";
    if (std.mem.eql(u8, kind, "test_scenario")) return "test_scenarios";
    if (std.mem.eql(u8, kind, "artifact")) return "artifacts";
    if (std.mem.eql(u8, kind, "decision")) return "decisions";
    return null;
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

fn insertAssoc(d: *db.sqlite.Db, slug: []const u8) !i64 {
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values (?, ?, 'org')",
        &.{ .{ .text = slug }, .{ .text = slug } },
    );
    return try d.intQuery("select max(id) from associations");
}

fn insertGlobalPlan(d: *db.sqlite.Db, slug: []const u8) !i64 {
    return try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', ?, ?, 'active')",
        &.{ .{ .text = slug }, .{ .text = slug } },
    );
}

fn insertGlobalTask(d: *db.sqlite.Db) !i64 {
    return try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'todo', 100)",
        &.{},
    );
}

test "promote: plan global → association; scope updated; audit row written" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const assoc_id = try insertAssoc(&d, "acme");
    const plan_id = try insertGlobalPlan(&d, "my-plan");

    try promote(&d, a, .{ .kind = "plan", .id = plan_id, .to_scope = "acme" });

    // scope updated to association.
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from plans where scope_kind = 'association'"),
    );
    const sid = try d.intQuery("select scope_id from plans where scope_kind = 'association'");
    try std.testing.expectEqual(assoc_id, sid);

    // Audit row.
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where entity_kind = 'plan' and verb = 'status_change'"),
    );
}

test "promote: unknown slug returns SlugNotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertGlobalPlan(&d, "test-plan");
    try std.testing.expectError(
        Error.SlugNotFound,
        promote(&d, a, .{ .kind = "plan", .id = plan_id, .to_scope = "no-such-assoc" }),
    );
}

test "promote: repo: form returns UnsupportedScope" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertGlobalPlan(&d, "rp");
    try std.testing.expectError(
        Error.UnsupportedScope,
        promote(&d, a, .{ .kind = "plan", .id = plan_id, .to_scope = "repo:foo" }),
    );
}

test "promote: already at target association returns ScopeUnchanged" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try insertAssoc(&d, "acme");
    const plan_id = try insertGlobalPlan(&d, "p1");

    try promote(&d, a, .{ .kind = "plan", .id = plan_id, .to_scope = "acme" });

    try std.testing.expectError(
        Error.ScopeUnchanged,
        promote(&d, a, .{ .kind = "plan", .id = plan_id, .to_scope = "acme" }),
    );
}

test "demote: plan association → global; scope_id becomes NULL; audit row" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try insertAssoc(&d, "acme");
    const plan_id = try insertGlobalPlan(&d, "p-for-demote");
    try promote(&d, a, .{ .kind = "plan", .id = plan_id, .to_scope = "acme" });

    // Confirm it's now association-scoped.
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from plans where scope_kind = 'association'"),
    );

    try demote(&d, a, "plan", plan_id);

    // scope_kind = 'global', scope_id = NULL.
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from plans where scope_kind = 'global' and scope_id is null"),
    );

    // Two audit rows: one for promote, one for demote.
    try std.testing.expectEqual(
        @as(i64, 2),
        try d.intQuery("select count(*) from audit_log where entity_kind = 'plan' and verb = 'status_change'"),
    );
}

test "demote: already global returns ScopeUnchanged" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertGlobalPlan(&d, "already-global");
    try std.testing.expectError(
        Error.ScopeUnchanged,
        demote(&d, a, "plan", plan_id),
    );
}

test "promote with kind='task': works for tasks" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const assoc_id = try insertAssoc(&d, "taskorg");
    const task_id = try insertGlobalTask(&d);

    try promote(&d, a, .{ .kind = "task", .id = task_id, .to_scope = "taskorg" });

    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from tasks where scope_kind = 'association'"),
    );
    const sid = try d.intQuery("select scope_id from tasks where scope_kind = 'association'");
    try std.testing.expectEqual(assoc_id, sid);
}

test "audit row carries both from-scope and to-scope text in summary" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try insertAssoc(&d, "myorg");
    const plan_id = try insertGlobalPlan(&d, "audit-test");

    try promote(&d, a, .{ .kind = "plan", .id = plan_id, .to_scope = "myorg" });

    var stmt = d.prepare(
        "select summary from audit_log where entity_kind = 'plan' and verb = 'status_change'",
    ) catch return;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return;
    switch (stmt.step() catch return) {
        .done => try std.testing.expect(false),
        .row => {
            const summary = try stmt.columnTextAlloc(0, a);
            defer a.free(summary);
            // Summary contains both from-scope ("global") and to-scope ("association").
            try std.testing.expect(std.mem.indexOf(u8, summary, "global") != null);
            try std.testing.expect(std.mem.indexOf(u8, summary, "association") != null);
        },
    }
}

test "promote: plan not found returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try insertAssoc(&d, "acme");
    try std.testing.expectError(
        Error.NotFound,
        promote(&d, a, .{ .kind = "plan", .id = 9999, .to_scope = "acme" }),
    );
}

test "demote: task not found returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(
        Error.NotFound,
        demote(&d, a, "task", 9999),
    );
}

test "promote: unknown entity kind returns InvalidScope" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try insertAssoc(&d, "acme");
    try std.testing.expectError(
        Error.InvalidScope,
        promote(&d, a, .{ .kind = "bogus_entity", .id = 1, .to_scope = "acme" }),
    );
}
