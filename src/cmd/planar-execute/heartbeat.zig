//! heartbeat.zig — the single, preemptive heartbeat thread (plan 492 M6 task
//! 3187).
//!
//! ## Why this exists (§ Failure handling of the tech-spec)
//!
//! A live worker holds a claim lease with a finite TTL. If nothing refreshes
//! that lease it expires and (with F1, already landed) the task becomes
//! re-queueable. That expiry is exactly what we WANT when a worker crashes —
//! its heartbeats stop, the lease lapses, the task is reclaimed. But a
//! long-running, still-alive worker must NOT be reclaimed mid-flight. So a
//! single shared heartbeat thread refreshes every LIVE claim token at TTL/2
//! (`planar-agent heartbeat --claim <tok> --ttl <ttl>`). One thread, all live
//! tokens — owning liveness because it owns the refresh cadence.
//!
//! ## The cardinal concurrency rule (§ Concurrency model)
//!
//! Two concurrency models coexist and are kept STRICTLY separate: the Lua
//! coroutine scheduler is cooperative (single-threaded); the heartbeat thread
//! is a preemptive Zig OS thread running independently. **No Lua state is
//! touched from more than one thread.** This module touches NOTHING Lua — not
//! the `lua_State`, not the scheduler's Lua-facing structures, not the
//! `AgentCallState`. It touches ONLY claim-token strings (duped copies it owns)
//! and shells `planar-agent heartbeat`.
//!
//! ## The synchronization contract — why a DEDICATED registry
//!
//! The main thread owns each worker's claim token inside `AgentCallState`
//! (main.zig), which it FREES when the scheduler slot is released. If this
//! thread read those slices directly that would be a use-after-free ACROSS
//! threads (the main thread frees the token while the heartbeat thread is
//! mid-refresh). So this registry owns its OWN duped copies:
//!
//!   - `register(token)`   — dupes `token` into the registry under the mutex
//!                           (called by the main thread when a worker goes live).
//!   - `unregister(token)` — finds + frees the matching duped token under the
//!                           mutex (called when the worker reaches terminal /
//!                           the slot is released).
//!   - the heartbeat thread, each tick, takes the mutex, SNAPSHOTS the live
//!     tokens into a freshly-duped list, RELEASES the mutex, then shells
//!     `planar-agent heartbeat` for each WITHOUT holding the mutex (subprocess
//!     I/O must never block the main thread's register/unregister).
//!
//! Lock ordering: there is exactly ONE lock (`mutex`). Every access to `tokens`
//! is under it. No token slice escapes the lock except as an owned copy. No
//! other lock is ever taken while holding it (the heartbeat subprocess runs
//! after the lock is dropped), so there is no lock-ordering hazard.
//!
//! ## Zig-0.16 synchronization note
//!
//! In Zig 0.16 the blocking sync primitives live on `std.Io` (`Io.Mutex`,
//! `Io.Condition`) and every lock/wait/signal operation takes the `Io`. The
//! registry therefore captures the run's `Io` at `init`; it MUST be a
//! thread-safe Io (the binary's threaded blocking Io is) because the heartbeat
//! thread and the main thread both touch the mutex.
//!
//! ## Clean start / stop / join (the primitive task 3193 calls on SIGINT)
//!
//! `start()` spawns the OS thread. `stop()` sets an atomic stop flag and
//! `join()`s the thread. The thread observes the flag PROMPTLY even mid-sleep:
//! it sleeps in short slices (`STOP_CHECK_SLICE`) and re-checks the flag
//! between slices, so `stop()` returns within one slice rather than after a
//! full TTL/2. Task 3193 will call `stop()` BEFORE releasing claims on SIGINT,
//! so a prompt, clean stop+join is required — this module provides that
//! primitive.
//!
//! ## Injectable heartbeat fn (testability)
//!
//! Mirroring spawn.zig's `Spawner` injection: the registry carries a
//! `HeartbeatFn` (a `*const fn(ctx, allocator, io, token, ttl_secs) void`).
//! Production wires `realHeartbeatFn`, which shells `planar-agent heartbeat
//! --claim <token> --ttl <ttl>` and IGNORES a non-zero exit (a refresh of a
//! just-terminal claim errors — the claim is gone, which is not the heartbeat
//! thread's problem). Tests wire a `FakeHeartbeat` that records (token,
//! call_count) and signals a condition var so a test can deterministically wait
//! until the thread has fired for the expected tokens, with no subprocesses and
//! no sleep-and-hope timing.

