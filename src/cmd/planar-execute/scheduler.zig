//! scheduler.zig — the M5 coroutine event loop (plan 492 task 3182).
//!
//! ## The model (single `lua_State`, single-threaded, cooperative)
//!
//! M4 ran `run(ctx)` via a blocking `lua_pcallk` and drove each `agent()` call
//! synchronously (spawn → wait → terminal-verb → return). M5 flips the internal
//! mechanism to coroutine-yielding, exactly as the Claude Workflow tool models
//! concurrency in JS (also single-threaded): the parallelism lives in the I/O
//! underneath an event loop, not in the script language.
//!
//!     Lua coroutine        ~  a Promise continuation
//!     Zig scheduler loop   ~  the JS event loop
//!     claude -p child proc ~  the async I/O the loop waits on
//!
//! `run(ctx)` executes inside a Lua coroutine thread (`lua_newthread`). The
//! scheduler drives it with `lua_resume`. When the workflow calls `agent()`,
//! the host function spawns the worker NON-BLOCKING, registers the in-flight
//! worker with the scheduler, and `lua_yield`s across the C boundary
//! (`lua_yieldk` + a continuation). The scheduler — seeing `LUA_YIELD` — drives
//! the single in-flight worker to its terminal state (process exit / claim
//! terminal) and resumes the coroutine, whose continuation reads the outcome
//! and returns the result table to the script.
//!
//! ## SCOPE — single coroutine (N=1) AND N-way concurrency (parallel)
//!
//! Task 3182 proved the yield/resume machinery for ONE coroutine driving ONE
//! in-flight worker (N=1): `runModule` waits the single handle (`driveInflight`)
//! and resumes. For N=1 the observable behavior is IDENTICAL to M4.
//!
//! Task 3183 (`parallel`) widens the in-flight-worker registry to N slots and
//! adds the WAIT-FOR-ANY primitives consumed by `parallel`'s N-child drive loop
//! (in main.zig): `pollReadyOnce` (non-blocking probe of every in-use slot,
//! returns the first terminal) and `waitForAny` (poll-loop + short sleep until
//! ANY worker completes — the FIRST in completion order, not registration
//! order). `inflightCount` gates the MAX_SLOTS concurrency cap so N>MAX_SLOTS
//! children QUEUE. No `pipeline` (3184), no heartbeat/timeout/SIGINT (M6), no
//! run-id/journal (M6/M8) here.
//!
//! ## State threading across the yield
//!
//! `lua_yieldk`'s `ctx` parameter is a `lua_KContext` (a `ptrdiff_t`), not a
//! pointer to rich state. So the per-`agent()`-call state needed by the
//! continuation (worktree path, claim token, the in-flight handle, the
//! pre-spawn HEAD, the spawn outcome the scheduler fills in) is stashed in a
//! scheduler-owned `Slot`, and the `ctx` passed to `lua_yieldk` is the slot
//! INDEX. The continuation recovers the slot by index. The `agent()` host
//! function (in main.zig) owns the slot's `payload` (an opaque
//! `*AgentCallState`); the scheduler owns the coroutine bookkeeping + the
//! worker drive.

const std = @import("std");
const lua = @import("lua.zig");
const c = lua.c;
const spawn = @import("spawn.zig");

/// MAX_SLOTS is the in-flight-worker registry capacity. This cycle exercises
/// N=1; the array shape is what task 3183 widens (a real N-coroutine `parallel`
/// will register up to one in-flight worker per concurrent coroutine).
pub const MAX_SLOTS: usize = 16;

/// A single in-flight-worker slot. Owned by the scheduler; keyed (found) by the
/// coroutine `lua_State *` that registered it.
///
/// Lifecycle: `registerInflight` populates `co`/`handle`/`payload` and sets
/// `in_use`. `driveInflight` waits the handle and stores `outcome`. The
/// continuation (in main.zig) reads `outcome` + `payload`, then calls
/// `releaseSlot` to free the slot for reuse. The `payload` is an opaque pointer
/// the caller owns and frees separately.
pub const Slot = struct {
    in_use: bool = false,
    /// The coroutine thread that registered this in-flight worker. The
    /// scheduler resumes THIS thread once the worker reaches terminal.
    co: ?*c.lua_State = null,
    /// The in-flight worker handle (from `Spawner.start`).
    handle: spawn.Handle = undefined,
    /// The outcome, filled in by `driveInflight`. Null until the worker
    /// reaches terminal. The continuation consumes + frees it.
    outcome: ?spawn.SpawnOutcome = null,
    /// Opaque per-call payload owned by the host (main.zig's AgentCallState).
    payload: ?*anyopaque = null,
    /// The wall-clock DEADLINE for this worker, in monotonic nanoseconds (the
    /// scheduler's injected clock domain). Set at `registerInflight` to
    /// `clockNow() + max_wall_clock_ns`. The poll loop times out a worker that
    /// is STILL in flight (poll == null) at or past this instant. A REAL
    /// monotonic clock (not the deterministic `ctx.now`): the timeout measures
    /// actual elapsed wall-clock so a genuinely-hung child is killed even when
    /// the workflow's injected `now` is frozen. Task 3188.
    deadline_ns: i128 = 0,
    /// Set true when the wall-clock timeout fired on this slot (task 3188): the
    /// worker was hung, the scheduler killed the child, and `outcome` carries
    /// the synthetic timed-out result. The continuation (agentContinue) branches
    /// on this to force `planar-agent fail`, tear down the worktree, and return
    /// a `status = "timed-out"` result instead of running the normal
    /// terminal-verb decision matrix.
    timed_out: bool = false,
};

