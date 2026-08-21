//! integration_tests/workspace_test.zig — workspace routing/regenerate surface.

const std = @import("std");
const harness = @import("harness");

fn parseJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) T {
    const trimmed = std.mem.trim(u8, buf, " \n");
    const parsed = std.json.parseFromSlice(T, arena, trimmed, .{
        .ignore_unknown_fields = true,
    }) catch unreachable;
    return parsed.value;
}

fn makeAbsTmp(allocator: std.mem.Allocator, tmp: std.testing.TmpDir) ![]u8 {
    return try std.fs.path.join(allocator, &.{ ".zig-cache/tmp", &tmp.sub_path });
}

fn mkdirp(path: []const u8) !void {
    try std.Io.Dir.cwd().createDirPath(std.Io.Threaded.global_single_threaded.io(), path);
}

fn writeFile(path: []const u8, body: []const u8) !void {
    try std.Io.Dir.cwd().writeFile(std.Io.Threaded.global_single_threaded.io(), .{
        .sub_path = path,
        .data = body,
    });
}

fn deleteFile(path: []const u8) !void {
    try std.Io.Dir.cwd().deleteFile(std.Io.Threaded.global_single_threaded.io(), path);
}

fn expectMissing(path: []const u8) !void {
    std.Io.Dir.cwd().access(std.Io.Threaded.global_single_threaded.io(), path, .{}) catch |err| switch (err) {
        error.FileNotFound => return,
        else => return err,
    };
    return error.ExpectedMissingPath;
}

fn mustRunWithInDir(
    suite: *const harness.Suite,
    cwd: []const u8,
    args: []const []const u8,
    extra_env: []const harness.Suite.ExtraEnvEntry,
) []u8 {
    const res = suite.execWithInDir(cwd, args, extra_env);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "\nmustRunWithInDir: non-zero exit in cwd '{s}'\nstdout: {s}\nstderr: {s}\n",
            .{ cwd, res.stdout, res.stderr },
        );
        res.deinit(suite.allocator);
        @panic("mustRunWithInDir: non-zero exit");
    }
    suite.allocator.free(res.stderr);
    return res.stdout;
}

fn expectProjectPath(projects: anytype, slug: []const u8, path: []const u8) !void {
    for (projects) |project| {
        if (std.mem.eql(u8, project.slug, slug)) {
            try std.testing.expectEqualStrings(path, project.path);
            return;
        }
    }
    return error.MissingProject;
}

fn expectMemberPath(members: anytype, slug: []const u8, path: []const u8) !void {
    for (members) |member| {
        if (std.mem.eql(u8, member.slug, slug)) {
            try std.testing.expect(member.root_path != null);
            try std.testing.expectEqualStrings(path, member.root_path.?);
            return;
        }
    }
    return error.MissingMember;
}

fn findAssocConfig(assocs: anytype, slug: []const u8) ?[]const u8 {
    for (assocs) |assoc| {
        if (std.mem.eql(u8, assoc.slug, slug)) return assoc.config_json;
    }
    return null;
}

fn sqliteExec(allocator: std.mem.Allocator, db_path: []const u8, sql: []const u8) !void {
    const result = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "sqlite3", db_path, sql },
    }) catch |e| std.debug.panic("sqlite3 spawn failed: {s}", .{@errorName(e)});
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("sqlite3 failed: {s}\n", .{result.stderr});
        return error.SqliteFailed;
    }
}

test "workspace init builds routing table and regenerate writes AGENTS.md" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const ws = try std.fs.path.join(arena, &.{ root, "workspace" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    try mkdirp(ws);
    try mkdirp(home);

    const repo_a = try std.fs.path.join(arena, &.{ ws, "repo-a" });
    const repo_b = try std.fs.path.join(arena, &.{ ws, "repo-b" });
    try mkdirp(try std.fs.path.join(arena, &.{ repo_a, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ repo_b, ".git" }));
    try writeFile(try std.fs.path.join(arena, &.{ repo_a, "README.md" }), "Repo A summary.\n\nBody.\n");
    try writeFile(try std.fs.path.join(arena, &.{ repo_b, "README.md" }), "Repo B summary.\n");
    try writeFile(try std.fs.path.join(arena, &.{ repo_a, "go.mod" }), "module example.com/repo-a\n\ngo 1.22\n");
    try writeFile(try std.fs.path.join(arena, &.{ repo_b, "go.mod" }), "module example.com/repo-b\n\ngo 1.22\n");

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    try std.process.setCurrentPath(std.testing.io, ws);

    const extra_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
    };

    const init_out = suite.mustRunWith(&.{
        "workspace",
        "init",
        "--json",
        "--slug",
        "ws-org",
    }, &extra_env);
    defer gpa.free(init_out);

    const InitJSON = struct {
        org: struct { id: i64, slug: []const u8 },
        projects: []const struct { slug: []const u8 },
        pipeline: struct { skipped: bool, @"error": ?[]const u8 = null },
    };
    const init = parseJSON(InitJSON, arena, init_out);
    try std.testing.expectEqualStrings("ws-org", init.org.slug);
    try std.testing.expectEqual(@as(usize, 2), init.projects.len);
    try std.testing.expect(!init.pipeline.skipped);

    const show_out = suite.mustRunWith(&.{
        "workspace",
        "routing",
        "show",
        "--json",
        "ws-org",
    }, &extra_env);
    defer gpa.free(show_out);
    try std.testing.expect(std.mem.indexOf(u8, show_out, "\"repo-a\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, show_out, "\"repo-b\"") != null);

    const regen_out = suite.mustRunWith(&.{
        "workspace",
        "regenerate",
        "--json",
        "ws-org",
    }, &extra_env);
    defer gpa.free(regen_out);
    const RegenJSON = struct {
        agents_path: []const u8,
        bytes_written: i64,
    };
    const regen = parseJSON(RegenJSON, arena, regen_out);
    try std.testing.expect(regen.bytes_written > 0);

    try std.Io.Dir.cwd().access(std.Io.Threaded.global_single_threaded.io(), regen.agents_path, .{ .read = true });
}

