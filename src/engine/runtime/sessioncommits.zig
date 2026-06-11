//! engine/runtime/sessioncommits — git-backed session commit collection.
//!
//! This module provides the runtime-facing git walkers for session commit
//! attribution. `walk()` is the automatic fail-soft path used by later
//! terminal/capture callers: any git failure degrades to zero commits.
//! `walkStrict()` and `resolveShas()` surface subprocess failures for
//! explicit operator verbs.

const std = @import("std");
const db = @import("db");

pub const Commit = struct {
    sha: []const u8,
    repo_root: ?[]const u8,
    branch: ?[]const u8,
    subject: ?[]const u8,
    author: ?[]const u8,
    committed_at: ?[]const u8,

    pub fn deinit(self: Commit, allocator: std.mem.Allocator) void {
        allocator.free(self.sha);
        if (self.repo_root) |value| allocator.free(value);
        if (self.branch) |value| allocator.free(value);
        if (self.subject) |value| allocator.free(value);
        if (self.author) |value| allocator.free(value);
        if (self.committed_at) |value| allocator.free(value);
    }
};

pub fn deinitMany(items: []const Commit, allocator: std.mem.Allocator) void {
    for (items) |item| item.deinit(allocator);
    allocator.free(items);
}

pub const Error = std.mem.Allocator.Error || error{
    GitFailed,
    NotGit,
    MalformedOutput,
    QueryFailed,
};

pub const Row = struct {
    id: i64,
    session_id: i64,
    claim_id: ?i64,
    sha: []const u8,
    repo_root: ?[]const u8,
    branch: ?[]const u8,
    subject: ?[]const u8,
    author: ?[]const u8,
    committed_at: ?[]const u8,
    recorded_at: []const u8,

    pub fn deinit(self: Row, allocator: std.mem.Allocator) void {
        allocator.free(self.sha);
        if (self.repo_root) |value| allocator.free(value);
        if (self.branch) |value| allocator.free(value);
        if (self.subject) |value| allocator.free(value);
        if (self.author) |value| allocator.free(value);
        if (self.committed_at) |value| allocator.free(value);
        allocator.free(self.recorded_at);
    }

    pub fn deinitMany(items: []const Row, allocator: std.mem.Allocator) void {
        for (items) |item| item.deinit(allocator);
        allocator.free(items);
    }
};

pub const ListFilter = struct {
    session_id: ?i64 = null,
    task_id: ?i64 = null,
};

pub const WalkArgs = struct {
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: []const u8,
    base_sha: []const u8,
    repo_root: ?[]const u8 = null,
};

pub const ResolveArgs = struct {
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: []const u8,
    shas: []const []const u8,
    repo_root: ?[]const u8 = null,
};

pub const ClaimWindow = struct {
    claim_id: i64,
    session_id: i64,
    worktree_path: ?[]const u8 = null,
    repo_root: ?[]const u8 = null,
    head_sha_at_claim: ?[]const u8 = null,
};

pub const RecordClaimWindowArgs = struct {
    allocator: std.mem.Allocator,
    io: std.Io,
    no_locality_probe: bool = false,
    window: ClaimWindow,
};

const log_format = "%H%x00%an%x00%s%x00%cI%x00";

const insert_sql: [:0]const u8 =
    \\insert or ignore into session_commits (
    \\  session_id, claim_id, sha, repo_root, branch, subject, author, committed_at
    \\) values (?, ?, ?, ?, ?, ?, ?, ?)
;

/// Persist commit rows for one session. The `(session_id, sha)` unique
/// constraint makes re-recording idempotent by design.
pub fn record(
    d: *db.sqlite.Db,
    session_id: i64,
    claim_id: ?i64,
    commits: []const Commit,
) Error!void {
    _ = try recordCount(d, session_id, claim_id, commits);
}

/// Persist commit rows for one session and return how many new rows were
/// inserted (duplicate `(session_id, sha)` rows count as zero).
pub fn recordCount(
    d: *db.sqlite.Db,
    session_id: i64,
    claim_id: ?i64,
    commits: []const Commit,
) Error!usize {
    var inserted: usize = 0;
    for (commits) |commit| {
        _ = d.execParams(insert_sql, &.{
            .{ .int = session_id },
            if (claim_id) |value| .{ .int = value } else .{ .null = {} },
            .{ .text = commit.sha },
            if (commit.repo_root) |value| .{ .text = value } else .{ .null = {} },
            if (commit.branch) |value| .{ .text = value } else .{ .null = {} },
            if (commit.subject) |value| .{ .text = value } else .{ .null = {} },
            if (commit.author) |value| .{ .text = value } else .{ .null = {} },
            if (commit.committed_at) |value| .{ .text = value } else .{ .null = {} },
        }) catch return Error.QueryFailed;
        const changed = d.intQuery("select changes()") catch return Error.QueryFailed;
        inserted += @intCast(changed);
    }
    return inserted;
}

