//! engine/runs/harvest — ground-truth touch harvesting.
//!
//! At fan-in the measurement rig records what a cycle *actually* changed,
//! versus what the closure extractor *declared* it would change. This
//! module produces the `actual` side: it shells `git diff --name-only`
//! against a worktree/branch, parses the path set, and writes one
//! `run_touches kind='actual'` row per path for a given (run, task) via
//! the runs lifecycle module.
//!
//! Boundary: this is path-level harvest (cheap, ships now). Symbol-level
//! harvest is a later refinement gated on whether path-level precision is
//! already adequate (run-record-schema.md §4). No LLM, no agent spawn —
//! just git via std.process.run + an engine insert.

const std = @import("std");
const db = @import("db");
const runs = @import("runs.zig");

pub const Error = error{
    /// git was not runnable, exited non-zero, or the directory is not a
    /// git worktree.
    GitFailed,
} || runs.Error;

/// What `git diff` is computed against. `working_tree` diffs the worktree
/// against HEAD (ALL uncommitted edits — both staged and unstaged, since
/// the harvest cares about the full set of changes a cycle made, not the
/// staging distinction). `range` diffs `base..head` (a committed cycle
/// branch against its base SHA) — the normal fan-in case.
pub const DiffSpec = union(enum) {
    /// `git diff --name-only HEAD` (worktree+index vs HEAD).
    working_tree,
    /// `git diff --name-only <base>..<head>`.
    range: struct { base: []const u8, head: []const u8 },
};

pub const HarvestArgs = struct {
    /// Absolute path to the git worktree to diff.
    worktree: []const u8,
    run_id: i64,
    task_id: i64,
    spec: DiffSpec = .working_tree,
};

/// Run `git diff --name-only` against `args.worktree` per `args.spec`,
/// then insert one `run_touches kind='actual'` row per distinct path.
/// Returns the number of touch rows written.
///
/// Paths are de-duplicated before insertion so a path that appears once
/// in the diff yields exactly one row (the UNIQUE constraint on
/// (run, task, path, kind) would otherwise reject a repeat). Empty diff
/// → zero rows, no error.
pub fn harvest(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    io: std.Io,
    args: HarvestArgs,
) Error!usize {
    const paths = try diffPaths(allocator, io, args.worktree, args.spec);
    defer {
        for (paths) |p| allocator.free(p);
        allocator.free(paths);
    }

    for (paths) |p| {
        _ = try runs.touch(d, args.run_id, args.task_id, p, .actual);
    }
    return paths.len;
}

/// Run `git diff --name-only` and return the de-duplicated, order-stable
/// set of changed paths (each an owned slice). Exposed separately so a
/// caller can inspect the path set without writing rows (and so tests can
/// assert the parse independently of the DB insert).
pub fn diffPaths(
    allocator: std.mem.Allocator,
    io: std.Io,
    worktree: []const u8,
    spec: DiffSpec,
) Error![][]const u8 {
    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(allocator);
    try argv.append(allocator, "git");
    try argv.append(allocator, "-C");
    try argv.append(allocator, worktree);
    try argv.append(allocator, "diff");
    try argv.append(allocator, "--name-only");

    // `range` needs an owned "base..head" buffer that outlives the run call.
    var range_buf: ?[]u8 = null;
    defer if (range_buf) |b| allocator.free(b);
    switch (spec) {
        // HEAD captures both staged and unstaged changes against the last
        // commit — the full uncommitted change set, not just unstaged.
        .working_tree => try argv.append(allocator, "HEAD"),
        .range => |r| {
            const buf = try std.fmt.allocPrint(allocator, "{s}..{s}", .{ r.base, r.head });
            range_buf = buf;
            try argv.append(allocator, buf);
        },
    }

    const raw = try runGit(allocator, io, argv.items);
    defer allocator.free(raw);

    return parsePaths(allocator, raw);
}

// =========================================================================
// Internals
// =========================================================================

/// Run git, returning owned stdout on a clean exit; GitFailed otherwise.
fn runGit(allocator: std.mem.Allocator, io: std.Io, argv: []const []const u8) Error![]u8 {
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

/// Split newline-delimited `git diff --name-only` output into a
/// de-duplicated, order-stable slice of owned path strings. Blank lines
/// and trailing whitespace are dropped.
fn parsePaths(allocator: std.mem.Allocator, raw: []const u8) Error![][]const u8 {
    var seen: std.StringHashMapUnmanaged(void) = .{};
    defer seen.deinit(allocator);

    var out: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (out.items) |p| allocator.free(p);
        out.deinit(allocator);
    }

    var it = std.mem.splitScalar(u8, raw, '\n');
    while (it.next()) |line| {
        const path = std.mem.trim(u8, line, " \t\r");
        if (path.len == 0) continue;
        if (seen.contains(path)) continue;

        const owned = try allocator.dupe(u8, path);
        errdefer allocator.free(owned);
        try seen.put(allocator, owned, {});
        try out.append(allocator, owned);
    }

    return out.toOwnedSlice(allocator);
}

