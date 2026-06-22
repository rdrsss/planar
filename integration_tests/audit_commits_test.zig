//! integration_tests/audit_commits_test.zig — `planar audit commits` contract.

const std = @import("std");
const harness = @import("harness");

test "audit commits supports default, json, shas, task filtering, empty sessions, and unknown ids" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = suite.registerProject("audit-commits");
    const repo_root = try makeFixtureRepo(gpa, &suite, "audit-commits-repo");
    defer gpa.free(repo_root);

    const task_out = suite.mustRunInDir(root, &.{ "task", "add", "audit commits fixture", "--json" });
    defer gpa.free(task_out);
    const task_id = extractIntField(task_out, "\"id\"") orelse @panic("no task id");
    const task_id_arg = try std.fmt.allocPrint(gpa, "{d}", .{task_id});
    defer gpa.free(task_id_arg);

    const session_out = suite.mustRunInDir(repo_root, &.{ "capture", "session", "--task", task_id_arg, "--json" });
    defer gpa.free(session_out);
    const session_id = extractIntField(session_out, "\"id\"") orelse @panic("no session id");
    const session_id_arg = try std.fmt.allocPrint(gpa, "{d}", .{session_id});
    defer gpa.free(session_id_arg);

    const empty_json = suite.mustRun(&.{ "audit", "commits", "--session", session_id_arg, "--json" });
    defer gpa.free(empty_json);
    try std.testing.expectEqualStrings("[]\n", empty_json);

    const empty_shas = suite.mustRun(&.{ "audit", "commits", "--session", session_id_arg, "--shas" });
    defer gpa.free(empty_shas);
    try std.testing.expectEqualStrings("", empty_shas);

    const task_ref = try std.fmt.allocPrint(gpa, "task:{d}", .{task_id});
    defer gpa.free(task_ref);
    const claim_json = mustRunAgentInDir(&suite, repo_root, &.{ "claim", "--entity", task_ref, "--json" });
    defer gpa.free(claim_json);
    const claim_token = try gpa.dupe(u8, extractStringField(claim_json, "\"claim_token\"") orelse @panic("no claim token"));
    defer gpa.free(claim_token);
    const claim_session_id = extractIntField(claim_json, "\"session_id\"") orelse @panic("no claim session id");
    const claim_session_id_arg = try std.fmt.allocPrint(gpa, "{d}", .{claim_session_id});
    defer gpa.free(claim_session_id_arg);

    // `claim --entity` does not flip task status (unlike `pull`). Advance to doing
    // so `complete` (doing → done) is legal under the real status matrix.
    // Pass --force because the claim is active; the claim guard blocks operator
    // status flips on claimed tasks (decision 533 / task 4165). We own the claim
    // in this test so --force is appropriate here.
    const start_out = suite.mustRunInDir(root, &.{ "task", "update", task_id_arg, "--status", "doing", "--force" });
    defer gpa.free(start_out);

    const sha_a = try createCommit(repo_root, "audit-a.txt", "a\n", "audit alpha");
    defer gpa.free(sha_a);
    const sha_b = try createCommit(repo_root, "audit-b.txt", "b\n", "audit beta");
    defer gpa.free(sha_b);

    const complete_out = mustRunAgentInDir(&suite, repo_root, &.{ "complete", "--claim", claim_token, "--summary", "done", "--json" });
    defer gpa.free(complete_out);

    const human = suite.mustRun(&.{ "audit", "commits", "--session", claim_session_id_arg });
    defer gpa.free(human);
    try std.testing.expect(std.mem.indexOf(u8, human, "SHA") != null);
    try std.testing.expect(std.mem.indexOf(u8, human, sha_a) != null);
    try std.testing.expect(std.mem.indexOf(u8, human, sha_b) != null);
    try std.testing.expect(std.mem.indexOf(u8, human, "audit alpha") != null);
    try std.testing.expect(std.mem.indexOf(u8, human, "audit beta") != null);

    const json = suite.mustRun(&.{ "audit", "commits", "--session", claim_session_id_arg, "--json" });
    defer gpa.free(json);
    try std.testing.expect(std.mem.indexOf(u8, json, "\"session_id\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, json, sha_a) != null);
    try std.testing.expect(std.mem.indexOf(u8, json, sha_b) != null);

    const shas = suite.mustRun(&.{ "audit", "commits", "--session", claim_session_id_arg, "--shas" });
    defer gpa.free(shas);
    var lines = std.mem.splitScalar(u8, std.mem.trim(u8, shas, "\n"), '\n');
    const line_a = lines.next() orelse @panic("missing first sha line");
    const line_b = lines.next() orelse @panic("missing second sha line");
    try std.testing.expect(lines.next() == null);
    try std.testing.expect(line_a.len == 40);
    try std.testing.expect(line_b.len == 40);
    try std.testing.expect(
        (std.mem.eql(u8, line_a, sha_a) and std.mem.eql(u8, line_b, sha_b)) or
            (std.mem.eql(u8, line_a, sha_b) and std.mem.eql(u8, line_b, sha_a)),
    );

    const task_filtered = suite.mustRun(&.{ "audit", "commits", "--task", task_id_arg, "--json" });
    defer gpa.free(task_filtered);
    try std.testing.expect(std.mem.indexOf(u8, task_filtered, sha_a) != null);
    try std.testing.expect(std.mem.indexOf(u8, task_filtered, sha_b) != null);

    const bad_session = suite.expectFailure(&.{ "audit", "commits", "--session", "999999" });
    defer gpa.free(bad_session);
    try std.testing.expect(std.mem.indexOf(u8, bad_session, "session 999999 not found") != null);

    const bad_session_json = suite.expectFailure(&.{ "audit", "commits", "--session", "999999", "--json" });
    defer gpa.free(bad_session_json);
    try std.testing.expect(std.mem.indexOf(u8, bad_session_json, "session 999999 not found") != null);

    const bad_task_human = suite.expectFailure(&.{ "audit", "commits", "--task", "999999" });
    defer gpa.free(bad_task_human);
    try std.testing.expect(std.mem.indexOf(u8, bad_task_human, "task 999999 not found") != null);

    const bad_task = suite.expectFailure(&.{ "audit", "commits", "--task", "999999", "--json" });
    defer gpa.free(bad_task);
    try std.testing.expect(std.mem.indexOf(u8, bad_task, "task 999999 not found") != null);
}

fn makeFixtureRepo(
    gpa: std.mem.Allocator,
    suite: *harness.Suite,
    name: []const u8,
) ![]u8 {
    const repo_root = try std.fs.path.join(gpa, &.{ suite.tmpAbsPath(), name });
    errdefer gpa.free(repo_root);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, repo_root);

    try runCommandDiscard(&.{ "git", "init", repo_root });
    try runCommandInDirDiscard(repo_root, &.{ "git", "config", "user.email", "planar-test@example.com" });
    try runCommandInDirDiscard(repo_root, &.{ "git", "config", "user.name", "Planar Test" });

    try writeRepoFile(repo_root, "README.md", "seed\n");
    try runCommandInDirDiscard(repo_root, &.{ "git", "add", "README.md" });
    try runCommandInDirDiscard(repo_root, &.{ "git", "commit", "-m", "seed" });
    return repo_root;
}

