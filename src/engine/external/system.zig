//! engine/external/system — external operational-plane systems.
//!
//! Mirrors Go `internal/external/external` system semantics:
//! - enum sets match the CHECK constraints exactly
//! - slug uniqueness is surfaced as SlugExists
//! - register-jira and register-github helpers carry CLI defaults

const std = @import("std");
const db = @import("db");

pub const SystemKind = enum {
    jira,
    @"github-issues",
    @"gitlab-issues",
    linear,

    pub fn fromText(s: []const u8) ?SystemKind {
        if (std.mem.eql(u8, s, "jira")) return .jira;
        if (std.mem.eql(u8, s, "github-issues")) return .@"github-issues";
        if (std.mem.eql(u8, s, "gitlab-issues")) return .@"gitlab-issues";
        if (std.mem.eql(u8, s, "linear")) return .linear;
        return null;
    }

    pub fn toText(self: SystemKind) []const u8 {
        return switch (self) {
            .jira => "jira",
            .@"github-issues" => "github-issues",
            .@"gitlab-issues" => "gitlab-issues",
            .linear => "linear",
        };
    }
};

pub const AuthMethod = enum {
    @"token-env",
    @"gh-cli",
    @"oauth-stored",

    pub fn fromText(s: []const u8) ?AuthMethod {
        if (std.mem.eql(u8, s, "token-env")) return .@"token-env";
        if (std.mem.eql(u8, s, "gh-cli")) return .@"gh-cli";
        if (std.mem.eql(u8, s, "oauth-stored")) return .@"oauth-stored";
        return null;
    }

    pub fn toText(self: AuthMethod) []const u8 {
        return switch (self) {
            .@"token-env" => "token-env",
            .@"gh-cli" => "gh-cli",
            .@"oauth-stored" => "oauth-stored",
        };
    }
};

pub const ExternalSystem = struct {
    id: i64,
    kind: SystemKind,
    slug: []const u8,
    base_url: ?[]const u8,
    default_project: ?[]const u8,
    auth_method: AuthMethod,
    auth_ref: []const u8,
    created_at: []const u8,
    updated_at: []const u8,
};

pub fn deinit(item: ExternalSystem, allocator: std.mem.Allocator) void {
    allocator.free(item.slug);
    if (item.base_url) |s| allocator.free(s);
    if (item.default_project) |s| allocator.free(s);
    allocator.free(item.auth_ref);
    allocator.free(item.created_at);
    allocator.free(item.updated_at);
}

pub fn deinitMany(items: []const ExternalSystem, allocator: std.mem.Allocator) void {
    for (items) |item| deinit(item, allocator);
    allocator.free(items);
}

pub const RegisterArgs = struct {
    kind: SystemKind,
    slug: []const u8,
    base_url: ?[]const u8 = null,
    default_project: ?[]const u8 = null,
    auth_method: AuthMethod,
    auth_ref: []const u8,
};

pub const RegisterJiraArgs = struct {
    slug: []const u8,
    base_url: []const u8,
    project: []const u8,
    auth_env: []const u8,
};

pub const RegisterGithubArgs = struct {
    slug: []const u8,
    project: []const u8,
    auth_env: ?[]const u8 = null,
};

pub const Error = error{
    NotFound,
    SlugExists,
    QueryFailed,
} || std.mem.Allocator.Error;

