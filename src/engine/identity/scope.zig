//! engine/identity/scope — cwd-derived scope resolution.
//!
//! "Scope" in Planar is the association slug that an entity belongs to.
//! Most write verbs target a single scope; reads default to "global"
//! (no scope filter) unless one is requested.
//!
//! The cwd-derive algorithm:
//!   1. Find the project whose `root_path` is the longest prefix of cwd.
//!   2. Look up that project's association memberships.
//!   3. If exactly one membership, return its slug.
//!   4. If zero or many, return null (caller treats as global; ambiguity
//!      surfaces in `Reason` for human-readable diagnostics).
//!
//! This module owns the SQL; the CLI-side `cmd/planar/scope.zig`
//! wrapper applies the `--scope` override precedence and runs the
//! cross-scope guard.

const std = @import("std");
const db = @import("db");

/// A scope is an association slug, or null when the entity is global.
/// Slug strings are borrowed from the allocator passed to `derive`.
pub const Scope = ?[]const u8;

pub const Error = error{
    QueryFailed,
    InvalidPath,
    SlugNotFound,
    UnsupportedScope,
} || std.mem.Allocator.Error;

/// Why the resolver returned the scope it did. Useful for `planar scope
/// show` and for diagnostic messages on scope-guard refusals.
pub const Reason = enum {
    /// No project's root_path was a prefix of cwd.
    no_project_match,
    /// Matching project has zero association memberships.
    project_unassociated,
    /// Matching project has multiple memberships; ambiguous without
    /// an explicit `--scope` override.
    project_multiple_associations,
    /// Matching project has exactly one membership; that slug is used.
    project_single_association,
};

pub const Resolution = struct {
    scope: Scope,
    reason: Reason,
    project_slug: ?[]const u8 = null,
};

/// ScopeRef is returned by resolveSlug: the resolved kind + database id.
pub const ScopeKind = enum {
    global,
    association,
    /// repo: form is reserved for future use; M3 returns UnsupportedScope.
    repo,
};

pub const ScopeRef = struct {
    kind: ScopeKind,
    /// Null for global scope; non-null for association/repo scope.
    id: ?i64,
};

/// Derive the scope from `cwd`. Returns `.scope = null` plus a `reason`
/// explaining why when no unambiguous association can be selected.
/// Caller owns any non-null `scope` and `project_slug` strings.
///
/// Algorithm (mirrors Go's `scopearg.DeriveFromCwd` + `scopearg.ResolveForWrite`):
///   1. Load all projects with a non-null root_path, ordered longest-first.
///   2. Pick the first (longest) whose root_path is a prefix of (or equals) cwd.
///   3. Query that project's association memberships.
///   4. Branch on count: 0 → project_unassociated, 1 → project_single_association,
///      2+ → project_multiple_associations.
pub fn deriveFromCwd(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    cwd: []const u8,
) Error!Resolution {
    if (cwd.len == 0 or cwd[0] != '/') return Error.InvalidPath;

    // Step 1 + 2: find the project with the longest root_path that is a
    // prefix of cwd. We iterate rows ordered by length(root_path) DESC so
    // the first match is the longest.
    const ProjectMatch = struct { id: i64, slug: []const u8 };

    var match: ?ProjectMatch = null;
    {
        var stmt = d.prepare(
            "select id, slug, root_path from projects " ++
                "where root_path is not null order by length(root_path) desc",
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{}) catch return Error.QueryFailed;

        while (match == null) {
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => break,
                .row => {
                    // column 2 = root_path
                    const rp = try stmt.columnTextOpt(2, allocator);
                    if (rp) |root| {
                        defer allocator.free(root);
                        if (pathHasPrefix(cwd, root)) {
                            const pid = stmt.columnInt(0);
                            const pslug = try stmt.columnTextAlloc(1, allocator);
                            match = .{ .id = pid, .slug = pslug };
                        }
                    }
                },
            }
        }
    }

    if (match == null) {
        return .{ .scope = null, .reason = .no_project_match };
    }

    const pid = match.?.id;
    // pslug is now owned by this scope; we will transfer it to the returned
    // Resolution.project_slug. We must NOT free it inside this function.
    const pslug = match.?.slug;

    // Step 3: load all association slugs for this project.
    var assoc_slugs: std.ArrayList([]const u8) = .empty;
    // errdefer for the list contents only — pslug is a separate allocation.
    errdefer {
        for (assoc_slugs.items) |s| allocator.free(s);
        assoc_slugs.deinit(allocator);
    }

    {
        var stmt2 = d.prepare(
            \\select a.slug from associations a
            \\join project_associations pa on pa.association_id = a.id
            \\where pa.project_id = ?
            \\order by a.id
        ) catch {
            allocator.free(pslug);
            return Error.QueryFailed;
        };
        defer stmt2.finalize();
        stmt2.bind(&.{.{ .int = pid }}) catch {
            allocator.free(pslug);
            return Error.QueryFailed;
        };

        while (true) {
            switch (stmt2.step() catch {
                allocator.free(pslug);
                return Error.QueryFailed;
            }) {
                .done => break,
                .row => {
                    const s = try stmt2.columnTextAlloc(0, allocator);
                    try assoc_slugs.append(allocator, s);
                },
            }
        }
    }

    // Step 4: branch on count.
    // In all cases pslug is transferred out via project_slug — do NOT free it here.
    switch (assoc_slugs.items.len) {
        0 => {
            assoc_slugs.deinit(allocator);
            return .{
                .scope = null,
                .reason = .project_unassociated,
                .project_slug = pslug,
            };
        },
        1 => {
            // Transfer ownership of the one slug; free the (now-empty-but-allocated)
            // list backing array.
            const scope_slug = assoc_slugs.items[0];
            assoc_slugs.deinit(allocator);
            return .{
                .scope = scope_slug,
                .reason = .project_single_association,
                .project_slug = pslug,
            };
        },
        else => {
            // Multiple — free all association slugs, return null scope.
            for (assoc_slugs.items) |s| allocator.free(s);
            assoc_slugs.deinit(allocator);
            return .{
                .scope = null,
                .reason = .project_multiple_associations,
                .project_slug = pslug,
            };
        },
    }
}