test "workspace init meta repo registers root nested projects and shape config" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const meta = try std.fs.path.join(arena, &.{ root, "meta-root" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    const nested_a = try std.fs.path.join(arena, &.{ meta, "modules", "nested-a" });
    const nested_b = try std.fs.path.join(arena, &.{ meta, "services", "nested-b" });
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ meta, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ nested_a, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ nested_b, ".git" }));

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_meta = if (std.fs.path.isAbsolute(meta))
        meta
    else
        try std.fs.path.join(arena, &.{ prev_cwd, meta });
    const abs_nested_a = if (std.fs.path.isAbsolute(nested_a))
        nested_a
    else
        try std.fs.path.join(arena, &.{ prev_cwd, nested_a });
    const abs_nested_b = if (std.fs.path.isAbsolute(nested_b))
        nested_b
    else
        try std.fs.path.join(arena, &.{ prev_cwd, nested_b });
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    try std.process.setCurrentPath(std.testing.io, abs_meta);

    const extra_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
    };

    const init_out = suite.mustRunWith(&.{
        "workspace", "init", "--meta-repo", "--scan", "2", "--no-scan", "--json", "--slug", "meta-ws",
    }, &extra_env);
    defer gpa.free(init_out);
    const InitJSON = struct {
        org: struct { slug: []const u8, created: bool },
        projects: []const struct { slug: []const u8, path: []const u8 },
        pipeline: struct { skipped: bool },
    };
    const init = parseJSON(InitJSON, arena, init_out);
    try std.testing.expectEqualStrings("meta-ws", init.org.slug);
    try std.testing.expect(init.org.created);
    try std.testing.expectEqual(@as(usize, 3), init.projects.len);
    try std.testing.expect(init.pipeline.skipped);
    try expectProjectPath(init.projects, "meta-root", abs_meta);
    try expectProjectPath(init.projects, "nested-a", abs_nested_a);
    try expectProjectPath(init.projects, "nested-b", abs_nested_b);

    const members_out = suite.mustRunWith(&.{ "assoc", "members", "--json", "meta-ws" }, &extra_env);
    defer gpa.free(members_out);
    const MemberJSON = struct { slug: []const u8, root_path: ?[]const u8 = null };
    const members = parseJSON([]const MemberJSON, arena, members_out);
    try std.testing.expectEqual(@as(usize, 3), members.len);
    try expectMemberPath(members, "meta-root", abs_meta);
    try expectMemberPath(members, "nested-a", abs_nested_a);
    try expectMemberPath(members, "nested-b", abs_nested_b);

    const assoc_out = suite.mustRunWith(&.{ "assoc", "list", "--kind", "org", "--json" }, &extra_env);
    defer gpa.free(assoc_out);
    const AssocJSON = struct { slug: []const u8, config_json: ?[]const u8 = null };
    const assocs = parseJSON([]const AssocJSON, arena, assoc_out);
    const config_json = findAssocConfig(assocs, "meta-ws") orelse return error.MissingMetaWorkspaceConfig;
    const ConfigJSON = struct { root_path: []const u8, workspace_shape: []const u8 };
    const config = parseJSON(ConfigJSON, arena, config_json);
    try std.testing.expectEqualStrings(abs_meta, config.root_path);
    try std.testing.expectEqualStrings("meta-repo", config.workspace_shape);

    const rerun_out = suite.mustRunWith(&.{
        "workspace", "init", "--meta-repo", "--scan", "2", "--no-scan", "--json", "--slug", "meta-ws",
    }, &extra_env);
    defer gpa.free(rerun_out);
    const rerun = parseJSON(InitJSON, arena, rerun_out);
    try std.testing.expectEqualStrings("meta-ws", rerun.org.slug);
    try std.testing.expect(!rerun.org.created);
    try std.testing.expectEqual(@as(usize, 3), rerun.projects.len);

    const assoc_rerun_out = suite.mustRunWith(&.{ "assoc", "list", "--kind", "org", "--json" }, &extra_env);
    defer gpa.free(assoc_rerun_out);
    const assocs_rerun = parseJSON([]const AssocJSON, arena, assoc_rerun_out);
    const config_json_rerun = findAssocConfig(assocs_rerun, "meta-ws") orelse return error.MissingMetaWorkspaceConfig;
    const config_rerun = parseJSON(ConfigJSON, arena, config_json_rerun);
    try std.testing.expectEqualStrings(abs_meta, config_rerun.root_path);
    try std.testing.expectEqualStrings("meta-repo", config_rerun.workspace_shape);
}

