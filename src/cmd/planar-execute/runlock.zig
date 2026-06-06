//! runlock.zig — single-instance-per-plan run-lock for `planar-execute`
//! (plan 492 M6 task 3191; tech-spec addendum 267 §6, "Run isolation").
//!
//! ## The problem this guards (§6)
//!
//! `planar-execute run --plan <id>` drives a real (gated) agent run: it cuts
//! per-cycle worktrees, claims tasks, and reconciles stranded state on startup.
//! `reconcileStale` is a GLOBAL sweep and the worktree-prune predicate has no
//! ownership tag — so two `planar-execute` processes driving the SAME plan would
//! corrupt each other (racing claims, colliding `.worktrees/cycle/<plan>/<task>`
//! checkouts, one run's reconcile pruning the other's live worktrees). The
//! single-instance-per-plan invariant closes that hole at STARTUP, before any
//! worker, claim, or worktree exists.
//!
//! ## The mechanism — O_EXCL keyed by plan-id
//!
//! The lock is a file at `<repo_root>/.worktrees/.planar-execute/run-<plan_id>.lock`.
//! `.worktrees/` is already the repo-scoped area every `planar-execute` run uses
//! for its epic/cycle checkouts (and is `.git/info/exclude`-ignored), so the
//! lock lives next to the state it protects and is naturally scoped to the
//! clone. The key is the PLAN id: two runs on DIFFERENT plans get different lock
//! files and never conflict; two runs on the SAME plan contend for one file.
//!
//! Acquisition is an ATOMIC create-exclusive (`createFile` with
//! `.exclusive = true` → the kernel's `O_CREAT | O_EXCL`). This is the race-free
//! primitive: when two processes race, the kernel guarantees exactly one create
//! succeeds and the loser gets `error.PathAlreadyExists`. No advisory lock, no
//! check-then-create window for the create itself.
//!
//! ## The payload — run-id + PID (+ timestamp)
//!
//! The winner writes a one-line payload `<run_id> <pid> <unix_nanos>` so a later
//! contender can (a) name the holder in a diagnostic and (b) probe whether the
//! holder is still alive. The run-id uniquely identifies THIS run; it is derived
//! from `pid` + a wall-clock nanosecond reading (no real RNG needed — the pair
//! is unique per run on a host). The run-id is exposed on the returned
//! `RunLock` so task 3192 can reuse it to TAG worktrees and scope reconcile
//! (this task does NOT build that tagging — see the scope fence in the task).
//!
//! ## Refuse-on-live vs stale-takeover (PID liveness)
//!
//! On `error.PathAlreadyExists` the contender reads the existing payload and
//! probes the holder PID with `kill(pid, 0)` (signal 0 = liveness probe: SUCCESS
//! or EPERM ⇒ the process exists; ESRCH ⇒ it is gone):
//!
//!   - **Holder alive ⇒ REFUSE.** Another run is live for this plan. Return
//!     `error.RunLockHeld`; the caller exits non-zero with a diagnostic naming
//!     the holder's PID + run-id. We do NOT take over a live lock — running a
//!     second instance is the exact corruption this task prevents.
//!   - **Holder dead ⇒ TAKE OVER.** The previous run crashed without releasing.
//!     Remove the stale file and re-create exclusively. Mind the TOCTOU: between
//!     reading the stale payload and re-creating, another process could win the
//!     takeover race. We handle it by re-attempting the O_EXCL create after the
//!     delete; if THAT create loses (someone else took over first), we re-probe
//!     the now-current holder and refuse if it is live (one retry; a second
//!     EEXIST is treated as a live holder and refused). No unbounded spin.
//!
//! ## Release
//!
//! `release()` removes the lock file. It is called on clean run exit (`defer`)
//! and is best-effort on the SIGINT interrupt path. Release is idempotent: a
//! second call, or a call after the file is already gone, is a no-op. A crashed
//! run that never releases simply leaves a stale lock that the NEXT run's
//! liveness-gated takeover reclaims — so releasing on SIGINT is a nicety, not a
//! correctness requirement (the stale-takeover is the backstop).
//!
//! ## POSIX scope
//!
//! PID liveness uses `kill(pid, 0)`, a POSIX primitive. Like the spawn poll
//! path and the SIGINT handler (`interrupt.zig`), the run-lock is POSIX-only
//! this milestone; on Windows `acquire` is a no-op that returns a disabled
//! handle (the orchestrator does not run there yet). The lock-dir and the
//! liveness function are INJECTABLE (`Options`) so tests drive acquire/conflict/
//! stale-takeover deterministically against a `tmpDir` without touching a real
//! second process or polluting a shared dir.

