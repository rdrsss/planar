//! engine/identity/project — Project entity + CRUD + path / git-remote lookups.
//!
//! A project (interchangeably "repo") is the unit that scope_kind='repo' refers
//! to. It is registered once via `planar init` and is thereafter the anchor for
//! association membership and cwd-derived scope resolution.
//!
//! Slug derivation (DeriveSlug semantics from the Go side):
//!   1. Lowercase the basename.
//!   2. Replace runs of non-alphanumeric characters with a single '-'.
//!   3. Trim leading and trailing '-'.
//!   4. Fall back to "project" when the result is empty.
//!
//! `findByPath` implements a longest-prefix match against `root_path` so that a
//! cwd of /a/b/c/d correctly resolves to the project registered at /a/b/c when
//! both /a and /a/b/c are in the table.
//!
//! `findByGitRemote` is an exact-match lookup on `git_remote`.
//!
//! Every mutation (add, update, delete) records one audit_log row after the SQL
//! write succeeds, mirroring the pattern in engine/identity/association.zig.

const std = @import("std");
const db = @import("db");
const policy = @import("../policy.zig");

// =========================================================================
// Types
// =========================================================================

/// One row from the `projects` table. All string fields owned by the
/// allocator passed to the producing function.
pub const Project = struct {
    id: i64,
    slug: []const u8,
    name: []const u8,
    root_path: ?[]const u8,
    git_remote: ?[]const u8,
    created_at: []const u8,
    updated_at: []const u8,
};

pub fn deinit(p: Project, allocator: std.mem.Allocator) void {
    allocator.free(p.slug);
    allocator.free(p.name);
    if (p.root_path) |s| allocator.free(s);
    if (p.git_remote) |s| allocator.free(s);
    allocator.free(p.created_at);
    allocator.free(p.updated_at);
}

pub fn deinitMany(items: []const Project, allocator: std.mem.Allocator) void {
    for (items) |p| deinit(p, allocator);
    allocator.free(items);
}

pub const AddArgs = struct {
    slug: []const u8,
    /// Defaults to slug when null.
    name: ?[]const u8 = null,
    root_path: ?[]const u8 = null,
    git_remote: ?[]const u8 = null,
};

pub const UpdateArgs = struct {
    name: ?[]const u8 = null,
    root_path: ?[]const u8 = null,
    git_remote: ?[]const u8 = null,
};

pub const ListFilter = struct {
    /// When non-null, only return projects with this root_path prefix.
    root_path_prefix: ?[]const u8 = null,
};

pub const Error =
    error{
        NotFound,
        SlugExists,
        NoFieldsToUpdate,
        QueryFailed,
    } ||
    std.mem.Allocator.Error ||
    policy.audit.Error;

// =========================================================================
// CRUD
// =========================================================================

/// Add a new project row. Returns the created project.
///
/// Returns error.SlugExists when the slug is already taken.
pub fn add(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: AddArgs) Error!Project {
    const name = args.name orelse args.slug;

    const id = d.execParams(
        \\insert into projects (slug, name, root_path, git_remote) values (?, ?, ?, ?)
    , &.{
        .{ .text = args.slug },
        .{ .text = name },
        if (args.root_path) |s| .{ .text = s } else .{ .null = {} },
        if (args.git_remote) |s| .{ .text = s } else .{ .null = {} },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.SlugExists;
        std.log.err("project.add exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const summary = try std.fmt.allocPrint(allocator, "create project '{s}'", .{args.slug});
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "project", .id = id },
        .summary = summary,
    });

    return try show(d, allocator, id);
}

