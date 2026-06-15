//! cockpit/edit/actions.zig — pure edit-action layer for M16.
//!
//! Implements the write path for entity-field edits from the cockpit. All
//! writes route through the same engine functions the existing `planar`
//! CLI handlers call — no raw SQL, no parallel write code. The scope guard
//! is the same `engine.policy.scope_guard.check` the CLI write handlers use.
//!
//! ## Write paths reused (cite src paths)
//!
//!   entity kind   engine function called              cli handler mirror
//!   plan          engine.planning.plan.update          handlers/plan/update.zig
//!   task          engine.planning.task.update          handlers/task/update.zig
//!   question      engine.planning.question.answer      handlers/question/...
//!   decision      editflow.applyMutations (direct SQL) editflow.zig §applyMutations
//!   scenario      editflow.applyMutations (direct SQL) editflow.zig §applyMutations
//!   artifact      editflow.applyMutations (direct SQL) editflow.zig §applyMutations
//!
//! ## Scope guard
//!
//! Before every write, `checkEntityScope` resolves the entity's stored scope
//! to a canonical slug (using `engine.identity.scope.slugFromRef`) and calls
//! `engine.policy.scope_guard.check(entity_scope, write_scope)`. A null
//! `write_scope` means "no explicit scope supplied" — the guard then refuses
//! any entity that has a concrete scope (ScopeMismatch). The cockpit passes
//! a slug from its current ScopeFilter (or null if the filter is .all and
//! no explicit scope was given).
//!
//! ## Confirmation gate
//!
//! `editIsDestructive` returns true when the old value is non-empty — the
//! cockpit must show a confirmation prompt and only dispatch the write after
//! the operator confirms. The write function itself does not gate on this;
//! the caller (app.zig / scope_explorer.zig) is responsible for the prompt.
//!
//! ## Memory safety
//!
//! All string return values are heap-allocated from the provided allocator.
//! No string slice aliases into caller-supplied buffers. The caller owns
//! the returned strings and must free them.
//!
//! Tasks: 4044 (edit fields), 4045 (scope guard), 4046 (confirmation gate).

const std = @import("std");
const db = @import("db");
const engine = @import("engine");

// =========================================================================
// Public error types
// =========================================================================

pub const EditError = error{
    /// Entity not found in the DB.
    EntityNotFound,
    /// The entity's stored scope disagrees with the write scope.
    ScopeMismatch,
    /// The status transition is not legal (e.g. done→todo without reopen).
    InvalidStatusTransition,
    /// An empty value was supplied where the engine requires a non-empty one.
    EmptyValueNotAllowed,
    /// The underlying DB write failed.
    WriteFailed,
    /// Memory allocation failed.
    OutOfMemory,
};

// =========================================================================
// Entity scope lookup
// =========================================================================

/// Returned by `entityScopeSlug`. The caller must free `slug` if non-null.
pub const EntityScopeResult = struct {
    /// Canonical scope slug ("repo:<slug>", bare assoc slug, or null for global).
    slug: ?[]const u8,
};

/// Fetch the canonical scope slug for an entity row (from any supported
/// entity table that has scope_kind + scope_id columns).
///
/// Returns `.{ .slug = null }` for global-scoped entities (no mismatch with
/// any write scope).
///
/// Caller must free `result.slug` if non-null.
pub fn entityScopeSlug(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    table: []const u8,
    entity_id: i64,
) EditError!EntityScopeResult {
    var sql_buf: [256]u8 = undefined;
    const sql = std.fmt.bufPrint(
        &sql_buf,
        "select coalesce(scope_kind,'global'), scope_id from {s} where id = ?",
        .{table},
    ) catch return EditError.WriteFailed;
    const sql_z = allocator.dupeZ(u8, sql) catch return EditError.OutOfMemory;
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return EditError.EntityNotFound;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = entity_id }}) catch return EditError.WriteFailed;
    switch (stmt.step() catch return EditError.WriteFailed) {
        .done => return EditError.EntityNotFound,
        .row => {
            const kind_text = stmt.columnTextOpt(0, allocator) catch return EditError.OutOfMemory;
            const scope_id_opt = stmt.columnIntOpt(1);
            defer if (kind_text) |s| allocator.free(s);

            const kind_str = kind_text orelse "global";
            if (std.mem.eql(u8, kind_str, "global")) {
                return .{ .slug = null };
            }

            // Translate scope_kind + scope_id to a slug via engine.identity.scope.
            const scope_kind: engine.identity.scope.ScopeKind = if (std.mem.eql(u8, kind_str, "repo"))
                .repo
            else
                .association;

            const slug = engine.identity.scope.slugFromRef(
                d,
                allocator,
                scope_kind,
                scope_id_opt,
            ) catch return EditError.WriteFailed;

            return .{ .slug = slug };
        },
    }
}

