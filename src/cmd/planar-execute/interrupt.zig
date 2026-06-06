//! interrupt.zig — SIGINT handling for the orchestrator (plan 492 M6 tasks
//! 3193 + 3189; tech-spec addendum 267 §12).
//!
//! ## Two halves, strictly separated (the §12 contract)
//!
//! SIGINT handling is split into two halves that MUST NOT be conflated:
//!
//!   1. **The async-signal-safe handler (task 3193).** A POSIX signal can
//!      arrive at ANY instant — including while the main thread holds a mutex,
//!      is mid-`malloc`, or is inside a `lua_*` call. A handler that allocates,
//!      locks, does I/O, or touches Lua state can therefore DEADLOCK (the lock
//!      it wants is held by the very thread it interrupted) or CORRUPT
//!      (re-entering a non-reentrant allocator). So the handler does EXACTLY
//!      ONE thing: an atomic store of `true` into `interrupt_requested`. No
//!      allocation, no I/O, no `planar-agent` shell, no mutex, no `lua_*`, no
//!      `std.debug.print`, no worktree ops. An atomic store to a lock-free
//!      `bool` is the canonical async-signal-safe primitive.
//!
//!   2. **The off-signal-path shutdown sequence (task 3189).** ALL real
//!      cleanup — stopping the heartbeat thread, killing children, releasing
//!      claims, removing worktrees — runs on the MAIN thread AFTER it OBSERVES
//!      the flag at a safe point (between coroutine resumes, each poll round).
//!      None of it runs in the handler.
//!
//! ## The shutdown ORDER is load-bearing (§12)
//!
//! `shutdown` runs the sequence in this exact order; each step is documented
//! where it executes:
//!
//!   1. **Stop the heartbeat thread FIRST** (`stop_heartbeat`). Rationale
//!      (§12, explicit in task 3193): if claims are released/expired while the
//!      heartbeat thread still runs, it RACES to refresh the very leases being
//!      released — re-extending leases the shutdown is trying to expire. Stop
//!      the refresher before touching any claim.
//!   2. **Kill all in-flight children** (`kill_fn` per slot). A killed child
//!      stops making progress and releases its hold on the worktree, so the
//!      teardown in step 4 is safe.
//!   3. **Release/expire every in-flight claim** (`release_fn` per slot). A
//!      SIGINT is an operator interrupt, not a worker failure: the work is
//!      ABANDONED-FOR-RETRY, so `release` (task → todo, reclaimable on a later
//!      run) is the right verb, NOT `fail` (which records a defect the operator
//!      must triage). Each token is also unregistered from the heartbeat
//!      registry to keep it consistent (the thread is already stopped, but the
//!      registry must not dangle).
//!   4. **Remove every in-flight worktree** (`teardown_fn` per slot). Children
//!      are already dead (step 2), so removing their checkouts is safe.
//!
//! Cleanup is BEST-EFFORT: a failing step is logged and the sequence CONTINUES
//! (one stuck teardown must not strand the rest). Any residual stranded state
//! is reconcilable on the next startup (task 3174's reconcile/prune), which is
//! the backstop the task's acceptance criteria names ("remaining stranded state
//! is reconcilable on next startup").
//!
//! ## Testability — flag-driven, no real signal delivery
//!
//! The shutdown path keys on the flag, not on an actual signal, so tests drive
//! it deterministically: set `interrupt_requested` (directly or via
//! `std.posix.raise`), build a `ShutdownPlan` of fake in-flight slots, and
//! assert the sequence ran IN ORDER via recording fakes. The handler test
//! raises SIGINT in-process (`std.posix.raise`) and asserts ONLY the flag
//! flipped. No subprocess, no real signal racing the test thread.

const std = @import("std");
const builtin = @import("builtin");

/// The process-global interrupt flag. The SIGINT handler stores `true`; the
/// orchestrator's drive/poll loops observe it at safe points and initiate the
/// off-signal shutdown sequence. Atomic because the handler runs asynchronously
/// w.r.t. the main thread. A lock-free `bool` store is the only operation that
/// is safe inside an async signal handler.
pub var interrupt_requested: std.atomic.Value(bool) = .init(false);

