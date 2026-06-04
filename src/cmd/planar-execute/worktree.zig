//! worktree.zig — Worktree lifecycle manager for `planar-execute`.
//!
//! Drives the harness's per-task worktree topology by shelling `git` via
//! `std.process.run`. It creates the once-per-plan epic (integration) worktree,
//! a per-task cycle worktree cut from the epic branch, and tears each cycle down
//! on both the success and abort paths. It links nothing from git as a library —
//! every operation is a `git` subprocess whose output is parsed (or whose exit
//! code is mapped) into the module-local `WorktreeError` set, mirroring
//! `state.zig:spawnPlanar` and `schema.zig:spawnBin`.
//!
//! ## Capability boundary
//!
//! This module holds NO SQLite handle and imports NO db/engine/runtime module.
//! Like the other `planar-execute` driver modules it is a pure process driver.
//!
//! ## Topology contract (authoritative — Appendix § "Worktree mechanics")
//!
//! Both worktree shapes are rooted at the TASK'S OWNING repo (`repo_root`,
//! passed in — NOT assumed to be cwd; the operator's cwd repo and the task's
//! repo may differ in a polyrepo workspace):
//!
//! | Worktree | Path                                            | Branch                        | Cut from           |
//! |----------|-------------------------------------------------|-------------------------------|--------------------|
//! | Epic     | `<repo>/.worktrees/epic/<plan-slug>/`           | `epic/<plan-slug>`            | master             |
//! | Cycle    | `<repo>/.worktrees/cycle/<plan-slug>/<task>/`   | `cycle/<plan-slug>/<task>`    | `epic/<plan-slug>` |
//!
//! The `epic/` and `cycle/` ref prefixes are load-bearing: git refuses any ref
//! whose path is a strict prefix of another existing ref, so a bare
//! `<plan-slug>` + `<plan-slug>/<task-slug>` pairing would collide. The prefixed
//! forms keep the two namespaces disjoint.
//!
//! The epic branch is cut from `master` (the contract uses `master`, not
//! `main`); the cycle branch is cut from `epic/<plan-slug>`, NOT master.
//!
//! ## Teardown on BOTH success and abort
//!
//! `teardownCycle` is an explicit caller-invoked verb (NOT a success-only
//! `defer`) so a failed or aborted cycle can be cleaned up too. It runs
//! `git worktree remove --force` then `git branch -D` — the cycle branch is
//! merged into epic but NOT into master, so `-d`'s merged-into-HEAD check would
//! fail; `-D` force-deletes unconditionally.
//!
//! ## `.git/info/exclude`
//!
//! `.worktrees/` is appended to `.git/info/exclude` on first use per clone. The
//! append is idempotent (grep-then-append): the line is added only if absent.
//!
//! ## Memory ownership
//!
//! `createCycle` returns a `Worktree` whose `path` and `branch` strings are
//! heap-owned (allocated with the supplied allocator). The caller MUST call
//! `Worktree.deinit(allocator)` when done.
//!
//! ## Error mapping
//!
//! All subprocess and non-zero-exit errors are mapped into `WorktreeError`.
//! No error is silently swallowed.
//!
//! ## Scope (M3 task 3173)
//!
//! This module implements create / ensure / teardown only. Run-id tagging, the
//! O_EXCL run-lock, run-id-scoped reconcile, basic global reconcile/prune
//! (task 3174), fan-in merge logic, and the Lua `agent()` wiring (M4) are
//! deliberately NOT here.

const std = @import("std");
const Io = std.Io;

// ---------------------------------------------------------------------------
// Error set
// ---------------------------------------------------------------------------

/// All errors this module can surface. Callers inspect these rather than
/// catching `anyerror`.
pub const WorktreeError = error{
    /// `std.process.run` itself failed (could not spawn `git`, broken pipe
    /// reading stdout, etc.). Treat as a transient / configuration error.
    SubprocessFailed,
    /// A `git` invocation exited with a non-zero status code.
    SubprocessNonZero,
    /// A filesystem operation (open / read / write of `.git/info/exclude`,
    /// directory existence probe) failed.
    FsError,
    /// Allocator returned OOM while building a path, branch, or argv.
    OutOfMemory,
};

