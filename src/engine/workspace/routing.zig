//! engine/workspace/routing — build + query workspace routing tables.

const std = @import("std");
const db = @import("db");
const identity = @import("../identity.zig");
const config_parse = @import("../config/parse.zig");

pub const SchemaVersionCurrent: i64 = 1;
pub const GeneratorVersionStatic = "static-v1";

pub const RoutingTable = identity.workspace.RoutingTable;
pub const ProjectRoute = identity.workspace.ProjectRoute;
pub const PlanarFocus = identity.workspace.PlanarFocus;
pub const CrossRepo = identity.workspace.CrossRepo;
pub const DependencyEdge = identity.workspace.DependencyEdge;

pub const CapabilityRule = struct {
    tag: []const u8,
    match_all: []const []const u8,
    match_any: []const []const u8,
    package_dep: []const u8,
    go_main: bool,
};

pub const CapabilityRules = struct {
    rules: []const CapabilityRule,
};

pub const ProjectOverride = struct {
    summary: ?[]const u8 = null,
    capabilities: ?[]const []const u8 = null,
    depends_on: ?[]const []const u8 = null,
};

pub const Overrides = struct {
    schema_version: i64 = 0,
    projects: std.StringHashMap(ProjectOverride),
};

const MemberRow = struct {
    id: i64,
    slug: []const u8,
    root_path: []const u8,
    git_remote: []const u8,
};

pub fn deinit(table: RoutingTable, allocator: std.mem.Allocator) void {
    allocator.free(table.workspace_slug);
    allocator.free(table.workspace_name);
    allocator.free(table.generated_at);
    allocator.free(table.generator_version);
    for (table.projects) |project| deinitProject(project, allocator);
    allocator.free(table.projects);
    allocator.free(table.cross_repo.plans_scoped_to_org);
    allocator.free(table.cross_repo.questions_scoped_to_org);
    for (table.cross_repo.dependency_edges) |edge| {
        allocator.free(edge.from);
        allocator.free(edge.to);
        allocator.free(edge.reason);
    }
    allocator.free(table.cross_repo.dependency_edges);
}

pub fn deinitCapabilityRules(rules: CapabilityRules, allocator: std.mem.Allocator) void {
    for (rules.rules) |rule| {
        allocator.free(rule.tag);
        freeStringArray(rule.match_all, allocator);
        freeStringArray(rule.match_any, allocator);
        allocator.free(rule.package_dep);
    }
    allocator.free(rules.rules);
}

pub fn deinitOverrides(overrides: Overrides, allocator: std.mem.Allocator) void {
    var it = overrides.projects.iterator();
    while (it.next()) |entry| {
        allocator.free(entry.key_ptr.*);
        if (entry.value_ptr.summary) |summary| allocator.free(summary);
        if (entry.value_ptr.capabilities) |caps| freeStringArray(caps, allocator);
        if (entry.value_ptr.depends_on) |deps| freeStringArray(deps, allocator);
    }
    var map = overrides.projects;
    map.deinit();
}

pub fn build(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    org_id: i64,
) !RoutingTable {
    const rules = try defaultCapabilityRules(allocator);
    defer deinitCapabilityRules(rules, allocator);
    return try buildWithRules(d, allocator, org_id, rules);
}

pub fn buildWithRules(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    org_id: i64,
    rules: CapabilityRules,
) !RoutingTable {
    const target = try std.fmt.allocPrint(allocator, "{d}", .{org_id});
    defer allocator.free(target);
    const org = try identity.workspace.resolveOrg(d, allocator, target);
    defer identity.workspace.deinitWorkspace(org, allocator);

    const members = try listMembers(d, allocator, org.id);
    defer deinitMembers(members, allocator);

    var projects = std.ArrayList(ProjectRoute).empty;
    errdefer {
        for (projects.items) |project| deinitProject(project, allocator);
        projects.deinit(allocator);
    }

    for (members) |member| {
        const project = try buildProject(d, allocator, member, members, rules);
        try projects.append(allocator, project);
    }

    const cross_repo = try buildCrossRepo(d, allocator, org.id, projects.items);
    errdefer deinitCrossRepo(cross_repo, allocator);

    return .{
        .schema_version = SchemaVersionCurrent,
        .workspace_id = org.id,
        .workspace_slug = try allocator.dupe(u8, org.slug),
        .workspace_name = try allocator.dupe(u8, org.name),
        .generated_at = try dbNow(d, allocator),
        .generator_version = try allocator.dupe(u8, GeneratorVersionStatic),
        .projects = try projects.toOwnedSlice(allocator),
        .cross_repo = cross_repo,
    };
}

pub fn write(path: []const u8, table: RoutingTable, allocator: std.mem.Allocator) !void {
    if (path.len == 0) return error.InvalidInput;

    var body: std.Io.Writer.Allocating = .init(allocator);
    defer body.deinit();
    try std.json.Stringify.value(table, .{ .whitespace = .indent_2 }, &body.writer);
    try body.writer.print("\n", .{});
    try body.writer.flush();

    const tmp = try std.fmt.allocPrint(allocator, "{s}.tmp", .{path});
    defer allocator.free(tmp);
    {
        try std.Io.Dir.cwd().writeFile(fsIo(), .{ .sub_path = tmp, .data = body.written() });
    }
    try std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), path, fsIo());
}

pub fn read(path: []const u8, allocator: std.mem.Allocator) !RoutingTable {
    const raw = try std.Io.Dir.cwd().readFileAlloc(
        fsIo(),
        path,
        allocator,
        std.Io.Limit.limited(16 * 1024 * 1024),
    );
    defer allocator.free(raw);

    var parsed = try std.json.parseFromSlice(std.json.Value, allocator, raw, .{});
    defer parsed.deinit();
    if (parsed.value != .object) return error.InvalidInput;
    return try fromJsonValue(allocator, parsed.value);
}

fn buildProject(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    member: MemberRow,
    siblings: []const MemberRow,
    rules: CapabilityRules,
) !ProjectRoute {
    var capabilities = std.ArrayList([]const u8).empty;
    errdefer {
        for (capabilities.items) |cap| allocator.free(cap);
        capabilities.deinit(allocator);
    }
    try detectCapabilities(allocator, member.root_path, rules, &capabilities);
    std.mem.sort([]const u8, capabilities.items, {}, struct {
        fn lessThan(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.lessThan(u8, a, b);
        }
    }.lessThan);
    dedupeSortedStrings(&capabilities);

    var dep_result = try inferDependencies(allocator, member.root_path, member.slug, siblings);
    defer dep_result.deinit(allocator);

    const entry_points = try detectEntryPoints(allocator, member.root_path);
    const languages = try countLanguages(allocator, member.root_path, 5000);
    errdefer {
        var it = languages.map.iterator();
        while (it.next()) |entry| allocator.free(entry.key_ptr.*);
        var tmp_languages = languages;
        tmp_languages.deinit(allocator);
    }

    const focus = try loadPlanarFocus(d, allocator, member.id);

    const summary = firstParagraphInRoot(allocator, member.root_path) catch "";
    defer if (summary.len > 0) allocator.free(summary);
    const summary_source = if (summary.len > 0) "readme" else "";
    const has_static_capabilities = capabilities.items.len > 0;

    return .{
        .slug = try allocator.dupe(u8, member.slug),
        .root_path = try allocator.dupe(u8, member.root_path),
        .git_remote = try allocator.dupe(u8, member.git_remote),
        .summary = if (summary.len > 0) try allocator.dupe(u8, summary) else try allocator.dupe(u8, ""),
        .summary_source = try allocator.dupe(u8, summary_source),
        .capabilities = try capabilities.toOwnedSlice(allocator),
        .capabilities_source = try allocator.dupe(u8, if (has_static_capabilities) "static" else ""),
        .depends_on = try dep_result.toOwnedSlice(allocator),
        .depends_on_source = try allocator.dupe(u8, dep_result.source),
        .entry_points = entry_points,
        .languages = languages,
        .planar_focus = focus,
    };
}