/// ClockFn returns a monotonic timestamp in nanoseconds. Injected on the
/// Scheduler so the per-worker wall-clock TIMEOUT (task 3188) can be driven
/// deterministically in tests (a settable fake clock the test advances past a
/// deadline) WITHOUT a real `std.time.sleep` — which would be both flaky and
/// slow. Production wires `realMonotonicNow`, which reads `std.time.Instant`.
///
/// IMPORTANT: this is a REAL monotonic clock in production, distinct from the
/// host-injected deterministic `ctx.now` (a frozen Unix-epoch second the
/// workflow sees). The timeout is a real-world liveness guard: it measures
/// actual elapsed wall-clock, so a hung child is killed even when `ctx.now` is
/// pinned.
pub const ClockFn = *const fn (ctx: ?*anyopaque, io: std.Io) i128;

/// realMonotonicNow reads the process monotonic ("awake") clock in nanoseconds.
/// Production clock for the wall-clock-timeout deadline. `Clock.awake` is the
/// non-settable monotonic clock (CLOCK_MONOTONIC on Linux / CLOCK_UPTIME_RAW on
/// macOS) — immune to wall-clock jumps, exactly right for a liveness ceiling.
pub fn realMonotonicNow(ctx: ?*anyopaque, io: std.Io) i128 {
    _ = ctx;
    return @as(i128, std.Io.Clock.awake.now(io).nanoseconds);
}

/// The default per-worker wall-clock timeout: 1800s (30 min). Chosen relative
/// to the claim TTL (heartbeat.DEFAULT_TTL_SECS = 600s, refreshed at TTL/2):
/// a worker is heartbeated alive indefinitely, so the lease CANNOT catch a
/// hang — only this independent hard ceiling can. 30 min is comfortably longer
/// than a healthy coder/reviewer cycle yet bounds a wedged child. Injectable
/// (small) for deterministic tests via `Scheduler.max_wall_clock_ns`.
pub const DEFAULT_MAX_WALL_CLOCK_NS: i128 = @as(i128, 1800) * std.time.ns_per_s;