const std = @import("std");
const Io = std.Io;

/// init / register surface the only fallible op: OOM duping a token.
pub const RegisterError = error{OutOfMemory};

/// The default lease TTL in seconds. `planar-agent` default is 600s; we refresh
/// at TTL/2 so a single missed tick still leaves the lease alive for the next.
pub const DEFAULT_TTL_SECS: u32 = 600;

/// The default heartbeat interval (TTL/2). Production cadence: refresh every
/// live lease every ~300s so a long-running worker is never reclaimed.
pub const DEFAULT_INTERVAL: Io.Duration = .{ .nanoseconds = @as(i96, DEFAULT_TTL_SECS / 2) * std.time.ns_per_s };

/// The sleep slice between stop-flag checks. The thread never blocks longer
/// than this on a single `io.sleep`, so `stop()` is observed within one slice
/// even when the configured interval is much larger. Mirrors the scheduler's
/// `POLL_SLEEP` cooperative-wait granularity.
pub const STOP_CHECK_SLICE: Io.Duration = .{ .nanoseconds = 25 * std.time.ns_per_ms };

/// HeartbeatFn refreshes ONE claim token's lease. It must NOT touch any Lua
/// state — only the token string. Errors are the implementation's to swallow
/// (a refresh of a terminal claim legitimately fails); the signature is
/// infallible so the thread loop never has an error to propagate.
pub const HeartbeatFn = *const fn (
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    token: []const u8,
    ttl_secs: u32,
) void;

