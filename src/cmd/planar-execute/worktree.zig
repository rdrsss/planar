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
//! ## Scope (M3 tasks 3173 + 3174)
//!
//! Task 3173 added create / ensure / teardown. Task 3174 adds the startup
//! reconcile pass — `reconcileAndPrune` + its pure pieces (`parseWorktreeList`,
//! `staleCycleWorktrees`) — which prunes stale planar-execute cycle worktrees
//! and calls `planar-agent reconcile` so a prior abnormal exit leaves no
//! stranded state.
//!
//! Still deliberately NOT here (later milestones): fan-in merge logic, the Lua
//! `agent()` wiring (M4), and — critically — the M6 run-isolation hardening.
//!
//! ## M3-vs-M6 boundary on the prune predicate (READ BEFORE CHANGING)
//!
//! The task-3174 prune predicate is intentionally GLOBAL / OWNERSHIP-TAG-FREE:
//! a cycle worktree is stale iff its task has no *active* claim (after
//! `planar-agent reconcile` has run). The tech-spec § "Run isolation —
//! single-instance lock + run-id-scoped reconcile / prune" documents that this
//! global predicate is provisional and is HARDENED in M6 (depends on engine
//! F2): M6 adds an `O_EXCL` plan-id run-lock carrying run-id + PID, run-id-
//! scoped reconcile via `--session`, run-id-TAGGED worktrees, a prune predicate
//! of "run-id-mismatch AND PID-not-alive", and a single-instance-per-plan
//! refusal. NONE of that belongs here — a future maintainer must not mistake
//! this global, no-ownership-tag predicate for the final design.

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
    /// `std.json.parseFromSlice` rejected a subprocess's `--json` output
    /// (`planar-watch ps`, `planar task list`, `planar-agent reconcile`).
    ParseFailed,
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
// Fan-in merge — task 3177 (M4): merge a completed cycle branch into epic.
// ---------------------------------------------------------------------------

/// Outcome of `mergeCycleIntoEpic`.
///
/// - `clean`     — the merge committed cleanly (or was a fast-forward / no-op).
/// - `conflict`  — git reported merge conflicts. The cycle worktree is LEFT IN
///                 PLACE and the merge in the epic worktree has been aborted
///                 (`git merge --abort`). The caller is expected to open a
///                 `planar question` and stop the cycle; the harness MUST NOT
///                 attempt auto-resolution (tech-spec appendix § "Conflict
///                 resolution at fan-in").
pub const MergeResult = enum { clean, conflict };

/// mergeCycleIntoEpic runs `git -C <epic_worktree> merge --no-ff
/// <cycle_branch> -m "<commit_msg>"` from inside the epic worktree.
///
/// On clean merge the function returns `MergeResult.clean`. On conflict the
/// function aborts the merge (`git merge --abort`) so the epic worktree is left
/// in a consistent pre-merge state, and returns `MergeResult.conflict`. The
/// cycle worktree is NOT torn down on conflict — the operator may want to
/// inspect it before opening a question.
///
/// `commit_msg` is the merge-commit subject. The caller chooses it (typically
/// `"Plan <id> task <id>: <task title>"`).
///
/// Pre-conditions: the epic worktree exists (call `ensureEpic` first) and the
/// cycle branch exists (the worker has committed at least once on it).
pub fn mergeCycleIntoEpic(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    plan_slug: []const u8,
    task_slug: []const u8,
    commit_msg: []const u8,
) WorktreeError!MergeResult {
    const epic_wt = try epicPath(allocator, repo_root, plan_slug);
    defer allocator.free(epic_wt);

    const cycle_ref = try cycleBranch(allocator, plan_slug, task_slug);
    defer allocator.free(cycle_ref);

    // Run the merge from inside the epic worktree (`git -C <epic_wt>`). The
    // `--no-ff` flag forces a merge commit even when fast-forwardable, so the
    // cycle's identity is preserved in the epic history (the integration view
    // shows "merged cycle X" as a discrete commit).
    const merged = gitOk(allocator, io, &.{
        "-C", epic_wt,    "merge",   "--no-ff",
        "-m", commit_msg, cycle_ref,
    }) catch |err| return err;

    if (merged) return .clean;

    // Conflict path: abort so the epic worktree is restored. `git merge
    // --abort` is itself a fallible operation (no merge in progress, etc.); we
    // call it best-effort — the caller already knows there is a conflict.
    _ = gitOk(allocator, io, &.{ "-C", epic_wt, "merge", "--abort" }) catch {};
    return .conflict;
}

// ---------------------------------------------------------------------------
// Pre/post commit sampling — task 3180 supporting helper.
// ---------------------------------------------------------------------------

/// branchHead returns the commit SHA at the tip of `<branch>` in `repo_root`,
/// or null when the branch does not exist (e.g. the worker has not yet created
/// the cycle branch). Used by the terminal-fallback decision matrix to compare
/// pre-spawn vs post-spawn cycle HEADs and infer commit-presence.
///
/// The returned slice is heap-owned (caller frees) on success; trimmed of the
/// trailing newline `git rev-parse` emits.
pub fn branchHead(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    branch: []const u8,
) WorktreeError!?[]u8 {
    const exists = branchExists(allocator, io, repo_root, branch) catch |err| return err;
    if (!exists) return null;

    const out = try runGit(allocator, io, &.{ "-C", repo_root, "rev-parse", branch });
    // Trim trailing whitespace (rev-parse appends a newline).
    const trimmed = std.mem.trim(u8, out, " \t\r\n");
    if (trimmed.len == out.len) {
        return out; // no trim needed
    }
    const owned = allocator.dupe(u8, trimmed) catch {
        allocator.free(out);
        return WorktreeError.OutOfMemory;
    };
    allocator.free(out);
    return owned;
}