/// Scheduler is the cooperative event loop + the in-flight-worker registry. One
/// instance backs one `runModule` invocation. Reachable from the `agent()` host
/// function via `HostState.scheduler` so the host can register an in-flight
/// worker and `lua_yield`.
pub const Scheduler = struct {
    /// The spawner used to drive in-flight workers to terminal. Injected from
    /// the AgentDriver (FakeSpawner in tests, realSpawner in production).
    /// Null in pure-Lua runs that never spawn (no `agent()` call) — the
    /// scheduler still drives the coroutine, it just never registers a worker.
    spawner: ?spawn.Spawner = null,
    allocator: std.mem.Allocator,
    /// The Io used to drive in-flight workers (wait/poll/sleep). OPTIONAL:
    /// `null` for pure-Lua runs that never spawn (no `agent()` → no worker →
    /// no driver → no genuine Io). It is set ONLY when a spawner is installed,
    /// so the worker-drive paths can `unwrapIo()` it with a loud panic instead
    /// of carrying an `undefined` landmine (plan 492 task 3260). The invariant:
    /// a slot is registered only when a spawner+io pair was wired, so any code
    /// reaching `unwrapIo` legitimately has a real Io.
    io: ?std.Io = null,
    slots: [MAX_SLOTS]Slot = .{Slot{}} ** MAX_SLOTS,

    /// The monotonic clock backing per-worker wall-clock deadlines (task 3188).
    /// Defaults to `realMonotonicNow` (the process "awake" clock). Tests inject
    /// a settable fake clock (via `clock_fn` + `clock_ctx`) so they can advance
    /// time past a deadline deterministically — no real sleep, no flakiness.
    clock_fn: ClockFn = realMonotonicNow,
    /// Opaque context for `clock_fn` (a `*FakeClock` in tests; null in prod).
    clock_ctx: ?*anyopaque = null,
    /// The per-worker wall-clock budget. A worker still in flight this many
    /// monotonic nanoseconds after it registered is HUNG → killed + reclaimed.
    /// Defaults to `DEFAULT_MAX_WALL_CLOCK_NS` (30 min); tests set a tiny value.
    max_wall_clock_ns: i128 = DEFAULT_MAX_WALL_CLOCK_NS,

    pub fn init(allocator: std.mem.Allocator, io: ?std.Io) Scheduler {
        return .{ .allocator = allocator, .io = io };
    }

    /// clockNow returns the current monotonic nanosecond reading via the
    /// injected clock. Used to stamp deadlines at registration and to test them
    /// each poll round.
    fn clockNow(self: *Scheduler) i128 {
        return self.clock_fn(self.clock_ctx, self.unwrapIo());
    }

    /// unwrapIo returns the drive Io or panics loudly. Reached only by the
    /// worker-drive paths (driveInflight/pollReadyOnce/waitForAny), which run
    /// only after a worker was registered — which requires a spawner+io. A null
    /// here is therefore a wiring bug, not a reachable state, and we trap it
    /// rather than propagate `undefined` UB.
    fn unwrapIo(self: *Scheduler) std.Io {
        return self.io orelse @panic(
            "scheduler: worker drive reached with no Io installed — a slot was registered without a spawner+io pair (wiring bug). See Scheduler.io (task 3260).",
        );
    }

    /// registerInflight claims a free slot for the in-flight worker `handle`
    /// owned by coroutine `co`, with the host's opaque `payload`. Returns the
    /// slot index (the value passed as `lua_yieldk`'s ctx). Errors if the
    /// registry is full (would only happen under N-way parallel beyond
    /// MAX_SLOTS; impossible at N=1).
    pub fn registerInflight(
        self: *Scheduler,
        co: *c.lua_State,
        handle: spawn.Handle,
        payload: ?*anyopaque,
    ) error{RegistryFull}!usize {
        // Stamp the wall-clock deadline at registration: spawn-time + budget.
        // Read the monotonic clock ONCE here (registration always happens with a
        // wired spawner+io, so clockNow's unwrapIo is safe).
        const deadline = self.clockNow() + self.max_wall_clock_ns;
        for (&self.slots, 0..) |*s, idx| {
            if (s.in_use) continue;
            s.* = .{
                .in_use = true,
                .co = co,
                .handle = handle,
                .outcome = null,
                .payload = payload,
                .deadline_ns = deadline,
                .timed_out = false,
            };
            return idx;
        }
        return error.RegistryFull;
    }

    /// slot returns a pointer to the slot at `idx` (the continuation recovers it
    /// from the `lua_KContext`).
    pub fn slot(self: *Scheduler, idx: usize) *Slot {
        return &self.slots[idx];
    }

    /// The synthetic exit code recorded on a timed-out worker's outcome. 255 is
    /// the same abnormal-termination sentinel `spawn.termExitCode` uses for a
    /// signal/crash; a non-zero exit drives the terminal-verb decision toward
    /// `fail`, which is exactly the timeout disposition (the continuation also
    /// forces `fail` explicitly via the `timed_out` flag).
    pub const TIMED_OUT_EXIT_CODE: u32 = 255;

    /// timeoutWorker reclaims a HUNG worker on slot `idx` (task 3188): it
    /// hard-kills the child via the spawner, stamps a synthetic timed-out
    /// `outcome` (so the slot is now "terminal" and the owning coroutine can be
    /// resumed), and marks `timed_out` so the continuation runs the timeout
    /// reclaim path (force `fail` + worktree teardown + `status = "timed-out"`).
    /// The heartbeat-unregister happens in the continuation (it owns the claim
    /// token), keeping this layer Lua-free and claim-free.
    ///
    /// Order (documented, load-bearing): KILL the child FIRST (so a live process
    /// is not left running in a worktree the continuation is about to tear
    /// down), then publish the terminal outcome. The continuation then does
    /// unregister → fail → teardown.
    fn timeoutWorker(self: *Scheduler, idx: usize) spawn.SpawnError!void {
        const s = &self.slots[idx];
        const sp = self.spawner.?; // caller already unwrapped a spawner.
        sp.kill(self.unwrapIo(), &s.handle);
        // Synthetic terminal outcome: empty captured streams + sentinel exit.
        const empty_out = self.allocator.alloc(u8, 0) catch return spawn.SpawnError.OutOfMemory;
        errdefer self.allocator.free(empty_out);
        const empty_err = self.allocator.alloc(u8, 0) catch return spawn.SpawnError.OutOfMemory;
        s.outcome = .{ .exit_code = TIMED_OUT_EXIT_CODE, .stdout = empty_out, .stderr = empty_err };
        s.timed_out = true;
    }

    /// driveInflight drives the slot's in-flight worker to terminal and stores
    /// the outcome on the slot. The continuation reads it from there. This is
    /// the N=1 fast path (one coroutine, one worker) and the per-`agent()` drive
    /// in `runModule`.
    ///
    /// It is a deadline-aware poll loop (NOT a bare blocking `wait`): each round
    /// it polls the handle, and if the worker is STILL running past its
    /// wall-clock deadline it times it out (kill + synthetic outcome). A bare
    /// `wait` could never catch a hung child — the heartbeat thread keeps its
    /// lease fresh forever, so the timeout is the only reclaim path. Task 3188.
    pub fn driveInflight(self: *Scheduler, idx: usize) spawn.SpawnError!void {
        const s = &self.slots[idx];
        std.debug.assert(s.in_use);
        const sp = self.spawner orelse @panic(
            "scheduler.driveInflight: no spawner installed but a worker was registered — the agent() host must only register an in-flight worker when a driver/spawner is wired.",
        );
        const io = self.unwrapIo();
        while (true) {
            if (s.outcome != null) return; // already terminal (e.g. timed out).
            if (try sp.poll(self.allocator, io, &s.handle)) |outcome| {
                s.outcome = outcome;
                return;
            }
            // Still running — wall-clock-deadline check.
            if (self.clockNow() >= s.deadline_ns) {
                try self.timeoutWorker(idx);
                return;
            }
            io.sleep(POLL_SLEEP, .awake) catch {};
        }
    }

    /// The poll-loop sleep between non-blocking probe rounds in `waitForAny`.
    /// A cooperative wait (no threads — the heartbeat thread is M6); ~10ms keeps
    /// latency low without busy-spinning the CPU.
    pub const POLL_SLEEP: std.Io.Duration = .{ .nanoseconds = 10 * std.time.ns_per_ms };

    /// pollReadyOnce probes EVERY in-use slot once (non-blocking) and, on the
    /// FIRST slot whose worker has reached terminal, stores its outcome on the
    /// slot and returns that slot's index. Returns `null` when no in-use slot is
    /// terminal yet (all still running) — the caller then sleeps + retries.
    ///
    /// This is the wait-for-any primitive for `parallel` (task 3183): polling
    /// slot[i] never blocks on a sibling slot[j] that has not finished. A slot
    /// that already has a stored `outcome` (driven by a prior round but not yet
    /// consumed) is skipped — the caller consumes it via the slot index.
    pub fn pollReadyOnce(self: *Scheduler) spawn.SpawnError!?usize {
        const sp = self.spawner orelse @panic(
            "scheduler.pollReadyOnce: no spawner installed but a worker was registered.",
        );
        const io = self.unwrapIo();
        // Pass 1: a naturally-terminal worker takes priority — a completed
        // worker is preferred over timing one out, and this keeps the
        // wait-for-any "first to complete" contract intact.
        for (&self.slots, 0..) |*s, idx| {
            if (!s.in_use) continue;
            if (s.outcome != null) continue; // already terminal, awaiting consume
            if (try sp.poll(self.allocator, io, &s.handle)) |outcome| {
                s.outcome = outcome;
                return idx;
            }
        }
        // Pass 2: NONE completed this round — check each still-running worker
        // against its wall-clock deadline (task 3188). A worker past its
        // deadline is HUNG (the heartbeat thread keeps its lease fresh, so the
        // lease can never reclaim it) → kill + synthesize a timed-out outcome
        // and return it as the terminal slot. Only ONE per round (we return the
        // first); a sibling past its deadline is caught on the next round.
        const now = self.clockNow();
        for (&self.slots, 0..) |*s, idx| {
            if (!s.in_use) continue;
            if (s.outcome != null) continue;
            if (now >= s.deadline_ns) {
                try self.timeoutWorker(idx);
                return idx;
            }
        }
        return null;
    }

    /// waitForAny drives the in-use slots until ANY worker reaches terminal,
    /// then returns that slot's index (its `outcome` is stored on the slot for
    /// the caller to consume). It is a cooperative poll-loop: each round probes
    /// every in-use slot via `pollReadyOnce`; if none is terminal it sleeps
    /// `POLL_SLEEP_NS` and retries. Returns `error.NoInflight` if no slot is in
    /// use (a caller bug — there is nothing to wait for).
    ///
    /// This is genuine wait-for-any: the FIRST worker to finish (in completion
    /// order, NOT registration order) is the one returned, so `parallel` resumes
    /// the owning child as soon as its worker is done regardless of order.
    pub fn waitForAny(self: *Scheduler) error{ NoInflight, SpawnFailed }!usize {
        // Cheap guard: nothing in flight means the caller has nothing to wait on.
        var any_in_use = false;
        for (&self.slots) |*s| {
            if (s.in_use and s.outcome == null) any_in_use = true;
        }
        if (!any_in_use) return error.NoInflight;

        while (true) {
            const ready = self.pollReadyOnce() catch return error.SpawnFailed;
            if (ready) |idx| return idx;
            // No worker terminal this round — sleep briefly, then poll again.
            // Sleep cancellation is benign here (we just re-poll immediately).
            self.unwrapIo().sleep(POLL_SLEEP, .awake) catch {};
        }
    }

    /// inflightCount returns the number of slots whose worker is in flight and
    /// has NOT yet had its outcome consumed. Used by `parallel`'s drive loop to
    /// decide when it must wait-for-any (cap reached or all children started).
    pub fn inflightCount(self: *Scheduler) usize {
        var n: usize = 0;
        for (&self.slots) |*s| {
            if (s.in_use and s.outcome == null) n += 1;
        }
        return n;
    }

    /// releaseSlot marks the slot free for reuse and clears its fields. Does NOT
    /// free `outcome` (the continuation took ownership) or `payload` (the host
    /// owns it); it only resets the registry entry.
    pub fn releaseSlot(self: *Scheduler, idx: usize) void {
        self.slots[idx] = .{};
    }
};

