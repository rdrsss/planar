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

const runlock = @import("runlock.zig");

/// Re-export of the run-lock's PID-liveness probe type so the prune predicate
/// can be driven by an injected fake in tests (and the real `kill(pid, 0)`
/// probe in production). The whole point of M6 run isolation is a SHARED
/// liveness primitive between the lock and the prune.
pub const PidAliveFn = runlock.PidAliveFn;

/// posixPidAlive is the production liveness probe (the run-lock's `kill(pid,0)`
/// check). Re-exported so callers wire the same conservative semantics:
/// SUCCESS / EPERM ⇒ alive, ESRCH ⇒ dead, unknown ⇒ conservatively alive.
pub const posixPidAlive = runlock.posixPidAlive;

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
// Owner marker — run-id + PID ownership tag (plan 492 M6 task 3192 part 2).
//
// Each cycle worktree carries an OWNER MARKER written by the run that created
// it: `<cycle-worktree>/.planar-execute/owner`, a one-line
// `<run_id> <pid> <unix_nanos>` payload (the same shape the run-lock writes).
// The marker is the authoritative ownership tag the prune predicate reads — it
// REPLACES the imprecise "no active claim" signal. The `.planar-execute/`
// subdir keeps the marker out of the worker's tracked tree (it lives under the
// worktree dir, which is already `.git/info/exclude`-ignored at the repo root).
//
// The prune predicate is CONSERVATIVE (load-bearing — a false positive destroys
// in-flight work): a cycle worktree is prunable ONLY IF its marker names a
// DIFFERENT run (`owner_run_id != current_run_id`) AND that run is DEAD
// (`!pid_alive(owner_pid)`). The CURRENT run's own worktree, a LIVE foreign
// run's worktree, and an UNKNOWN (missing / unreadable) marker are NEVER
// pruned.
// ---------------------------------------------------------------------------

/// Relative path (under a cycle worktree) of the run-local metadata subdir.
pub const owner_marker_subdir = ".planar-execute";

/// Relative path (under a cycle worktree) of the owner marker file itself.
pub const owner_marker_rel = owner_marker_subdir ++ std.fs.path.sep_str ++ "owner";

/// A cycle worktree's parsed ownership tag: the creating run's id (heap-owned)
/// and PID. Free `run_id` via `Owner.deinit` when `run_id.len != 0`.
pub const Owner = struct {
    run_id: []const u8,
    pid: i32,

    pub fn deinit(self: *const Owner, allocator: std.mem.Allocator) void {
        if (self.run_id.len != 0) allocator.free(self.run_id);
    }
};

/// writeOwnerMarker creates `<worktree_path>/.planar-execute/owner` with the
/// `<run_id> <pid> <unix_nanos>` payload. Idempotent on the directory create
/// (PathAlreadyExists is tolerated); the file is overwritten if it exists.
///
/// Called by `createCycle` immediately after the worktree is checked out so the
/// ownership tag is present before any worker touches the tree.
pub fn writeOwnerMarker(
    allocator: std.mem.Allocator,
    io: Io,
    worktree_path: []const u8,
    run_id: []const u8,
    pid: i32,
) WorktreeError!void {
    const dir = std.fs.path.join(allocator, &.{ worktree_path, owner_marker_subdir }) catch
        return WorktreeError.OutOfMemory;
    defer allocator.free(dir);
    std.Io.Dir.cwd().createDirPath(io, dir) catch |err| switch (err) {
        error.PathAlreadyExists => {},
        else => return WorktreeError.FsError,
    };

    const file = std.fs.path.join(allocator, &.{ worktree_path, owner_marker_rel }) catch
        return WorktreeError.OutOfMemory;
    defer allocator.free(file);

    const payload = std.fmt.allocPrint(allocator, "{s} {d} {d}\n", .{ run_id, pid, nowNanos() }) catch
        return WorktreeError.OutOfMemory;
    defer allocator.free(payload);

    std.Io.Dir.cwd().writeFile(io, .{ .sub_path = file, .data = payload }) catch
        return WorktreeError.FsError;
}