// ---------------------------------------------------------------------------
// Startup reconcile pass — task 3174 (m3-startup-reconcile).
//
// On startup a prior `planar-execute` run may have crashed mid-cycle, leaving
// (a) a cycle worktree on disk and (b) a now-expired claim on that cycle's
// task. The reconcile pass removes BOTH classes of stranded state:
//
//   1. Claim side — shell `planar-agent reconcile` (the existing global verb)
//      to mark expired claims stale and close orphaned actions.
//   2. Worktree side — enumerate `git worktree list --porcelain`, filter to
//      THIS plan's planar-execute-managed worktrees, and prune cycle worktrees
//      whose task no longer holds an *active* claim. The epic worktree PERSISTS
//      (it survives until the operator merges epic→master). Foreign plans'
//      worktrees are NEVER touched (plan-scoped filter).
//
// Testability seam (load-bearing): the *decision* — given the on-disk
// planar-execute worktrees for this plan and the set of task-slugs that still
// have an active claim, which cycle worktrees are stale? — is the pure function
// `staleCycleWorktrees`, unit-tested with hand-built inputs (no git, no DB).
// The porcelain parse is the pure `parseWorktreeList`. Only `reconcileAndPrune`
// touches subprocesses; its live end-to-end coverage is deferred to the M3+
// integration pass (the same deferral as task 3236 for subprocess reads).
// ---------------------------------------------------------------------------

/// A single entry parsed from `git worktree list --porcelain`.
///
/// `path` is the worktree's absolute filesystem path. `branch` is the short
/// branch name (`refs/heads/` stripped) checked out in it, or null for a
/// detached-HEAD / bare worktree. Both strings are heap-owned by the slice the
/// parser returns; free via `freeWorktreeList`.
pub const WorktreeEntry = struct {
    path: []const u8,
    branch: ?[]const u8,
};

/// The classification of a planar-execute-managed worktree this plan owns.
pub const WorktreeRole = enum {
    /// `epic/<plan_slug>` — PERSISTS, never pruned.
    epic,
    /// `cycle/<plan_slug>/<task_slug>` — prune candidate.
    cycle,
};

/// A cycle worktree the classifier has determined is stale and must be pruned.
///
/// `task_slug` is heap-owned (duped from the parsed branch); free via
/// `freeStaleList`. `path` is a borrow into the caller's `WorktreeEntry` slice
/// (not owned) — it is provided for diagnostics; the pruner re-derives the path
/// from `task_slug` via `cyclePath` so it never depends on the borrowed string
/// outliving the entry list.
pub const StaleCycle = struct {
    task_slug: []const u8,
    path: []const u8,
};

/// freeWorktreeList frees a slice returned by `parseWorktreeList`.
pub fn freeWorktreeList(allocator: std.mem.Allocator, list: []WorktreeEntry) void {
    for (list) |e| {
        allocator.free(e.path);
        if (e.branch) |b| allocator.free(b);
    }
    allocator.free(list);
}

/// freeStaleList frees a slice returned by `staleCycleWorktrees`.
pub fn freeStaleList(allocator: std.mem.Allocator, list: []StaleCycle) void {
    for (list) |s| allocator.free(s.task_slug);
    allocator.free(list);
}

// ---------------------------------------------------------------------------
// Pure: porcelain parser
// ---------------------------------------------------------------------------

/// parseWorktreeList parses `git worktree list --porcelain` output into a slice
/// of `WorktreeEntry`.
///
/// PURE — no subprocess, no DB. Unit-tested against fixture porcelain strings.
///
/// The porcelain format emits one record per worktree, records separated by a
/// blank line. Each record is a set of `key value` (or bare `key`) lines:
///   worktree <abs-path>
///   HEAD <sha>
///   branch refs/heads/<branch>      (absent for detached HEAD)
///   bare                            (for the bare main repo, no `worktree` HEAD)
///   detached                        (for detached-HEAD worktrees)
///
/// We capture `worktree <path>` (record start) and `branch refs/heads/<branch>`
/// (short branch name = the ref with the `refs/heads/` prefix stripped). A
/// record with no `branch` line yields `branch = null`.
///
/// The caller owns the returned slice and MUST free it via `freeWorktreeList`.
pub fn parseWorktreeList(
    allocator: std.mem.Allocator,
    porcelain: []const u8,
) WorktreeError![]WorktreeEntry {
    var list = std.ArrayList(WorktreeEntry).empty;
    errdefer {
        for (list.items) |e| {
            allocator.free(e.path);
            if (e.branch) |b| allocator.free(b);
        }
        list.deinit(allocator);
    }

    // Per-record in-flight state. A record is committed on the blank-line
    // boundary (or at EOF) when it has a `worktree <path>` line.
    var cur_path: ?[]const u8 = null;
    var cur_branch: ?[]const u8 = null;
    errdefer {
        if (cur_path) |p| allocator.free(p);
        if (cur_branch) |b| allocator.free(b);
    }

    var lines = std.mem.splitScalar(u8, porcelain, '\n');
    while (lines.next()) |raw| {
        const line = std.mem.trimEnd(u8, raw, "\r");
        if (line.len == 0) {
            // Record boundary: commit the in-flight record (if any).
            if (cur_path) |p| {
                try list.append(allocator, .{ .path = p, .branch = cur_branch });
                cur_path = null;
                cur_branch = null;
            }
            continue;
        }

        if (std.mem.startsWith(u8, line, "worktree ")) {
            // A new `worktree` line starting before a blank-line boundary means
            // the previous record had no trailing blank (rare). Commit it.
            if (cur_path) |p| {
                try list.append(allocator, .{ .path = p, .branch = cur_branch });
                cur_branch = null;
            }
            const val = line["worktree ".len..];
            cur_path = allocator.dupe(u8, val) catch return WorktreeError.OutOfMemory;
        } else if (std.mem.startsWith(u8, line, "branch ")) {
            const ref = line["branch ".len..];
            const short = if (std.mem.startsWith(u8, ref, "refs/heads/"))
                ref["refs/heads/".len..]
            else
                ref;
            cur_branch = allocator.dupe(u8, short) catch return WorktreeError.OutOfMemory;
        }
        // All other lines (HEAD, bare, detached, locked, prunable, ...) ignored.
    }

    // EOF: commit a final record with no trailing blank line.
    if (cur_path) |p| {
        try list.append(allocator, .{ .path = p, .branch = cur_branch });
        cur_path = null;
        cur_branch = null;
    }

    return list.toOwnedSlice(allocator) catch return WorktreeError.OutOfMemory;
}