test "workspace init meta repo refuses existing org slug at different root" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const meta_one = try std.fs.path.join(arena, &.{ root, "meta-one" });
    const meta_two = try std.fs.path.join(arena, &.{ root, "meta-two" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ meta_one, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ meta_one, "nested", ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ meta_two, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ meta_two, "nested", ".git" }));

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_meta_one = if (std.fs.path.isAbsolute(meta_one))
        meta_one
    else
        try std.fs.path.join(arena, &.{ prev_cwd, meta_one });
    const abs_meta_two = if (std.fs.path.isAbsolute(meta_two))
        meta_two
    else
        try std.fs.path.join(arena, &.{ prev_cwd, meta_two });
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });

    const env_one = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PWD", .value = abs_meta_one },
    };
    const init_one = mustRunWithInDir(&suite, abs_meta_one, &.{
        "workspace", "init", "--meta-repo", "--scan", "2", "--no-scan", "--json", "--slug", "collision-ws",
    }, &env_one);
    defer gpa.free(init_one);

    const assoc_before_out = suite.mustRunWith(&.{ "assoc", "list", "--kind", "org", "--json" }, &env_one);
    defer gpa.free(assoc_before_out);
    const AssocJSON = struct { slug: []const u8, config_json: ?[]const u8 = null };
    const before = parseJSON([]const AssocJSON, arena, assoc_before_out);
    const before_config = findAssocConfig(before, "collision-ws") orelse return error.MissingMetaWorkspaceConfig;

    const env_two = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PWD", .value = abs_meta_two },
    };
    const collision = suite.execWithInDir(abs_meta_two, &.{
        "workspace", "init", "--meta-repo", "--scan", "2", "--no-scan", "--slug", "collision-ws",
    }, &env_two);
    defer collision.deinit(gpa);
    try std.testing.expect(collision.term == .exited and collision.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, collision.stderr, "already exists with a different workspace root") != null);

    const assoc_after_out = suite.mustRunWith(&.{ "assoc", "list", "--kind", "org", "--json" }, &env_one);
    defer gpa.free(assoc_after_out);
    const after = parseJSON([]const AssocJSON, arena, assoc_after_out);
    const after_config = findAssocConfig(after, "collision-ws") orelse return error.MissingMetaWorkspaceConfig;
    try std.testing.expectEqualStrings(before_config, after_config);
}

test "workspace init meta repo discovers submodule git file markers" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const meta = try std.fs.path.join(arena, &.{ root, "meta-with-submodule" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    const submodule = try std.fs.path.join(arena, &.{ meta, "vendor", "submodule-a" });
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ meta, ".git" }));
    try mkdirp(submodule);
    try writeFile(try std.fs.path.join(arena, &.{ submodule, ".git" }), "gitdir: ../../.git/modules/vendor/submodule-a\n");

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_meta = if (std.fs.path.isAbsolute(meta))
        meta
    else
        try std.fs.path.join(arena, &.{ prev_cwd, meta });
    const abs_submodule = if (std.fs.path.isAbsolute(submodule))
        submodule
    else
        try std.fs.path.join(arena, &.{ prev_cwd, submodule });
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    try std.process.setCurrentPath(std.testing.io, abs_meta);

    const extra_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
    };
    const init_out = suite.mustRunWith(&.{
        "workspace", "init", "--meta-repo", "--scan", "2", "--no-scan", "--json", "--slug", "submodule-ws",
    }, &extra_env);
    defer gpa.free(init_out);
    try std.testing.expect(std.mem.indexOf(u8, init_out, "\"submodule-a\"") != null);

    const members_out = suite.mustRunWith(&.{ "assoc", "members", "--json", "submodule-ws" }, &extra_env);
    defer gpa.free(members_out);
    const MemberJSON = struct { slug: []const u8, root_path: ?[]const u8 = null };
    const members = parseJSON([]const MemberJSON, arena, members_out);
    try std.testing.expectEqual(@as(usize, 2), members.len);
    try expectMemberPath(members, "meta-with-submodule", abs_meta);
    try expectMemberPath(members, "submodule-a", abs_submodule);
}