/// HeartbeatRegistry is the mutex-protected set of LIVE claim tokens plus the
/// preemptive OS thread that refreshes them. One instance backs one run.
///
/// Ownership: the registry OWNS every token string in `tokens` (duped on
/// `register`, freed on `unregister`/`deinit`). It never borrows the main
/// thread's `AgentCallState.claim_token`.
///
/// Thread safety: `tokens` is guarded by `mutex`. `register`/`unregister` run
/// on the main thread; the heartbeat thread only ever SNAPSHOTS under the lock
/// and refreshes outside it. The atomic `stop_flag` is the cross-thread
/// shutdown signal.
pub const HeartbeatRegistry = struct {
    allocator: std.mem.Allocator,
    /// The run's thread-safe Io (used for the mutex + sleeps + subprocesses).
    io: Io,
    mutex: Io.Mutex = .init,
    /// The live claim tokens. Registry-owned duped copies. Guarded by `mutex`.
    tokens: std.ArrayList([]u8) = .empty,

    /// The injected refresh fn + its context. `realHeartbeatFn` in production,
    /// a `FakeHeartbeat` in tests.
    heartbeat_fn: HeartbeatFn,
    heartbeat_ctx: ?*anyopaque = null,

    /// TTL passed to each refresh. Configurable so tests can use a tiny value.
    ttl_secs: u32 = DEFAULT_TTL_SECS,
    /// The interval between refresh ticks (TTL/2 in production; tiny in tests).
    interval: Io.Duration = DEFAULT_INTERVAL,

    /// The spawned OS thread, present only between `start` and `stop`.
    thread: ?std.Thread = null,
    /// Cross-thread shutdown signal. The thread observes it between sleep
    /// slices and exits promptly.
    stop_flag: std.atomic.Value(bool) = .init(false),

    /// init builds a registry with the given refresh fn. The thread is NOT
    /// started until `start`. `io` MUST be thread-safe (the binary's threaded
    /// blocking Io is) — both the main thread and the heartbeat thread lock the
    /// mutex through it.
    pub fn init(
        allocator: std.mem.Allocator,
        io: Io,
        heartbeat_fn: HeartbeatFn,
        heartbeat_ctx: ?*anyopaque,
    ) HeartbeatRegistry {
        return .{
            .allocator = allocator,
            .io = io,
            .heartbeat_fn = heartbeat_fn,
            .heartbeat_ctx = heartbeat_ctx,
        };
    }

    /// deinit frees every owned token. The thread MUST already be stopped
    /// (`stop` joined it) — deinit asserts no thread is live so a forgotten
    /// `stop` trips loudly rather than racing the freeing of `tokens`.
    pub fn deinit(self: *HeartbeatRegistry) void {
        std.debug.assert(self.thread == null); // stop() must precede deinit().
        for (self.tokens.items) |t| self.allocator.free(t);
        self.tokens.deinit(self.allocator);
    }

    /// register dupes `token` into the live set under the mutex. Idempotent:
    /// re-registering an already-live token is a no-op (a token appears once).
    /// Called by the main thread when a worker's claim goes live.
    pub fn register(self: *HeartbeatRegistry, token: []const u8) RegisterError!void {
        if (token.len == 0) return; // no token → nothing to refresh.
        self.mutex.lockUncancelable(self.io);
        defer self.mutex.unlock(self.io);
        for (self.tokens.items) |t| {
            if (std.mem.eql(u8, t, token)) return; // already live.
        }
        const dup = try self.allocator.dupe(u8, token);
        errdefer self.allocator.free(dup);
        try self.tokens.append(self.allocator, dup);
    }

    /// unregister removes + frees the matching duped token under the mutex.
    /// No-op if the token is absent. Called by the main thread when a worker
    /// reaches terminal / its slot is released.
    pub fn unregister(self: *HeartbeatRegistry, token: []const u8) void {
        if (token.len == 0) return;
        self.mutex.lockUncancelable(self.io);
        defer self.mutex.unlock(self.io);
        var i: usize = 0;
        while (i < self.tokens.items.len) : (i += 1) {
            if (std.mem.eql(u8, self.tokens.items[i], token)) {
                const owned = self.tokens.swapRemove(i);
                self.allocator.free(owned);
                return;
            }
        }
    }

    /// liveCount returns the number of live tokens (for tests / observability).
    /// Takes the mutex.
    pub fn liveCount(self: *HeartbeatRegistry) usize {
        self.mutex.lockUncancelable(self.io);
        defer self.mutex.unlock(self.io);
        return self.tokens.items.len;
    }

    /// snapshot copies the live tokens into a freshly-duped list owned by the
    /// CALLER (the caller frees each entry + the list via `freeSnapshot`). Held
    /// the mutex only long enough to copy — the refresh I/O then runs WITHOUT
    /// the lock so register/unregister never block on subprocess latency.
    fn snapshot(self: *HeartbeatRegistry, allocator: std.mem.Allocator) RegisterError![]const []u8 {
        self.mutex.lockUncancelable(self.io);
        defer self.mutex.unlock(self.io);
        const out = try allocator.alloc([]u8, self.tokens.items.len);
        errdefer allocator.free(out);
        var made: usize = 0;
        errdefer for (out[0..made]) |s| allocator.free(s);
        for (self.tokens.items, 0..) |t, idx| {
            out[idx] = try allocator.dupe(u8, t);
            made += 1;
        }
        return out;
    }

    /// freeSnapshot frees a list returned by `snapshot`.
    fn freeSnapshot(allocator: std.mem.Allocator, snap: []const []u8) void {
        for (snap) |s| allocator.free(s);
        allocator.free(snap);
    }

    /// start spawns the heartbeat OS thread. Calling `start` twice is a bug
    /// (asserted). The thread refreshes every live token at `interval` until
    /// `stop`.
    pub fn start(self: *HeartbeatRegistry) std.Thread.SpawnError!void {
        std.debug.assert(self.thread == null);
        self.stop_flag.store(false, .release);
        self.thread = try std.Thread.spawn(.{}, threadMain, .{self});
    }

    /// stop sets the stop flag and joins the thread. Idempotent: a no-op if the
    /// thread was never started / already stopped. Prompt: the thread observes
    /// the flag within one `STOP_CHECK_SLICE`, so this returns quickly even if a
    /// full TTL/2 interval was nominally in progress.
    ///
    /// This is the primitive task 3193 calls on SIGINT BEFORE releasing claims:
    /// stop the refresher first (so it cannot re-refresh a lease the SIGINT path
    /// is about to release), then release.
    pub fn stop(self: *HeartbeatRegistry) void {
        const thr = self.thread orelse return;
        self.stop_flag.store(true, .release);
        thr.join();
        self.thread = null;
    }

    /// threadMain is the heartbeat thread's loop. Each iteration:
    ///   1. snapshot the live tokens under the mutex (own copies),
    ///   2. release the mutex,
    ///   3. refresh each token via `heartbeat_fn` WITHOUT the lock,
    ///   4. sleep one `interval` in `STOP_CHECK_SLICE` slices, re-checking the
    ///      stop flag between slices so `stop()` is observed promptly.
    /// Exits when the stop flag is set. Touches NO Lua state.
    fn threadMain(self: *HeartbeatRegistry) void {
        const io = self.io;
        // An arena keeps the per-tick snapshot allocations cheap to free; we
        // reset it each tick so the thread's footprint stays bounded.
        var arena = std.heap.ArenaAllocator.init(self.allocator);
        defer arena.deinit();

        while (!self.stop_flag.load(.acquire)) {
            // 1+2) snapshot under the lock, release immediately.
            const snap = self.snapshot(arena.allocator()) catch {
                // OOM building the snapshot — skip this tick, try next.
                _ = arena.reset(.retain_capacity);
                self.sleepInterval(io);
                continue;
            };

            // 3) refresh each live token WITHOUT holding the lock.
            for (snap) |token| {
                if (self.stop_flag.load(.acquire)) break; // bail mid-tick on stop.
                self.heartbeat_fn(self.heartbeat_ctx, self.allocator, io, token, self.ttl_secs);
            }
            freeSnapshot(arena.allocator(), snap);
            _ = arena.reset(.retain_capacity);

            // 4) sleep the interval in stop-observing slices.
            self.sleepInterval(io);
        }
    }

    /// sleepInterval sleeps `self.interval` total, in `STOP_CHECK_SLICE` chunks,
    /// returning early the moment the stop flag is observed. Sleep cancellation
    /// is benign (we just re-check the flag and either sleep again or exit).
    fn sleepInterval(self: *HeartbeatRegistry, io: Io) void {
        const interval_ns: i96 = self.interval.nanoseconds;
        const slice_ns: i96 = STOP_CHECK_SLICE.nanoseconds;
        var slept: i96 = 0;
        while (slept < interval_ns) {
            if (self.stop_flag.load(.acquire)) return;
            const remaining = interval_ns - slept;
            const this_slice = @min(remaining, slice_ns);
            io.sleep(.{ .nanoseconds = this_slice }, .awake) catch {};
            slept += this_slice;
        }
    }
};

