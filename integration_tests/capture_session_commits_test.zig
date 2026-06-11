//! integration_tests/capture_session_commits_test.zig — capture-end commit attribution.

const std = @import("std");
const harness = @import("harness");

test "capture end records session commits for operator sessions" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const repo_root = try std.fs.path.join(gpa, &.{ suite.tmpAbsPath(), "capture-session-commits-repo" });
    defer gpa.free(repo_root);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, repo_root);

    try runCommandDiscard(&.{ "git", "init", repo_root });
    try runCommandInDirDiscard(repo_root, &.{ "git", "config", "user.email", "planar-test@example.com" });
    try runCommandInDirDiscard(repo_root, &.{ "git", "config", "user.name", "Planar Test" });

    try writeRepoFile(repo_root, "README.md", "seed\n");
    try runCommandInDirDiscard(repo_root, &.{ "git", "add", "README.md" });
    try runCommandInDirDiscard(repo_root, &.{ "git", "commit", "-m", "seed" });

    const branch = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "branch", "--show-current" }));
    defer gpa.free(branch);
    const canonical_repo_root = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "rev-parse", "--show-toplevel" }));
    defer gpa.free(canonical_repo_root);

    const open_out = suite.mustRunInDir(repo_root, &.{ "capture", "session", "--json" });
    defer gpa.free(open_out);
    const session_id = extractIntField(open_out, "\"id\"") orelse @panic("no session id");

    const session_sql = try std.fmt.allocPrint(gpa,
        \\select repo_root, head_sha_at_start
        \\from sessions
        \\where id = {d};
    , .{session_id});
    defer gpa.free(session_sql);
    const session_row = try sqliteQueryLines(gpa, suite.db_path, session_sql);
    defer gpa.free(session_row);
    try std.testing.expect(std.mem.indexOf(u8, session_row, canonical_repo_root) != null);

    const commit_sha = try createCommit(repo_root, "tracked.txt", "tracked\n", "tracked commit");
    defer gpa.free(commit_sha);

    const session_id_arg = try std.fmt.allocPrint(gpa, "{d}", .{session_id});
    defer gpa.free(session_id_arg);
    const end_out = suite.mustRunInDir(repo_root, &.{ "capture", "end", session_id_arg, "--json" });
    defer gpa.free(end_out);
    try std.testing.expect(std.mem.indexOf(u8, end_out, "\"ok\":true") != null);

    try assertCommitRows(
        gpa,
        suite.db_path,
        session_id,
        canonical_repo_root,
        branch,
        &.{.{ .sha = commit_sha, .subject = "tracked commit" }},
    );
}

test "capture end in non-git cwd degrades to zero commit rows" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const nongit_root = suite.freshSystemTmpDir();

    const open_out = suite.mustRunInDir(nongit_root, &.{ "capture", "session", "--json" });
    defer gpa.free(open_out);
    const session_id = extractIntField(open_out, "\"id\"") orelse @panic("no session id");

    const session_id_arg = try std.fmt.allocPrint(gpa, "{d}", .{session_id});
    defer gpa.free(session_id_arg);
    const end_out = suite.mustRunInDir(nongit_root, &.{ "capture", "end", session_id_arg, "--json" });
    defer gpa.free(end_out);
    try std.testing.expect(std.mem.indexOf(u8, end_out, "\"ok\":true") != null);

    const session_sql = try std.fmt.allocPrint(gpa,
        \\select count(*)
        \\from sessions
        \\where id = {d}
        \\  and repo_root is null
        \\  and head_sha_at_start is null;
    , .{session_id});
    defer gpa.free(session_sql);
    try std.testing.expectEqual(@as(i64, 1), try sqliteScalar(gpa, suite.db_path, session_sql));

    const count_sql = try std.fmt.allocPrint(gpa,
        \\select count(*)
        \\from session_commits
        \\where session_id = {d};
    , .{session_id});
    defer gpa.free(count_sql);
    try std.testing.expectEqual(@as(i64, 0), try sqliteScalar(gpa, suite.db_path, count_sql));
}

test "capture commits records a range into the active session" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const repo_root = try makeFixtureRepo(gpa, &suite, "capture-commits-range-repo");
    defer gpa.free(repo_root);

    const open_out = suite.mustRunInDir(repo_root, &.{ "capture", "session", "--json" });
    defer gpa.free(open_out);
    const session_id = extractIntField(open_out, "\"id\"") orelse @panic("no session id");

    const base = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "rev-parse", "HEAD" }));
    defer gpa.free(base);
    const sha_a = try createCommit(repo_root, "a.txt", "a\n", "alpha commit");
    defer gpa.free(sha_a);
    const sha_b = try createCommit(repo_root, "b.txt", "b\n", "beta commit");
    defer gpa.free(sha_b);

    const out = suite.mustRunInDir(repo_root, &.{ "capture", "commits", "--since", base, "--json" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"commit_count\":2") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"inserted_count\":2") != null);
    const canonical_repo_root = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "rev-parse", "--show-toplevel" }));
    defer gpa.free(canonical_repo_root);
    const branch = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "branch", "--show-current" }));
    defer gpa.free(branch);

    try assertCommitRows(
        gpa,
        suite.db_path,
        session_id,
        canonical_repo_root,
        branch,
        &.{
            .{ .sha = sha_a, .subject = "alpha commit" },
            .{ .sha = sha_b, .subject = "beta commit" },
        },
    );
}

