//! engine/runtime/capture — orchestrator that ties sessions, entries,
//! and snapshots into the operator-facing `capture` verbs.
//!
//! The underlying primitives live in `session.zig` and `snapshot.zig`.
//! This module groups them so handlers have a small, intention-named
//! API: `openSession`, `closeSession`, `appendNote`, `appendCommand`,
//! `appendFile`, `takeSnapshot`.

const std = @import("std");
const db = @import("db");
const session_mod = @import("session.zig");
const session_commits = @import("sessioncommits.zig");
const snapshot_mod = @import("snapshot.zig");

var captureTestCwdMutex: std.atomic.Mutex = .unlocked;

fn lockCaptureTestCwd() void {
    while (!captureTestCwdMutex.tryLock()) {
        std.Thread.yield() catch {};
    }
}

pub const Error = session_mod.Error || snapshot_mod.Error || session_commits.Error || error{NoActiveSession};

pub const Session = session_mod.Session;
pub const Snapshot = snapshot_mod.Snapshot;

pub const OpenArgs = struct {
    vendor: []const u8,
    vendor_session_id: ?[]const u8 = null,
    task_id: ?i64 = null,
    model: ?[]const u8 = null,
};

/// Open or reuse a session and emit a session-start entry. Mirrors the
/// Go `runCaptureSession` glue (calls StartSession + AppendEntry).
pub fn openSession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    io: std.Io,
    args: OpenArgs,
) Error!Session {
    var s = try session_mod.startSession(d, allocator, .{
        .vendor = args.vendor,
        .vendor_session_id = args.vendor_session_id,
        .task_id = args.task_id,
        .model = args.model,
    });
    errdefer session_mod.deinit(s, allocator);

    if (probeStartGitContext(allocator, io)) |ctx| {
        defer ctx.deinit(allocator);
        session_mod.setStartGitContextIfUnset(d, s.id, ctx.repo_root, ctx.head_sha_at_start) catch {};
        session_mod.deinit(s, allocator);
        s = try session_mod.getById(d, allocator, s.id);
    }

    // Best-effort marker; ignore failure so session creation isn't lost
    // if the entry insert hiccups (matches Go behavior).
    session_mod.appendEntry(d, s.id, "action", "session opened") catch {};
    return s;
}

pub const CloseArgs = struct {
    /// Explicit session id; when null, the caller must resolve the
    /// active session externally and pass it in. Centralizing
    /// vendor-env lookup is the handler's job (it owns `process.Environ`).
    session_id: i64,
    summary: ?[]const u8 = null,
};

/// Close the named session. Appends a "session ended" note before flipping
/// ended_at so the timeline records the boundary.
pub fn closeSession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    io: std.Io,
    args: CloseArgs,
) Error!void {
    try recordSessionWindow(d, allocator, io, args.session_id);
    session_mod.appendEntry(d, args.session_id, "note", "session ended") catch {};
    try session_mod.endSession(d, allocator, args.session_id, args.summary);
}

pub fn appendNote(
    d: *db.sqlite.Db,
    session_id: i64,
    body: []const u8,
) Error!void {
    try session_mod.appendEntry(d, session_id, "note", body);
}

pub fn appendCommand(
    d: *db.sqlite.Db,
    session_id: i64,
    body: []const u8,
) Error!void {
    try session_mod.appendEntry(d, session_id, "command", body);
}

pub fn appendFile(
    d: *db.sqlite.Db,
    session_id: i64,
    body: []const u8,
) Error!void {
    try session_mod.appendEntry(d, session_id, "file", body);
}

pub const SnapshotArgs = struct {
    session_id: i64,
    task_id: ?i64 = null,
    vendor: []const u8,
    vendor_session_id: ?[]const u8 = null,
    body: ?[]const u8 = null,
    next_action: ?[]const u8 = null,
};