/// Read persisted commits with optional session and task-via-claim
/// filters, ordered newest-first by `recorded_at`, then `id`.
pub fn listFiltered(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: ListFilter,
) Error![]const Row {
    var sql: std.ArrayList(u8) = .empty;
    defer sql.deinit(allocator);

    try sql.appendSlice(allocator,
        \\select sc.id, sc.session_id, sc.claim_id, sc.sha, sc.repo_root,
        \\       sc.branch, sc.subject, sc.author, sc.committed_at, sc.recorded_at
        \\from session_commits sc
    );
    if (filter.task_id != null) {
        try sql.appendSlice(allocator,
            \\ join agent_work_claims awc on awc.id = sc.claim_id
        );
    }
    try sql.appendSlice(allocator, "\nwhere 1 = 1");
    if (filter.session_id != null) try sql.appendSlice(allocator, "\n  and sc.session_id = ?");
    if (filter.task_id != null) {
        try sql.appendSlice(allocator,
            \\
            \\  and awc.entity_kind = 'task'
            \\  and awc.entity_id = ?
        );
    }
    try sql.appendSlice(allocator,
        \\
        \\order by sc.recorded_at desc, sc.id desc
    );

    const query = try allocator.dupeZ(u8, sql.items);
    defer allocator.free(query);

    var stmt = d.prepare(query) catch return Error.QueryFailed;
    defer stmt.finalize();

    var params: [2]db.sqlite.Param = undefined;
    var param_count: usize = 0;
    if (filter.session_id) |value| {
        params[param_count] = .{ .int = value };
        param_count += 1;
    }
    if (filter.task_id) |value| {
        params[param_count] = .{ .int = value };
        param_count += 1;
    }
    stmt.bind(params[0..param_count]) catch return Error.QueryFailed;
    return readRows(&stmt, allocator);
}

/// Batch-read commits for many sessions in one query for audit fold-ins.
/// Rows are ordered newest-first across the whole session set, with an
/// optional cap applied at the SQL layer.
pub fn listForSessions(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_ids: []const i64,
    limit: ?i64,
) Error![]const Row {
    if (session_ids.len == 0) return try allocator.alloc(Row, 0);

    var sql: std.ArrayList(u8) = .empty;
    defer sql.deinit(allocator);
    try sql.appendSlice(allocator,
        \\select sc.id, sc.session_id, sc.claim_id, sc.sha, sc.repo_root,
        \\       sc.branch, sc.subject, sc.author, sc.committed_at, sc.recorded_at
        \\from session_commits sc
        \\where sc.session_id in (
    );
    for (session_ids, 0..) |_, index| {
        if (index > 0) try sql.appendSlice(allocator, ", ");
        try sql.appendSlice(allocator, "?");
    }
    try sql.appendSlice(allocator,
        \\)
        \\order by sc.recorded_at desc, sc.id desc
    );
    if (limit) |_| {
        try sql.appendSlice(allocator,
            \\
            \\limit ?
        );
    }

    const query = try allocator.dupeZ(u8, sql.items);
    defer allocator.free(query);

    var stmt = d.prepare(query) catch return Error.QueryFailed;
    defer stmt.finalize();

    const extra_params: usize = if (limit != null) 1 else 0;
    var params = try allocator.alloc(db.sqlite.Param, session_ids.len + extra_params);
    defer allocator.free(params);
    for (session_ids, 0..) |session_id, index| {
        params[index] = .{ .int = session_id };
    }
    if (limit) |value| {
        params[session_ids.len] = .{ .int = value };
    }
    stmt.bind(params) catch return Error.QueryFailed;
    return readRows(&stmt, allocator);
}

pub fn writeJson(writer: *std.Io.Writer, row: Row) !void {
    try writer.print("{{\"id\":{d}", .{row.id});
    try writer.print(",\"session_id\":{d}", .{row.session_id});
    try writeIntOpt(writer, "claim_id", row.claim_id);
    try writer.print(",\"sha\":", .{});
    try std.json.Stringify.encodeJsonString(row.sha, .{}, writer);
    try writeStringOpt(writer, "repo_root", row.repo_root);
    try writeStringOpt(writer, "branch", row.branch);
    try writeStringOpt(writer, "subject", row.subject);
    try writeStringOpt(writer, "author", row.author);
    try writeStringOpt(writer, "committed_at", row.committed_at);
    try writer.print(",\"recorded_at\":", .{});
    try std.json.Stringify.encodeJsonString(row.recorded_at, .{}, writer);
    try writer.print("}}", .{});
}

pub fn writeJsonList(writer: *std.Io.Writer, rows: []const Row) !void {
    try writer.print("[", .{});
    for (rows, 0..) |row, index| {
        if (index > 0) try writer.print(",", .{});
        try writeJson(writer, row);
    }
    try writer.print("]", .{});
}

/// Best-effort automatic claim-window collection. Never throws and never
/// shells git when `no_locality_probe` is true.
pub fn recordClaimWindowBestEffort(
    d: *db.sqlite.Db,
    args: RecordClaimWindowArgs,
) void {
    if (args.no_locality_probe) return;

    const base_sha = args.window.head_sha_at_claim orelse return;
    const repo_root = args.window.repo_root;
    const primary_dir = args.window.worktree_path orelse repo_root orelse return;
    const fallback_dir = blk: {
        if (repo_root) |root| {
            if (!std.mem.eql(u8, root, primary_dir)) break :blk root;
        }
        break :blk null;
    };

    const commits = walkClaimWindow(args, primary_dir, fallback_dir, base_sha) catch return;
    defer deinitMany(commits, args.allocator);
    if (commits.len == 0) return;

    d.exec("begin immediate") catch return;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    record(d, args.window.session_id, args.window.claim_id, commits) catch return;
    d.exec("commit") catch return;
    committed = true;
}