test "capture commits records explicit SHAs into an ended session" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const repo_root = try makeFixtureRepo(gpa, &suite, "capture-commits-ended-repo");
    defer gpa.free(repo_root);
    const outside = suite.freshSystemTmpDir();

    const open_out = suite.mustRunInDir(repo_root, &.{ "capture", "session", "--json" });
    defer gpa.free(open_out);
    const session_id = extractIntField(open_out, "\"id\"") orelse @panic("no session id");
    const session_id_arg = try std.fmt.allocPrint(gpa, "{d}", .{session_id});
    defer gpa.free(session_id_arg);

    const end_out = suite.mustRunInDir(repo_root, &.{ "capture", "end", session_id_arg, "--json" });
    defer gpa.free(end_out);

    const sha_a = try createCommit(repo_root, "after-a.txt", "a\n", "after session a");
    defer gpa.free(sha_a);
    const sha_b = try createCommit(repo_root, "after-b.txt", "b\n", "after session b");
    defer gpa.free(sha_b);

    const canonical_repo_root = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "rev-parse", "--show-toplevel" }));
    defer gpa.free(canonical_repo_root);
    const branch = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "branch", "--show-current" }));
    defer gpa.free(branch);

    const out = suite.mustRunInDir(outside, &.{ "capture", "commits", "--session", session_id_arg, "--repo", repo_root, "--json", sha_a, sha_b });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"commit_count\":2") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"inserted_count\":2") != null);

    try assertCommitRows(
        gpa,
        suite.db_path,
        session_id,
        canonical_repo_root,
        branch,
        &.{
            .{ .sha = sha_a, .subject = "after session a" },
            .{ .sha = sha_b, .subject = "after session b" },
        },
    );
}

test "capture commits records a range via --repo from outside the repository" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const repo_root = try makeFixtureRepo(gpa, &suite, "capture-commits-external-range-repo");
    defer gpa.free(repo_root);
    const outside = suite.freshSystemTmpDir();

    const open_out = suite.mustRunInDir(repo_root, &.{ "capture", "session", "--json" });
    defer gpa.free(open_out);
    const session_id = extractIntField(open_out, "\"id\"") orelse @panic("no session id");

    const base = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "rev-parse", "HEAD" }));
    defer gpa.free(base);
    const sha_a = try createCommit(repo_root, "outside-a.txt", "a\n", "outside repo alpha");
    defer gpa.free(sha_a);
    const sha_b = try createCommit(repo_root, "outside-b.txt", "b\n", "outside repo beta");
    defer gpa.free(sha_b);

    const out = suite.mustRunInDir(outside, &.{ "capture", "commits", "--repo", repo_root, "--since", base, "--json" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"commit_count\":2") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"inserted_count\":2") != null);

    const canonical_repo_root = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "rev-parse", "--show-toplevel" }));
    defer gpa.free(canonical_repo_root);
    const branch = try trimOwned(gpa, try runCommandInDir(repo_root, &.{ "git", "branch", "--show-current" }));
    defer gpa.free(branch);

    try assertCommitRows(
        gpa,
        suite.db_path,
        session_id,
        canonical_repo_root,
        branch,
        &.{
            .{ .sha = sha_a, .subject = "outside repo alpha" },
            .{ .sha = sha_b, .subject = "outside repo beta" },
        },
    );
}

test "capture commits fails loudly for missing active session, non-git repo, bad ref, and unknown session" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const repo_root = try makeFixtureRepo(gpa, &suite, "capture-commits-errors-repo");
    defer gpa.free(repo_root);
    const outside = suite.freshSystemTmpDir();

    const no_session = suite.expectFailureInDir(repo_root, &.{ "capture", "commits", "--since", "HEAD" });
    defer gpa.free(no_session);
    try std.testing.expect(std.mem.indexOf(u8, no_session, "no active session") != null);

    const open_out = suite.mustRunInDir(repo_root, &.{ "capture", "session", "--json" });
    defer gpa.free(open_out);
    const session_id = extractIntField(open_out, "\"id\"") orelse @panic("no session id");
    const session_id_arg = try std.fmt.allocPrint(gpa, "{d}", .{session_id});
    defer gpa.free(session_id_arg);

    const nongit = suite.expectFailureInDir(outside, &.{ "capture", "commits", "--session", session_id_arg, "--repo", outside, "--since", "HEAD" });
    defer gpa.free(nongit);
    try std.testing.expect(std.mem.indexOf(u8, nongit, "repo is not a git repository") != null);

    const bad_ref = suite.expectFailureInDir(repo_root, &.{ "capture", "commits", "--since", "not-a-ref" });
    defer gpa.free(bad_ref);
    try std.testing.expect(std.mem.indexOf(u8, bad_ref, "cannot resolve ref 'not-a-ref'") != null);

    const unknown_sha = suite.expectFailureInDir(repo_root, &.{ "capture", "commits", "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef" });
    defer gpa.free(unknown_sha);
    try std.testing.expect(std.mem.indexOf(u8, unknown_sha, "one or more commit SHAs could not be resolved") != null);

    const unknown_session = suite.expectFailureInDir(repo_root, &.{ "capture", "commits", "--session", "999999", "--since", "HEAD" });
    defer gpa.free(unknown_session);
    try std.testing.expect(std.mem.indexOf(u8, unknown_session, "session 999999 not found") != null);
}