/// Create a context_snapshots row and emit a "snapshot created" note in
/// the same session timeline.
pub fn takeSnapshot(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: SnapshotArgs,
) Error!Snapshot {
    const snap = try snapshot_mod.create(d, allocator, .{
        .session_id = args.session_id,
        .task_id = args.task_id,
        .vendor = args.vendor,
        .vendor_session_id = args.vendor_session_id,
        .body = args.body,
        .next_action = args.next_action,
    });
    const note = std.fmt.allocPrint(
        allocator,
        "snapshot created: id={d}",
        .{snap.id},
    ) catch return snap;
    defer allocator.free(note);
    session_mod.appendEntry(d, args.session_id, "note", note) catch {};
    return snap;
}

pub const RecordCommitsArgs = struct {
    session_id: i64,
    repo_dir: []const u8,
    since: ?[]const u8 = null,
    shas: []const []const u8 = &.{},
};

pub const RecordCommitsResult = struct {
    session_id: i64,
    repo_root: []const u8,
    commit_count: usize,
    inserted_count: usize,

    pub fn deinit(self: RecordCommitsResult, allocator: std.mem.Allocator) void {
        allocator.free(self.repo_root);
    }
};

/// Record an explicit set of commits into a session. This is the loud-fail
/// operator path used by `planar capture commits`.
pub fn recordCommits(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    io: std.Io,
    args: RecordCommitsArgs,
) Error!RecordCommitsResult {
    const s = try session_mod.getById(d, allocator, args.session_id);
    defer session_mod.deinit(s, allocator);

    const repo_root = try session_commits.resolveRepoRootStrict(allocator, io, args.repo_dir);
    errdefer allocator.free(repo_root);

    const commits = if (args.since) |base_sha|
        try session_commits.walkStrict(.{
            .allocator = allocator,
            .io = io,
            .dir = args.repo_dir,
            .base_sha = base_sha,
            .repo_root = repo_root,
        })
    else
        try session_commits.resolveShas(.{
            .allocator = allocator,
            .io = io,
            .dir = args.repo_dir,
            .shas = args.shas,
            .repo_root = repo_root,
        });
    defer session_commits.deinitMany(commits, allocator);

    d.exec("begin immediate") catch return Error.QueryFailed;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    const inserted_count = session_commits.recordCount(d, s.id, null, commits) catch |err| switch (err) {
        error.OutOfMemory => return error.OutOfMemory,
        error.QueryFailed => return Error.QueryFailed,
        else => return Error.QueryFailed,
    };
    d.exec("commit") catch return Error.QueryFailed;
    committed = true;

    return .{
        .session_id = s.id,
        .repo_root = repo_root,
        .commit_count = commits.len,
        .inserted_count = inserted_count,
    };
}

const StartGitContext = struct {
    repo_root: []const u8,
    head_sha_at_start: []const u8,

    fn deinit(self: StartGitContext, allocator: std.mem.Allocator) void {
        allocator.free(self.repo_root);
        allocator.free(self.head_sha_at_start);
    }
};

fn probeStartGitContext(
    allocator: std.mem.Allocator,
    io: std.Io,
) ?StartGitContext {
    const cwd = std.Io.Dir.realPathFileAlloc(.cwd(), io, ".", allocator) catch return null;
    defer allocator.free(cwd);

    const repo_root = gitTrim(allocator, io, cwd, &.{ "rev-parse", "--show-toplevel" }) orelse return null;
    errdefer allocator.free(repo_root);

    const head_sha = gitTrim(allocator, io, cwd, &.{ "rev-parse", "HEAD" }) orelse return null;
    errdefer allocator.free(head_sha);

    return .{
        .repo_root = repo_root,
        .head_sha_at_start = head_sha,
    };
}