/// Walk `base_sha..HEAD` and return commit metadata. Any git failure
/// degrades to an empty slice; malformed stdout also degrades to empty.
pub fn walk(args: WalkArgs) std.mem.Allocator.Error![]const Commit {
    return walkStrict(args) catch return try args.allocator.alloc(Commit, 0);
}

/// Walk `base_sha..HEAD` and return commit metadata, surfacing git and
/// parse failures for explicit/manual callers.
pub fn walkStrict(args: WalkArgs) Error![]const Commit {
    const range = try std.fmt.allocPrint(args.allocator, "{s}..HEAD", .{args.base_sha});
    defer args.allocator.free(range);

    const raw = try runGitStrict(
        args.allocator,
        args.io,
        &.{ "git", "-C", args.dir, "log", "-z", "--format=" ++ log_format, range },
    );
    defer args.allocator.free(raw);

    const branch = try branchForRepo(args.allocator, args.io, args.dir);
    defer if (branch) |value| args.allocator.free(value);

    return parseCommits(args.allocator, args.repo_root orelse args.dir, branch, raw);
}

/// Resolve explicit SHAs into the same denormalized metadata shape used by
/// `walkStrict()`. Unknown SHAs or non-git directories surface as errors.
pub fn resolveShas(args: ResolveArgs) Error![]const Commit {
    if (args.shas.len == 0) return try args.allocator.alloc(Commit, 0);

    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(args.allocator);

    try argv.append(args.allocator, "git");
    try argv.append(args.allocator, "-C");
    try argv.append(args.allocator, args.dir);
    try argv.append(args.allocator, "show");
    try argv.append(args.allocator, "--no-patch");
    try argv.append(args.allocator, "-z");
    try argv.append(args.allocator, "--format=" ++ log_format);
    for (args.shas) |sha| try argv.append(args.allocator, sha);

    const raw = try runGitStrict(args.allocator, args.io, argv.items);
    defer args.allocator.free(raw);

    const branch = try branchForRepo(args.allocator, args.io, args.dir);
    defer if (branch) |value| args.allocator.free(value);

    return parseCommits(args.allocator, args.repo_root orelse args.dir, branch, raw);
}

/// Resolve a repo path to the canonical git toplevel. Non-git directories
/// surface `error.NotGit` for the explicit/manual operator paths.
pub fn resolveRepoRootStrict(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: []const u8,
) Error![]const u8 {
    const raw = runGitStrict(
        allocator,
        io,
        &.{ "git", "-C", dir, "rev-parse", "--show-toplevel" },
    ) catch |err| switch (err) {
        error.GitFailed => return Error.NotGit,
        else => return err,
    };
    defer allocator.free(raw);

    const trimmed = std.mem.trim(u8, raw, " \t\r\n");
    if (trimmed.len == 0) return Error.MalformedOutput;
    return try allocator.dupe(u8, trimmed);
}

fn branchForRepo(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: []const u8,
) std.mem.Allocator.Error!?[]const u8 {
    const raw = runGitBestEffort(allocator, io, &.{ "git", "-C", dir, "symbolic-ref", "--short", "HEAD" }) orelse return null;
    defer allocator.free(raw);
    const trimmed = std.mem.trim(u8, raw, " \t\r\n");
    if (trimmed.len == 0) return null;
    return try allocator.dupe(u8, trimmed);
}

fn writeStringOpt(writer: *std.Io.Writer, key: []const u8, value: ?[]const u8) !void {
    try writer.print(",\"{s}\":", .{key});
    if (value) |text| {
        try std.json.Stringify.encodeJsonString(text, .{}, writer);
    } else {
        try writer.print("null", .{});
    }
}

fn writeIntOpt(writer: *std.Io.Writer, key: []const u8, value: ?i64) !void {
    if (value) |int_value| {
        try writer.print(",\"{s}\":{d}", .{ key, int_value });
    } else {
        try writer.print(",\"{s}\":null", .{key});
    }
}

fn walkClaimWindow(
    args: RecordClaimWindowArgs,
    primary_dir: []const u8,
    fallback_dir: ?[]const u8,
    base_sha: []const u8,
) std.mem.Allocator.Error![]const Commit {
    const primary = walkStrict(.{
        .allocator = args.allocator,
        .io = args.io,
        .dir = primary_dir,
        .base_sha = base_sha,
        .repo_root = args.window.repo_root,
    }) catch {
        if (fallback_dir == null) return try args.allocator.alloc(Commit, 0);
        return walk(.{
            .allocator = args.allocator,
            .io = args.io,
            .dir = fallback_dir.?,
            .base_sha = base_sha,
            .repo_root = args.window.repo_root,
        });
    };
    return primary;
}