fn deinitProject(project: ProjectRoute, allocator: std.mem.Allocator) void {
    allocator.free(project.slug);
    allocator.free(project.root_path);
    allocator.free(project.git_remote);
    allocator.free(project.summary);
    allocator.free(project.summary_source);
    for (project.capabilities) |cap| allocator.free(cap);
    allocator.free(project.capabilities);
    allocator.free(project.capabilities_source);
    for (project.depends_on) |dep| allocator.free(dep);
    allocator.free(project.depends_on);
    allocator.free(project.depends_on_source);
    for (project.entry_points) |entry| allocator.free(entry);
    allocator.free(project.entry_points);
    var it = project.languages.map.iterator();
    while (it.next()) |entry| allocator.free(entry.key_ptr.*);
    var languages = project.languages;
    languages.deinit(allocator);
    allocator.free(project.planar_focus.active_plans);
    allocator.free(project.planar_focus.recent_session_ids);
}

fn buildCrossRepo(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    org_id: i64,
    projects: []const ProjectRoute,
) !CrossRepo {
    const plans = try queryIDs(
        d,
        allocator,
        \\select id from plans
        \\where scope_kind = 'association' and scope_id = ?
        \\order by id
    ,
        org_id,
    );
    errdefer allocator.free(plans);

    const questions = try queryIDs(
        d,
        allocator,
        \\select id from questions
        \\where scope_kind = 'association' and scope_id = ? and status = 'open'
        \\order by id
    ,
        org_id,
    );
    errdefer allocator.free(questions);

    var edges = std.ArrayList(DependencyEdge).empty;
    errdefer {
        for (edges.items) |edge| {
            allocator.free(edge.from);
            allocator.free(edge.to);
            allocator.free(edge.reason);
        }
        edges.deinit(allocator);
    }
    for (projects) |project| {
        const reason = reasonFor(project.depends_on_source);
        for (project.depends_on) |target| {
            try edges.append(allocator, .{
                .from = try allocator.dupe(u8, project.slug),
                .to = try allocator.dupe(u8, target),
                .reason = try allocator.dupe(u8, reason),
            });
        }
    }
    std.mem.sort(DependencyEdge, edges.items, {}, struct {
        fn lessThan(_: void, a: DependencyEdge, b: DependencyEdge) bool {
            const from_less = std.mem.order(u8, a.from, b.from);
            if (from_less == .lt) return true;
            if (from_less == .gt) return false;
            return std.mem.lessThan(u8, a.to, b.to);
        }
    }.lessThan);

    return .{
        .plans_scoped_to_org = plans,
        .questions_scoped_to_org = questions,
        .dependency_edges = try edges.toOwnedSlice(allocator),
    };
}

fn deinitCrossRepo(cross_repo: CrossRepo, allocator: std.mem.Allocator) void {
    allocator.free(cross_repo.plans_scoped_to_org);
    allocator.free(cross_repo.questions_scoped_to_org);
    for (cross_repo.dependency_edges) |edge| {
        allocator.free(edge.from);
        allocator.free(edge.to);
        allocator.free(edge.reason);
    }
    allocator.free(cross_repo.dependency_edges);
}