fn gitTrim(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: []const u8,
    argv_extra: []const []const u8,
) ?[]u8 {
    var argv: [8][]const u8 = undefined;
    if (3 + argv_extra.len > argv.len) return null;
    argv[0] = "git";
    argv[1] = "-C";
    argv[2] = dir;
    for (argv_extra, 0..) |arg, index| argv[3 + index] = arg;

    const result = std.process.run(allocator, io, .{
        .argv = argv[0 .. 3 + argv_extra.len],
    }) catch return null;
    defer allocator.free(result.stderr);
    defer allocator.free(result.stdout);

    switch (result.term) {
        .exited => |code| {
            if (code != 0) return null;
        },
        else => return null,
    }

    const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
    if (trimmed.len == 0) return null;
    return allocator.dupe(u8, trimmed) catch null;
}

fn recordSessionWindow(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    io: std.Io,
    session_id: i64,
) Error!void {
    const s = try session_mod.getById(d, allocator, session_id);
    defer session_mod.deinit(s, allocator);

    const repo_root = s.repo_root orelse return;
    const base_sha = s.head_sha_at_start orelse return;

    const commits = try session_commits.walk(.{
        .allocator = allocator,
        .io = io,
        .dir = repo_root,
        .base_sha = base_sha,
        .repo_root = repo_root,
    });
    defer session_commits.deinitMany(commits, allocator);
    if (commits.len == 0) return;

    d.exec("begin immediate") catch return Error.QueryFailed;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    session_commits.record(d, session_id, null, commits) catch |err| switch (err) {
        error.OutOfMemory => return error.OutOfMemory,
        error.QueryFailed => return Error.QueryFailed,
        else => return Error.QueryFailed,
    };
    d.exec("commit") catch return Error.QueryFailed;
    committed = true;
}

// =========================================================================
// Tests
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "openSession appends a session-opened action entry" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try openSession(&d, a, std.testing.io, .{ .vendor = "v" });
    defer session_mod.deinit(s, a);
    const entries = try session_mod.listEntriesForSession(&d, a, s.id);
    defer session_mod.deinitEntries(entries, a);
    try std.testing.expectEqual(@as(usize, 1), entries.len);
    try std.testing.expectEqualStrings("action", entries[0].prefix);
    try std.testing.expectEqualStrings("session opened", entries[0].body);
}

test "closeSession appends note + ends session" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try openSession(&d, a, std.testing.io, .{ .vendor = "v" });
    defer session_mod.deinit(s, a);
    try closeSession(&d, a, std.testing.io, .{ .session_id = s.id, .summary = "done" });
    const after = try session_mod.getById(&d, a, s.id);
    defer session_mod.deinit(after, a);
    try std.testing.expect(after.ended_at != null);
    try std.testing.expectEqualStrings("done", after.summary.?);
}

test "closeSession records commits before ending an operator session" {
    lockCaptureTestCwd();
    defer captureTestCwdMutex.unlock();

    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const repo = try initFixtureRepo(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    const expected_repo_root = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, repo, a);
    defer a.free(expected_repo_root);

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", a);
    defer a.free(prev_cwd);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    try std.process.setCurrentPath(std.testing.io, repo);

    const s = try openSession(&d, a, std.testing.io, .{ .vendor = "v", .vendor_session_id = "close-git-session" });
    defer session_mod.deinit(s, a);

    const commit_sha = try commitFixtureFile(a, repo, "tracked.txt", "tracked\n", "tracked commit");
    defer a.free(commit_sha);

    try closeSession(&d, a, std.testing.io, .{ .session_id = s.id, .summary = "done" });

    const after = try session_mod.getById(&d, a, s.id);
    defer session_mod.deinit(after, a);
    try std.testing.expect(after.ended_at != null);

    const rows = try session_commits.listFiltered(&d, a, .{ .session_id = s.id });
    defer session_commits.Row.deinitMany(rows, a);
    try std.testing.expectEqual(@as(usize, 1), rows.len);
    try std.testing.expectEqual(rows[0].session_id, s.id);
    try std.testing.expect(rows[0].claim_id == null);
    try std.testing.expectEqualStrings(commit_sha, rows[0].sha);
    try std.testing.expectEqualStrings(expected_repo_root, rows[0].repo_root.?);
    try std.testing.expectEqualStrings("tracked commit", rows[0].subject.?);
}