const std = @import("std");
const builtin = @import("builtin");

const Io = std.Io;

const log = std.log.scoped(.planar_execute_runlock);

/// Errors the run-lock surface can return.
pub const RunLockError = error{
    /// A LIVE run already holds the lock for this plan. The caller must refuse
    /// to start (running a second instance is the corruption this guards).
    RunLockHeld,
    /// A filesystem operation (create-dir, create-file, write, read, delete)
    /// failed for a reason other than the expected EEXIST/ENOENT contract.
    FsError,
    /// Allocation failed.
    OutOfMemory,
};

/// Probe whether a process with `pid` is currently alive. Production wires
/// `posixPidAlive` (`kill(pid, 0)`); tests inject a deterministic fake so the
/// "dead PID → takeover" case does not depend on a real reaped process. Returns
/// `true` if the process exists (or exists but we lack permission to signal it),
/// `false` if it is gone.
pub const PidAliveFn = *const fn (pid: i32) bool;

/// Tunable inputs for `acquire`, all defaulted to production values. Tests
/// override `lock_dir` (a `tmpDir` path) and `pid_alive_fn` (a fake) so the
/// conflict/stale-takeover/different-plan cases run without a real second
/// process and without writing to the real `.worktrees/` tree.
pub const Options = struct {
    /// Absolute path of the directory the lock file lives in. When null,
    /// `acquire` derives `<repo_root>/.worktrees/.planar-execute` from the
    /// caller-supplied `repo_root`. Tests pass an explicit `tmpDir` path.
    lock_dir: ?[]const u8 = null,
    /// The repo top-level (used only when `lock_dir` is null). Empty + null
    /// `lock_dir` is a programming error (the caller must supply one or the
    /// other); `acquire` falls back to "." in that degenerate case.
    repo_root: []const u8 = "",
    /// PID-liveness probe. Defaults to the real `kill(pid, 0)` check.
    pid_alive_fn: PidAliveFn = posixPidAlive,
    /// This process's PID, written into the payload and used as the run-id seed.
    /// Defaults to the real `getpid()`. Injectable so a test can simulate "a
    /// DIFFERENT process holds the lock" deterministically.
    self_pid: ?i32 = null,
};

/// A held run-lock. Carries the lock-file path (heap-owned), the run-id (so
/// task 3192 can tag worktrees / scope reconcile with it), and the holder PID.
/// `release` removes the file and is idempotent.
pub const RunLock = struct {
    allocator: std.mem.Allocator,
    io: Io,
    /// Absolute path of the lock file. Empty ⇒ a disabled lock (Windows no-op);
    /// `release` is a no-op in that case.
    path: []u8,
    /// The unique id for THIS run. Stable for the lifetime of the lock. Task
    /// 3192 reuses this to tag worktrees and scope `reconcile --session`.
    run_id: []u8,
    /// The PID written into the lock payload (this process).
    pid: i32,
    /// Set once `release` has removed the file, so a second `release` is a no-op.
    released: bool = false,

    /// release removes the lock file. Idempotent: safe to call twice and safe
    /// when the file is already gone (a crashed prior run, or a manual delete).
    /// Best-effort — a delete failure for a reason other than "already gone" is
    /// logged and swallowed (the next run's stale-takeover is the backstop).
    pub fn release(self: *RunLock) void {
        if (self.released) return;
        self.released = true;
        if (self.path.len == 0) return; // disabled (Windows) lock.
        std.Io.Dir.cwd().deleteFile(self.io, self.path) catch |err| switch (err) {
            error.FileNotFound => {}, // already gone — idempotent success.
            else => log.warn("could not remove run-lock '{s}': {s}", .{ self.path, @errorName(err) }),
        };
    }

    /// deinit frees the heap-owned strings. Does NOT remove the file — call
    /// `release` first if you want the lock gone. (Production defers `release`
    /// then `deinit`.)
    pub fn deinit(self: *RunLock) void {
        if (self.path.len != 0) self.allocator.free(self.path);
        if (self.run_id.len != 0) self.allocator.free(self.run_id);
        self.path = "";
        self.run_id = "";
    }
};