fn listMembers(d: *db.sqlite.Db, allocator: std.mem.Allocator, org_id: i64) ![]MemberRow {
    var stmt = d.prepare(
        \\select p.id, p.slug, coalesce(p.root_path, ''), coalesce(p.git_remote, '')
        \\from projects p
        \\join project_associations pa on pa.project_id = p.id
        \\where pa.association_id = ?
        \\order by p.slug
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = org_id }}) catch return error.QueryFailed;

    var out = std.ArrayList(MemberRow).empty;
    errdefer {
        for (out.items) |member| {
            allocator.free(member.slug);
            allocator.free(member.root_path);
            allocator.free(member.git_remote);
        }
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .slug = try stmt.columnTextAlloc(1, allocator),
                .root_path = try stmt.columnTextAlloc(2, allocator),
                .git_remote = try stmt.columnTextAlloc(3, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn deinitMembers(members: []const MemberRow, allocator: std.mem.Allocator) void {
    for (members) |member| {
        allocator.free(member.slug);
        allocator.free(member.root_path);
        allocator.free(member.git_remote);
    }
    allocator.free(members);
}

fn firstParagraphInRoot(allocator: std.mem.Allocator, root: []const u8) ![]const u8 {
    const names = [_][]const u8{ "README.md", "readme.md", "README", "readme" };
    for (names) |name| {
        const path = try std.fs.path.join(allocator, &.{ root, name });
        defer allocator.free(path);
        if (!pathExists(path)) continue;
        const paragraph = try firstParagraph(allocator, path);
        if (paragraph.len > 0) return paragraph;
        allocator.free(paragraph);
    }
    return try allocator.dupe(u8, "");
}

fn firstParagraph(allocator: std.mem.Allocator, filename: []const u8) ![]const u8 {
    const buf = std.Io.Dir.cwd().readFileAlloc(
        fsIo(),
        filename,
        allocator,
        std.Io.Limit.limited(64 * 1024),
    ) catch return try allocator.dupe(u8, "");
    defer allocator.free(buf);
    const normalized = try std.mem.replaceOwned(u8, allocator, buf, "\r\n", "\n");
    defer allocator.free(normalized);

    var lines = std.mem.splitScalar(u8, normalized, '\n');
    var current = std.ArrayList([]const u8).empty;
    defer current.deinit(allocator);
    var paras = std.ArrayList([]const []const u8).empty;
    defer paras.deinit(allocator);
    while (lines.next()) |line| {
        if (std.mem.trim(u8, line, " \t").len == 0) {
            if (current.items.len > 0) {
                try paras.append(allocator, try current.toOwnedSlice(allocator));
                current = std.ArrayList([]const u8).empty;
            }
            continue;
        }
        try current.append(allocator, line);
    }
    if (current.items.len > 0) try paras.append(allocator, try current.toOwnedSlice(allocator));

    for (paras.items) |para| {
        defer allocator.free(para);
        var start: usize = 0;
        if (para.len > 0 and std.mem.startsWith(u8, std.mem.trim(u8, para[0], " \t"), "#")) {
            if (para.len == 1) continue;
            start = 1;
        }
        var out = std.ArrayList(u8).empty;
        defer out.deinit(allocator);
        for (para[start..], 0..) |line, i| {
            if (i > 0) try out.append(allocator, ' ');
            try out.appendSlice(allocator, std.mem.trim(u8, line, " \t"));
        }
        const text = std.mem.trim(u8, out.items, " \t");
        if (text.len > 0) return try allocator.dupe(u8, text);
    }
    return try allocator.dupe(u8, "");
}

fn detectCapabilities(
    allocator: std.mem.Allocator,
    root: []const u8,
    rules: CapabilityRules,
    out: *std.ArrayList([]const u8),
) !void {
    for (rules.rules) |rule| {
        if (!ruleMatches(allocator, root, rule)) continue;
        if (rule.package_dep.len > 0 and !packageHasDep(allocator, root, rule.package_dep)) continue;
        if (rule.go_main and !hasGoMain(allocator, root)) continue;
        try out.append(allocator, try allocator.dupe(u8, rule.tag));
    }
    if (containsString(out.items, "go-service")) removeString(out, "go-library");
}

fn ruleMatches(allocator: std.mem.Allocator, root: []const u8, rule: CapabilityRule) bool {
    if (rule.match_all.len == 0 and rule.match_any.len == 0) return false;
    for (rule.match_all) |pattern| {
        if (!patternFinds(allocator, root, pattern)) return false;
    }
    if (rule.match_any.len > 0) {
        var matched = false;
        for (rule.match_any) |pattern| {
            if (patternFinds(allocator, root, pattern)) {
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    return true;
}

fn patternFinds(allocator: std.mem.Allocator, root: []const u8, pattern: []const u8) bool {
    if (pattern.len == 0) return false;
    var dir = root;
    var glob = pattern;
    if (std.mem.indexOfScalar(u8, pattern, '/')) |idx| {
        const subdir = std.fs.path.join(allocator, &.{ root, pattern[0..idx] }) catch return false;
        defer allocator.free(subdir);
        dir = subdir;
        glob = pattern[idx + 1 ..];
    }
    var opened = std.Io.Dir.cwd().openDir(fsIo(), dir, .{ .iterate = true }) catch return false;
    defer opened.close(fsIo());
    var it = opened.iterate();
    while (it.next(fsIo()) catch return false) |entry| {
        if (simpleGlobMatch(glob, entry.name)) return true;
    }
    return false;
}

fn simpleGlobMatch(pattern: []const u8, candidate: []const u8) bool {
    if (std.mem.indexOfScalar(u8, pattern, '*') == null) return std.mem.eql(u8, pattern, candidate);

    var pat_it = std.mem.splitScalar(u8, pattern, '*');
    var first = true;
    var cursor: usize = 0;
    while (pat_it.next()) |part| {
        if (part.len == 0) {
            first = false;
            continue;
        }
        if (first and !std.mem.startsWith(u8, candidate, part)) return false;
        if (first) {
            cursor = part.len;
            first = false;
            continue;
        }
        const idx = std.mem.indexOfPos(u8, candidate, cursor, part) orelse return false;
        cursor = idx + part.len;
    }
    if (pattern.len > 0 and pattern[pattern.len - 1] != '*') {
        const last = std.mem.lastIndexOfScalar(u8, pattern, '*') orelse 0;
        const suffix = pattern[last + 1 ..];
        if (suffix.len > 0 and !std.mem.endsWith(u8, candidate, suffix)) return false;
    }
    return true;
}

fn hasGoMain(allocator: std.mem.Allocator, root: []const u8) bool {
    const root_main = hasGoMainInDir(allocator, root);
    if (root_main) return true;
    const cmd_dir = std.fs.path.join(allocator, &.{ root, "cmd" }) catch return false;
    defer allocator.free(cmd_dir);
    var dir = std.Io.Dir.cwd().openDir(fsIo(), cmd_dir, .{ .iterate = true }) catch return false;
    defer dir.close(fsIo());
    var it = dir.iterate();
    while (it.next(fsIo()) catch return false) |entry| {
        if (entry.kind != .directory) continue;
        const sub = std.fs.path.join(allocator, &.{ cmd_dir, entry.name }) catch continue;
        defer allocator.free(sub);
        if (hasGoMainInDir(allocator, sub)) return true;
    }
    return false;
}

fn hasGoMainInDir(allocator: std.mem.Allocator, dir_path: []const u8) bool {
    var dir = std.Io.Dir.cwd().openDir(fsIo(), dir_path, .{ .iterate = true }) catch return false;
    defer dir.close(fsIo());
    var it = dir.iterate();
    while (it.next(fsIo()) catch return false) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".go")) continue;
        const path = std.fs.path.join(allocator, &.{ dir_path, entry.name }) catch continue;
        defer allocator.free(path);
        if (fileDeclaresPackageMain(allocator, path)) return true;
    }
    return false;
}

fn fileDeclaresPackageMain(allocator: std.mem.Allocator, path: []const u8) bool {
    const raw = std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(512 * 1024)) catch return false;
    defer allocator.free(raw);
    var lines = std.mem.splitScalar(u8, raw, '\n');
    while (lines.next()) |line| {
        const s = std.mem.trim(u8, line, " \t\r");
        if (s.len == 0 or std.mem.startsWith(u8, s, "//")) continue;
        if (std.mem.startsWith(u8, s, "package ")) {
            return std.mem.eql(u8, std.mem.trim(u8, s["package ".len..], " \t"), "main");
        }
    }
    return false;
}

const InferDepsResult = struct {
    deps: std.ArrayList([]const u8),
    source: []const u8,

    fn deinit(self: InferDepsResult, allocator: std.mem.Allocator) void {
        for (self.deps.items) |dep| allocator.free(dep);
        var deps = self.deps;
        deps.deinit(allocator);
    }

    fn toOwnedSlice(self: *InferDepsResult, allocator: std.mem.Allocator) ![]const []const u8 {
        return try self.deps.toOwnedSlice(allocator);
    }
};

fn inferDependencies(
    allocator: std.mem.Allocator,
    project_root: []const u8,
    project_slug: []const u8,
    siblings: []const MemberRow,
) !InferDepsResult {
    var dep_set = std.StringHashMap(void).init(allocator);
    defer dep_set.deinit();
    var source: []const u8 = "";

    const gomod_path = try std.fs.path.join(allocator, &.{ project_root, "go.mod" });
    defer allocator.free(gomod_path);
    if (pathExists(gomod_path)) {
        const body = std.Io.Dir.cwd().readFileAlloc(fsIo(), gomod_path, allocator, std.Io.Limit.limited(2 * 1024 * 1024)) catch "";
        defer if (body.len > 0) allocator.free(body);
        var lines = std.mem.splitScalar(u8, body, '\n');
        while (lines.next()) |line| {
            const idx = std.mem.indexOf(u8, line, "=>") orelse continue;
            const rhs = std.mem.trim(u8, line[idx + 2 ..], " \t\r");
            if (rhs.len == 0) continue;
            if (!std.mem.startsWith(u8, rhs, ".") and !std.mem.startsWith(u8, rhs, "/")) continue;
            const abs = if (std.fs.path.isAbsolute(rhs))
                try allocator.dupe(u8, std.mem.trim(u8, rhs, " \t"))
            else
                try std.fs.path.resolve(allocator, &.{ project_root, rhs });
            defer allocator.free(abs);
            for (siblings) |sib| {
                if (std.mem.eql(u8, sib.slug, project_slug)) continue;
                if (std.mem.eql(u8, sib.root_path, abs)) {
                    try dep_set.put(try allocator.dupe(u8, sib.slug), {});
                    source = "go.mod";
                }
            }
        }
    }

    const package_path = try std.fs.path.join(allocator, &.{ project_root, "package.json" });
    defer allocator.free(package_path);
    if (pathExists(package_path)) {
        const raw = std.Io.Dir.cwd().readFileAlloc(fsIo(), package_path, allocator, std.Io.Limit.limited(2 * 1024 * 1024)) catch "";
        defer if (raw.len > 0) allocator.free(raw);
        if (raw.len > 0) {
            var parsed = std.json.parseFromSlice(std.json.Value, allocator, raw, .{}) catch null;
            if (parsed) |*p| {
                defer p.deinit();
                if (p.value == .object) {
                    try collectWorkspaceDeps(allocator, p.value.object.get("dependencies"), siblings, project_slug, &dep_set);
                    try collectWorkspaceDeps(allocator, p.value.object.get("devDependencies"), siblings, project_slug, &dep_set);
                    if (source.len == 0 and dep_set.count() > 0) source = "package.json";
                }
            }
        }
    }

    var deps = std.ArrayList([]const u8).empty;
    errdefer {
        for (deps.items) |dep| allocator.free(dep);
        deps.deinit(allocator);
    }
    var it = dep_set.iterator();
    while (it.next()) |entry| {
        try deps.append(allocator, try allocator.dupe(u8, entry.key_ptr.*));
    }
    std.mem.sort([]const u8, deps.items, {}, struct {
        fn lessThan(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.lessThan(u8, a, b);
        }
    }.lessThan);
    return .{ .deps = deps, .source = source };
}

fn collectWorkspaceDeps(
    allocator: std.mem.Allocator,
    value: ?std.json.Value,
    siblings: []const MemberRow,
    project_slug: []const u8,
    dep_set: *std.StringHashMap(void),
) !void {
    if (value == null or value.? != .object) return;
    var it = value.?.object.iterator();
    while (it.next()) |entry| {
        if (entry.value_ptr.* != .string) continue;
        if (!std.mem.startsWith(u8, entry.value_ptr.*.string, "workspace:")) continue;
        for (siblings) |sib| {
            if (std.mem.eql(u8, sib.slug, project_slug)) continue;
            if (std.mem.eql(u8, sib.slug, entry.key_ptr.*)) {
                try dep_set.put(try allocator.dupe(u8, sib.slug), {});
                break;
            }
        }
    }
}

fn detectEntryPoints(allocator: std.mem.Allocator, root: []const u8) ![]const []const u8 {
    var found = std.StringHashMap(void).init(allocator);
    defer found.deinit();

    const cmd_dir = try std.fs.path.join(allocator, &.{ root, "cmd" });
    defer allocator.free(cmd_dir);
    if (pathExists(cmd_dir)) {
        var dir = std.Io.Dir.cwd().openDir(fsIo(), cmd_dir, .{ .iterate = true }) catch null;
        if (dir) |*d| {
            defer d.close(fsIo());
            var it = d.iterate();
            while (it.next(fsIo()) catch null) |entry| {
                if (entry.kind != .directory) continue;
                const rel = try std.fmt.allocPrint(allocator, "cmd/{s}/main.go", .{entry.name});
                defer allocator.free(rel);
                const abs = try std.fs.path.join(allocator, &.{ root, rel });
                defer allocator.free(abs);
                if (pathExists(abs)) try found.put(try allocator.dupe(u8, rel), {});
            }
        }
    }

    const static_candidates = [_][]const u8{
        "src/index.ts", "src/index.tsx",  "src/index.js", "src/index.jsx",
        "main.py",      "manage.py",      "Makefile",     "Cargo.toml",
        "package.json", "pyproject.toml", "go.mod",
    };
    for (static_candidates) |candidate| {
        const abs = try std.fs.path.join(allocator, &.{ root, candidate });
        defer allocator.free(abs);
        if (pathExists(abs)) try found.put(try allocator.dupe(u8, candidate), {});
    }

    const bin_dir = try std.fs.path.join(allocator, &.{ root, "bin" });
    defer allocator.free(bin_dir);
    if (pathExists(bin_dir)) {
        var dir = std.Io.Dir.cwd().openDir(fsIo(), bin_dir, .{ .iterate = true }) catch null;
        if (dir) |*d| {
            defer d.close(fsIo());
            var it = d.iterate();
            while (it.next(fsIo()) catch null) |entry| {
                if (entry.kind != .file) continue;
                if (entry.name.len > 0 and entry.name[0] == '.') continue;
                const rel = try std.fmt.allocPrint(allocator, "bin/{s}", .{entry.name});
                defer allocator.free(rel);
                try found.put(try allocator.dupe(u8, rel), {});
            }
        }
    }

    var out = std.ArrayList([]const u8).empty;
    errdefer {
        for (out.items) |entry| allocator.free(entry);
        out.deinit(allocator);
    }
    var it = found.iterator();
    while (it.next()) |entry| try out.append(allocator, try allocator.dupe(u8, entry.key_ptr.*));
    std.mem.sort([]const u8, out.items, {}, struct {
        fn lessThan(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.lessThan(u8, a, b);
        }
    }.lessThan);
    if (out.items.len > 5) out.items.len = 5;
    return try out.toOwnedSlice(allocator);
}

fn countLanguages(
    allocator: std.mem.Allocator,
    root: []const u8,
    max_files: usize,
) !std.json.ArrayHashMap(f64) {
    var counts = std.StringHashMap(i64).init(allocator);
    defer {
        var it = counts.iterator();
        while (it.next()) |entry| allocator.free(entry.key_ptr.*);
        counts.deinit();
    }

    var visited: usize = 0;
    try walkLanguage(allocator, root, max_files, &visited, &counts);

    var total: i64 = 0;
    var it_total = counts.iterator();
    while (it_total.next()) |entry| total += entry.value_ptr.*;

    var out: std.json.ArrayHashMap(f64) = .{};
    if (total == 0) return out;
    var it = counts.iterator();
    while (it.next()) |entry| {
        const pct = @as(f64, @floatFromInt(entry.value_ptr.*)) / @as(f64, @floatFromInt(total));
        const rounded = @round(pct * 100.0) / 100.0;
        try out.map.put(allocator, try allocator.dupe(u8, entry.key_ptr.*), rounded);
    }
    return out;
}

fn walkLanguage(
    allocator: std.mem.Allocator,
    dir_path: []const u8,
    max_files: usize,
    visited: *usize,
    counts: *std.StringHashMap(i64),
) !void {
    var dir = std.Io.Dir.cwd().openDir(fsIo(), dir_path, .{ .iterate = true }) catch return;
    defer dir.close(fsIo());

    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        switch (entry.kind) {
            .directory => {
                if (shouldSkipDir(entry.name)) continue;
                const sub = try std.fs.path.join(allocator, &.{ dir_path, entry.name });
                defer allocator.free(sub);
                try walkLanguage(allocator, sub, max_files, visited, counts);
            },
            .file => {
                visited.* += 1;
                if (visited.* > max_files) return;
                const lang = languageForExt(std.fs.path.extension(entry.name)) orelse continue;
                const key = try allocator.dupe(u8, lang);
                const gop = counts.getOrPut(key) catch |err| {
                    allocator.free(key);
                    return err;
                };
                if (gop.found_existing) {
                    allocator.free(key);
                } else {
                    gop.value_ptr.* = 0;
                }
                gop.value_ptr.* += 1;
            },
            else => {},
        }
    }
}

