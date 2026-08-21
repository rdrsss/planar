//! engine/init — bootstrap a Planar database.
//!
//! Mirrors the Go side `cmd/planar/internal/identity/init.go` flow:
//!
//!   1. Open or create the SQLite DB at `db_path`.
//!   2. Apply every pending up-migration in version order.
//!   3. Assert the DB schema is not ahead of the binary's embedded max
//!      (schema-version-too-new guard).
//!   4. Optionally register `cwd` as a project (`INSERT OR IGNORE` —
//!      idempotent: a second `planar init` from the same cwd is a no-op).
//!
//! Step 1–3 already live in `cmd/planar/runtime.ensureDb`; the CLI
//! handler reuses that path. This module owns Step 4 (the project
//! registration helper) plus the slug-derivation utility so engine-side
//! callers (tests, future internal tools) can run a full bootstrap
//! without going through the runtime.
//!
//! Idempotency contract:
//!   - Running `init` twice on the same DB MUST NOT error.
//!   - Running `init` twice from the same cwd MUST NOT create a second
//!     projects row. The slug-uniqueness constraint plus INSERT OR IGNORE
//!     enforces this at the SQL layer.
//!
//! What this module deliberately does NOT do:
//!   - It does not open the DB or run migrations — those run in
//!     `runtime.ensureDb` so help-only paths avoid the cost.
//!   - It does not invoke `git` to read the remote — the handler does
//!     that (engine layer stays IO-free apart from the SQLite handle).

const std = @import("std");
const db = @import("db");
const project = @import("identity/project.zig");

/// Inputs for `registerCwd`. All strings are borrowed for the duration
/// of the call.
pub const RegisterCwdArgs = struct {
    /// Absolute path of the working directory to register.
    cwd: []const u8,
    /// Optional human-readable name. Defaults to the cwd basename.
    name: ?[]const u8 = null,
    /// Optional explicit slug. Defaults to `deriveSlug(basename(cwd))`.
    slug: ?[]const u8 = null,
    /// Optional git remote URL captured by the caller.
    git_remote: ?[]const u8 = null,
    /// Overwrite an existing registration: when the slug row already
    /// exists, repoint its root_path (and refresh name/git_remote) to
    /// this call's values under the SAME project id, so association
    /// memberships and scoped entities carry over. This is the
    /// checkout-moved migration path (e.g. a standalone checkout
    /// retired in favor of a superproject submodule). Without force,
    /// insert-or-ignore semantics hold and an existing row is returned
    /// unchanged.
    force: bool = false,
};

pub const Error =
    error{
        InvalidPath,
        QueryFailed,
    } ||
    std.mem.Allocator.Error;

/// Register `cwd` as a project. Idempotent: if a row with the derived
/// (or supplied) slug already exists, the existing row is returned
/// unchanged. The caller owns the returned project's allocator-owned
/// strings — use `project.deinit` to release them.
///
/// Mirrors Go's `project.Register` (`INSERT OR IGNORE` semantics):
/// `name`, `cwd`, and `git_remote` are only written on first insert.
pub fn registerCwd(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: RegisterCwdArgs,
) Error!project.Project {
    if (args.cwd.len == 0 or args.cwd[0] != '/') return Error.InvalidPath;

    const base = std.fs.path.basename(args.cwd);
    const slug_owned: []const u8 = if (args.slug) |s|
        try allocator.dupe(u8, s)
    else
        try deriveSlug(allocator, base);
    defer allocator.free(slug_owned);

    const name: []const u8 = args.name orelse base;

    // INSERT OR IGNORE — second call from the same cwd is a no-op.
    // With `force`, an existing slug row is repointed in place instead
    // (same id — memberships and scoped entities carry over).
    const sql = if (args.force)
        \\insert into projects (slug, name, root_path, git_remote)
        \\values (?, ?, ?, ?)
        \\on conflict (slug) do update set
        \\  name = excluded.name,
        \\  root_path = excluded.root_path,
        \\  git_remote = excluded.git_remote,
        \\  updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
    else
        \\insert or ignore into projects (slug, name, root_path, git_remote)
        \\values (?, ?, ?, ?)
    ;
    _ = d.execParams(sql, &.{
        .{ .text = slug_owned },
        .{ .text = name },
        .{ .text = args.cwd },
        if (args.git_remote) |g| .{ .text = g } else .{ .null = {} },
    }) catch |e| {
        std.log.err("engine.init.registerCwd insert failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    // Read back the row covering both insert and already-exists paths.
    var stmt = d.prepare(
        \\select id, slug, name, root_path, git_remote, created_at, updated_at
        \\from projects where slug = ?
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug_owned }}) catch return Error.QueryFailed;

    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.QueryFailed, // we just inserted (or it already existed) — should be unreachable
        .row => .{
            .id = stmt.columnInt(0),
            .slug = try stmt.columnTextAlloc(1, allocator),
            .name = try stmt.columnTextAlloc(2, allocator),
            .root_path = try stmt.columnTextOpt(3, allocator),
            .git_remote = try stmt.columnTextOpt(4, allocator),
            .created_at = try stmt.columnTextAlloc(5, allocator),
            .updated_at = try stmt.columnTextAlloc(6, allocator),
        },
    };
}