/// Resolve a --scope flag value to a (kind, id) pair.
///
/// Accepted forms (mirrors Go's scopearg.Parse / scopearg.Resolve):
///   "global"         → {kind: .global,      id: null}
///   "<slug>"         → {kind: .association, id: <assoc_id>}
///   "assoc:<slug>"   → {kind: .association, id: <assoc_id>}
///   "repo:<slug>"    → error.UnsupportedScope (Cycle B)
///
/// Returns error.SlugNotFound when the association slug does not exist.
/// Returns error.UnsupportedScope for the repo: prefix form.
pub fn resolveSlug(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    slug: []const u8,
) Error!ScopeRef {
    _ = allocator; // not needed for the lookup, but kept in signature for future use

    // "global" → global scope, no id.
    if (std.mem.eql(u8, slug, "global")) {
        return .{ .kind = .global, .id = null };
    }

    // "repo:<slug>" → unsupported in M3.
    if (std.mem.startsWith(u8, slug, "repo:")) {
        return Error.UnsupportedScope;
    }

    // Strip optional "assoc:" prefix.
    const bare = if (std.mem.startsWith(u8, slug, "assoc:"))
        slug["assoc:".len..]
    else
        slug;

    if (bare.len == 0) return Error.SlugNotFound;

    // Look up associations.id by slug.
    var stmt = d.prepare("select id from associations where slug = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = bare }}) catch return Error.QueryFailed;

    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.SlugNotFound,
        .row => .{ .kind = .association, .id = stmt.columnInt(0) },
    };
}

/// Reverse of `resolveSlug`: given a stored scope_kind + scope_id,
/// return the slug. Returns null for `.global` (no slug per the rule
/// that global is the absence of scope). Returns SlugNotFound if the
/// id does not exist in the corresponding table.
///
/// Caller owns the returned slice; pass through `allocator.free` once
/// done. Used by the cross-scope write guard to convert an entity's
/// stored scope into a slug it can compare against the resolved
/// operator write scope.
pub fn slugFromRef(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: ScopeKind,
    id: ?i64,
) Error!?[]const u8 {
    return switch (kind) {
        .global => null,
        .association => slugFromAssocId(d, allocator, id orelse return Error.SlugNotFound),
        .repo => slugFromProjectId(d, allocator, id orelse return Error.SlugNotFound),
    };
}