// ---------------------------------------------------------------------------
// Worktree — a created worktree's path + branch (heap-owned).
// ---------------------------------------------------------------------------

/// A created worktree: its absolute filesystem `path` and the `branch` checked
/// out in it. Both strings are heap-owned; call `deinit` to free them.
pub const Worktree = struct {
    /// Absolute path to the worktree directory.
    path: []const u8,
    /// The branch name checked out in the worktree (e.g. `cycle/<plan>/<task>`).
    branch: []const u8,

    /// Free the heap-owned `path` and `branch` strings.
    pub fn deinit(self: *Worktree, allocator: std.mem.Allocator) void {
        allocator.free(self.path);
        allocator.free(self.branch);
    }
};

// ---------------------------------------------------------------------------
// Internal subprocess helper
// ---------------------------------------------------------------------------

/// runGit runs `git <argv_tail...>` and returns the captured stdout.
///
/// `git` is resolved via PATH (argv[0] = `"git"`, NOT a hard-coded path),
/// mirroring `state.zig:spawnPlanar`'s convention. The caller owns the returned
/// slice and must free it with `allocator`. Maps subprocess and non-zero-exit
/// errors into `WorktreeError`. Stderr is freed immediately (not surfaced to
/// callers).
fn runGit(
    allocator: std.mem.Allocator,
    io: Io,
    argv_tail: []const []const u8,
) WorktreeError![]u8 {
    // Build argv: ["git"] ++ argv_tail. argv_tail is variable-length so we
    // cannot use a comptime stack array. One exact-size alloc collapses N
    // per-append OOM branches into a single failure point.
    const argv = allocator.alloc([]const u8, argv_tail.len + 1) catch return WorktreeError.OutOfMemory;
    defer allocator.free(argv);
    argv[0] = "git";
    for (argv_tail, 0..) |arg, i| argv[i + 1] = arg;

    // 256 KiB cap: git porcelain/plumbing output here (show-ref, branch --list,
    // worktree add/remove) is tiny; the cap is generous headroom.
    const result = std.process.run(allocator, io, .{
        .argv = argv,
        .stdout_limit = Io.Limit.limited(256 * 1024),
        .stderr_limit = Io.Limit.limited(8192),
    }) catch return WorktreeError.SubprocessFailed;

    // Free stderr immediately (not surfaced to callers).
    allocator.free(result.stderr);

    // Map non-zero exit to an error. Free stdout before returning the error.
    const exit_ok = result.term == .exited and result.term.exited == 0;
    if (!exit_ok) {
        allocator.free(result.stdout);
        return WorktreeError.SubprocessNonZero;
    }

    return result.stdout;
}

/// gitOk runs `git <argv_tail...>` purely for its exit status, discarding
/// stdout. Returns true on exit 0, false on any non-zero exit. A spawn failure
/// is mapped to `WorktreeError.SubprocessFailed` (distinct from "ran and exited
/// non-zero", which is a legitimate false answer for existence probes).
fn gitOk(
    allocator: std.mem.Allocator,
    io: Io,
    argv_tail: []const []const u8,
) WorktreeError!bool {
    const argv = allocator.alloc([]const u8, argv_tail.len + 1) catch return WorktreeError.OutOfMemory;
    defer allocator.free(argv);
    argv[0] = "git";
    for (argv_tail, 0..) |arg, i| argv[i + 1] = arg;

    const result = std.process.run(allocator, io, .{
        .argv = argv,
        .stdout_limit = Io.Limit.limited(64 * 1024),
        .stderr_limit = Io.Limit.limited(8192),
    }) catch return WorktreeError.SubprocessFailed;

    allocator.free(result.stdout);
    allocator.free(result.stderr);

    return result.term == .exited and result.term.exited == 0;
}