fn runCommand(argv: []const []const u8) ![]u8 {
    const gpa = std.testing.allocator;
    const result = try std.process.run(gpa, std.testing.io, .{ .argv = argv });
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("command failed: {s}\nstderr: {s}\n", .{ argv[0], result.stderr });
        gpa.free(result.stdout);
        return error.CommandFailed;
    }
    return result.stdout;
}

fn runCommandDiscard(argv: []const []const u8) !void {
    const stdout = try runCommand(argv);
    std.testing.allocator.free(stdout);
}

fn runCommandInDir(cwd: []const u8, argv: []const []const u8) ![]u8 {
    const gpa = std.testing.allocator;
    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv,
        .cwd = .{ .path = cwd },
    });
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("command failed in '{s}': {s}\nstderr: {s}\n", .{ cwd, argv[0], result.stderr });
        gpa.free(result.stdout);
        return error.CommandFailed;
    }
    return result.stdout;
}

fn runCommandInDirDiscard(cwd: []const u8, argv: []const []const u8) !void {
    const stdout = try runCommandInDir(cwd, argv);
    std.testing.allocator.free(stdout);
}

fn writeRepoFile(repo_root: []const u8, rel_path: []const u8, contents: []const u8) !void {
    const gpa = std.testing.allocator;
    const path = try std.fs.path.join(gpa, &.{ repo_root, rel_path });
    defer gpa.free(path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = contents });
}

fn createCommit(
    repo_root: []const u8,
    rel_path: []const u8,
    contents: []const u8,
    subject: []const u8,
) ![]u8 {
    try writeRepoFile(repo_root, rel_path, contents);
    try runCommandInDirDiscard(repo_root, &.{ "git", "add", rel_path });
    try runCommandInDirDiscard(repo_root, &.{ "git", "commit", "-m", subject });
    const raw = try runCommandInDir(repo_root, &.{ "git", "rev-parse", "HEAD" });
    defer std.testing.allocator.free(raw);
    return try std.testing.allocator.dupe(u8, std.mem.trim(u8, raw, " \t\r\n"));
}

fn extractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

fn extractStringField(json: []const u8, key: []const u8) ?[]const u8 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    if (i >= json.len or json[i] != '"') return null;
    i += 1;
    const start = i;
    while (i < json.len and json[i] != '"') i += 1;
    if (i >= json.len) return null;
    return json[start..i];
}

fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_AGENT_BIN=")) {
            return s["PLANAR_AGENT_BIN=".len..];
        }
    }
    @panic("PLANAR_AGENT_BIN is not set. Run integration tests via: make test-integration");
}

fn mustRunAgentInDir(
    suite: *harness.Suite,
    cwd: []const u8,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const agent_bin = resolveAgentBin();

    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, agent_bin) catch @panic("OOM");
    for (args) |arg| argv_list.append(gpa, arg) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.absDbPath()) catch @panic("OOM injecting PLANAR_DB");
    env_map.put("PWD", cwd) catch @panic("OOM injecting PWD");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
        .cwd = .{ .path = cwd },
    }) catch |e| std.debug.panic("runAgentInDir spawn failed: {s}", .{@errorName(e)});
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print(
            "planar-agent failed in cwd '{s}' (term={any}): {s}\nstderr: {s}\n",
            .{ cwd, result.term, result.stdout, result.stderr },
        );
        gpa.free(result.stdout);
        @panic("planar-agent must-run-in-dir failed");
    }
    return result.stdout;
}