// =========================================================================
// Tests
// =========================================================================

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

fn gitMust(allocator: std.mem.Allocator, dir: []const u8, args: []const []const u8) ![]u8 {
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

fn gitTrim(allocator: std.mem.Allocator, dir: []const u8, args: []const []const u8) ![]const u8 {
    const raw = try gitMust(allocator, dir, args);
    defer allocator.free(raw);
    return allocator.dupe(u8, std.mem.trim(u8, raw, " \t\r\n"));
}

/// Write `content` to `dir_abs/rel` (rel must be a flat filename — these
/// fixtures avoid subdirectories so no parent-dir creation is needed).
fn writeFile(allocator: std.mem.Allocator, dir_abs: []const u8, rel: []const u8, content: []const u8) !void {
    const full = try std.fs.path.join(allocator, &.{ dir_abs, rel });
    defer allocator.free(full);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = full, .data = content });
}

/// Stand up a throwaway git repo with an initial commit. Returns its
/// absolute root path (owned).
fn initFixtureRepo(allocator: std.mem.Allocator) ![]const u8 {
    try ensureGitAvailable(allocator);

    var tmp = std.testing.tmpDir(.{});
    errdefer tmp.cleanup();

    const root = try tmpRootPath(allocator, &tmp);
    errdefer allocator.free(root);

    inline for (.{
        &.{"init"},
        &.{ "checkout", "-b", "main" },
        &.{ "config", "user.email", "planar@example.com" },
        &.{ "config", "user.name", "Planar Test User" },
    }) |cmd| {
        const out = try gitMust(allocator, root, cmd);
        allocator.free(out);
    }

    std.mem.doNotOptimizeAway(tmp);
    return root;
}

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

fn seedRun(d: *db.sqlite.Db, allocator: std.mem.Allocator, run_uid: []const u8) !i64 {
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','harvest plan','harvest-plan','draft')",
        &.{},
    );
    const res = try runs.start(d, allocator, .{
        .run_uid = run_uid,
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "base",
        .config_hash = "h",
    });
    defer res.deinit(allocator);
    return res.id;
}

test "parsePaths de-duplicates and drops blank lines" {
    const a = std.testing.allocator;
    const raw = "src/a.zig\nsrc/b.zig\n\nsrc/a.zig\n  \nsrc/c.zig\n";
    const paths = try parsePaths(a, raw);
    defer {
        for (paths) |p| a.free(p);
        a.free(paths);
    }
    try std.testing.expectEqual(@as(usize, 3), paths.len);
    try std.testing.expectEqualStrings("src/a.zig", paths[0]);
    try std.testing.expectEqualStrings("src/b.zig", paths[1]);
    try std.testing.expectEqualStrings("src/c.zig", paths[2]);
}

test "diffPaths: working_tree picks up uncommitted edits" {
    const a = std.testing.allocator;
    const repo = initFixtureRepo(a) catch |e| switch (e) {
        error.SkipZigTest => return error.SkipZigTest,
        else => return e,
    };
    defer a.free(repo);

    // Commit two files so we have a baseline HEAD.
    try writeFile(a, repo, "keep.txt", "original\n");
    try writeFile(a, repo, "edit.txt", "v1\n");
    {
        const add = try gitMust(a, repo, &.{ "add", "." });
        a.free(add);
        const commit = try gitMust(a, repo, &.{ "commit", "-m", "base" });
        a.free(commit);
    }

    // Modify one tracked file and add one new (staged) file.
    try writeFile(a, repo, "edit.txt", "v2\n");
    try writeFile(a, repo, "new.txt", "fresh\n");
    {
        const add = try gitMust(a, repo, &.{ "add", "new.txt" });
        a.free(add);
    }

    const paths = try diffPaths(a, std.testing.io, repo, .working_tree);
    defer {
        for (paths) |p| a.free(p);
        a.free(paths);
    }

    // `git diff --name-only` reports the modified tracked file and the
    // staged-but-new file (staged content differs from HEAD).
    try std.testing.expectEqual(@as(usize, 2), paths.len);
    var saw_edit = false;
    var saw_new = false;
    for (paths) |p| {
        if (std.mem.eql(u8, p, "edit.txt")) saw_edit = true;
        if (std.mem.eql(u8, p, "new.txt")) saw_new = true;
    }
    try std.testing.expect(saw_edit);
    try std.testing.expect(saw_new);
}