/// readOwnerMarker reads + parses `<worktree_path>/.planar-execute/owner`.
///
/// Returns `null` when the marker is MISSING or UNREADABLE — UNKNOWN ownership,
/// which the prune predicate treats conservatively (never prune). A malformed
/// payload (no pid token) yields a marker with `pid = -1` (probed as dead),
/// which is still gated by the run-id-mismatch half of the predicate.
///
/// On success the returned `Owner.run_id` is heap-owned; free via `Owner.deinit`.
pub fn readOwnerMarker(
    allocator: std.mem.Allocator,
    io: Io,
    worktree_path: []const u8,
) WorktreeError!?Owner {
    const file = std.fs.path.join(allocator, &.{ worktree_path, owner_marker_rel }) catch
        return WorktreeError.OutOfMemory;
    defer allocator.free(file);

    const data = std.Io.Dir.cwd().readFileAlloc(io, file, allocator, .limited(4096)) catch |err| switch (err) {
        error.FileNotFound => return null, // missing → UNKNOWN ownership.
        error.OutOfMemory => return WorktreeError.OutOfMemory,
        else => return null, // unreadable → UNKNOWN ownership (conservative).
    };
    defer allocator.free(data);

    const trimmed = std.mem.trim(u8, data, " \t\r\n");
    var it = std.mem.tokenizeScalar(u8, trimmed, ' ');
    const run_id_tok = it.next() orelse return null; // empty payload → UNKNOWN.
    const run_id = allocator.dupe(u8, run_id_tok) catch return WorktreeError.OutOfMemory;
    errdefer allocator.free(run_id);
    const pid_tok = it.next() orelse return .{ .run_id = run_id, .pid = -1 };
    const pid = std.fmt.parseInt(i32, pid_tok, 10) catch -1;
    return .{ .run_id = run_id, .pid = pid };
}

/// nowNanos reads a nanosecond wall clock for the owner-marker timestamp.
/// Informational only (the run-id + pid carry the identity); mirrors
/// runlock.zig's reading via `clock_gettime(REALTIME)`.
fn nowNanos() i128 {
    const builtin = @import("builtin");
    if (builtin.os.tag == .windows) return 0;
    var ts: std.c.timespec = undefined;
    if (std.c.clock_gettime(.REALTIME, &ts) != 0) return 0;
    return @as(i128, @intCast(ts.sec)) * std.time.ns_per_s + @as(i128, @intCast(ts.nsec));
}

/// prunableByOwnership is the PURE core of the M6 prune decision.
///
/// Given a cycle worktree's parsed `owner` marker (null ⇒ missing/unreadable),
/// the CURRENT run's id, and a PID-liveness probe, it returns whether the cycle
/// worktree is prunable. The rule (CONSERVATIVE — err toward NOT pruning):
///
///   prune IFF  owner != null
///         AND  owner.run_id != current_run_id   (a FOREIGN run owns it)
///         AND  !pid_alive(owner.pid)            (that foreign run is DEAD)
///
/// NEVER prune when:
///   - owner == null            (UNKNOWN ownership — pre-feature / foreign tool)
///   - owner.run_id == current  (the current run's OWN live worktree)
///   - pid_alive(owner.pid)     (a LIVE run — foreign or not; concurrent work)
///
/// PURE — no git, no DB, no filesystem. Unit-tested with hand-built `Owner`s and
/// the injectable `pid_alive_fn` fakes so the four cases are deterministic.
pub fn prunableByOwnership(
    owner: ?Owner,
    current_run_id: []const u8,
    pid_alive_fn: PidAliveFn,
) bool {
    const o = owner orelse return false; // UNKNOWN ownership → never prune.
    // The current run owns it → never prune (run-id match short-circuits, even
    // if the marker's pid happens to read dead).
    if (std.mem.eql(u8, o.run_id, current_run_id)) return false;
    // A live owner (foreign or not) → never prune (concurrent run owns it).
    if (pid_alive_fn(o.pid)) return false;
    // Foreign AND dead → prunable.
    return true;
}

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