test "closeSession degrades cleanly when start git metadata is missing" {
    lockCaptureTestCwd();
    defer captureTestCwdMutex.unlock();

    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const root = try freshSystemTmpDir(a, "planar-capture-end-nongit.XXXXXX");
    defer a.free(root);
    defer rmTree(a, root);

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", a);
    defer a.free(prev_cwd);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    try std.process.setCurrentPath(std.testing.io, root);

    const s = try openSession(&d, a, std.testing.io, .{ .vendor = "v", .vendor_session_id = "close-non-git-session" });
    defer session_mod.deinit(s, a);

    try closeSession(&d, a, std.testing.io, .{ .session_id = s.id, .summary = "done" });

    const rows = try session_commits.listFiltered(&d, a, .{ .session_id = s.id });
    defer session_commits.Row.deinitMany(rows, a);
    try std.testing.expectEqual(@as(usize, 0), rows.len);
}

test "recordCommits records a range into an ended session" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const repo = try initFixtureRepo(a);
    defer a.free(repo);
    defer rmTree(a, repo);

    const s = try openSession(&d, a, std.testing.io, .{ .vendor = "v", .vendor_session_id = "manual-ended-session" });
    defer session_mod.deinit(s, a);
    try closeSession(&d, a, std.testing.io, .{ .session_id = s.id, .summary = "done" });

    const base = try gitOutputTrim(a, repo, &.{ "rev-parse", "HEAD~0" });
    defer a.free(base);
    const commit_sha = try commitFixtureFile(a, repo, "late.txt", "late\n", "late commit");
    defer a.free(commit_sha);

    var result = try recordCommits(&d, a, std.testing.io, .{
        .session_id = s.id,
        .repo_dir = repo,
        .since = base,
    });
    defer result.deinit(a);

    try std.testing.expectEqual(@as(usize, 1), result.commit_count);
    try std.testing.expectEqual(@as(usize, 1), result.inserted_count);

    const rows = try session_commits.listFiltered(&d, a, .{ .session_id = s.id });
    defer session_commits.Row.deinitMany(rows, a);
    try std.testing.expectEqual(@as(usize, 1), rows.len);
    try std.testing.expectEqualStrings(commit_sha, rows[0].sha);
    try std.testing.expectEqualStrings("late commit", rows[0].subject.?);
}

test "recordCommits surfaces non-git repos and bad refs" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const repo = try initFixtureRepo(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    const nongit = try freshSystemTmpDir(a, "planar-capture-commits-nongit.XXXXXX");
    defer a.free(nongit);
    defer rmTree(a, nongit);

    const s = try openSession(&d, a, std.testing.io, .{ .vendor = "v", .vendor_session_id = "manual-errors-session" });
    defer session_mod.deinit(s, a);

    try std.testing.expectError(error.NotGit, recordCommits(&d, a, std.testing.io, .{
        .session_id = s.id,
        .repo_dir = nongit,
        .since = "HEAD",
    }));

    try std.testing.expectError(error.GitFailed, recordCommits(&d, a, std.testing.io, .{
        .session_id = s.id,
        .repo_dir = repo,
        .since = "not-a-ref",
    }));
}

test "appendNote / appendCommand / appendFile use the right prefixes" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try openSession(&d, a, std.testing.io, .{ .vendor = "v" });
    defer session_mod.deinit(s, a);
    try appendNote(&d, s.id, "n");
    try appendCommand(&d, s.id, "c");
    try appendFile(&d, s.id, "f");
    const entries = try session_mod.listEntriesForSession(&d, a, s.id);
    defer session_mod.deinitEntries(entries, a);
    // 1: opened action, 2: note, 3: command, 4: file
    try std.testing.expectEqual(@as(usize, 4), entries.len);
    try std.testing.expectEqualStrings("note", entries[1].prefix);
    try std.testing.expectEqualStrings("command", entries[2].prefix);
    try std.testing.expectEqualStrings("file", entries[3].prefix);
}