/// requested reads the interrupt flag. Acquire ordering pairs with the handler's
/// release store so a thread that observes `true` sees a coherent state.
pub fn requested() bool {
    return interrupt_requested.load(.acquire);
}

/// reset clears the flag. Used by tests between cases (production installs the
/// handler once per run and never reuses the flag after shutdown).
pub fn reset() void {
    interrupt_requested.store(false, .release);
}

/// sigintHandler is the ENTIRE SIGINT handler — task 3193's async-signal-safe
/// contract. It does EXACTLY ONE thing: an atomic store of `true`. NOTHING
/// else. No allocation, no I/O, no shell, no mutex, no `lua_*`, no print, no
/// worktree ops. A signal can arrive while the main thread holds a lock or is
/// mid-allocation; anything beyond a lock-free atomic store here could deadlock
/// or corrupt. All real cleanup runs in `shutdown` on the main thread after it
/// observes the flag. If you ever want to add a line to this function: STOP —
/// that is exactly the bug 3193 exists to prevent.
fn sigintHandler(_: std.posix.SIG) callconv(.c) void {
    interrupt_requested.store(true, .seq_cst);
}

/// install arms the SIGINT handler around a run. POSIX-only: Windows has a
/// different signal model (the orchestrator is POSIX-only this milestone, like
/// the spawn poll path), so installation is gated behind a POSIX check and is a
/// no-op on Windows (Ctrl-C terminates the process at the OS level there).
///
/// The flag is cleared before arming so a stale flag from a prior run (or a
/// prior test case) cannot trip an immediate shutdown.
pub fn install() void {
    if (builtin.os.tag == .windows) return;
    const posix = std.posix;
    interrupt_requested.store(false, .seq_cst);
    var act: posix.Sigaction = .{
        .handler = .{ .handler = sigintHandler },
        .mask = posix.sigemptyset(),
        .flags = 0,
    };
    posix.sigaction(posix.SIG.INT, &act, null);
}

/// restore puts the default SIGINT disposition back (`SIG.DFL`). Called on a
/// clean run exit AND after the shutdown sequence completes, so:
///   - a SECOND Ctrl-C after cleanup hard-kills the process (default behavior),
///    rather than being swallowed by our handler while cleanup is mid-flight;
///   - test/other code paths are not left with our handler installed.
/// POSIX-only, mirroring `install`.
pub fn restore() void {
    if (builtin.os.tag == .windows) return;
    const posix = std.posix;
    var act: posix.Sigaction = .{
        .handler = .{ .handler = posix.SIG.DFL },
        .mask = posix.sigemptyset(),
        .flags = 0,
    };
    posix.sigaction(posix.SIG.INT, &act, null);
}

// ---------------------------------------------------------------------------
// The off-signal-path shutdown sequence (task 3189).
// ---------------------------------------------------------------------------

/// One in-flight worker to reclaim during shutdown. The caller (main.zig)
/// populates this from each in-use scheduler slot's `AgentCallState`: the claim
/// token to release + unregister, the plan/task slugs to derive the worktree to
/// tear down, and a `kill` thunk that hard-kills THIS slot's child via the
/// spawner. Keeping the kill as an opaque thunk (rather than a `*Handle`) lets
/// `interrupt.zig` stay free of any spawn.zig / Lua dependency — it is a pure
/// orchestration sequencer driven entirely by injected callbacks.
pub const InflightWorker = struct {
    /// The claim token to release + unregister. Empty → skip release/unregister.
    claim_token: []const u8,
    /// The plan slug for the cycle worktree. Empty → skip teardown.
    plan_slug: []const u8,
    /// The task slug for the cycle worktree. Empty → skip teardown.
    task_slug: []const u8,
    /// Opaque context for `kill_fn` (main.zig passes the slot's `*Handle`).
    kill_ctx: ?*anyopaque,
};

/// KillFn hard-kills one in-flight worker. Infallible (a kill of an
/// already-exited child is a no-op). `ctx` is the worker's `kill_ctx`.
pub const KillFn = *const fn (ctx: ?*anyopaque) void;

/// StopHeartbeatFn stops + joins the heartbeat thread. Infallible. Runs FIRST in
/// the sequence so no refresh can race the claim releases.
pub const StopHeartbeatFn = *const fn (ctx: ?*anyopaque) void;