// ---------------------------------------------------------------------------
// realHeartbeatFn — production: shells `planar-agent heartbeat`.
// ---------------------------------------------------------------------------

/// realHeartbeatFn refreshes one lease by shelling
/// `planar-agent heartbeat --claim <token> --ttl <ttl>`. It runs on the
/// heartbeat thread (NOT the main thread). A non-zero exit is IGNORED: a
/// refresh of a just-terminal claim legitimately errors (the claim is gone),
/// which is not the heartbeat thread's problem. `planar-agent` is resolved via
/// PATH (the run's PATH carries the operator's binary).
pub fn realHeartbeatFn(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    token: []const u8,
    ttl_secs: u32,
) void {
    _ = ctx;
    if (token.len == 0) return;

    var ttl_buf: [16]u8 = undefined;
    const ttl_str = std.fmt.bufPrint(&ttl_buf, "{d}", .{ttl_secs}) catch return;

    const argv = [_][]const u8{
        "planar-agent", "heartbeat", "--claim", token, "--ttl", ttl_str,
    };

    const result = std.process.run(allocator, io, .{
        .argv = &argv,
        .stdout_limit = Io.Limit.limited(8192),
        .stderr_limit = Io.Limit.limited(8192),
    }) catch return; // could-not-spawn → swallow; next tick retries.
    allocator.free(result.stdout);
    allocator.free(result.stderr);
    // Non-zero exit is intentionally ignored (terminal/vanished claim).
}

// ---------------------------------------------------------------------------
// FakeHeartbeat — test double: records (token, call_count), signals a CV.
// ---------------------------------------------------------------------------