// ---------------------------------------------------------------------------
// Pure: plan-scoped classifier
// ---------------------------------------------------------------------------

/// classifyManaged decides whether `entry` is a planar-execute-managed worktree
/// for `plan_slug`, and if so whether it is the epic or a cycle (returning the
/// cycle's task-slug). Returns null for anything NOT managed by this plan —
/// foreign plans' worktrees, the bare main checkout, detached worktrees, or
/// paths outside `<repo_root>/.worktrees/`.
///
/// PURE. The branch name carries the authoritative identity (the naming helpers
/// `epicBranch` / `cycleBranch` define it); the path check is a defense-in-depth
/// guard that the worktree lives under our `.worktrees/` tree so a foreign
/// worktree that merely shares a branch-name shape is still excluded.
fn classifyManaged(
    entry: WorktreeEntry,
    repo_root: []const u8,
    plan_slug: []const u8,
) ?struct { role: WorktreeRole, task_slug: []const u8 } {
    const branch = entry.branch orelse return null;

    // Path guard: must live under `<repo_root>/.worktrees/`. We match the
    // segment rather than a raw prefix so `<repo_root>/.worktrees-foo` does not
    // sneak through.
    const wt_marker = ".worktrees" ++ std.fs.path.sep_str;
    const under_repo = std.mem.startsWith(u8, entry.path, repo_root);
    if (!under_repo) return null;
    if (std.mem.indexOf(u8, entry.path, wt_marker) == null) return null;

    // Epic: branch == `epic/<plan_slug>` exactly.
    {
        var buf: [256]u8 = undefined;
        const epic = std.fmt.bufPrint(&buf, "epic/{s}", .{plan_slug}) catch return null;
        if (std.mem.eql(u8, branch, epic)) {
            return .{ .role = .epic, .task_slug = "" };
        }
    }

    // Cycle: branch == `cycle/<plan_slug>/<task_slug>`. The task-slug is
    // everything after the `cycle/<plan_slug>/` prefix (task-slugs do not
    // themselves contain `/`, but we take the full remainder for safety).
    {
        var buf: [256]u8 = undefined;
        const prefix = std.fmt.bufPrint(&buf, "cycle/{s}/", .{plan_slug}) catch return null;
        if (std.mem.startsWith(u8, branch, prefix)) {
            const task_slug = branch[prefix.len..];
            if (task_slug.len == 0) return null; // malformed; not a cycle
            return .{ .role = .cycle, .task_slug = task_slug };
        }
    }

    return null;
}

/// staleCycleWorktrees is the pure core of the prune decision.
///
/// Given the parsed on-disk worktree list, the current `plan_slug`, and the set
/// of task-slugs that STILL have an active claim, it returns the cycle
/// worktrees that are stale and must be pruned.
///
/// PURE — no git, no DB. Inject `active_task_slugs` (derived at runtime by
/// correlating `planar-watch ps` active claims with the task id↔slug mapping)
/// and unit-test the decision directly.
///
/// Invariants (each pinned by a unit test):
///   - The epic worktree is NEVER returned (it persists until epic→master merge).
///   - A cycle whose task-slug IS in `active_task_slugs` is NEVER returned
///     (a live run owns it).
///   - Foreign-plan worktrees are NEVER returned (classifyManaged excludes them).
///
/// The caller owns the returned slice and MUST free it via `freeStaleList`.
pub fn staleCycleWorktrees(
    allocator: std.mem.Allocator,
    all_worktrees: []const WorktreeEntry,
    repo_root: []const u8,
    plan_slug: []const u8,
    active_task_slugs: []const []const u8,
) WorktreeError![]StaleCycle {
    var stale = std.ArrayList(StaleCycle).empty;
    errdefer {
        for (stale.items) |s| allocator.free(s.task_slug);
        stale.deinit(allocator);
    }

    for (all_worktrees) |entry| {
        const managed = classifyManaged(entry, repo_root, plan_slug) orelse continue;
        if (managed.role == .epic) continue; // epic persists, never prune

        // Cycle: stale iff its task-slug is NOT in the active set.
        var is_active = false;
        for (active_task_slugs) |slug| {
            if (std.mem.eql(u8, slug, managed.task_slug)) {
                is_active = true;
                break;
            }
        }
        if (is_active) continue;

        const owned_slug = allocator.dupe(u8, managed.task_slug) catch return WorktreeError.OutOfMemory;
        errdefer allocator.free(owned_slug);
        try stale.append(allocator, .{ .task_slug = owned_slug, .path = entry.path });
    }

    return stale.toOwnedSlice(allocator) catch return WorktreeError.OutOfMemory;
}

// ---------------------------------------------------------------------------
// Subprocess: generic binary spawn (planar / planar-watch / planar-agent)
// ---------------------------------------------------------------------------