fn loadPlanarFocus(d: *db.sqlite.Db, allocator: std.mem.Allocator, project_id: i64) !PlanarFocus {
    const active_plans = try queryIDs2(
        d,
        allocator,
        \\select distinct id from plans
        \\where status = 'active' and (
        \\  (scope_kind = 'repo' and scope_id = ?)
        \\  or id in (
        \\    select from_id from entity_links
        \\    where from_kind = 'plan' and to_kind = 'repo' and to_id = ? and relationship = 'touches'
        \\  )
        \\)
        \\order by id
    ,
        project_id,
        project_id,
    );
    errdefer allocator.free(active_plans);

    const open_tasks = try scalarCount(d,
        \\select count(*) from tasks
        \\where status in ('todo','doing','blocked') and scope_kind = 'repo' and scope_id = ?
    , project_id);
    const open_questions = try scalarCount(d,
        \\select count(*) from questions
        \\where status = 'open' and scope_kind = 'repo' and scope_id = ?
    , project_id);

    return .{
        .active_plans = active_plans,
        .open_tasks = open_tasks,
        .open_questions = open_questions,
        .recent_session_ids = try allocator.alloc(i64, 0),
    };
}

fn reasonFor(source: []const u8) []const u8 {
    if (std.mem.eql(u8, source, "go.mod")) return "go.mod replace";
    if (std.mem.eql(u8, source, "package.json")) return "package.json workspace dep";
    if (std.mem.eql(u8, source, "manual")) return "operator override";
    return source;
}