/// Update an existing project. Only fields set in `patch` are written.
///
/// Returns error.NotFound when id does not exist.
/// Returns error.NoFieldsToUpdate when all fields in patch are null.
pub fn update(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    patch: UpdateArgs,
) Error!Project {
    // Verify existence before building the dynamic UPDATE.
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);

    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, "update projects set ");

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    var first = true;
    const appendSep = struct {
        fn call(buf: *std.ArrayList(u8), is_first: *bool, alloc: std.mem.Allocator) !void {
            if (!is_first.*) try buf.appendSlice(alloc, ", ");
            is_first.* = false;
        }
    }.call;

    if (patch.name) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "name = ?");
        try params.append(allocator, .{ .text = s });
    }
    if (patch.root_path) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "root_path = ?");
        try params.append(allocator, .{ .text = s });
    }
    if (patch.git_remote) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "git_remote = ?");
        try params.append(allocator, .{ .text = s });
    }

    if (first) return Error.NoFieldsToUpdate;

    try appendSep(&sql_buf, &first, allocator);
    try sql_buf.appendSlice(allocator, "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
    try params.append(allocator, .{ .int = id });

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    _ = d.execParams(sql_z, params.items) catch |e| {
        std.log.err("project.update exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const summary = try std.fmt.allocPrint(allocator, "update project id={d}", .{id});
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .update,
        .entity = .{ .kind = "project", .id = id },
        .summary = summary,
    });

    return try show(d, allocator, id);
}

/// Delete the project with `id`. Returns error.NotFound when id does not exist.
pub fn delete(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!void {
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);

    _ = d.execParams("delete from projects where id = ?", &.{.{ .int = id }}) catch |e| {
        std.log.err("project.delete exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    try policy.audit.record(d, .{
        .verb = .delete,
        .entity = .{ .kind = "project", .id = id },
        .summary = null,
    });
}