fn slugFromAssocId(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!?[]const u8 {
    var stmt = d.prepare("select slug from associations where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.SlugNotFound,
        .row => try stmt.columnTextAlloc(0, allocator),
    };
}

fn slugFromProjectId(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!?[]const u8 {
    var stmt = d.prepare("select slug from projects where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.SlugNotFound,
        .row => try stmt.columnTextAlloc(0, allocator),
    };
}

// =========================================================================
// Suggest (cwd-based association proposals from existing memberships)
// =========================================================================

/// One candidate association surfaced by `suggest`. Mirrors the Go side
/// `scope.ScopeProposal`. All string fields are owned by the allocator
/// passed to `suggest` — release via `deinitSuggestion` / `deinitSuggestions`.
pub const Suggestion = struct {
    slug: []const u8,
    association_id: i64,
    /// Human-readable explanation derived from `project_associations.source`.
    reason: []const u8,
};

pub fn deinitSuggestion(s: Suggestion, allocator: std.mem.Allocator) void {
    allocator.free(s.slug);
    allocator.free(s.reason);
}

pub fn deinitSuggestions(items: []const Suggestion, allocator: std.mem.Allocator) void {
    for (items) |s| deinitSuggestion(s, allocator);
    allocator.free(items);
}

/// Inspect `root_path` and return the associations the project at that
/// path is already a member of. Read-only: no rows are created. Returns
/// an empty slice when no project is registered at `root_path` or when
/// the project has no memberships.
///
/// Mirrors the Go side `scope.Suggest` semantics (membership-based;
/// returns proposals ordered by association slug). Note: the lookup
/// uses `root_path = ?` exact match. Sub-paths of a registered project
/// will NOT match here; the caller should pass the project's root.
pub fn suggest(d: *db.sqlite.Db, allocator: std.mem.Allocator, root_path: []const u8) Error![]Suggestion {
    var project_id: i64 = 0;
    {
        var stmt = d.prepare("select id from projects where root_path = ?") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .text = root_path }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return try allocator.alloc(Suggestion, 0),
            .row => project_id = stmt.columnInt(0),
        }
    }

    var stmt2 = d.prepare(
        \\select a.id, a.slug, pa.source
        \\from project_associations pa
        \\join associations a on a.id = pa.association_id
        \\where pa.project_id = ?
        \\order by a.slug
    ) catch return Error.QueryFailed;
    defer stmt2.finalize();
    stmt2.bind(&.{.{ .int = project_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(Suggestion) = .empty;
    errdefer {
        for (out.items) |s| deinitSuggestion(s, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt2.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const aid = stmt2.columnInt(0);
                const slug = try stmt2.columnTextAlloc(1, allocator);
                errdefer allocator.free(slug);
                const source = try stmt2.columnTextAlloc(2, allocator);
                defer allocator.free(source);
                const reason = try allocator.dupe(u8, reasonFromSource(source));
                errdefer allocator.free(reason);
                try out.append(allocator, .{
                    .slug = slug,
                    .association_id = aid,
                    .reason = reason,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Convert a `project_associations.source` value to the human-readable
/// label `planar scope suggest` prints.
pub fn reasonFromSource(source: []const u8) []const u8 {
    if (std.mem.eql(u8, source, "user")) return "explicit member";
    if (std.mem.eql(u8, source, "auto:git-remote")) return "from git remote";
    if (std.mem.eql(u8, source, "auto:path")) return "from parent directory";
    if (std.mem.eql(u8, source, "auto:lang")) return "from language ecosystem";
    return source;
}

// The cross-scope guard rule moved to `engine.policy.scope_guard.check`
// — every guarded data-plane mutation calls policy directly. Scope
// itself stays here because it's identity-layer vocabulary: the
// "what is a scope" type. Policy enforces rules ABOUT scopes.

// =========================================================================
// Internals
// =========================================================================

/// pathHasPrefix reports whether `target` equals `root` or starts with `root/`.
/// Both arguments must be clean paths (no trailing slash except the filesystem root).
fn pathHasPrefix(target: []const u8, root: []const u8) bool {
    if (std.mem.eql(u8, target, root)) return true;
    if (!std.mem.startsWith(u8, target, root)) return false;
    if (target.len <= root.len) return false;
    return target[root.len] == '/';
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

test "deriveFromCwd rejects relative paths" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.InvalidPath, deriveFromCwd(&d, a, "relative"));
}

test "deriveFromCwd on empty DB reports no project match" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const res = try deriveFromCwd(&d, a, "/some/path");
    try std.testing.expect(res.scope == null);
    try std.testing.expectEqual(Reason.no_project_match, res.reason);
}

test "deriveFromCwd: project + 1 association → slug + reason=project_single_association" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('myrepo', 'My Repo', '/work/myrepo')",
        &.{},
    );
    const project_id = try d.intQuery("select id from projects where slug = 'myrepo'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('myorg', 'My Org', 'org')",
        &.{},
    );
    const assoc_id = try d.intQuery("select id from associations where slug = 'myorg'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = project_id }, .{ .int = assoc_id } },
    );

    const res = try deriveFromCwd(&d, a, "/work/myrepo/src/foo");
    defer if (res.scope) |s| a.free(s);
    defer if (res.project_slug) |s| a.free(s);

    try std.testing.expectEqual(Reason.project_single_association, res.reason);
    try std.testing.expect(res.scope != null);
    try std.testing.expectEqualStrings("myorg", res.scope.?);
    try std.testing.expectEqualStrings("myrepo", res.project_slug.?);
}

test "deriveFromCwd: project + 0 associations → null + reason=project_unassociated" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('lone', 'Lone', '/work/lone')",
        &.{},
    );

    const res = try deriveFromCwd(&d, a, "/work/lone/src");
    defer if (res.scope) |s| a.free(s);
    defer if (res.project_slug) |s| a.free(s);

    try std.testing.expectEqual(Reason.project_unassociated, res.reason);
    try std.testing.expect(res.scope == null);
    try std.testing.expectEqualStrings("lone", res.project_slug.?);
}

test "deriveFromCwd: project + 2 associations → null + reason=project_multiple_associations" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('multi', 'Multi', '/work/multi')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'multi'");

    _ = try d.execParams("insert into associations (slug, name, kind) values ('a1', 'A1', 'org')", &.{});
    const aid1 = try d.intQuery("select id from associations where slug = 'a1'");
    _ = try d.execParams("insert into associations (slug, name, kind) values ('a2', 'A2', 'org')", &.{});
    const aid2 = try d.intQuery("select id from associations where slug = 'a2'");

    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid1 } },
    );
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid2 } },
    );

    const res = try deriveFromCwd(&d, a, "/work/multi/lib");
    defer if (res.scope) |s| a.free(s);
    defer if (res.project_slug) |s| a.free(s);

    try std.testing.expectEqual(Reason.project_multiple_associations, res.reason);
    try std.testing.expect(res.scope == null);
    try std.testing.expect(res.project_slug != null);
    try std.testing.expectEqualStrings("multi", res.project_slug.?);
}