pub fn register(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: RegisterArgs) Error!ExternalSystem {
    const insert_sql: [:0]const u8 =
        \\insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref)
        \\values (?, ?, ?, ?, ?, ?)
    ;
    const id = d.execParams(insert_sql, &.{
        .{ .text = args.kind.toText() },
        .{ .text = args.slug },
        if (args.base_url) |s| .{ .text = s } else .{ .null = {} },
        if (args.default_project) |s| .{ .text = s } else .{ .null = {} },
        .{ .text = args.auth_method.toText() },
        .{ .text = args.auth_ref },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.SlugExists;
        std.log.err("external.system.register exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };
    return try showById(d, allocator, id);
}

pub fn registerJira(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: RegisterJiraArgs) Error!ExternalSystem {
    return try register(d, allocator, .{
        .kind = .jira,
        .slug = args.slug,
        .base_url = args.base_url,
        .default_project = args.project,
        .auth_method = .@"token-env",
        .auth_ref = args.auth_env,
    });
}

pub fn registerGithub(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: RegisterGithubArgs) Error!ExternalSystem {
    if (args.auth_env) |auth_env| {
        return try register(d, allocator, .{
            .kind = .@"github-issues",
            .slug = args.slug,
            .base_url = "https://api.github.com",
            .default_project = args.project,
            .auth_method = .@"token-env",
            .auth_ref = auth_env,
        });
    }
    return try register(d, allocator, .{
        .kind = .@"github-issues",
        .slug = args.slug,
        .base_url = "https://api.github.com",
        .default_project = args.project,
        .auth_method = .@"gh-cli",
        .auth_ref = "default",
    });
}

pub fn showBySlug(d: *db.sqlite.Db, allocator: std.mem.Allocator, slug: []const u8) Error!ExternalSystem {
    var stmt = d.prepare(select_by_slug_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

pub fn showById(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!ExternalSystem {
    var stmt = d.prepare(select_by_id_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator) Error![]ExternalSystem {
    var stmt = d.prepare(select_all_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return Error.QueryFailed;

    var out: std.ArrayList(ExternalSystem) = .empty;
    errdefer {
        for (out.items) |item| deinit(item, allocator);
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

const select_columns =
    "id, kind, slug, base_url, default_project, auth_method, auth_ref, created_at, updated_at";
const select_by_slug_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from external_systems where slug = ?";
const select_by_id_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from external_systems where id = ?";
const select_all_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from external_systems order by id";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!ExternalSystem {
    const kind_text = try stmt.columnTextAlloc(1, allocator);
    defer allocator.free(kind_text);
    const kind = SystemKind.fromText(kind_text) orelse return Error.QueryFailed;

    const auth_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(auth_text);
    const auth_method = AuthMethod.fromText(auth_text) orelse return Error.QueryFailed;

    return .{
        .id = stmt.columnInt(0),
        .kind = kind,
        .slug = try stmt.columnTextAlloc(2, allocator),
        .base_url = try stmt.columnTextOpt(3, allocator),
        .default_project = try stmt.columnTextOpt(4, allocator),
        .auth_method = auth_method,
        .auth_ref = try stmt.columnTextAlloc(6, allocator),
        .created_at = try stmt.columnTextAlloc(7, allocator),
        .updated_at = try stmt.columnTextAlloc(8, allocator),
    };
}

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var dn = try db.sqlite.Db.openMemory();
    errdefer dn.close();
    try db.migrate.applyAll(&dn, allocator);
    return dn;
}

test "register jira helper persists canonical defaults" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sys = try registerJira(&d, a, .{
        .slug = "jira-main",
        .base_url = "https://acme.atlassian.net",
        .project = "ACME",
        .auth_env = "JIRA_TOKEN",
    });
    defer deinit(sys, a);

    try std.testing.expectEqual(SystemKind.jira, sys.kind);
    try std.testing.expect(sys.base_url != null);
    try std.testing.expectEqualStrings("https://acme.atlassian.net", sys.base_url.?);
    try std.testing.expect(sys.default_project != null);
    try std.testing.expectEqualStrings("ACME", sys.default_project.?);
    try std.testing.expectEqual(AuthMethod.@"token-env", sys.auth_method);
    try std.testing.expectEqualStrings("JIRA_TOKEN", sys.auth_ref);
}

test "register duplicate slug returns SlugExists" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sys = try register(&d, a, .{
        .kind = .jira,
        .slug = "dup",
        .auth_method = .@"token-env",
        .auth_ref = "TOKEN",
    });
    defer deinit(sys, a);

    try std.testing.expectError(
        Error.SlugExists,
        register(&d, a, .{
            .kind = .jira,
            .slug = "dup",
            .auth_method = .@"token-env",
            .auth_ref = "TOKEN",
        }),
    );
}

test "register github helper uses gh-cli defaults without auth_env" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sys = try registerGithub(&d, a, .{
        .slug = "gh-default",
        .project = "acme/repo",
    });
    defer deinit(sys, a);

    try std.testing.expectEqual(SystemKind.@"github-issues", sys.kind);
    try std.testing.expect(sys.base_url != null);
    try std.testing.expectEqualStrings("https://api.github.com", sys.base_url.?);
    try std.testing.expectEqual(AuthMethod.@"gh-cli", sys.auth_method);
    try std.testing.expectEqualStrings("default", sys.auth_ref);
}

test "register github helper switches to token-env when auth_env is provided" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sys = try registerGithub(&d, a, .{
        .slug = "gh-env",
        .project = "acme/repo",
        .auth_env = "GH_TOKEN",
    });
    defer deinit(sys, a);

    try std.testing.expectEqual(AuthMethod.@"token-env", sys.auth_method);
    try std.testing.expectEqualStrings("GH_TOKEN", sys.auth_ref);
}

test "showBySlug and list round-trip" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const a1 = try register(&d, a, .{
        .kind = .jira,
        .slug = "s-a",
        .auth_method = .@"token-env",
        .auth_ref = "A",
    });
    defer deinit(a1, a);
    const a2 = try register(&d, a, .{
        .kind = .@"github-issues",
        .slug = "s-b",
        .auth_method = .@"gh-cli",
        .auth_ref = "default",
    });
    defer deinit(a2, a);

    const got = try showBySlug(&d, a, "s-b");
    defer deinit(got, a);
    try std.testing.expectEqualStrings("s-b", got.slug);
    try std.testing.expectEqual(SystemKind.@"github-issues", got.kind);

    const systems = try list(&d, a);
    defer deinitMany(systems, a);
    try std.testing.expectEqual(@as(usize, 2), systems.len);
    try std.testing.expectEqualStrings("s-a", systems[0].slug);
    try std.testing.expectEqualStrings("s-b", systems[1].slug);
}