/// Fetch a project by its primary key. Returns error.NotFound when id is absent.
pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Project {
    var stmt = d.prepare(select_columns_sql ++ " where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

/// Fetch a project by its slug. Returns error.NotFound when the slug is absent.
pub fn showBySlug(d: *db.sqlite.Db, allocator: std.mem.Allocator, slug: []const u8) Error!Project {
    var stmt = d.prepare(select_columns_sql ++ " where slug = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

/// List all projects, ordered by slug. The optional `filter` narrows the result
/// when root_path_prefix is set (prefix-match via LIKE).
pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: ListFilter) Error![]Project {
    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, select_columns_sql ++ " where 1 = 1");

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    if (filter.root_path_prefix) |pfx| {
        try sql_buf.appendSlice(allocator, " and root_path like ? escape '\\'");
        // Escape LIKE metacharacters in the prefix itself.
        const escaped = try escapeLike(allocator, pfx);
        defer allocator.free(escaped);
        const pattern = try std.fmt.allocPrint(allocator, "{s}%", .{escaped});
        defer allocator.free(pattern);
        try params.append(allocator, .{ .text = pattern });
    }
    try sql_buf.appendSlice(allocator, " order by slug");

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var out: std.ArrayList(Project) = .empty;
    errdefer {
        for (out.items) |p| deinit(p, allocator);
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

/// Find the project whose root_path is the longest prefix of `path`.
/// Returns null when no project matches. Implements the same longest-prefix
/// semantics as the Go `DeriveFromCwd` algorithm.
///
/// A root_path R matches path P when P == R or P starts with R + '/'.
/// Among all matching rows, the one with the longest root_path wins.
/// The ORDER BY length(root_path) DESC guarantees the first hit is the winner.
pub fn findByPath(d: *db.sqlite.Db, allocator: std.mem.Allocator, path: []const u8) Error!?Project {
    // Column order from select_columns_sql:
    //   0=id, 1=slug, 2=name, 3=root_path, 4=git_remote, 5=created_at, 6=updated_at
    //
    // Load rows ordered longest-root_path first and return the first row whose
    // root_path is a proper prefix of (or equals) `path`. The projects table is
    // small enough that a full scan is fine.
    var stmt = d.prepare(
        select_columns_sql ++ " where root_path is not null order by length(root_path) desc",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return Error.QueryFailed;

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                // column 3 = root_path (nullable TEXT)
                const rp_raw = stmt.columnTextOpt(3, allocator) catch return Error.QueryFailed;
                if (rp_raw) |rp| {
                    defer allocator.free(rp);
                    if (pathHasPrefix(path, rp)) {
                        // Re-read the full row from the same current statement position.
                        return try readRow(&stmt, allocator);
                    }
                }
            },
        }
    }
    return null;
}

/// Find the project with exactly matching `git_remote`. Returns null when not found.
pub fn findByGitRemote(d: *db.sqlite.Db, allocator: std.mem.Allocator, remote_url: []const u8) Error!?Project {
    var stmt = d.prepare(
        select_columns_sql ++ " where git_remote = ?",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = remote_url }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => null,
        .row => try readRow(&stmt, allocator),
    };
}

// =========================================================================
// Internals
// =========================================================================

// Column order: 0=id, 1=slug, 2=name, 3=root_path, 4=git_remote, 5=created_at, 6=updated_at
const select_columns_sql: [:0]const u8 =
    "select id, slug, name, root_path, git_remote, created_at, updated_at from projects";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Project {
    return .{
        .id = stmt.columnInt(0),
        .slug = try stmt.columnTextAlloc(1, allocator),
        .name = try stmt.columnTextAlloc(2, allocator),
        .root_path = try stmt.columnTextOpt(3, allocator),
        .git_remote = try stmt.columnTextOpt(4, allocator),
        .created_at = try stmt.columnTextAlloc(5, allocator),
        .updated_at = try stmt.columnTextAlloc(6, allocator),
    };
}

/// pathHasPrefix reports whether `target` equals `root` or starts with `root/`.
/// Both arguments must already be filepath-clean (no trailing slash except "/").
fn pathHasPrefix(target: []const u8, root: []const u8) bool {
    if (std.mem.eql(u8, target, root)) return true;
    if (!std.mem.startsWith(u8, target, root)) return false;
    // Ensure the next character after `root` is '/' so "/foo/bar" does not
    // match root "/foo/b".
    if (target.len <= root.len) return false;
    return target[root.len] == '/';
}

fn escapeLike(allocator: std.mem.Allocator, s: []const u8) ![]const u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    for (s) |ch| {
        if (ch == '%' or ch == '_' or ch == '\\') try out.append(allocator, '\\');
        try out.append(allocator, ch);
    }
    return try out.toOwnedSlice(allocator);
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

test "add: happy path + audit row" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{
        .slug = "myrepo",
        .name = "My Repo",
        .root_path = "/home/user/myrepo",
        .git_remote = "git@github.com:user/myrepo.git",
    });
    defer deinit(p, a);

    try std.testing.expectEqualStrings("myrepo", p.slug);
    try std.testing.expectEqualStrings("My Repo", p.name);
    try std.testing.expectEqualStrings("/home/user/myrepo", p.root_path.?);
    try std.testing.expectEqualStrings("git@github.com:user/myrepo.git", p.git_remote.?);

    // Audit row must exist.
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='create' and entity_kind='project'"),
    );
}

test "add: duplicate slug returns SlugExists" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{ .slug = "dup" });
    defer deinit(p, a);

    try std.testing.expectError(Error.SlugExists, add(&d, a, .{ .slug = "dup" }));
}

test "add: name defaults to slug" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{ .slug = "bare-slug" });
    defer deinit(p, a);

    try std.testing.expectEqualStrings("bare-slug", p.name);
}

test "update: single field" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{ .slug = "repo1", .name = "Old Name" });
    defer deinit(p, a);

    const updated = try update(&d, a, p.id, .{ .name = "New Name" });
    defer deinit(updated, a);

    try std.testing.expectEqualStrings("New Name", updated.name);
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='update' and entity_kind='project'"),
    );
}

test "update: multiple fields" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{ .slug = "repo2" });
    defer deinit(p, a);

    const updated = try update(&d, a, p.id, .{
        .name = "New Name",
        .root_path = "/new/path",
        .git_remote = "git@github.com:org/repo.git",
    });
    defer deinit(updated, a);

    try std.testing.expectEqualStrings("New Name", updated.name);
    try std.testing.expectEqualStrings("/new/path", updated.root_path.?);
    try std.testing.expectEqualStrings("git@github.com:org/repo.git", updated.git_remote.?);
}