test "workspace init meta repo disambiguates nested repos with duplicate basenames" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const meta = try std.fs.path.join(arena, &.{ root, "meta-duplicate" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    const modules_api = try std.fs.path.join(arena, &.{ meta, "modules", "api" });
    const services_api = try std.fs.path.join(arena, &.{ meta, "services", "api" });
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ meta, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ modules_api, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ services_api, ".git" }));

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_meta = if (std.fs.path.isAbsolute(meta))
        meta
    else
        try std.fs.path.join(arena, &.{ prev_cwd, meta });
    const abs_modules_api = if (std.fs.path.isAbsolute(modules_api))
        modules_api
    else
        try std.fs.path.join(arena, &.{ prev_cwd, modules_api });
    const abs_services_api = if (std.fs.path.isAbsolute(services_api))
        services_api
    else
        try std.fs.path.join(arena, &.{ prev_cwd, services_api });
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    try std.process.setCurrentPath(std.testing.io, abs_meta);

    const extra_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
    };
    const init_out = suite.mustRunWith(&.{
        "workspace", "init", "--meta-repo", "--scan", "2", "--no-scan", "--json", "--slug", "duplicate-ws",
    }, &extra_env);
    defer gpa.free(init_out);
    const InitJSON = struct {
        projects: []const struct { slug: []const u8, path: []const u8 },
    };
    const init = parseJSON(InitJSON, arena, init_out);
    try std.testing.expectEqual(@as(usize, 3), init.projects.len);
    try expectProjectPath(init.projects, "api", abs_modules_api);
    try expectProjectPath(init.projects, "api-2", abs_services_api);

    const members_out = suite.mustRunWith(&.{ "assoc", "members", "--json", "duplicate-ws" }, &extra_env);
    defer gpa.free(members_out);
    const MemberJSON = struct { slug: []const u8, root_path: ?[]const u8 = null };
    const members = parseJSON([]const MemberJSON, arena, members_out);
    try std.testing.expectEqual(@as(usize, 3), members.len);
    try expectMemberPath(members, "api", abs_modules_api);
    try expectMemberPath(members, "api-2", abs_services_api);
}

test "workspace init meta repo write scope safety and read set" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const meta = try std.fs.path.join(arena, &.{ root, "meta-safety" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    const root_src = try std.fs.path.join(arena, &.{ meta, "src" });
    const nested = try std.fs.path.join(arena, &.{ meta, "modules", "nested" });
    const nested_src = try std.fs.path.join(arena, &.{ nested, "src" });
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ meta, ".git" }));
    try mkdirp(root_src);
    try mkdirp(try std.fs.path.join(arena, &.{ nested, ".git" }));
    try mkdirp(nested_src);

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_meta = if (std.fs.path.isAbsolute(meta))
        meta
    else
        try std.fs.path.join(arena, &.{ prev_cwd, meta });
    const abs_root_src = if (std.fs.path.isAbsolute(root_src))
        root_src
    else
        try std.fs.path.join(arena, &.{ prev_cwd, root_src });
    const abs_nested = if (std.fs.path.isAbsolute(nested))
        nested
    else
        try std.fs.path.join(arena, &.{ prev_cwd, nested });
    const abs_nested_src = if (std.fs.path.isAbsolute(nested_src))
        nested_src
    else
        try std.fs.path.join(arena, &.{ prev_cwd, nested_src });
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });

    const init_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PWD", .value = abs_meta },
    };
    const init_out = mustRunWithInDir(&suite, abs_meta, &.{
        "workspace", "init", "--meta-repo", "--scan", "2", "--no-scan", "--json", "--slug", "safety-ws",
    }, &init_env);
    defer gpa.free(init_out);

    const meta_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PWD", .value = abs_meta },
    };
    const read_set = mustRunWithInDir(&suite, abs_meta, &.{ "scope", "show", "--json" }, &meta_env);
    defer gpa.free(read_set);
    try std.testing.expect(std.mem.indexOf(u8, read_set, "\"kind\":\"association\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, read_set, "\"slug\":\"safety-ws\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, read_set, "\"slug\":\"meta-safety\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, read_set, "\"slug\":\"nested\"") != null);

    const empty_tasks = mustRunWithInDir(&suite, abs_meta, &.{ "task", "list", "--json" }, &meta_env);
    defer gpa.free(empty_tasks);
    try std.testing.expectEqualStrings("[]\n", empty_tasks);

    const ambiguous = suite.execWithInDir(abs_meta, &.{ "task", "add", "ambiguous root task" }, &meta_env);
    defer ambiguous.deinit(gpa);
    try std.testing.expect(ambiguous.term == .exited and ambiguous.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, ambiguous.stderr, "ambiguous meta workspace root") != null);
    try std.testing.expect(std.mem.indexOf(u8, ambiguous.stderr, "--scope assoc:safety-ws") != null);
    try std.testing.expect(std.mem.indexOf(u8, ambiguous.stderr, "--scope repo:meta-safety") != null);

    const TaskJSON = struct {
        id: i64,
        scope_kind: []const u8,
        scope_id: ?i64 = null,
    };
    const root_task_out = mustRunWithInDir(&suite, abs_meta, &.{ "task", "add", "--json", "--scope", "repo:meta-safety", "root repo task" }, &meta_env);
    defer gpa.free(root_task_out);
    const root_task = parseJSON(TaskJSON, arena, root_task_out);
    try std.testing.expectEqualStrings("repo", root_task.scope_kind);

    const org_task_out = mustRunWithInDir(&suite, abs_meta, &.{ "task", "add", "--json", "--scope", "assoc:safety-ws", "workspace task" }, &meta_env);
    defer gpa.free(org_task_out);
    const org_task = parseJSON(TaskJSON, arena, org_task_out);
    try std.testing.expectEqualStrings("association", org_task.scope_kind);

    const root_src_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PWD", .value = abs_root_src },
    };
    const root_only_out = mustRunWithInDir(&suite, abs_root_src, &.{ "task", "add", "--json", "root-only path task" }, &root_src_env);
    defer gpa.free(root_only_out);
    const root_only = parseJSON(TaskJSON, arena, root_only_out);
    try std.testing.expectEqualStrings("repo", root_only.scope_kind);
    try std.testing.expectEqual(root_task.scope_id.?, root_only.scope_id.?);

    const nested_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PWD", .value = abs_nested_src },
    };
    const nested_task_out = mustRunWithInDir(&suite, abs_nested_src, &.{ "task", "add", "--json", "nested repo task" }, &nested_env);
    defer gpa.free(nested_task_out);
    const nested_task = parseJSON(TaskJSON, arena, nested_task_out);
    try std.testing.expectEqualStrings("repo", nested_task.scope_kind);
    try std.testing.expect(nested_task.scope_id.? != root_task.scope_id.?);

    const nested_show = mustRunWithInDir(&suite, abs_nested_src, &.{ "scope", "show" }, &nested_env);
    defer gpa.free(nested_show);
    try std.testing.expect(std.mem.indexOf(u8, nested_show, "repo:nested") != null);
    try std.testing.expect(std.mem.indexOf(u8, nested_show, "repo:meta-safety") == null);

    const root_show = mustRunWithInDir(&suite, abs_root_src, &.{ "scope", "show" }, &root_src_env);
    defer gpa.free(root_show);
    try std.testing.expect(std.mem.indexOf(u8, root_show, "repo:meta-safety") != null);
    try std.testing.expect(std.mem.indexOf(u8, root_show, "repo:nested") == null);

    const nested_task_id = try std.fmt.allocPrint(arena, "{d}", .{nested_task.id});
    const mismatch = suite.execWithInDir(abs_root_src, &.{ "task", "update", "--next-action", "wrong repo", nested_task_id }, &root_src_env);
    defer mismatch.deinit(gpa);
    try std.testing.expect(mismatch.term == .exited and mismatch.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, mismatch.stderr, "scope mismatch") != null);
    try std.testing.expect(std.mem.indexOf(u8, mismatch.stderr, "repo:nested") != null);
    try std.testing.expect(std.mem.indexOf(u8, mismatch.stderr, "repo:meta-safety") != null);

    _ = abs_nested;
}