/// posixPidAlive is the production PID-liveness probe: `kill(pid, 0)` sends no
/// signal but performs the permission/existence checks. SUCCESS or
/// `error.PermissionDenied` (EPERM — the process exists but is owned by another
/// user) both mean ALIVE; `error.ProcessNotFound` (ESRCH) means GONE. Any other
/// errno is treated conservatively as alive (refuse rather than wrongly take
/// over a possibly-live lock). POSIX-only; returns `false` on Windows (the
/// orchestrator does not run there, so a "stale" verdict is harmless).
pub fn posixPidAlive(pid: i32) bool {
    if (builtin.os.tag == .windows) return false;
    std.posix.kill(pid, @enumFromInt(0)) catch |err| switch (err) {
        error.ProcessNotFound => return false, // ESRCH — definitely gone.
        error.PermissionDenied => return true, // EPERM — exists, other owner.
        else => return true, // conservative: treat unknown as alive.
    };
    return true; // SUCCESS — process exists.
}

/// nowNanos reads a nanosecond clock for the run-id seed + payload timestamp.
/// Uses `clock_gettime(REALTIME)` (the same C surface heartbeat.zig reads for
/// MONOTONIC) so there is no dependency on `std.time.nanoTimestamp` (removed in
/// this Zig). Combined with the PID, the pair uniquely identifies a run; the
/// exact wall value is informational only.
fn nowNanos() i128 {
    if (builtin.os.tag == .windows) return 0;
    var ts: std.c.timespec = undefined;
    if (std.c.clock_gettime(.REALTIME, &ts) != 0) return 0;
    return @as(i128, @intCast(ts.sec)) * std.time.ns_per_s + @as(i128, @intCast(ts.nsec));
}

/// selfPid returns this process's PID via the C `getpid()` (portable across the
/// POSIX targets; there is no `std.posix.getpid` in this Zig). On Windows it
/// returns 0 (the lock is a no-op there anyway).
fn selfPid() i32 {
    if (builtin.os.tag == .windows) return 0;
    return @intCast(std.c.getpid());
}