test "update: zero fields returns NoFieldsToUpdate" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{ .slug = "repo3" });
    defer deinit(p, a);

    try std.testing.expectError(Error.NoFieldsToUpdate, update(&d, a, p.id, .{}));
}

test "delete: happy path + audit row" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{ .slug = "todelete" });
    const id = p.id;
    deinit(p, a);

    try delete(&d, a, id);

    try std.testing.expectError(Error.NotFound, show(&d, a, id));
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='delete' and entity_kind='project'"),
    );
}

test "delete: missing returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(Error.NotFound, delete(&d, a, 9999));
}

test "show: happy path" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try add(&d, a, .{ .slug = "visible" });
    defer deinit(p1, a);

    const p2 = try show(&d, a, p1.id);
    defer deinit(p2, a);

    try std.testing.expectEqual(p1.id, p2.id);
    try std.testing.expectEqualStrings(p1.slug, p2.slug);
}

test "show: NotFound for missing id" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
}

test "list: empty result returns empty slice" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const items = try list(&d, a, .{});
    defer deinitMany(items, a);

    try std.testing.expectEqual(@as(usize, 0), items.len);
}

test "list: returns all projects ordered by slug" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try add(&d, a, .{ .slug = "beta" });
    defer deinit(p1, a);
    const p2 = try add(&d, a, .{ .slug = "alpha" });
    defer deinit(p2, a);

    const items = try list(&d, a, .{});
    defer deinitMany(items, a);

    try std.testing.expectEqual(@as(usize, 2), items.len);
    try std.testing.expectEqualStrings("alpha", items[0].slug);
    try std.testing.expectEqualStrings("beta", items[1].slug);
}

test "findByPath: longest-prefix match" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const pa = try add(&d, a, .{ .slug = "a", .root_path = "/a" });
    defer deinit(pa, a);
    const pab = try add(&d, a, .{ .slug = "a-b", .root_path = "/a/b" });
    defer deinit(pab, a);
    const pabc = try add(&d, a, .{ .slug = "a-b-c", .root_path = "/a/b/c" });
    defer deinit(pabc, a);

    // /a/b/c/d → should match /a/b/c (longest prefix)
    const found = try findByPath(&d, a, "/a/b/c/d");
    try std.testing.expect(found != null);
    defer deinit(found.?, a);
    try std.testing.expectEqualStrings("a-b-c", found.?.slug);
}

test "findByPath: exact match" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{ .slug = "exact", .root_path = "/exact/path" });
    defer deinit(p, a);

    const found = try findByPath(&d, a, "/exact/path");
    try std.testing.expect(found != null);
    defer deinit(found.?, a);
    try std.testing.expectEqualStrings("exact", found.?.slug);
}

test "findByPath: no match returns null" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{ .slug = "nomatch", .root_path = "/registered" });
    defer deinit(p, a);

    const found = try findByPath(&d, a, "/unrelated/path");
    try std.testing.expect(found == null);
}

test "findByPath: no false prefix match (/foo/bar does not match /foo/barbaz)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{ .slug = "foo-bar", .root_path = "/foo/bar" });
    defer deinit(p, a);

    const found = try findByPath(&d, a, "/foo/barbaz");
    try std.testing.expect(found == null);
}

test "findByGitRemote: exact match" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try add(&d, a, .{ .slug = "gr", .git_remote = "git@github.com:org/gr.git" });
    defer deinit(p, a);

    const found = try findByGitRemote(&d, a, "git@github.com:org/gr.git");
    try std.testing.expect(found != null);
    defer deinit(found.?, a);
    try std.testing.expectEqualStrings("gr", found.?.slug);
}

test "findByGitRemote: no match returns null" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const found = try findByGitRemote(&d, a, "git@github.com:ghost/repo.git");
    try std.testing.expect(found == null);
}