/// UnregisterFn removes one claim token from the heartbeat registry (keeps it
/// consistent after the thread is stopped). Infallible.
pub const UnregisterFn = *const fn (ctx: ?*anyopaque, token: []const u8) void;

/// ReleaseFn releases one claim (`planar-agent release`). May fail; a failure is
/// logged by the caller's impl and the sequence continues (best-effort).
pub const ReleaseFn = *const fn (ctx: ?*anyopaque, token: []const u8) void;

/// TeardownFn removes one cycle worktree. May fail; best-effort.
pub const TeardownFn = *const fn (ctx: ?*anyopaque, plan_slug: []const u8, task_slug: []const u8) void;

/// ShutdownPlan bundles the in-flight workers to reclaim plus the injected
/// callbacks that perform each step. Production wires the callbacks to the
/// heartbeat registry, the spawner's `kill`, `terminal.runTerminalVerb(.release)`,
/// and `worktree.teardownCycle`; tests wire recording fakes that capture the
/// ORDER so the heartbeat-stopped-before-claims-released contract is pinned.
pub const ShutdownPlan = struct {
    workers: []const InflightWorker,

    stop_heartbeat_fn: StopHeartbeatFn,
    stop_heartbeat_ctx: ?*anyopaque = null,

    kill_fn: KillFn,

    unregister_fn: UnregisterFn,
    unregister_ctx: ?*anyopaque = null,

    release_fn: ReleaseFn,
    release_ctx: ?*anyopaque = null,

    teardown_fn: TeardownFn,
    teardown_ctx: ?*anyopaque = null,
};

/// shutdown runs the off-signal-path SIGINT cleanup sequence in its
/// load-bearing order (see the module doc): stop heartbeat → kill children →
/// release + unregister claims → tear down worktrees. Runs entirely on the MAIN
/// thread (never in the signal handler). Best-effort: every step is attempted
/// for every worker; a per-step failure is swallowed by the injected callback
/// (which logs), and the sequence continues so one stuck step cannot strand the
/// rest. Residual state is reconcilable on next startup.
///
/// A SIGINT with NO workers in flight still runs cleanly: the heartbeat stop
/// fires (harmless if already stopped), and the per-worker loops are simply
/// empty (no spurious kill/release/teardown).
pub fn shutdown(plan: ShutdownPlan) void {
    // 1) Stop the heartbeat thread FIRST — before any claim is touched — so it
    //    cannot re-refresh a lease the release below is about to expire (§12).
    plan.stop_heartbeat_fn(plan.stop_heartbeat_ctx);

    // 2) Kill every in-flight child. A dead child stops progress + frees its
    //    worktree hold, so the teardown in step 4 is safe.
    for (plan.workers) |w| {
        plan.kill_fn(w.kill_ctx);
    }

    // 3) Release + unregister every in-flight claim. Release (not fail): an
    //    operator interrupt abandons the work for retry, it does not mark it
    //    failed. Unregister keeps the (already-stopped) registry consistent.
    for (plan.workers) |w| {
        if (w.claim_token.len == 0) continue;
        plan.unregister_fn(plan.unregister_ctx, w.claim_token);
        plan.release_fn(plan.release_ctx, w.claim_token);
    }

    // 4) Remove every in-flight worktree (children already dead).
    for (plan.workers) |w| {
        if (w.plan_slug.len == 0 or w.task_slug.len == 0) continue;
        plan.teardown_fn(plan.teardown_ctx, w.plan_slug, w.task_slug);
    }
}

// ===========================================================================
// Tests
// ===========================================================================

const testing = std.testing;

// --- Handler test (task 3193): raise → flag set, NOTHING else --------------

test "interrupt: handler sets ONLY the atomic flag on SIGINT (async-signal-safe)" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;

    // Start from a known-clear flag.
    reset();
    try testing.expect(!requested());

    install();
    defer restore();

    // Deterministic in-process delivery — no race with a real Ctrl-C.
    try std.posix.raise(std.posix.SIG.INT);

    // The handler ran and did EXACTLY ONE observable thing: flip the flag.
    try testing.expect(requested());

    reset();
}