// ---------------------------------------------------------------------------
// Deterministic naming helpers
// ---------------------------------------------------------------------------

/// epicBranch returns `epic/<plan_slug>` (heap-owned; caller frees).
pub fn epicBranch(allocator: std.mem.Allocator, plan_slug: []const u8) WorktreeError![]u8 {
    return std.fmt.allocPrint(allocator, "epic/{s}", .{plan_slug}) catch
        return WorktreeError.OutOfMemory;
}

/// cycleBranch returns `cycle/<plan_slug>/<task_slug>` (heap-owned; caller frees).
pub fn cycleBranch(
    allocator: std.mem.Allocator,
    plan_slug: []const u8,
    task_slug: []const u8,
) WorktreeError![]u8 {
    return std.fmt.allocPrint(allocator, "cycle/{s}/{s}", .{ plan_slug, task_slug }) catch
        return WorktreeError.OutOfMemory;
}

/// epicPath returns `<repo_root>/.worktrees/epic/<plan_slug>` (heap-owned; caller frees).
pub fn epicPath(
    allocator: std.mem.Allocator,
    repo_root: []const u8,
    plan_slug: []const u8,
) WorktreeError![]u8 {
    return std.fs.path.join(allocator, &.{ repo_root, ".worktrees", "epic", plan_slug }) catch
        return WorktreeError.OutOfMemory;
}

/// cyclePath returns `<repo_root>/.worktrees/cycle/<plan_slug>/<task_slug>`
/// (heap-owned; caller frees).
pub fn cyclePath(
    allocator: std.mem.Allocator,
    repo_root: []const u8,
    plan_slug: []const u8,
    task_slug: []const u8,
) WorktreeError![]u8 {
    return std.fs.path.join(allocator, &.{ repo_root, ".worktrees", "cycle", plan_slug, task_slug }) catch
        return WorktreeError.OutOfMemory;
}

// ---------------------------------------------------------------------------
// `.git/info/exclude` append (idempotent)
// ---------------------------------------------------------------------------

/// ensureWorktreesExcluded appends a `.worktrees/` line to
/// `<repo_root>/.git/info/exclude` if (and only if) it is not already present.
///
/// The grep-then-append ritual is idempotent: calling it twice leaves a single
/// `.worktrees/` line. This is per-clone state (`.git/info/exclude` is not
/// shared via the repo), so it is appended on first use. The append is done as
/// a read-modify-write of the whole file (the file is small — a handful of
/// glob lines), which keeps the `io`-threaded `std.Io.Dir` surface simple.
pub fn ensureWorktreesExcluded(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
) WorktreeError!void {
    const exclude_path = std.fs.path.join(allocator, &.{ repo_root, ".git", "info", "exclude" }) catch
        return WorktreeError.OutOfMemory;
    defer allocator.free(exclude_path);

    // Read the current contents (tolerate a missing file — git may not have
    // created info/exclude yet; we create it on write).
    const existing: []u8 = std.Io.Dir.cwd().readFileAlloc(io, exclude_path, allocator, .limited(1 << 20)) catch |err| switch (err) {
        error.FileNotFound => "",
        error.OutOfMemory => return WorktreeError.OutOfMemory,
        else => return WorktreeError.FsError,
    };
    defer if (existing.len != 0) allocator.free(existing);

    // Idempotency: scan existing lines for an exact `.worktrees/` match.
    var lines = std.mem.splitScalar(u8, existing, '\n');
    while (lines.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r");
        if (std.mem.eql(u8, line, ".worktrees/")) return; // already excluded
    }

    // Ensure the `.git/info` directory exists (git normally creates it, but be
    // defensive). createDirPath ignores PathAlreadyExists.
    const info_dir = std.fs.path.join(allocator, &.{ repo_root, ".git", "info" }) catch
        return WorktreeError.OutOfMemory;
    defer allocator.free(info_dir);
    std.Io.Dir.cwd().createDirPath(io, info_dir) catch |err| switch (err) {
        error.PathAlreadyExists => {},
        else => return WorktreeError.FsError,
    };

    // Build the new content: existing bytes + a `.worktrees/` line, inserting a
    // separating newline only when the existing content is non-empty and lacks
    // a trailing one (so we never run `foo` and `.worktrees/` together).
    const needs_leading_nl = existing.len != 0 and existing[existing.len - 1] != '\n';
    const new_content = if (needs_leading_nl)
        std.fmt.allocPrint(allocator, "{s}\n.worktrees/\n", .{existing}) catch return WorktreeError.OutOfMemory
    else
        std.fmt.allocPrint(allocator, "{s}.worktrees/\n", .{existing}) catch return WorktreeError.OutOfMemory;
    defer allocator.free(new_content);

    std.Io.Dir.cwd().writeFile(io, .{ .sub_path = exclude_path, .data = new_content }) catch
        return WorktreeError.FsError;
}