test "workspace init meta repo pipeline skips root instruction files and routes members" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const meta = try std.fs.path.join(arena, &.{ root, "meta-files" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    const nested = try std.fs.path.join(arena, &.{ meta, "modules", "nested" });
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ meta, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ nested, ".git" }));
    try writeFile(try std.fs.path.join(arena, &.{ meta, "README.md" }), "Meta root summary.\n\nBody.\n");
    try writeFile(try std.fs.path.join(arena, &.{ nested, "README.md" }), "Nested summary.\n");

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_meta = if (std.fs.path.isAbsolute(meta))
        meta
    else
        try std.fs.path.join(arena, &.{ prev_cwd, meta });
    const abs_nested = if (std.fs.path.isAbsolute(nested))
        nested
    else
        try std.fs.path.join(arena, &.{ prev_cwd, nested });
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    const root_agents = try std.fs.path.join(arena, &.{ abs_meta, "AGENTS.md" });
    const root_claude = try std.fs.path.join(arena, &.{ abs_meta, "CLAUDE.md" });

    try expectMissing(root_agents);
    try expectMissing(root_claude);

    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PWD", .value = abs_meta },
    };
    const init_out = mustRunWithInDir(&suite, abs_meta, &.{
        "workspace", "init", "--meta-repo", "--scan", "2", "--json", "--slug", "meta-files",
    }, &env);
    defer gpa.free(init_out);
    try std.testing.expect(std.mem.indexOf(u8, init_out, "\"strategy\":\"skipped-meta-repo\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, init_out, "\"installed\":[]") != null);
    try expectMissing(root_agents);
    try expectMissing(root_claude);

    const routing_out = mustRunWithInDir(&suite, abs_meta, &.{ "workspace", "routing", "show", "--json", "meta-files" }, &env);
    defer gpa.free(routing_out);
    try std.testing.expect(std.mem.indexOf(u8, routing_out, "\"slug\": \"meta-files\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, routing_out, "\"slug\": \"nested\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, routing_out, abs_meta) != null);
    try std.testing.expect(std.mem.indexOf(u8, routing_out, abs_nested) != null);

    const RegenJSON = struct { agents_path: []const u8, bytes_written: i64 };
    const regen_out = mustRunWithInDir(&suite, abs_meta, &.{ "workspace", "regenerate", "--json", "meta-files" }, &env);
    defer gpa.free(regen_out);
    const regen = parseJSON(RegenJSON, arena, regen_out);
    try std.testing.expect(regen.bytes_written > 0);
    try std.Io.Dir.cwd().access(std.Io.Threaded.global_single_threaded.io(), regen.agents_path, .{ .read = true });
    try expectMissing(root_agents);
    try expectMissing(root_claude);

    try writeFile(root_agents, "repo-owned instructions\n");
    const doctor_out = mustRunWithInDir(&suite, abs_meta, &.{ "workspace", "doctor", "--json" }, &env);
    defer gpa.free(doctor_out);
    try std.testing.expect(std.mem.indexOf(u8, doctor_out, "\"issues_found\":0") != null);
    const root_agents_body = try std.Io.Dir.cwd().readFileAlloc(
        std.Io.Threaded.global_single_threaded.io(),
        root_agents,
        gpa,
        std.Io.Limit.limited(1024),
    );
    defer gpa.free(root_agents_body);
    try std.testing.expectEqualStrings("repo-owned instructions\n", root_agents_body);
    try expectMissing(root_claude);
}

test "workspace doctor fail closed on malformed workspace config" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const ws = try std.fs.path.join(arena, &.{ root, "workspace" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ ws, "repo-a", ".git" }));

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_ws = if (std.fs.path.isAbsolute(ws))
        ws
    else
        try std.fs.path.join(arena, &.{ prev_cwd, ws });
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    const root_agents = try std.fs.path.join(arena, &.{ abs_ws, "AGENTS.md" });
    const root_claude = try std.fs.path.join(arena, &.{ abs_ws, "CLAUDE.md" });
    try std.process.setCurrentPath(std.testing.io, abs_ws);

    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PWD", .value = abs_ws },
    };
    const init_out = suite.mustRunWith(&.{
        "workspace", "init", "--scan", "1", "--no-scan", "--json", "--slug", "malformed-ws",
    }, &env);
    defer gpa.free(init_out);
    try expectMissing(root_agents);
    try expectMissing(root_claude);

    try sqliteExec(gpa, abs_db, "update associations set config_json = '{' where slug = 'malformed-ws';");

    const doctor_out = suite.mustRunWith(&.{ "workspace", "doctor", "--json" }, &env);
    defer gpa.free(doctor_out);
    try std.testing.expect(std.mem.indexOf(u8, doctor_out, "workspace config_json is malformed") != null);
    try std.testing.expect(std.mem.indexOf(u8, doctor_out, "\"kind\":\"error\"") != null);
    try expectMissing(root_agents);
    try expectMissing(root_claude);
}