/// runBin runs `<bin> <argv_tail...>` and returns captured stdout, mirroring
/// `runGit` (PATH-resolved argv[0], stdout/stderr caps, error mapping, caller
/// owns stdout, stderr freed). Used for `planar-agent` / `planar-watch` /
/// `planar` invocations in the reconcile path.
fn runBin(
    allocator: std.mem.Allocator,
    io: Io,
    bin: []const u8,
    argv_tail: []const []const u8,
    stdout_limit: usize,
) WorktreeError![]u8 {
    const argv = allocator.alloc([]const u8, argv_tail.len + 1) catch return WorktreeError.OutOfMemory;
    defer allocator.free(argv);
    argv[0] = bin;
    for (argv_tail, 0..) |arg, i| argv[i + 1] = arg;

    const result = std.process.run(allocator, io, .{
        .argv = argv,
        .stdout_limit = Io.Limit.limited(stdout_limit),
        .stderr_limit = Io.Limit.limited(8192),
    }) catch return WorktreeError.SubprocessFailed;

    allocator.free(result.stderr);

    const exit_ok = result.term == .exited and result.term.exited == 0;
    if (!exit_ok) {
        allocator.free(result.stdout);
        return WorktreeError.SubprocessNonZero;
    }

    return result.stdout;
}

// ---------------------------------------------------------------------------
// Runtime correlation: active task-slugs for a plan
// ---------------------------------------------------------------------------

/// Minimal view of an active-claim row from `planar-watch ps --plan <id> --json`.
/// Only `entity_kind` / `entity_id` are consumed (we want active task claims).
const PsClaim = struct {
    entity_kind: []const u8,
    entity_id: u64,
};

/// Minimal view of `planar-watch ps --plan <id> --json`.
/// Shape: {"generated_at":"...","active":[<claim>...],"stale":[<claim>...]}.
const PsResult = struct {
    active: []PsClaim,
};

/// Minimal view of a task row from `planar task list --plan <id> --json`.
const TaskListRow = struct {
    id: u64,
    slug: ?[]const u8 = null,
};

/// activeTaskSlugs returns the set of task-slugs that currently hold an ACTIVE
/// claim on `plan_id`, by correlating `planar-watch ps --plan <id> --json`'s
/// `active` array (entity task ids) with `planar task list --plan <id> --json`
/// (task id↔slug). Only `entity_kind == "task"` claims with a non-null slug are
/// included.
///
/// The correlation lives here (subprocess side); the resulting slug set is then
/// injected into the PURE `staleCycleWorktrees` so the decision stays testable.
///
/// The caller owns the returned slice and each slug within; free via
/// `freeActiveSlugs`.
fn activeTaskSlugs(
    allocator: std.mem.Allocator,
    io: Io,
    plan_id: u64,
) WorktreeError![][]const u8 {
    var id_buf: [32]u8 = undefined;
    const id_str = std.fmt.bufPrint(&id_buf, "{d}", .{plan_id}) catch return WorktreeError.SubprocessFailed;

    // 1) Active claims for the plan.
    const ps_out = try runBin(allocator, io, "planar-watch", &.{ "ps", "--plan", id_str, "--json" }, 4 * 1024 * 1024);
    defer allocator.free(ps_out);
    const ps = std.json.parseFromSlice(PsResult, allocator, ps_out, .{ .ignore_unknown_fields = true }) catch
        return WorktreeError.ParseFailed;
    defer ps.deinit();

    // 2) Task id↔slug mapping for the plan.
    const tl_out = try runBin(allocator, io, "planar", &.{ "task", "list", "--plan", id_str, "--json" }, 4 * 1024 * 1024);
    defer allocator.free(tl_out);
    const tasks = std.json.parseFromSlice([]TaskListRow, allocator, tl_out, .{ .ignore_unknown_fields = true }) catch
        return WorktreeError.ParseFailed;
    defer tasks.deinit();

    var slugs = std.ArrayList([]const u8).empty;
    errdefer {
        for (slugs.items) |s| allocator.free(s);
        slugs.deinit(allocator);
    }

    for (ps.value.active) |claim| {
        if (!std.mem.eql(u8, claim.entity_kind, "task")) continue;
        // Find the slug for this entity_id.
        for (tasks.value) |t| {
            if (t.id != claim.entity_id) continue;
            const slug = t.slug orelse break;
            const owned = allocator.dupe(u8, slug) catch return WorktreeError.OutOfMemory;
            errdefer allocator.free(owned);
            try slugs.append(allocator, owned);
            break;
        }
    }

    return slugs.toOwnedSlice(allocator) catch return WorktreeError.OutOfMemory;
}

/// freeActiveSlugs frees the slice (and each slug) returned by `activeTaskSlugs`.
fn freeActiveSlugs(allocator: std.mem.Allocator, slugs: [][]const u8) void {
    for (slugs) |s| allocator.free(s);
    allocator.free(slugs);
}

// ---------------------------------------------------------------------------
// Top-level orchestrator: reconcileAndPrune
// ---------------------------------------------------------------------------

/// Outcome of a `reconcileAndPrune` pass.
///
/// `stale` is the number of cycle worktrees the classifier flagged as stale
/// (i.e. their owning task no longer holds an active claim). `pruned` is the
/// number actually torn down. In a normal (non-dry-run) pass `pruned == stale`
/// once teardown succeeds for every flagged cycle. Under a dry run NO teardown
/// is performed, so `pruned == 0` while `stale` still reports what *would* be
/// pruned — this is what the read-only `doctor` probe reports as `stale_cycles`.
pub const ReconcileResult = struct {
    /// Cycle worktrees classified as stale (would-be / actual prune targets).
    stale: usize,
    /// Cycle worktrees actually torn down this pass (0 under dry run).
    pruned: usize,
};