// ---------------------------------------------------------------------------
// Existence probes
// ---------------------------------------------------------------------------

/// branchExists reports whether `refs/heads/<branch>` exists in `repo_root`.
/// Uses `git -C <repo_root> show-ref --quiet refs/heads/<branch>` (exit 0 =
/// exists, exit 1 = absent).
fn branchExists(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    branch: []const u8,
) WorktreeError!bool {
    const ref = std.fmt.allocPrint(allocator, "refs/heads/{s}", .{branch}) catch
        return WorktreeError.OutOfMemory;
    defer allocator.free(ref);

    return gitOk(allocator, io, &.{ "-C", repo_root, "show-ref", "--quiet", "--verify", ref });
}

// ---------------------------------------------------------------------------
// Public lifecycle verbs
// ---------------------------------------------------------------------------

/// ensureEpic idempotently creates the once-per-plan epic worktree.
///
/// If `epic/<plan_slug>` already exists it is a clean no-op. Otherwise it
/// appends `.worktrees/` to `.git/info/exclude` (idempotent), then runs
/// `git -C <repo_root> worktree add -b epic/<plan_slug>
/// <repo_root>/.worktrees/epic/<plan_slug> master` — cutting the epic branch
/// from master and checking it out in its own worktree.
///
/// Returns the created (or pre-existing) `Worktree`; the caller MUST call
/// `Worktree.deinit(allocator)` when done.
pub fn ensureEpic(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    plan_slug: []const u8,
) WorktreeError!Worktree {
    const branch = try epicBranch(allocator, plan_slug);
    errdefer allocator.free(branch);
    const path = try epicPath(allocator, repo_root, plan_slug);
    errdefer allocator.free(path);

    if (try branchExists(allocator, io, repo_root, branch)) {
        // No-op: the epic branch already exists. Return the existing handle.
        return .{ .path = path, .branch = branch };
    }

    try ensureWorktreesExcluded(allocator, io, repo_root);

    const out = try runGit(allocator, io, &.{ "-C", repo_root, "worktree", "add", "-b", branch, path, "master" });
    allocator.free(out);

    return .{ .path = path, .branch = branch };
}

/// createCycle creates a per-task cycle worktree cut from the epic branch.
///
/// Runs `git -C <repo_root> worktree add -b cycle/<plan_slug>/<task_slug>
/// <repo_root>/.worktrees/cycle/<plan_slug>/<task_slug> epic/<plan_slug>` — the
/// cycle branch is cut from `epic/<plan_slug>`, NOT master.
///
/// The caller is expected to have created the epic via `ensureEpic` first.
/// Returns the created `Worktree`; the caller MUST call
/// `Worktree.deinit(allocator)` when done.
pub fn createCycle(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    plan_slug: []const u8,
    task_slug: []const u8,
) WorktreeError!Worktree {
    const branch = try cycleBranch(allocator, plan_slug, task_slug);
    errdefer allocator.free(branch);
    const path = try cyclePath(allocator, repo_root, plan_slug, task_slug);
    errdefer allocator.free(path);

    const epic_ref = try epicBranch(allocator, plan_slug);
    defer allocator.free(epic_ref);

    const out = try runGit(allocator, io, &.{ "-C", repo_root, "worktree", "add", "-b", branch, path, epic_ref });
    allocator.free(out);

    return .{ .path = path, .branch = branch };
}