/// ResumeStatus is the classified result of one `lua_resume` step on the
/// coroutine, abstracting the raw Lua status codes for `driveCoroutine`.
pub const ResumeStatus = enum {
    /// The coroutine finished (`LUA_OK`).
    done,
    /// The coroutine yielded (`LUA_YIELD`) — an in-flight worker is registered.
    yielded,
    /// The coroutine raised an error. The error value is on `co`'s stack top.
    err,
};

/// classifyResume maps a raw `lua_resume` return code to a `ResumeStatus`.
pub fn classifyResume(rc: c_int) ResumeStatus {
    return switch (rc) {
        c.LUA_OK => .done,
        c.LUA_YIELD => .yielded,
        else => .err,
    };
}

// ---------------------------------------------------------------------------
// Unit tests — the registry + classification logic (no Lua state needed).
// ---------------------------------------------------------------------------

const testing = std.testing;

test "scheduler: classifyResume maps Lua status codes" {
    try testing.expectEqual(ResumeStatus.done, classifyResume(c.LUA_OK));
    try testing.expectEqual(ResumeStatus.yielded, classifyResume(c.LUA_YIELD));
    try testing.expectEqual(ResumeStatus.err, classifyResume(c.LUA_ERRRUN));
    try testing.expectEqual(ResumeStatus.err, classifyResume(2)); // LUA_ERRRUN-ish
}