/// reconcileAndPrune is the startup reconcile pass (task 3174), with a
/// non-destructive dry-run mode (task 3236).
///
/// It orchestrates (its pure constituents are unit-tested; the live subprocess
/// half is covered end-to-end by `integration_tests/planar_execute_doctor_test.zig`
/// via the `doctor` verb's dry-run probe — task 3236):
///   1. Shell `planar-agent reconcile [--dry-run]` — mark expired claims stale,
///      close orphaned actions (under `--dry-run` it only reports candidates).
///      Run first so a crashed cycle's claim is staled BEFORE we read active
///      claims, otherwise an about-to-expire claim could wrongly look active.
///   2. Read the now-current active task-slugs for `plan_id` (`activeTaskSlugs`).
///   3. Enumerate worktrees (`git worktree list --porcelain`) → parse.
///   4. `staleCycleWorktrees` (PURE) → the cycle worktrees to prune.
///   5. `teardownCycle` each stale one (plan-scoped, epic excluded, foreign
///      worktrees never returned by the classifier). SKIPPED under dry run.
///   6. `git worktree prune` — clear admin entries for cycle dirs already
///      manually deleted (dir gone, git metadata lingering). SKIPPED under dry run.
///
/// When `dry_run == true` the function performs steps 1–4 (reads only — step 1
/// passes `--dry-run` to `planar-agent reconcile` so it does not write) and
/// returns the computed stale count WITHOUT mutating anything: no `teardownCycle`,
/// no `git worktree prune`. The non-dry path is bit-identical to the task-3174
/// approved behavior.
///
/// Errors from steps 1–4 propagate (a failed reconcile/read is a hard startup
/// problem). A `teardownCycle` failure on one stale cycle is NOT
/// logged-and-skipped — we let it propagate so the operator sees the first
/// failure rather than silently leaving half-pruned state; the caller decides
/// whether a partial prune is fatal. (M6 will refine this under the run-lock.)
pub fn reconcileAndPrune(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    plan_slug: []const u8,
    plan_id: u64,
    dry_run: bool,
) WorktreeError!ReconcileResult {
    // 1) Claim side: global reconcile (mark expired claims stale). Under a dry
    //    run pass `--dry-run` so the verb only reports candidates (no write).
    const rec_out = if (dry_run)
        try runBin(allocator, io, "planar-agent", &.{ "reconcile", "--dry-run", "--json" }, 256 * 1024)
    else
        try runBin(allocator, io, "planar-agent", &.{ "reconcile", "--json" }, 256 * 1024);
    allocator.free(rec_out);

    // 2) Read the current active task-slug set for the plan.
    const active = try activeTaskSlugs(allocator, io, plan_id);
    defer freeActiveSlugs(allocator, active);

    // 3) Enumerate + parse worktrees.
    const porcelain = try runGit(allocator, io, &.{ "-C", repo_root, "worktree", "list", "--porcelain" });
    defer allocator.free(porcelain);
    const worktrees = try parseWorktreeList(allocator, porcelain);
    defer freeWorktreeList(allocator, worktrees);

    // 4) Classify (PURE). Compare against the CANONICAL repo root: `git worktree
    // list --porcelain` emits symlink-resolved absolute paths, so the path-guard
    // in the classifier must use the same canonical form (a non-canonical
    // `repo_root` — e.g. a `/var/...` mktemp path on macOS that git reports as
    // `/private/var/...` — would otherwise exclude every managed worktree). The
    // FIRST porcelain record is always the main checkout, whose path git has
    // already canonicalized; use it as the canonical root (falling back to the
    // passed `repo_root` if the list is somehow empty).
    const canon_root = if (worktrees.len > 0) worktrees[0].path else repo_root;
    const stale = try staleCycleWorktrees(allocator, worktrees, canon_root, plan_slug, active);
    defer freeStaleList(allocator, stale);

    // Dry run: report the computed stale count, mutate nothing.
    if (dry_run) {
        return .{ .stale = stale.len, .pruned = 0 };
    }

    // 5) Teardown each stale cycle worktree.
    for (stale) |s| {
        try teardownCycle(allocator, io, repo_root, plan_slug, s.task_slug);
    }

    // 6) git worktree prune — clear admin entries for already-deleted dirs.
    const prune_out = try runGit(allocator, io, &.{ "-C", repo_root, "worktree", "prune" });
    allocator.free(prune_out);

    return .{ .stale = stale.len, .pruned = stale.len };
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

// ---------------------------------------------------------------------------
// Reconcile/prune tests — task 3174.
//
// The porcelain parser and the classifier are PURE: tested with hand-built
// fixtures, no git, no DB. The prune-action test drives a real throwaway git
// repo (skip-probed via gitAvailable). The `planar-agent reconcile` shell-out
// and the live claim read are NOT unit-tested here (no live binary + DB) — that
// live end-to-end coverage is deferred to the M3+ integration pass (task 3236).
// ---------------------------------------------------------------------------

/// finds the index of the entry whose branch equals `branch` (or null).
fn findByBranch(list: []const WorktreeEntry, branch: []const u8) ?usize {
    for (list, 0..) |e, i| {
        if (e.branch) |b| {
            if (std.mem.eql(u8, b, branch)) return i;
        }
    }
    return null;
}

test "parseWorktreeList: multi-plan fixture — records parsed, foreign + bare tolerated" {
    const a = std.testing.allocator;
    // Fixture mirrors real `git worktree list --porcelain` output: the bare main
    // checkout, this plan's epic + a cycle, a FOREIGN plan's epic, and a
    // detached-HEAD worktree (no branch). Paths use the host path separator so
    // the classifier's path-guard test below is portable.
    const sep = std.fs.path.sep_str;
    const fixture = try std.fmt.allocPrint(a,
        \\worktree {0s}{1s}repo
        \\HEAD 06b95ef9b113e480abc060dbcb30874462bdef93
        \\branch refs/heads/master
        \\
        \\worktree {0s}{1s}repo{1s}.worktrees{1s}epic{1s}p492
        \\HEAD aaaa1111
        \\branch refs/heads/epic/p492
        \\
        \\worktree {0s}{1s}repo{1s}.worktrees{1s}cycle{1s}p492{1s}m3-startup
        \\HEAD bbbb2222
        \\branch refs/heads/cycle/p492/m3-startup
        \\
        \\worktree {0s}{1s}repo{1s}.worktrees{1s}epic{1s}p493
        \\HEAD cccc3333
        \\branch refs/heads/epic/p493
        \\
        \\worktree {0s}{1s}repo{1s}.worktrees{1s}detached-one
        \\HEAD dddd4444
        \\detached
        \\
    , .{ sep, sep });
    defer a.free(fixture);

    const list = try parseWorktreeList(a, fixture);
    defer freeWorktreeList(a, list);

    try std.testing.expectEqual(@as(usize, 5), list.len);
    // master worktree: branch short-name strips refs/heads/.
    try std.testing.expect(findByBranch(list, "master") != null);
    try std.testing.expect(findByBranch(list, "epic/p492") != null);
    try std.testing.expect(findByBranch(list, "cycle/p492/m3-startup") != null);
    try std.testing.expect(findByBranch(list, "epic/p493") != null);
    // The detached worktree has a null branch.
    var detached_count: usize = 0;
    for (list) |e| {
        if (e.branch == null) detached_count += 1;
    }
    try std.testing.expectEqual(@as(usize, 1), detached_count);
}

test "parseWorktreeList: final record without trailing blank line is committed" {
    const a = std.testing.allocator;
    const fixture =
        \\worktree /r/.worktrees/cycle/p1/t1
        \\HEAD abc
        \\branch refs/heads/cycle/p1/t1
    ;
    const list = try parseWorktreeList(a, fixture);
    defer freeWorktreeList(a, list);
    try std.testing.expectEqual(@as(usize, 1), list.len);
    try std.testing.expectEqualStrings("cycle/p1/t1", list[0].branch.?);
}

test "parseWorktreeList: empty input yields empty list" {
    const a = std.testing.allocator;
    const list = try parseWorktreeList(a, "");
    defer freeWorktreeList(a, list);
    try std.testing.expectEqual(@as(usize, 0), list.len);
}

test "staleCycleWorktrees: epic persists, foreign excluded, active cycle kept, stale cycle pruned" {
    const a = std.testing.allocator;
    const sep = std.fs.path.sep_str;
    const repo = try std.fmt.allocPrint(a, "{0s}{1s}repo", .{ sep, sep });
    defer a.free(repo);

    // Build the on-disk worktree set for plan slug "p492":
    //   - epic/p492            → must NEVER be stale
    //   - cycle/p492/active-t  → has an active claim → NOT pruned
    //   - cycle/p492/stale-t   → no active claim → PRUNED
    //   - epic/p493 (FOREIGN)  → never returned
    //   - cycle/p493/x (FOREIGN) → never returned
    //   - master               → never returned (not managed)
    const epic_p492_path = try std.fmt.allocPrint(a, "{0s}{1s}.worktrees{1s}epic{1s}p492", .{ repo, sep });
    defer a.free(epic_p492_path);
    const cyc_active_path = try std.fmt.allocPrint(a, "{0s}{1s}.worktrees{1s}cycle{1s}p492{1s}active-t", .{ repo, sep });
    defer a.free(cyc_active_path);
    const cyc_stale_path = try std.fmt.allocPrint(a, "{0s}{1s}.worktrees{1s}cycle{1s}p492{1s}stale-t", .{ repo, sep });
    defer a.free(cyc_stale_path);
    const epic_p493_path = try std.fmt.allocPrint(a, "{0s}{1s}.worktrees{1s}epic{1s}p493", .{ repo, sep });
    defer a.free(epic_p493_path);
    const cyc_p493_path = try std.fmt.allocPrint(a, "{0s}{1s}.worktrees{1s}cycle{1s}p493{1s}x", .{ repo, sep });
    defer a.free(cyc_p493_path);

    const worktrees = [_]WorktreeEntry{
        .{ .path = repo, .branch = "master" },
        .{ .path = epic_p492_path, .branch = "epic/p492" },
        .{ .path = cyc_active_path, .branch = "cycle/p492/active-t" },
        .{ .path = cyc_stale_path, .branch = "cycle/p492/stale-t" },
        .{ .path = epic_p493_path, .branch = "epic/p493" },
        .{ .path = cyc_p493_path, .branch = "cycle/p493/x" },
    };

    const active = [_][]const u8{"active-t"};

    const stale = try staleCycleWorktrees(a, &worktrees, repo, "p492", &active);
    defer freeStaleList(a, stale);

    // Exactly one stale cycle: stale-t. Epic excluded; active-t kept; foreign
    // plan p493 worktrees never returned.
    try std.testing.expectEqual(@as(usize, 1), stale.len);
    try std.testing.expectEqualStrings("stale-t", stale[0].task_slug);
}

test "staleCycleWorktrees: all cycles active → nothing pruned; epic still excluded" {
    const a = std.testing.allocator;
    const sep = std.fs.path.sep_str;
    const repo = try std.fmt.allocPrint(a, "{0s}{1s}repo", .{ sep, sep });
    defer a.free(repo);
    const epic_path = try std.fmt.allocPrint(a, "{0s}{1s}.worktrees{1s}epic{1s}p492", .{ repo, sep });
    defer a.free(epic_path);
    const cyc_path = try std.fmt.allocPrint(a, "{0s}{1s}.worktrees{1s}cycle{1s}p492{1s}t1", .{ repo, sep });
    defer a.free(cyc_path);

    const worktrees = [_]WorktreeEntry{
        .{ .path = epic_path, .branch = "epic/p492" },
        .{ .path = cyc_path, .branch = "cycle/p492/t1" },
    };
    const active = [_][]const u8{"t1"};

    const stale = try staleCycleWorktrees(a, &worktrees, repo, "p492", &active);
    defer freeStaleList(a, stale);
    try std.testing.expectEqual(@as(usize, 0), stale.len);
}

test "staleCycleWorktrees: a cycle branch-name shape OUTSIDE .worktrees/ is not managed" {
    const a = std.testing.allocator;
    const sep = std.fs.path.sep_str;
    const repo = try std.fmt.allocPrint(a, "{0s}{1s}repo", .{ sep, sep });
    defer a.free(repo);
    // A worktree whose branch matches the cycle shape but whose PATH is not under
    // <repo>/.worktrees/ — the path-guard must exclude it (defense in depth).
    const foreign_path = try std.fmt.allocPrint(a, "{0s}{1s}repo{1s}somewhere-else{1s}t1", .{ sep, sep });
    defer a.free(foreign_path);
    const worktrees = [_]WorktreeEntry{
        .{ .path = foreign_path, .branch = "cycle/p492/t1" },
    };
    const active = [_][]const u8{};
    const stale = try staleCycleWorktrees(a, &worktrees, repo, "p492", &active);
    defer freeStaleList(a, stale);
    try std.testing.expectEqual(@as(usize, 0), stale.len);
}

test "prune action: stale cycle removed, active cycle + epic remain (real git repo)" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);
    var c1 = try createCycle(a, std.testing.io, repo, "planx", "keep-task");
    defer c1.deinit(a);
    var c2 = try createCycle(a, std.testing.io, repo, "planx", "drop-task");
    defer c2.deinit(a);

    // Enumerate via the real porcelain, parse it, classify with keep-task active.
    const porcelain = try runGit(a, std.testing.io, &.{ "-C", repo, "worktree", "list", "--porcelain" });
    defer a.free(porcelain);
    const worktrees = try parseWorktreeList(a, porcelain);
    defer freeWorktreeList(a, worktrees);

    // Compare against the canonical repo root — the first porcelain record is
    // the main checkout, whose path git has canonicalized (matching production's
    // reconcileAndPrune, which also derives canon_root from worktrees[0]).
    try std.testing.expect(worktrees.len > 0);
    const canon = worktrees[0].path;
    const active = [_][]const u8{"keep-task"};
    const stale = try staleCycleWorktrees(a, worktrees, canon, "planx", &active);
    defer freeStaleList(a, stale);

    try std.testing.expectEqual(@as(usize, 1), stale.len);
    try std.testing.expectEqualStrings("drop-task", stale[0].task_slug);

    // Tear down the stale cycle (the action reconcileAndPrune performs).
    try teardownCycle(a, std.testing.io, repo, "planx", stale[0].task_slug);

    // drop-task gone; keep-task + epic remain.
    try std.testing.expect(!dirExists(std.testing.io, c2.path));
    try std.testing.expect(!branchListed(a, repo, "cycle/planx/drop-task"));
    try std.testing.expect(dirExists(std.testing.io, c1.path));
    try std.testing.expect(branchListed(a, repo, "cycle/planx/keep-task"));
    try std.testing.expect(dirExists(std.testing.io, epic.path));
    try std.testing.expect(branchListed(a, repo, "epic/planx"));
}

