const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workspace", "init" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    if (args.no_scan and args.enrich) {
        exit.die(ctx, error.InvalidInput, "cannot combine --no-scan and --enrich", .{});
    }

    const cwd = std.Io.Dir.realPathFileAlloc(.cwd(), ctx.io, ".", ctx.allocator) catch |e|
        exit.die(ctx, e, "getting working directory failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(cwd);
    const own_git = try std.fs.path.join(ctx.allocator, &.{ cwd, ".git" });
    defer ctx.allocator.free(own_git);
    if (pathExists(own_git)) {
        exit.die(ctx, error.InvalidInput, "current directory {s} is a git repository; use `planar init` for single repos", .{cwd});
    }

    const scan_depth = if (args.scan > 0) @as(usize, @intCast(args.scan)) else @as(usize, 1);
    const children = scanGitChildren(ctx.allocator, cwd, scan_depth) catch |e|
        exit.die(ctx, e, "scanning child repositories failed: {s}", .{@errorName(e)});
    defer freeStrings(children, ctx.allocator);
    if (children.len == 0) {
        exit.die(ctx, error.InvalidInput, "no child directories with .git found under {s}; nothing to initialize", .{cwd});
    }

    const base = std.fs.path.basename(cwd);
    const org_slug = if (args.slug) |slug|
        try ctx.allocator.dupe(u8, slug)
    else
        try engine.init.deriveSlug(ctx.allocator, base);
    defer ctx.allocator.free(org_slug);
    const org_name = if (args.name) |name| name else base;

    var projects = std.ArrayList(ProjectResult).empty;
    defer {
        for (projects.items) |project| {
            ctx.allocator.free(project.slug);
            ctx.allocator.free(project.path);
        }
        projects.deinit(ctx.allocator);
    }

    const org_state = registerWorkspace(d, ctx.allocator, cwd, org_slug, org_name, children, &projects) catch |e|
        exit.die(ctx, e, "registering workspace failed: {s}", .{@errorName(e)});

    const layout = engine.identity.workspace.ensureLayout(ctx.allocator, ctx.io, ctx.environ, org_state.id) catch null;
    defer if (layout) |l| engine.identity.workspace.deinitLayout(l, ctx.allocator);

    var pipeline = PipelineResult{};
    if (args.no_scan) {
        pipeline.skipped = true;
    } else if (layout) |l| {
        pipeline = runPipeline(ctx, d, org_state.id, cwd, l, args.enrich) catch |e| blk: {
            const msg = std.fmt.allocPrint(ctx.allocator, "{s}", .{@errorName(e)}) catch "";
            pipeline.@"error" = msg;
            break :blk pipeline;
        };
        if (pipeline.@"error".len > 0 and !args.json) {
            try ctx.stderr.print("warning: pipeline pass failed: {s}\n", .{pipeline.@"error"});
            try ctx.stderr.print("hint: re-run `planar workspace routing build` && `planar workspace regenerate`\n", .{});
        }
    } else {
        pipeline.@"error" = "layout-create-failed";
    }
    defer if (pipeline.@"error".len > 0) ctx.allocator.free(pipeline.@"error");

    if (args.json) {
        try std.json.Stringify.value(.{
            .org = .{
                .id = org_state.id,
                .slug = org_slug,
                .name = org_name,
                .created = org_state.created,
            },
            .projects = projects.items,
            .pipeline = pipeline,
        }, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }

    const org_verb = if (org_state.created) "created" else "reused";
    try ctx.stdout.print("{s} org:{s} ({s})\n", .{ org_verb, org_slug, org_name });
    for (projects.items, 0..) |project, i| {
        const branch = if (i + 1 == projects.items.len) "└─" else "├─";
        const status = if (!project.created and !project.membership_created)
            "already present"
        else if (!project.created and project.membership_created)
            "linked existing project"
        else
            "auto-created";
        try ctx.stdout.print("  {s} project:{s}   [{s}]   ({s}, member-of org:{s})\n", .{
            branch,
            project.slug,
            project.path,
            status,
            org_slug,
        });
    }
    try ctx.stdout.print("\n{d} repos initialized as projects, all members of org:{s}.\n", .{ projects.items.len, org_slug });
    if (pipeline.skipped) {
        try ctx.stdout.print("Run `planar workspace routing build` and `planar workspace regenerate` to populate the state directory.\n", .{});
    } else if (pipeline.@"error".len == 0) {
        try ctx.stdout.print("Routing table refreshed ({d} projects, {d} cross-repo deps).\n", .{
            pipeline.routing.project_count,
            pipeline.routing.cross_repo_deps,
        });
        try ctx.stdout.print("AGENTS.md regenerated ({d} bytes).\n", .{pipeline.regenerate.bytes});
        try ctx.stdout.print("Symlinks installed: AGENTS.md, CLAUDE.md (strategy: {s}).\n", .{pipeline.symlinks.strategy});
    }
    try ctx.stdout.print("Run `planar assoc tree` to view the hierarchy.\n", .{});
}

const OrgResult = struct {
    id: i64,
    created: bool,
};

const ProjectResult = struct {
    slug: []const u8,
    path: []const u8,
    created: bool,
    membership_created: bool,
};

const PipelineResult = struct {
    skipped: bool = false,
    routing: struct {
        project_count: i64 = 0,
        cross_repo_deps: i64 = 0,
        enrich_enabled: bool = false,
        enrich_misses: i64 = 0,
    } = .{},
    regenerate: struct {
        agents_path: []const u8 = "",
        bytes: i64 = 0,
    } = .{},
    symlinks: struct {
        strategy: []const u8 = "",
        installed: []const []const u8 = &.{},
    } = .{},
    @"error": []const u8 = "",
};

fn runPipeline(
    ctx: *const runtime.Ctx,
    d: *db.sqlite.Db,
    org_id: i64,
    workspace_root: []const u8,
    layout: engine.identity.workspace.Layout,
    enrich: bool,
) !PipelineResult {
    var out = PipelineResult{};
    const rules_path = try capabilityRulesPath(ctx.allocator, ctx.environ);
    defer ctx.allocator.free(rules_path);
    var rules = try engine.workspace.routing.loadCapabilityRules(ctx.allocator, rules_path);
    if (rules.rules.len == 0) {
        engine.workspace.routing.deinitCapabilityRules(rules, ctx.allocator);
        rules = try engine.workspace.routing.defaultCapabilityRules(ctx.allocator);
    }
    defer engine.workspace.routing.deinitCapabilityRules(rules, ctx.allocator);

    const overrides_path = try std.fs.path.join(ctx.allocator, &.{ layout.dir, "routing-table-overrides.json" });
    defer ctx.allocator.free(overrides_path);
    const overrides = try engine.workspace.routing.loadOverrides(ctx.allocator, overrides_path);
    defer engine.workspace.routing.deinitOverrides(overrides, ctx.allocator);

    var table = try engine.workspace.routing.buildWithRules(d, ctx.allocator, org_id, rules);
    defer engine.workspace.routing.deinit(table, ctx.allocator);
    try engine.workspace.routing.applyOverrides(&table, overrides, ctx.allocator);
    try engine.workspace.routing.write(layout.routing_table, table, ctx.allocator);
    out.routing = .{
        .project_count = @intCast(table.projects.len),
        .cross_repo_deps = @intCast(table.cross_repo.dependency_edges.len),
        .enrich_enabled = false,
        .enrich_misses = 0,
    };
    if (enrich) {
        try ctx.stderr.print("warning: --enrich is not yet implemented in Zig; skipping enrichment pass\n", .{});
    }

    const regen = try engine.workspace.regenerate.regenerate(d, ctx.allocator, ctx.io, ctx.environ, org_id);
    defer engine.workspace.regenerate.deinitResult(regen, ctx.allocator);
    out.regenerate = .{
        .agents_path = try ctx.allocator.dupe(u8, regen.agents_path),
        .bytes = regen.bytes_written,
    };

    const strategy = try engine.identity.workspace.installSymlinks(ctx.allocator, ctx.io, workspace_root, layout);
    out.symlinks = .{
        .strategy = strategy,
        .installed = &.{ "AGENTS.md", "CLAUDE.md" },
    };
    return out;
}

fn capabilityRulesPath(
    allocator: std.mem.Allocator,
    environ: std.process.Environ,
) ![]u8 {
    const home = try engine.identity.workspace.planarHome(allocator, environ);
    defer allocator.free(home);
    return try std.fs.path.join(allocator, &.{ home, "templates", "workspace-capabilities.toml" });
}

fn registerWorkspace(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    cwd: []const u8,
    org_slug: []const u8,
    org_name: []const u8,
    git_children: []const []const u8,
    projects: *std.ArrayList(ProjectResult),
) !OrgResult {
    try d.savepoint(allocator, "workspace_init");
    var committed = false;
    defer {
        if (!committed) {
            d.rollbackToSavepoint(allocator, "workspace_init") catch {};
            d.releaseSavepoint(allocator, "workspace_init") catch {};
        }
    }

    const org = try upsertOrg(d, allocator, org_slug, org_name, cwd);

    for (git_children) |child_rel| {
        const repo_path = try std.fs.path.join(allocator, &.{ cwd, child_rel });
        defer allocator.free(repo_path);
        const base = std.fs.path.basename(child_rel);
        const slug = try engine.init.deriveSlug(allocator, base);
        defer allocator.free(slug);
        const project = try upsertProject(d, allocator, slug, base, repo_path);
        const member_created = try upsertMembership(d, project.id, org.id);
        try projects.append(allocator, .{
            .slug = try allocator.dupe(u8, slug),
            .path = try allocator.dupe(u8, repo_path),
            .created = project.created,
            .membership_created = member_created,
        });
    }

    try d.releaseSavepoint(allocator, "workspace_init");
    committed = true;
    return org;
}

fn upsertOrg(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    slug: []const u8,
    name: []const u8,
    root_path: []const u8,
) !OrgResult {
    var stmt = d.prepare("select id, kind, coalesce(config_json, '') from associations where slug = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .row => {
            const id = stmt.columnInt(0);
            const kind = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(kind);
            if (!std.mem.eql(u8, kind, "org")) return error.InvalidInput;
            return .{ .id = id, .created = false };
        },
        .done => {},
    }

    const config_json = try rootPathJSON(allocator, root_path);
    defer allocator.free(config_json);
    // Route the write through the identity engine so the org creation is
    // audited like every other association create (auto_detected falls to
    // the column default 0, matching the prior raw insert).
    const assoc = engine.identity.association.create(d, allocator, .{
        .slug = slug,
        .name = name,
        .kind = .org,
        .config_json = config_json,
    }) catch return error.QueryFailed;
    defer engine.identity.association.deinit(assoc, allocator);
    return .{ .id = assoc.id, .created = true };
}