/// teardownCycle removes a cycle worktree and force-deletes its branch.
///
/// Usable on BOTH the success path (after a merge into epic) AND the abort path
/// (a failed / aborted cycle, before any merge). Runs
/// `git -C <repo_root> worktree remove --force
/// <repo_root>/.worktrees/cycle/<plan_slug>/<task_slug>` then
/// `git -C <repo_root> branch -D cycle/<plan_slug>/<task_slug>`. The `-D`
/// (force) delete is required because the cycle branch is merged into epic but
/// NOT into master, so `-d`'s merged-into-HEAD check fails.
///
/// `--force` on `worktree remove` tolerates a dirty worktree (the abort path may
/// leave uncommitted work behind), keeping teardown reliable on failure.
pub fn teardownCycle(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    plan_slug: []const u8,
    task_slug: []const u8,
) WorktreeError!void {
    const branch = try cycleBranch(allocator, plan_slug, task_slug);
    defer allocator.free(branch);
    const path = try cyclePath(allocator, repo_root, plan_slug, task_slug);
    defer allocator.free(path);

    const rm_out = try runGit(allocator, io, &.{ "-C", repo_root, "worktree", "remove", "--force", path });
    allocator.free(rm_out);

    const br_out = try runGit(allocator, io, &.{ "-C", repo_root, "branch", "-D", branch });
    allocator.free(br_out);
}

// ---------------------------------------------------------------------------
// Unit tests — operate against real throwaway git repos.
//
// Each test creates an isolated repo via std.testing.tmpDir + `git init`, with
// `user.email`/`user.name` set locally and an initial commit on a `master`
// branch (git's default may be `main`, so we initialize/rename explicitly). The
// git-requiring tests skip via `error.SkipZigTest` when `git` is absent.
// ---------------------------------------------------------------------------

/// gitAvailable reports whether `git` is on PATH (mirrors the integration-suite
/// `gitAvailable` probe). Returns false on spawn failure or non-zero exit.
fn gitAvailable(allocator: std.mem.Allocator) bool {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "git", "--version" },
    }) catch return false;
    defer allocator.free(r.stdout);
    defer allocator.free(r.stderr);
    return r.term == .exited and r.term.exited == 0;
}

/// mkTmpRepoDir creates a fresh system temp directory via `mktemp -d` and
/// returns its absolute path (heap-owned; caller frees). Using `mktemp` mirrors
/// the integration harness's `freshSystemTmpDir`: it gives a clean absolute path
/// that is NOT nested under this checkout's own `.worktrees/` tree (which would
/// confuse `git worktree`), sidestepping the `std.testing.tmpDir` realpath
/// quirks. The caller is responsible for `rm -rf`-ing it via `rmTree`.
fn mkTmpRepoDir(allocator: std.mem.Allocator) []const u8 {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "mktemp", "-d", "-t", "planar-wt.XXXXXX" },
    }) catch @panic("mkTmpRepoDir: mktemp spawn failed");
    defer allocator.free(r.stderr);
    if (!(r.term == .exited and r.term.exited == 0)) {
        allocator.free(r.stdout);
        @panic("mkTmpRepoDir: mktemp non-zero exit");
    }
    const trimmed = std.mem.trim(u8, r.stdout, " \t\r\n");
    const owned = allocator.dupe(u8, trimmed) catch @panic("OOM");
    allocator.free(r.stdout);
    return owned;
}

/// rmTree removes `path` and its contents via `rm -rf` (best-effort cleanup).
fn rmTree(allocator: std.mem.Allocator, path: []const u8) void {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "rm", "-rf", path },
    }) catch return;
    allocator.free(r.stdout);
    allocator.free(r.stderr);
}