fn dbNow(d: *db.sqlite.Db, allocator: std.mem.Allocator) ![]const u8 {
    var stmt = d.prepare("select strftime('%Y-%m-%dT%H:%M:%SZ','now')") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.QueryFailed,
        .row => return try stmt.columnTextAlloc(0, allocator),
    }
}

fn queryIDs(d: *db.sqlite.Db, allocator: std.mem.Allocator, sql: [:0]const u8, arg: i64) ![]const i64 {
    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = arg }}) catch return error.QueryFailed;
    var out = std.ArrayList(i64).empty;
    defer out.deinit(allocator);
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, stmt.columnInt(0)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn queryIDs2(d: *db.sqlite.Db, allocator: std.mem.Allocator, sql: [:0]const u8, arg1: i64, arg2: i64) ![]const i64 {
    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = arg1 }, .{ .int = arg2 } }) catch return error.QueryFailed;
    var out = std.ArrayList(i64).empty;
    defer out.deinit(allocator);
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, stmt.columnInt(0)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn scalarCount(d: *db.sqlite.Db, sql: [:0]const u8, arg: i64) !i64 {
    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = arg }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return 0,
        .row => return stmt.columnInt(0),
    }
}

fn packageHasDep(allocator: std.mem.Allocator, root: []const u8, dep: []const u8) bool {
    const package_path = std.fs.path.join(allocator, &.{ root, "package.json" }) catch return false;
    defer allocator.free(package_path);
    const raw = std.Io.Dir.cwd().readFileAlloc(fsIo(), package_path, allocator, std.Io.Limit.limited(2 * 1024 * 1024)) catch return false;
    defer allocator.free(raw);
    var parsed = std.json.parseFromSlice(std.json.Value, allocator, raw, .{}) catch return false;
    defer parsed.deinit();
    if (parsed.value != .object) return false;
    return jsonObjectHasKey(parsed.value.object.get("dependencies"), dep) or
        jsonObjectHasKey(parsed.value.object.get("devDependencies"), dep);
}

fn jsonObjectHasKey(value: ?std.json.Value, key: []const u8) bool {
    if (value == null or value.? != .object) return false;
    return value.?.object.get(key) != null;
}

fn globExists(allocator: std.mem.Allocator, root: []const u8, ext: []const u8) bool {
    var dir = std.Io.Dir.cwd().openDir(fsIo(), root, .{ .iterate = true }) catch return false;
    defer dir.close(fsIo());
    var it = dir.iterate();
    while (it.next(fsIo()) catch return false) |entry| {
        if (entry.kind == .file and std.mem.endsWith(u8, entry.name, ext)) return true;
        if (entry.kind == .directory and !shouldSkipDir(entry.name)) {
            const sub = std.fs.path.join(allocator, &.{ root, entry.name }) catch continue;
            defer allocator.free(sub);
            if (globExists(allocator, sub, ext)) return true;
        }
    }
    return false;
}