fn upsertProject(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    slug: []const u8,
    name: []const u8,
    root_path: []const u8,
) !struct { id: i64, created: bool } {
    var stmt = d.prepare("select id from projects where slug = ?") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .row => return .{ .id = stmt.columnInt(0), .created = false },
        .done => {},
    }

    const remote = gitRemoteOrigin(allocator, root_path) catch null;
    defer if (remote) |value| allocator.free(value);
    // Route the write through the identity engine so the project create is
    // audited; project.add takes the same (slug, name, root_path,
    // git_remote) shape as the prior raw insert.
    const project = engine.identity.project.add(d, allocator, .{
        .slug = slug,
        .name = name,
        .root_path = root_path,
        .git_remote = remote,
    }) catch return error.QueryFailed;
    defer engine.identity.project.deinit(project, allocator);
    return .{ .id = project.id, .created = true };
}

// NOTE: this stays a direct write because the identity engine has no
// project_id-based link API — `association.addMember` resolves the project
// by path (findOrCreateProjectByPath) rather than taking the project_id we
// already hold here. A future `association.linkProject(project_id,
// assoc_id, source)` would let this delegate like upsertOrg/upsertProject.
fn upsertMembership(d: *db.sqlite.Db, project_id: i64, org_id: i64) !bool {
    var stmt = d.prepare(
        \\select count(*) from project_associations
        \\where project_id = ? and association_id = ?
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = project_id }, .{ .int = org_id } }) catch return error.QueryFailed;
    const existing = switch (stmt.step() catch return error.QueryFailed) {
        .done => 0,
        .row => stmt.columnInt(0),
    };
    if (existing > 0) return false;
    _ = d.execParams(
        \\insert into project_associations (project_id, association_id, source)
        \\values (?, ?, 'user')
    , &.{ .{ .int = project_id }, .{ .int = org_id } }) catch return error.QueryFailed;
    return true;
}