test "scheduler: registerInflight claims a slot and driveInflight stores the outcome" {
    const a = testing.allocator;
    var fake = spawn.FakeSpawnerState.init(a, 0, "DRIVEN", "");
    defer fake.deinit();

    var sched = Scheduler.init(a, std.testing.io);
    sched.spawner = fake.spawner();

    // A fake coroutine pointer (the registry only stores it; the drive path
    // never dereferences `co`).
    const fake_co: *c.lua_State = @ptrFromInt(0x1000);

    const handle = try sched.spawner.?.start(a, std.testing.io, .{
        .role = .coder,
        .worktree_path = "/tmp/wt",
        .brief = "B",
        .role_spec = "",
        .env_map = null,
    });
    const idx = try sched.registerInflight(fake_co, handle, null);
    try testing.expect(sched.slot(idx).in_use);
    try testing.expect(sched.slot(idx).co == fake_co);
    try testing.expect(sched.slot(idx).outcome == null);

    // Drive the worker to terminal — the scheduler waits the handle.
    try sched.driveInflight(idx);
    try testing.expect(fake.reached_terminal);
    try testing.expect(sched.slot(idx).outcome != null);
    try testing.expectEqualStrings("DRIVEN", sched.slot(idx).outcome.?.stdout);

    // The continuation would take ownership of outcome; here we free it and
    // release the slot.
    sched.slot(idx).outcome.?.deinit(a);
    sched.releaseSlot(idx);
    try testing.expect(!sched.slot(idx).in_use);
}