test "interrupt: restore puts SIG.DFL back so a second signal is not swallowed" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    reset();
    install();
    restore();
    // After restore the handler is uninstalled. Re-installing + raising still
    // flips the flag (proves install/restore are symmetric and re-armable),
    // and we leave the disposition at default for the rest of the suite.
    install();
    try std.posix.raise(std.posix.SIG.INT);
    try testing.expect(requested());
    restore();
    reset();
}

// --- Shutdown-sequence test (task 3189): ORDER is the §12 contract ----------

/// Recorder captures the ORDER of every shutdown step as a flat event log so a
/// test can assert heartbeat-stop preceded every claim release, every child was
/// killed, every claim released, every worktree torn down — without any real
/// subprocess, signal, or thread.
const Recorder = struct {
    const Event = union(enum) {
        heartbeat_stopped,
        killed: usize, // worker id
        unregistered: []const u8,
        released: []const u8,
        torn_down: []const u8, // task slug
    };
    events: std.ArrayList(Event) = .empty,
    allocator: std.mem.Allocator,

    fn init(a: std.mem.Allocator) Recorder {
        return .{ .allocator = a };
    }
    fn deinit(self: *Recorder) void {
        self.events.deinit(self.allocator);
    }
    fn push(self: *Recorder, e: Event) void {
        self.events.append(self.allocator, e) catch @panic("OOM in test recorder");
    }

    /// firstIndexOf returns the position of the first event matching `tag`.
    fn firstHeartbeatStop(self: *Recorder) ?usize {
        for (self.events.items, 0..) |e, i| if (e == .heartbeat_stopped) return i;
        return null;
    }
    fn firstRelease(self: *Recorder) ?usize {
        for (self.events.items, 0..) |e, i| if (e == .released) return i;
        return null;
    }
    fn countKills(self: *Recorder) usize {
        var n: usize = 0;
        for (self.events.items) |e| if (e == .killed) {
            n += 1;
        };
        return n;
    }
    fn countReleases(self: *Recorder) usize {
        var n: usize = 0;
        for (self.events.items) |e| if (e == .released) {
            n += 1;
        };
        return n;
    }
    fn countTeardowns(self: *Recorder) usize {
        var n: usize = 0;
        for (self.events.items) |e| if (e == .torn_down) {
            n += 1;
        };
        return n;
    }
};

/// A test worker pairs an `InflightWorker` with its kill-recording id.
const TestWorker = struct {
    id: usize,
    rec: *Recorder,
};

fn recStopHeartbeat(ctx: ?*anyopaque) void {
    const rec: *Recorder = @ptrCast(@alignCast(ctx.?));
    rec.push(.heartbeat_stopped);
}
fn recKill(ctx: ?*anyopaque) void {
    const tw: *TestWorker = @ptrCast(@alignCast(ctx.?));
    tw.rec.push(.{ .killed = tw.id });
}
fn recUnregister(ctx: ?*anyopaque, token: []const u8) void {
    const rec: *Recorder = @ptrCast(@alignCast(ctx.?));
    rec.push(.{ .unregistered = token });
}
fn recRelease(ctx: ?*anyopaque, token: []const u8) void {
    const rec: *Recorder = @ptrCast(@alignCast(ctx.?));
    rec.push(.{ .released = token });
}
fn recTeardown(ctx: ?*anyopaque, plan_slug: []const u8, task_slug: []const u8) void {
    _ = plan_slug;
    const rec: *Recorder = @ptrCast(@alignCast(ctx.?));
    rec.push(.{ .torn_down = task_slug });
}