test "takeSnapshot creates row and emits note entry" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try openSession(&d, a, std.testing.io, .{ .vendor = "v" });
    defer session_mod.deinit(s, a);
    const snap = try takeSnapshot(&d, a, .{
        .session_id = s.id,
        .vendor = "v",
        .body = "the body",
        .next_action = "do it",
    });
    defer snapshot_mod.deinit(snap, a);
    try std.testing.expectEqualStrings("the body", snap.body);

    const entries = try session_mod.listEntriesForSession(&d, a, s.id);
    defer session_mod.deinitEntries(entries, a);
    // 1: opened, 2: snapshot created note
    try std.testing.expectEqual(@as(usize, 2), entries.len);
    try std.testing.expectEqualStrings("note", entries[1].prefix);
    try std.testing.expect(std.mem.startsWith(u8, entries[1].body, "snapshot created"));
}

test "openSession records repo_root and head_sha_at_start from a git cwd" {
    lockCaptureTestCwd();
    defer captureTestCwdMutex.unlock();

    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const repo = try initFixtureRepo(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    const expected_repo_root = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, repo, a);
    defer a.free(expected_repo_root);
    const expected_head = try gitOutputTrim(a, repo, &.{ "rev-parse", "HEAD" });
    defer a.free(expected_head);

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", a);
    defer a.free(prev_cwd);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    try std.process.setCurrentPath(std.testing.io, repo);

    const s = try openSession(&d, a, std.testing.io, .{ .vendor = "v", .vendor_session_id = "git-session" });
    defer session_mod.deinit(s, a);

    try std.testing.expectEqualStrings(expected_repo_root, s.repo_root.?);
    try std.testing.expectEqualStrings(expected_head, s.head_sha_at_start.?);
}

test "openSession leaves start metadata null outside git and does not fail" {
    lockCaptureTestCwd();
    defer captureTestCwdMutex.unlock();

    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const root = try freshSystemTmpDir(a, "planar-capture-nongit.XXXXXX");
    defer a.free(root);
    defer rmTree(a, root);

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", a);
    defer a.free(prev_cwd);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};
    try std.process.setCurrentPath(std.testing.io, root);

    const s = try openSession(&d, a, std.testing.io, .{ .vendor = "v", .vendor_session_id = "non-git-session" });
    defer session_mod.deinit(s, a);

    try std.testing.expect(s.repo_root == null);
    try std.testing.expect(s.head_sha_at_start == null);
}

test "openSession preserves first-open git metadata when reusing a session" {
    lockCaptureTestCwd();
    defer captureTestCwdMutex.unlock();

    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const repo_a = try initFixtureRepo(a);
    defer a.free(repo_a);
    defer rmTree(a, repo_a);
    const repo_b = try initFixtureRepo(a);
    defer a.free(repo_b);
    defer rmTree(a, repo_b);

    const expected_repo_root = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, repo_a, a);
    defer a.free(expected_repo_root);
    const expected_head = try gitOutputTrim(a, repo_a, &.{ "rev-parse", "HEAD" });
    defer a.free(expected_head);

    const prev_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", a);
    defer a.free(prev_cwd);
    defer std.process.setCurrentPath(std.testing.io, prev_cwd) catch {};

    try std.process.setCurrentPath(std.testing.io, repo_a);
    const first = try openSession(&d, a, std.testing.io, .{ .vendor = "v", .vendor_session_id = "reuse-session" });
    defer session_mod.deinit(first, a);

    try std.process.setCurrentPath(std.testing.io, repo_b);
    const second = try openSession(&d, a, std.testing.io, .{ .vendor = "v", .vendor_session_id = "reuse-session" });
    defer session_mod.deinit(second, a);

    try std.testing.expectEqual(first.id, second.id);
    try std.testing.expectEqualStrings(expected_repo_root, second.repo_root.?);
    try std.testing.expectEqualStrings(expected_head, second.head_sha_at_start.?);
}

