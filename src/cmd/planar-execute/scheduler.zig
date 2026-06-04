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
};

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

    pub fn init(allocator: std.mem.Allocator, io: ?std.Io) Scheduler {
        return .{ .allocator = allocator, .io = io };
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
        for (&self.slots, 0..) |*s, idx| {
            if (s.in_use) continue;
            s.* = .{
                .in_use = true,
                .co = co,
                .handle = handle,
                .outcome = null,
                .payload = payload,
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

    /// driveInflight waits the slot's in-flight worker to its terminal state
    /// (a blocking `wait` on the single handle) and stores the outcome on the
    /// slot. The continuation reads it from there. This is the N=1 fast path
    /// (one coroutine, one worker) and the per-`agent()` drive in `runModule`.
    pub fn driveInflight(self: *Scheduler, idx: usize) spawn.SpawnError!void {
        const s = &self.slots[idx];
        std.debug.assert(s.in_use);
        const sp = self.spawner orelse @panic(
            "scheduler.driveInflight: no spawner installed but a worker was registered — the agent() host must only register an in-flight worker when a driver/spawner is wired.",
        );
        s.outcome = try sp.wait(self.allocator, self.unwrapIo(), &s.handle);
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
        for (&self.slots, 0..) |*s, idx| {
            if (!s.in_use) continue;
            if (s.outcome != null) continue; // already terminal, awaiting consume
            if (try sp.poll(self.allocator, io, &s.handle)) |outcome| {
                s.outcome = outcome;
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