fn scanGitChildren(
    allocator: std.mem.Allocator,
    cwd: []const u8,
    max_depth: usize,
) ![]const []const u8 {
    var out = std.ArrayList([]const u8).empty;
    errdefer {
        freeStrings(out.items, allocator);
        out.deinit(allocator);
    }
    try walk(allocator, cwd, "", 1, max_depth, &out);
    std.mem.sort([]const u8, out.items, {}, struct {
        fn lessThan(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.lessThan(u8, a, b);
        }
    }.lessThan);
    return try out.toOwnedSlice(allocator);
}

fn walk(
    allocator: std.mem.Allocator,
    cwd: []const u8,
    rel: []const u8,
    depth: usize,
    max_depth: usize,
    out: *std.ArrayList([]const u8),
) !void {
    if (depth > max_depth) return;
    const abs_dir = if (rel.len == 0) try allocator.dupe(u8, cwd) else try std.fs.path.join(allocator, &.{ cwd, rel });
    defer allocator.free(abs_dir);

    var dir = try std.Io.Dir.cwd().openDir(ctxIo(), abs_dir, .{ .iterate = true });
    defer dir.close(ctxIo());
    var it = dir.iterate();
    while (try it.next(ctxIo())) |entry| {
        if (entry.kind != .directory) continue;
        if (shouldSkipChild(entry.name)) continue;
        const child_rel = if (rel.len == 0)
            try allocator.dupe(u8, entry.name)
        else
            try std.fs.path.join(allocator, &.{ rel, entry.name });
        defer allocator.free(child_rel);
        const git_path = try std.fs.path.join(allocator, &.{ cwd, child_rel, ".git" });
        defer allocator.free(git_path);
        if (pathExists(git_path)) {
            try out.append(allocator, try allocator.dupe(u8, child_rel));
            continue;
        }
        if (depth + 1 <= max_depth) try walk(allocator, cwd, child_rel, depth + 1, max_depth, out);
    }
}