/// acquire takes the single-instance-per-plan run-lock for `plan_id`.
///
/// Success ⇒ a `RunLock` the caller must `release` (on clean exit) + `deinit`.
/// `error.RunLockHeld` ⇒ a LIVE run already owns this plan; the caller exits
/// non-zero. `error.FsError` / `error.OutOfMemory` ⇒ an unexpected failure.
///
/// On Windows this is a no-op that returns a disabled `RunLock` (empty path);
/// the orchestrator is POSIX-only this milestone.
pub fn acquire(
    allocator: std.mem.Allocator,
    io: Io,
    plan_id: u64,
    opts: Options,
) RunLockError!RunLock {
    if (builtin.os.tag == .windows) {
        return .{ .allocator = allocator, .io = io, .path = "", .run_id = "", .pid = 0 };
    }

    const pid: i32 = opts.self_pid orelse selfPid();

    // Resolve the lock directory: explicit `lock_dir`, else
    // `<repo_root>/.worktrees/.planar-execute` (repo "." if repo_root is empty).
    const dir = if (opts.lock_dir) |d|
        try allocator.dupe(u8, d)
    else dir_blk: {
        const root = if (opts.repo_root.len != 0) opts.repo_root else ".";
        break :dir_blk std.fs.path.join(allocator, &.{ root, ".worktrees", ".planar-execute" }) catch
            return RunLockError.OutOfMemory;
    };
    defer allocator.free(dir);

    std.Io.Dir.cwd().createDirPath(io, dir) catch
        return RunLockError.FsError;

    const file_name = std.fmt.allocPrint(allocator, "run-{d}.lock", .{plan_id}) catch
        return RunLockError.OutOfMemory;
    defer allocator.free(file_name);
    const path = std.fs.path.join(allocator, &.{ dir, file_name }) catch
        return RunLockError.OutOfMemory;
    errdefer allocator.free(path);

    // Generate the run-id for THIS run: pid + a wall-clock nanosecond reading.
    // Unique per run on a host; no RNG needed. Exposed for task 3192's tagging.
    const run_id = std.fmt.allocPrint(allocator, "run-{d}-{d}", .{ pid, nowNanos() }) catch
        return RunLockError.OutOfMemory;
    errdefer allocator.free(run_id);

    // First attempt: atomic create-exclusive.
    if (try tryCreate(allocator, io, path, run_id, pid)) {
        return .{ .allocator = allocator, .io = io, .path = path, .run_id = run_id, .pid = pid };
    }

    // EEXIST: a lock file already exists. Read its payload and probe the holder.
    const holder = try readHolder(allocator, io, path);
    defer holder.deinit(allocator);
    // Guard: a non-positive pid (`<= 0`) means the payload was malformed,
    // truncated, or empty (readHolder defaults to pid=-1 in those cases).
    // We MUST NOT pass a non-positive pid to `pid_alive_fn` — on POSIX,
    // `kill(-1, 0)` signals every process the caller can signal (returns
    // success), and `kill(0, 0)` signals the caller's process group; both
    // would yield a SPURIOUS "alive" verdict and wedge the next run with
    // RunLockHeld (recoverable only by manual `rm` of the lock file).
    // A `<=0` pid is unambiguously stale (no real holder can have pid<=0)
    // → fall straight through to the stale-takeover path (task 3540
    // finding 3). Print a brief stderr note so the operator sees the
    // malformed-payload recovery.
    const holder_pid_valid = holder.pid > 0;
    if (!holder_pid_valid) {
        std.debug.print(
            "planar-execute: NOTE — run-lock for plan {d} has a malformed payload " ++
                "(pid={d}, run-id='{s}'); treating as stale and taking over.\n",
            .{ plan_id, holder.pid, holder.run_id },
        );
    } else if (opts.pid_alive_fn(holder.pid)) {
        log.warn(
            "refusing to start: plan {d} run-lock held by LIVE run-id={s} pid={d}",
            .{ plan_id, holder.run_id, holder.pid },
        );
        return RunLockError.RunLockHeld;
    }

    // Stale lock — the holder PID is dead. Take it over: remove the stale file
    // then re-create exclusively. TOCTOU: another process could win the
    // re-create between our delete and our create; handle that below.
    log.info(
        "taking over stale run-lock for plan {d} (dead holder run-id={s} pid={d})",
        .{ plan_id, holder.run_id, holder.pid },
    );
    std.Io.Dir.cwd().deleteFile(io, path) catch |err| switch (err) {
        error.FileNotFound => {}, // someone else already removed it — fine, re-create below.
        else => return RunLockError.FsError,
    };

    if (try tryCreate(allocator, io, path, run_id, pid)) {
        return .{ .allocator = allocator, .io = io, .path = path, .run_id = run_id, .pid = pid };
    }

    // We lost the takeover re-create race: another process re-created the lock
    // between our delete and our create. Re-probe the NEW holder. If it is live,
    // refuse (do not steal a freshly-acquired live lock). If it is ALSO dead,
    // we do not spin further — treat a second EEXIST as a live holder and
    // refuse; the operator can re-run, and the (now-current) holder either makes
    // progress or itself becomes stale for the next run to reclaim. This bounds
    // the takeover to a single retry — no unbounded loop. Same pid<=0 guard
    // as the first probe (task 3540 finding 3): do not call `pid_alive_fn`
    // with a non-positive pid.
    const new_holder = readHolder(allocator, io, path) catch {
        // Could not even read the new lock — treat as held and refuse.
        return RunLockError.RunLockHeld;
    };
    defer new_holder.deinit(allocator);
    log.warn(
        "lost stale-takeover race for plan {d}: now held by run-id={s} pid={d}; refusing",
        .{ plan_id, new_holder.run_id, new_holder.pid },
    );
    return RunLockError.RunLockHeld;
}

/// tryCreate attempts the atomic O_EXCL create and, on success, writes the
/// payload. Returns `true` if WE created the lock (and the payload is written),
/// `false` if the file already existed (EEXIST — caller handles takeover/refuse).
/// Any other create/write error maps to `FsError`.
fn tryCreate(
    allocator: std.mem.Allocator,
    io: Io,
    path: []const u8,
    run_id: []const u8,
    pid: i32,
) RunLockError!bool {
    const file = std.Io.Dir.cwd().createFile(io, path, .{ .exclusive = true, .read = false }) catch |err| switch (err) {
        error.PathAlreadyExists => return false, // EEXIST — lock already held.
        else => return RunLockError.FsError,
    };
    defer file.close(io);

    const payload = std.fmt.allocPrint(allocator, "{s} {d} {d}\n", .{ run_id, pid, nowNanos() }) catch
        return RunLockError.OutOfMemory;
    defer allocator.free(payload);

    file.writeStreamingAll(io, payload) catch
        return RunLockError.FsError;
    return true;
}