fn gitClone(allocator: std.mem.Allocator, source: []const u8, target: []const u8) !void {
    const result = try std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "git", "clone", source, target },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);
    switch (result.term) {
        .exited => |code| {
            if (code != 0) return error.GitCommandFailed;
        },
        else => return error.GitCommandFailed,
    }
}

fn freshSiblingRepoPath(allocator: std.mem.Allocator, name: []const u8) ![]const u8 {
    var tmp = std.testing.tmpDir(.{});
    errdefer tmp.cleanup();

    const root = try tmpRootPath(allocator, &tmp);
    defer allocator.free(root);

    const sibling = try std.fs.path.join(allocator, &.{ root, name });
    errdefer allocator.free(sibling);

    std.mem.doNotOptimizeAway(tmp);
    return sibling;
}

fn runGitBestEffort(
    allocator: std.mem.Allocator,
    io: std.Io,
    argv: []const []const u8,
) ?[]u8 {
    const result = std.process.run(allocator, io, .{ .argv = argv }) catch return null;
    defer allocator.free(result.stderr);
    switch (result.term) {
        .exited => |code| {
            if (code != 0) {
                allocator.free(result.stdout);
                return null;
            }
        },
        else => {
            allocator.free(result.stdout);
            return null;
        },
    }
    return result.stdout;
}

fn runGitStrict(
    allocator: std.mem.Allocator,
    io: std.Io,
    argv: []const []const u8,
) Error![]u8 {
    const result = std.process.run(allocator, io, .{ .argv = argv }) catch return Error.GitFailed;
    defer allocator.free(result.stderr);
    switch (result.term) {
        .exited => |code| {
            if (code != 0) {
                allocator.free(result.stdout);
                return Error.GitFailed;
            }
        },
        else => {
            allocator.free(result.stdout);
            return Error.GitFailed;
        },
    }
    return result.stdout;
}

fn parseCommits(
    allocator: std.mem.Allocator,
    repo_root: []const u8,
    branch: ?[]const u8,
    raw: []const u8,
) Error![]const Commit {
    var out: std.ArrayList(Commit) = .empty;
    errdefer {
        for (out.items) |item| item.deinit(allocator);
        out.deinit(allocator);
    }

    var index: usize = 0;
    while (index < raw.len) {
        while (index < raw.len and raw[index] == 0) : (index += 1) {}
        if (index >= raw.len) break;

        const sha = try nextField(allocator, raw, &index);
        errdefer allocator.free(sha);
        const author = try nextField(allocator, raw, &index);
        errdefer allocator.free(author);
        const subject = try nextField(allocator, raw, &index);
        errdefer allocator.free(subject);
        const committed_at = try nextField(allocator, raw, &index);
        errdefer allocator.free(committed_at);

        try out.append(allocator, .{
            .sha = sha,
            .repo_root = try allocator.dupe(u8, repo_root),
            .branch = if (branch) |value| try allocator.dupe(u8, value) else null,
            .subject = subject,
            .author = author,
            .committed_at = committed_at,
        });
    }

    return out.toOwnedSlice(allocator);
}

fn nextField(
    allocator: std.mem.Allocator,
    raw: []const u8,
    index: *usize,
) Error![]const u8 {
    const start = index.*;
    const rel_end = std.mem.indexOfScalarPos(u8, raw, start, 0) orelse return Error.MalformedOutput;
    index.* = rel_end + 1;
    return try allocator.dupe(u8, raw[start..rel_end]);
}

fn readRows(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error![]const Row {
    var out: std.ArrayList(Row) = .empty;
    errdefer {
        for (out.items) |row| row.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readRow(stmt, allocator)),
        }
    }

    return out.toOwnedSlice(allocator);
}

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Row {
    return .{
        .id = stmt.columnInt(0),
        .session_id = stmt.columnInt(1),
        .claim_id = stmt.columnIntOpt(2),
        .sha = try stmt.columnTextAlloc(3, allocator),
        .repo_root = try stmt.columnTextOpt(4, allocator),
        .branch = try stmt.columnTextOpt(5, allocator),
        .subject = try stmt.columnTextOpt(6, allocator),
        .author = try stmt.columnTextOpt(7, allocator),
        .committed_at = try stmt.columnTextOpt(8, allocator),
        .recorded_at = try stmt.columnTextAlloc(9, allocator),
    };
}

fn ensureGitAvailable(allocator: std.mem.Allocator) !void {
    const result = std.process.run(allocator, std.testing.io, .{ .argv = &.{ "git", "--version" } }) catch return error.SkipZigTest;
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);
    switch (result.term) {
        .exited => |code| if (code != 0) return error.SkipZigTest,
        else => return error.SkipZigTest,
    }
}

fn tmpRootPath(allocator: std.mem.Allocator, tmp: *std.testing.TmpDir) ![]const u8 {
    const rel = try std.fs.path.join(allocator, &.{ ".zig-cache", "tmp", &tmp.sub_path });
    defer allocator.free(rel);
    const sentinel = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, rel, allocator);
    defer allocator.free(sentinel);
    return allocator.dupe(u8, sentinel[0..sentinel.len]);
}

