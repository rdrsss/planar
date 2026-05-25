//! integration_tests/scope_test.zig
//!
//! Black-box parity checks for `planar scope`.

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

test "scope removed verbs tolerate legacy args/flags and print plan 153 redirect" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const use_stderr = suite.expectFailure(&.{
        "scope",            "use",
        "--legacy",         "foo",
        "--scope",          "assoc:old",
        "extra-positional",
    });
    defer gpa.free(use_stderr);
    try std.testing.expect(std.mem.containsAtLeast(u8, use_stderr, 1, "removed in plan 153 M5"));
    try std.testing.expect(std.mem.containsAtLeast(u8, use_stderr, 1, "--scope"));

    const pop_stderr = suite.expectFailure(&.{
        "scope",         "pop",
        "--random-flag", "value",
        "tail",
    });
    defer gpa.free(pop_stderr);
    try std.testing.expect(std.mem.containsAtLeast(u8, pop_stderr, 1, "removed in plan 153 M5"));

    const clear_stderr = suite.expectFailure(&.{
        "scope",                "clear",
        "--this-used-to-exist", "legacy",
        "args",
    });
    defer gpa.free(clear_stderr);
    try std.testing.expect(std.mem.containsAtLeast(u8, clear_stderr, 1, "removed in plan 153 M5"));
}

test "scope show json uses canonical keys for cwd-derived project scope" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const init_out = suite.mustRun(&.{"init"});
    defer gpa.free(init_out);
    const create_out = suite.mustRun(&.{ "assoc", "create", "json-target", "--kind", "project" });
    defer gpa.free(create_out);

    const raw = suite.mustRun(&.{ "--json", "scope", "show", "--scope", "assoc:json-target" });
    defer gpa.free(raw);

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const parsed = std.json.parseFromSlice(std.json.Value, arena, raw, .{}) catch |e| {
        std.debug.print("scope show json parse error: {s}\nraw: {s}\n", .{ @errorName(e), raw });
        try std.testing.expect(false);
        unreachable;
    };
    const obj = parsed.value.object;
    try std.testing.expect(obj.get("resolved_scopes") != null);
    try std.testing.expect(obj.get("source") != null);
    try std.testing.expect(obj.get("cwd") != null);
    try std.testing.expect(obj.get("reason") == null);
    try std.testing.expect(obj.get("project_slug") == null);

    const scopes = obj.get("resolved_scopes").?.array.items;
    try std.testing.expect(scopes.len >= 1);
    const first = scopes[0].object;
    try std.testing.expect(first.get("kind") != null);
    try std.testing.expect(first.get("id") != null);
    try std.testing.expect(first.get("slug") != null);
    try std.testing.expect(first.get("name") != null);
    try std.testing.expect(first.get("kind_label") != null);
}

test "scope show json at workspace root returns org plus member scopes" {
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
    try writeFile(try std.fs.path.join(arena, &.{ repo_a, "README.md" }), "Repo A.\n");
    try writeFile(try std.fs.path.join(arena, &.{ repo_b, "README.md" }), "Repo B.\n");

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", gpa);
    defer gpa.free(prev_cwd);
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(gpa, &.{ prev_cwd, suite.db_path });
    defer if (!std.fs.path.isAbsolute(suite.db_path)) gpa.free(abs_db);
    const abs_home = if (std.fs.path.isAbsolute(home))
        home
    else
        try std.fs.path.join(arena, &.{ prev_cwd, home });

    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = abs_db },
    };

    const init_res = suite.execWithInDir(ws, &.{
        "workspace", "init", "--json", "--slug", "ws-org",
    }, &env);
    defer init_res.deinit(gpa);
    try std.testing.expect(init_res.term == .exited and init_res.term.exited == 0);

    const show_res = suite.execWithInDir(ws, &.{
        "scope", "show", "--json",
    }, &env);
    defer show_res.deinit(gpa);
    try std.testing.expect(show_res.term == .exited and show_res.term.exited == 0);

    const ScopeShowJSON = struct {
        resolved_scopes: []const struct {
            kind: []const u8,
            slug: []const u8 = "",
            kind_label: []const u8 = "",
        },
        source: []const u8,
        cwd: []const u8,
    };
    const parsed = parseJSON(ScopeShowJSON, arena, show_res.stdout);
    try std.testing.expectEqualStrings("cwd", parsed.source);
    try std.testing.expect(parsed.cwd.len > 0);
    try std.testing.expect(parsed.resolved_scopes.len >= 3);

    var saw_org = false;
    var saw_repo_a = false;
    var saw_repo_b = false;
    for (parsed.resolved_scopes) |row| {
        if (std.mem.eql(u8, row.slug, "ws-org")) saw_org = true;
        if (std.mem.eql(u8, row.slug, "repo-a")) saw_repo_a = true;
        if (std.mem.eql(u8, row.slug, "repo-b")) saw_repo_b = true;
    }
    try std.testing.expect(saw_org);
    try std.testing.expect(saw_repo_a);
    try std.testing.expect(saw_repo_b);
}

test "scope suggest uses exact project-root match (no parent walk from subdir)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var tmp_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp_len = suite.tmp_dir.dir.realPath(std.testing.io, &tmp_buf) catch @panic("cannot resolve tmp dir");
    const tmp_abs = tmp_buf[0..tmp_len];
    const project_root = std.fs.path.join(gpa, &.{ tmp_abs, "member-subdir" }) catch @panic("OOM");
    defer gpa.free(project_root);
    const deep = std.fs.path.join(gpa, &.{ project_root, "src", "nested" }) catch @panic("OOM");
    defer gpa.free(deep);

    std.Io.Dir.cwd().createDirPath(std.testing.io, deep) catch |e| switch (e) {
        error.PathAlreadyExists => {},
        else => return e,
    };

    const create_out = suite.mustRun(&.{ "assoc", "create", "member-subdir", "--kind", "project" });
    defer gpa.free(create_out);
    const add_res = suite.execWith(&.{ "assoc", "add", "member-subdir", project_root }, &.{});
    defer add_res.deinit(gpa);
    try std.testing.expect(add_res.term == .exited and add_res.term.exited == 0);

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", gpa);
    defer gpa.free(prev_cwd);
    const abs_db = if (std.fs.path.isAbsolute(suite.db_path))
        suite.db_path
    else
        try std.fs.path.join(gpa, &.{ prev_cwd, suite.db_path });
    defer if (!std.fs.path.isAbsolute(suite.db_path)) gpa.free(abs_db);
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DB", .value = abs_db },
    };

    const suggest_root = suite.execWithInDir(project_root, &.{ "scope", "suggest" }, &env);
    defer suggest_root.deinit(gpa);
    try std.testing.expect(suggest_root.term == .exited and suggest_root.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, suggest_root.stdout, 1, "member-subdir"));

    const suggest_subdir = suite.execWithInDir(deep, &.{ "scope", "suggest" }, &env);
    defer suggest_subdir.deinit(gpa);
    try std.testing.expect(suggest_subdir.term == .exited and suggest_subdir.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, suggest_subdir.stdout, 1, "no scope suggestions for cwd"));
}