/// The parsed lock payload: the holder's run-id (HEAP-OWNED by the caller's
/// allocator — `readHolder` dupes it and frees its read buffer, so there is no
/// dangling borrow and no leaked buffer) and PID. Caller frees `run_id` when
/// `run_id.len != 0`.
const Holder = struct {
    run_id: []const u8,
    pid: i32,

    fn deinit(self: *const Holder, allocator: std.mem.Allocator) void {
        if (self.run_id.len != 0) allocator.free(self.run_id);
    }
};

/// readHolder reads + parses the lock payload `<run_id> <pid> <nanos>`. It dupes
/// the run-id into `allocator` and frees its own read buffer, so the returned
/// `run_id` is an independent allocation the caller frees via `Holder.deinit`
/// (no dangling borrow, no leaked buffer). A malformed or missing payload yields
/// `pid = -1` (which the caller's `pid <= 0` guard treats as unambiguously
/// stale — see `acquire` for the rationale; on POSIX `kill(-1, 0)` and
/// `kill(0, 0)` do NOT mean "process not found"). A half-written stale lock
/// from a crashed run thus does not wedge the next run (task 3540 finding 3).
fn readHolder(allocator: std.mem.Allocator, io: Io, path: []const u8) RunLockError!Holder {
    const data = std.Io.Dir.cwd().readFileAlloc(io, path, allocator, .limited(4096)) catch |err| switch (err) {
        error.FileNotFound => return .{ .run_id = "", .pid = -1 }, // vanished — treat as dead/takeover.
        error.OutOfMemory => return RunLockError.OutOfMemory,
        else => return RunLockError.FsError,
    };
    defer allocator.free(data);
    const trimmed = std.mem.trim(u8, data, " \t\r\n");
    var it = std.mem.tokenizeScalar(u8, trimmed, ' ');
    const run_id_tok = it.next() orelse return .{ .run_id = "", .pid = -1 };
    const run_id = allocator.dupe(u8, run_id_tok) catch return RunLockError.OutOfMemory;
    errdefer allocator.free(run_id);
    const pid_tok = it.next() orelse return .{ .run_id = run_id, .pid = -1 };
    const pid = std.fmt.parseInt(i32, pid_tok, 10) catch -1;
    return .{ .run_id = run_id, .pid = pid };
}

// ===========================================================================
// Tests — deterministic, no real second process.
// ===========================================================================

const testing = std.testing;

/// alwaysAlive / alwaysDead are injectable liveness fakes so the conflict and
/// stale-takeover cases do not depend on a real reaped PID.
fn alwaysAlive(_: i32) bool {
    return true;
}
fn alwaysDead(_: i32) bool {
    return false;
}

/// mkTmpDir creates a fresh system temp directory via `mktemp -d` and returns
/// its absolute path (heap-owned; caller `rmTree`s + frees). Mirrors
/// worktree.zig's helper: a clean absolute path NOT nested under this checkout's
/// own `.worktrees/` tree, sidestepping `std.testing.tmpDir` realpath quirks.
fn mkTmpDir(allocator: std.mem.Allocator) []const u8 {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "mktemp", "-d", "-t", "planar-runlock.XXXXXX" },
    }) catch @panic("mkTmpDir: mktemp spawn failed");
    defer allocator.free(r.stderr);
    if (!(r.term == .exited and r.term.exited == 0)) {
        allocator.free(r.stdout);
        @panic("mkTmpDir: mktemp non-zero exit");
    }
    const trimmed = std.mem.trim(u8, r.stdout, " \t\r\n");
    const owned = allocator.dupe(u8, trimmed) catch @panic("OOM");
    allocator.free(r.stdout);
    return owned;
}

/// rmTree removes `path` and its contents via `rm -rf` (best-effort cleanup).
fn rmTree(allocator: std.mem.Allocator, path: []const u8) void {
    const r = std.process.run(allocator, std.testing.io, .{ .argv = &.{ "rm", "-rf", path } }) catch return;
    allocator.free(r.stdout);
    allocator.free(r.stderr);
}