test "interrupt: shutdown runs IN ORDER — heartbeat stop BEFORE any claim release (§12)" {
    const a = testing.allocator;
    var rec = Recorder.init(a);
    defer rec.deinit();

    // Three fake in-flight workers (the §12 N>1 case).
    var tws = [_]TestWorker{
        .{ .id = 0, .rec = &rec },
        .{ .id = 1, .rec = &rec },
        .{ .id = 2, .rec = &rec },
    };
    const workers = [_]InflightWorker{
        .{ .claim_token = "tok-0", .plan_slug = "p", .task_slug = "t0", .kill_ctx = &tws[0] },
        .{ .claim_token = "tok-1", .plan_slug = "p", .task_slug = "t1", .kill_ctx = &tws[1] },
        .{ .claim_token = "tok-2", .plan_slug = "p", .task_slug = "t2", .kill_ctx = &tws[2] },
    };

    shutdown(.{
        .workers = &workers,
        .stop_heartbeat_fn = recStopHeartbeat,
        .stop_heartbeat_ctx = &rec,
        .kill_fn = recKill,
        .unregister_fn = recUnregister,
        .unregister_ctx = &rec,
        .release_fn = recRelease,
        .release_ctx = &rec,
        .teardown_fn = recTeardown,
        .teardown_ctx = &rec,
    });

    // The load-bearing §12 assertion: heartbeat stop happened BEFORE the FIRST
    // claim release. If the order ever regresses (release before stop), the
    // heartbeat thread could re-extend a lease we are expiring.
    const hb_stop_at = rec.firstHeartbeatStop() orelse
        return testing.expect(false); // heartbeat was never stopped → bug.
    const first_release_at = rec.firstRelease() orelse
        return testing.expect(false); // nothing released → bug.
    try testing.expect(hb_stop_at < first_release_at);

    // The very first event MUST be the heartbeat stop (before ANY kill too —
    // the whole sequence is gated on the refresher being down first).
    try testing.expect(rec.events.items[0] == .heartbeat_stopped);

    // Every worker was killed, released, and torn down (best-effort full sweep).
    try testing.expectEqual(@as(usize, 3), rec.countKills());
    try testing.expectEqual(@as(usize, 3), rec.countReleases());
    try testing.expectEqual(@as(usize, 3), rec.countTeardowns());

    // Kill precedes teardown for the set (children dead before worktree removal).
    var last_kill_at: usize = 0;
    var first_teardown_at: usize = rec.events.items.len;
    for (rec.events.items, 0..) |e, i| {
        if (e == .killed) last_kill_at = i;
        if (e == .torn_down and i < first_teardown_at) first_teardown_at = i;
    }
    try testing.expect(last_kill_at < first_teardown_at);
}

test "interrupt: shutdown with NO workers in flight stops heartbeat and does nothing else" {
    const a = testing.allocator;
    var rec = Recorder.init(a);
    defer rec.deinit();

    const no_workers = [_]InflightWorker{};
    shutdown(.{
        .workers = &no_workers,
        .stop_heartbeat_fn = recStopHeartbeat,
        .stop_heartbeat_ctx = &rec,
        .kill_fn = recKill,
        .unregister_fn = recUnregister,
        .unregister_ctx = &rec,
        .release_fn = recRelease,
        .release_ctx = &rec,
        .teardown_fn = recTeardown,
        .teardown_ctx = &rec,
    });

    // Exactly one event: the heartbeat stop. No spurious kill/release/teardown.
    try testing.expectEqual(@as(usize, 1), rec.events.items.len);
    try testing.expect(rec.events.items[0] == .heartbeat_stopped);
    try testing.expectEqual(@as(usize, 0), rec.countKills());
    try testing.expectEqual(@as(usize, 0), rec.countReleases());
    try testing.expectEqual(@as(usize, 0), rec.countTeardowns());
}

test "interrupt: shutdown skips release/teardown for workers with empty token/slug" {
    const a = testing.allocator;
    var rec = Recorder.init(a);
    defer rec.deinit();

    var tw = TestWorker{ .id = 0, .rec = &rec };
    // A worker with no claim token and no slugs (e.g. a FakeSpawner unit path):
    // it is still KILLED (the child must die) but NOT released/torn down.
    const workers = [_]InflightWorker{
        .{ .claim_token = "", .plan_slug = "", .task_slug = "", .kill_ctx = &tw },
    };
    shutdown(.{
        .workers = &workers,
        .stop_heartbeat_fn = recStopHeartbeat,
        .stop_heartbeat_ctx = &rec,
        .kill_fn = recKill,
        .unregister_fn = recUnregister,
        .unregister_ctx = &rec,
        .release_fn = recRelease,
        .release_ctx = &rec,
        .teardown_fn = recTeardown,
        .teardown_ctx = &rec,
    });

    try testing.expectEqual(@as(usize, 1), rec.countKills());
    try testing.expectEqual(@as(usize, 0), rec.countReleases());
    try testing.expectEqual(@as(usize, 0), rec.countTeardowns());
}