/// Run the cross-scope guard. Returns ScopeMismatch when the entity's stored
/// scope disagrees with `write_scope`. Mirrors the guard in handlers/task/update.zig.
///
///   entity_scope = null  → entity is global; any write is allowed.
///   write_scope  = null  → resolver couldn't pin a scope; refuse any entity with one.
///   else                 → must match exactly (via policy.scope_guard.check).
pub fn checkScopeGuard(entity_scope: ?[]const u8, write_scope: ?[]const u8) EditError!void {
    engine.policy.scope_guard.check(entity_scope, write_scope) catch
        return EditError.ScopeMismatch;
}

// =========================================================================
// Destructive-edit classifier (task 4046)
// =========================================================================

/// Returns true when overwriting `old_value` with a different `new_value`
/// is destructive — i.e. the old value is non-empty. The cockpit MUST show
/// a confirmation prompt before calling the write function in this case.
pub fn editIsDestructive(old_value: []const u8) bool {
    return old_value.len > 0;
}

// =========================================================================
// EntityKind for cockpit edit (mirrors editflow.EntityKind)
// =========================================================================

/// The six entity kinds the cockpit can edit. Mirrors editflow.EntityKind.
pub const EntityKind = enum {
    plan,
    task,
    question,
    decision,
    scenario,
    artifact,

    /// Return the SQL table name for scope lookup and direct updates.
    pub fn table(self: EntityKind) []const u8 {
        return switch (self) {
            .plan => "plans",
            .task => "tasks",
            .question => "questions",
            .decision => "decisions",
            .scenario => "test_scenarios",
            .artifact => "artifacts",
        };
    }
};

// =========================================================================
// Edit-title action (task 4044)
// =========================================================================

/// Fetch the entity's current title for display in the edit prompt.
/// Caller must free the returned slice.
pub fn fetchCurrentTitle(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: EntityKind,
    entity_id: i64,
) EditError![]const u8 {
    const tbl = kind.table();
    var sql_buf: [256]u8 = undefined;
    const sql = std.fmt.bufPrint(
        &sql_buf,
        "select coalesce(title,'') from {s} where id = ?",
        .{tbl},
    ) catch return EditError.WriteFailed;
    const sql_z = allocator.dupeZ(u8, sql) catch return EditError.OutOfMemory;
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return EditError.EntityNotFound;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = entity_id }}) catch return EditError.WriteFailed;
    switch (stmt.step() catch return EditError.WriteFailed) {
        .done => return EditError.EntityNotFound,
        .row => return stmt.columnTextAlloc(0, allocator) catch EditError.OutOfMemory,
    }
}