test "scheduler: registry fills and reports RegistryFull beyond capacity" {
    const a = testing.allocator;
    var fake = spawn.FakeSpawnerState.init(a, 0, "", "");
    defer fake.deinit();
    var sched = Scheduler.init(a, std.testing.io);
    sched.spawner = fake.spawner();
    const fake_co: *c.lua_State = @ptrFromInt(0x1000);

    var i: usize = 0;
    while (i < MAX_SLOTS) : (i += 1) {
        const h = try sched.spawner.?.start(a, std.testing.io, .{
            .role = .coder,
            .worktree_path = "/tmp/wt",
            .brief = "",
            .role_spec = "",
            .env_map = null,
        });
        _ = try sched.registerInflight(fake_co, h, null);
    }
    // One more start + register exceeds capacity. Drive/free the extra handle
    // ourselves since it never entered the registry.
    var extra = try sched.spawner.?.start(a, std.testing.io, .{
        .role = .coder,
        .worktree_path = "/tmp/wt",
        .brief = "",
        .role_spec = "",
        .env_map = null,
    });
    try testing.expectError(error.RegistryFull, sched.registerInflight(fake_co, extra, null));
    var out = try fake.spawner().wait(a, std.testing.io, &extra);
    out.deinit(a);

    // Drain the registered handles' outcomes so nothing leaks.
    i = 0;
    while (i < MAX_SLOTS) : (i += 1) {
        try sched.driveInflight(i);
        sched.slot(i).outcome.?.deinit(a);
        sched.releaseSlot(i);
    }
}

test "scheduler: waitForAny returns the FIRST-completing slot (wait-for-any, not block-on-one)" {
    const a = testing.allocator;
    // Three workers with different poll countdowns so completion order ≠
    // registration order: slot0 needs 3 polls, slot1 0, slot2 1.
    var fake = spawn.FakeSpawnerState.init(a, 0, "OUT", "");
    defer fake.deinit();
    const seq = [_]u32{ 3, 0, 1 };
    fake.poll_until_done_seq = &seq;

    var sched = Scheduler.init(a, std.testing.io);
    sched.spawner = fake.spawner();

    const cos: [3]*c.lua_State = .{
        @ptrFromInt(0x1000),
        @ptrFromInt(0x2000),
        @ptrFromInt(0x3000),
    };
    for (0..3) |k| {
        const h = try sched.spawner.?.start(a, std.testing.io, .{
            .role = .coder,
            .worktree_path = "/tmp/wt",
            .brief = "",
            .role_spec = "",
            .env_map = null,
        });
        _ = try sched.registerInflight(cos[k], h, null);
    }
    try testing.expectEqual(@as(usize, 3), sched.inflightCount());

    // First wait-for-any: slot1 (0 remaining) completes first — proves the
    // FIRST-completing worker is returned, not slot0 (registered first).
    const first = try sched.waitForAny();
    try testing.expectEqual(@as(usize, 1), first);
    sched.slot(first).outcome.?.deinit(a);
    sched.releaseSlot(first);
    try testing.expectEqual(@as(usize, 2), sched.inflightCount());

    // Second: slot2 (1 remaining) finishes before slot0 (3 remaining).
    const second = try sched.waitForAny();
    try testing.expectEqual(@as(usize, 2), second);
    sched.slot(second).outcome.?.deinit(a);
    sched.releaseSlot(second);

    // Third: only slot0 left.
    const third = try sched.waitForAny();
    try testing.expectEqual(@as(usize, 0), third);
    sched.slot(third).outcome.?.deinit(a);
    sched.releaseSlot(third);

    try testing.expectEqual(@as(usize, 0), sched.inflightCount());
    // Nothing in flight → NoInflight.
    try testing.expectError(error.NoInflight, sched.waitForAny());
}

test "scheduler: pure-Lua run leaves Io null and never reaches the worker drive (task 3260)" {
    // The undefined_io replacement: a scheduler with no spawner+io is valid; the
    // worker-drive paths are simply never reached (no worker is registered).
    const a = testing.allocator;
    var sched = Scheduler.init(a, null);
    try testing.expect(sched.io == null);
    try testing.expect(sched.spawner == null);
    try testing.expectEqual(@as(usize, 0), sched.inflightCount());
    // No registration ⇒ waitForAny reports NoInflight rather than touching Io.
    try testing.expectError(error.NoInflight, sched.waitForAny());
}