test "diffPaths: range diffs base..head across commits" {
    const a = std.testing.allocator;
    const repo = initFixtureRepo(a) catch |e| switch (e) {
        error.SkipZigTest => return error.SkipZigTest,
        else => return e,
    };
    defer a.free(repo);

    try writeFile(a, repo, "a.txt", "a\n");
    {
        const add = try gitMust(a, repo, &.{ "add", "." });
        a.free(add);
        const commit = try gitMust(a, repo, &.{ "commit", "-m", "base" });
        a.free(commit);
    }
    const base = try gitTrim(a, repo, &.{ "rev-parse", "HEAD" });
    defer a.free(base);

    // Two further commits on a cycle branch touching distinct files.
    try writeFile(a, repo, "b.txt", "b\n");
    {
        const add = try gitMust(a, repo, &.{ "add", "." });
        a.free(add);
        const commit = try gitMust(a, repo, &.{ "commit", "-m", "c1" });
        a.free(commit);
    }
    try writeFile(a, repo, "c.txt", "c\n");
    {
        const add = try gitMust(a, repo, &.{ "add", "." });
        a.free(add);
        const commit = try gitMust(a, repo, &.{ "commit", "-m", "c2" });
        a.free(commit);
    }
    const head = try gitTrim(a, repo, &.{ "rev-parse", "HEAD" });
    defer a.free(head);

    const paths = try diffPaths(a, std.testing.io, repo, .{ .range = .{ .base = base, .head = head } });
    defer {
        for (paths) |p| a.free(p);
        a.free(paths);
    }
    try std.testing.expectEqual(@as(usize, 2), paths.len);
    var saw_b = false;
    var saw_c = false;
    for (paths) |p| {
        if (std.mem.eql(u8, p, "b.txt")) saw_b = true;
        if (std.mem.eql(u8, p, "c.txt")) saw_c = true;
    }
    try std.testing.expect(saw_b);
    try std.testing.expect(saw_c);
}

test "harvest writes run_touches kind='actual' for the diff set" {
    const a = std.testing.allocator;
    const repo = initFixtureRepo(a) catch |e| switch (e) {
        error.SkipZigTest => return error.SkipZigTest,
        else => return e,
    };
    defer a.free(repo);

    try writeFile(a, repo, "x.txt", "1\n");
    {
        const add = try gitMust(a, repo, &.{ "add", "." });
        a.free(add);
        const commit = try gitMust(a, repo, &.{ "commit", "-m", "base" });
        a.free(commit);
    }
    // Uncommitted edits: one modified, one new+staged.
    try writeFile(a, repo, "x.txt", "2\n");
    try writeFile(a, repo, "y.txt", "new\n");
    {
        const add = try gitMust(a, repo, &.{ "add", "y.txt" });
        a.free(add);
    }

    var d = try setupTestDb(a);
    defer d.close();
    const run_id = try seedRun(&d, a, "uid-harvest");

    const n = try harvest(&d, a, std.testing.io, .{
        .worktree = repo,
        .run_id = run_id,
        .task_id = 42,
        .spec = .working_tree,
    });
    try std.testing.expectEqual(@as(usize, 2), n);

    const actual = try runs.touches(&d, a, run_id, .actual);
    defer runs.deinitTouches(actual, a);
    try std.testing.expectEqual(@as(usize, 2), actual.len);
    for (actual) |t| {
        try std.testing.expectEqual(@as(i64, 42), t.task_id);
        try std.testing.expectEqual(runs.TouchKind.actual, t.kind);
    }

    // No declared rows were written by harvest.
    const declared = try runs.touches(&d, a, run_id, .declared);
    defer runs.deinitTouches(declared, a);
    try std.testing.expectEqual(@as(usize, 0), declared.len);
}

test "harvest on a clean worktree writes zero rows" {
    const a = std.testing.allocator;
    const repo = initFixtureRepo(a) catch |e| switch (e) {
        error.SkipZigTest => return error.SkipZigTest,
        else => return e,
    };
    defer a.free(repo);

    try writeFile(a, repo, "z.txt", "1\n");
    {
        const add = try gitMust(a, repo, &.{ "add", "." });
        a.free(add);
        const commit = try gitMust(a, repo, &.{ "commit", "-m", "base" });
        a.free(commit);
    }

    var d = try setupTestDb(a);
    defer d.close();
    const run_id = try seedRun(&d, a, "uid-clean");

    const n = try harvest(&d, a, std.testing.io, .{
        .worktree = repo,
        .run_id = run_id,
        .task_id = 7,
        .spec = .working_tree,
    });
    try std.testing.expectEqual(@as(usize, 0), n);
}

test "harvest against a nonexistent worktree returns GitFailed" {
    // Pointing `git -C` at a path that does not exist reliably makes git
    // exit non-zero regardless of the test environment's repo nesting
    // (the harness tmp tree lives inside the planar checkout, so an
    // *existing* empty dir would resolve up to the enclosing repo). A
    // nonexistent path is the unambiguous failure case.
    const a = std.testing.allocator;
    try ensureGitAvailable(a);

    var d = try setupTestDb(a);
    defer d.close();
    const run_id = try seedRun(&d, a, "uid-nogit");

    try std.testing.expectError(Error.GitFailed, harvest(&d, a, std.testing.io, .{
        .worktree = "/nonexistent/planar-harvest/path/xyzzy",
        .run_id = run_id,
        .task_id = 1,
        .spec = .working_tree,
    }));
}