fn gitMust(
    allocator: std.mem.Allocator,
    dir: []const u8,
    args: []const []const u8,
) ![]u8 {
    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(allocator);
    try argv.append(allocator, "git");
    try argv.append(allocator, "-C");
    try argv.append(allocator, dir);
    for (args) |arg| try argv.append(allocator, arg);

    const result = try std.process.run(allocator, std.testing.io, .{ .argv = argv.items });
    defer allocator.free(result.stderr);
    switch (result.term) {
        .exited => |code| {
            if (code != 0) {
                allocator.free(result.stdout);
                return error.GitCommandFailed;
            }
        },
        else => {
            allocator.free(result.stdout);
            return error.GitCommandFailed;
        },
    }
    return result.stdout;
}

fn gitTrim(
    allocator: std.mem.Allocator,
    dir: []const u8,
    args: []const []const u8,
) ![]const u8 {
    const raw = try gitMust(allocator, dir, args);
    defer allocator.free(raw);
    return try allocator.dupe(u8, std.mem.trim(u8, raw, " \t\r\n"));
}

fn initFixtureRepo(allocator: std.mem.Allocator) ![]const u8 {
    try ensureGitAvailable(allocator);

    var tmp = std.testing.tmpDir(.{});
    errdefer tmp.cleanup();

    const root = try tmpRootPath(allocator, &tmp);
    errdefer allocator.free(root);

    const init_out = try gitMust(allocator, root, &.{"init"});
    defer allocator.free(init_out);
    const checkout_out = try gitMust(allocator, root, &.{ "checkout", "-b", "main" });
    defer allocator.free(checkout_out);
    const email_out = try gitMust(allocator, root, &.{ "config", "user.email", "planar@example.com" });
    defer allocator.free(email_out);
    const name_out = try gitMust(allocator, root, &.{ "config", "user.name", "Planar Test User" });
    defer allocator.free(name_out);

    // Intentionally leak cleanup ownership to the test process; tmp dirs are
    // under the harness-owned `.zig-cache/tmp` tree and disappear after the
    // test run.
    std.mem.doNotOptimizeAway(tmp);
    return root;
}

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

fn insertTestSession(d: *db.sqlite.Db, vendor: []const u8) !i64 {
    return d.execParams(
        "insert into sessions (vendor) values (?)",
        &.{.{ .text = vendor }},
    );
}

fn insertTestTask(d: *db.sqlite.Db, title: []const u8) !i64 {
    return d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global', ?, 'todo')",
        &.{.{ .text = title }},
    );
}

fn insertTestClaim(d: *db.sqlite.Db, session_id: i64, task_id: i64, token: []const u8) !i64 {
    return d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id, vendor, lease_expires_at
        \\) values (?, ?, 'task', ?, 'test', strftime('%Y-%m-%dT%H:%M:%fZ','now', '+10 minutes'))
    , &.{
        .{ .text = token },
        .{ .int = session_id },
        .{ .int = task_id },
    });
}

fn testCommit(
    sha: []const u8,
    repo_root: ?[]const u8,
    branch: ?[]const u8,
    subject: ?[]const u8,
    author: ?[]const u8,
    committed_at: ?[]const u8,
) Commit {
    return .{
        .sha = sha,
        .repo_root = repo_root,
        .branch = branch,
        .subject = subject,
        .author = author,
        .committed_at = committed_at,
    };
}

fn commitEmpty(allocator: std.mem.Allocator, dir: []const u8, message: []const u8) ![]const u8 {
    const out = try gitMust(allocator, dir, &.{ "commit", "--allow-empty", "-m", message });
    defer allocator.free(out);
    return gitTrim(allocator, dir, &.{ "rev-parse", "HEAD" });
}

test "walkStrict returns commits in base..HEAD with owned metadata" {
    const a = std.testing.allocator;
    const repo = try initFixtureRepo(a);
    defer a.free(repo);

    const base = try commitEmpty(a, repo, "base");
    defer a.free(base);
    const first = try commitEmpty(a, repo, "first: punctuation, spaces, and commas");
    defer a.free(first);
    const second = try commitEmpty(a, repo, "second subject");
    defer a.free(second);

    const commits = try walkStrict(.{
        .allocator = a,
        .io = std.testing.io,
        .dir = repo,
        .base_sha = base,
    });
    defer deinitMany(commits, a);

    try std.testing.expectEqual(@as(usize, 2), commits.len);
    try std.testing.expectEqualStrings(second, commits[0].sha);
    try std.testing.expectEqualStrings(first, commits[1].sha);
    try std.testing.expectEqualStrings(repo, commits[0].repo_root.?);
    try std.testing.expectEqualStrings("main", commits[0].branch.?);
    try std.testing.expectEqualStrings("Planar Test User", commits[0].author.?);
    try std.testing.expectEqualStrings("second subject", commits[0].subject.?);
    try std.testing.expect(commits[0].committed_at != null);
}