fn languageForExt(ext: []const u8) ?[]const u8 {
    if (std.mem.eql(u8, ext, ".go")) return "go";
    if (std.mem.eql(u8, ext, ".js") or std.mem.eql(u8, ext, ".jsx") or std.mem.eql(u8, ext, ".mjs") or std.mem.eql(u8, ext, ".cjs")) return "javascript";
    if (std.mem.eql(u8, ext, ".ts") or std.mem.eql(u8, ext, ".tsx")) return "typescript";
    if (std.mem.eql(u8, ext, ".py")) return "python";
    if (std.mem.eql(u8, ext, ".rs")) return "rust";
    if (std.mem.eql(u8, ext, ".java")) return "java";
    if (std.mem.eql(u8, ext, ".kt")) return "kotlin";
    if (std.mem.eql(u8, ext, ".rb")) return "ruby";
    if (std.mem.eql(u8, ext, ".sh") or std.mem.eql(u8, ext, ".bash") or std.mem.eql(u8, ext, ".zsh")) return "shell";
    if (std.mem.eql(u8, ext, ".md") or std.mem.eql(u8, ext, ".markdown")) return "markdown";
    if (std.mem.eql(u8, ext, ".toml")) return "toml";
    if (std.mem.eql(u8, ext, ".yaml") or std.mem.eql(u8, ext, ".yml")) return "yaml";
    if (std.mem.eql(u8, ext, ".json")) return "json";
    if (std.mem.eql(u8, ext, ".html")) return "html";
    if (std.mem.eql(u8, ext, ".css") or std.mem.eql(u8, ext, ".scss")) return "css";
    if (std.mem.eql(u8, ext, ".sql")) return "sql";
    if (std.mem.eql(u8, ext, ".proto")) return "protobuf";
    if (std.mem.eql(u8, ext, ".c") or std.mem.eql(u8, ext, ".h")) return "c";
    if (std.mem.eql(u8, ext, ".cpp") or std.mem.eql(u8, ext, ".cc") or std.mem.eql(u8, ext, ".hpp")) return "cpp";
    if (std.mem.eql(u8, ext, ".swift")) return "swift";
    if (std.mem.eql(u8, ext, ".m") or std.mem.eql(u8, ext, ".mm")) return "objective-c";
    if (std.mem.eql(u8, ext, ".lua")) return "lua";
    if (std.mem.eql(u8, ext, ".php")) return "php";
    if (std.mem.eql(u8, ext, ".cs")) return "csharp";
    return null;
}

fn shouldSkipDir(name: []const u8) bool {
    if (name.len == 0) return true;
    if (name[0] == '.') return true;
    return std.mem.eql(u8, name, "node_modules") or
        std.mem.eql(u8, name, "vendor") or
        std.mem.eql(u8, name, "target") or
        std.mem.eql(u8, name, "dist") or
        std.mem.eql(u8, name, "build") or
        std.mem.eql(u8, name, ".venv");
}

fn pathExists(path: []const u8) bool {
    std.Io.Dir.cwd().access(fsIo(), path, .{}) catch return false;
    return true;
}

fn pathExistsJoin(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) bool {
    const path = std.fs.path.join(allocator, &.{ root, rel }) catch return false;
    defer allocator.free(path);
    return pathExists(path);
}

pub fn defaultCapabilityRules(allocator: std.mem.Allocator) !CapabilityRules {
    var rules = std.ArrayList(CapabilityRule).empty;
    errdefer {
        for (rules.items) |rule| {
            allocator.free(rule.tag);
            freeStringArray(rule.match_all, allocator);
            freeStringArray(rule.match_any, allocator);
            allocator.free(rule.package_dep);
        }
        rules.deinit(allocator);
    }
    try rules.append(allocator, try makeRule(allocator, "go-service", &.{"go.mod"}, &.{}, "", true));
    try rules.append(allocator, try makeRule(allocator, "go-library", &.{"go.mod"}, &.{}, "", false));
    try rules.append(allocator, try makeRule(allocator, "node-service", &.{"package.json"}, &.{}, "express", false));
    try rules.append(allocator, try makeRule(allocator, "react-app", &.{"package.json"}, &.{}, "react", false));
    try rules.append(allocator, try makeRule(allocator, "rust", &.{"Cargo.toml"}, &.{}, "", false));
    try rules.append(allocator, try makeRule(allocator, "python", &.{}, &.{ "pyproject.toml", "setup.py", "requirements.txt" }, "", false));
    try rules.append(allocator, try makeRule(allocator, "protobuf", &.{}, &.{ "*.proto", "proto/*.proto" }, "", false));
    return .{ .rules = try rules.toOwnedSlice(allocator) };
}

pub fn loadCapabilityRules(allocator: std.mem.Allocator, path: []const u8) !CapabilityRules {
    const raw = std.Io.Dir.cwd().readFileAlloc(
        fsIo(),
        path,
        allocator,
        std.Io.Limit.limited(256 * 1024),
    ) catch |err| switch (err) {
        error.FileNotFound => return .{ .rules = try allocator.alloc(CapabilityRule, 0) },
        else => return err,
    };
    defer allocator.free(raw);

    var rules = std.ArrayList(CapabilityRule).empty;
    errdefer {
        for (rules.items) |rule| {
            allocator.free(rule.tag);
            freeStringArray(rule.match_all, allocator);
            freeStringArray(rule.match_any, allocator);
            allocator.free(rule.package_dep);
        }
        rules.deinit(allocator);
    }

    var current: ?CapabilityRule = null;
    var lines = std.mem.splitScalar(u8, raw, '\n');
    while (lines.next()) |line_raw| {
        const line_no_comment = if (std.mem.indexOfScalar(u8, line_raw, '#')) |idx| line_raw[0..idx] else line_raw;
        const line = std.mem.trim(u8, line_no_comment, " \t\r");
        if (line.len == 0) continue;
        if (std.mem.eql(u8, line, "[[rule]]")) {
            if (current) |rule| try rules.append(allocator, rule);
            current = .{
                .tag = try allocator.dupe(u8, ""),
                .match_all = try allocator.alloc([]const u8, 0),
                .match_any = try allocator.alloc([]const u8, 0),
                .package_dep = try allocator.dupe(u8, ""),
                .go_main = false,
            };
            continue;
        }
        if (current == null) continue;
        if (std.mem.startsWith(u8, line, "tag")) {
            const value = try parseTomlString(allocator, line);
            allocator.free(current.?.tag);
            current.?.tag = value;
        } else if (std.mem.startsWith(u8, line, "match_all")) {
            const values = try parseTomlStringArray(allocator, line);
            freeStringArray(current.?.match_all, allocator);
            current.?.match_all = values;
        } else if (std.mem.startsWith(u8, line, "match_any")) {
            const values = try parseTomlStringArray(allocator, line);
            freeStringArray(current.?.match_any, allocator);
            current.?.match_any = values;
        } else if (std.mem.startsWith(u8, line, "package_dep")) {
            const value = try parseTomlString(allocator, line);
            allocator.free(current.?.package_dep);
            current.?.package_dep = value;
        } else if (std.mem.startsWith(u8, line, "go_main")) {
            current.?.go_main = std.mem.indexOf(u8, line, "true") != null;
        }
    }
    if (current) |rule| try rules.append(allocator, rule);
    return .{ .rules = try rules.toOwnedSlice(allocator) };
}