// ---------------------------------------------------------------------------
// Wall-clock-timeout tests (task 3188) — a settable fake clock the test
// advances past a deadline DETERMINISTICALLY. No real sleep-past-a-deadline.
// ---------------------------------------------------------------------------

/// FakeClock is a settable monotonic clock the test controls. `now_ns` is the
/// value `clockFn` returns; the test advances it to cross a deadline without any
/// real elapsed time — so the timeout fires deterministically and the test is
/// neither slow nor flaky.
const FakeClock = struct {
    now_ns: i128 = 0,
    fn clockFn(ctx: ?*anyopaque, io: std.Io) i128 {
        _ = io;
        const self: *FakeClock = @ptrCast(@alignCast(ctx.?));
        return self.now_ns;
    }
};

test "scheduler: a hung worker past its deadline is timed out (kill + synthetic outcome)" {
    const a = testing.allocator;
    // One worker that is HUNG: poll returns null forever.
    var fake = spawn.FakeSpawnerState.init(a, 0, "NEVER", "");
    defer fake.deinit();
    const hung_seq = [_]bool{true};
    fake.hung_starts_seq = &hung_seq;

    var clock = FakeClock{ .now_ns = 1000 };

    var sched = Scheduler.init(a, std.testing.io);
    sched.spawner = fake.spawner();
    sched.clock_fn = FakeClock.clockFn;
    sched.clock_ctx = &clock;
    sched.max_wall_clock_ns = 500; // tiny budget for the test.

    const fake_co: *c.lua_State = @ptrFromInt(0x1000);
    const handle = try sched.spawner.?.start(a, std.testing.io, .{
        .role = .coder,
        .worktree_path = "/tmp/wt",
        .brief = "B",
        .role_spec = "",
        .env_map = null,
    });
    const idx = try sched.registerInflight(fake_co, handle, null);
    // Deadline = 1000 (register-time now) + 500 = 1500.
    try testing.expectEqual(@as(i128, 1500), sched.slot(idx).deadline_ns);

    // Before the deadline: pollReadyOnce reports nothing terminal (hung), no kill.
    clock.now_ns = 1400;
    try testing.expect((try sched.pollReadyOnce()) == null);
    try testing.expectEqual(@as(u32, 0), fake.kill_count);
    try testing.expect(!sched.slot(idx).timed_out);

    // Advance PAST the deadline: the timeout fires.
    clock.now_ns = 1600;
    const ready = try sched.pollReadyOnce();
    try testing.expectEqual(@as(?usize, idx), ready);
    // Kill was called on THIS worker.
    try testing.expectEqual(@as(u32, 1), fake.kill_count);
    try testing.expect(fake.killedWorker(0));
    // The slot is now terminal with the timed-out marker + synthetic outcome.
    try testing.expect(sched.slot(idx).timed_out);
    try testing.expect(sched.slot(idx).outcome != null);
    try testing.expectEqual(Scheduler.TIMED_OUT_EXIT_CODE, sched.slot(idx).outcome.?.exit_code);

    // Cleanup.
    sched.slot(idx).outcome.?.deinit(a);
    sched.releaseSlot(idx);
}

test "scheduler: a worker that completes before its deadline is NOT timed out (no kill)" {
    const a = testing.allocator;
    var fake = spawn.FakeSpawnerState.init(a, 0, "DONE", "");
    defer fake.deinit();
    // poll_until_done = 0 ⇒ the first poll returns the outcome (completes).

    var clock = FakeClock{ .now_ns = 0 };

    var sched = Scheduler.init(a, std.testing.io);
    sched.spawner = fake.spawner();
    sched.clock_fn = FakeClock.clockFn;
    sched.clock_ctx = &clock;
    sched.max_wall_clock_ns = 100;

    const fake_co: *c.lua_State = @ptrFromInt(0x1000);
    const handle = try sched.spawner.?.start(a, std.testing.io, .{
        .role = .coder,
        .worktree_path = "/tmp/wt",
        .brief = "B",
        .role_spec = "",
        .env_map = null,
    });
    const idx = try sched.registerInflight(fake_co, handle, null);

    // Even after advancing past the deadline, the worker already completed —
    // pass 1 of pollReadyOnce returns the natural outcome BEFORE the deadline
    // pass can fire, so kill is never called.
    clock.now_ns = 9999;
    const ready = try sched.pollReadyOnce();
    try testing.expectEqual(@as(?usize, idx), ready);
    try testing.expectEqual(@as(u32, 0), fake.kill_count);
    try testing.expect(!sched.slot(idx).timed_out);
    try testing.expectEqual(@as(u32, 0), sched.slot(idx).outcome.?.exit_code);
    try testing.expectEqualStrings("DONE", sched.slot(idx).outcome.?.stdout);

    sched.slot(idx).outcome.?.deinit(a);
    sched.releaseSlot(idx);
}