test "deriveFromCwd: no project prefix match → null + reason=no_project_match" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('other', 'Other', '/work/other')",
        &.{},
    );

    const res = try deriveFromCwd(&d, a, "/home/user/unrelated");
    defer if (res.scope) |s| a.free(s);
    defer if (res.project_slug) |s| a.free(s);

    try std.testing.expectEqual(Reason.no_project_match, res.reason);
    try std.testing.expect(res.scope == null);
}

test "deriveFromCwd: longest-prefix wins when multiple projects match" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('parent', 'Parent', '/work')",
        &.{},
    );
    const parent_id = try d.intQuery("select id from projects where slug = 'parent'");

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('child', 'Child', '/work/child')",
        &.{},
    );
    const child_id = try d.intQuery("select id from projects where slug = 'child'");

    _ = try d.execParams("insert into associations (slug, name, kind) values ('parent-org', 'PO', 'org')", &.{});
    const parent_assoc = try d.intQuery("select id from associations where slug = 'parent-org'");
    _ = try d.execParams("insert into associations (slug, name, kind) values ('child-org', 'CO', 'org')", &.{});
    const child_assoc = try d.intQuery("select id from associations where slug = 'child-org'");

    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = parent_id }, .{ .int = parent_assoc } },
    );
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = child_id }, .{ .int = child_assoc } },
    );

    const res = try deriveFromCwd(&d, a, "/work/child/src");
    defer if (res.scope) |s| a.free(s);
    defer if (res.project_slug) |s| a.free(s);

    // The child project (/work/child) is a longer prefix than parent (/work).
    try std.testing.expectEqual(Reason.project_single_association, res.reason);
    try std.testing.expect(res.scope != null);
    try std.testing.expectEqualStrings("child-org", res.scope.?);
    try std.testing.expectEqualStrings("child", res.project_slug.?);
}