pub fn loadOverrides(allocator: std.mem.Allocator, path: []const u8) !Overrides {
    const raw = std.Io.Dir.cwd().readFileAlloc(
        fsIo(),
        path,
        allocator,
        std.Io.Limit.limited(4 * 1024 * 1024),
    ) catch |err| switch (err) {
        error.FileNotFound => return .{ .projects = std.StringHashMap(ProjectOverride).init(allocator) },
        else => return err,
    };
    defer allocator.free(raw);

    var parsed = try std.json.parseFromSlice(std.json.Value, allocator, raw, .{});
    defer parsed.deinit();
    if (parsed.value != .object) return error.InvalidInput;

    var projects = std.StringHashMap(ProjectOverride).init(allocator);
    errdefer {
        var it = projects.iterator();
        while (it.next()) |entry| {
            allocator.free(entry.key_ptr.*);
            if (entry.value_ptr.summary) |summary| allocator.free(summary);
            if (entry.value_ptr.capabilities) |caps| freeStringArray(caps, allocator);
            if (entry.value_ptr.depends_on) |deps| freeStringArray(deps, allocator);
        }
        projects.deinit();
    }

    const schema_version = getInteger(parsed.value.object, "schema_version") orelse 0;
    const projects_value = parsed.value.object.get("projects");
    if (projects_value != null and projects_value.? == .object) {
        var it = projects_value.?.object.iterator();
        while (it.next()) |entry| {
            if (entry.value_ptr.* != .object) continue;
            const pobj = entry.value_ptr.*.object;
            var ov: ProjectOverride = .{};
            if (getString(pobj, "summary")) |summary| ov.summary = try allocator.dupe(u8, summary);
            const caps = try parseStringArray(allocator, pobj.get("capabilities"));
            if (caps.len > 0 or hasArray(pobj.get("capabilities"))) {
                ov.capabilities = caps;
            } else {
                allocator.free(caps);
            }
            const deps = try parseStringArray(allocator, pobj.get("depends_on"));
            if (deps.len > 0 or hasArray(pobj.get("depends_on"))) {
                ov.depends_on = deps;
            } else {
                allocator.free(deps);
            }
            try projects.put(try allocator.dupe(u8, entry.key_ptr.*), ov);
        }
    }

    return .{
        .schema_version = schema_version,
        .projects = projects,
    };
}

pub fn applyOverrides(table: *RoutingTable, overrides: Overrides, allocator: std.mem.Allocator) !void {
    for (table.projects) |*project| {
        const ov = overrides.projects.get(project.slug) orelse continue;
        if (ov.summary) |summary| {
            allocator.free(project.summary);
            allocator.free(project.summary_source);
            project.summary = try allocator.dupe(u8, summary);
            project.summary_source = try allocator.dupe(u8, "manual");
        }
        if (ov.capabilities) |caps| {
            for (project.capabilities) |cap| allocator.free(cap);
            allocator.free(project.capabilities);
            allocator.free(project.capabilities_source);
            project.capabilities = try cloneSortedUniqueStrings(allocator, caps);
            project.capabilities_source = try allocator.dupe(u8, "manual");
        }
        if (ov.depends_on) |deps| {
            for (project.depends_on) |dep| allocator.free(dep);
            allocator.free(project.depends_on);
            allocator.free(project.depends_on_source);
            project.depends_on = try cloneSortedUniqueStrings(allocator, deps);
            project.depends_on_source = try allocator.dupe(u8, "manual");
        }
    }
}

fn hasArray(value: ?std.json.Value) bool {
    return value != null and value.? == .array;
}

fn makeRule(
    allocator: std.mem.Allocator,
    tag: []const u8,
    match_all: []const []const u8,
    match_any: []const []const u8,
    package_dep: []const u8,
    go_main: bool,
) !CapabilityRule {
    return .{
        .tag = try allocator.dupe(u8, tag),
        .match_all = try cloneStringArray(allocator, match_all),
        .match_any = try cloneStringArray(allocator, match_any),
        .package_dep = try allocator.dupe(u8, package_dep),
        .go_main = go_main,
    };
}

fn cloneStringArray(allocator: std.mem.Allocator, values: []const []const u8) ![]const []const u8 {
    var out = std.ArrayList([]const u8).empty;
    errdefer {
        for (out.items) |value| allocator.free(value);
        out.deinit(allocator);
    }
    for (values) |value| try out.append(allocator, try allocator.dupe(u8, value));
    return try out.toOwnedSlice(allocator);
}

fn cloneSortedUniqueStrings(allocator: std.mem.Allocator, values: []const []const u8) ![]const []const u8 {
    var out = std.ArrayList([]const u8).empty;
    errdefer {
        for (out.items) |value| allocator.free(value);
        out.deinit(allocator);
    }
    for (values) |value| try out.append(allocator, try allocator.dupe(u8, value));
    std.mem.sort([]const u8, out.items, {}, struct {
        fn lessThan(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.lessThan(u8, a, b);
        }
    }.lessThan);
    dedupeSortedStrings(&out);
    return try out.toOwnedSlice(allocator);
}

fn parseTomlString(allocator: std.mem.Allocator, line: []const u8) ![]const u8 {
    const eq = std.mem.indexOfScalar(u8, line, '=') orelse return error.InvalidInput;
    const rhs = std.mem.trim(u8, line[eq + 1 ..], " \t");
    if (rhs.len < 2 or rhs[0] != '"' or rhs[rhs.len - 1] != '"') return error.InvalidInput;
    return try allocator.dupe(u8, rhs[1 .. rhs.len - 1]);
}

fn parseTomlStringArray(allocator: std.mem.Allocator, line: []const u8) ![]const []const u8 {
    const eq = std.mem.indexOfScalar(u8, line, '=') orelse return error.InvalidInput;
    const rhs = std.mem.trim(u8, line[eq + 1 ..], " \t");
    var pe: config_parse.ParseError = .{ .line = 1, .column = 1, .message = "" };
    const wrapped = try std.fmt.allocPrint(allocator, "v = {s}\n", .{rhs});
    defer allocator.free(wrapped);
    var map = try config_parse.parse(allocator, wrapped, &pe);
    defer {
        config_parse.deinitMap(&map, allocator);
    }
    const value = map.get("v") orelse return error.InvalidInput;
    if (value != .array) return error.InvalidInput;
    return try cloneStringArray(allocator, value.array);
}

fn dedupeSortedStrings(values: *std.ArrayList([]const u8)) void {
    if (values.items.len <= 1) return;
    var write_idx: usize = 1;
    var read_idx: usize = 1;
    while (read_idx < values.items.len) : (read_idx += 1) {
        if (std.mem.eql(u8, values.items[read_idx], values.items[write_idx - 1])) continue;
        values.items[write_idx] = values.items[read_idx];
        write_idx += 1;
    }
    values.items.len = write_idx;
}

fn containsString(values: []const []const u8, needle: []const u8) bool {
    for (values) |value| {
        if (std.mem.eql(u8, value, needle)) return true;
    }
    return false;
}

fn removeString(values: *std.ArrayList([]const u8), needle: []const u8) void {
    var write_idx: usize = 0;
    for (values.items) |value| {
        if (std.mem.eql(u8, value, needle)) continue;
        values.items[write_idx] = value;
        write_idx += 1;
    }
    values.items.len = write_idx;
}