test "walkStrict returns empty for an empty range even with dirty uncommitted files" {
    const a = std.testing.allocator;
    const repo = try initFixtureRepo(a);
    defer a.free(repo);

    const base = try commitEmpty(a, repo, "base");
    defer a.free(base);
    const head = try gitTrim(a, repo, &.{ "rev-parse", "HEAD" });
    defer a.free(head);

    const dirty_path = try std.fs.path.join(a, &.{ repo, "dirty.txt" });
    defer a.free(dirty_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = dirty_path,
        .data = "dirty\n",
    });

    const commits = try walkStrict(.{
        .allocator = a,
        .io = std.testing.io,
        .dir = repo,
        .base_sha = head,
    });
    defer deinitMany(commits, a);

    try std.testing.expectEqual(@as(usize, 0), commits.len);
}

test "resolveShas returns explicit commits in requested order" {
    const a = std.testing.allocator;
    const repo = try initFixtureRepo(a);
    defer a.free(repo);

    const first = try commitEmpty(a, repo, "alpha");
    defer a.free(first);
    const second = try commitEmpty(a, repo, "beta");
    defer a.free(second);

    const commits = try resolveShas(.{
        .allocator = a,
        .io = std.testing.io,
        .dir = repo,
        .shas = &.{ second, first },
    });
    defer deinitMany(commits, a);

    try std.testing.expectEqual(@as(usize, 2), commits.len);
    try std.testing.expectEqualStrings(second, commits[0].sha);
    try std.testing.expectEqualStrings("beta", commits[0].subject.?);
    try std.testing.expectEqualStrings(first, commits[1].sha);
    try std.testing.expectEqualStrings("alpha", commits[1].subject.?);
}

test "walkStrict and resolveShas surface git failures" {
    const a = std.testing.allocator;
    const repo = try initFixtureRepo(a);
    defer a.free(repo);

    const base = try commitEmpty(a, repo, "base");
    defer a.free(base);

    try std.testing.expectError(error.GitFailed, walkStrict(.{
        .allocator = a,
        .io = std.testing.io,
        .dir = repo,
        .base_sha = "not-a-ref",
    }));

    try std.testing.expectError(error.GitFailed, resolveShas(.{
        .allocator = a,
        .io = std.testing.io,
        .dir = repo,
        .shas = &.{"definitely-not-a-sha"},
    }));
}

test "walk fail-soft degrades bad refs and missing directories to zero commits" {
    const a = std.testing.allocator;
    const repo = try initFixtureRepo(a);
    defer a.free(repo);

    const base = try commitEmpty(a, repo, "base");
    defer a.free(base);

    const bad_ref = try walk(.{
        .allocator = a,
        .io = std.testing.io,
        .dir = repo,
        .base_sha = "not-a-ref",
    });
    defer deinitMany(bad_ref, a);
    try std.testing.expectEqual(@as(usize, 0), bad_ref.len);

    const missing = try walk(.{
        .allocator = a,
        .io = std.testing.io,
        .dir = "/nonexistent/planar-session-commits-missing",
        .base_sha = "HEAD",
    });
    defer deinitMany(missing, a);
    try std.testing.expectEqual(@as(usize, 0), missing.len);
}

test "record is idempotent per session and same sha can exist in another session" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const session_a = try insertTestSession(&d, "test-a");
    const session_b = try insertTestSession(&d, "test-b");

    const commits = [_]Commit{
        testCommit("sha-1", "/repo", "main", "subject", "author", "2026-06-10T00:00:00Z"),
        testCommit("sha-2", "/repo", "main", "subject-2", "author", "2026-06-10T00:00:01Z"),
    };

    try record(&d, session_a, null, &commits);
    try record(&d, session_a, null, &commits);
    try record(&d, session_b, null, commits[0..1]);

    const total = try d.intQuery("select count(*) from session_commits");
    try std.testing.expectEqual(@as(i64, 3), total);
}

test "listFiltered orders newest first and filters by session" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const session_a = try insertTestSession(&d, "test-a");
    const session_b = try insertTestSession(&d, "test-b");

    _ = try d.execParams(
        \\insert into session_commits (
        \\  session_id, sha, subject, recorded_at
        \\) values (?, ?, ?, ?)
    , &.{
        .{ .int = session_a },
        .{ .text = "older-a" },
        .{ .text = "older" },
        .{ .text = "2026-06-10T00:00:00.000Z" },
    });
    _ = try d.execParams(
        \\insert into session_commits (
        \\  session_id, sha, subject, recorded_at
        \\) values (?, ?, ?, ?)
    , &.{
        .{ .int = session_a },
        .{ .text = "newer-a" },
        .{ .text = "newer" },
        .{ .text = "2026-06-10T00:00:01.000Z" },
    });
    _ = try d.execParams(
        \\insert into session_commits (
        \\  session_id, sha, subject, recorded_at
        \\) values (?, ?, ?, ?)
    , &.{
        .{ .int = session_b },
        .{ .text = "other-session" },
        .{ .text = "other" },
        .{ .text = "2026-06-10T00:00:02.000Z" },
    });

    const filtered = try listFiltered(&d, a, .{ .session_id = session_a });
    defer Row.deinitMany(filtered, a);

    try std.testing.expectEqual(@as(usize, 2), filtered.len);
    try std.testing.expectEqualStrings("newer-a", filtered[0].sha);
    try std.testing.expectEqualStrings("older-a", filtered[1].sha);
    try std.testing.expectEqual(session_a, filtered[0].session_id);
}