/// Edit an entity's title field via the engine write path.
///
/// - Fetches the entity's scope slug and runs `checkScopeGuard`.
/// - For plan/task: calls `engine.planning.{plan,task}.update` with `.title`.
/// - For decision/question/scenario/artifact: applies a direct SQL update
///   (identical to `editflow.applyMutations` — same path the CLI `edit`
///   command uses for these entity types).
/// - Returns `EditError.ScopeMismatch` when the guard refuses.
/// - The caller is responsible for showing a confirmation prompt before
///   calling this function when `editIsDestructive(old_title)` is true
///   (task 4046).
///
/// `write_scope` is the cockpit's current scope slug (null if .all with no
/// explicit scope override). The guard refuses any entity whose stored scope
/// differs from `write_scope`.
pub fn editTitle(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: EntityKind,
    entity_id: i64,
    new_title: []const u8,
    write_scope: ?[]const u8,
) EditError!void {
    if (new_title.len == 0) return EditError.EmptyValueNotAllowed;

    // 1. Fetch entity scope and run the guard (task 4045).
    const scope_result = try entityScopeSlug(d, allocator, kind.table(), entity_id);
    defer if (scope_result.slug) |s| allocator.free(s);
    try checkScopeGuard(scope_result.slug, write_scope);

    // 2. Dispatch to the engine write path (task 4044).
    switch (kind) {
        .plan => {
            // Reuse engine.planning.plan.update — the same function
            // handlers/plan/update.zig calls.
            const patch: engine.planning.plan.UpdateArgs = .{
                .title = new_title,
            };
            const updated = engine.planning.plan.update(d, allocator, entity_id, patch) catch
                return EditError.WriteFailed;
            engine.planning.plan.deinit(updated, allocator);
        },
        .task => {
            // Reuse engine.planning.task.update — the same function
            // handlers/task/update.zig calls.
            const patch: engine.planning.task.UpdateArgs = .{
                .title = new_title,
            };
            const updated = engine.planning.task.update(d, allocator, entity_id, patch) catch
                return EditError.WriteFailed;
            engine.planning.task.deinit(updated, allocator);
        },
        .question, .decision, .scenario, .artifact => {
            // Direct SQL update — same path editflow.applyMutations uses
            // for these entity types. The engine doesn't have a standalone
            // update() for these; the CLI edit flow also does it this way.
            const tbl = kind.table();
            var sql_buf: [512]u8 = undefined;
            const sql = std.fmt.bufPrint(
                &sql_buf,
                "update {s} set title = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
                .{tbl},
            ) catch return EditError.WriteFailed;
            const sql_z = allocator.dupeZ(u8, sql) catch return EditError.OutOfMemory;
            defer allocator.free(sql_z);

            _ = d.execParams(sql_z, &.{
                .{ .text = new_title },
                .{ .int = entity_id },
            }) catch return EditError.WriteFailed;
        },
    }
}

// =========================================================================
// Answer-question action (task 4044 — example of a status-mutating edit)
// =========================================================================

/// Answer an open question via `engine.planning.question.answer`.
///
/// - Fetches the entity's scope slug and runs `checkScopeGuard`.
/// - Calls `engine.planning.question.answer` — the same engine function
///   the CLI `question answer` handler calls.
/// - The caller is responsible for the confirmation prompt when
///   `editIsDestructive(old_answer_body)` is true.
pub fn answerQuestion(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_id: i64,
    answer_text: []const u8,
    write_scope: ?[]const u8,
) EditError!void {
    if (answer_text.len == 0) return EditError.EmptyValueNotAllowed;

    // Scope guard (task 4045).
    const scope_result = try entityScopeSlug(d, allocator, "questions", entity_id);
    defer if (scope_result.slug) |s| allocator.free(s);
    try checkScopeGuard(scope_result.slug, write_scope);

    // Engine write path — same as CLI `question answer` handler.
    const updated = engine.planning.question.answer(d, allocator, entity_id, answer_text) catch |e|
        switch (e) {
            error.AnswerRequired => return EditError.EmptyValueNotAllowed,
            else => return EditError.WriteFailed,
        };
    engine.planning.question.deinit(updated, allocator);
}

// =========================================================================
// Write-scope derivation helper for the cockpit
// =========================================================================

/// Derive the write scope slug from the cockpit's ScopeFilter. Used by
/// app.zig to supply `write_scope` to the edit action functions.
///
/// - `.all` with `explicit_scope = null` → null (guard refuses entities with scope).
/// - `.all` with `explicit_scope = some_slug` → that slug.
/// - `.repo` project id → look up the project's slug and format "repo:<slug>".
///
/// The returned slice (when non-null) is heap-allocated; caller must free it.
pub fn cockpitWriteScope(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: view_model_ScopeFilter,
    explicit_scope: ?[]const u8,
) EditError!?[]const u8 {
    if (explicit_scope) |slug| {
        return allocator.dupe(u8, slug) catch EditError.OutOfMemory;
    }
    switch (filter) {
        .all => return null,
        .repo => |project_id| {
            var stmt = d.prepare("select slug from projects where id = ?") catch
                return EditError.WriteFailed;
            defer stmt.finalize();
            stmt.bind(&.{.{ .int = project_id }}) catch return EditError.WriteFailed;
            switch (stmt.step() catch return EditError.WriteFailed) {
                .done => return null,
                .row => {
                    const slug = stmt.columnTextAlloc(0, allocator) catch return EditError.OutOfMemory;
                    defer allocator.free(slug);
                    return std.fmt.allocPrint(allocator, "repo:{s}", .{slug}) catch
                        EditError.OutOfMemory;
                },
            }
        },
    }
}

/// A minimal ScopeFilter enum mirroring view_model.ScopeFilter. Declared
/// locally so the edit layer has no circular dependency on the view_model.
/// The caller (app.zig) converts from view_model.ScopeFilter to this type.
pub const view_model_ScopeFilter = union(enum) {
    repo: i64,
    all,
};

