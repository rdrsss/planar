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
//! ## SCOPE — single coroutine, single in-flight worker (this cycle)
//!
//! This module proves the yield/resume machinery for ONE coroutine driving ONE
//! in-flight worker (N=1). For N=1 the observable behavior is IDENTICAL to M4:
//! the script calls `agent()` and gets the same result table. The change is
//! purely the internal mechanism (blocking → yield/resume).
//!
//! The structure generalizes to N coroutines (task 3183, `parallel`): the slot
//! registry is an array, `driveCoroutine` runs a resume loop, and the
//! in-flight-worker drive is a `wait` on the single handle (which 3183 will
//! generalize to wait-for-any across N handles via the `Spawner.poll` surface).
//! No `parallel`/`pipeline` (3183/3184), no heartbeat/timeout/SIGINT (M6), no
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
    io: std.Io,
    slots: [MAX_SLOTS]Slot = .{Slot{}} ** MAX_SLOTS,

    pub fn init(allocator: std.mem.Allocator, io: std.Io) Scheduler {
        return .{ .allocator = allocator, .io = io };
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
    /// (N=1: a blocking `wait` on the single handle) and stores the outcome on
    /// the slot. The continuation reads it from there.
    ///
    /// For N>1 (task 3183) this is where a wait-for-any across the in-use slots'
    /// handles goes (poll-loop with a small sleep, or block on whichever
    /// completes first); for N=1 a blocking wait on the one handle is exactly
    /// correct and the scheduler resumes the single coroutine right after.
    pub fn driveInflight(self: *Scheduler, idx: usize) spawn.SpawnError!void {
        const s = &self.slots[idx];
        std.debug.assert(s.in_use);
        const sp = self.spawner orelse @panic(
            "scheduler.driveInflight: no spawner installed but a worker was registered — the agent() host must only register an in-flight worker when a driver/spawner is wired.",
        );
        s.outcome = try sp.wait(self.allocator, self.io, &s.handle);
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