fn fromJsonValue(allocator: std.mem.Allocator, value: std.json.Value) !RoutingTable {
    const obj = value.object;
    const projects_value = obj.get("projects") orelse return error.InvalidInput;
    if (projects_value != .array) return error.InvalidInput;

    var projects = std.ArrayList(ProjectRoute).empty;
    errdefer {
        for (projects.items) |project| deinitProject(project, allocator);
        projects.deinit(allocator);
    }
    for (projects_value.array.items) |project_value| {
        try projects.append(allocator, try parseProject(allocator, project_value));
    }

    const cross_value = obj.get("cross_repo") orelse return error.InvalidInput;
    if (cross_value != .object) return error.InvalidInput;
    const cross = try parseCrossRepo(allocator, cross_value);
    errdefer deinitCrossRepo(cross, allocator);

    return .{
        .schema_version = getInteger(obj, "schema_version") orelse return error.InvalidInput,
        .workspace_id = getInteger(obj, "workspace_id") orelse return error.InvalidInput,
        .workspace_slug = try allocator.dupe(u8, getString(obj, "workspace_slug") orelse return error.InvalidInput),
        .workspace_name = try allocator.dupe(u8, getString(obj, "workspace_name") orelse return error.InvalidInput),
        .generated_at = try allocator.dupe(u8, getString(obj, "generated_at") orelse return error.InvalidInput),
        .generator_version = try allocator.dupe(u8, getString(obj, "generator_version") orelse GeneratorVersionStatic),
        .projects = try projects.toOwnedSlice(allocator),
        .cross_repo = cross,
    };
}

fn parseProject(allocator: std.mem.Allocator, value: std.json.Value) !ProjectRoute {
    if (value != .object) return error.InvalidInput;
    const obj = value.object;

    const capabilities = try parseStringArray(allocator, obj.get("capabilities"));
    errdefer freeStringArray(capabilities, allocator);
    const depends_on = try parseStringArray(allocator, obj.get("depends_on"));
    errdefer freeStringArray(depends_on, allocator);
    const entry_points = try parseStringArray(allocator, obj.get("entry_points"));
    errdefer freeStringArray(entry_points, allocator);

    const languages = try parseLanguages(allocator, obj.get("languages"));
    errdefer {
        var it = languages.map.iterator();
        while (it.next()) |entry| allocator.free(entry.key_ptr.*);
        var tmp_languages = languages;
        tmp_languages.deinit(allocator);
    }

    const focus = try parsePlanarFocus(allocator, obj.get("planar_focus") orelse return error.InvalidInput);

    return .{
        .slug = try allocator.dupe(u8, getString(obj, "slug") orelse ""),
        .root_path = try allocator.dupe(u8, getString(obj, "root_path") orelse ""),
        .git_remote = try allocator.dupe(u8, getString(obj, "git_remote") orelse ""),
        .summary = try allocator.dupe(u8, getString(obj, "summary") orelse ""),
        .summary_source = try allocator.dupe(u8, getString(obj, "summary_source") orelse ""),
        .capabilities = capabilities,
        .capabilities_source = try allocator.dupe(u8, getString(obj, "capabilities_source") orelse ""),
        .depends_on = depends_on,
        .depends_on_source = try allocator.dupe(u8, getString(obj, "depends_on_source") orelse ""),
        .entry_points = entry_points,
        .languages = languages,
        .planar_focus = focus,
    };
}

fn parseCrossRepo(allocator: std.mem.Allocator, value: std.json.Value) !CrossRepo {
    if (value != .object) return error.InvalidInput;
    const obj = value.object;
    const plans = try parseIntArray(allocator, obj.get("plans_scoped_to_org"));
    errdefer allocator.free(plans);
    const questions = try parseIntArray(allocator, obj.get("questions_scoped_to_org"));
    errdefer allocator.free(questions);

    const edges_value = obj.get("dependency_edges") orelse return error.InvalidInput;
    if (edges_value != .array) return error.InvalidInput;
    var edges = std.ArrayList(DependencyEdge).empty;
    errdefer {
        for (edges.items) |edge| {
            allocator.free(edge.from);
            allocator.free(edge.to);
            allocator.free(edge.reason);
        }
        edges.deinit(allocator);
    }
    for (edges_value.array.items) |edge_value| {
        if (edge_value != .object) continue;
        try edges.append(allocator, .{
            .from = try allocator.dupe(u8, getString(edge_value.object, "from") orelse ""),
            .to = try allocator.dupe(u8, getString(edge_value.object, "to") orelse ""),
            .reason = try allocator.dupe(u8, getString(edge_value.object, "reason") orelse ""),
        });
    }
    return .{
        .plans_scoped_to_org = plans,
        .questions_scoped_to_org = questions,
        .dependency_edges = try edges.toOwnedSlice(allocator),
    };
}

fn parsePlanarFocus(allocator: std.mem.Allocator, value: std.json.Value) !PlanarFocus {
    if (value != .object) return error.InvalidInput;
    const obj = value.object;
    return .{
        .active_plans = try parseIntArray(allocator, obj.get("active_plans")),
        .open_tasks = getInteger(obj, "open_tasks") orelse 0,
        .open_questions = getInteger(obj, "open_questions") orelse 0,
        .recent_session_ids = try parseIntArray(allocator, obj.get("recent_session_ids")),
    };
}

fn parseLanguages(
    allocator: std.mem.Allocator,
    value: ?std.json.Value,
) !std.json.ArrayHashMap(f64) {
    var out: std.json.ArrayHashMap(f64) = .{};
    if (value == null) return out;
    if (value.? != .object) return out;
    var it = value.?.object.iterator();
    while (it.next()) |entry| {
        if (entry.value_ptr.* != .float and entry.value_ptr.* != .integer) continue;
        const numeric = if (entry.value_ptr.* == .float)
            entry.value_ptr.*.float
        else
            @as(f64, @floatFromInt(entry.value_ptr.*.integer));
        try out.map.put(allocator, try allocator.dupe(u8, entry.key_ptr.*), numeric);
    }
    return out;
}

fn parseStringArray(allocator: std.mem.Allocator, value: ?std.json.Value) ![]const []const u8 {
    if (value == null or value.? != .array) return try allocator.alloc([]const u8, 0);
    var out = std.ArrayList([]const u8).empty;
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit(allocator);
    }
    for (value.?.array.items) |item| {
        if (item != .string) continue;
        try out.append(allocator, try allocator.dupe(u8, item.string));
    }
    return try out.toOwnedSlice(allocator);
}

fn parseIntArray(allocator: std.mem.Allocator, value: ?std.json.Value) ![]const i64 {
    if (value == null or value.? != .array) return try allocator.alloc(i64, 0);
    var out = std.ArrayList(i64).empty;
    defer out.deinit(allocator);
    for (value.?.array.items) |item| {
        if (item == .integer) try out.append(allocator, item.integer);
    }
    return try out.toOwnedSlice(allocator);
}

fn freeStringArray(values: []const []const u8, allocator: std.mem.Allocator) void {
    for (values) |value| allocator.free(value);
    allocator.free(values);
}

fn getString(obj: std.json.ObjectMap, key: []const u8) ?[]const u8 {
    const value = obj.get(key) orelse return null;
    if (value != .string) return null;
    return value.string;
}

fn getInteger(obj: std.json.ObjectMap, key: []const u8) ?i64 {
    const value = obj.get(key) orelse return null;
    if (value != .integer) return null;
    return value.integer;
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}