test "resolveSlug: 'global' → kind=.global, id=null" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const ref = try resolveSlug(&d, a, "global");
    try std.testing.expectEqual(ScopeKind.global, ref.kind);
    try std.testing.expect(ref.id == null);
}

test "resolveSlug: known association slug → kind=.association, id=<assoc_id>" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'acme'");

    const ref = try resolveSlug(&d, a, "acme");
    try std.testing.expectEqual(ScopeKind.association, ref.kind);
    try std.testing.expectEqual(assoc_id, ref.id.?);
}

test "resolveSlug: 'assoc:<slug>' prefix form → same as bare slug" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into associations (slug, name, kind) values ('myassoc', 'MA', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'myassoc'");

    const bare_ref = try resolveSlug(&d, a, "myassoc");
    const prefixed_ref = try resolveSlug(&d, a, "assoc:myassoc");

    try std.testing.expectEqual(ScopeKind.association, bare_ref.kind);
    try std.testing.expectEqual(ScopeKind.association, prefixed_ref.kind);
    try std.testing.expectEqual(assoc_id, bare_ref.id.?);
    try std.testing.expectEqual(assoc_id, prefixed_ref.id.?);
}

test "resolveSlug: unknown slug → error.SlugNotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(Error.SlugNotFound, resolveSlug(&d, a, "no-such-slug"));
}

test "resolveSlug: 'repo:<slug>' prefix form → error.UnsupportedScope" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(Error.UnsupportedScope, resolveSlug(&d, a, "repo:myrepo"));
}

test "suggest: empty on unregistered cwd" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const items = try suggest(&d, a, "/unregistered");
    defer deinitSuggestions(items, a);
    try std.testing.expectEqual(@as(usize, 0), items.len);
}

test "suggest: returns the memberships of the project at root_path" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('s', 'S', '/work/s')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 's'");
    _ = try d.execParams("insert into associations (slug, name, kind) values ('alpha', 'A', 'org')", &.{});
    const aid1 = try d.intQuery("select id from associations where slug = 'alpha'");
    _ = try d.execParams("insert into associations (slug, name, kind) values ('beta', 'B', 'host')", &.{});
    const aid2 = try d.intQuery("select id from associations where slug = 'beta'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid1 } },
    );
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'auto:git-remote')",
        &.{ .{ .int = pid }, .{ .int = aid2 } },
    );

    const items = try suggest(&d, a, "/work/s");
    defer deinitSuggestions(items, a);

    try std.testing.expectEqual(@as(usize, 2), items.len);
    // Ordered by slug → alpha first.
    try std.testing.expectEqualStrings("alpha", items[0].slug);
    try std.testing.expectEqualStrings("explicit member", items[0].reason);
    try std.testing.expectEqualStrings("beta", items[1].slug);
    try std.testing.expectEqualStrings("from git remote", items[1].reason);
}

test "suggest: project with no memberships returns empty" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('lonely', 'L', '/work/lonely')",
        &.{},
    );
    const items = try suggest(&d, a, "/work/lonely");
    defer deinitSuggestions(items, a);
    try std.testing.expectEqual(@as(usize, 0), items.len);
}

test "reasonFromSource: known + unknown values" {
    try std.testing.expectEqualStrings("explicit member", reasonFromSource("user"));
    try std.testing.expectEqualStrings("from git remote", reasonFromSource("auto:git-remote"));
    try std.testing.expectEqualStrings("from parent directory", reasonFromSource("auto:path"));
    try std.testing.expectEqualStrings("from language ecosystem", reasonFromSource("auto:lang"));
    // Unknown sources pass through.
    try std.testing.expectEqualStrings("auto:custom", reasonFromSource("auto:custom"));
}