/// createCycle creates a per-task cycle worktree cut from the epic branch and
/// TAGS it with the creating run's ownership marker (run-id + PID).
///
/// Runs `git -C <repo_root> worktree add -b cycle/<plan_slug>/<task_slug>
/// <repo_root>/.worktrees/cycle/<plan_slug>/<task_slug> epic/<plan_slug>` — the
/// cycle branch is cut from `epic/<plan_slug>`, NOT master — then writes
/// `<cycle-worktree>/.planar-execute/owner` containing `<run_id> <pid> <nanos>`.
/// The marker is the ownership tag the M6 prune predicate reads
/// (`prunableByOwnership`): only a DIFFERENT, DEAD run's worktree is prunable.
///
/// `run_id` + `pid` come from the run's `RunLock` (task 3191): the caller
/// threads `run_lock.run_id` + `run_lock.pid` here. (No production front-half
/// call site exists yet — that is M5; this signature carries the tag for it.)
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
    run_id: []const u8,
    pid: i32,
) WorktreeError!Worktree {
    const branch = try cycleBranch(allocator, plan_slug, task_slug);
    errdefer allocator.free(branch);
    const path = try cyclePath(allocator, repo_root, plan_slug, task_slug);
    errdefer allocator.free(path);

    const epic_ref = try epicBranch(allocator, plan_slug);
    defer allocator.free(epic_ref);

    const out = try runGit(allocator, io, &.{ "-C", repo_root, "worktree", "add", "-b", branch, path, epic_ref });
    allocator.free(out);

    // Tag the freshly-created worktree with the owning run's run-id + PID so the
    // M6 prune predicate can distinguish "this run's live worktree" from "a
    // foreign dead run's stranded worktree". Best-effort would silently strip
    // the ownership signal, so a marker write failure is propagated.
    try writeOwnerMarker(allocator, io, path, run_id, pid);

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

/// The result of `mergeCycleIntoEpicCapture` — the merge `outcome` plus, on
/// conflict, the list of conflicting file paths captured BEFORE the merge was
/// aborted.
///
/// `conflict_files` is heap-owned (caller frees each element AND the slice via
/// `deinit`). It is empty on a clean merge, and on a conflict it carries the
/// `git diff --name-only --diff-filter=U` file list (may be empty if the
/// capture itself failed — the caller falls back to a generic body in that
/// case). The slice is sampled in the conflicted-but-not-yet-aborted window so
/// the operator's question can name the exact files.
pub const MergeCapture = struct {
    outcome: MergeResult,
    conflict_files: []const []const u8,

    /// Frees every captured file path and the backing slice.
    pub fn deinit(self: *MergeCapture, allocator: std.mem.Allocator) void {
        for (self.conflict_files) |f| allocator.free(f);
        allocator.free(self.conflict_files);
        self.conflict_files = &.{};
    }
};

/// mergeCycleIntoEpicCapture is the M9 fan-in variant of `mergeCycleIntoEpic`:
/// identical merge behavior, but on conflict it CAPTURES the conflicting file
/// list (via `git -C <epic_wt> diff --name-only --diff-filter=U`) in the window
/// AFTER git reports the conflict and BEFORE `git merge --abort` discards it.
/// The operator's fan-in `planar question` then names the exact files.
///
/// On a clean merge the returned `MergeCapture.conflict_files` is empty. On a
/// conflict it carries the (possibly empty — capture is best-effort) file list,
/// and the merge has been aborted so the epic worktree is restored, exactly as
/// `mergeCycleIntoEpic` does. The cycle worktree is NOT torn down on conflict.
///
/// The caller owns the returned `MergeCapture` and MUST call `.deinit()`.
pub fn mergeCycleIntoEpicCapture(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    plan_slug: []const u8,
    task_slug: []const u8,
    commit_msg: []const u8,
) WorktreeError!MergeCapture {
    const epic_wt = try epicPath(allocator, repo_root, plan_slug);
    defer allocator.free(epic_wt);

    const cycle_ref = try cycleBranch(allocator, plan_slug, task_slug);
    defer allocator.free(cycle_ref);

    const merged = gitOk(allocator, io, &.{
        "-C", epic_wt,    "merge",   "--no-ff",
        "-m", commit_msg, cycle_ref,
    }) catch |err| return err;

    const empty: []const []const u8 = &.{};
    if (merged) return .{ .outcome = .clean, .conflict_files = empty };

    // Conflict: capture the unmerged file list BEFORE aborting. Best-effort —
    // a capture failure leaves `conflict_files` empty and the caller falls back
    // to a generic question body. `--diff-filter=U` selects unmerged paths.
    const files: []const []const u8 = captureConflictFiles(allocator, io, epic_wt) catch empty;

    // Abort so the epic worktree is restored (same as mergeCycleIntoEpic).
    _ = gitOk(allocator, io, &.{ "-C", epic_wt, "merge", "--abort" }) catch {};
    return .{ .outcome = .conflict, .conflict_files = files };
}

/// captureConflictFiles runs `git -C <epic_wt> diff --name-only --diff-filter=U`
/// and parses the newline-separated output into an owned slice of owned file
/// paths. Used by `mergeCycleIntoEpicCapture` in the conflicted-not-yet-aborted
/// window. Returns an empty slice when there is no output.
fn captureConflictFiles(
    allocator: std.mem.Allocator,
    io: Io,
    epic_wt: []const u8,
) WorktreeError![]const []const u8 {
    const out = try runGit(allocator, io, &.{ "-C", epic_wt, "diff", "--name-only", "--diff-filter=U" });
    defer allocator.free(out);

    var list = std.ArrayList([]u8).empty;
    errdefer {
        for (list.items) |f| allocator.free(f);
        list.deinit(allocator);
    }

    var lines = std.mem.splitScalar(u8, out, '\n');
    while (lines.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r\n");
        if (line.len == 0) continue;
        const owned = allocator.dupe(u8, line) catch return WorktreeError.OutOfMemory;
        list.append(allocator, owned) catch {
            allocator.free(owned);
            return WorktreeError.OutOfMemory;
        };
    }
    return list.toOwnedSlice(allocator) catch return WorktreeError.OutOfMemory;
}

/// epicWorktreePresent reports whether the M9 fan-in topology is present for
/// `plan_slug` in `repo_root`: BOTH the epic branch (`epic/<plan_slug>`) exists
/// AND its worktree directory (`<repo_root>/.worktrees/epic/<plan_slug>`) is a
/// real directory. Returns false (skip fan-in) when either is absent — e.g. a
/// classic in-pwd run or a caller that did not prepare the worktree topology.
///
/// Best-effort: any probe error degrades to false (skip the merge) rather than
/// surfacing — a fan-in MUST NOT fail the run because the topology check could
/// not complete.
pub fn epicWorktreePresent(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    plan_slug: []const u8,
) bool {
    const branch = epicBranch(allocator, plan_slug) catch return false;
    defer allocator.free(branch);
    const exists = branchExists(allocator, io, repo_root, branch) catch return false;
    if (!exists) return false;

    const path = epicPath(allocator, repo_root, plan_slug) catch return false;
    defer allocator.free(path);
    var d = std.Io.Dir.cwd().openDir(io, path, .{}) catch return false;
    d.close(io);
    return true;
}

/// cycleBranchPresent reports whether the cycle branch
/// (`cycle/<plan_slug>/<task_slug>`) exists in `repo_root`. The fan-in topology
/// guard requires BOTH the epic worktree AND the cycle branch (a completed
/// worker committed onto it). Best-effort: a probe error degrades to false.
pub fn cycleBranchPresent(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    plan_slug: []const u8,
    task_slug: []const u8,
) bool {
    const branch = cycleBranch(allocator, plan_slug, task_slug) catch return false;
    defer allocator.free(branch);
    return branchExists(allocator, io, repo_root, branch) catch false;
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

/// gitTopLevel resolves the git top-level (repository root) of the current
/// working directory via `git rev-parse --show-toplevel`. Returns the trimmed
/// absolute path (heap-owned; caller frees) on success, or a WorktreeError when
/// the cwd is not inside a git work tree.
///
/// Used by the gated live-agent driver (plan 492 M4) to root the cycle-branch
/// HEAD samples at the repo that owns the cycle worktree.
pub fn gitTopLevel(allocator: std.mem.Allocator, io: Io) WorktreeError![]u8 {
    const out = try runGit(allocator, io, &.{ "rev-parse", "--show-toplevel" });
    const trimmed = std.mem.trim(u8, out, " \t\r\n");
    if (trimmed.len == out.len) return out;
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

/// staleCycleWorktrees is the M6 ownership-scoped prune decision (task 3192
/// part 2). It REPLACES the M3 claim-based predicate ("no active claim").
///
/// For each MANAGED cycle worktree of `plan_slug`, it reads the worktree's owner
/// marker (`.planar-execute/owner`) and applies the CONSERVATIVE ownership rule
/// via the pure `prunableByOwnership`:
///
///   prune IFF  marker present
///         AND  owner.run_id != current_run_id   (a FOREIGN run)
///         AND  !pid_alive(owner.pid)            (that run is DEAD)
///
/// Invariants (each pinned by a unit test):
///   - The epic worktree is NEVER returned (it persists until epic→master merge).
///   - The CURRENT run's own cycle worktree is NEVER returned (run-id match).
///   - A LIVE foreign run's cycle worktree is NEVER returned (pid_alive true) —
///     this is the catastrophic false-positive the predicate must avoid.
///   - A worktree with a MISSING / unreadable marker is NEVER returned
///     (UNKNOWN ownership → conservative; leave it for the operator).
///   - Foreign-PLAN worktrees are NEVER returned (classifyManaged excludes them).
///
/// `current_run_id` is the run's `RunLock.run_id`. `pid_alive_fn` is the
/// liveness probe (production `posixPidAlive`; tests inject a fake). `io` is
/// needed to read each marker — the file read is a thin shell around the PURE
/// `prunableByOwnership`, which carries the load-bearing decision.
///
/// The caller owns the returned slice and MUST free it via `freeStaleList`.
pub fn staleCycleWorktrees(
    allocator: std.mem.Allocator,
    io: Io,
    all_worktrees: []const WorktreeEntry,
    repo_root: []const u8,
    plan_slug: []const u8,
    current_run_id: []const u8,
    pid_alive_fn: PidAliveFn,
) WorktreeError![]StaleCycle {
    var stale = std.ArrayList(StaleCycle).empty;
    errdefer {
        for (stale.items) |s| allocator.free(s.task_slug);
        stale.deinit(allocator);
    }

    for (all_worktrees) |entry| {
        const managed = classifyManaged(entry, repo_root, plan_slug) orelse continue;
        if (managed.role == .epic) continue; // epic persists, never prune

        // Read the ownership marker for this cycle worktree and apply the pure
        // ownership predicate. A read failure other than missing/unreadable
        // (which readOwnerMarker maps to null) propagates as a hard error.
        const owner = try readOwnerMarker(allocator, io, entry.path);
        defer if (owner) |o| o.deinit(allocator);

        if (!prunableByOwnership(owner, current_run_id, pid_alive_fn)) continue;

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
// Top-level orchestrator: reconcileAndPrune
// ---------------------------------------------------------------------------

/// Outcome of a `reconcileAndPrune` pass.
///
/// `stale` is the number of cycle worktrees the ownership predicate flagged as
/// prunable (owned by a DIFFERENT, DEAD run). `pruned` is the number actually
/// torn down. In a normal (non-dry-run) pass `pruned == stale` once teardown
/// succeeds for every flagged cycle. Under a dry run NO teardown is performed,
/// so `pruned == 0` while `stale` still reports what *would* be pruned — this is
/// what the read-only `doctor` probe reports as `stale_cycles`.
pub const ReconcileResult = struct {
    /// Cycle worktrees classified as prunable (would-be / actual prune targets).
    stale: usize,
    /// Cycle worktrees actually torn down this pass (0 under dry run).
    pruned: usize,
};

/// reconcileAndPrune is the startup reconcile pass (task 3174), hardened for M6
/// run isolation (task 3192 part 2) with a non-destructive dry-run mode (3236).
///
/// It orchestrates (its pure constituents are unit-tested; the live subprocess
/// half is covered end-to-end by `integration_tests/planar_execute_doctor_test.zig`
/// via the `doctor` verb's dry-run probe — task 3236):
///   1. Shell `planar-agent reconcile [--dry-run]` — mark EXPIRED claims stale,
///      close orphaned actions (under `--dry-run` it only reports candidates).
///   2. Enumerate worktrees (`git worktree list --porcelain`) → parse.
///   3. `staleCycleWorktrees` — for each managed cycle worktree, read its owner
///      marker and apply the PURE ownership predicate (`prunableByOwnership`):
///      prune IFF owner-run-id ≠ current AND owner-pid DEAD.
///   4. `teardownCycle` each prunable one (plan-scoped, epic excluded, foreign
///      PLANs excluded). SKIPPED under dry run.
///   5. `git worktree prune` — clear admin entries for cycle dirs already
///      manually deleted (dir gone, git metadata lingering). SKIPPED under dry run.
///
/// `current_run_id` is the live run's `RunLock.run_id` (task 3191); a worktree
/// tagged with this id is the current run's OWN and is NEVER pruned. `pid_alive_fn`
/// is the liveness probe — production passes `posixPidAlive`; tests inject a fake.
///
/// ## Reconcile scoping — why the claim sweep stays GLOBAL (engine F2 deferred)
///
/// Part 1 of this task added `planar-agent reconcile --session <id>` (engine F2)
/// for per-session-scoped claim sweeps. This STARTUP reconcile keeps the GLOBAL
/// sweep — deliberately, because there is no clean session_id to scope by HERE
/// and global is provably safe at this point:
///
///   - The startup pass cleans up a PRIOR run's stranded state. The NEW run does
///     not know the crashed prior run's session id, so there is no honest
///     session_id to pass — fabricating one would scope the sweep to the WRONG
///     session and leave the actual stranded claims un-staled.
///   - The single-instance run-lock (task 3191) already prevents two
///     `planar-execute` runs on the SAME plan, so there is no concurrent
///     same-plan run whose claims the global sweep could wrongly touch.
///   - `reconcileStale` (engine) only marks `status='active' AND lease_expired`
///     claims stale. A LIVE run's claims are kept fresh by its heartbeat thread
///     (task 3187), so they are NEVER expired — therefore the global sweep
///     CANNOT stale a live foreign run's claims even if one existed.
///
/// F2 (`--session`) remains available for a future caller that DOES carry a
/// clean session id (e.g. a same-process re-reconcile that knows its own
/// session). The worktree-side ownership predicate (run-id + PID), not the claim
/// sweep, is what provides the run-isolation guarantee on the worktree side.
///
/// When `dry_run == true` the function performs steps 1–3 (reads only — step 1
/// passes `--dry-run` to `planar-agent reconcile` so it does not write) and
/// returns the computed prune count WITHOUT mutating anything.
///
/// Errors from steps 1–3 propagate (a failed reconcile/read is a hard startup
/// problem). A `teardownCycle` failure on one prunable cycle is NOT
/// logged-and-skipped — we let it propagate so the operator sees the first
/// failure rather than silently leaving half-pruned state.
pub fn reconcileAndPrune(
    allocator: std.mem.Allocator,
    io: Io,
    repo_root: []const u8,
    plan_slug: []const u8,
    current_run_id: []const u8,
    pid_alive_fn: PidAliveFn,
    dry_run: bool,
) WorktreeError!ReconcileResult {
    // 1) Claim side: GLOBAL reconcile (mark expired claims stale). Safe as global
    //    per the scoping rationale in the doc-comment above (run-lock + the
    //    expired-only sweep + heartbeat-fresh live claims). Under a dry run pass
    //    `--dry-run` so the verb only reports candidates (no write).
    const rec_out = if (dry_run)
        try runBin(allocator, io, "planar-agent", &.{ "reconcile", "--dry-run", "--json" }, 256 * 1024)
    else
        try runBin(allocator, io, "planar-agent", &.{ "reconcile", "--json" }, 256 * 1024);
    allocator.free(rec_out);

    // 2) Enumerate + parse worktrees.
    const porcelain = try runGit(allocator, io, &.{ "-C", repo_root, "worktree", "list", "--porcelain" });
    defer allocator.free(porcelain);
    const worktrees = try parseWorktreeList(allocator, porcelain);
    defer freeWorktreeList(allocator, worktrees);

    // 3) Ownership prune predicate. Compare against the CANONICAL repo root: `git
    // worktree list --porcelain` emits symlink-resolved absolute paths, so the
    // path-guard in the classifier must use the same canonical form (a
    // non-canonical `repo_root` — e.g. a `/var/...` mktemp path on macOS that git
    // reports as `/private/var/...` — would otherwise exclude every managed
    // worktree). The FIRST porcelain record is always the main checkout, whose
    // path git has already canonicalized; use it as the canonical root (falling
    // back to the passed `repo_root` if the list is somehow empty).
    const canon_root = if (worktrees.len > 0) worktrees[0].path else repo_root;
    const stale = try staleCycleWorktrees(allocator, io, worktrees, canon_root, plan_slug, current_run_id, pid_alive_fn);
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

/// Injectable PID-liveness fakes for the ownership-prune tests. `aliveFake`
/// reports every PID as ALIVE; `deadFake` reports every PID as DEAD. They make
/// the foreign-live / foreign-dead prune cases deterministic — no real reaped
/// process, no PID guessing in the load-bearing predicate tests.
fn aliveFake(_: i32) bool {
    return true;
}
fn deadFake(_: i32) bool {
    return false;
}

/// A test run-id used by createCycle calls that do not exercise the prune.
const test_run_id = "run-test-1";

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

    var cyc = try createCycle(a, std.testing.io, repo, "planx", "task-one", test_run_id, 4242);
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

    var cyc = try createCycle(a, std.testing.io, repo, "planx", "task-one", test_run_id, 4242);
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

    var cyc = try createCycle(a, std.testing.io, repo, "planx", "aborted-task", test_run_id, 4242);
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

// ---------------------------------------------------------------------------
// M6 ownership-prune tests (task 3192 part 2) — the LOAD-BEARING correctness.
//
// The pure decision `prunableByOwnership` is exhaustively driven for the four
// cases via the injectable liveness fakes (no real PID, no git). The disk-side
// `staleCycleWorktrees` is driven against a real git repo with createCycle-
// written markers, hand-edited to simulate foreign / dead / live / missing.
// ---------------------------------------------------------------------------

test "prunableByOwnership: missing marker (UNKNOWN ownership) → NEVER pruned" {
    // The catastrophic false-positive guard: no marker ⇒ never prune (a
    // pre-feature worktree or a foreign tool's; the operator decides).
    try std.testing.expect(!prunableByOwnership(null, "run-current", aliveFake));
    try std.testing.expect(!prunableByOwnership(null, "run-current", deadFake));
}

test "prunableByOwnership: current-run-owned → NEVER pruned (even with a dead pid)" {
    // run-id match short-circuits before the liveness probe even runs.
    const owner = Owner{ .run_id = "run-current", .pid = 999999 };
    try std.testing.expect(!prunableByOwnership(owner, "run-current", deadFake));
    try std.testing.expect(!prunableByOwnership(owner, "run-current", aliveFake));
}

test "prunableByOwnership: foreign + DEAD → PRUNED" {
    const owner = Owner{ .run_id = "run-foreign", .pid = 12345 };
    try std.testing.expect(prunableByOwnership(owner, "run-current", deadFake));
}

test "prunableByOwnership: foreign + LIVE → NEVER pruned (concurrent run owns it)" {
    // The other catastrophic case: a live foreign run (e.g. a concurrent run on
    // another plan) must never have its in-flight worktree pruned.
    const owner = Owner{ .run_id = "run-foreign", .pid = 12345 };
    try std.testing.expect(!prunableByOwnership(owner, "run-current", aliveFake));
}

test "owner marker round-trip: createCycle writes it, readOwnerMarker parses run_id + pid" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);
    var cyc = try createCycle(a, std.testing.io, repo, "planx", "rt-task", "run-rt-77", 4242);
    defer cyc.deinit(a);

    const owner = (try readOwnerMarker(a, std.testing.io, cyc.path)) orelse return error.TestUnexpectedResult;
    defer owner.deinit(a);
    try std.testing.expectEqualStrings("run-rt-77", owner.run_id);
    try std.testing.expectEqual(@as(i32, 4242), owner.pid);
}

test "readOwnerMarker: missing marker yields null (UNKNOWN ownership)" {
    const a = std.testing.allocator;
    const dir = mkTmpRepoDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);
    // No .planar-execute/owner under `dir` → null.
    const owner = try readOwnerMarker(a, std.testing.io, dir);
    try std.testing.expect(owner == null);
}

test "staleCycleWorktrees: current-owned kept, foreign-dead pruned, foreign-live + missing kept (real repo)" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);

    // Four cycle worktrees, each tagged differently:
    //   mine        → marker run_id == current  → NEVER pruned
    //   foreign-dead→ marker run_id != current, pid DEAD → PRUNED
    //   foreign-live→ marker run_id != current, pid LIVE → NEVER pruned
    //   no-marker   → marker removed (UNKNOWN ownership)  → NEVER pruned
    const current = "run-current-1";
    var mine = try createCycle(a, std.testing.io, repo, "planx", "mine", current, 4242);
    defer mine.deinit(a);
    var fdead = try createCycle(a, std.testing.io, repo, "planx", "foreign-dead", "run-foreign-1", 111);
    defer fdead.deinit(a);
    var flive = try createCycle(a, std.testing.io, repo, "planx", "foreign-live", "run-foreign-2", 222);
    defer flive.deinit(a);
    var nomark = try createCycle(a, std.testing.io, repo, "planx", "no-marker", "run-foreign-3", 333);
    defer nomark.deinit(a);

    // Remove the no-marker cycle's owner file to simulate UNKNOWN ownership.
    {
        const mpath = try std.fs.path.join(a, &.{ nomark.path, owner_marker_rel });
        defer a.free(mpath);
        try std.Io.Dir.cwd().deleteFile(std.testing.io, mpath);
    }

    const porcelain = try runGit(a, std.testing.io, &.{ "-C", repo, "worktree", "list", "--porcelain" });
    defer a.free(porcelain);
    const worktrees = try parseWorktreeList(a, porcelain);
    defer freeWorktreeList(a, worktrees);
    try std.testing.expect(worktrees.len > 0);
    const canon = worktrees[0].path;

    // pid_alive: foreign-live's pid (222) reports ALIVE; everything else DEAD.
    const liveOnly222 = struct {
        fn f(pid: i32) bool {
            return pid == 222;
        }
    }.f;

    const stale = try staleCycleWorktrees(a, std.testing.io, worktrees, canon, "planx", current, liveOnly222);
    defer freeStaleList(a, stale);

    // EXACTLY one prunable cycle: foreign-dead. mine (current), foreign-live
    // (alive), no-marker (unknown) are all kept.
    try std.testing.expectEqual(@as(usize, 1), stale.len);
    try std.testing.expectEqualStrings("foreign-dead", stale[0].task_slug);
}

test "staleCycleWorktrees: epic worktree is NEVER pruned (real repo, owner marker present)" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);
    // Even if the epic carried a foreign-dead-looking marker, it is a .epic role
    // and excluded before the ownership read. Write one to prove it is ignored.
    try writeOwnerMarker(a, std.testing.io, epic.path, "run-foreign-X", 111);

    const porcelain = try runGit(a, std.testing.io, &.{ "-C", repo, "worktree", "list", "--porcelain" });
    defer a.free(porcelain);
    const worktrees = try parseWorktreeList(a, porcelain);
    defer freeWorktreeList(a, worktrees);
    const canon = worktrees[0].path;

    // deadFake reports everything dead; the epic still must not be returned.
    const stale = try staleCycleWorktrees(a, std.testing.io, worktrees, canon, "planx", "run-current-1", deadFake);
    defer freeStaleList(a, stale);
    try std.testing.expectEqual(@as(usize, 0), stale.len);
}