test "workspace init meta repo layout failure commits registration and recovers without root files" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const meta = try std.fs.path.join(arena, &.{ root, "meta-recovery" });
    const home_file = try std.fs.path.join(arena, &.{ root, "planar-home-file" });
    const nested = try std.fs.path.join(arena, &.{ meta, "modules", "nested" });
    try mkdirp(try std.fs.path.join(arena, &.{ meta, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ nested, ".git" }));
    try writeFile(home_file, "not a directory\n");

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_meta = if (std.fs.path.isAbsolute(meta))
        meta
    else
        try std.fs.path.join(arena, &.{ prev_cwd, meta });
    const abs_nested = if (std.fs.path.isAbsolute(nested))
        nested
    else
        try std.fs.path.join(arena, &.{ prev_cwd, nested });
    const abs_home = if (std.fs.path.isAbsolute(home_file))
        home_file
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home_file });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    const root_agents = try std.fs.path.join(arena, &.{ abs_meta, "AGENTS.md" });
    const root_claude = try std.fs.path.join(arena, &.{ abs_meta, "CLAUDE.md" });

    const failing_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PWD", .value = abs_meta },
    };
    const failed_pipeline = suite.execWithInDir(abs_meta, &.{
        "workspace", "init", "--meta-repo", "--scan", "2", "--slug", "recover-ws",
    }, &failing_env);
    defer failed_pipeline.deinit(gpa);
    try std.testing.expect(failed_pipeline.term == .exited and failed_pipeline.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, failed_pipeline.stderr, "pipeline pass failed") != null);
    try std.testing.expect(std.mem.indexOf(u8, failed_pipeline.stderr, "planar workspace doctor") != null);
    try std.testing.expect(std.mem.indexOf(u8, failed_pipeline.stderr, "planar workspace routing build") != null);
    try expectMissing(root_agents);
    try expectMissing(root_claude);

    const members_out = suite.mustRunWith(&.{ "assoc", "members", "--json", "recover-ws" }, &failing_env);
    defer gpa.free(members_out);
    const MemberJSON = struct { slug: []const u8, root_path: ?[]const u8 = null };
    const members = parseJSON([]const MemberJSON, arena, members_out);
    try std.testing.expectEqual(@as(usize, 2), members.len);
    try expectMemberPath(members, "meta-recovery", abs_meta);
    try expectMemberPath(members, "nested", abs_nested);

    try deleteFile(abs_home);
    try mkdirp(abs_home);
    const recovery_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PWD", .value = abs_meta },
    };
    const doctor_out = mustRunWithInDir(&suite, abs_meta, &.{ "workspace", "doctor", "--json" }, &recovery_env);
    defer gpa.free(doctor_out);
    try std.testing.expect(std.mem.indexOf(u8, doctor_out, "\"slug\":\"recover-ws\"") != null);
    const routing_out = mustRunWithInDir(&suite, abs_meta, &.{ "workspace", "routing", "build", "--json", "recover-ws" }, &recovery_env);
    defer gpa.free(routing_out);
    try std.testing.expect(std.mem.indexOf(u8, routing_out, "\"projects\":2") != null);
    const regen_out = mustRunWithInDir(&suite, abs_meta, &.{ "workspace", "regenerate", "--json", "recover-ws" }, &recovery_env);
    defer gpa.free(regen_out);
    try std.testing.expect(std.mem.indexOf(u8, regen_out, "\"bytes_written\":") != null);
    try expectMissing(root_agents);
    try expectMissing(root_claude);
}

