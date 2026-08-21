//! engine/identity/workspace — workspace identity + state-dir layout.

const std = @import("std");
const db = @import("db");

pub const Workspace = struct {
    id: i64,
    slug: []const u8,
    name: []const u8,
    root_path: ?[]const u8,
};

pub fn deinitWorkspace(workspace: Workspace, allocator: std.mem.Allocator) void {
    allocator.free(workspace.slug);
    allocator.free(workspace.name);
    if (workspace.root_path) |root_path| allocator.free(root_path);
}

pub fn deinitMany(workspaces: []const Workspace, allocator: std.mem.Allocator) void {
    for (workspaces) |workspace| deinitWorkspace(workspace, allocator);
    allocator.free(workspaces);
}

pub const Layout = struct {
    org_id: i64,
    dir: []const u8,
    agents_md: []const u8,
    routing_table: []const u8,
    config_toml: []const u8,
    readme_md: []const u8,
};

pub fn deinitLayout(layout: Layout, allocator: std.mem.Allocator) void {
    allocator.free(layout.dir);
    allocator.free(layout.agents_md);
    allocator.free(layout.routing_table);
    allocator.free(layout.config_toml);
    allocator.free(layout.readme_md);
}

pub const StrategySymlink = "symlink";
pub const StrategyCopy = "copy";

const link_names = [_][]const u8{ "AGENTS.md", "CLAUDE.md" };

pub fn resolveOrg(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    target: ?[]const u8,
) !Workspace {
    const raw = if (target) |value| std.mem.trim(u8, value, " \t\r\n") else "";
    if (raw.len == 0) {
        var stmt = d.prepare(
            \\select id, slug, name, coalesce(config_json, '')
            \\from associations
            \\where kind = 'org'
            \\order by id
            \\limit 2
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{}) catch return error.QueryFailed;

        var first: ?Workspace = null;
        var count: usize = 0;
        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    count += 1;
                    if (count == 1) {
                        const config_json = try stmt.columnTextAlloc(3, allocator);
                        defer allocator.free(config_json);
                        first = .{
                            .id = stmt.columnInt(0),
                            .slug = try stmt.columnTextAlloc(1, allocator),
                            .name = try stmt.columnTextAlloc(2, allocator),
                            .root_path = try parseRootPathFromConfig(allocator, config_json),
                        };
                    }
                },
            }
        }
        if (count == 0) return error.NotFound;
        if (count > 1) {
            if (first) |workspace| deinitWorkspace(workspace, allocator);
            return error.InvalidInput;
        }
        return first.?;
    }

    const value = if (std.mem.startsWith(u8, raw, "org:")) raw["org:".len..] else raw;
    if (std.fmt.parseInt(i64, value, 10)) |org_id| {
        return try resolveOrgByID(d, allocator, org_id);
    } else |_| {
        return try resolveOrgBySlug(d, allocator, value);
    }
}

