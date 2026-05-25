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