const ExpectedCommit = struct {
    sha: []const u8,
    subject: []const u8,
};

fn runCommand(argv: []const []const u8) ![]u8 {
    const gpa = std.testing.allocator;
    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv,
    });
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("command failed: {s}\nstderr: {s}\n", .{ argv[0], result.stderr });
        gpa.free(result.stdout);
        return error.CommandFailed;
    }
    return result.stdout;
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

fn trimOwned(gpa: std.mem.Allocator, raw: []u8) ![]u8 {
    defer gpa.free(raw);
    return try gpa.dupe(u8, std.mem.trim(u8, raw, " \t\r\n"));
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
    return trimOwned(std.testing.allocator, try runCommandInDir(repo_root, &.{ "git", "rev-parse", "HEAD" }));
}

fn sqliteQueryLines(
    gpa: std.mem.Allocator,
    db_path: []const u8,
    sql: []const u8,
) ![]u8 {
    const result = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", "-separator", "|", db_path, sql },
    }) catch |e| {
        std.debug.print("sqlite3 spawn failed: {s}\n", .{@errorName(e)});
        return error.SkipZigTest;
    };
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("sqlite3 failed: {s}\n", .{result.stderr});
        gpa.free(result.stdout);
        return error.SqliteFailed;
    }
    return result.stdout;
}

fn sqliteScalar(
    gpa: std.mem.Allocator,
    db_path: []const u8,
    sql: []const u8,
) !i64 {
    const out = try sqliteQueryLines(gpa, db_path, sql);
    defer gpa.free(out);
    const trimmed = std.mem.trim(u8, out, " \t\r\n");
    return std.fmt.parseInt(i64, trimmed, 10) catch |e| {
        std.debug.print("sqlite3 output not integer ('{s}'): {s}\n", .{ trimmed, @errorName(e) });
        return error.SqliteParseFailed;
    };
}

fn assertCommitRows(
    gpa: std.mem.Allocator,
    db_path: []const u8,
    session_id: i64,
    repo_root: []const u8,
    branch: []const u8,
    expected: []const ExpectedCommit,
) !void {
    const sql = try std.fmt.allocPrint(gpa,
        \\select sha, claim_id, subject, author, committed_at, repo_root, branch
        \\from session_commits
        \\where session_id = {d}
        \\order by sha asc;
    , .{session_id});
    defer gpa.free(sql);
    const out = try sqliteQueryLines(gpa, db_path, sql);
    defer gpa.free(out);

    var actual_lines = std.ArrayList([]const u8).empty;
    defer actual_lines.deinit(gpa);
    var split = std.mem.splitScalar(u8, std.mem.trim(u8, out, "\n"), '\n');
    while (split.next()) |line| {
        if (line.len == 0) continue;
        try actual_lines.append(gpa, line);
    }
    try std.testing.expectEqual(expected.len, actual_lines.items.len);

    const expected_sorted = try gpa.alloc(ExpectedCommit, expected.len);
    defer gpa.free(expected_sorted);
    @memcpy(expected_sorted, expected);
    std.mem.sort(ExpectedCommit, expected_sorted, {}, struct {
        fn lessThan(_: void, a: ExpectedCommit, b: ExpectedCommit) bool {
            return std.mem.lessThan(u8, a.sha, b.sha);
        }
    }.lessThan);

    for (actual_lines.items, expected_sorted) |line, want| {
        var fields = std.mem.splitScalar(u8, line, '|');
        const sha = fields.next() orelse return error.MalformedSqliteRow;
        const claim_id = fields.next() orelse return error.MalformedSqliteRow;
        const subject = fields.next() orelse return error.MalformedSqliteRow;
        const author = fields.next() orelse return error.MalformedSqliteRow;
        const committed_at = fields.next() orelse return error.MalformedSqliteRow;
        const got_repo_root = fields.next() orelse return error.MalformedSqliteRow;
        const got_branch = fields.next() orelse return error.MalformedSqliteRow;

        try std.testing.expectEqualStrings(want.sha, sha);
        try std.testing.expectEqualStrings("", claim_id);
        try std.testing.expectEqualStrings(want.subject, subject);
        try std.testing.expectEqualStrings("Planar Test", author);
        try std.testing.expect(committed_at.len > 0);
        try std.testing.expectEqualStrings(repo_root, got_repo_root);
        try std.testing.expectEqualStrings(branch, got_branch);
    }
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