test "prune action: foreign-dead cycle removed, current + foreign-live + epic remain (real git repo)" {
    const a = std.testing.allocator;
    if (!gitAvailable(a)) return error.SkipZigTest;

    const repo = mkTmpRepoDir(a);
    defer a.free(repo);
    defer rmTree(a, repo);
    initRepo(a, repo);

    const current = "run-current-1";
    var epic = try ensureEpic(a, std.testing.io, repo, "planx");
    defer epic.deinit(a);
    // c1 = the current run's own worktree (kept); c2 = foreign DEAD (pruned).
    var c1 = try createCycle(a, std.testing.io, repo, "planx", "keep-task", current, 4242);
    defer c1.deinit(a);
    var c2 = try createCycle(a, std.testing.io, repo, "planx", "drop-task", "run-foreign-1", 111);
    defer c2.deinit(a);

    const porcelain = try runGit(a, std.testing.io, &.{ "-C", repo, "worktree", "list", "--porcelain" });
    defer a.free(porcelain);
    const worktrees = try parseWorktreeList(a, porcelain);
    defer freeWorktreeList(a, worktrees);

    try std.testing.expect(worktrees.len > 0);
    const canon = worktrees[0].path;
    // deadFake ⇒ the foreign owner is dead ⇒ drop-task is the lone prune target;
    // keep-task is the current run's own (run-id match) and is never returned.
    const stale = try staleCycleWorktrees(a, std.testing.io, worktrees, canon, "planx", current, deadFake);
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
    var cyc = try createCycle(a, std.testing.io, repo, "planx", "ghost-task", test_run_id, 4242);
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
    var cyc = try createCycle(a, std.testing.io, repo, "planx", "happy-task", test_run_id, 4242);
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
    var cyc = try createCycle(a, std.testing.io, repo, "conf", "ct1", test_run_id, 4242);
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
    var cyc = try createCycle(a, std.testing.io, repo, "cp", "ct", test_run_id, 4242);
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