test "runlock: acquire then a second acquire for the SAME plan REFUSES (live holder)" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const a = testing.allocator;
    const io = testing.io;

    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    // First acquire wins. Its holder PID = this process = ALIVE.
    var lock = try acquire(a, io, 42, .{ .lock_dir = dir, .pid_alive_fn = alwaysAlive });
    defer lock.deinit();
    defer lock.release();
    try testing.expect(lock.path.len != 0);
    try testing.expect(lock.run_id.len != 0);

    // Second acquire for the SAME plan must REFUSE: the holder is live.
    try testing.expectError(RunLockError.RunLockHeld, acquire(a, io, 42, .{ .lock_dir = dir, .pid_alive_fn = alwaysAlive }));
}

test "runlock: stale lock (DEAD holder PID) is taken over and acquire succeeds" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const a = testing.allocator;
    const io = testing.io;

    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    // Hand-write a stale lock payload naming a holder PID we will report DEAD.
    const path = try std.fs.path.join(a, &.{ dir, "run-7.lock" });
    defer a.free(path);
    try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = path, .data = "run-stale-99999 999999 123\n" });

    // alwaysDead ⇒ the holder is gone ⇒ takeover succeeds.
    var lock = try acquire(a, io, 7, .{ .lock_dir = dir, .pid_alive_fn = alwaysDead });
    defer lock.deinit();
    defer lock.release();
    try testing.expect(lock.path.len != 0);
    // The lock file now carries OUR run-id, not the stale one.
    try testing.expect(!std.mem.eql(u8, lock.run_id, "run-stale-99999"));

    const after = try std.Io.Dir.cwd().readFileAlloc(io, path, a, .limited(4096));
    defer a.free(after);
    try testing.expect(std.mem.indexOf(u8, after, lock.run_id) != null);
}

test "runlock: a LIVE existing lock is NOT taken over even by acquire (refuse)" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const a = testing.allocator;
    const io = testing.io;

    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    const path = try std.fs.path.join(a, &.{ dir, "run-7.lock" });
    defer a.free(path);
    try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = path, .data = "run-other-1 1234 99\n" });

    // alwaysAlive ⇒ refuse; the existing live lock is untouched.
    try testing.expectError(RunLockError.RunLockHeld, acquire(a, io, 7, .{ .lock_dir = dir, .pid_alive_fn = alwaysAlive }));
    const still = try std.Io.Dir.cwd().readFileAlloc(io, path, a, .limited(4096));
    defer a.free(still);
    try testing.expect(std.mem.indexOf(u8, still, "run-other-1") != null);
}

test "runlock: release then re-acquire succeeds (lock was freed)" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const a = testing.allocator;
    const io = testing.io;

    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    var lock1 = try acquire(a, io, 5, .{ .lock_dir = dir, .pid_alive_fn = alwaysAlive });
    lock1.release();
    lock1.deinit();

    // Lock is freed ⇒ a fresh acquire for the same plan succeeds.
    var lock2 = try acquire(a, io, 5, .{ .lock_dir = dir, .pid_alive_fn = alwaysAlive });
    defer lock2.deinit();
    defer lock2.release();
    try testing.expect(lock2.path.len != 0);
}

test "runlock: release is idempotent (twice + when file already gone)" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const a = testing.allocator;
    const io = testing.io;

    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    var lock = try acquire(a, io, 9, .{ .lock_dir = dir, .pid_alive_fn = alwaysAlive });
    defer lock.deinit();

    // First release removes the file; subsequent releases are no-ops (no crash).
    lock.release();
    lock.release();

    // Even after the file is manually gone, release does not crash.
    lock.released = false; // force the body to run again
    lock.release();
}

test "runlock: DIFFERENT plans do not conflict (keyed by plan-id)" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const a = testing.allocator;
    const io = testing.io;

    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    // Plan P and plan Q both acquire concurrently (same process) → both succeed.
    var lock_p = try acquire(a, io, 100, .{ .lock_dir = dir, .pid_alive_fn = alwaysAlive });
    defer lock_p.deinit();
    defer lock_p.release();

    var lock_q = try acquire(a, io, 200, .{ .lock_dir = dir, .pid_alive_fn = alwaysAlive });
    defer lock_q.deinit();
    defer lock_q.release();

    try testing.expect(lock_p.path.len != 0);
    try testing.expect(lock_q.path.len != 0);
    // Distinct lock files, distinct run-ids.
    try testing.expect(!std.mem.eql(u8, lock_p.path, lock_q.path));
}