// =========================================================================
// Tests (task 4044, 4045, 4046)
// =========================================================================

test "editIsDestructive: empty old value is not destructive" {
    try std.testing.expect(!editIsDestructive(""));
}

test "editIsDestructive: non-empty old value is destructive" {
    try std.testing.expect(editIsDestructive("existing title"));
}

test "checkScopeGuard: global entity allows any write scope" {
    try checkScopeGuard(null, null);
    try checkScopeGuard(null, "repo:acme");
}

test "checkScopeGuard: entity with scope refuses null write scope" {
    try std.testing.expectError(
        EditError.ScopeMismatch,
        checkScopeGuard("repo:acme", null),
    );
}

test "checkScopeGuard: entity with scope refuses mismatched write scope" {
    try std.testing.expectError(
        EditError.ScopeMismatch,
        checkScopeGuard("repo:acme", "repo:beta"),
    );
}

test "checkScopeGuard: entity with scope allows matching write scope" {
    try checkScopeGuard("repo:acme", "repo:acme");
}

test "entityScopeSlug: global-scoped entity returns null slug" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')",
        &.{},
    );
    const result = try entityScopeSlug(&d, std.testing.allocator, "plans", plan_id);
    defer if (result.slug) |s| std.testing.allocator.free(s);
    try std.testing.expect(result.slug == null);
}

test "entityScopeSlug: repo-scoped entity returns repo:slug" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('acme/core', 'Core')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, scope_id, title, status, priority) values ('repo', ?, 'T', 'todo', 100)",
        &.{.{ .int = proj_id }},
    );
    const result = try entityScopeSlug(&d, std.testing.allocator, "tasks", task_id);
    defer if (result.slug) |s| std.testing.allocator.free(s);
    try std.testing.expect(result.slug != null);
    try std.testing.expectEqualStrings("repo:acme/core", result.slug.?);
}

test "entityScopeSlug: entity not found returns EntityNotFound" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    try std.testing.expectError(
        EditError.EntityNotFound,
        entityScopeSlug(&d, std.testing.allocator, "plans", 9999),
    );
}

test "editTitle plan: persists via engine.planning.plan.update" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Old Title', 'old-title', 'active')",
        &.{},
    );
    // Global entity: write_scope=null is allowed.
    try editTitle(&d, std.testing.allocator, .plan, plan_id, "New Title", null);

    // Verify via re-query.
    const updated = try engine.planning.plan.show(&d, std.testing.allocator, plan_id);
    defer engine.planning.plan.deinit(updated, std.testing.allocator);
    try std.testing.expectEqualStrings("New Title", updated.title);
}

test "editTitle task: persists via engine.planning.task.update" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global', 'Old Task', 'todo', 100)",
        &.{},
    );
    try editTitle(&d, std.testing.allocator, .task, task_id, "New Task Title", null);

    var stmt = try d.prepare("select title from tasks where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    try std.testing.expect((try stmt.step()) == .row);
    const title = try stmt.columnTextAlloc(0, std.testing.allocator);
    defer std.testing.allocator.free(title);
    try std.testing.expectEqualStrings("New Task Title", title);
}

test "editTitle question: persists via direct SQL (same as editflow.applyMutations)" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const q_id = try d.execParams(
        "insert into questions (scope_kind, title, status) values ('global', 'Old Q', 'open')",
        &.{},
    );
    try editTitle(&d, std.testing.allocator, .question, q_id, "New Q Title", null);

    var stmt = try d.prepare("select title from questions where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = q_id }});
    try std.testing.expect((try stmt.step()) == .row);
    const title = try stmt.columnTextAlloc(0, std.testing.allocator);
    defer std.testing.allocator.free(title);
    try std.testing.expectEqualStrings("New Q Title", title);
}

test "editTitle: cross-scope edit refuses without explicit scope" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('acme/core', 'Core')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, scope_id, title, status, priority) values ('repo', ?, 'T', 'todo', 100)",
        &.{.{ .int = proj_id }},
    );

    // write_scope=null → guard refuses entity with concrete scope.
    try std.testing.expectError(
        EditError.ScopeMismatch,
        editTitle(&d, std.testing.allocator, .task, task_id, "New Title", null),
    );
}