test "workspace init still refuses git root without meta repo flag" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const meta = try std.fs.path.join(arena, &.{ root, "meta-root" });
    const nested = try std.fs.path.join(arena, &.{ meta, "nested" });
    try mkdirp(try std.fs.path.join(arena, &.{ meta, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ nested, ".git" }));

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_meta = if (std.fs.path.isAbsolute(meta))
        meta
    else
        try std.fs.path.join(arena, &.{ prev_cwd, meta });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    try std.process.setCurrentPath(std.testing.io, abs_meta);

    const extra_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DB", .value = abs_db },
    };
    const res = suite.execWithInDir(abs_meta, &.{ "workspace", "init", "--scan", "2" }, &extra_env);
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited and res.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "is a git repository; use `planar init` for single repos") != null);
}

test "workspace doctor emits org report" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const ws = try std.fs.path.join(arena, &.{ root, "workspace" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    try mkdirp(ws);
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ ws, "repo-a", ".git" }));
    try writeFile(try std.fs.path.join(arena, &.{ ws, "repo-a", "README.md" }), "Repo A summary.\n");
    try writeFile(try std.fs.path.join(arena, &.{ ws, "repo-a", "go.mod" }), "module example.com/repo-a\n\ngo 1.22\n");

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    try std.process.setCurrentPath(std.testing.io, ws);

    const extra_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
    };
    const init_out = suite.mustRunWith(&.{ "workspace", "init", "--slug", "doc-org" }, &extra_env);
    gpa.free(init_out);

    const doctor_out = suite.mustRunWith(&.{ "workspace", "doctor", "--json" }, &extra_env);
    defer gpa.free(doctor_out);
    const DoctorJSON = struct {
        orgs: []const struct {
            slug: []const u8,
            org_id: i64,
            issues_found: i64,
        },
    };
    const doctor = parseJSON(DoctorJSON, arena, doctor_out);
    try std.testing.expect(doctor.orgs.len >= 1);
}

test "workspace routing build applies overrides from state dir" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const ws = try std.fs.path.join(arena, &.{ root, "workspace" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    try mkdirp(ws);
    try mkdirp(home);

    const repo_a = try std.fs.path.join(arena, &.{ ws, "repo-a" });
    const repo_b = try std.fs.path.join(arena, &.{ ws, "repo-b" });
    try mkdirp(try std.fs.path.join(arena, &.{ repo_a, ".git" }));
    try mkdirp(try std.fs.path.join(arena, &.{ repo_b, ".git" }));
    try writeFile(try std.fs.path.join(arena, &.{ repo_a, "README.md" }), "Repo A summary.\n");
    try writeFile(try std.fs.path.join(arena, &.{ repo_b, "README.md" }), "Repo B summary.\n");

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    try std.process.setCurrentPath(std.testing.io, ws);

    const extra_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
    };
    const init_out = suite.mustRunWith(&.{ "workspace", "init", "--json", "--slug", "ov-org" }, &extra_env);
    defer gpa.free(init_out);
    const InitJSON = struct { org: struct { id: i64 } };
    const init = parseJSON(InitJSON, arena, init_out);

    const state_dir = try std.fs.path.join(arena, &.{ abs_home, "workspaces", try std.fmt.allocPrint(arena, "{d}", .{init.org.id}) });
    const overrides = try std.fs.path.join(arena, &.{ state_dir, "routing-table-overrides.json" });
    try writeFile(overrides,
        \\{
        \\  "schema_version": 1,
        \\  "projects": {
        \\    "repo-a": {
        \\      "summary": "manual summary",
        \\      "capabilities": ["forced-tag"],
        \\      "depends_on": ["repo-b"]
        \\    }
        \\  }
        \\}
    );

    const build_out = suite.mustRunWith(&.{ "workspace", "routing", "build", "--json", "ov-org" }, &extra_env);
    defer gpa.free(build_out);
    const BuildJSON = struct { path: []const u8, projects: i64 };
    const build = parseJSON(BuildJSON, arena, build_out);
    try std.testing.expect(build.projects >= 2);

    const show_out = suite.mustRunWith(&.{ "workspace", "routing", "show", "--json", "ov-org" }, &extra_env);
    defer gpa.free(show_out);
    const ShowJSON = struct {
        projects: []const struct {
            slug: []const u8,
            summary: []const u8,
            capabilities: []const []const u8,
            depends_on: []const []const u8,
        },
    };
    const show = parseJSON(ShowJSON, arena, show_out);
    var found = false;
    for (show.projects) |project| {
        if (!std.mem.eql(u8, project.slug, "repo-a")) continue;
        found = true;
        try std.testing.expectEqualStrings("manual summary", project.summary);
        try std.testing.expect(project.capabilities.len == 1 and std.mem.eql(u8, project.capabilities[0], "forced-tag"));
        try std.testing.expect(project.depends_on.len == 1 and std.mem.eql(u8, project.depends_on[0], "repo-b"));
    }
    try std.testing.expect(found);
}