pub fn listOrgs(d: *db.sqlite.Db, allocator: std.mem.Allocator) ![]Workspace {
    var stmt = d.prepare(
        \\select id, slug, name, coalesce(config_json, '')
        \\from associations
        \\where kind = 'org'
        \\order by id
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return error.QueryFailed;

    var out: std.ArrayList(Workspace) = .empty;
    errdefer {
        for (out.items) |workspace| deinitWorkspace(workspace, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const config_json = try stmt.columnTextAlloc(3, allocator);
                defer allocator.free(config_json);
                try out.append(allocator, .{
                    .id = stmt.columnInt(0),
                    .slug = try stmt.columnTextAlloc(1, allocator),
                    .name = try stmt.columnTextAlloc(2, allocator),
                    .root_path = try parseRootPathFromConfig(allocator, config_json),
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn planarHome(allocator: std.mem.Allocator, environ: std.process.Environ) ![]u8 {
    if (environ.getPosix("PLANAR_HOME")) |raw| {
        if (raw.len > 0) return try expandTilde(allocator, raw, environ);
    }
    const home = environ.getPosix("HOME") orelse return error.HomeNotSet;
    return try std.fs.path.join(allocator, &.{ home, ".planar" });
}

pub fn workspaceDir(
    allocator: std.mem.Allocator,
    environ: std.process.Environ,
    org_id: i64,
) ![]u8 {
    if (org_id <= 0) return error.InvalidInput;
    const home = try planarHome(allocator, environ);
    defer allocator.free(home);
    const id_text = try std.fmt.allocPrint(allocator, "{d}", .{org_id});
    defer allocator.free(id_text);
    return try std.fs.path.join(allocator, &.{ home, "workspaces", id_text });
}

pub fn loadLayout(
    allocator: std.mem.Allocator,
    environ: std.process.Environ,
    org_id: i64,
) !Layout {
    const dir = try workspaceDir(allocator, environ, org_id);
    errdefer allocator.free(dir);

    const agents_md = try std.fs.path.join(allocator, &.{ dir, "AGENTS.md" });
    errdefer allocator.free(agents_md);
    const routing_table = try std.fs.path.join(allocator, &.{ dir, "routing-table.json" });
    errdefer allocator.free(routing_table);
    const config_toml = try std.fs.path.join(allocator, &.{ dir, "config.toml" });
    errdefer allocator.free(config_toml);
    const readme_md = try std.fs.path.join(allocator, &.{ dir, "README.md" });
    errdefer allocator.free(readme_md);

    return .{
        .org_id = org_id,
        .dir = dir,
        .agents_md = agents_md,
        .routing_table = routing_table,
        .config_toml = config_toml,
        .readme_md = readme_md,
    };
}

pub fn ensureLayout(
    allocator: std.mem.Allocator,
    io: std.Io,
    environ: std.process.Environ,
    org_id: i64,
) !Layout {
    const layout = try loadLayout(allocator, environ, org_id);
    errdefer deinitLayout(layout, allocator);
    try std.Io.Dir.cwd().createDirPath(io, layout.dir);
    return layout;
}

pub fn installSymlinks(
    allocator: std.mem.Allocator,
    io: std.Io,
    workspace_root: []const u8,
    layout: Layout,
) ![]const u8 {
    if (workspace_root.len == 0) return error.InvalidInput;
    try std.Io.Dir.cwd().createDirPath(io, workspace_root);
    var used_copy = false;
    for (link_names) |name| {
        const link_path = try std.fs.path.join(allocator, &.{ workspace_root, name });
        defer allocator.free(link_path);
        const copied = try installOne(allocator, io, link_path, layout.agents_md);
        used_copy = used_copy or copied;
    }
    return if (used_copy) StrategyCopy else StrategySymlink;
}

pub fn removeSymlinks(allocator: std.mem.Allocator, io: std.Io, workspace_root: []const u8) !void {
    if (workspace_root.len == 0) return error.InvalidInput;
    for (link_names) |name| {
        const link_path = try std.fs.path.join(allocator, &.{ workspace_root, name });
        defer allocator.free(link_path);
        std.Io.Dir.cwd().deleteFile(io, link_path) catch |err| switch (err) {
            error.FileNotFound => {},
            else => return err,
        };
    }
}

pub const PlanarFocus = struct {
    active_plans: []const i64,
    open_tasks: i64,
    open_questions: i64,
    recent_session_ids: []const i64,
};

pub const ProjectRoute = struct {
    slug: []const u8,
    root_path: []const u8,
    git_remote: []const u8,
    summary: []const u8,
    summary_source: []const u8,
    capabilities: []const []const u8,
    capabilities_source: []const u8,
    depends_on: []const []const u8,
    depends_on_source: []const u8,
    entry_points: []const []const u8,
    languages: std.json.ArrayHashMap(f64),
    planar_focus: PlanarFocus,
};

pub const DependencyEdge = struct {
    from: []const u8,
    to: []const u8,
    reason: []const u8,
};

pub const CrossRepo = struct {
    plans_scoped_to_org: []const i64,
    questions_scoped_to_org: []const i64,
    dependency_edges: []const DependencyEdge,
};

pub const RoutingTable = struct {
    schema_version: i64,
    workspace_id: i64,
    workspace_slug: []const u8,
    workspace_name: []const u8,
    generated_at: []const u8,
    generator_version: []const u8,
    projects: []ProjectRoute,
    cross_repo: CrossRepo,
};

fn resolveOrgByID(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    org_id: i64,
) !Workspace {
    var stmt = d.prepare(
        \\select id, slug, name, coalesce(config_json, '')
        \\from associations
        \\where kind = 'org' and id = ?
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = org_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const config_json = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(config_json);
            return .{
                .id = stmt.columnInt(0),
                .slug = try stmt.columnTextAlloc(1, allocator),
                .name = try stmt.columnTextAlloc(2, allocator),
                .root_path = try parseRootPathFromConfig(allocator, config_json),
            };
        },
    }
}

fn resolveOrgBySlug(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    slug: []const u8,
) !Workspace {
    var stmt = d.prepare(
        \\select id, slug, name, coalesce(config_json, '')
        \\from associations
        \\where kind = 'org' and slug = ?
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => {
            const config_json = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(config_json);
            return .{
                .id = stmt.columnInt(0),
                .slug = try stmt.columnTextAlloc(1, allocator),
                .name = try stmt.columnTextAlloc(2, allocator),
                .root_path = try parseRootPathFromConfig(allocator, config_json),
            };
        },
    }
}

fn parseRootPathFromConfig(allocator: std.mem.Allocator, config_json: []const u8) !?[]const u8 {
    if (config_json.len == 0) return null;
    var parsed = std.json.parseFromSlice(std.json.Value, allocator, config_json, .{}) catch return null;
    defer parsed.deinit();
    if (parsed.value != .object) return null;
    if (parsed.value.object.get("root_path")) |value| {
        if (value == .string and value.string.len > 0) return try allocator.dupe(u8, value.string);
    }
    return null;
}

fn expandTilde(
    allocator: std.mem.Allocator,
    path: []const u8,
    environ: std.process.Environ,
) ![]u8 {
    if (std.mem.eql(u8, path, "~")) {
        const home = environ.getPosix("HOME") orelse return error.HomeNotSet;
        return try allocator.dupe(u8, home);
    }
    if (std.mem.startsWith(u8, path, "~/")) {
        const home = environ.getPosix("HOME") orelse return error.HomeNotSet;
        return try std.fs.path.join(allocator, &.{ home, path[2..] });
    }
    return try allocator.dupe(u8, path);
}

fn installOne(
    allocator: std.mem.Allocator,
    io: std.Io,
    link_path: []const u8,
    target: []const u8,
) !bool {
    var existing_buf: [std.fs.max_path_bytes]u8 = undefined;
    if (std.Io.Dir.cwd().readLink(io, link_path, &existing_buf)) |n| {
        if (std.mem.eql(u8, existing_buf[0..n], target)) return false;
    } else |_| {}

    const tmp = try std.fmt.allocPrint(
        allocator,
        "{s}/.{s}.symlink.tmp",
        .{
            std.fs.path.dirname(link_path) orelse ".",
            std.fs.path.basename(link_path),
        },
    );
    defer allocator.free(tmp);
    std.Io.Dir.cwd().deleteFile(io, tmp) catch {};

    std.Io.Dir.cwd().symLink(io, target, tmp, .{}) catch |err| {
        if (!isUnsupportedSymlinkErr(err)) return err;
        const body = try std.Io.Dir.cwd().readFileAlloc(io, target, allocator, std.Io.Limit.limited(16 * 1024 * 1024));
        defer allocator.free(body);
        try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = tmp, .data = body });
        std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), link_path, io) catch |rename_err| {
            std.Io.Dir.cwd().deleteFile(io, tmp) catch {};
            return rename_err;
        };
        return true;
    };
    std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), link_path, io) catch |rename_err| {
        std.Io.Dir.cwd().deleteFile(io, tmp) catch {};
        return rename_err;
    };
    return false;
}

fn isUnsupportedSymlinkErr(err: anyerror) bool {
    return err == error.PermissionDenied or
        err == error.AccessDenied or
        err == error.ReadOnlyFileSystem or
        err == error.FileSystem;
}