/// FakeHeartbeat records every refresh call so a deterministic test can WAIT
/// (via a condition var, not a fixed sleep) until the heartbeat thread has
/// fired for the expected tokens. No subprocess, no timing dependence.
///
/// Thread safety: `mutex` guards `calls` + `total`. `recordCall` (on the
/// heartbeat thread) bumps the count and signals `cond`; `waitForTotal` (on the
/// test thread) waits on `cond` until `total >= want`. The condition var is the
/// deterministic synchronization primitive — the test never sleeps-and-hopes.
pub const FakeHeartbeat = struct {
    allocator: std.mem.Allocator,
    io: Io,
    mutex: Io.Mutex = .init,
    cond: Io.Condition = .init,
    /// Per-token call counts. Owns the key strings.
    calls: std.StringHashMapUnmanaged(usize) = .{},
    /// Total refresh calls across all tokens.
    total: usize = 0,
    /// Last TTL seen (lets a test assert the configured TTL flows through).
    last_ttl: u32 = 0,

    pub fn init(allocator: std.mem.Allocator, io: Io) FakeHeartbeat {
        return .{ .allocator = allocator, .io = io };
    }

    pub fn deinit(self: *FakeHeartbeat) void {
        var it = self.calls.keyIterator();
        while (it.next()) |k| self.allocator.free(k.*);
        self.calls.deinit(self.allocator);
    }

    /// heartbeatFn returns the HeartbeatFn that drives THIS fake.
    pub fn heartbeatFn(self: *FakeHeartbeat) HeartbeatFn {
        _ = self;
        return recordCall;
    }

    /// fnCtx returns the opaque ctx pointer to pair with `heartbeatFn`.
    pub fn fnCtx(self: *FakeHeartbeat) ?*anyopaque {
        return self;
    }

    /// callCount returns how many times `token` has been refreshed.
    pub fn callCount(self: *FakeHeartbeat, token: []const u8) usize {
        self.mutex.lockUncancelable(self.io);
        defer self.mutex.unlock(self.io);
        return self.calls.get(token) orelse 0;
    }

    /// totalCalls returns the total refresh count across all tokens.
    pub fn totalCalls(self: *FakeHeartbeat) usize {
        self.mutex.lockUncancelable(self.io);
        defer self.mutex.unlock(self.io);
        return self.total;
    }

    /// waitForTotal blocks (on the condition var) until `total >= want`. This is
    /// the DETERMINISTIC wait the threading tests use instead of a fixed sleep:
    /// it returns the instant the heartbeat thread has fired `want` times.
    /// There is no wall-clock timeout here; the test wraps itself in a
    /// `Watchdog` so a genuine hang fails LOUDLY rather than waiting forever.
    pub fn waitForTotal(self: *FakeHeartbeat, want: usize) void {
        self.mutex.lockUncancelable(self.io);
        defer self.mutex.unlock(self.io);
        while (self.total < want) {
            self.cond.waitUncancelable(self.io, &self.mutex);
        }
    }
};

/// recordCall is the FakeHeartbeat's HeartbeatFn. Runs on the heartbeat thread.
fn recordCall(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    token: []const u8,
    ttl_secs: u32,
) void {
    _ = allocator;
    const self: *FakeHeartbeat = @ptrCast(@alignCast(ctx.?));
    self.mutex.lockUncancelable(io);
    defer self.mutex.unlock(io);
    const gop = self.calls.getOrPut(self.allocator, token) catch return;
    if (!gop.found_existing) {
        gop.key_ptr.* = self.allocator.dupe(u8, token) catch {
            _ = self.calls.remove(token);
            return;
        };
        gop.value_ptr.* = 0;
    }
    gop.value_ptr.* += 1;
    self.total += 1;
    self.last_ttl = ttl_secs;
    self.cond.signal(io);
}

// ===========================================================================
// Tests
// ===========================================================================

const testing = std.testing;

// --- Registry unit tests (no thread) ---------------------------------------

fn noopHeartbeat(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    token: []const u8,
    ttl_secs: u32,
) void {
    _ = ctx;
    _ = allocator;
    _ = io;
    _ = token;
    _ = ttl_secs;
}

test "heartbeat: register dupes tokens; snapshot reflects them; no leak" {
    const a = testing.allocator;
    var reg = HeartbeatRegistry.init(a, testing.io, noopHeartbeat, null);
    defer reg.deinit();

    try reg.register("tok-a");
    try reg.register("tok-b");
    try testing.expectEqual(@as(usize, 2), reg.liveCount());

    // snapshot owns its copies (caller frees).
    const snap = try reg.snapshot(a);
    defer HeartbeatRegistry.freeSnapshot(a, snap);
    try testing.expectEqual(@as(usize, 2), snap.len);
    // The snapshot strings are independent copies, not the registry's slices.
    for (snap) |s| {
        try testing.expect(s.ptr != reg.tokens.items[0].ptr);
    }
}

test "heartbeat: register is idempotent (a token appears once)" {
    const a = testing.allocator;
    var reg = HeartbeatRegistry.init(a, testing.io, noopHeartbeat, null);
    defer reg.deinit();

    try reg.register("dup");
    try reg.register("dup");
    try reg.register("dup");
    try testing.expectEqual(@as(usize, 1), reg.liveCount());
}