test "listFiltered task filter traverses claim linkage" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const session_a = try insertTestSession(&d, "test-a");
    const session_b = try insertTestSession(&d, "test-b");
    const task_a = try insertTestTask(&d, "task-a");
    const task_b = try insertTestTask(&d, "task-b");
    const claim_a = try insertTestClaim(&d, session_a, task_a, "claim-a");
    const claim_b = try insertTestClaim(&d, session_b, task_b, "claim-b");

    try record(&d, session_a, claim_a, &.{testCommit("sha-a", "/repo", "main", "sub-a", "author", "2026-06-10T00:00:00Z")});
    try record(&d, session_b, claim_b, &.{testCommit("sha-b", "/repo", "main", "sub-b", "author", "2026-06-10T00:00:01Z")});
    try record(&d, session_a, null, &.{testCommit("sha-unclaimed", "/repo", "main", "sub-u", "author", "2026-06-10T00:00:02Z")});

    const filtered = try listFiltered(&d, a, .{ .task_id = task_a });
    defer Row.deinitMany(filtered, a);

    try std.testing.expectEqual(@as(usize, 1), filtered.len);
    try std.testing.expectEqual(claim_a, filtered[0].claim_id.?);
    try std.testing.expectEqualStrings("sha-a", filtered[0].sha);
}

test "listForSessions returns newest rows across sessions in one read" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const session_a = try insertTestSession(&d, "test-a");
    const session_b = try insertTestSession(&d, "test-b");

    _ = try d.execParams(
        "insert into session_commits (session_id, sha, recorded_at) values (?, ?, ?)",
        &.{ .{ .int = session_a }, .{ .text = "a-older" }, .{ .text = "2026-06-10T00:00:00.000Z" } },
    );
    _ = try d.execParams(
        "insert into session_commits (session_id, sha, recorded_at) values (?, ?, ?)",
        &.{ .{ .int = session_a }, .{ .text = "a-newer" }, .{ .text = "2026-06-10T00:00:01.000Z" } },
    );
    _ = try d.execParams(
        "insert into session_commits (session_id, sha, recorded_at) values (?, ?, ?)",
        &.{ .{ .int = session_b }, .{ .text = "b-only" }, .{ .text = "2026-06-10T00:00:02.000Z" } },
    );

    const rows = try listForSessions(&d, a, &.{ session_b, session_a }, null);
    defer Row.deinitMany(rows, a);

    try std.testing.expectEqual(@as(usize, 3), rows.len);
    try std.testing.expectEqual(session_b, rows[0].session_id);
    try std.testing.expectEqualStrings("b-only", rows[0].sha);
    try std.testing.expectEqual(session_a, rows[1].session_id);
    try std.testing.expectEqualStrings("a-newer", rows[1].sha);
    try std.testing.expectEqualStrings("a-older", rows[2].sha);
}

test "listForSessions returns empty for an empty session set" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const rows = try listForSessions(&d, a, &.{}, null);
    defer Row.deinitMany(rows, a);

    try std.testing.expectEqual(@as(usize, 0), rows.len);
}

test "listForSessions applies the requested row cap" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const session_a = try insertTestSession(&d, "cap-a");
    const session_b = try insertTestSession(&d, "cap-b");

    _ = try d.execParams(
        "insert into session_commits (session_id, sha, recorded_at) values (?, ?, ?)",
        &.{ .{ .int = session_a }, .{ .text = "oldest" }, .{ .text = "2026-06-10T00:00:00.000Z" } },
    );
    _ = try d.execParams(
        "insert into session_commits (session_id, sha, recorded_at) values (?, ?, ?)",
        &.{ .{ .int = session_b }, .{ .text = "middle" }, .{ .text = "2026-06-10T00:00:01.000Z" } },
    );
    _ = try d.execParams(
        "insert into session_commits (session_id, sha, recorded_at) values (?, ?, ?)",
        &.{ .{ .int = session_a }, .{ .text = "newest" }, .{ .text = "2026-06-10T00:00:02.000Z" } },
    );

    const rows = try listForSessions(&d, a, &.{ session_a, session_b }, 2);
    defer Row.deinitMany(rows, a);

    try std.testing.expectEqual(@as(usize, 2), rows.len);
    try std.testing.expectEqualStrings("newest", rows[0].sha);
    try std.testing.expectEqualStrings("middle", rows[1].sha);
}

test "writeJsonList emits persisted row shape" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const session_id = try insertTestSession(&d, "json-test");
    try record(&d, session_id, null, &.{testCommit("sha-json", "/repo", "main", "json subject", "json author", "2026-06-10T00:00:00Z")});

    const rows = try listFiltered(&d, a, .{ .session_id = session_id });
    defer Row.deinitMany(rows, a);

    var out: std.Io.Writer.Allocating = .init(a);
    defer out.deinit();
    try writeJsonList(&out.writer, rows);

    try std.testing.expect(std.mem.startsWith(u8, out.written(), "[{\"id\":"));
    try std.testing.expect(std.mem.indexOf(u8, out.written(), "\"session_id\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.written(), "\"sha\":\"sha-json\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.written(), "\"recorded_at\":\"") != null);
}