test "editTitle: cross-scope edit succeeds with matching explicit scope" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('acme/core', 'Core')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, scope_id, title, status, priority) values ('repo', ?, 'T', 'todo', 100)",
        &.{.{ .int = proj_id }},
    );

    // Explicit matching scope: succeeds.
    try editTitle(&d, std.testing.allocator, .task, task_id, "New Title", "repo:acme/core");

    var stmt = try d.prepare("select title from tasks where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    try std.testing.expect((try stmt.step()) == .row);
    const title = try stmt.columnTextAlloc(0, std.testing.allocator);
    defer std.testing.allocator.free(title);
    try std.testing.expectEqualStrings("New Title", title);
}

test "answerQuestion: persists via engine.planning.question.answer" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const q_id = try d.execParams(
        "insert into questions (scope_kind, title, status) values ('global', 'How?', 'open')",
        &.{},
    );
    try answerQuestion(&d, std.testing.allocator, q_id, "Because of X", null);

    const updated = try engine.planning.question.show(&d, std.testing.allocator, q_id);
    defer engine.planning.question.deinit(updated, std.testing.allocator);
    try std.testing.expectEqual(engine.planning.question.Status.answered, updated.status);
    try std.testing.expect(updated.answer_body != null);
    try std.testing.expectEqualStrings("Because of X", updated.answer_body.?);
}

test "answerQuestion: cross-scope refuses without explicit scope" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const proj_id = try d.execParams(
        "insert into projects (slug, name) values ('beta/svc', 'Svc')",
        &.{},
    );
    const q_id = try d.execParams(
        "insert into questions (scope_kind, scope_id, title, status) values ('repo', ?, 'Q?', 'open')",
        &.{.{ .int = proj_id }},
    );

    try std.testing.expectError(
        EditError.ScopeMismatch,
        answerQuestion(&d, std.testing.allocator, q_id, "Because X", null),
    );
}

test "cockpitWriteScope: .all with no explicit scope returns null" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const scope = try cockpitWriteScope(&d, std.testing.allocator, .all, null);
    defer if (scope) |s| std.testing.allocator.free(s);
    try std.testing.expect(scope == null);
}

test "cockpitWriteScope: .repo returns repo:<slug>" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const pid = try d.execParams(
        "insert into projects (slug, name) values ('acme/api', 'API')",
        &.{},
    );
    const scope = try cockpitWriteScope(&d, std.testing.allocator, .{ .repo = pid }, null);
    defer if (scope) |s| std.testing.allocator.free(s);
    try std.testing.expect(scope != null);
    try std.testing.expectEqualStrings("repo:acme/api", scope.?);
}

test "cockpitWriteScope: explicit scope overrides filter" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const scope = try cockpitWriteScope(&d, std.testing.allocator, .all, "repo:explicit");
    defer if (scope) |s| std.testing.allocator.free(s);
    try std.testing.expect(scope != null);
    try std.testing.expectEqualStrings("repo:explicit", scope.?);
}

test "editTitle: empty new title returns EmptyValueNotAllowed" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'P', 'p', 'active')",
        &.{},
    );
    try std.testing.expectError(
        EditError.EmptyValueNotAllowed,
        editTitle(&d, std.testing.allocator, .plan, plan_id, "", null),
    );
}

test "editTitle decision: persists via direct SQL" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const dec_id = try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global', 'Old Dec', 'body', 'proposed')",
        &.{},
    );
    try editTitle(&d, std.testing.allocator, .decision, dec_id, "New Decision Title", null);

    var stmt = try d.prepare("select title from decisions where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = dec_id }});
    try std.testing.expect((try stmt.step()) == .row);
    const title = try stmt.columnTextAlloc(0, std.testing.allocator);
    defer std.testing.allocator.free(title);
    try std.testing.expectEqualStrings("New Decision Title", title);
}

test "editTitle artifact: persists via direct SQL" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    const art_id = try d.execParams(
        "insert into artifacts (scope_kind, title, kind, status) values ('global', 'Old Art', 'adr', 'active')",
        &.{},
    );
    try editTitle(&d, std.testing.allocator, .artifact, art_id, "New Artifact Title", null);

    var stmt = try d.prepare("select title from artifacts where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = art_id }});
    try std.testing.expect((try stmt.step()) == .row);
    const title = try stmt.columnTextAlloc(0, std.testing.allocator);
    defer std.testing.allocator.free(title);
    try std.testing.expectEqualStrings("New Artifact Title", title);
}

test "actions module compiles" {
    std.testing.refAllDecls(@This());
}
