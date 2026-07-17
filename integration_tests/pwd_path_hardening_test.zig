//! Regression coverage for plan 351 task 2378.
//!
//! A shell preserves a symlink-spelled working directory in PWD, while
//! realPath resolves it to the target. Operator-facing paths must retain the
//! PWD spelling so project registrations, scope lookups, and staged LLM
//! requests all use the same key.

const std = @import("std");
const harness = @import("harness");

const StagedJSON = struct {
    repo_slug: []const u8 = "",
    pending_path: []const u8 = "",
};

const RequestJSON = struct {
    repo_root: []const u8 = "",
};

fn makeSymlinkRepo(suite: *harness.Suite, allocator: std.mem.Allocator) !struct { real: []u8, alias: []u8 } {
    const base = suite.freshSystemTmpDir();
    const real = try std.fs.path.join(allocator, &.{ base, "real-repo" });
    errdefer allocator.free(real);
    const alias = try std.fs.path.join(allocator, &.{ base, "alias-repo" });
    errdefer allocator.free(alias);

    try std.Io.Dir.cwd().createDirPath(std.testing.io, real);
    const docs = try std.fs.path.join(allocator, &.{ real, "docs" });
    defer allocator.free(docs);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, docs);
    const readme = try std.fs.path.join(allocator, &.{ real, "README.md" });
    defer allocator.free(readme);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = readme, .data = "# PWD fixture\n" });
    const tech = try std.fs.path.join(allocator, &.{ docs, "tech-spec.md" });
    defer allocator.free(tech);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = tech, .data = "# Tech spec\n" });
    try std.Io.Dir.cwd().symLink(std.testing.io, real, alias, .{ .is_directory = true });
    return .{ .real = real, .alias = alias };
}

fn parseJSON(comptime T: type, allocator: std.mem.Allocator, raw: []const u8) !T {
    const parsed = try std.json.parseFromSlice(T, allocator, std.mem.trim(u8, raw, " \t\r\n"), .{
        .ignore_unknown_fields = true,
    });
    return parsed.value;
}

fn assertStagedRoot(suite: *harness.Suite, arena: std.mem.Allocator, alias: []const u8, verb: []const u8) !void {
    const out = if (std.mem.eql(u8, verb, "import"))
        suite.mustRunInDir(alias, &.{ verb, ".", "--interpret", "--json" })
    else
        suite.mustRunInDir(alias, &.{ verb, ".", "--json" });
    defer suite.allocator.free(out);
    const staged = try parseJSON(StagedJSON, arena, out);
    try std.testing.expectEqualStrings("alias-repo", staged.repo_slug);

    const request_path = if (std.fs.path.isAbsolute(staged.pending_path))
        staged.pending_path
    else
        try std.fs.path.resolve(arena, &.{ alias, staged.pending_path });
    const request_raw = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, request_path, arena, .limited(16 * 1024 * 1024));
    const request = try parseJSON(RequestJSON, arena, request_raw);
    try std.testing.expectEqualStrings(alias, request.repo_root);
}

test "task 2378: import and synthesize preserve PWD spelling for relative repo roots" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = try makeSymlinkRepo(&suite, gpa);
    defer gpa.free(repo.real);
    defer gpa.free(repo.alias);

    try assertStagedRoot(&suite, arena, repo.alias, "import");
    try assertStagedRoot(&suite, arena, repo.alias, "synthesize");
}

test "task 2378: association detect and scope suggest preserve PWD spelling" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const repo = try makeSymlinkRepo(&suite, gpa);
    defer gpa.free(repo.real);
    defer gpa.free(repo.alias);

    const initialized = suite.mustRunInDir(repo.alias, &.{ "init", "--allow-no-repo", "--name", "pwd-hardening" });
    gpa.free(initialized);
    const created = suite.mustRun(&.{ "assoc", "create", "pwd-hardening", "--kind", "project" });
    gpa.free(created);
    const added = suite.mustRun(&.{ "assoc", "add", "pwd-hardening", repo.alias });
    gpa.free(added);

    const suggestions = suite.mustRunInDir(repo.alias, &.{ "scope", "suggest", "--json" });
    defer gpa.free(suggestions);
    try std.testing.expect(std.mem.containsAtLeast(u8, suggestions, 1, "pwd-hardening"));

    const detected = suite.mustRunInDir(repo.alias, &.{ "assoc", "detect", "--apply", "--json" });
    gpa.free(detected);
}