test "scheduler: driveInflight (N=1) times out a hung worker via the deadline poll loop" {
    const a = testing.allocator;
    var fake = spawn.FakeSpawnerState.init(a, 0, "X", "");
    defer fake.deinit();
    const hung_seq = [_]bool{true};
    fake.hung_starts_seq = &hung_seq;

    // The clock advances on each read so the poll loop crosses the deadline
    // without any real sleep. Start at 0; each clockNow read bumps it.
    const Advancing = struct {
        now_ns: i128 = 0,
        fn clockFn(ctx: ?*anyopaque, io: std.Io) i128 {
            _ = io;
            const self: *@This() = @ptrCast(@alignCast(ctx.?));
            const v = self.now_ns;
            self.now_ns += 1_000_000; // +1ms per read.
            return v;
        }
    };
    var clk = Advancing{};

    var sched = Scheduler.init(a, std.testing.io);
    sched.spawner = fake.spawner();
    sched.clock_fn = Advancing.clockFn;
    sched.clock_ctx = &clk;
    sched.max_wall_clock_ns = 5_000_000; // 5ms budget → loop crosses it quickly.

    const fake_co: *c.lua_State = @ptrFromInt(0x1000);
    const handle = try sched.spawner.?.start(a, std.testing.io, .{
        .role = .coder,
        .worktree_path = "/tmp/wt",
        .brief = "B",
        .role_spec = "",
        .env_map = null,
    });
    const idx = try sched.registerInflight(fake_co, handle, null);

    // driveInflight polls a hung worker; once the advancing clock passes the
    // deadline it times out (kill + synthetic outcome) and returns.
    try sched.driveInflight(idx);
    try testing.expect(sched.slot(idx).timed_out);
    try testing.expectEqual(@as(u32, 1), fake.kill_count);
    try testing.expectEqual(Scheduler.TIMED_OUT_EXIT_CODE, sched.slot(idx).outcome.?.exit_code);

    sched.slot(idx).outcome.?.deinit(a);
    sched.releaseSlot(idx);
}

test "scheduler: in a 2-worker set one hangs + times out while the other completes (slot reclaimed, no hang)" {
    const a = testing.allocator;
    var fake = spawn.FakeSpawnerState.init(a, 0, "OUT", "");
    defer fake.deinit();
    // Worker 0 hung; worker 1 completes immediately (poll 0).
    const hung_seq = [_]bool{ true, false };
    fake.hung_starts_seq = &hung_seq;
    const poll_seq = [_]u32{ 0, 0 };
    fake.poll_until_done_seq = &poll_seq;

    var clock = FakeClock{ .now_ns = 0 };
    var sched = Scheduler.init(a, std.testing.io);
    sched.spawner = fake.spawner();
    sched.clock_fn = FakeClock.clockFn;
    sched.clock_ctx = &clock;
    sched.max_wall_clock_ns = 100;

    const cos: [2]*c.lua_State = .{ @ptrFromInt(0x1000), @ptrFromInt(0x2000) };
    for (0..2) |k| {
        const h = try sched.spawner.?.start(a, std.testing.io, .{
            .role = .coder,
            .worktree_path = "/tmp/wt",
            .brief = "",
            .role_spec = "",
            .env_map = null,
        });
        _ = try sched.registerInflight(cos[k], h, null);
    }
    try testing.expectEqual(@as(usize, 2), sched.inflightCount());

    // First wait-for-any (clock under deadline): the NORMAL worker (slot1)
    // completes; the hung worker is NOT timed out yet.
    clock.now_ns = 50;
    const first = try sched.waitForAny();
    try testing.expectEqual(@as(usize, 1), first);
    try testing.expect(!sched.slot(first).timed_out);
    sched.slot(first).outcome.?.deinit(a);
    sched.releaseSlot(first);
    try testing.expectEqual(@as(u32, 0), fake.kill_count);

    // Now advance past the deadline: the hung worker (slot0) times out, so
    // waitForAny RETURNS rather than hanging forever on the hung child.
    clock.now_ns = 200;
    const second = try sched.waitForAny();
    try testing.expectEqual(@as(usize, 0), second);
    try testing.expect(sched.slot(second).timed_out);
    try testing.expectEqual(@as(u32, 1), fake.kill_count);
    try testing.expect(fake.killedWorker(0));
    sched.slot(second).outcome.?.deinit(a);
    sched.releaseSlot(second);

    try testing.expectEqual(@as(usize, 0), sched.inflightCount());
}