/// runGitIn runs a git command in `cwd`, panicking on failure (tests treat git
/// as a hard dependency once `gitAvailable` has cleared).
fn runGitIn(allocator: std.mem.Allocator, cwd: []const u8, args: []const []const u8) void {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(allocator);
    argv.append(allocator, "git") catch @panic("OOM");
    argv.append(allocator, "-C") catch @panic("OOM");
    argv.append(allocator, cwd) catch @panic("OOM");
    for (args) |a| argv.append(allocator, a) catch @panic("OOM");

    const r = std.process.run(allocator, std.testing.io, .{ .argv = argv.items }) catch
        @panic("runGitIn: spawn failed");
    defer allocator.free(r.stdout);
    defer allocator.free(r.stderr);
    switch (r.term) {
        .exited => |code| if (code != 0) {
            std.debug.print(
                "\nrunGitIn failed (exit {d}) in {s}\nargs[0]: {s}\nstderr: {s}\n",
                .{ code, cwd, args[0], r.stderr },
            );
            @panic("runGitIn: non-zero exit");
        },
        else => @panic("runGitIn: abnormal termination"),
    }
}

/// initRepo creates a throwaway git repo at `dir` with an initial commit on a
/// `master` branch. `user.email`/`user.name` are set locally so the commit
/// succeeds in CI environments with no global git identity.
fn initRepo(allocator: std.mem.Allocator, dir: []const u8) void {
    // `git init -b master` forces the initial branch name regardless of the
    // host's `init.defaultBranch` (which may be `main`).
    runGitIn(allocator, dir, &.{ "init", "-b", "master" });
    runGitIn(allocator, dir, &.{ "config", "user.email", "test@planar.local" });
    runGitIn(allocator, dir, &.{ "config", "user.name", "Planar Test" });
    // An initial commit so `master` is a real ref the worktrees can branch from.
    runGitIn(allocator, dir, &.{ "commit", "--allow-empty", "-m", "initial" });
}

/// dirExists reports whether `path` is an existing directory.
fn dirExists(io: Io, path: []const u8) bool {
    var d = std.Io.Dir.cwd().openDir(io, path, .{}) catch return false;
    d.close(io);
    return true;
}

/// branchListed reports whether `branch` appears in `git -C <repo> branch --list <branch>`.
fn branchListed(allocator: std.mem.Allocator, repo_root: []const u8, branch: []const u8) bool {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "git", "-C", repo_root, "branch", "--list", branch },
    }) catch return false;
    defer allocator.free(r.stdout);
    defer allocator.free(r.stderr);
    if (!(r.term == .exited and r.term.exited == 0)) return false;
    return std.mem.indexOf(u8, r.stdout, branch) != null;
}

test "deterministic naming: branch and path strings match the appendix table" {
    const a = std.testing.allocator;

    const eb = try epicBranch(a, "p492-orchestrate-harness");
    defer a.free(eb);
    try std.testing.expectEqualStrings("epic/p492-orchestrate-harness", eb);

    const cb = try cycleBranch(a, "p492-orchestrate-harness", "m3-worktree-lifecycle");
    defer a.free(cb);
    try std.testing.expectEqualStrings("cycle/p492-orchestrate-harness/m3-worktree-lifecycle", cb);

    const ep = try epicPath(a, "/repo", "p492-orchestrate-harness");
    defer a.free(ep);
    try std.testing.expectEqualStrings("/repo/.worktrees/epic/p492-orchestrate-harness", ep);

    const cp = try cyclePath(a, "/repo", "p492-orchestrate-harness", "m3-worktree-lifecycle");
    defer a.free(cp);
    try std.testing.expectEqualStrings("/repo/.worktrees/cycle/p492-orchestrate-harness/m3-worktree-lifecycle", cp);
}