test "heartbeat: empty token register/unregister are no-ops" {
    const a = testing.allocator;
    var reg = HeartbeatRegistry.init(a, testing.io, noopHeartbeat, null);
    defer reg.deinit();

    try reg.register("");
    reg.unregister("");
    try testing.expectEqual(@as(usize, 0), reg.liveCount());
}

test "heartbeat: unregister removes + frees the matching token (no leak)" {
    const a = testing.allocator;
    var reg = HeartbeatRegistry.init(a, testing.io, noopHeartbeat, null);
    defer reg.deinit();

    try reg.register("keep");
    try reg.register("drop");
    try reg.register("also-keep");
    try testing.expectEqual(@as(usize, 3), reg.liveCount());

    reg.unregister("drop");
    try testing.expectEqual(@as(usize, 2), reg.liveCount());

    // "drop" is gone; the survivors remain.
    const snap = try reg.snapshot(a);
    defer HeartbeatRegistry.freeSnapshot(a, snap);
    var saw_keep = false;
    var saw_also = false;
    var saw_drop = false;
    for (snap) |s| {
        if (std.mem.eql(u8, s, "keep")) saw_keep = true;
        if (std.mem.eql(u8, s, "also-keep")) saw_also = true;
        if (std.mem.eql(u8, s, "drop")) saw_drop = true;
    }
    try testing.expect(saw_keep and saw_also and !saw_drop);

    // unregister of an absent token is a no-op.
    reg.unregister("never-registered");
    try testing.expectEqual(@as(usize, 2), reg.liveCount());
}

// --- Thread lifecycle tests (deterministic via the fake's condition var) ----

/// Watchdog panics loudly if a deterministic test does not reach its expected
/// state within a generous wall-clock bound. This converts a genuine hang (a
/// real deadlock / missed-signal bug) into a LOUD failure instead of a CI job
/// that hangs until the runner times out. The correctness assertions never
/// depend on timing — they wait on the fake's condition var (`waitForTotal`)
/// which returns the instant the count is reached. The watchdog only guards
/// against the loop never completing at all.
const Watchdog = struct {
    io: Io,
    deadline_ns: u64,
    armed: std.atomic.Value(bool),
    thread: ?std.Thread = null,

    fn monoNs() u64 {
        var ts: std.c.timespec = undefined;
        if (std.c.clock_gettime(.MONOTONIC, &ts) != 0) return 0;
        return @as(u64, @intCast(ts.sec)) * std.time.ns_per_s + @as(u64, @intCast(ts.nsec));
    }

    fn arm(io: Io, budget_ns: u64) Watchdog {
        return .{ .io = io, .deadline_ns = monoNs() + budget_ns, .armed = .init(true) };
    }

    fn start(self: *Watchdog) !void {
        self.thread = try std.Thread.spawn(.{}, watch, .{self});
    }

    fn watch(self: *Watchdog) void {
        while (self.armed.load(.acquire)) {
            if (monoNs() > self.deadline_ns) {
                @panic("heartbeat test watchdog: deterministic wait exceeded budget — a missed signal / deadlock bug, NOT a timing flake.");
            }
            self.io.sleep(.{ .nanoseconds = 5 * std.time.ns_per_ms }, .awake) catch {};
        }
    }

    fn disarm(self: *Watchdog) void {
        self.armed.store(false, .release);
        if (self.thread) |t| {
            t.join();
            self.thread = null;
        }
    }
};

test "heartbeat: thread refreshes registered tokens at the configured cadence" {
    const a = testing.allocator;
    const io = testing.io;

    var fake = FakeHeartbeat.init(a, io);
    defer fake.deinit();

    var reg = HeartbeatRegistry.init(a, io, fake.heartbeatFn(), fake.fnCtx());
    reg.ttl_secs = 900;
    // Tiny interval so ticks fire rapidly; correctness is gated on the fake's
    // observed count via the condition var, NOT on this interval.
    reg.interval = .{ .nanoseconds = 1 * std.time.ns_per_ms };
    defer reg.deinit();

    try reg.register("tok-1");
    try reg.register("tok-2");

    var wd = Watchdog.arm(io, 10 * std.time.ns_per_s);
    try wd.start();
    defer wd.disarm();

    try reg.start();

    // Deterministic wait: block until the thread has fired enough times that
    // both tokens must have been refreshed (2 tokens × several ticks).
    fake.waitForTotal(8);
    reg.stop();

    // Both tokens were heartbeated; the configured TTL flowed through.
    try testing.expect(fake.callCount("tok-1") >= 1);
    try testing.expect(fake.callCount("tok-2") >= 1);
    try testing.expectEqual(@as(u32, 900), fake.last_ttl);
}