test "recordClaimWindowBestEffort prefers worktree_path before repo_root" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const session_id = try insertTestSession(&d, "claim-window-worktree");
    const task_id = try insertTestTask(&d, "claim-window-task");
    const claim_id = try insertTestClaim(&d, session_id, task_id, "claim-window-worktree");

    const repo_root = try initFixtureRepo(a);
    defer a.free(repo_root);
    const worktree_repo = try initFixtureRepo(a);
    defer a.free(worktree_repo);

    const repo_root_base = try commitEmpty(a, repo_root, "root-base");
    defer a.free(repo_root_base);
    std.mem.doNotOptimizeAway(repo_root_base);
    const worktree_base = try commitEmpty(a, worktree_repo, "worktree-base");
    defer a.free(worktree_base);
    const worktree_head = try commitEmpty(a, worktree_repo, "worktree-target");
    defer a.free(worktree_head);

    recordClaimWindowBestEffort(&d, .{
        .allocator = a,
        .io = std.testing.io,
        .window = .{
            .claim_id = claim_id,
            .session_id = session_id,
            .worktree_path = worktree_repo,
            .repo_root = repo_root,
            .head_sha_at_claim = worktree_base,
        },
    });

    const rows = try listFiltered(&d, a, .{ .session_id = session_id });
    defer Row.deinitMany(rows, a);

    try std.testing.expectEqual(@as(usize, 1), rows.len);
    try std.testing.expectEqual(claim_id, rows[0].claim_id.?);
    try std.testing.expectEqualStrings(worktree_head, rows[0].sha);
}

test "recordClaimWindowBestEffort falls back to repo_root when worktree_path is missing" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const session_id = try insertTestSession(&d, "claim-window-fallback");
    const task_id = try insertTestTask(&d, "claim-window-task");
    const claim_id = try insertTestClaim(&d, session_id, task_id, "claim-window-fallback");

    const repo = try initFixtureRepo(a);
    defer a.free(repo);

    const base = try commitEmpty(a, repo, "base");
    defer a.free(base);
    const head = try commitEmpty(a, repo, "fallback-target");
    defer a.free(head);

    recordClaimWindowBestEffort(&d, .{
        .allocator = a,
        .io = std.testing.io,
        .window = .{
            .claim_id = claim_id,
            .session_id = session_id,
            .worktree_path = "/nonexistent/planar-session-commits-worktree",
            .repo_root = repo,
            .head_sha_at_claim = base,
        },
    });

    const rows = try listFiltered(&d, a, .{ .session_id = session_id });
    defer Row.deinitMany(rows, a);

    try std.testing.expectEqual(@as(usize, 1), rows.len);
    try std.testing.expectEqualStrings(head, rows[0].sha);
}

test "recordClaimWindowBestEffort does not fall back when worktree window is valid but empty" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const session_id = try insertTestSession(&d, "claim-window-empty-worktree");
    const task_id = try insertTestTask(&d, "claim-window-task");
    const claim_id = try insertTestClaim(&d, session_id, task_id, "claim-window-empty-worktree");

    const repo_root = try initFixtureRepo(a);
    defer a.free(repo_root);

    const base = try commitEmpty(a, repo_root, "shared-base");
    defer a.free(base);
    const worktree_repo = try freshSiblingRepoPath(a, "worktree-clone");
    defer a.free(worktree_repo);
    try gitClone(a, repo_root, worktree_repo);

    const root_head = try commitEmpty(a, repo_root, "repo-root-only");
    defer a.free(root_head);

    recordClaimWindowBestEffort(&d, .{
        .allocator = a,
        .io = std.testing.io,
        .window = .{
            .claim_id = claim_id,
            .session_id = session_id,
            .worktree_path = worktree_repo,
            .repo_root = repo_root,
            .head_sha_at_claim = base,
        },
    });

    const rows = try listFiltered(&d, a, .{ .session_id = session_id });
    defer Row.deinitMany(rows, a);

    try std.testing.expectEqual(@as(usize, 0), rows.len);
}

test "recordClaimWindowBestEffort honors no_locality_probe" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const session_id = try insertTestSession(&d, "claim-window-skipped");
    const task_id = try insertTestTask(&d, "claim-window-task");
    const claim_id = try insertTestClaim(&d, session_id, task_id, "claim-window-skipped");

    const repo = try initFixtureRepo(a);
    defer a.free(repo);

    const base = try commitEmpty(a, repo, "base");
    defer a.free(base);
    const head = try commitEmpty(a, repo, "should-not-record");
    defer a.free(head);
    std.mem.doNotOptimizeAway(head);

    recordClaimWindowBestEffort(&d, .{
        .allocator = a,
        .io = std.testing.io,
        .no_locality_probe = true,
        .window = .{
            .claim_id = claim_id,
            .session_id = session_id,
            .worktree_path = repo,
            .repo_root = repo,
            .head_sha_at_claim = base,
        },
    });

    const rows = try listFiltered(&d, a, .{ .session_id = session_id });
    defer Row.deinitMany(rows, a);

    try std.testing.expectEqual(@as(usize, 0), rows.len);
}