test "git worktree prune: already-deleted cycle dir → no error, metadata cleaned" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);
    var cyc = try createCycle(a, std.testing.io, repo, "planx", "ghost-task");
    defer cyc.deinit(a);

    // Delete the cycle worktree dir out from under git (dir gone, git metadata
    // lingering) — git worktree prune must clear the admin entry without error.
    rmTree(a, cyc.path);
    try std.testing.expect(!dirExists(std.testing.io, cyc.path));

    const prune_out = try runGit(a, std.testing.io, &.{ "-C", repo, "worktree", "prune" });
    a.free(prune_out);

    // After prune, the ghost worktree is no longer listed.
    const porcelain = try runGit(a, std.testing.io, &.{ "-C", repo, "worktree", "list", "--porcelain" });
    defer a.free(porcelain);
    const worktrees = try parseWorktreeList(a, porcelain);
    defer freeWorktreeList(a, worktrees);
    try std.testing.expect(findByBranch(worktrees, "cycle/planx/ghost-task") == null);
    // The epic remains.
    try std.testing.expect(findByBranch(worktrees, "epic/planx") != null);
}

// ---------------------------------------------------------------------------
// M4 fan-in merge tests — task 3177.
// ---------------------------------------------------------------------------

test "mergeCycleIntoEpic: clean merge of a cycle with one commit lands on epic" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);
    var cyc = try createCycle(a, std.testing.io, repo, "planx", "happy-task");
    defer cyc.deinit(a);

    // Commit something on the cycle branch (inside the cycle worktree).
    runGitIn(a, cyc.path, &.{ "config", "user.email", "test@planar.local" });
    runGitIn(a, cyc.path, &.{ "config", "user.name", "Planar Test" });
    runGitIn(a, cyc.path, &.{ "commit", "--allow-empty", "-m", "worker commit" });

    // Configure the epic worktree's identity so the merge commit succeeds.
    runGitIn(a, epic.path, &.{ "config", "user.email", "test@planar.local" });
    runGitIn(a, epic.path, &.{ "config", "user.name", "Planar Test" });

    const result = try mergeCycleIntoEpic(a, std.testing.io, repo, "planx", "happy-task", "merge: happy-task");
    try std.testing.expectEqual(MergeResult.clean, result);

    // Epic tip now reachable-from-includes the cycle tip (the merge commit
    // parent-walk includes cycle's tip). Cheapest probe: `git merge-base
    // --is-ancestor cycle/planx/happy-task epic/planx` exits 0.
    const is_anc = try gitOk(a, std.testing.io, &.{
        "-C",
        repo,
        "merge-base",
        "--is-ancestor",
        "cycle/planx/happy-task",
        "epic/planx",
    });
    try std.testing.expect(is_anc);
}