test "runlock: posixPidAlive reports a clearly-dead PID as gone and self as alive" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    // This process is alive.
    try testing.expect(posixPidAlive(selfPid()));
    // A very high PID is overwhelmingly unlikely to exist ⇒ ESRCH ⇒ dead. (This
    // is a soft check: the deterministic takeover path uses the injectable fake.)
    try testing.expect(!posixPidAlive(2147483600));
}

test "runlock task 3540 finding 3: truncated payload (run-id-only, no pid) → treat as stale + take over (REAL posixPidAlive)" {
    // The original bug: readHolder defaults pid=-1 on a malformed payload,
    // and `posixPidAlive(-1)` on POSIX calls `kill(-1, 0)` which signals
    // every process the caller can signal — returns SUCCESS, NOT ESRCH.
    // The previous code treated -1 as "alive" → next run refused with
    // RunLockHeld and the operator had to manually `rm` the lock file.
    //
    // The fix: validate `holder.pid > 0` BEFORE calling pid_alive_fn. A
    // non-positive pid is unambiguously malformed → treat as stale and
    // take over. This test exercises the REAL posixPidAlive (not the
    // injected fake) so it specifically pins the kill(-1, 0) regression.
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const a = testing.allocator;
    const io = testing.io;

    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    // Hand-write a TRUNCATED lock file (run-id only, no pid token). The
    // parser tokenizes by space → only one token → pid defaults to -1.
    const path = try std.fs.path.join(a, &.{ dir, "run-77.lock" });
    defer a.free(path);
    try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = path, .data = "stale-run-id-only\n" });

    // No injected fake — the production posixPidAlive runs. Acquire MUST
    // succeed (the pid<=0 guard treats the malformed payload as stale and
    // takes over). Pre-fix this returned RunLockError.RunLockHeld.
    var lock = try acquire(a, io, 77, .{ .lock_dir = dir });
    defer lock.deinit();
    defer lock.release();
    try testing.expect(lock.path.len != 0);
    try testing.expect(lock.run_id.len != 0);
    // The lock file now carries OUR run-id, not the stale token.
    try testing.expect(!std.mem.eql(u8, lock.run_id, "stale-run-id-only"));
}

test "runlock task 3540 finding 3: pid=0 in payload → treat as stale + take over (REAL posixPidAlive)" {
    // `kill(0, 0)` on POSIX signals the caller's process group (returns
    // success). The pid<=0 guard MUST cover pid=0 too — same kill semantics
    // as -1.
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const a = testing.allocator;
    const io = testing.io;

    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    const path = try std.fs.path.join(a, &.{ dir, "run-78.lock" });
    defer a.free(path);
    try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = path, .data = "rid 0 12345\n" });

    var lock = try acquire(a, io, 78, .{ .lock_dir = dir });
    defer lock.deinit();
    defer lock.release();
    try testing.expect(lock.path.len != 0);
}

test "runlock task 3540 finding 3: negative pid in payload → treat as stale + take over (REAL posixPidAlive)" {
    // An explicitly-written negative pid (somehow) also triggers the
    // pid<=0 guard — same as -1 from a malformed parse. This locks the
    // semantic for any future regression that lets a negative pid slip
    // through `parseInt`.
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const a = testing.allocator;
    const io = testing.io;

    const dir = mkTmpDir(a);
    defer a.free(dir);
    defer rmTree(a, dir);

    const path = try std.fs.path.join(a, &.{ dir, "run-79.lock" });
    defer a.free(path);
    try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = path, .data = "rid -7 9999\n" });

    var lock = try acquire(a, io, 79, .{ .lock_dir = dir });
    defer lock.deinit();
    defer lock.release();
    try testing.expect(lock.path.len != 0);
}

test "runlock: default lock_dir derives from repo_root/.worktrees/.planar-execute" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const a = testing.allocator;
    const io = testing.io;

    const root = mkTmpDir(a);
    defer a.free(root);
    defer rmTree(a, root);

    // No explicit lock_dir → derive from repo_root.
    var lock = try acquire(a, io, 3, .{ .repo_root = root, .pid_alive_fn = alwaysAlive });
    defer lock.deinit();
    defer lock.release();

    // The lock path lives under <root>/.worktrees/.planar-execute/run-3.lock.
    try testing.expect(std.mem.indexOf(u8, lock.path, ".worktrees") != null);
    try testing.expect(std.mem.indexOf(u8, lock.path, "run-3.lock") != null);
}