/// Derive a project slug from a name (typically the cwd basename).
///
/// Rules — mirror Go's `project.DeriveSlug`:
///   1. Lowercase the input.
///   2. Replace runs of non-alphanumeric characters with a single '-'.
///   3. Trim leading and trailing '-'.
///   4. Fall back to "project" when the result is empty.
///
/// Caller owns the returned slice.
pub fn deriveSlug(allocator: std.mem.Allocator, name: []const u8) std.mem.Allocator.Error![]const u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);

    var prev_dash = true; // trims leading '-'
    for (name) |raw| {
        const ch = std.ascii.toLower(raw);
        if (std.ascii.isAlphanumeric(ch)) {
            try out.append(allocator, ch);
            prev_dash = false;
        } else if (!prev_dash) {
            try out.append(allocator, '-');
            prev_dash = true;
        }
    }
    if (out.items.len > 0 and out.items[out.items.len - 1] == '-') _ = out.pop();
    if (out.items.len == 0) {
        try out.appendSlice(allocator, "project");
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

test "deriveSlug: lowercases + collapses non-alphanumeric runs" {
    const a = std.testing.allocator;
    const s = try deriveSlug(a, "My.Project!");
    defer a.free(s);
    try std.testing.expectEqualStrings("my-project", s);
}

test "deriveSlug: trims leading/trailing dashes" {
    const a = std.testing.allocator;
    const s = try deriveSlug(a, "  hello  ");
    defer a.free(s);
    try std.testing.expectEqualStrings("hello", s);
}

test "deriveSlug: empty input falls back to 'project'" {
    const a = std.testing.allocator;
    const s = try deriveSlug(a, "");
    defer a.free(s);
    try std.testing.expectEqualStrings("project", s);
}

test "deriveSlug: only-punctuation input falls back to 'project'" {
    const a = std.testing.allocator;
    const s = try deriveSlug(a, "!!!");
    defer a.free(s);
    try std.testing.expectEqualStrings("project", s);
}

test "registerCwd: happy path inserts a project row" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try registerCwd(&d, a, .{
        .cwd = "/home/user/myrepo",
        .git_remote = "git@github.com:user/myrepo.git",
    });
    defer project.deinit(p, a);

    try std.testing.expectEqualStrings("myrepo", p.slug);
    try std.testing.expectEqualStrings("myrepo", p.name);
    try std.testing.expectEqualStrings("/home/user/myrepo", p.root_path.?);
    try std.testing.expectEqualStrings("git@github.com:user/myrepo.git", p.git_remote.?);
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from projects"),
    );
}

test "registerCwd: second call from the same cwd is idempotent" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try registerCwd(&d, a, .{ .cwd = "/work/idemp" });
    defer project.deinit(p1, a);
    const p2 = try registerCwd(&d, a, .{ .cwd = "/work/idemp" });
    defer project.deinit(p2, a);

    try std.testing.expectEqual(p1.id, p2.id);
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from projects"),
    );
}

test "registerCwd: force repoints root_path under the same id" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Slug derives from the cwd BASENAME — the checkout-moved case is
    // the same leaf directory name under a new parent (a sibling
    // checkout retired in favor of a superproject submodule).
    const p1 = try registerCwd(&d, a, .{ .cwd = "/work/old/mover" });
    defer project.deinit(p1, a);

    // Without force: existing row wins, root_path untouched.
    const p2 = try registerCwd(&d, a, .{ .cwd = "/work/new/mover" });
    defer project.deinit(p2, a);
    try std.testing.expectEqual(p1.id, p2.id);
    try std.testing.expectEqualStrings("/work/old/mover", p2.root_path.?);

    // With force: same id, repointed root_path, refreshed remote.
    const p3 = try registerCwd(&d, a, .{
        .cwd = "/work/new/mover",
        .git_remote = "git@example.com:mover.git",
        .force = true,
    });
    defer project.deinit(p3, a);
    try std.testing.expectEqual(p1.id, p3.id);
    try std.testing.expectEqualStrings("/work/new/mover", p3.root_path.?);
    try std.testing.expectEqualStrings("git@example.com:mover.git", p3.git_remote.?);
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from projects"),
    );
}

test "registerCwd: rejects a non-absolute path" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(
        Error.InvalidPath,
        registerCwd(&d, a, .{ .cwd = "relative/path" }),
    );
}

test "registerCwd: explicit name + slug overrides win" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try registerCwd(&d, a, .{
        .cwd = "/work/something",
        .name = "Friendly Name",
        .slug = "custom-slug",
    });
    defer project.deinit(p, a);

    try std.testing.expectEqualStrings("custom-slug", p.slug);
    try std.testing.expectEqualStrings("Friendly Name", p.name);
}

test "registerCwd: git_remote nullable" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try registerCwd(&d, a, .{ .cwd = "/work/noremote" });
    defer project.deinit(p, a);

    try std.testing.expect(p.git_remote == null);
}