test "mergeCycleIntoEpic: conflict path returns .conflict and aborts (epic worktree clean)" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    // Configure identity on the main repo so subsequent commits succeed.
    runGitIn(a, repo, &.{ "config", "user.email", "test@planar.local" });
    runGitIn(a, repo, &.{ "config", "user.name", "Planar Test" });

    var epic = try ensureEpic(a, std.testing.io, repo, "conf");
    defer epic.deinit(a);
    var cyc = try createCycle(a, std.testing.io, repo, "conf", "ct1");
    defer cyc.deinit(a);

    // Identity inside both worktrees.
    runGitIn(a, epic.path, &.{ "config", "user.email", "test@planar.local" });
    runGitIn(a, epic.path, &.{ "config", "user.name", "Planar Test" });
    runGitIn(a, cyc.path, &.{ "config", "user.email", "test@planar.local" });
    runGitIn(a, cyc.path, &.{ "config", "user.name", "Planar Test" });

    // Both sides modify the same file with different content → real conflict.
    // Cycle side: write "from-cycle\n" to README.md and commit.
    {
        const wf = std.fs.path.join(a, &.{ cyc.path, "README.md" }) catch unreachable;
        defer a.free(wf);
        std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = wf, .data = "from-cycle\n" }) catch
            return error.SkipZigTest;
    }
    runGitIn(a, cyc.path, &.{ "add", "README.md" });
    runGitIn(a, cyc.path, &.{ "commit", "-m", "cycle writes readme" });

    // Epic side: write "from-epic\n" to README.md and commit.
    {
        const wf = std.fs.path.join(a, &.{ epic.path, "README.md" }) catch unreachable;
        defer a.free(wf);
        std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = wf, .data = "from-epic\n" }) catch
            return error.SkipZigTest;
    }
    runGitIn(a, epic.path, &.{ "add", "README.md" });
    runGitIn(a, epic.path, &.{ "commit", "-m", "epic writes readme" });

    const result = try mergeCycleIntoEpic(a, std.testing.io, repo, "conf", "ct1", "merge attempt");
    try std.testing.expectEqual(MergeResult.conflict, result);

    // After conflict + abort, the epic worktree is clean (no MERGE_HEAD).
    const has_merge_head = std.Io.Dir.cwd().openDir(std.testing.io, epic.path, .{}) catch return;
    var dir = has_merge_head;
    defer dir.close(std.testing.io);
    // Probe for .git/MERGE_HEAD by attempting to open it; absent ⇒ aborted.
    const merge_head_path = std.fs.path.join(a, &.{ epic.path, ".git", "MERGE_HEAD" }) catch unreachable;
    defer a.free(merge_head_path);
    // .git inside a worktree is a file (gitlink), so MERGE_HEAD sits next to
    // the real git dir under .git/worktrees/<name>/. The simplest probe is
    // `git -C <epic.path> rev-parse MERGE_HEAD` — succeeds iff a merge is in
    // progress.
    const in_merge = try gitOk(a, std.testing.io, &.{ "-C", epic.path, "rev-parse", "--verify", "-q", "MERGE_HEAD" });
    try std.testing.expect(!in_merge);
}