fn shouldSkipChild(name: []const u8) bool {
    if (name.len == 0) return true;
    if (name[0] == '.') return true;
    return std.mem.eql(u8, name, "node_modules");
}

fn rootPathJSON(allocator: std.mem.Allocator, root_path: []const u8) ![]u8 {
    var writer: std.Io.Writer.Allocating = .init(allocator);
    defer writer.deinit();
    try writer.writer.print("{{\"root_path\":", .{});
    try std.json.Stringify.encodeJsonString(root_path, .{}, &writer.writer);
    try writer.writer.print("}}", .{});
    try writer.writer.flush();
    return try allocator.dupe(u8, writer.written());
}

fn gitRemoteOrigin(allocator: std.mem.Allocator, cwd: []const u8) !?[]u8 {
    const result = std.process.run(allocator, ctxIo(), .{
        .argv = &.{ "git", "-C", cwd, "remote", "get-url", "origin" },
    }) catch return null;
    defer allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        allocator.free(result.stdout);
        return null;
    }
    const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
    if (trimmed.len == 0) {
        allocator.free(result.stdout);
        return null;
    }
    return result.stdout;
}

fn pathExists(path: []const u8) bool {
    std.Io.Dir.cwd().access(ctxIo(), path, .{}) catch return false;
    return true;
}

fn freeStrings(items: []const []const u8, allocator: std.mem.Allocator) void {
    for (items) |item| allocator.free(item);
    allocator.free(items);
}

fn ctxIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}