test "ensureEpic creates the epic branch + worktree; second call is a clean no-op" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var wt = try ensureEpic(a, std.testing.io, repo, "planx");
    defer wt.deinit(a);

    try std.testing.expectEqualStrings("epic/planx", wt.branch);
    try std.testing.expect(dirExists(std.testing.io, wt.path));
    try std.testing.expect(branchListed(a, repo, "epic/planx"));

    // Second call: must NOT fail (would error if it re-ran `worktree add`).
    var wt2 = try ensureEpic(a, std.testing.io, repo, "planx");
    defer wt2.deinit(a);
    try std.testing.expectEqualStrings("epic/planx", wt2.branch);
    try std.testing.expect(dirExists(std.testing.io, wt2.path));
}

test "createCycle creates the cycle worktree on a branch cut from epic" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);

    var cyc = try createCycle(a, std.testing.io, repo, "planx", "task-one");
    defer cyc.deinit(a);

    try std.testing.expectEqualStrings("cycle/planx/task-one", cyc.branch);
    try std.testing.expect(dirExists(std.testing.io, cyc.path));
    try std.testing.expect(branchListed(a, repo, "cycle/planx/task-one"));
}

test "teardownCycle removes the worktree dir and force-deletes the branch" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);

    var cyc = try createCycle(a, std.testing.io, repo, "planx", "task-one");
    defer cyc.deinit(a);
    try std.testing.expect(dirExists(std.testing.io, cyc.path));
    try std.testing.expect(branchListed(a, repo, "cycle/planx/task-one"));

    try teardownCycle(a, std.testing.io, repo, "planx", "task-one");

    // Both gone afterward.
    try std.testing.expect(!dirExists(std.testing.io, cyc.path));
    try std.testing.expect(!branchListed(a, repo, "cycle/planx/task-one"));
}

test "teardownCycle works on the abort path (no merge ever happened)" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);

    var cyc = try createCycle(a, std.testing.io, repo, "planx", "aborted-task");
    defer cyc.deinit(a);

    // Make a commit on the cycle branch so it diverges from epic — this is the
    // abort path: unmerged work that `-d` would refuse but `-D` force-deletes.
    runGitIn(a, cyc.path, &.{ "config", "user.email", "test@planar.local" });
    runGitIn(a, cyc.path, &.{ "config", "user.name", "Planar Test" });
    runGitIn(a, cyc.path, &.{ "commit", "--allow-empty", "-m", "wip on aborted branch" });

    // Teardown must still succeed despite the unmerged divergence.
    try teardownCycle(a, std.testing.io, repo, "planx", "aborted-task");
    try std.testing.expect(!dirExists(std.testing.io, cyc.path));
    try std.testing.expect(!branchListed(a, repo, "cycle/planx/aborted-task"));
}

test ".git/info/exclude gains a .worktrees/ line on first use, not duplicated on second" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    try ensureWorktreesExcluded(a, std.testing.io, repo);
    try ensureWorktreesExcluded(a, std.testing.io, repo); // idempotent second call

    const exclude_path = try std.fs.path.join(a, &.{ repo, ".git", "info", "exclude" });
    defer a.free(exclude_path);
    const contents = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, exclude_path, a, .limited(1 << 20));
    defer a.free(contents);

    // Exactly one `.worktrees/` line.
    var count: usize = 0;
    var lines = std.mem.splitScalar(u8, contents, '\n');
    while (lines.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r");
        if (std.mem.eql(u8, line, ".worktrees/")) count += 1;
    }
    try std.testing.expectEqual(@as(usize, 1), count);
}

test "ensureEpic appends .worktrees/ to exclude on first epic creation" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);

    const exclude_path = try std.fs.path.join(a, &.{ repo, ".git", "info", "exclude" });
    defer a.free(exclude_path);
    const contents = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, exclude_path, a, .limited(1 << 20));
    defer a.free(contents);
    try std.testing.expect(std.mem.indexOf(u8, contents, ".worktrees/") != null);
}