test "branchHead: returns null for absent branch, sha for present (task 3180 helper)" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    // Absent branch.
    const absent = try branchHead(a, std.testing.io, repo, "no-such-branch");
    try std.testing.expect(absent == null);

    // Present branch (master, created by initRepo).
    const present = try branchHead(a, std.testing.io, repo, "master");
    try std.testing.expect(present != null);
    defer if (present) |p| a.free(p);
    // SHA is hex, 40 chars (or 64 for sha256; allow either).
    try std.testing.expect(present.?.len == 40 or present.?.len == 64);
    for (present.?) |ch| {
        try std.testing.expect(std.ascii.isHex(ch));
    }
}

test "branchHead: detects a new commit on the cycle branch (commit-presence proxy)" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "cp");
    defer epic.deinit(a);
    var cyc = try createCycle(a, std.testing.io, repo, "cp", "ct");
    defer cyc.deinit(a);

    const before = try branchHead(a, std.testing.io, repo, "cycle/cp/ct");
    try std.testing.expect(before != null);
    defer if (before) |b| a.free(b);

    // Worker commits something on the cycle branch.
    runGitIn(a, cyc.path, &.{ "config", "user.email", "t@p.l" });
    runGitIn(a, cyc.path, &.{ "config", "user.name", "Planar" });
    runGitIn(a, cyc.path, &.{ "commit", "--allow-empty", "-m", "worker did work" });

    const after = try branchHead(a, std.testing.io, repo, "cycle/cp/ct");
    try std.testing.expect(after != null);
    defer if (after) |x| a.free(x);

    try std.testing.expect(!std.mem.eql(u8, before.?, after.?));
}