test "workspace regenerate consults operator template path" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const ws = try std.fs.path.join(arena, &.{ root, "workspace" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    try mkdirp(ws);
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ ws, "repo-a", ".git" }));
    try writeFile(try std.fs.path.join(arena, &.{ ws, "repo-a", "README.md" }), "Repo A summary.\n");

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });
    const tpl_dir = try std.fs.path.join(arena, &.{ abs_home, "templates", "doc-prompts" });
    try mkdirp(tpl_dir);
    const template_path = try std.fs.path.join(arena, &.{ tpl_dir, "agents.md" });
    try writeFile(template_path,
        \\workspace={{.WorkspaceSlug}}
        \\generated={{.GeneratedAt}}
        \\{{range .Projects}}project={{.Slug}} summary={{.Summary}} caps={{commaJoin .Capabilities}}
        \\{{end}}{{if .ActivePlans}}has-plans{{else}}no-plans{{end}}
    );
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    try std.process.setCurrentPath(std.testing.io, ws);

    const extra_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
    };
    const init_out = suite.mustRunWith(&.{ "workspace", "init", "--json", "--slug", "tpl-org" }, &extra_env);
    defer gpa.free(init_out);

    const regen_out = suite.mustRunWith(&.{ "workspace", "regenerate", "--json", "tpl-org" }, &extra_env);
    defer gpa.free(regen_out);
    const RegenJSON = struct { agents_path: []const u8 };
    const regen = parseJSON(RegenJSON, arena, regen_out);
    const body = try std.Io.Dir.cwd().readFileAlloc(
        std.Io.Threaded.global_single_threaded.io(),
        regen.agents_path,
        gpa,
        std.Io.Limit.limited(1024 * 1024),
    );
    defer gpa.free(body);
    try std.testing.expect(std.mem.indexOf(u8, body, "workspace=tpl-org") != null);
    try std.testing.expect(std.mem.indexOf(u8, body, "project=repo-a summary=Repo A summary.") != null);
    try std.testing.expect(std.mem.indexOf(u8, body, "no-plans") != null);

    // Missing template should fall back to embedded default output.
    std.Io.Dir.cwd().deleteFile(std.Io.Threaded.global_single_threaded.io(), template_path) catch |err| switch (err) {
        error.FileNotFound => {},
        else => return err,
    };
    const regen2_out = suite.mustRunWith(&.{ "workspace", "regenerate", "--json", "tpl-org" }, &extra_env);
    defer gpa.free(regen2_out);
    const regen2 = parseJSON(RegenJSON, arena, regen2_out);
    const body2 = try std.Io.Dir.cwd().readFileAlloc(
        std.Io.Threaded.global_single_threaded.io(),
        regen2.agents_path,
        gpa,
        std.Io.Limit.limited(1024 * 1024),
    );
    defer gpa.free(body2);
    try std.testing.expect(std.mem.indexOf(u8, body2, "Workspace AGENTS Guide") != null);
}

test "workspace routing build --enrich reports consistently in JSON" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try makeAbsTmp(arena, tmp);
    const ws = try std.fs.path.join(arena, &.{ root, "workspace" });
    const home = try std.fs.path.join(arena, &.{ root, "planar-home" });
    try mkdirp(ws);
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ ws, "repo-a", ".git" }));
    try writeFile(try std.fs.path.join(arena, &.{ ws, "repo-a", "README.md" }), "Repo A summary.\n");

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", arena);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(arena, &.{ prev_cwd, suite.db_path });
    try std.process.setCurrentPath(std.testing.io, ws);

    const extra_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = home },
        .{ .key = "PLANAR_DB", .value = abs_db },
    };
    const init_out = suite.mustRunWith(&.{ "workspace", "init", "--json", "--slug", "enrich-org", "--enrich" }, &extra_env);
    defer gpa.free(init_out);
    const InitJSON = struct {
        pipeline: struct {
            routing: struct {
                enrich_enabled: bool,
            },
        },
    };
    const init = parseJSON(InitJSON, arena, init_out);
    try std.testing.expect(!init.pipeline.routing.enrich_enabled);

    const build_out = suite.mustRunWith(&.{ "workspace", "routing", "build", "--json", "--enrich", "enrich-org" }, &extra_env);
    defer gpa.free(build_out);
    const BuildJSON = struct {
        enrich_enabled: bool,
        enrich_misses: i64,
    };
    const build = parseJSON(BuildJSON, arena, build_out);
    try std.testing.expect(!build.enrich_enabled);
    try std.testing.expectEqual(@as(i64, 0), build.enrich_misses);
}