test "heartbeat: unregistered token stops being refreshed; survivor continues" {
    const a = testing.allocator;
    const io = testing.io;

    var fake = FakeHeartbeat.init(a, io);
    defer fake.deinit();

    var reg = HeartbeatRegistry.init(a, io, fake.heartbeatFn(), fake.fnCtx());
    reg.interval = .{ .nanoseconds = 1 * std.time.ns_per_ms };
    defer reg.deinit();

    try reg.register("gone");
    try reg.register("stays");

    var wd = Watchdog.arm(io, 10 * std.time.ns_per_s);
    try wd.start();
    defer wd.disarm();

    try reg.start();

    // Wait until both tokens have been seen at least once.
    fake.waitForTotal(2);
    // Drop "gone" — after this, only "stays" should keep accruing.
    reg.unregister("gone");
    const gone_at_unregister = fake.callCount("gone");

    // Deterministically wait for several more refreshes of the survivor.
    const stays_baseline = fake.callCount("stays");
    while (fake.callCount("stays") < stays_baseline + 5) {
        fake.waitForTotal(fake.totalCalls() + 1);
    }
    reg.stop();

    // "gone" did not accrue meaningfully after unregister: at most one extra
    // refresh could be in-flight at the unregister boundary (a tick already
    // snapshotted it), never the +5 the survivor got.
    const gone_after = fake.callCount("gone");
    try testing.expect(gone_after <= gone_at_unregister + 1);
    try testing.expect(fake.callCount("stays") >= stays_baseline + 5);
}

test "heartbeat: no refresh fires after stop()" {
    const a = testing.allocator;
    const io = testing.io;

    var fake = FakeHeartbeat.init(a, io);
    defer fake.deinit();

    var reg = HeartbeatRegistry.init(a, io, fake.heartbeatFn(), fake.fnCtx());
    reg.interval = .{ .nanoseconds = 1 * std.time.ns_per_ms };
    defer reg.deinit();

    try reg.register("t");

    var wd = Watchdog.arm(io, 10 * std.time.ns_per_s);
    try wd.start();
    defer wd.disarm();

    try reg.start();
    fake.waitForTotal(3);
    reg.stop();

    // After join() returns, the thread is gone. No further refresh can fire.
    const after_stop = fake.totalCalls();
    // Sleep a few intervals' worth — a still-running thread would bump the
    // count; a properly-joined thread cannot.
    io.sleep(.{ .nanoseconds = 50 * std.time.ns_per_ms }, .awake) catch {};
    try testing.expectEqual(after_stop, fake.totalCalls());
}

test "heartbeat: stop() before any tick returns promptly (mid-sleep observation)" {
    const a = testing.allocator;
    const io = testing.io;

    var fake = FakeHeartbeat.init(a, io);
    defer fake.deinit();

    var reg = HeartbeatRegistry.init(a, io, fake.heartbeatFn(), fake.fnCtx());
    // A LARGE interval: if stop were only observed at interval boundaries,
    // join() would block for ~an hour. The slice-based stop must return fast.
    reg.interval = .{ .nanoseconds = 3600 * std.time.ns_per_s };
    defer reg.deinit();

    // No tokens registered → the first tick's refresh loop is empty, then the
    // thread enters the long sleep. stop() must interrupt that sleep promptly.
    var wd = Watchdog.arm(io, 10 * std.time.ns_per_s);
    try wd.start();
    defer wd.disarm();

    try reg.start();
    // Immediately stop. The thread is almost certainly mid-(long-)sleep; stop()
    // must return well inside the watchdog budget (one STOP_CHECK_SLICE, ~25ms),
    // not after the 3600s interval.
    reg.stop();
    // If we got here without the watchdog firing, stop() was prompt.
    try testing.expect(reg.thread == null);
}

test "heartbeat: stop() is idempotent and safe before start" {
    const a = testing.allocator;
    var reg = HeartbeatRegistry.init(a, testing.io, noopHeartbeat, null);
    defer reg.deinit();
    // stop before start is a no-op.
    reg.stop();
    try testing.expect(reg.thread == null);
}