fn initFixtureRepo(allocator: std.mem.Allocator) ![]u8 {
    try ensureGitAvailable(allocator);
    const root = try freshSystemTmpDir(allocator, "planar-capture-repo.XXXXXX");
    errdefer allocator.free(root);

    try runCommandDiscard(allocator, &.{ "git", "init", root });
    try runCommandInDirDiscard(allocator, root, &.{ "git", "config", "user.email", "planar-test@example.com" });
    try runCommandInDirDiscard(allocator, root, &.{ "git", "config", "user.name", "Planar Test" });
    try runCommandInDirDiscard(allocator, root, &.{ "git", "branch", "-m", "main" });
    const readme_path = try std.fs.path.join(allocator, &.{ root, "README.md" });
    defer allocator.free(readme_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = readme_path,
        .data = "seed\n",
    });
    try runCommandInDirDiscard(allocator, root, &.{ "git", "add", "README.md" });
    try runCommandInDirDiscard(allocator, root, &.{ "git", "commit", "-m", "seed" });
    return root;
}

fn commitFixtureFile(
    allocator: std.mem.Allocator,
    repo_root: []const u8,
    rel_path: []const u8,
    contents: []const u8,
    subject: []const u8,
) ![]u8 {
    const full_path = try std.fs.path.join(allocator, &.{ repo_root, rel_path });
    defer allocator.free(full_path);

    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = full_path,
        .data = contents,
    });
    try runCommandInDirDiscard(allocator, repo_root, &.{ "git", "add", rel_path });
    try runCommandInDirDiscard(allocator, repo_root, &.{ "git", "commit", "-m", subject });
    return gitOutputTrim(allocator, repo_root, &.{ "rev-parse", "HEAD" });
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

fn freshSystemTmpDir(allocator: std.mem.Allocator, template: []const u8) ![]u8 {
    const result = try std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "mktemp", "-d", "-t", template },
    });
    defer allocator.free(result.stderr);
    errdefer allocator.free(result.stdout);
    switch (result.term) {
        .exited => |code| if (code != 0) return error.CommandFailed,
        else => return error.CommandFailed,
    }
    const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
    const owned = try allocator.dupe(u8, trimmed);
    allocator.free(result.stdout);
    return owned;
}

fn rmTree(allocator: std.mem.Allocator, path: []const u8) void {
    const result = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "rm", "-rf", path },
    }) catch return;
    allocator.free(result.stdout);
    allocator.free(result.stderr);
}

fn runCommandDiscard(allocator: std.mem.Allocator, argv: []const []const u8) !void {
    const result = try std.process.run(allocator, std.testing.io, .{ .argv = argv });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);
    switch (result.term) {
        .exited => |code| if (code != 0) return error.CommandFailed,
        else => return error.CommandFailed,
    }
}

fn runCommandInDirDiscard(allocator: std.mem.Allocator, dir: []const u8, argv: []const []const u8) !void {
    const result = try std.process.run(allocator, std.testing.io, .{
        .argv = argv,
        .cwd = .{ .path = dir },
    });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);
    switch (result.term) {
        .exited => |code| if (code != 0) return error.CommandFailed,
        else => return error.CommandFailed,
    }
}

fn gitOutputTrim(allocator: std.mem.Allocator, dir: []const u8, argv_extra: []const []const u8) ![]u8 {
    var argv: [8][]const u8 = undefined;
    argv[0] = "git";
    argv[1] = "-C";
    argv[2] = dir;
    for (argv_extra, 0..) |arg, index| argv[3 + index] = arg;

    const result = try std.process.run(allocator, std.testing.io, .{
        .argv = argv[0 .. 3 + argv_extra.len],
    });
    defer allocator.free(result.stderr);
    errdefer allocator.free(result.stdout);
    switch (result.term) {
        .exited => |code| if (code != 0) return error.CommandFailed,
        else => return error.CommandFailed,
    }

    const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
    const owned = try allocator.dupe(u8, trimmed);
    allocator.free(result.stdout);
    return owned;
}
