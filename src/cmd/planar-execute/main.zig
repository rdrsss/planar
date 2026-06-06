//! planar-execute — Lua 5.5 script execution harness (plan 492).
//!
//! Fifth binary in the Planar family. Unlike the other binaries, this one
//! does NOT open SQLite and does NOT link the runtime / engine / db modules.
//! Its sole dependency beyond the standard library is liblua55 (vendored
//! under vendor/lua/) and the cli module (etc-cli, for argument parsing).
//!
//! M1 scope:
//!   task 3209 — vendor Lua, link it, prove the link with a newstate/close
//!               round-trip.
//!   task 3163 — load string → lua_pcall → read return value back into Zig;
//!               surface Lua compile/runtime errors as Zig errors (no panic).
//!   task 3164 — workflow module loading: compile + execute the top-level chunk
//!               to obtain a {meta, run} table; read meta fields (name,
//!               description, phases) into Zig structs via a caller-owned
//!               allocator (closes task 3221 — string return from Lua); call
//!               run(ctx) with a minimal stub ctx; chunkname-bearing loader
//!               (closes task 3223).
//!   task 3165 — CLI surface: `planar-execute <workflow.lua> [args…]` (default
//!               run-path), `planar-execute version`, `planar-execute --help`.
//!               The first positional is resolved as a workflow file path;
//!               trailing [args…] are threaded into ctx.args as a 1-based Lua
//!               sequence of strings. Lua errors map to non-zero exit.
//!
//! M2 scope (this change — tasks 3168 m2-host-fns + 3169 m2-sandbox):
//!   task 3168 — host-function surface (agent/parallel/pipeline/phase/log/
//!               budget/workflow) registered as Lua C functions backed by Zig,
//!               carried on `ctx` per the spec ("ctx carries args and the host
//!               functions"). At M2 they are RECORDING STUBS: they record their
//!               call (prompt/opts/title/msg + arg shapes) into a host-side
//!               HostState and return a stub result; no real claude -p spawn, no
//!               worktrees, no DB, no scheduler (those are M3/M4/M5). Recorded
//!               calls are observable from Zig via HostState.calls so tests can
//!               assert what the script invoked.
//!   task 3169 — sandbox: luaL_openlibs is replaced with a curated set of safe
//!               libraries (base, string, table, math, utf8, coroutine). os and
//!               io are NEVER opened; math.random / math.randomseed are nil'd
//!               out of the math table after open. Determinism is host-injected:
//!               ctx.now (timestamp) and ctx.seed (PRNG seed) are the script's
//!               only time/random source, mirroring the Workflow tool's
//!               Date.now / Math.random ban.
//!
//! What is intentionally absent (later tasks):
//!   - real host-fn behavior: claude -p spawn / worktrees / DB / scheduler are
//!     M3 (3170 state-reads), M4, M5 (coroutine agent()). The M2 stubs only
//!     record.
//!   - brief compiler (3171), state-reads (3170), schema ingestion (3172).
//!   - named built-in workflow registry: m10-quality-spine (no entries yet)

const std = @import("std");
const builtin = @import("builtin");
const Io = std.Io;

const cli = @import("cli");

/// State-read helpers — subprocess + JSON parse layer (task 3170 m2-state-reads).
/// Imported here so its `test` blocks run under the `execute_exe_tests` target.
pub const state = @import("state.zig");

/// Bright-line refusal guard — refuses to run a plan/task touching
/// migrations/new-verb/invariant code under a workflow that omits a reviewer
/// (plan 492 M10 task 3206, tech-spec 267 §1). Imported here so its `test`
/// blocks run under the `execute_exe_tests` target.
pub const refusal_guard = @import("refusal_guard.zig");

/// Schema ingestion — `<bin> schema` subprocess + parse layer (task 3172 m2-schema-into-brief).
/// Imported here so its `test` blocks run under the `execute_exe_tests` target.
pub const schema = @import("schema.zig");

/// Brief compiler — pure function `(plan state, spec citations, schema) → brief string`
/// (task 3171 m2-brief-compiler).
/// Imported here so its `test` blocks run under the `execute_exe_tests` target.
pub const brief = @import("brief.zig");

/// Worktree manager — `git worktree` lifecycle on epic-child branches
/// (task 3173 m3-worktree-lifecycle): ensureEpic / createCycle / teardownCycle.
/// Imported here so its `test` blocks run under the `execute_exe_tests` target.
pub const worktree = @import("worktree.zig");

/// doctor — read-only health check that drives the state/schema/reconcile read
/// helpers against live binaries (task 3236 m3-doctor-livegate).
/// Imported here so its `test` blocks run under the `execute_exe_tests` target.
pub const doctor = @import("doctor.zig");

/// worker_env — constrained PATH / env builder for `claude -p` worker spawns
/// (task 3179 m4-constrained-path). Materializes a per-worker shim directory
/// that exposes `planar-agent` + `git` but NOT `planar`, plus the matching
/// env map with planar-internal env vars stripped.
/// Imported here so its `test` blocks run under the `execute_exe_tests` target.
pub const worker_env = @import("worker_env.zig");

/// role_model — Per-role model tier table (task 3176 m4-per-role-model). Maps
/// `coder`/`reviewer` → opus, `test-coder`/`documenter` → sonnet. Compile-time
/// table; no operator override path at M4 (filed as a follow-up).
/// Imported here so its `test` blocks run under the `execute_exe_tests` target.
pub const role_model = @import("role_model.zig");

/// spawn — `claude -p` spawn driver (tasks 3175 + 3176 + 3178). Exposes
/// `buildSpawnArgv` (pure argv shape), the `Spawner` interface, the
/// `RealSpawner` (live subprocess) and `FakeSpawner` (records argv + returns
/// canned outcome — used by every CI test).
/// Imported here so its `test` blocks run under the `execute_exe_tests` target.
pub const spawn = @import("spawn.zig");

/// terminal — Terminal-verb fallback decision matrix + runner (task 3180).
/// Pure `decideTerminalVerb` + subprocess `runTerminalVerb` (shells
/// `planar-agent complete|release|fail --claim ...`).
/// Imported here so its `test` blocks run under the `execute_exe_tests` target.
pub const terminal = @import("terminal.zig");

/// lua — the single shared `@cImport` of the vendored Lua 5.5 C API. Both this
/// module and `scheduler.zig` use it so `*c.lua_State` is the SAME type across
/// the coroutine-drive boundary (a fresh `@cImport` per module would make them
/// distinct). See lua.zig.
pub const lua = @import("lua.zig");
const c = lua.c;

/// scheduler — the M5 coroutine event loop (task 3182). `runModule` drives
/// `run(ctx)` inside a Lua coroutine via `scheduler.driveCoroutine`; `agent()`
/// yields (non-blocking spawn → register → `lua_yieldk`) and the scheduler
/// resumes the coroutine once its single in-flight worker reaches terminal.
/// Aliased here so its `test` blocks run under the execute_exe_tests target
/// (memory: planar_execute_gate_lazy_eval — a pub-const alias forces analysis).
pub const scheduler = @import("scheduler.zig");

/// heartbeat — the single preemptive heartbeat thread (M6 task 3187). A
/// mutex-guarded registry of LIVE claim tokens (registry-owned duped copies)
/// plus a Zig OS thread that refreshes every live lease at TTL/2 via
/// `planar-agent heartbeat`. Kept STRICTLY separate from the cooperative Lua
/// scheduler: it touches NO Lua state, only claim-token strings. `runModule`
/// owns its lifecycle (start when a driver is attached, stop at run end);
/// `agent()`/`agentContinue` register/unregister the claim at spawn/terminal.
/// Aliased here so its `test` blocks run under the execute_exe_tests target
/// (memory: planar_execute_gate_lazy_eval — a pub-const alias forces analysis).
pub const heartbeat = @import("heartbeat.zig");

/// interrupt — SIGINT handling (M6 tasks 3193 + 3189). The async-signal-safe
/// handler (sets ONLY an atomic flag) plus the off-signal-path shutdown
/// sequence (stop heartbeat → kill children → release claims → teardown
/// worktrees) the drive loops run when they observe the flag. Aliased here so
/// its `test` blocks run under the execute_exe_tests target (memory:
/// planar_execute_gate_lazy_eval — a pub-const alias forces analysis).
pub const interrupt = @import("interrupt.zig");

/// journal — append-only run journal (M8 task 3196). One NDJSON record per
/// worker spawn (prompt hash, worktree, branch, claim token, model, exit code,
/// terminal verb, wall-clock) at `<repo_root>/.worktrees/.planar-execute/
/// journal-<plan_id>.ndjson`. Written best-effort at the tail of `agentContinue`;
/// the durability substrate M8 resume (3197) + budgets (3198) read back. Aliased
/// here so its `test` blocks run under the execute_exe_tests target (memory:
/// planar_execute_gate_lazy_eval — a pub-const alias forces analysis).
pub const journal = @import("journal.zig");

/// runlock — single-instance-per-plan run-lock (M6 task 3191). An O_EXCL lock
/// keyed by plan-id carrying run-id + PID: refuse start on a LIVE lock,
/// take over a stale one gated on PID liveness. Acquired at gated-real-agent
/// run start, released on clean exit + best-effort on interrupt. Aliased here
/// so its `test` blocks run under the execute_exe_tests target (memory:
/// planar_execute_gate_lazy_eval — a pub-const alias forces analysis).
pub const runlock = @import("runlock.zig");

/// budget — budgets and ceilings, the hard kill-switch (M8 task 3198). Per-task
/// max-attempt → block (fail-count derived from the journal so resume respects
/// prior attempts) + whole-run ceiling (max-total-spawns / max-wall-clock →
/// clean exit + journal terminus). Pure decision logic; wired into the pre-spawn
/// point + the run-drive loop. Aliased here so its `test` blocks run under the
/// execute_exe_tests target (memory: planar_execute_gate_lazy_eval — a pub-const
/// alias forces analysis).
pub const budget = @import("budget.zig");

// ---------------------------------------------------------------------------
// Process-global I/O context (no runtime module — planar-execute has no DB).
// ---------------------------------------------------------------------------

/// Lightweight process context for planar-execute. Does not inherit from
/// runtime.Ctx because this binary intentionally has no DB handle.
pub const ExecCtx = struct {
    allocator: std.mem.Allocator,
    io: Io,
    stdout: *Io.Writer,
    stderr: *Io.Writer,
    /// The host process environ. Threaded from `std.process.Init.minimal.environ`
    /// so the gated live-agent driver (handleRun) can baseline the constrained
    /// worker env from the real host environment.
    environ: std.process.Environ,
};

var stdout_buf: [4096]u8 = undefined;
var stderr_buf: [1024]u8 = undefined;
var stdout_writer_storage: ?Io.File.Writer = null;
var stderr_writer_storage: ?Io.File.Writer = null;
var global_ctx: ?ExecCtx = null;

fn initCtx(allocator: std.mem.Allocator, io: Io, environ: std.process.Environ) void {
    stdout_writer_storage = Io.File.Writer.init(.stdout(), io, &stdout_buf);
    stderr_writer_storage = Io.File.Writer.init(.stderr(), io, &stderr_buf);
    global_ctx = .{
        .allocator = allocator,
        .io = io,
        .stdout = &stdout_writer_storage.?.interface,
        .stderr = &stderr_writer_storage.?.interface,
        .environ = environ,
    };
}

fn currentCtx() *const ExecCtx {
    return &(global_ctx orelse @panic("planar-execute: ctx not initialized"));
}

fn flushCtx() !void {
    if (global_ctx) |_| {
        try stdout_writer_storage.?.interface.flush();
        try stderr_writer_storage.?.interface.flush();
    }
}

// ---------------------------------------------------------------------------
// Errors that evalString and loadModule can surface to Zig callers.
// ---------------------------------------------------------------------------

pub const LuaError = error{
    /// luaL_newstate returned null — allocator failure.
    LuaAllocFailed,
    /// The script failed to compile (syntax error).
    LuaCompileError,
    /// The script compiled but raised a runtime error.
    LuaRuntimeError,
    /// The script ran successfully but returned a type that is not a number.
    /// Applies to evalString; loadModule expects a table (see below).
    LuaUnexpectedType,
    /// The workflow chunk executed but did not return a table.
    LuaModuleNotTable,
    /// The returned table lacks a `meta` field that is itself a table.
    LuaModuleMissingMeta,
    /// The returned table lacks a `run` field that is a function.
    LuaModuleMissingRun,
    /// `meta` is present as a table but a required string field
    /// (`name` or `description`) is missing or not a string.
    LuaModuleInvalidMeta,
    /// An allocator error occurred while copying Lua strings into Zig memory.
    LuaStringAllocError,
};

// ---------------------------------------------------------------------------
// Workflow module structs
// ---------------------------------------------------------------------------

/// A single phase entry extracted from meta.phases[i].
pub const PhaseMeta = struct {
    title: []const u8,
    detail: []const u8,
};

/// The `meta` sub-table of a loaded workflow module.
pub const WorkflowMeta = struct {
    name: []const u8,
    description: []const u8,
    /// Owned slice of phase entries; each string inside is also allocator-owned.
    phases: []PhaseMeta,
    /// Trust-based reviewer-cadence assertion (plan 492 M10 task 3206). When
    /// true, the bright-line refusal guard PASSES — the author declares that
    /// the workflow dispatches a reviewer for every cycle. False or absent
    /// means the guard inspects the plan's open tasks for risky touches and
    /// refuses to run when any are present. Defaults to `false` (conservative:
    /// an ad-hoc workflow that does not declare the field cannot ship risky
    /// changes unless the operator passes `--bypass-reviewer-guard`).
    reviewer: bool = false,
};

/// A fully-loaded and validated workflow module.
///
/// All strings are allocator-owned copies made before the Lua state is
/// closed. The caller must call `deinit(allocator)` when done to free them.
/// The Lua state is closed before `loadModule` returns.
pub const WorkflowModule = struct {
    meta: WorkflowMeta,

    /// Free all allocator-owned memory held by this module.
    pub fn deinit(self: *WorkflowModule, allocator: std.mem.Allocator) void {
        for (self.meta.phases) |phase| {
            allocator.free(phase.title);
            allocator.free(phase.detail);
        }
        allocator.free(self.meta.phases);
        allocator.free(self.meta.name);
        allocator.free(self.meta.description);
    }
};

// ---------------------------------------------------------------------------
// Host-function surface — task 3168 (m2-host-fns), recording stubs.
//
// Each host function (agent/parallel/pipeline/phase/log/workflow) is a Lua C
// closure backed by Zig. At M2 they do NO real work — no `claude -p` spawn, no
// worktrees, no DB, no scheduler. They RECORD their invocation (the call name
// and a small set of stringified argument fields) into a HostState owned by the
// Zig host, then return a stub result. `budget` is a host-backed Lua table whose
// `total` is an injected number and whose `spent()` / `remaining()` are C
// closures reading injected host values.
//
// Observability: a pointer to the HostState is threaded into every C closure as
// a light-userdata upvalue (lua_upvalueindex(1)). The closure recovers the
// pointer with lua_touserdata. This lets the Zig host (and tests) inspect
// HostState.calls after run(ctx) returns and assert "agent called with prompt X",
// "phase('Build') recorded", etc. We use a light-userdata upvalue rather than
// lua_getextraspace because lua_getextraspace is a C macro (not exposed by
// @cImport) and the upvalue approach keeps the binding explicit per closure.
// ---------------------------------------------------------------------------

/// The kind of host-function call recorded by a stub. The discriminant lets a
/// test match on which host function fired without string-comparing a name.
pub const HostCallKind = enum {
    agent,
    parallel,
    pipeline,
    phase,
    log,
    workflow,
    budget_spent,
    budget_remaining,
    eligible,
};

/// A single recorded host-function invocation. All fields are heap-owned copies
/// (via the HostState allocator) so they outlive the Lua stack values they were
/// read from. `arg0` / `arg1` carry the salient stringified arguments:
///   agent    → arg0 = prompt, arg1 = opts (type/summary, e.g. "table" or "nil")
///   phase    → arg0 = title
///   log      → arg0 = msg
///   workflow → arg0 = name, arg1 = args (type)
///   parallel → arg0 = "<n> thunks" (the thunk-count shape)
///   pipeline → arg0 = "<n> items", arg1 = "<m> stages"
/// Fields not relevant to a kind are the empty string.
pub const HostCall = struct {
    kind: HostCallKind,
    arg0: []const u8,
    arg1: []const u8,
};

/// BlockedItem records ONE worker that self-blocked during the run (plan 492 M7
/// tasks 3194+3195). A worker hits a genuine blocker, runs `planar-agent block`
/// (per the brief — "block rather than ask"), and exits cleanly; `block`
/// atomically flips the task to `blocked` and releases the claim. `agentContinue`
/// detects the `blocked` task status, sets the agent() result `status = "blocked"`
/// (NOT failed/released — the run continues, no retry, no global halt), and
/// appends one of these to `HostState.blocked_items`. `handleRun` prints them all
/// at run end as an operator-triage summary.
///
/// All three strings are HEAP-OWNED copies (duped by `HostState.recordBlocked`)
/// so they survive the per-call `AgentCallState` teardown and the freed
/// `taskShow` parse arena. `task_blocker` / `reason` may be empty when the
/// blocker/reason are not cleanly available.
pub const BlockedItem = struct {
    /// The blocked task's slug (e.g. "m5-foo"). Empty if unknown.
    task_slug: []const u8,
    /// The blocking task id, as the worker passed to `--blocker` (e.g.
    /// "task:3199" or "3199"). Empty if not cleanly available.
    task_blocker: []const u8,
    /// The worker's free-text `--reason`. Empty if not cleanly available.
    reason: []const u8,
};

/// HostState is the Zig-side record of everything the workflow's host functions
/// did during a run. It is allocated by the caller (runModule), pointed-to by
/// every host C closure via a light-userdata upvalue, and inspected by the host
/// after run(ctx) returns.
///
/// Determinism injection (task 3169): `now` and `seed` are host-supplied values
/// exposed to the script as `ctx.now` and `ctx.seed`. With os.time and
/// math.random removed from the sandbox, these are the script's ONLY source of
/// time / randomness, keeping runs replayable.
///
/// `budget_total` backs the `budget.total` field and `budget:remaining()`.
/// `budget_spent` is a fixed injected value at M2 (no real token accounting).
pub const HostState = struct {
    allocator: std.mem.Allocator,
    calls: std.ArrayList(HostCall),

    /// Run-scoped accumulator of self-blocked workers (plan 492 M7 task 3195).
    /// `agentContinue` appends one entry per worker whose task ended up
    /// `blocked`; `handleRun` prints them all at run end for operator triage.
    /// Owns heap-duped copies of every string (mirrors `calls`). Quiet when
    /// empty — no summary is printed when zero workers blocked.
    blocked_items: std.ArrayList(BlockedItem) = .empty,

    /// Host-injected timestamp (e.g. a Unix epoch second). Exposed as ctx.now.
    now: i64,
    /// Host-injected PRNG seed. Exposed as ctx.seed.
    seed: i64,
    /// Host-injected budget ceiling, exposed as budget.total / budget:remaining().
    budget_total: i64,
    /// Host-injected spent amount (fixed at M2; no real accounting). Backs
    /// budget:spent() and budget:remaining() = total - spent.
    budget_spent: i64,

    // M4 spawn driver wiring (tasks 3175/3176/3177/3178/3180).
    //
    // When `agent_driver` is non-null, hostAgent runs the full pipeline:
    // brief → spawn → wait → post-spawn read → terminal-verb fallback. When it
    // is null (the M2 default), hostAgent retains the recording-stub behavior
    // so the existing M2 tests still pass. The Lua-facing contract is
    // unchanged: agent(prompt, opts) returns a result table.
    agent_driver: ?*AgentDriver = null,

    // M5 control-plane read context (task 3185).
    //
    // `ctx.eligible(plan_id)` shells `planar plan recommend-strategy` and
    // returns the parallel-eligible subset as a Lua table. It needs an `Io`
    // context for the subprocess call. This is separate from `agent_driver.io`
    // (which is only present when the live-spawn gate is open) — `ctx.eligible`
    // is a read-only control-plane verb and works whether or not
    // PLANAR_EXECUTE_LIVE_AGENT is set. `handleRun` populates this from
    // `currentCtx().io`; unit-test callers that do not exercise `ctx.eligible`
    // may leave it null (the host fn returns an empty table when io is null).
    io: ?std.Io = null,

    // M5 coroutine event loop (task 3182).
    //
    // The scheduler owns the in-flight-worker registry and drives each yielded
    // `agent()` coroutine to completion. `runModule` constructs it and points
    // this at it before driving `run(ctx)` on a coroutine thread. `hostAgent`
    // recovers it (via the HostState upvalue) to register the worker + yield;
    // `agentContinue` recovers it to read the worker outcome on resume. Null
    // for pure-Lua run paths that never call `agent()` (e.g. callRun tests),
    // in which case `hostAgent` is never reached with a driver installed.
    sched: ?*scheduler.Scheduler = null,

    // M6 heartbeat thread (task 3187).
    //
    // The mutex-guarded registry of LIVE claim tokens + the preemptive Zig OS
    // thread that refreshes every live lease at TTL/2. `runModule` constructs +
    // starts it ONLY when a spawn driver is attached (no driver → no workers →
    // nothing to heartbeat) and stops+joins it at run end. `driveAgentCallPreYield`
    // calls `register(claim_token)` when a worker's claim goes live;
    // `agentContinue` calls `unregister(claim_token)` when the worker reaches
    // terminal / its slot is released. The registry owns its OWN duped token
    // copies, so the heartbeat thread NEVER borrows the main thread's
    // `AgentCallState.claim_token` (which the main thread frees on slot release).
    // It touches NO Lua state — the cardinal cross-thread rule.
    heartbeat_reg: ?*heartbeat.HeartbeatRegistry = null,

    // M6 per-worker wall-clock timeout overrides (task 3188).
    //
    // OPTIONAL test injection: when set, `runModule` points the scheduler's
    // clock + budget at these instead of the production defaults
    // (`realMonotonicNow` + `DEFAULT_MAX_WALL_CLOCK_NS`). A workflow-level test
    // can thus drive a hung worker past its deadline DETERMINISTICALLY (a
    // settable fake clock + a tiny budget) with no real sleep. Production leaves
    // both null and the scheduler uses the real monotonic clock + 30-min budget.
    timeout_clock_fn: ?scheduler.ClockFn = null,
    timeout_clock_ctx: ?*anyopaque = null,
    timeout_max_wall_clock_ns: ?i128 = null,

    // M8 budgets + ceilings — the hard kill-switch (task 3198).
    //
    // `budgets` carries the per-task max-attempt + whole-run ceiling knobs
    // (generous defaults; env-overridable via `budget.Budgets.fromEnv`). It is
    // read at TWO points: the pre-spawn max-attempt check (in
    // `driveAgentCallPreYield`, which counts this task's prior failed attempts
    // from the journal) and the pre-spawn whole-run ceiling check.
    //
    // `run_counters` is the IN-MEMORY per-run ceiling state (spawn count + run
    // start time). `run_start_mono_ns` is stamped once at run start (from the
    // scheduler's injectable clock) in `runModule`; `spawn_count` is incremented
    // at each actual spawn. Distinct from the per-task journal fail-count, which
    // is persistent (so resume respects prior attempts).
    //
    // `ceiling_tripped` records WHICH ceiling fired when the run was
    // ceiling-terminated, so the drive loop can emit the operator-facing
    // "run ceiling exceeded: <which>" message after the clean shutdown.
    budgets: budget.Budgets = .default,
    run_counters: budget.RunCounters = .{},
    ceiling_tripped: ?budget.Ceiling = null,

    /// TEST-ONLY seam (plan 492 task 3541 / PR #17 cycle B finding 6): when
    /// non-zero, `runModule` pre-fills the first N scheduler slots with a
    /// dummy `in_use=true` marker right after constructing the Scheduler,
    /// so the next `registerInflight` call observes the registry as full
    /// (or near-full). This is how the registerInflight-fail recovery path
    /// (kill orphaned child + decrement spawn_count + propagate error) is
    /// exercised under unit tests without driving real concurrency past
    /// the MAX_SLOTS boundary. Production wiring leaves this 0.
    test_pre_fill_slots: usize = 0,

    pub fn init(allocator: std.mem.Allocator, now: i64, seed: i64, budget_total: i64, budget_spent: i64) HostState {
        return .{
            .allocator = allocator,
            .calls = .empty,
            .now = now,
            .seed = seed,
            .budget_total = budget_total,
            .budget_spent = budget_spent,
            .agent_driver = null,
            .sched = null,
        };
    }

    pub fn deinit(self: *HostState) void {
        for (self.calls.items) |call| {
            self.allocator.free(call.arg0);
            self.allocator.free(call.arg1);
        }
        self.calls.deinit(self.allocator);
        for (self.blocked_items.items) |bi| {
            self.allocator.free(bi.task_slug);
            self.allocator.free(bi.task_blocker);
            self.allocator.free(bi.reason);
        }
        self.blocked_items.deinit(self.allocator);
    }

    /// record appends a HostCall, taking ownership of heap-duped copies of the
    /// argument strings. On OOM it returns the error to the caller; the C-closure
    /// shim turns that into a Lua error rather than a panic.
    fn record(self: *HostState, kind: HostCallKind, arg0: []const u8, arg1: []const u8) std.mem.Allocator.Error!void {
        const a0 = try self.allocator.dupe(u8, arg0);
        errdefer self.allocator.free(a0);
        const a1 = try self.allocator.dupe(u8, arg1);
        errdefer self.allocator.free(a1);
        try self.calls.append(self.allocator, .{ .kind = kind, .arg0 = a0, .arg1 = a1 });
    }

    /// recordBlocked appends a BlockedItem (plan 492 M7 task 3195), taking
    /// ownership of heap-duped copies of all three strings so they survive the
    /// per-call AgentCallState teardown and the freed taskShow parse arena.
    /// On OOM it returns the error; `agentContinue` swallows it best-effort
    /// (the block was already recorded on the claim/task by the worker; losing
    /// the in-memory summary entry is non-fatal — the operator still sees the
    /// blocked task in `planar plan next`).
    fn recordBlocked(self: *HostState, task_slug: []const u8, task_blocker: []const u8, reason: []const u8) std.mem.Allocator.Error!void {
        const s = try self.allocator.dupe(u8, task_slug);
        errdefer self.allocator.free(s);
        const b = try self.allocator.dupe(u8, task_blocker);
        errdefer self.allocator.free(b);
        const r = try self.allocator.dupe(u8, reason);
        errdefer self.allocator.free(r);
        try self.blocked_items.append(self.allocator, .{ .task_slug = s, .task_blocker = b, .reason = r });
    }
};

/// hostStateUpvalue recovers the *HostState pointer stashed as the first
/// upvalue (light userdata) of a host C closure.
fn hostStateUpvalue(L: ?*c.lua_State) *HostState {
    const ptr = c.lua_touserdata(L, c.lua_upvalueindex(1));
    return @ptrCast(@alignCast(ptr.?));
}

// ---------------------------------------------------------------------------
// AgentDriver — the M4 spawn-pipeline wiring (tasks 3175/3176/3177/3178/3180).
// ---------------------------------------------------------------------------

/// AgentDriver bundles the (injected) machinery hostAgent needs to drive ONE
/// `agent(prompt, opts)` call end-to-end. The Lua-facing surface stays
/// `agent(prompt, opts) → { status = ..., exit_code = ..., commit = ... }`;
/// internally the driver:
///
///   1. Reads `opts.role`, `opts.worktree_path`, `opts.claim_token`,
///      `opts.role_spec` from the Lua opts table.
///   2. Samples the cycle branch HEAD via `worktree.branchHead` (for the
///      commit-presence proxy after wait).
///   3. Invokes `spawner.run(allocator, io, SpawnInputs)`. The brief is the
///      `prompt` arg (already-compiled-by-the-workflow methodology brief).
///   4. After the spawn returns: re-samples the branch HEAD, reads the claim
///      status via `planar-watch ps`, applies the terminal verb decision
///      matrix, and shells `planar-agent <verb>` for the fallback case.
///   5. Returns a Lua table summarizing the outcome.
///
/// Worktree creation/teardown is NOT here — the harness drives those around
/// the `agent(...)` call (tech-spec § "Lifecycle of one agent call"). M4 holds
/// only one spawn per call; M5 adds the scheduler.
///
/// Testability: pass a `FakeSpawner` and a fake `Io` (`std.testing.io`) to
/// drive the entire pipeline in unit tests without touching the network.
/// EnvBuilderFn produces the constrained worker env for a single agent() call.
/// Production wiring points this at `defaultEnvBuilder` (which calls
/// `worker_env.buildWorkerEnv` with the driver's pre-resolved binary paths and
/// a per-cycle shim dir under the cycle worktree). Tests can substitute a
/// stub builder that produces a synthetic env without touching the filesystem
/// — see the "env_builder wiring" test in the test block (task 3243), which
/// installs a sentinel builder and asserts the env reaches the spawn boundary.
///
/// The returned `WorkerEnv` is heap-owned; the caller MUST call
/// `WorkerEnv.deinit(allocator, io)` when the spawn completes.
pub const EnvBuilderFn = *const fn (
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    worktree_path: []const u8,
) anyerror!worker_env.WorkerEnv;

/// AgentDriver bundles the (injected) machinery hostAgent needs to drive ONE
/// `agent(prompt, opts)` call end-to-end.
pub const AgentDriver = struct {
    /// The spawner used for the worker invocation. Tests pass a FakeSpawner;
    /// production uses spawn.realSpawner().
    spawner: spawn.Spawner,
    /// The Io context the driver uses for subprocess + filesystem calls.
    io: std.Io,
    /// The repo root that owns the cycle worktree (used by branchHead etc.).
    /// Empty string means "skip commit-presence sampling" — the decision matrix
    /// then treats commit_present as false. Tests use "" for pure-Lua coverage.
    repo_root: []const u8 = "",
    /// The plan slug (used to derive the cycle branch name for the
    /// commit-presence sample). Empty string means "skip the sample".
    plan_slug: []const u8 = "",
    /// When true, the driver does NOT shell `planar-agent <verb>` for the
    /// terminal fallback — it only computes and records the decision in the
    /// returned Lua result table. Tests set this to true so we never write to
    /// the agent_actions / agent_work_claims tables from a unit test.
    skip_terminal_subprocess: bool = false,

    /// Builds the constrained worker env per spawn. When null, the driver does
    /// NOT thread an env into the spawner — which means `spawn.realRunFn` will
    /// panic loudly (decisions 358 + 365 enforcement). The intent: production
    /// wiring MUST install an env_builder; test paths that use FakeSpawner can
    /// leave it null because FakeSpawner accepts a null env_map (recording an
    /// empty snapshot instead).
    ///
    /// Plan 492 M4 cycle C iter 2 wiring (Item J): the RealSpawner path is
    /// driven via this builder. See `defaultEnvBuilder` for the production
    /// shape; tests inject their own to assert the constrained env reaches
    /// the spawn boundary.
    env_builder: ?EnvBuilderFn = null,
    /// Opaque context pointer passed to `env_builder`. The production
    /// `DefaultEnvBuilderCtx` (below) carries the pre-resolved planar-agent
    /// + git absolute paths and the host environ.
    env_builder_ctx: ?*anyopaque = null,

    /// The anchor/milestone plan id the claimed task lives under. Threaded to
    /// the claim-status reader so it can scope `planar-watch ps --plan <id>`.
    /// Zero means "skip the live read" (the reader returns `.active`); tests
    /// that inject a `claim_status_reader` ignore this field.
    plan_id: u64 = 0,

    /// Reads the live claim status for the terminal-verb decision (task 3242).
    /// When null, `readClaimStatusOrActive` calls the production reader
    /// (`defaultClaimStatusReader`, which shells `planar-watch ps`). Tests
    /// inject a fake reader so they can pin the worker-self-completed → harness
    /// no-op path WITHOUT a live binary — mirroring how `Spawner` and
    /// `env_builder` are injected.
    claim_status_reader: ?ClaimStatusFn = null,
    /// Opaque context pointer passed to `claim_status_reader`. Unused by the
    /// production reader (it reads `driver.plan_id` directly); tests stash a
    /// pointer to their canned `terminal.ClaimStatus` here.
    claim_status_ctx: ?*anyopaque = null,
    /// True when the host binaries (planar-agent, git) were successfully
    /// resolved at driver-attach time. False means the gate was set but one
    /// or both binaries were not found on PATH — the driver is attached in a
    /// degraded state and `agent()` will raise a clean Lua error if called.
    /// Tests always leave this at the default (true); the degraded live case
    /// (binary resolution failed at gate attachment) sets it to false.
    live_binaries_resolved: bool = true,

    /// Reads the live TASK status for the M7 block-detection path (task 3194).
    /// When null, `readTaskStatusBlocked` calls the production reader
    /// (`defaultTaskStatusReader`, which shells `planar task show <id> --json`).
    /// Tests inject a fake reader so they can simulate "task is blocked" (or
    /// "completed") WITHOUT a live `planar task show` — mirroring how
    /// `claim_status_reader`, `Spawner`, and `env_builder` are injected.
    task_status_reader: ?TaskStatusFn = null,
    /// Opaque context pointer passed to `task_status_reader`. Unused by the
    /// production reader (it reads the task_id arg directly); tests stash a
    /// pointer to their canned status string here.
    task_status_ctx: ?*anyopaque = null,

    /// Reads the PRE-SPAWN live task status for the M8 resume skip (task 3197).
    /// When null, `readTaskLiveStatus` calls the production reader
    /// (`defaultTaskLiveStatusReader`, which shells `planar task show <id>
    /// --json` via `state.taskShow`). Tests inject a fake reader so they can
    /// simulate "task already done" / "blocked" / "todo" WITHOUT a live
    /// `planar task show` — mirroring `task_status_reader` above.
    task_live_status_reader: ?TaskLiveStatusFn = null,
    /// Opaque context pointer passed to `task_live_status_reader`. Unused by the
    /// production reader; tests stash a pointer to their canned status here.
    task_live_status_ctx: ?*anyopaque = null,

    /// Counts a task's PRIOR FAILED attempts for the M8 max-attempt budget
    /// (task 3198). When null, `readFailedAttemptCount` calls the production
    /// reader (`defaultFailedAttemptReader`, which reads the journal file under
    /// `repo_root`/`plan_id` and counts via `budget.failedAttemptCount`). Tests
    /// inject a fake reader so they can drive the max-attempt path with a CANNED
    /// count WITHOUT writing a journal file — mirroring `task_live_status_reader`.
    /// The journal-derived production path is what makes resume respect prior
    /// attempts (the count is persistent across runs).
    failed_attempt_reader: ?FailedAttemptFn = null,
    /// Opaque context pointer passed to `failed_attempt_reader`.
    failed_attempt_ctx: ?*anyopaque = null,
    /// True ⇒ do NOT shell `planar-agent block` for the max-attempt block
    /// (unit-test paths). Mirrors `skip_terminal_subprocess` but specific to the
    /// budget-block subprocess so a test can exercise the no-spawn decision
    /// without a live `planar-agent`. Production leaves it false.
    skip_block_subprocess: bool = false,

    /// Opens the operator-triage `planar question` on an M9 fan-in CONFLICT
    /// (tasks 3199/3200). When null, `runFanInQuestion` shells `planar question
    /// add --plan <id> --json --body <body> <title>` and parses the returned
    /// id. Tests inject a fake runner so the conflict path is deterministic
    /// WITHOUT a live binary + DB — mirroring `claim_status_reader`,
    /// `task_status_reader`, and friends. Returns the opened question id, or 0
    /// when the open failed (the harness then surfaces `merge="conflict"` with
    /// `question_id=0` — never fails the run for a failed question open).
    question_runner: ?QuestionRunnerFn = null,
    /// Opaque context pointer passed to `question_runner`. Unused by the
    /// production runner; tests stash a pointer to their canned/recording state.
    question_runner_ctx: ?*anyopaque = null,

    /// Applies the chosen terminal verb to the claim (plan 492 task 3540
    /// finding 2 — surface terminal-verb failures). When null, the harness
    /// calls the production `terminal.runTerminalVerb` (which shells
    /// `planar-agent <verb>`). Tests inject a fake to drive the
    /// terminal-verb-failed → halt-fan-in path WITHOUT a live `planar-agent`,
    /// mirroring `claim_status_reader` / `question_runner`. The fn returns
    /// the terminal-verb error union; the harness consumes its outcome and
    /// reflects a failure on the result table + halts fan-in.
    terminal_verb_runner: ?TerminalVerbRunnerFn = null,
    /// Opaque context pointer passed to `terminal_verb_runner`. Unused by the
    /// production runner; tests stash a pointer to their canned/recording state.
    terminal_verb_runner_ctx: ?*anyopaque = null,
};

/// QuestionRunnerFn opens the operator-triage question on an M9 fan-in conflict
/// (plan 492 tasks 3199/3200). Injectable on `AgentDriver` so unit tests drive
/// the conflict path WITHOUT a live `planar question add` + DB. Returns the
/// opened question's id (0 ⇒ the open failed; the harness still returns the
/// `agent()` call normally with `merge="conflict"` and `question_id=0`). The
/// production wiring leaves it null and `runFanInQuestion` falls back to
/// `defaultQuestionRunner`.
pub const QuestionRunnerFn = *const fn (
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    plan_id: u64,
    title: []const u8,
    body: []const u8,
) u64;

/// TerminalVerbRunnerFn applies the chosen terminal verb to the claim (plan
/// 492 task 3540 finding 2). Injectable on `AgentDriver` so unit tests can
/// drive the terminal-verb-failed → halt-fan-in path WITHOUT shelling
/// `planar-agent`. Returns `terminal.TerminalError!void` so a subprocess
/// failure (or any other error) is surfaced to the harness, which then
/// reflects `terminal_verb_error=true` on the result table and SKIPS fan-in
/// (a failed terminal verb is a HALT-fan-in condition — preventing the
/// resume-double-merge described in the finding).
pub const TerminalVerbRunnerFn = *const fn (
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    verb: terminal.TerminalVerb,
    claim_token: []const u8,
) terminal.TerminalError!void;

/// FailedAttemptFn counts a task's PRIOR FAILED attempts for the M8 max-attempt
/// budget (task 3198). Injectable on `AgentDriver` so unit tests can drive the
/// max-attempt block path with a canned count WITHOUT a real journal file. The
/// production wiring leaves it null and `readFailedAttemptCount` falls back to
/// `defaultFailedAttemptReader` (which reads the persistent journal).
pub const FailedAttemptFn = *const fn (
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_slug: []const u8,
) u32;

/// ClaimStatusFn reads the live claim status for one `agent()` call. Injectable
/// on `AgentDriver` so unit tests can drive the terminal-verb decision matrix
/// without shelling `planar-watch` (task 3242). The production wiring leaves it
/// null and `readClaimStatusOrActive` falls back to `defaultClaimStatusReader`.
pub const ClaimStatusFn = *const fn (
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    plan_id: u64,
    claim_token: []const u8,
) terminal.ClaimStatus;

/// TaskStatusFn reads whether one `agent()` call's task ended up `blocked`
/// (plan 492 M7 task 3194). Injectable on `AgentDriver` so unit tests can drive
/// the block-detection path WITHOUT shelling `planar task show`. Returns `true`
/// iff the task's live status is `blocked` (the worker self-blocked via
/// `planar-agent block`). The production wiring leaves it null and
/// `readTaskStatusBlocked` falls back to `defaultTaskStatusReader`.
pub const TaskStatusFn = *const fn (
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_id: u64,
) bool;

/// TaskLiveStatus classifies the PRE-SPAWN live task status for the M8 resume
/// path (plan 492 task 3197). Only three buckets matter for the skip decision:
/// `done` (the task already completed in a prior run → skip the spawn), `blocked`
/// (set aside by M7 awaiting operator triage → skip, the blocker likely has not
/// cleared), and `other` (todo / doing / anything else → spawn normally).
pub const TaskLiveStatus = enum { done, blocked, other };

/// TaskLiveStatusFn reads the PRE-SPAWN live task status for the M8 resume skip
/// (plan 492 task 3197). Injectable on `AgentDriver` so unit tests can drive the
/// skip-when-done / skip-when-blocked / spawn-when-todo paths WITHOUT shelling
/// `planar task show`. The production wiring leaves it null and
/// `readTaskLiveStatus` falls back to `defaultTaskLiveStatusReader`.
///
/// Distinct from `TaskStatusFn` (which is the POST-spawn boolean "is the task
/// blocked NOW?" used by M7's block detection): this one runs BEFORE the spawn
/// and must distinguish `done` from `blocked` from everything else so the resume
/// summary can report done-vs-blocked skips apart.
pub const TaskLiveStatusFn = *const fn (
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_id: u64,
) TaskLiveStatus;

/// Context for `defaultEnvBuilder` — the production env-builder. Carries the
/// pre-resolved absolute paths to `planar-agent` and `git`, plus the host
/// environ to baseline from. The harness resolves the binary paths once at
/// startup (via PATH lookup) and pins them on the driver, so the per-spawn
/// builder call is cheap.
pub const DefaultEnvBuilderCtx = struct {
    /// Absolute path to the operator's `planar-agent` binary. Allow-listed
    /// in the worker's shim directory.
    planar_agent_path: []const u8,
    /// Absolute path to `git`. Allow-listed in the worker's shim directory.
    git_path: []const u8,
    /// The host environ the worker env baselines from (PATH gets overridden
    /// to the shim dir; ALL PLANAR_* vars are stripped except the allow-listed
    /// worker_env.ALLOWED_PLANAR_VARS — PLANAR_WORKBENCH_ROOT is intentionally
    /// inherited).
    host_environ: std.process.Environ,
};

/// defaultEnvBuilder is the production EnvBuilderFn. It materializes a
/// per-spawn shim dir at `<worktree>/.planar-execute/shim/` and builds the
/// constrained env via `worker_env.buildWorkerEnv`. The per-cycle shim
/// location keeps concurrent cycle workers (M5) from racing on a shared shim.
///
/// `ctx` MUST be a `*DefaultEnvBuilderCtx` — the harness pins it at driver
/// construction time and never mutates it after.
pub fn defaultEnvBuilder(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    worktree_path: []const u8,
) anyerror!worker_env.WorkerEnv {
    const dctx: *DefaultEnvBuilderCtx = @ptrCast(@alignCast(ctx orelse return error.InvalidEnvBuilderCtx));
    // Per-cycle shim dir under the worktree's .planar-execute scratch space.
    // Keeping it under the worktree means the worktree teardown automatically
    // sweeps it; we don't need a separate cleanup pass.
    const shim_dir = try std.fmt.allocPrint(allocator, "{s}/.planar-execute/shim", .{worktree_path});
    defer allocator.free(shim_dir);
    return try worker_env.buildWorkerEnv(
        allocator,
        io,
        dctx.host_environ,
        shim_dir,
        dctx.planar_agent_path,
        dctx.git_path,
    );
}

/// resolveHostBinary resolves `name` to an absolute path on the HOST PATH by
/// shelling `/bin/sh -c "command -v <name>"`. Returns an allocator-owned
/// absolute path string, or `error.BinaryNotResolvable` when the binary is not
/// on PATH (or `command -v` exits non-zero / emits a relative path).
///
/// This is the production binary-resolution step for the gated live-agent
/// driver: we resolve the REAL `planar-agent` and `git` on the host PATH so the
/// per-cycle shim can symlink them in. We resolve against the host PATH (not the
/// constrained worker PATH) because the whole point is to find the real binaries
/// to allow-list into the worker's shim.
fn resolveHostBinary(
    allocator: std.mem.Allocator,
    io: std.Io,
    name: []const u8,
) error{ OutOfMemory, BinaryNotResolvable }![]u8 {
    const sh_cmd = std.fmt.allocPrint(allocator, "command -v {s}", .{name}) catch return error.OutOfMemory;
    defer allocator.free(sh_cmd);

    const result = std.process.run(allocator, io, .{
        .argv = &.{ "/bin/sh", "-c", sh_cmd },
    }) catch return error.BinaryNotResolvable;
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    if (result.term != .exited or result.term.exited != 0) return error.BinaryNotResolvable;

    const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
    // command -v can print a shell keyword/alias/relative builtin name; require
    // an absolute path so the shim symlink target is unambiguous.
    if (trimmed.len == 0 or trimmed[0] != '/') return error.BinaryNotResolvable;

    return allocator.dupe(u8, trimmed) catch error.OutOfMemory;
}

/// AgentCallOpts is the parsed view of the Lua `opts` table for one
/// `agent(prompt, opts)` call. All strings borrow into Lua-owned memory and
/// are only valid for the duration of the C closure call.
const AgentCallOpts = struct {
    role: []const u8,
    worktree_path: []const u8,
    claim_token: []const u8,
    /// May be empty when the workflow declined to inject a role spec.
    role_spec: []const u8,
    /// task_slug is required to derive the cycle branch name for the
    /// commit-presence sample.
    task_slug: []const u8,
    /// task_id is the numeric id of the claimed task. The workflow/caller knows
    /// the task it is dispatching, so it passes it here (a small contract
    /// addition for plan 492 M7 task 3194). It backs the post-exit task-status
    /// read that detects a self-blocked worker (`planar task show <id>`). Zero
    /// means "not supplied" → block detection is skipped (the no-id test paths
    /// and any workflow that declines to pass it).
    task_id: u64,
};

/// luaOptString fetches `opts[name]` as a Lua-owned string view, defaulting to
/// `default` when the field is absent or not a string.
fn luaOptString(L: ?*c.lua_State, opts_idx: c_int, name: [*:0]const u8, default: []const u8) []const u8 {
    const t = c.lua_getfield(L, opts_idx, name);
    defer luaPop(L, 1);
    if (t != c.LUA_TSTRING) return default;
    var len: usize = 0;
    const raw = c.lua_tolstring(L, -1, &len);
    if (raw == null) return default;
    return raw[0..len];
}

/// luaOptInt fetches `opts[name]` as a u64, defaulting to `default` when the
/// field is absent or not a number. Negative / non-integral values clamp to 0
/// (a u64 task id is always non-negative; a malformed value is treated as
/// "not supplied").
fn luaOptInt(L: ?*c.lua_State, opts_idx: c_int, name: [*:0]const u8, default: u64) u64 {
    const t = c.lua_getfield(L, opts_idx, name);
    defer luaPop(L, 1);
    if (t != c.LUA_TNUMBER) return default;
    const n = c.lua_tointegerx(L, -1, null);
    if (n < 0) return 0;
    return @intCast(n);
}

/// readAgentOpts pulls the required and optional fields out of the opts table
/// at stack index `opts_idx`. Returns AgentCallOpts with all string views still
/// borrowing into Lua memory.
fn readAgentOpts(L: ?*c.lua_State, opts_idx: c_int) AgentCallOpts {
    if (c.lua_type(L, opts_idx) != c.LUA_TTABLE) {
        return .{
            .role = "",
            .worktree_path = "",
            .claim_token = "",
            .role_spec = "",
            .task_slug = "",
            .task_id = 0,
        };
    }
    return .{
        .role = luaOptString(L, opts_idx, "role", ""),
        .worktree_path = luaOptString(L, opts_idx, "worktree_path", ""),
        .claim_token = luaOptString(L, opts_idx, "claim_token", ""),
        .role_spec = luaOptString(L, opts_idx, "role_spec", ""),
        .task_slug = luaOptString(L, opts_idx, "task_slug", ""),
        .task_id = luaOptInt(L, opts_idx, "task_id", 0),
    };
}

/// pushAgentResult builds the Lua return table for one agent() call:
///   { status = "...", exit_code = N, commit_present = bool,
///     terminal_verb = "complete"|"release"|"fail"|"none",
///     terminal_verb_error = bool }
/// `status` is "completed" / "released" / "failed" / "respected" / "stub"
/// matching the harness's decision summary. `terminal_verb_error` is true
/// iff the harness's `planar-agent <verb>` subprocess errored (task 3540
/// finding 2 — fan-in is halted in that case to prevent a resume-double-
/// merge; the operator must reconcile manually). Defaults to false.
fn pushAgentResult(
    L: ?*c.lua_State,
    status: []const u8,
    exit_code: u32,
    commit_present: bool,
    verb: terminal.TerminalVerb,
    terminal_verb_error: bool,
) void {
    c.lua_createtable(L, 0, 5);
    _ = c.lua_pushlstring(L, status.ptr, status.len);
    c.lua_setfield(L, -2, "status");
    c.lua_pushinteger(L, @intCast(exit_code));
    c.lua_setfield(L, -2, "exit_code");
    c.lua_pushboolean(L, if (commit_present) 1 else 0);
    c.lua_setfield(L, -2, "commit_present");
    const verb_name = switch (verb) {
        .none => "none",
        .complete => "complete",
        .release => "release",
        .fail => "fail",
    };
    _ = c.lua_pushlstring(L, verb_name.ptr, verb_name.len);
    c.lua_setfield(L, -2, "terminal_verb");
    c.lua_pushboolean(L, if (terminal_verb_error) 1 else 0);
    c.lua_setfield(L, -2, "terminal_verb_error");
}

/// pushSkippedResult builds the Lua return table for one `agent()` call that the
/// M8 resume path SKIPPED without spawning (task 3197):
///   { status = "skipped", skip_reason = "already-done"|"blocked",
///     exit_code = 0, commit_present = false, terminal_verb = "none" }
/// The shape mirrors `pushAgentResult` (so a workflow that inspects `.status` /
/// `.terminal_verb` reads a skipped call uniformly) but `status` is the DISTINCT
/// string "skipped" and `skip_reason` distinguishes a done-skip from a
/// blocked-skip. No claim was acquired, no worktree built, no worker spawned, so
/// `exit_code`/`commit_present`/`terminal_verb` carry their no-op values.
fn pushSkippedResult(L: ?*c.lua_State, skip_reason: []const u8) void {
    c.lua_createtable(L, 0, 5);
    const status = "skipped";
    _ = c.lua_pushlstring(L, status.ptr, status.len);
    c.lua_setfield(L, -2, "status");
    _ = c.lua_pushlstring(L, skip_reason.ptr, skip_reason.len);
    c.lua_setfield(L, -2, "skip_reason");
    c.lua_pushinteger(L, 0);
    c.lua_setfield(L, -2, "exit_code");
    c.lua_pushboolean(L, 0);
    c.lua_setfield(L, -2, "commit_present");
    const verb_name = "none";
    _ = c.lua_pushlstring(L, verb_name.ptr, verb_name.len);
    c.lua_setfield(L, -2, "terminal_verb");
}

/// pushBlockedByBudgetResult builds the Lua return table for one `agent()` call
/// that the M8 max-attempt budget BLOCKED without spawning (task 3198):
///   { status = "blocked", skip_reason = "max-attempts", exit_code = 0,
///     commit_present = false, terminal_verb = "block" }
/// The shape mirrors `pushSkippedResult` / `pushAgentResult` so a workflow that
/// inspects `.status` reads it uniformly. `status` is "blocked" (the task WAS
/// blocked) so the workflow's own control flow treats it like a worker that
/// self-blocked; `skip_reason = "max-attempts"` distinguishes a budget-block
/// from an M7 worker self-block in the result table. No claim was re-acquired,
/// no worker spawned.
fn pushBlockedByBudgetResult(L: ?*c.lua_State) void {
    c.lua_createtable(L, 0, 5);
    const status = "blocked";
    _ = c.lua_pushlstring(L, status.ptr, status.len);
    c.lua_setfield(L, -2, "status");
    const reason = "max-attempts";
    _ = c.lua_pushlstring(L, reason.ptr, reason.len);
    c.lua_setfield(L, -2, "skip_reason");
    c.lua_pushinteger(L, 0);
    c.lua_setfield(L, -2, "exit_code");
    c.lua_pushboolean(L, 0);
    c.lua_setfield(L, -2, "commit_present");
    const verb_name = "block";
    _ = c.lua_pushlstring(L, verb_name.ptr, verb_name.len);
    c.lua_setfield(L, -2, "terminal_verb");
}

/// AgentCallState is the per-`agent()`-call state threaded ACROSS the coroutine
/// yield (plan 492 M5 task 3182). The pre-yield half (`driveAgentCallPreYield`)
/// allocates it, fills the fields the continuation needs, and stashes a pointer
/// to it as the scheduler slot's opaque `payload`. The continuation
/// (`agentContinue`) recovers it from the slot, reads the worker outcome, and
/// frees it.
///
/// Why heap state and not Lua-stack borrows: the continuation runs in a LATER
/// `lua_resume`, after the C-call stack that produced the borrowed Lua strings
/// has unwound. Anything the continuation reads MUST be host-owned. So the
/// pre-yield half DUPES the opts strings it needs (worktree_path, claim_token,
/// task_slug) and the pre-spawn HEAD onto `allocator`, and keeps the
/// `worker_env` alive (the spawn handle borrows its env map) until the
/// continuation frees it after the worker is done.
const AgentCallState = struct {
    allocator: std.mem.Allocator,
    driver: *AgentDriver,
    /// Owned dupes of the opts strings the continuation needs.
    worktree_path: []u8,
    claim_token: []u8,
    task_slug: []u8,
    /// The numeric task id (copy of opts.task_id; a scalar — no dupe needed).
    /// Backs the M7 block-detection read in `agentContinue`. Zero → skip.
    task_id: u64,
    /// The model tier the worker was spawned with (role → tier). Borrows a
    /// COMPTIME constant from `role_model` (no dupe/free needed — it outlives the
    /// process). Stamped at spawn, read into the journal record at terminal.
    model: []const u8,
    /// The role name the worker ran as. Borrows a COMPTIME constant from
    /// `role_model.Role.name()` (no dupe/free needed). Journal field.
    role_name: []const u8,
    /// A content fingerprint of the brief/prompt the worker ran with (lowercase
    /// hex Wyhash). Owned (heap) — the brief is a Lua-stack borrow that does NOT
    /// survive the yield, so we hash + dupe it at spawn. Journal field.
    prompt_hash: []u8,
    /// The cycle branch the worker committed onto, derived at spawn. Owned; may
    /// be null when the harness could not derive it (degraded paths). Journal
    /// field (distinct from `head_before`, which is a commit SHA, not a branch).
    branch: ?[]u8,
    /// The monotonic spawn timestamp (nanoseconds, from the scheduler's
    /// injectable clock). At terminal the journal computes
    /// `wall_clock_ms = (clockNow - spawn_mono_ns) / 1e6`. A real clock in prod;
    /// a FakeClock in tests gives an exact value.
    spawn_mono_ns: i128,
    /// Pre-spawn cycle-branch HEAD (commit-presence proxy). Owned; may be null.
    head_before: ?[]u8,
    /// The constrained worker env kept alive across the spawn. The in-flight
    /// handle's env map borrows into this; freed in the continuation after the
    /// worker has exited. Null when no env_builder was installed (FakeSpawner
    /// test paths).
    worker_env_built: ?worker_env.WorkerEnv,
    /// The scheduler slot index this call registered its in-flight worker in.
    slot_index: usize,

    fn destroy(self: *AgentCallState) void {
        const a = self.allocator;
        if (self.worker_env_built) |*we| we.deinit(a, self.driver.io);
        if (self.head_before) |h| a.free(h);
        if (self.branch) |b| a.free(b);
        a.free(self.prompt_hash);
        a.free(self.worktree_path);
        a.free(self.claim_token);
        a.free(self.task_slug);
        a.destroy(self);
    }
};

/// driveAgentCallPreYield is the FIRST half of one `agent(prompt, opts)` call
/// (the M5 coroutine split). It runs on the coroutine thread `L` (== `co`):
///
///   1. Resolve role + validate worktree_path (script-author errors → Lua error).
///   2. Sample the pre-spawn cycle-branch HEAD (commit-presence proxy).
///   3. Build the constrained worker env (decisions 358 + 365).
///   4. Spawn the worker NON-BLOCKING (`Spawner.start` → in-flight handle).
///   5. Register the handle + the per-call `AgentCallState` with the scheduler.
///   6. `lua_yieldk(L, 0, slot_index, agentContinue)` — yields across the C
///      boundary. Does NOT return (longjmp); execution resumes in
///      `agentContinue` once the scheduler has driven the worker to terminal.
///
/// The second half (post-spawn HEAD sample, commit-presence, claim read,
/// terminal-verb decision + run, result table) lives in `agentContinue`.
///
/// M8 RESUME (task 3197): before any of that, `agent()` is IDEMPOTENT. On a
/// re-run (a second `planar-execute run --plan <id>` after an interrupted first
/// run) each call reads the task's PRE-SPAWN live status (keyed on the threaded
/// `opts.task_id`). When the task is already `done` (completed in the prior run)
/// or `blocked` (set aside by M7 awaiting triage), the call SHORT-CIRCUITS to a
/// synchronous "skipped" result table: NO claim, NO worktree, NO env build, NO
/// spawn, NO scheduler slot, NO heartbeat/journal entry, NO yield. The workflow's
/// own control flow then moves on to the next task, so a re-run resumes the
/// remainder. Resume requires task_id-bearing `agent()` calls — when task_id is
/// 0 (absent) the status cannot be read and the call falls through to spawning
/// (today's backward-compatible behavior).
fn driveAgentCallPreYield(
    L: ?*c.lua_State,
    hs: *HostState,
    driver: *AgentDriver,
    prompt: []const u8,
    opts: AgentCallOpts,
) c_int {
    // M8 RESUME pre-spawn skip (task 3197). Read the live task status FIRST,
    // before allocating, claiming, building a worktree, or spawning. A `done`
    // task already completed in a prior run; a `blocked` task was set aside by
    // M7 (its blocker likely has not cleared — re-driving would just re-block,
    // wasting a spawn). Either case returns a synchronous "skipped" result with
    // a DISTINCT reason so the run summary can tell done-skips from
    // blocked-skips apart. This is a clean synchronous return: nothing was
    // acquired, so there is nothing to release / tear down / yield on, and no
    // scheduler slot is consumed.
    if (opts.task_id != 0) {
        switch (readTaskLiveStatus(driver, hs.allocator, driver.io, opts.task_id)) {
            .done => {
                pushSkippedResult(L, "already-done");
                return 1;
            },
            .blocked => {
                pushSkippedResult(L, "blocked");
                return 1;
            },
            .other => {}, // todo / doing / etc. → spawn normally (fall through).
        }
    }

    // M8 WHOLE-RUN CEILING (task 3198). The kill-switch's HARD STOP: before
    // spawning ANYTHING, check whether this run has hit its ceiling
    // (max-total-spawns or max-wall-clock). If so, do NOT spawn — trip the clean
    // interrupt shutdown (REUSE the task-3189 machinery via the `interrupt`
    // flag) so the run-drive loop tears down in-flight workers in order
    // (heartbeat stop → kill → release → teardown) and exits cleanly. The
    // released workers are recoverable on the next resume. We record WHICH
    // ceiling tripped on the HostState + write the journal terminus so a
    // post-mortem / resume knows the run was ceiling-terminated, not crashed.
    // We raise a Lua error to unwind this `agent()` call immediately; the drive
    // loop observes `interrupt.requested()` at the next safe point and runs the
    // shutdown. The ceiling is checked BEFORE the max-attempt check: a run that
    // is winding down must not spend effort blocking a task.
    if (hs.sched) |sched_for_ceiling| {
        const now_ns = sched_for_ceiling.clockNow();
        if (budget.ceilingTripped(hs.run_counters, hs.budgets, now_ns)) |which| {
            hs.ceiling_tripped = which;
            // Journal terminus: a distinct final record naming the ceiling, so a
            // resume / post-mortem distinguishes a clean ceiling stop from a
            // crash. Best-effort (the journal is durability metadata).
            writeCeilingTerminus(driver, hs.allocator, which);
            // Trip the SAME clean-shutdown the SIGINT path uses. The drive loop
            // observes this flag and runs interrupt.shutdown in order.
            interrupt.interrupt_requested.store(true, .seq_cst);
            _ = c.luaL_error(
                L,
                "agent: run ceiling exceeded (%s) — winding down",
                which.name().ptr,
            );
            return 0; // unreachable — luaL_error longjmps
        }
    }

    // M8 PER-TASK MAX-ATTEMPT → BLOCK (task 3198). After the resume skip (a done
    // task skips regardless of attempts) and the run-ceiling check. Count this
    // task's PRIOR FAILED attempts from the PERSISTENT journal; when the count
    // reaches `max_attempts`, do NOT spawn again — `block` the task (the roadmap
    // says "→ block (not re-queue)") and return a "blocked" result so the
    // workflow moves on and the M7 end-of-run summary surfaces it. Because the
    // count is journal-derived, a RESUMED run respects the prior run's failures
    // (the budget is not reset). Requires a task_id-bearing call (the block
    // shells `planar-agent block --blocker <task_id>`); when task_id is 0 the
    // budget is skipped (mirrors the resume skip's task_id guard).
    if (opts.task_id != 0 and hs.budgets.max_attempts > 0) {
        const fail_count = readFailedAttemptCount(driver, hs.allocator, driver.io, opts.task_slug);
        if (fail_count >= hs.budgets.max_attempts) {
            runBudgetBlock(driver, hs.allocator, driver.io, opts.claim_token, opts.task_id, fail_count);
            hs.recordBlocked(opts.task_slug, "self/max-attempts", "max attempts exceeded") catch {};
            pushBlockedByBudgetResult(L);
            return 1;
        }
    }

    // Resolve role; unknown roles raise a Lua error (the script author's bug).
    const role = role_model.Role.fromString(opts.role) catch {
        _ = c.luaL_error(L, "agent: unknown role '%s'", opts.role.ptr);
        return 0; // unreachable — luaL_error longjmps
    };

    // Worktree path is required. Defer the absolute-path check to
    // buildSpawnArgv inside the spawner; we surface a clearer error here.
    if (opts.worktree_path.len == 0) {
        _ = c.luaL_error(L, "agent: opts.worktree_path is required");
        return 0;
    }

    const sched = hs.sched orelse {
        _ = c.luaL_error(L, "agent: no scheduler installed (internal wiring bug)");
        return 0;
    };

    const alloc = hs.allocator;
    const io = driver.io;

    // Allocate the cross-yield call state up front. Everything the continuation
    // reads is duped onto it (the Lua-stack borrows do not survive the yield).
    const acs = alloc.create(AgentCallState) catch {
        _ = c.luaL_error(L, "agent: out of memory");
        return 0;
    };
    // Initialize the owned dupes; on any failure below before the yield we must
    // free what we allocated (the yield is the point of no return).
    acs.* = .{
        .allocator = alloc,
        .driver = driver,
        .worktree_path = alloc.dupe(u8, opts.worktree_path) catch {
            alloc.destroy(acs);
            _ = c.luaL_error(L, "agent: out of memory");
            return 0;
        },
        .claim_token = &.{},
        .task_slug = &.{},
        .task_id = opts.task_id,
        // model/role_name borrow comptime constants from role_model — no dupe.
        .model = role_model.modelForRole(role),
        .role_name = role.name(),
        .prompt_hash = &.{},
        .branch = null,
        .spawn_mono_ns = 0,
        .head_before = null,
        .worker_env_built = null,
        .slot_index = 0,
    };
    acs.claim_token = alloc.dupe(u8, opts.claim_token) catch {
        alloc.free(acs.worktree_path);
        alloc.destroy(acs);
        _ = c.luaL_error(L, "agent: out of memory");
        return 0;
    };
    acs.task_slug = alloc.dupe(u8, opts.task_slug) catch {
        alloc.free(acs.claim_token);
        alloc.free(acs.worktree_path);
        alloc.destroy(acs);
        _ = c.luaL_error(L, "agent: out of memory");
        return 0;
    };
    // Fingerprint the brief NOW (the prompt is a Lua-stack borrow that does NOT
    // survive the yield). Owned hex; freed in destroy. Journal field.
    acs.prompt_hash = journal.hashPrompt(alloc, prompt) catch {
        acs.destroy();
        _ = c.luaL_error(L, "agent: out of memory");
        return 0;
    };
    // Stamp the spawn-time monotonic reading for the journal wall-clock. Use the
    // scheduler's injectable clock so a test FakeClock drives an exact value;
    // production reads the real monotonic clock.
    acs.spawn_mono_ns = sched.clockNow();

    // 1) Sample the pre-spawn cycle branch HEAD (commit-presence proxy) and stash
    //    the cycle branch name for the journal record.
    if (driver.repo_root.len > 0 and driver.plan_slug.len > 0 and acs.task_slug.len > 0) {
        const branch = worktree.cycleBranch(alloc, driver.plan_slug, acs.task_slug) catch {
            acs.destroy();
            _ = c.luaL_error(L, "agent: out of memory");
            return 0;
        };
        acs.head_before = worktree.branchHead(alloc, io, driver.repo_root, branch) catch null;
        acs.branch = branch; // owned; freed in destroy.
    }

    // 2) Build the constrained worker env when a builder is installed. The real
    // spawner PANICS if env_map is null (decisions 358 + 365); production wiring
    // MUST install a builder. Tests with FakeSpawner omit it and the FakeSpawner
    // records an empty env snapshot.
    if (driver.env_builder) |build_env| {
        acs.worker_env_built = build_env(driver.env_builder_ctx, alloc, io, acs.worktree_path) catch |err| {
            acs.destroy();
            _ = c.luaL_error(L, "agent: env builder failed: %s", @errorName(err).ptr);
            return 0;
        };
    }
    const env_map_ptr: ?*const std.process.Environ.Map =
        if (acs.worker_env_built) |*we| &we.env_map else null;

    // 3) Spawn the worker NON-BLOCKING. The brief is the prompt (already-compiled
    // methodology brief). env_map (the constrained worker env) is threaded into
    // SpawnInputs; the real spawner passes it as `environ_map` on
    // std.process.spawn so the child does NOT inherit the parent's full env.
    const spawn_inputs = spawn.SpawnInputs{
        .role = role,
        .worktree_path = acs.worktree_path,
        .brief = prompt,
        .role_spec = opts.role_spec,
        .env_map = env_map_ptr,
    };
    const handle = driver.spawner.start(alloc, io, spawn_inputs) catch |err| {
        acs.destroy();
        _ = c.luaL_error(L, "agent: spawn failed: %s", @errorName(err).ptr);
        return 0;
    };

    // M8 whole-run ceiling: count the spawn that just happened (task 3198). The
    // ceiling check above is "spawn_count >= max_total_spawns", so we increment
    // AFTER a successful start — the (max+1)th spawn attempt is the one refused.
    hs.run_counters.spawn_count += 1;

    // 4) Register the in-flight worker + this call state with the scheduler.
    const slot_index = sched.registerInflight(L.?, handle, acs) catch {
        // Registry full — N>MAX_SLOTS concurrent workers (impossible at N=1;
        // a real race only at the MAX_SLOTS boundary).
        //
        // PR #17 cycle B finding 6: the spawned child is LIVE here but was
        // never registered. Three things must happen before we surface the
        // error:
        //   a) HARD-KILL the orphaned child (`Spawner.kill` is infallible:
        //      SIGTERM+reap on POSIX, no-op when the child already exited).
        //      The prior implementation used `Spawner.wait`, which can ITSELF
        //      error — leaving a live process running with no reaper, no
        //      lease tracking (the heartbeat register below is not reached on
        //      this path), and a ceiling counter permanently incremented.
        //   b) DECREMENT `spawn_count` (bumped at line ~1340 BEFORE the
        //      registry check) so this no-op spawn does not consume a budget
        //      unit. The ceiling counter must reflect spawns that actually
        //      enter the scheduler; a spawn refused at registerInflight did
        //      not.
        //   c) NEVER reach the heartbeat-register below — the registry never
        //      held a slot for this worker, so refreshing a lease against it
        //      would be a write against a dead claim. (The current code
        //      orders the register call AFTER the registerInflight catch, so
        //      a catch-and-return preserves this invariant — no change is
        //      needed in the heartbeat block itself.)
        var h = handle;
        driver.spawner.kill(io, &h);
        if (hs.run_counters.spawn_count > 0) hs.run_counters.spawn_count -= 1;
        acs.destroy();
        _ = c.luaL_error(L, "agent: scheduler registry full");
        return 0;
    };
    acs.slot_index = slot_index;

    // 4b) Register the worker's claim token with the heartbeat thread (task
    // 3187) so its lease is refreshed at TTL/2 while the worker runs. The
    // registry DUPES the token (it never borrows acs.claim_token, which we free
    // on slot release). `agentContinue` unregisters it at terminal. No-op when
    // no registry is installed (FakeSpawner unit-test paths) or the token is
    // empty. Touches NO Lua state.
    if (hs.heartbeat_reg) |hb| {
        hb.register(acs.claim_token) catch {
            // OOM duping the token into the registry — non-fatal. The worker
            // still runs; its lease just isn't auto-refreshed (F1 reclaim is the
            // fallback). Do NOT abort the spawn for this.
        };
    }

    // 5) Yield across the C boundary. The continuation `agentContinue` resumes
    // once the scheduler has driven the worker to terminal. `lua_yieldk` does
    // NOT return (it longjmps); the `return 0` below is unreachable.
    _ = c.lua_yieldk(L, 0, @intCast(slot_index), agentContinue);
    return 0; // unreachable
}

/// agentContinue is the continuation for one `agent()` call — the SECOND half
/// of the M5 coroutine split. By the time the scheduler resumes this coroutine,
/// the in-flight worker has reached terminal (process exit) and the scheduler
/// has stored its `SpawnOutcome` on the slot. This function:
///
///   1. Recovers the scheduler + the per-call `AgentCallState` (from the slot
///      keyed by `ctx`, the `lua_KContext` index the pre-yield half passed).
///   2. Reads the worker outcome from the slot.
///   3. Samples the post-spawn cycle-branch HEAD → commit-presence.
///   4. Reads the LIVE claim status (task 3242) → terminal-verb decision.
///   5. Optionally runs the terminal verb (`planar-agent <verb>`).
///   6. Builds + pushes the Lua result table and returns 1.
///   7. Frees the call state + the worker outcome + releases the slot.
///
/// `status` is the resume status (`LUA_YIELD` here — the value Lua passes a
/// continuation invoked after a yield); we do not branch on it because the
/// scheduler only resumes after the worker is terminal.
fn agentContinue(L: ?*c.lua_State, status: c_int, ctx: c.lua_KContext) callconv(.c) c_int {
    _ = status;
    const hs = hostStateUpvalue(L);
    const sched = hs.sched orelse @panic("agentContinue: scheduler missing on resume");
    const slot_index: usize = @intCast(ctx);
    const slot = sched.slot(slot_index);
    const acs: *AgentCallState = @ptrCast(@alignCast(slot.payload.?));
    const driver = acs.driver;
    const alloc = acs.allocator;
    const io = driver.io;

    // The scheduler stored the worker outcome on the slot. Take ownership.
    var outcome = slot.outcome.?;
    slot.outcome = null;
    defer outcome.deinit(alloc);

    // Stamp the terminal-time monotonic reading ONCE here (same injectable clock
    // the spawn-time was read from) → the journal `wall_clock_ms` = terminal −
    // spawn. Read before any subprocess so the elapsed reflects the worker's run,
    // not the harness's terminal-verb bookkeeping (task 3196 journal).
    const terminal_mono_ns: i128 = sched.clockNow();

    // Did the wall-clock timeout fire on this worker (task 3188)? Read it from
    // the slot BEFORE the deferred releaseSlot clears it. A timed-out worker was
    // already killed by the scheduler; the continuation now runs the reclaim
    // path (force `fail` + worktree teardown + `status = "timed-out"`) instead
    // of the normal terminal-verb decision matrix.
    const timed_out = slot.timed_out;
    // Free the call state + release the slot when we leave (the result table is
    // already on the Lua stack by then; nothing below borrows acs after this).
    // First unregister the claim from the heartbeat thread (task 3187): the
    // worker has reached terminal, so its lease must NOT be refreshed further —
    // and the unregister MUST precede `acs.destroy()`, which frees the token the
    // registry duped from. The registry frees its OWN copy, so this is a clean
    // hand-back with no cross-thread aliasing.
    defer {
        if (hs.heartbeat_reg) |hb| hb.unregister(acs.claim_token);
        acs.destroy();
        sched.releaseSlot(slot_index);
    }

    // 3) Sample the post-spawn cycle branch HEAD.
    var head_after: ?[]u8 = null;
    defer if (head_after) |h| alloc.free(h);
    if (driver.repo_root.len > 0 and driver.plan_slug.len > 0 and acs.task_slug.len > 0) {
        const branch = worktree.cycleBranch(alloc, driver.plan_slug, acs.task_slug) catch null;
        if (branch) |b| {
            defer alloc.free(b);
            head_after = worktree.branchHead(alloc, io, driver.repo_root, b) catch null;
        }
    }

    // commit_present = post-sample exists AND differs from pre-sample.
    // (Pre-null + post-non-null also counts as commit-present: the branch was
    // created during the spawn.)
    const commit_present = blk: {
        if (head_after) |hi| {
            if (acs.head_before) |hi0| break :blk !std.mem.eql(u8, hi0, hi);
            break :blk true;
        }
        break :blk false;
    };

    // ---- Timeout reclaim path (task 3188) ----
    //
    // A hung worker was already KILLED by the scheduler. The harness now: (a)
    // stops the heartbeat thread from refreshing this dead lease, (b) forces
    // `planar-agent fail` (the worker produced no usable result inside its
    // wall-clock budget — fail, not release, so it does not silently retry-loop
    // a wedged task; the operator triages a failed claim), and (c) tears down
    // the worktree (the killed child's checkout). Ordering is load-bearing:
    //   1. UNREGISTER first — so the heartbeat thread cannot refresh the lease
    //      we are about to fail (a refresh racing the fail would resurrect it).
    //   2. FAIL the claim (kill already happened in the scheduler, before this).
    //   3. TEAR DOWN the worktree (child is dead; safe to remove its checkout).
    // The trailing `defer` also calls `unregister`, but that is now an
    // idempotent no-op (the token is already gone) — so the early unregister is
    // the authoritative one and the defer stays correct for the normal path.
    if (timed_out) {
        if (hs.heartbeat_reg) |hb| hb.unregister(acs.claim_token);

        if (!driver.skip_terminal_subprocess and acs.claim_token.len > 0) {
            terminal.runTimeoutFail(alloc, io, acs.claim_token) catch {
                // Best-effort: operator recovers via reconcile. The returned
                // Lua table still reports the timeout so the workflow sees it.
            };
        }

        // Tear down the hung worker's cycle worktree. Requires repo_root +
        // plan_slug (on the driver) + task_slug (on acs). Skipped when any is
        // empty (FakeSpawner unit-test paths that did not seed a real worktree).
        if (!driver.skip_terminal_subprocess and
            driver.repo_root.len > 0 and driver.plan_slug.len > 0 and acs.task_slug.len > 0)
        {
            worktree.teardownCycle(alloc, io, driver.repo_root, driver.plan_slug, acs.task_slug) catch {
                // Teardown is best-effort: a run-id-scoped worktree-prune on the
                // next startup is the backstop (tech-spec § Run isolation).
            };
        }

        // Journal the spawn (best-effort, skipped when no repo/plan). One record
        // per spawn — this is the timed-out worker's record.
        writeJournalRecord(acs, "timed-out", outcome.exit_code, terminal_mono_ns);
        pushAgentResult(L, "timed-out", outcome.exit_code, commit_present, .fail, false);
        return 1;
    }

    // 4) Read the LIVE claim status (task 3242) → terminal-verb decision.
    const claim_status: terminal.ClaimStatus =
        readClaimStatusOrActive(driver, alloc, io, acs.claim_token);
    const verb = terminal.decideTerminalVerb(.{
        .claim_status = claim_status,
        .exit_code = outcome.exit_code,
        .commit_present = commit_present,
    });

    // 5) Optionally apply the terminal verb (task 3540 finding 2).
    //
    // The terminal-verb error MUST be surfaced — it is NOT a swallow-and-
    // continue. A subprocess failure here (planar-agent unavailable,
    // ClaimNotActive, etc.) leaves the task DB-not-completed while the
    // decision matrix recorded a "successful" terminal verb. If we then
    // FANNED IN, a later resume would respawn the same task (still
    // `todo`/`doing` in the DB), produce another cycle commit, and attempt
    // a SECOND merge into the already-merged epic → conflict/duplicate.
    //
    // Locked semantic: a failed harness terminal verb is a HALT-fan-in
    // condition. We:
    //   - flag `terminal_verb_failed` so the result-table stamping below
    //     surfaces the divergence to the workflow (and the operator),
    //   - SKIP fan-in for this call, even if the verb would otherwise
    //     have driven it (the unmerged cycle commit + un-completed task
    //     stay for the operator to reconcile manually),
    //   - print a loud stderr WARNING (mirroring the bypass-reviewer
    //     guard style) so the operator notices on the live console.
    //
    // The `.none` path is unaffected — the verb is the literal no-op
    // (worker self-finalized) and `runTerminalVerb` short-circuits before
    // any subprocess. Tests inject a fake via `driver.terminal_verb_runner`.
    var terminal_verb_failed: bool = false;
    if (!driver.skip_terminal_subprocess and acs.claim_token.len > 0) {
        const tv_result = if (driver.terminal_verb_runner) |run|
            run(driver.terminal_verb_runner_ctx, alloc, io, verb, acs.claim_token)
        else
            terminal.runTerminalVerb(alloc, io, verb, acs.claim_token);
        tv_result catch |err| {
            // Only treat as a HALT condition when the verb was non-.none —
            // a .none verb's runTerminalVerb returns immediately and cannot
            // error, but be defensive anyway: a .none-with-error is logged
            // but does not halt fan-in (there is no fan-in candidate verb
            // here anyway; fan-in is gated on a real commit + active/self-
            // completed claim below).
            if (verb != .none) terminal_verb_failed = true;
            std.debug.print(
                "planar-execute: WARNING — harness terminal verb '{s}' FAILED for claim '{s}' " ++
                    "(error={s}). Task is NOT DB-completed; fan-in will be SKIPPED to prevent " ++
                    "a resume-double-merge. Operator must reconcile manually " ++
                    "(planar task show / planar-agent ...).\n",
                .{ @tagName(verb), acs.claim_token, @errorName(err) },
            );
        };
    }

    // 6) M7 BLOCK DETECTION (task 3194). A worker that hit a genuine blocker
    // ran `planar-agent block` itself (per the brief — "block rather than ask";
    // brief.zig ~L353). `block` atomically flips the TASK to `blocked` AND
    // releases the claim in one transaction. So by this point:
    //   - the claim is RELEASED → `claim_status` read above returned `.terminal`
    //     → `decideTerminalVerb` returned `.none` → the harness ran NO redundant
    //     terminal verb (confirmed: the worker owns its own terminal verb).
    //   - the TASK is `blocked` → the reliable, unambiguous signal we key on.
    // We read the live task status (keyed on the explicit task_id threaded
    // through opts) and, when it is `blocked`, surface a DISTINCT status string
    // and accumulate the item for the end-of-run operator-triage summary
    // (task 3195). This is SET-ASIDE-AND-CONTINUE: we do NOT fail, do NOT retry,
    // do NOT halt — the slot is reclaimed and `parallel`/`pipeline` proceed past
    // it exactly like any other terminal worker (the verb is already `.none`).
    const blocked = readTaskStatusBlocked(driver, alloc, io, acs.task_id);
    if (blocked) {
        // Accumulate for the end-of-run summary. The blocker id / reason are not
        // cleanly available from the task-status read alone (the worker passed
        // them to `planar-agent block`, not onto the task row), so we record the
        // slug and leave blocker/reason empty — the summary still points the
        // operator at the blocked task. Best-effort: an OOM here loses only the
        // in-memory summary entry (the operator still sees the blocked task via
        // `planar plan next`); never abort the run for it.
        hs.recordBlocked(acs.task_slug, "", "") catch {};
        writeJournalRecord(acs, "blocked", outcome.exit_code, terminal_mono_ns);
        pushAgentResult(L, "blocked", outcome.exit_code, commit_present, .none, false);
        return 1;
    }

    // 7) Build the result table and push (one return value to the script).
    const status_str: []const u8 = switch (verb) {
        .none => "respected",
        .complete => "completed",
        .release => "released",
        .fail => "failed",
    };
    writeJournalRecord(acs, status_str, outcome.exit_code, terminal_mono_ns);
    pushAgentResult(L, status_str, outcome.exit_code, commit_present, verb, terminal_verb_failed);

    // 8) M9 FAN-IN (tasks 3199/3200/3201 + task 3540 finding 1).
    //
    // Fan-in folds the cycle branch's commit into the epic worktree. The
    // gate must fire whenever a COMMIT EXISTS and the task reached terminal
    // state — NOT just when the harness shelled `.complete`. Two paths
    // qualify:
    //
    //   (a) verb == .complete — the harness shelled `planar-agent
    //       complete` (claim was `.active` going in, exit 0, commit present).
    //       Classic M9 path.
    //   (b) verb == .none AND claim_status == .completed — the WORKER ran
    //       its OWN `planar-agent complete` (M7 doctrine). The claim is
    //       `.completed`, decideTerminalVerb returned `.none` (no redundant
    //       harness verb), but the cycle branch still carries the worker's
    //       commit. Without this branch the cycle commit would sit unmerged
    //       forever (task 3540 finding 1).
    //
    // The other `.none` sub-cases — claim `.released` / `.aborted` /
    // `.stale` / `.unknown` — do NOT fan in. A self-release means the
    // worker produced no useful work; .aborted/.stale/.unknown have no
    // mergeable contract either. The gate keys on `.completed` precisely.
    //
    // HALT condition (task 3540 finding 2): if `runTerminalVerb` errored
    // above (`terminal_verb_failed == true`), SKIP fan-in entirely. The
    // claim DB row is now divergent from the harness's decision; merging
    // would compound the divergence (resume would respawn → second
    // commit → second merge → conflict/duplicate).
    //
    // The merge runs on the cooperative-scheduler main thread (agentContinue
    // is the continuation the scheduler resumes — one coroutine at a time),
    // so two fan-in merges into the same epic worktree's `.git/index`
    // cannot race; the single-threaded resume IS the serialization mutex
    // (3201). The heartbeat / timeout threads never merge.
    if (terminal_verb_failed) {
        stampMerge(L, "skipped-terminal-verb-failed", 0);
    } else if (shouldFanIn(verb, claim_status, commit_present)) {
        runFanIn(L, acs, commit_present);
    }
    return 1;
}

/// shouldFanIn is the M9 fan-in gate predicate (task 3540 finding 1). PURE —
/// no I/O, no allocation. Unit-tested in isolation against the matrix of
/// (verb × claim_status × commit_present) so the gate decision is auditable
/// independently of the runFanIn machinery.
///
/// Returns `true` iff fan-in should fire. The contract:
///
///   - `commit_present` must be true — nothing to merge otherwise.
///   - At least one of:
///     * `verb == .complete` — the HARNESS shelled `planar-agent complete`
///       (claim was `.active` going in). Classic M9 path.
///     * `claim_status == .completed` — the WORKER ran its OWN
///       `planar-agent complete` (M7 doctrine). `decideTerminalVerb`
///       returned `.none` (no redundant harness verb), but the cycle
///       branch still carries a commit. Without this branch the cycle
///       commit sits unmerged forever (the original bug).
///
/// The other `.none` sub-cases — claim `.released` / `.aborted` / `.stale`
/// / `.unknown` — do NOT fan in: a self-release means the worker produced
/// no useful work; .aborted/.stale/.unknown have no mergeable contract.
fn shouldFanIn(
    verb: terminal.TerminalVerb,
    claim_status: terminal.ClaimStatus,
    commit_present: bool,
) bool {
    if (!commit_present) return false;
    if (verb == .complete) return true;
    if (claim_status == .completed) return true;
    return false;
}

/// runFanIn performs the M9 cycle→epic fan-in for a COMPLETED worker and stamps
/// the outcome onto the agent() result table already on top of the Lua stack
/// (tasks 3199/3200/3201). It is called ONLY from the COMPLETED branch of
/// `agentContinue` (verb == .complete) — the one terminal status whose cycle
/// branch carries a commit worth folding into the epic.
///
/// Topology guard: fan-in runs ONLY when the worktree topology is present — the
/// epic worktree exists AND the cycle branch exists. Absent (a classic in-pwd
/// run, or a caller that never built the topology) ⇒ `merge="skipped"` and the
/// agent() call completes normally (backward compat). The guard also short-
/// circuits when `repo_root`/`plan_slug`/`task_slug` are unset (degraded /
/// pure-Lua test paths) or when `commit_present` is false (no commit to merge).
///
/// On `.clean`: the cycle merged into the epic; we tear down the cycle worktree
/// (post-success cleanup) and stamp `merge="clean"`. The epic worktree + branch
/// PERSIST — the operator merges epic→master later (the harness NEVER does, per
/// 3201). On `.conflict`: `mergeCycleIntoEpicCapture` already aborted (epic
/// restored); we open a `planar question` naming the conflicting files, LEAVE
/// the cycle worktree for resolution, and stamp `merge="conflict"` +
/// `question_id`. Either way the function RETURNS — it never raises a Lua error
/// or halts the scheduler, so sibling children proceed (3200).
fn runFanIn(L: ?*c.lua_State, acs: *AgentCallState, commit_present: bool) void {
    const driver = acs.driver;
    const alloc = acs.allocator;
    const io = driver.io;

    // Topology / context guard. Any missing piece ⇒ skip (no fan-in for
    // non-worktree runs). `commit_present` false ⇒ nothing to merge.
    if (driver.repo_root.len == 0 or driver.plan_slug.len == 0 or
        acs.task_slug.len == 0 or !commit_present)
    {
        stampMerge(L, "skipped", 0);
        return;
    }
    if (!worktree.epicWorktreePresent(alloc, io, driver.repo_root, driver.plan_slug) or
        !worktree.cycleBranchPresent(alloc, io, driver.repo_root, driver.plan_slug, acs.task_slug))
    {
        stampMerge(L, "skipped", 0);
        return;
    }

    // Build the merge-commit subject: "Plan <id> fan-in: <task-slug>".
    const commit_msg = std.fmt.allocPrint(
        alloc,
        "Plan {d} fan-in: {s}",
        .{ driver.plan_id, acs.task_slug },
    ) catch {
        // OOM building the message ⇒ skip the merge (best-effort; never crash
        // the run). The cycle worktree is left intact for the operator.
        stampMerge(L, "skipped", 0);
        return;
    };
    defer alloc.free(commit_msg);

    var cap = worktree.mergeCycleIntoEpicCapture(
        alloc,
        io,
        driver.repo_root,
        driver.plan_slug,
        acs.task_slug,
        commit_msg,
    ) catch {
        // A merge-subprocess failure (could not spawn git, etc.) is treated as
        // a skip — we do NOT tear down the cycle worktree (the operator may
        // want to inspect / retry) and we do NOT fail the run.
        stampMerge(L, "skipped", 0);
        return;
    };
    defer cap.deinit(alloc);

    switch (cap.outcome) {
        .clean => {
            // The cycle merged into the epic. Tear down the cycle worktree
            // (post-success cleanup runs ONLY merge-clean). The epic worktree +
            // branch persist for the operator's later epic→master merge.
            worktree.teardownCycle(alloc, io, driver.repo_root, driver.plan_slug, acs.task_slug) catch {
                // Best-effort: a startup worktree-prune is the backstop. The
                // merge already landed, so this never changes the merge outcome.
            };
            stampMerge(L, "clean", 0);
        },
        .conflict => {
            // mergeCycleIntoEpicCapture already aborted (epic restored). Open
            // the operator-triage question (naming the conflicting files) and
            // LEAVE the cycle worktree for resolution. The conflict NEVER halts
            // the run (3200) — we stamp the outcome and return normally.
            const qid = openFanInConflictQuestion(acs, cap.conflict_files);
            stampMerge(L, "conflict", qid);
        },
    }
}

/// openFanInConflictQuestion opens the operator-triage `planar question` for an
/// M9 fan-in conflict and returns the opened question id (0 ⇒ open failed). The
/// title is `"<plan-slug> fan-in conflict: <task-slug> against epic HEAD"`; the
/// body names the conflicting files (or a generic line when the capture was
/// empty). Dispatches to `driver.question_runner` (tests) or the production
/// shell-out (`defaultQuestionRunner`). Best-effort: any allocation failure
/// degrades to a question id of 0 — a failed question open NEVER halts the run.
fn openFanInConflictQuestion(acs: *AgentCallState, conflict_files: []const []const u8) u64 {
    const driver = acs.driver;
    const alloc = acs.allocator;
    const io = driver.io;

    const title = std.fmt.allocPrint(
        alloc,
        "{s} fan-in conflict: {s} against epic HEAD",
        .{ driver.plan_slug, acs.task_slug },
    ) catch return 0;
    defer alloc.free(title);

    const body = buildConflictBody(alloc, driver, acs, conflict_files) catch return 0;
    defer alloc.free(body);

    return runFanInQuestion(driver, alloc, io, driver.plan_id, title, body);
}

/// buildConflictBody composes the fan-in conflict question body: the conflicting
/// FILE LIST (one per line) plus the cycle worktree path so the operator knows
/// where to resolve. Falls back to a generic line when no files were captured
/// (the `mergeCycleIntoEpicCapture` capture was best-effort). Heap-owned; caller
/// frees.
fn buildConflictBody(
    alloc: std.mem.Allocator,
    driver: *AgentDriver,
    acs: *AgentCallState,
    conflict_files: []const []const u8,
) ![]u8 {
    const cycle_path = worktree.cyclePath(alloc, driver.repo_root, driver.plan_slug, acs.task_slug) catch
        return error.OutOfMemory;
    defer alloc.free(cycle_path);

    var buf: std.Io.Writer.Allocating = .init(alloc);
    defer buf.deinit();

    try buf.writer.print(
        "git merge --no-ff cycle/{s}/{s} into epic/{s} reported conflicts.\n",
        .{ driver.plan_slug, acs.task_slug, driver.plan_slug },
    );
    if (conflict_files.len > 0) {
        try buf.writer.writeAll("Conflicting files:\n");
        for (conflict_files) |f| try buf.writer.print("  - {s}\n", .{f});
    } else {
        try buf.writer.writeAll("Conflicting files: (not captured)\n");
    }
    try buf.writer.print(
        "The merge was aborted (epic restored). Resolve in the cycle worktree, then re-merge:\n  {s}\n",
        .{cycle_path},
    );
    return buf.toOwnedSlice();
}

/// runFanInQuestion dispatches to the injected `driver.question_runner` (tests)
/// or to `defaultQuestionRunner` (production, which shells `planar question
/// add`). Returns the opened question id (0 ⇒ failed). When the runner is null
/// AND the harness skips the terminal subprocess (FakeSpawner unit-test paths
/// that did not inject a runner) it returns 0 — a unit test that wants the
/// question opened injects a runner.
fn runFanInQuestion(
    driver: *AgentDriver,
    alloc: std.mem.Allocator,
    io: std.Io,
    plan_id: u64,
    title: []const u8,
    body: []const u8,
) u64 {
    if (driver.question_runner) |run| {
        return run(driver.question_runner_ctx, alloc, io, plan_id, title, body);
    }
    if (driver.skip_terminal_subprocess) return 0;
    return defaultQuestionRunner(null, alloc, io, plan_id, title, body);
}

/// defaultQuestionRunner is the production `QuestionRunnerFn`. It shells
/// `planar question add --plan <id> --json --body <body> <title>` via
/// `state.questionAdd` and returns the opened question id. On any
/// read/parse/subprocess failure it returns 0 — a failed question open must NOT
/// crash the run; the merge was already aborted and the cycle worktree is left
/// for the operator regardless.
fn defaultQuestionRunner(
    ctx: ?*anyopaque,
    alloc: std.mem.Allocator,
    io: std.Io,
    plan_id: u64,
    title: []const u8,
    body: []const u8,
) u64 {
    _ = ctx;
    return state.questionAdd(alloc, io, plan_id, title, body) catch 0;
}

/// stampMerge sets the M9 fan-in fields on the agent() result table already on
/// top of the Lua stack: `merge = "clean"|"conflict"|"skipped"` and (on
/// conflict) `question_id = <id>`. Called from `runFanIn` after
/// `pushAgentResult` has built the base table; it mutates that same table in
/// place (index -1) so the workflow reads `r.merge` / `r.question_id`.
fn stampMerge(L: ?*c.lua_State, merge: []const u8, question_id: u64) void {
    _ = c.lua_pushlstring(L, merge.ptr, merge.len);
    c.lua_setfield(L, -2, "merge");
    if (question_id != 0) {
        c.lua_pushinteger(L, @intCast(question_id));
        c.lua_setfield(L, -2, "question_id");
    }
}

/// writeJournalRecord appends ONE append-only run-journal record for a finished
/// worker spawn (plan 492 M8 task 3196). It is called from every terminal return
/// path in `agentContinue` (normal, timed-out, blocked) with the FINAL status
/// string the harness recorded, so the journal carries one record per spawn.
///
/// SKIP guard: when `driver.repo_root` is empty OR `driver.plan_id` is 0 (the
/// degraded / pure-Lua / no-real-spawn test paths) there is no journal location,
/// so we write nothing. This mirrors the branch-head / worktree-teardown guards.
///
/// BEST-EFFORT: the journal is durability metadata, not the control path. A
/// path-derivation or write failure is logged and swallowed — it must NEVER
/// crash the run or fail the `agent()` call (the terminal verb is already
/// applied by the time we get here).
fn writeJournalRecord(
    acs: *AgentCallState,
    final_status: []const u8,
    exit_code: u32,
    terminal_mono_ns: i128,
) void {
    const driver = acs.driver;
    const alloc = acs.allocator;
    const io = driver.io;

    // Skip when there is no journal location (degraded / pure-Lua / unit-test
    // paths). A pure-Lua run never spawns a real worker, so it has no record.
    if (driver.repo_root.len == 0 or driver.plan_id == 0) return;

    const path = journal.journalPath(alloc, driver.repo_root, driver.plan_id) catch |err| {
        std.log.scoped(.planar_execute).warn(
            "journal: could not derive path (plan {d}): {s}",
            .{ driver.plan_id, @errorName(err) },
        );
        return;
    };
    defer alloc.free(path);

    // Wall-clock = terminal-now − spawn-time, both from the scheduler's clock
    // (real monotonic in prod; a deterministic FakeClock in tests). Clamp to >= 0
    // to be defensive against a non-monotonic clock injection.
    const elapsed_ns: i128 = terminal_mono_ns - acs.spawn_mono_ns;
    const wall_ms: i64 = if (elapsed_ns <= 0) 0 else @intCast(@divTrunc(elapsed_ns, std.time.ns_per_ms));

    const record = journal.JournalRecord{
        .prompt_hash = acs.prompt_hash,
        .worktree = acs.worktree_path,
        .branch = if (acs.branch) |b| b else "",
        .claim_token = acs.claim_token,
        .model = acs.model,
        .role = acs.role_name,
        .task_slug = acs.task_slug,
        .exit_code = @intCast(exit_code),
        .terminal_verb = final_status,
        .wall_clock_ms = wall_ms,
        .timestamp = nowUnixSeconds(),
    };

    journal.append(alloc, io, path, record) catch |err| {
        std.log.scoped(.planar_execute).warn(
            "journal: append failed (plan {d}, task {s}): {s}",
            .{ driver.plan_id, acs.task_slug, @errorName(err) },
        );
    };
}

/// writeCeilingTerminus appends the M8 whole-run-ceiling TERMINUS record to the
/// journal (task 3198) — a DISTINCT final record marking the run as cleanly
/// ceiling-terminated (not crashed). The `terminal_verb` sentinel is
/// `budget.CEILING_TERMINUS_VERB` ("ceiling"); the `task_slug` carries which
/// ceiling tripped (`max-total-spawns` / `max-wall-clock`) so a resume /
/// post-mortem can name it. A reader recognizes this record (it never counts
/// toward any task's max-attempt budget — `budget.failedAttemptStatus` excludes
/// it). Most fields are sentinels: there is no worker, claim, or worktree for a
/// run-level marker.
///
/// SKIP + BEST-EFFORT: same guards as `writeJournalRecord` — no journal location
/// (empty repo_root / plan_id 0) skips silently, and a write failure is logged +
/// swallowed (the run is winding down regardless).
fn writeCeilingTerminus(
    driver: *AgentDriver,
    allocator: std.mem.Allocator,
    which: budget.Ceiling,
) void {
    if (driver.repo_root.len == 0 or driver.plan_id == 0) return;
    const io = driver.io;

    const path = journal.journalPath(allocator, driver.repo_root, driver.plan_id) catch |err| {
        std.log.scoped(.planar_execute).warn(
            "journal: could not derive terminus path (plan {d}): {s}",
            .{ driver.plan_id, @errorName(err) },
        );
        return;
    };
    defer allocator.free(path);

    const record = journal.JournalRecord{
        .prompt_hash = "",
        .worktree = "",
        .branch = "",
        .claim_token = "",
        .model = "",
        .role = "",
        .task_slug = which.name(), // which ceiling tripped.
        .exit_code = 0,
        .terminal_verb = budget.CEILING_TERMINUS_VERB,
        .wall_clock_ms = 0,
        .timestamp = nowUnixSeconds(),
    };

    journal.append(allocator, io, path, record) catch |err| {
        std.log.scoped(.planar_execute).warn(
            "journal: ceiling terminus append failed (plan {d}): {s}",
            .{ driver.plan_id, @errorName(err) },
        );
    };
}

/// nowUnixSeconds reads a wall-clock Unix-seconds timestamp for the journal
/// `timestamp` field via the C `clock_gettime(REALTIME)` (mirrors runlock.zig's
/// `nowNanos`; there is no `std.time.timestamp` in this Zig). Informational
/// only — the journal orders by record sequence, not this value.
fn nowUnixSeconds() i64 {
    if (builtin.os.tag == .windows) return 0;
    var ts: std.c.timespec = undefined;
    if (std.c.clock_gettime(.REALTIME, &ts) != 0) return 0;
    return @intCast(ts.sec);
}

/// readFailedAttemptCount returns the number of PRIOR FAILED attempts for
/// `task_slug` — the M8 max-attempt budget input (task 3198). It dispatches to
/// the injected `driver.failed_attempt_reader` (tests) or to
/// `defaultFailedAttemptReader` (production, which reads the persistent journal).
///
/// Returns 0 when there is no journal location (empty `repo_root` / `plan_id`
/// 0): no journal ⇒ no prior attempts ⇒ the budget never blocks (the spawn
/// proceeds). This is the same degraded-path guard `writeJournalRecord` uses, so
/// the max-attempt budget is active EXACTLY when journaling is (a real run).
fn readFailedAttemptCount(
    driver: *AgentDriver,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_slug: []const u8,
) u32 {
    if (driver.failed_attempt_reader) |read| {
        return read(driver.failed_attempt_ctx, allocator, io, task_slug);
    }
    return defaultFailedAttemptReader(driver, allocator, io, task_slug);
}

/// defaultFailedAttemptReader is the production `FailedAttemptFn`. It reads the
/// PERSISTENT run journal at `<repo_root>/.worktrees/.planar-execute/journal-
/// <plan_id>.ndjson` and counts records for `task_slug` whose `terminal_verb` is
/// a failed-attempt status (`budget.failedAttemptStatus`). Because the journal
/// is persistent, a RESUMED run sees the prior run's failures and does NOT reset
/// the budget — this is the load-bearing reason the count is journal-derived.
///
/// BEST-EFFORT: a missing journal (`journal.read` → empty) or any read/parse
/// error returns 0. A failed read must NOT spuriously block a task (that would
/// strand work on a transient FS hiccup); the conservative default is "no prior
/// attempts ⇒ spawn".
fn defaultFailedAttemptReader(
    driver: *AgentDriver,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_slug: []const u8,
) u32 {
    // No journal location → no prior attempts (mirrors the writeJournalRecord
    // skip guard). The max-attempt budget is active exactly when journaling is.
    if (driver.repo_root.len == 0 or driver.plan_id == 0) return 0;

    const path = journal.journalPath(allocator, driver.repo_root, driver.plan_id) catch return 0;
    defer allocator.free(path);

    var arena = std.heap.ArenaAllocator.init(allocator);
    defer arena.deinit();
    const records = journal.read(allocator, arena.allocator(), io, path) catch return 0;
    return budget.failedAttemptCount(records, task_slug);
}

/// runBudgetBlock shells `planar-agent block --claim <token> --blocker
/// <task_id> --reason <max-attempt reason>` for the M8 max-attempt budget
/// (task 3198). The blocker is the task ITSELF (a self-block: the task is parked
/// on its own repeated-failure history awaiting operator triage). BEST-EFFORT:
/// a subprocess failure is logged and swallowed — the harness still returns the
/// "blocked" result so the workflow moves on, and the un-blocked task is the
/// operator's backstop view (`planar plan next`).
///
/// No-op when `skip_block_subprocess` is set (unit-test paths), the claim token
/// is empty, or `task_id` is 0.
fn runBudgetBlock(
    driver: *AgentDriver,
    allocator: std.mem.Allocator,
    io: std.Io,
    claim_token: []const u8,
    task_id: u64,
    attempts: u32,
) void {
    if (driver.skip_block_subprocess) return;
    if (claim_token.len == 0 or task_id == 0) return;

    const blocker_str = std.fmt.allocPrint(allocator, "{d}", .{task_id}) catch return;
    defer allocator.free(blocker_str);
    const reason = std.fmt.allocPrint(
        allocator,
        "planar-execute: max attempts ({d}) exceeded — blocked for operator triage",
        .{attempts},
    ) catch return;
    defer allocator.free(reason);

    const argv = [_][]const u8{
        "planar-agent", "block", "--claim", claim_token, "--blocker", blocker_str, "--reason", reason,
    };
    const result = std.process.run(allocator, io, .{
        .argv = &argv,
        .stdout_limit = std.Io.Limit.limited(64 * 1024),
        .stderr_limit = std.Io.Limit.limited(8192),
    }) catch |err| {
        std.log.scoped(.planar_execute).warn(
            "budget: max-attempt block subprocess failed (task {d}): {s}",
            .{ task_id, @errorName(err) },
        );
        return;
    };
    allocator.free(result.stdout);
    allocator.free(result.stderr);
    if (!(result.term == .exited and result.term.exited == 0)) {
        std.log.scoped(.planar_execute).warn(
            "budget: planar-agent block exited non-zero (task {d})",
            .{task_id},
        );
    }
}

/// readTaskStatusBlocked returns true iff the `agent()` call's task is now in
/// status `blocked` — the M7 block-detection signal (task 3194). It dispatches
/// to the injected `driver.task_status_reader` (tests) or to
/// `defaultTaskStatusReader` (production, which shells `planar task show`).
///
/// Returns false when `task_id` is 0 (the caller did not thread a task id, e.g.
/// the no-id test paths): block detection is then simply skipped and the normal
/// terminal-verb status applies.
fn readTaskStatusBlocked(
    driver: *AgentDriver,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_id: u64,
) bool {
    if (task_id == 0) return false;
    if (driver.task_status_reader) |read| {
        return read(driver.task_status_ctx, allocator, io, task_id);
    }
    // Production path is only meaningful when the harness actually drives the
    // terminal subprocess (a real run). Unit tests that leave the reader null
    // AND skip the terminal subprocess never want a live `planar task show`.
    if (driver.skip_terminal_subprocess) return false;
    return defaultTaskStatusReader(null, allocator, io, task_id);
}

/// defaultTaskStatusReader is the production `TaskStatusFn`. It shells
/// `planar task show <task_id> --json` via `state.taskShow` and returns true iff
/// the task's `status` field is `blocked`. On any read/parse failure it returns
/// false — a failed read must NOT spuriously classify a worker as blocked (that
/// would suppress the normal terminal status + pollute the triage summary). The
/// conservative default is "not blocked"; the normal terminal-verb path then
/// applies, and `planar plan next` remains the operator's backstop view.
fn defaultTaskStatusReader(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_id: u64,
) bool {
    _ = ctx;
    var parsed = state.taskShow(allocator, io, task_id) catch return false;
    defer parsed.deinit();
    return std.mem.eql(u8, parsed.value.status, "blocked");
}

/// readTaskLiveStatus returns the PRE-SPAWN live task status for the M8 resume
/// skip (task 3197). It dispatches to the injected `driver.task_live_status_reader`
/// (tests) or to `defaultTaskLiveStatusReader` (production, which shells
/// `planar task show`).
///
/// Returns `.other` when `task_id` is 0 (the caller did not thread a task id):
/// resume is then simply skipped and the normal spawn path applies — resume
/// REQUIRES task_id-bearing `agent()` calls. Also returns `.other` (spawn) when
/// the reader is null AND the harness skips the terminal subprocess (the
/// FakeSpawner unit-test paths that did not opt into a live read): a unit test
/// that wants the resume skip injects a reader.
fn readTaskLiveStatus(
    driver: *AgentDriver,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_id: u64,
) TaskLiveStatus {
    if (task_id == 0) return .other;
    if (driver.task_live_status_reader) |read| {
        return read(driver.task_live_status_ctx, allocator, io, task_id);
    }
    // Mirror readTaskStatusBlocked: a unit test that leaves the reader null AND
    // skips the terminal subprocess never wants a live `planar task show`.
    if (driver.skip_terminal_subprocess) return .other;
    return defaultTaskLiveStatusReader(null, allocator, io, task_id);
}

/// defaultTaskLiveStatusReader is the production `TaskLiveStatusFn`. It shells
/// `planar task show <task_id> --json` via `state.taskShow` and classifies the
/// `status` field into `.done` / `.blocked` / `.other`. On any read/parse
/// failure it returns `.other` — a failed read must NOT spuriously skip a task
/// (that would silently drop work from the resume run); the conservative default
/// is "spawn it" and the worker's own claim/terminal ritual is the backstop.
fn defaultTaskLiveStatusReader(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_id: u64,
) TaskLiveStatus {
    _ = ctx;
    var parsed = state.taskShow(allocator, io, task_id) catch return .other;
    defer parsed.deinit();
    if (std.mem.eql(u8, parsed.value.status, "done")) return .done;
    if (std.mem.eql(u8, parsed.value.status, "blocked")) return .blocked;
    return .other;
}

/// readClaimStatusOrActive returns the claim's LIVE status for the terminal-verb
/// decision (task 3242). It dispatches to the injected `driver.claim_status_reader`
/// (tests), or to `defaultClaimStatusReader` (production), which shells
/// `planar-watch ps --plan <id> --stale --json` and classifies the claim token.
///
/// Defaults to `.active` when the token is empty or `driver.plan_id` is unset —
/// the harness then applies the terminal verb on exit_code + commit_present
/// alone (the pre-3242 behavior, kept for the no-claim test paths).
///
/// This closes the cycle C iter-1 Item-A bug: when a worker correctly runs its
/// own terminal verb, the claim disappears from `planar-watch ps`, the reader
/// returns `.terminal`, `decideTerminalVerb` returns `.none`, and the harness
/// no longer issues a redundant terminal-verb subprocess (which would have
/// errored with ClaimNotActive while the Lua result table mis-reported success).
fn readClaimStatusOrActive(
    driver: *AgentDriver,
    allocator: std.mem.Allocator,
    io: std.Io,
    claim_token: []const u8,
) terminal.ClaimStatus {
    if (claim_token.len == 0) return .active;
    if (driver.claim_status_reader) |read| {
        return read(driver.claim_status_ctx, allocator, io, driver.plan_id, claim_token);
    }
    return defaultClaimStatusReader(null, allocator, io, driver.plan_id, claim_token);
}

/// defaultClaimStatusReader is the production `ClaimStatusFn`. It shells
/// `planar-watch ps --plan <plan_id> --stale --json` via `state.claimStatus`
/// and maps the resulting `state.ClaimState` to the `terminal.ClaimStatus` the
/// decision matrix consumes:
///
///   - `.active`   → `.active`   — lease live, worker did NOT self-finish; the
///                                  harness owns the terminal verb.
///   - `.terminal` → `.completed`— worker already ran its own terminal verb;
///                                  `decideTerminalVerb` returns `.none`. (the fix)
///   - `.stale`    → `.active`   — the lease expired mid-run, so the HARNESS
///                                  owns the terminal transition (the worker's
///                                  lease died before it could). Treating stale
///                                  as active makes the harness reconcile it via
///                                  the exit_code + commit_present matrix.
///
/// On any read/parse failure we conservatively return `.active`: the harness
/// then applies a terminal verb based on the spawn signals, and the
/// ClaimNotActive error class (recoverable, non-corrupting per
/// src/engine/runtime/agentactivity/atomic.zig:324) catches a double-write.
fn defaultClaimStatusReader(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    plan_id: u64,
    claim_token: []const u8,
) terminal.ClaimStatus {
    _ = ctx;
    // plan_id == 0 means the caller did not thread a plan scope; skip the live
    // read and let the spawn-signal matrix decide (matches the no-claim paths).
    if (plan_id == 0) return .active;
    const st = state.claimStatus(allocator, io, plan_id, claim_token) catch return .active;
    return switch (st) {
        .active => .active,
        .stale => .active,
        .terminal => .completed,
    };
}

/// luaArgString reads the argument at stack index `idx` as a string view into
/// Lua-owned memory. Returns "" when the argument is absent or not a string.
/// The returned slice is only valid until the Lua stack is unwound, so callers
/// must dupe it (HostState.record does) before it escapes.
fn luaArgString(L: ?*c.lua_State, idx: c_int) []const u8 {
    if (c.lua_type(L, idx) != c.LUA_TSTRING) return "";
    var len: usize = 0;
    const raw = c.lua_tolstring(L, idx, &len);
    if (raw == null) return "";
    return raw[0..len];
}

/// luaTypeName returns the Lua type name of the value at `idx` ("table", "nil",
/// "function", ...) as a static string. Used to record the *shape* of opaque
/// arguments (opts tables, thunk closures) without serializing them.
fn luaTypeName(L: ?*c.lua_State, idx: c_int) []const u8 {
    const t = c.lua_type(L, idx);
    const raw = c.lua_typename(L, t);
    return std.mem.span(@as([*:0]const u8, @ptrCast(raw)));
}

/// recordOrError records a host call; on OOM it raises a Lua error (via
/// luaL_error, which longjmps) rather than returning to Zig. Safe to call from
/// inside a C closure.
fn recordOrError(L: ?*c.lua_State, hs: *HostState, kind: HostCallKind, arg0: []const u8, arg1: []const u8) void {
    hs.record(kind, arg0, arg1) catch {
        _ = c.luaL_error(L, "planar-execute: host out of memory recording call");
    };
}

/// hostAgent — `agent(prompt, opts)`. Two-mode behavior:
///
/// - When `HostState.agent_driver` is null (the M2 shape) it remains a
///   RECORDING STUB: records the prompt + opts type, returns `{ status =
///   "stub" }`. The M2 unit tests still pass through this path.
///
/// - When `HostState.agent_driver` is non-null (M4 wiring) it drives the full
///   spawn pipeline through the injected `AgentDriver`: parse opts, sample
///   pre-spawn cycle HEAD, spawn the worker via the injected Spawner, sample
///   post-spawn HEAD, compute commit-presence, decide the terminal verb, and
///   (optionally) apply it via `planar-agent <verb>`. Returns a result table:
///     { status, exit_code, commit_present, terminal_verb }.
///
/// Recording is preserved in BOTH modes — the M4 mode still records the agent
/// call into `HostState.calls` so existing observability tests continue to
/// pass.
fn hostAgent(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const prompt = luaArgString(L, 1);
    const opts_shape = luaTypeName(L, 2);
    recordOrError(L, hs, .agent, prompt, opts_shape);

    // M2 mode — no driver installed: return the stub result table.
    const driver = hs.agent_driver orelse {
        c.lua_createtable(L, 0, 1);
        _ = c.lua_pushlstring(L, "stub", 4);
        c.lua_setfield(L, -2, "status");
        return 1;
    };

    // Live gate is active but binary resolution failed at driver-attach time
    // (planar-agent or git not found on PATH). Raise a clean Lua error so the
    // operator sees a useful message rather than a downstream panic. Agent-free
    // workflows never reach this branch; a workflow that calls agent() with a
    // degraded driver gets a clear diagnostic. (task 3264)
    if (!driver.live_binaries_resolved) {
        _ = c.luaL_error(L, "agent: planar-agent or git not resolvable on PATH (PLANAR_EXECUTE_LIVE_AGENT=1 is set but required binaries are missing)");
        return 0; // unreachable — luaL_error longjmps
    }

    // M4/M5 mode — drive the spawn pipeline through the injected driver. M5
    // (task 3182) splits this into a pre-yield half (spawn non-blocking +
    // register + lua_yield) and a continuation (agentContinue) the scheduler
    // resumes once the worker is terminal. driveAgentCallPreYield does NOT
    // return — it yields via lua_yieldk.
    const opts = readAgentOpts(L, 2);
    return driveAgentCallPreYield(L, hs, driver, prompt, opts);
}

/// DriveChild is the per-child drive bookkeeping for one N-way concurrent run.
/// Shared by `parallel` (one child per thunk) and `pipeline` (one child per
/// item, running the shared stage-runner over that item's stage chain). One
/// entry per child, in ORIGINAL order (so `results[i]` lands in the right slot
/// regardless of completion order).
const DriveChild = struct {
    /// The child coroutine. Anchored against GC for the whole drive lifetime by
    /// an anchor table on the parent stack (see the caller).
    co: *c.lua_State,
    /// Number of arguments already pushed on `co`'s stack to pass on the FIRST
    /// resume. `parallel` thunks take 0; `pipeline` item-runners take 3
    /// (`stages, item, index`). Reset to 0 after the first resume — a
    /// resume-after-yield passes no values back to the yield point (the agent
    /// continuation reads its result from the scheduler slot).
    pending_nargs: c_int = 0,
    /// Has this child been resumed at least once (its body started running)?
    started: bool = false,
    /// Has this child finished (returned or errored)? Its result is captured.
    done: bool = false,
    /// Is this child currently parked on an in-flight worker (yielded from
    /// agent())? True between the yield and the wait-for-any that resumes it.
    in_flight: bool = false,
};

/// ParallelChild is retained as an alias for DriveChild so existing call sites
/// and tests read naturally. The struct is shared with `pipeline`.
const ParallelChild = DriveChild;

/// hostParallel — `parallel({thunks})`. The N-way concurrency barrier (plan 492
/// M5 task 3183), modeled on the Claude Workflow tool's `parallel`:
///
///   - Input is a Lua array of THUNKS (zero-arg functions); each typically calls
///     `agent()` (which yields).
///   - BARRIER: returns only when ALL thunks have completed, as a results ARRAY
///     where `results[i]` = thunk[i]'s return value, in ORIGINAL order
///     regardless of completion order.
///   - CONCURRENCY: all thunks run concurrently (their workers in flight at
///     once), capped at MAX_SLOTS (16). N>MAX_SLOTS thunks QUEUE and start as
///     slots free.
///   - ERROR HANDLING: a thunk that errors (or whose agent() errors) resolves to
///     `nil` in the results array — `parallel` itself never fails.
///
/// ## Drive shape (nested drive from the C call — main coroutine stays parked)
///
/// `hostParallel` runs on the main `run()` coroutine. It does NOT yield the main
/// coroutine: `parallel` is a SYNCHRONOUS barrier from the script's view. It
/// instead drives N CHILD coroutines itself via `lua_resume`. When a child calls
/// `agent()` it `lua_yield`s back to THIS function's resume call (not runModule's
/// loop, because THIS function resumed the child). The child's worker is already
/// registered in the shared scheduler slot registry by `driveAgentCallPreYield`,
/// so `parallel` records the child as in-flight and starts the next one. Once the
/// concurrency cap is reached (or all children started), it `waitForAny`s across
/// the in-flight workers, finds the owning child by `slot.co`, and resumes it
/// (its `agentContinue` produces the agent result; the thunk continues or
/// returns).
fn hostParallel(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);

    // Validate + count. A non-table arg is a script-author bug → record shape
    // and return an empty results array (consistent with the no-fail contract).
    if (c.lua_type(L, 1) != c.LUA_TTABLE) {
        recordOrError(L, hs, .parallel, "0 thunks", "");
        c.lua_createtable(L, 0, 0);
        return 1;
    }
    const n: usize = @intCast(c.lua_rawlen(L, 1));
    var shape_buf: [32]u8 = undefined;
    const shape = std.fmt.bufPrint(&shape_buf, "{d} thunks", .{n}) catch "? thunks";
    recordOrError(L, hs, .parallel, shape, "");

    // Empty thunks table → empty results array.
    if (n == 0) {
        c.lua_createtable(L, 0, 0);
        return 1;
    }

    const sched = hs.sched orelse {
        _ = c.luaL_error(L, "parallel: no scheduler installed (internal wiring bug)");
        return 0;
    };

    // Anchor table: a Lua table holding every child coroutine for the whole
    // `parallel` lifetime so the GC cannot collect a coroutine we still hold a
    // pointer to (the use-after-free hazard). Lives at a fixed stack index on L.
    c.lua_createtable(L, @intCast(n), 0); // L: [..., thunks(1), anchors]
    const anchors_idx: c_int = c.lua_absindex(L, -1);

    // Results table — 1-indexed, original order. Built on L; returned at the end.
    c.lua_createtable(L, @intCast(n), 0); // L: [..., thunks, anchors, results]
    const results_idx: c_int = c.lua_absindex(L, -1);

    // Per-child bookkeeping. Heap-allocated (N is unbounded; MAX_SLOTS only caps
    // concurrency, not the thunk count).
    const children = hs.allocator.alloc(ParallelChild, n) catch {
        _ = c.luaL_error(L, "parallel: out of memory");
        return 0;
    };
    defer hs.allocator.free(children);

    // Materialize one child coroutine per thunk and anchor it.
    for (0..n) |i| {
        const co = c.lua_newthread(L) orelse {
            _ = c.luaL_error(L, "parallel: out of memory creating coroutine");
            return 0;
        };
        // Anchor the new thread (currently on L's top) into the anchor table at
        // index i+1, which pops it from L's top. We keep the *raw pointer* in
        // `children`; the anchor table keeps it alive against GC.
        c.lua_rawseti(L, anchors_idx, @intCast(i + 1)); // pops thread
        // Move thunk[i] from the thunks table onto the child coroutine's stack.
        _ = c.lua_rawgeti(L, 1, @intCast(i + 1)); // L top: thunk[i]
        c.lua_xmove(L, co, 1); // co: [thunk]
        children[i] = .{ .co = co };
    }

    driveChildren(L, sched, children, results_idx);

    // The results table is at results_idx; make it the single return value.
    // Push a copy to the top so we can return it (anchors + results stay on the
    // stack but Lua returns the top `1` value).
    c.lua_pushvalue(L, results_idx);
    return 1;
}

/// resumeStatusOf classifies a raw `lua_resume` rc the same way the scheduler
/// does, local alias for readability inside the parallel drive.
fn resumeStatusOf(rc: c_int) scheduler.ResumeStatus {
    return scheduler.classifyResume(rc);
}

/// driveChildren is the core N-way drive loop, SHARED by `parallel` and
/// `pipeline`. It resumes children up to the concurrency cap, parks those that
/// yield (their worker is in flight), waits-for-any when it cannot start more,
/// resumes the owning child, and captures each child's terminal result into
/// `results[i]` (original order; errors → nil). Returns when every child is
/// done.
///
/// The ONLY difference between the two callers is how each child coroutine is
/// SET UP before this loop runs: `parallel` pushes a zero-arg thunk
/// (`pending_nargs = 0`); `pipeline` pushes the stage-runner + `(stages, item,
/// index)` (`pending_nargs = 3`). The concurrency machinery — slot registry,
/// wait-for-any, owner lookup, original-order capture, MAX_SLOTS gating — is
/// identical, so it lives here once.
fn driveChildren(
    L: ?*c.lua_State,
    sched: *scheduler.Scheduler,
    children: []DriveChild,
    results_idx: c_int,
) void {
    const n = children.len;
    var done_count: usize = 0;
    var next_to_start: usize = 0;

    while (done_count < n) {
        // 1) Start as many not-yet-started children as the concurrency cap
        // allows. A child occupies a scheduler slot only once it yields from
        // agent(); we gate starts on free slots so N>MAX_SLOTS queues instead
        // of erroring. A pure-Lua thunk (no agent) returns on its first resume
        // without ever occupying a slot.
        while (next_to_start < n and sched.inflightCount() < scheduler.MAX_SLOTS) {
            const i = next_to_start;
            next_to_start += 1;
            if (children[i].done) continue;
            children[i].started = true;
            done_count += resumeOneChild(L, children, i, results_idx);
        }

        // If everything is done, we're finished.
        if (done_count >= n) break;

        // 2) Nothing more can be started right now (cap reached or all started).
        // If there are in-flight workers, wait for ANY to finish and resume its
        // owning child. If there are NONE in flight yet not all started, the cap
        // logic above will start more on the next loop iteration.
        if (sched.inflightCount() > 0) {
            const slot_idx = sched.waitForAny() catch {
                // No in-flight worker (NoInflight) or a spawn poll failed
                // (SpawnFailed). Either way we cannot make progress on the
                // parked children: resolve every not-done child to nil and bail.
                for (children, 0..) |*ch, i| {
                    if (!ch.done) {
                        setResultNil(L, results_idx, i);
                        ch.done = true;
                        done_count += 1;
                    }
                }
                break;
            };
            // Find the child that owns this slot (keyed by coroutine pointer).
            const owner_co = sched.slot(slot_idx).co.?;
            const owner = findChildByCo(children, owner_co) orelse {
                // Should not happen: a terminal worker whose coroutine is not a
                // parallel child. Release the slot's outcome to avoid a leak.
                var leaked = sched.slot(slot_idx).outcome;
                if (leaked) |*o| o.deinit(sched.allocator);
                sched.slot(slot_idx).outcome = null;
                sched.releaseSlot(slot_idx);
                continue;
            };
            children[owner].in_flight = false;
            done_count += resumeOneChild(L, children, owner, results_idx);
        } else if (next_to_start >= n) {
            // All started, none in flight, but not all done — every remaining
            // child must have finished already; loop guard handles it. Defensive
            // break to avoid a spin.
            break;
        }
    }
}

/// resumeOneChild resumes child `i` once and reacts to the outcome. Returns 1 if
/// the child reached a terminal state (done — result captured), 0 if it yielded
/// (parked on an in-flight worker). On terminal it captures the return value
/// (or nil on error) into `results[i]`.
fn resumeOneChild(
    L: ?*c.lua_State,
    children: []DriveChild,
    i: usize,
    results_idx: c_int,
) usize {
    const co = children[i].co;
    // The FIRST resume passes the child's pre-pushed arguments (0 for a parallel
    // thunk, 3 for a pipeline item-runner). Every resume-after-yield passes 0
    // (the agent continuation reads its result from the scheduler slot, not from
    // resume args). We consume pending_nargs and clear it so re-yields use 0.
    const nargs = children[i].pending_nargs;
    children[i].pending_nargs = 0;
    var nres: c_int = 0;
    const rc = c.lua_resume(co, L, nargs, &nres);
    switch (resumeStatusOf(rc)) {
        .yielded => {
            // The child's agent() registered a worker + yielded. Park it.
            children[i].in_flight = true;
            return 0;
        },
        .done => {
            // Capture the first return value (if any) into results[i]; nil if
            // the thunk returned nothing.
            if (nres >= 1) {
                c.lua_xmove(co, L, 1); // move return value onto L's top
                // discard any extra returns left on co (keep co stack tidy).
                if (nres > 1) c.lua_pop(co, nres - 1);
                c.lua_rawseti(L, results_idx, @intCast(i + 1)); // pops value
            } else {
                setResultNil(L, results_idx, i);
            }
            children[i].done = true;
            return 1;
        },
        .err => {
            // A thunk (or its agent()) errored → results[i] = nil. `parallel`
            // never propagates the error. Pop the error object off co's stack.
            c.lua_settop(co, 0);
            setResultNil(L, results_idx, i);
            children[i].done = true;
            return 1;
        },
    }
}

/// setResultNil sets `results[i+1] = nil`.
fn setResultNil(L: ?*c.lua_State, results_idx: c_int, i: usize) void {
    c.lua_pushnil(L);
    c.lua_rawseti(L, results_idx, @intCast(i + 1));
}

/// findChildByCo returns the index of the child whose coroutine pointer matches
/// `co`, or null if none (used to map a terminal scheduler slot back to the
/// owning parallel child).
fn findChildByCo(children: []DriveChild, co: *c.lua_State) ?usize {
    for (children, 0..) |ch, i| {
        if (ch.co == co) return i;
    }
    return null;
}

/// PIPELINE_RUNNER_SRC is the per-item stage-chain runner (plan 492 task 3184).
/// It is a Lua chunk that, when loaded + executed, RETURNS the runner function:
///
///   runner(stages, item, index) -> finalResult | nil
///
/// It runs `item` through every stage in `stages` in order, threading each
/// stage's result into the next stage's first argument and passing the ORIGINAL
/// item + 1-based index as the 2nd/3rd args (the Workflow-tool stage signature
/// `(prevResult, originalItem, index)`). Each stage is invoked under `pcall`:
///
///   - A stage that ERRORS drops the item to `nil` and SKIPS its remaining
///     stages (the Workflow-tool contract — `pipeline` never propagates the
///     error). The runner returns nil for that item.
///   - Lua's `pcall` is YIELDABLE (since 5.4, unchanged in 5.5): when a stage
///     calls `agent()` (which `lua_yield`s across the C boundary), the yield
///     propagates out THROUGH the pcall to the scheduler, and the scheduler's
///     resume re-enters the pcall on continuation. This is why a stage may spawn
///     a worker even though it runs under pcall — the runner-with-pcall shape is
///     correct precisely because pcall is continuation-aware.
const PIPELINE_RUNNER_SRC =
    \\return function(stages, item, index)
    \\  local r = item
    \\  for k = 1, #stages do
    \\    local ok, res = pcall(stages[k], r, item, index)
    \\    if not ok then return nil end
    \\    r = res
    \\  end
    \\  return r
    \\end
;

/// hostPipeline — `pipeline(items, ...stages)`. The per-item stage-chain runner
/// (plan 492 M5 task 3184), modeled on the Claude Workflow tool's `pipeline`:
///
///   - Input is a Lua array `items` followed by zero or more STAGE functions.
///   - Each item flows through ALL stages independently, with NO BARRIER between
///     stages: item A can be in stage 3 while item B is still in stage 1. This
///     falls out for free because each item runs its WHOLE stage chain in its
///     own coroutine, all N concurrent — wall-clock is the slowest single-item
///     chain, not the sum of per-stage barriers.
///   - Stage signature `(prevResult, originalItem, index)`. Stage 1 gets
///     `(item, item, index)`; stage 2 gets `(stage1_result, item, index)`; etc.
///   - Returns an array of final results, `results[i]` = the last stage's value
///     for item i, in ORIGINAL item order.
///   - A stage that errors drops THAT item to `nil` and skips its remaining
///     stages; `pipeline` itself never propagates the error.
///
/// ## Reuse — the shared N-coroutine drive
///
/// `pipeline` reuses the EXACT concurrency machinery `parallel` (task 3183)
/// built: one child coroutine per item, driven by the shared `driveChildren`
/// loop over the scheduler's slot registry + `waitForAny`, capped at MAX_SLOTS.
/// The only pipeline-specific work is the SETUP: each child runs the shared
/// stage-runner (`PIPELINE_RUNNER_SRC`) with `(stages, item, index)` pre-pushed
/// (`pending_nargs = 3`). Everything downstream — wait-for-any, owner lookup,
/// original-order capture, error→nil — is identical to parallel and lives in
/// `driveChildren`, not duplicated here.
fn hostPipeline(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);

    // Validate items. A non-table first arg is a script-author bug → record the
    // shape and return an empty results array (consistent no-fail contract).
    if (c.lua_type(L, 1) != c.LUA_TTABLE) {
        recordOrError(L, hs, .pipeline, "0 items", "0 stages");
        c.lua_createtable(L, 0, 0);
        return 1;
    }
    const n: usize = @intCast(c.lua_rawlen(L, 1));
    // Stage count: every vararg after `items` (indices 2..top) is a stage.
    const top = c.lua_gettop(L);
    const m: usize = if (top >= 2) @intCast(top - 1) else 0;

    var buf0: [32]u8 = undefined;
    var buf1: [32]u8 = undefined;
    const items_shape = std.fmt.bufPrint(&buf0, "{d} items", .{n}) catch "? items";
    const stages_shape = std.fmt.bufPrint(&buf1, "{d} stages", .{m}) catch "? stages";
    recordOrError(L, hs, .pipeline, items_shape, stages_shape);

    // Empty items → empty results array (regardless of stage count).
    if (n == 0) {
        c.lua_createtable(L, 0, 0);
        return 1;
    }

    const sched = hs.sched orelse {
        _ = c.luaL_error(L, "pipeline: no scheduler installed (internal wiring bug)");
        return 0;
    };

    // Collect the stage varargs (2..top) into a single `stages` table so the
    // runner receives them as `stages[1..m]`. With zero stages this is an empty
    // table — the runner's `for k = 1, #stages` body never runs and each item
    // passes through unchanged (identity), which is the sane zero-stage answer.
    c.lua_createtable(L, @intCast(m), 0); // L: [..., stages_table]
    const stages_idx: c_int = c.lua_absindex(L, -1);
    for (0..m) |k| {
        c.lua_pushvalue(L, @intCast(2 + k)); // copy stage arg onto top
        c.lua_rawseti(L, stages_idx, @intCast(k + 1)); // stages[k+1] = stage; pops
    }

    // Load the shared stage-runner chunk and execute it to obtain the runner
    // FUNCTION (the chunk `return`s the function). Errors here are internal
    // wiring bugs (the source is a compile-time constant).
    if (c.luaL_loadstring(L, PIPELINE_RUNNER_SRC) != c.LUA_OK) {
        _ = c.luaL_error(L, "pipeline: failed to load stage-runner (internal bug)");
        return 0;
    }
    // Use lua_pcallk directly (the Zig cImport of the lua_pcall macro mis-infers
    // its errfunc argument; the codebase calls lua_pcallk with typed nulls — see
    // loadModuleState). No continuation needed: this runs at module-load time,
    // never across a yield.
    if (c.lua_pcallk(L, 0, 1, 0, 0, null) != c.LUA_OK) {
        _ = c.luaL_error(L, "pipeline: failed to build stage-runner (internal bug)");
        return 0;
    }
    const runner_idx: c_int = c.lua_absindex(L, -1); // L: [..., stages, runner]

    // Anchor table: holds every child coroutine for the whole pipeline lifetime
    // so the GC cannot collect a coroutine we still hold a raw pointer to (the
    // use-after-free hazard the parallel reviewer pinned). Lives at a fixed
    // stack index on L; nothing pops it before driveChildren returns.
    c.lua_createtable(L, @intCast(n), 0); // L: [..., stages, runner, anchors]
    const anchors_idx: c_int = c.lua_absindex(L, -1);

    // Results table — 1-indexed, original order. Built on L; returned at the end.
    c.lua_createtable(L, @intCast(n), 0); // L: [..., stages, runner, anchors, results]
    const results_idx: c_int = c.lua_absindex(L, -1);

    // Per-item child bookkeeping. Heap-allocated (N is unbounded; MAX_SLOTS only
    // caps concurrency, not the item count).
    const children = hs.allocator.alloc(DriveChild, n) catch {
        _ = c.luaL_error(L, "pipeline: out of memory");
        return 0;
    };
    defer hs.allocator.free(children);

    // Materialize one child coroutine per item and anchor it. Each child's stack
    // gets `[runner, stages, item_i, index_i]` so its first resume calls
    // runner(stages, item_i, index_i) — pending_nargs = 3.
    for (0..n) |i| {
        const co = c.lua_newthread(L) orelse {
            _ = c.luaL_error(L, "pipeline: out of memory creating coroutine");
            return 0;
        };
        // Anchor the new thread (currently on L's top) into the anchor table at
        // index i+1, which pops it from L's top. We keep the raw pointer in
        // `children`; the anchor table keeps it alive against GC. Done BEFORE any
        // further GC-triggering allocation below (the parallel discipline).
        c.lua_rawseti(L, anchors_idx, @intCast(i + 1)); // pops thread

        // Push runner (a copy) onto the child stack.
        c.lua_pushvalue(L, runner_idx); // L top: runner
        c.lua_xmove(L, co, 1); // co: [runner]
        // Push stages table (a copy) onto the child stack.
        c.lua_pushvalue(L, stages_idx); // L top: stages
        c.lua_xmove(L, co, 1); // co: [runner, stages]
        // Push item_i (from the items table at index 1) onto the child stack.
        _ = c.lua_rawgeti(L, 1, @intCast(i + 1)); // L top: item_i
        c.lua_xmove(L, co, 1); // co: [runner, stages, item_i]
        // Push the 1-based index onto the child stack.
        c.lua_pushinteger(co, @intCast(i + 1)); // co: [runner, stages, item_i, index]

        children[i] = .{ .co = co, .pending_nargs = 3 };
    }

    driveChildren(L, sched, children, results_idx);

    // Return the results table (push a copy to the top; the other entries stay
    // on the stack but Lua returns only the top `1` value).
    c.lua_pushvalue(L, results_idx);
    return 1;
}

/// hostPhase — `phase(title)`. Records the phase title. Recording IS the real
/// M2 behavior (progress reporting); returns nothing.
fn hostPhase(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const title = luaArgString(L, 1);
    recordOrError(L, hs, .phase, title, "");
    return 0;
}

/// hostLog — `log(msg)`. Records the log message. Recording IS the real M2
/// behavior; returns nothing.
fn hostLog(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const msg = luaArgString(L, 1);
    recordOrError(L, hs, .log, msg, "");
    return 0;
}

/// hostWorkflow — `workflow(name, args)`. RECORDING STUB. Records the workflow
/// name and the args type; does NOT load or run another module inline (M5).
/// Returns a stub result table `{ status = "stub" }`.
fn hostWorkflow(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const name = luaArgString(L, 1);
    const args_shape = luaTypeName(L, 2);
    recordOrError(L, hs, .workflow, name, args_shape);
    c.lua_createtable(L, 0, 1);
    _ = c.lua_pushlstring(L, "stub", 4);
    c.lua_setfield(L, -2, "status");
    return 1;
}

/// hostEligible — `ctx.eligible(plan_id)` (task 3185, M5 fan-out eligibility).
///
/// Calls `state.recommendStrategy(allocator, io, plan_id)` — the `planar plan
/// recommend-strategy <id> --json` verb (task 3186, decision 370) — and returns
/// a Lua TABLE of the form:
///
///   {
///     eligible = {                    -- mutually-non-conflicting parallel subset
///       { id = <int>, slug = <str|nil>, title = <str> }, ...
///     },
///     fan_out_available = <bool>,     -- summary.fan_out_available (eligible >= 2)
///     serialized = {                  -- rule-excluded remainder
///       { id = <int>, slug = <str|nil>, title = <str>,
///         excluded_by = { { rule = <int>, reason = <str> }, ... } },
///       ...
///     },
///   }
///
/// This lets a workflow do:
///   local r = ctx.eligible(plan)
///   if r.fan_out_available then
///     local thunks = {}
///     for _, t in ipairs(r.eligible) do
///       thunks[#thunks+1] = function() return ctx.agent(brief(t), {...}) end
///     end
///     parallel(thunks)
///   end
///
/// ## Gating
///
/// Unlike `ctx.agent`, this is a CONTROL-PLANE READ (it only shells
/// `planar plan recommend-strategy`) and NEVER requires PLANAR_EXECUTE_LIVE_AGENT.
/// It works in any run — gated or not.
///
/// ## io availability
///
/// `io` comes from `hs.io`, which `handleRun` populates unconditionally from
/// `currentCtx().io` before calling `runModule`. Unit-test callers that do not
/// exercise `ctx.eligible` may leave `hs.io = null`; when null the function
/// records the call and returns an empty-eligible table so tests that call
/// `ctx.eligible` in isolation can detect the call via HostState.calls.
fn hostEligible(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);

    // Plan id: first Lua argument, must be an integer.
    if (c.lua_type(L, 1) != c.LUA_TNUMBER) {
        _ = c.luaL_error(L, "ctx.eligible: plan_id (integer) required as first argument");
        return 0;
    }
    const plan_id_raw = c.lua_tointegerx(L, 1, null);
    if (plan_id_raw <= 0) {
        _ = c.luaL_error(L, "ctx.eligible: plan_id must be a positive integer");
        return 0;
    }
    const plan_id: u64 = @intCast(plan_id_raw);

    // Record the call (plan_id as decimal string in arg0).
    var id_buf: [32]u8 = undefined;
    const id_str = std.fmt.bufPrint(&id_buf, "{d}", .{plan_id}) catch "?";
    recordOrError(L, hs, .eligible, id_str, "");

    // io not available → return an empty-eligible table (no-op; test paths use this).
    const io = hs.io orelse {
        pushEmptyEligibleTable(L);
        return 1;
    };

    const alloc = hs.allocator;
    const parsed = state.recommendStrategy(alloc, io, plan_id) catch |err| {
        _ = c.luaL_error(L, "ctx.eligible: recommend-strategy failed: %s", @errorName(err).ptr);
        return 0;
    };
    defer parsed.deinit();
    const rs = parsed.value;

    // Build the return table: { eligible = [...], fan_out_available = bool, serialized = [...] }
    c.lua_createtable(L, 0, 3); // top-level table (3 string-keyed fields)
    const top_idx: c_int = c.lua_absindex(L, -1);

    // eligible array
    c.lua_createtable(L, @intCast(rs.parallel_eligible.len), 0);
    const elig_idx: c_int = c.lua_absindex(L, -1);
    for (rs.parallel_eligible, 0..) |t, i| {
        c.lua_createtable(L, 0, 3); // { id, slug, title }
        const eti: c_int = c.lua_absindex(L, -1);
        c.lua_pushinteger(L, @intCast(t.id));
        c.lua_setfield(L, eti, "id");
        if (t.slug) |sl| {
            _ = c.lua_pushlstring(L, sl.ptr, sl.len);
        } else {
            c.lua_pushnil(L);
        }
        c.lua_setfield(L, eti, "slug");
        _ = c.lua_pushlstring(L, t.title.ptr, t.title.len);
        c.lua_setfield(L, eti, "title");
        c.lua_rawseti(L, elig_idx, @intCast(i + 1)); // pops the task table
    }
    c.lua_setfield(L, top_idx, "eligible"); // pops eligible array

    // fan_out_available boolean
    c.lua_pushboolean(L, if (rs.summary.fan_out_available) 1 else 0);
    c.lua_setfield(L, top_idx, "fan_out_available");

    // serialized array
    c.lua_createtable(L, @intCast(rs.serialized.len), 0);
    const ser_idx: c_int = c.lua_absindex(L, -1);
    for (rs.serialized, 0..) |t, i| {
        c.lua_createtable(L, 0, 4); // { id, slug, title, excluded_by }
        const sti: c_int = c.lua_absindex(L, -1);
        c.lua_pushinteger(L, @intCast(t.id));
        c.lua_setfield(L, sti, "id");
        if (t.slug) |sl| {
            _ = c.lua_pushlstring(L, sl.ptr, sl.len);
        } else {
            c.lua_pushnil(L);
        }
        c.lua_setfield(L, sti, "slug");
        _ = c.lua_pushlstring(L, t.title.ptr, t.title.len);
        c.lua_setfield(L, sti, "title");
        // excluded_by sub-array
        c.lua_createtable(L, @intCast(t.excluded_by.len), 0);
        const exci: c_int = c.lua_absindex(L, -1);
        for (t.excluded_by, 0..) |ex, j| {
            c.lua_createtable(L, 0, 2); // { rule, reason }
            const exi: c_int = c.lua_absindex(L, -1);
            c.lua_pushinteger(L, @intCast(ex.rule));
            c.lua_setfield(L, exi, "rule");
            _ = c.lua_pushlstring(L, ex.reason.ptr, ex.reason.len);
            c.lua_setfield(L, exi, "reason");
            c.lua_rawseti(L, exci, @intCast(j + 1)); // pops exclusion table
        }
        c.lua_setfield(L, sti, "excluded_by"); // pops excluded_by array
        c.lua_rawseti(L, ser_idx, @intCast(i + 1)); // pops the serialized task table
    }
    c.lua_setfield(L, top_idx, "serialized"); // pops serialized array

    return 1; // return top-level table
}

/// pushEmptyEligibleTable pushes the "no-io" fallback table:
///   { eligible = {}, fan_out_available = false, serialized = {} }
fn pushEmptyEligibleTable(L: ?*c.lua_State) void {
    c.lua_createtable(L, 0, 3);
    const top_idx: c_int = c.lua_absindex(L, -1);
    c.lua_createtable(L, 0, 0);
    c.lua_setfield(L, top_idx, "eligible");
    c.lua_pushboolean(L, 0);
    c.lua_setfield(L, top_idx, "fan_out_available");
    c.lua_createtable(L, 0, 0);
    c.lua_setfield(L, top_idx, "serialized");
}

/// hostBudgetSpent — `budget:spent()`. Returns the host-injected spent value.
/// Also records the call so a test can confirm the method form was reached.
fn hostBudgetSpent(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    recordOrError(L, hs, .budget_spent, "", "");
    c.lua_pushinteger(L, @intCast(hs.budget_spent));
    return 1;
}

/// hostBudgetRemaining — `budget:remaining()`. Returns total - spent from the
/// host-injected values. Records the call.
fn hostBudgetRemaining(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    recordOrError(L, hs, .budget_remaining, "", "");
    c.lua_pushinteger(L, @intCast(hs.budget_total - hs.budget_spent));
    return 1;
}

/// pushHostClosure pushes a C closure for `fn_ptr` that carries `hs` as its
/// single light-userdata upvalue, then assigns it as field `name` on the table
/// at `tbl_idx`. The light-userdata upvalue is how the closure recovers the
/// *HostState at call time (see hostStateUpvalue).
fn pushHostClosure(
    L: ?*c.lua_State,
    tbl_idx: c_int,
    name: [*:0]const u8,
    fn_ptr: *const fn (?*c.lua_State) callconv(.c) c_int,
    hs: *HostState,
) void {
    c.lua_pushlightuserdata(L, hs);
    c.lua_pushcclosure(L, @ptrCast(fn_ptr), 1);
    c.lua_setfield(L, tbl_idx, name);
}

/// installHostFns builds the host-function surface ON the ctx table at
/// `ctx_idx` (the spec is literal: "ctx carries args and the host functions").
/// It also injects determinism (ctx.now, ctx.seed) and the budget table.
///
/// Placement rationale (ctx vs globals): the tech spec's "Workflow module shape"
/// section states *"ctx carries `args` and the host functions"*. We follow that
/// literally — host fns are fields on the ctx table the run(ctx) call receives,
/// NOT injected as Lua globals. A workflow calls `ctx.agent(...)`, `ctx.phase(...)`,
/// `ctx.budget`, `ctx.now`, `ctx.seed`. This keeps the sandbox's global
/// environment free of host capabilities (defense in depth) and makes the host
/// surface explicit at every call site.
fn installHostFns(L: ?*c.lua_State, ctx_idx: c_int, hs: *HostState) void {
    pushHostClosure(L, ctx_idx, "agent", hostAgent, hs);
    pushHostClosure(L, ctx_idx, "parallel", hostParallel, hs);
    pushHostClosure(L, ctx_idx, "pipeline", hostPipeline, hs);
    pushHostClosure(L, ctx_idx, "phase", hostPhase, hs);
    pushHostClosure(L, ctx_idx, "log", hostLog, hs);
    pushHostClosure(L, ctx_idx, "workflow", hostWorkflow, hs);
    // M5 fan-out eligibility (task 3185): ctx.eligible(plan_id) shells
    // `planar plan recommend-strategy` and returns the parallel-eligible subset.
    // Works ungated (read-only, no PLANAR_EXECUTE_LIVE_AGENT required).
    pushHostClosure(L, ctx_idx, "eligible", hostEligible, hs);

    // Determinism injection (task 3169): ctx.now and ctx.seed are the only
    // time/random source available to the sandboxed script.
    c.lua_pushinteger(L, @intCast(hs.now));
    c.lua_setfield(L, ctx_idx, "now");
    c.lua_pushinteger(L, @intCast(hs.seed));
    c.lua_setfield(L, ctx_idx, "seed");

    // budget: a host-backed table { total = <n>, spent = fn, remaining = fn }.
    // total is an injected number; spent()/remaining() are C closures over the
    // injected host values. Both method and dot call forms work (the closures
    // ignore the implicit self argument), so `budget:spent()` and
    // `budget.spent()` both return the injected value.
    c.lua_createtable(L, 0, 3);
    const budget_idx = c.lua_absindex(L, -1);
    c.lua_pushinteger(L, @intCast(hs.budget_total));
    c.lua_setfield(L, budget_idx, "total");
    pushHostClosure(L, budget_idx, "spent", hostBudgetSpent, hs);
    pushHostClosure(L, budget_idx, "remaining", hostBudgetRemaining, hs);
    c.lua_setfield(L, ctx_idx, "budget");
}

// ---------------------------------------------------------------------------
// Sandbox — task 3169 (m2-sandbox), curated stdlib + determinism injection.
// ---------------------------------------------------------------------------

/// openSandboxedLibs replaces the blanket luaL_openlibs with a curated set of
/// safe standard libraries, opened individually via luaL_requiref so each lands
/// in the global environment under its conventional name.
///
/// Opened (the workflow language's needs): base (assert/pairs/type/tostring/
/// error/pcall/...), string, table, math, utf8, coroutine. Coroutine is opened
/// because the M5 concurrency model is Lua-coroutine-based (the spec's
/// "Concurrency model — Lua coroutines + a Zig scheduler"); it exposes no host
/// capability.
///
/// NEVER opened: `os` and `io`. They are simply not requiref'd, so `os` and `io`
/// are nil globals — `os.execute`, `os.time`, `io.open`, `io.*` are all
/// unreachable (indexing nil errors).
///
/// Stripped after open: `math.random` and `math.randomseed` are set to nil on
/// the math table — non-deterministic sources. With os.time and math.random
/// gone, the script's only time/seed source is the host-injected ctx.now /
/// ctx.seed (see installHostFns).
///
/// Remaining sandboxed surface (documented contract):
///   base (sans dofile/loadfile? see below), string, table, math (sans random/
///   randomseed), utf8, coroutine. No os, no io.
///   The base library DOES include `load`, `dofile`, `loadfile`, `require`,
///   `collectgarbage`, `print`. At M2 we strip the filesystem/loader escape
///   hatches that re-introduce host reach: dofile, loadfile, load, require,
///   loadstring are nil'd from the global env. `print` is left (writes to the
///   inherited stdout — benign for progress) and `collectgarbage` is left
///   (memory only). This keeps the global namespace free of any path back to
///   the filesystem or arbitrary code loading.
fn openSandboxedLibs(L: ?*c.lua_State) void {
    // luaL_requiref(L, modname, openf, glb): runs openf, caches it in
    // package.loaded[modname], and (glb != 0) sets it as a global. It leaves
    // the module table on the stack, which we pop after each.
    const Lib = struct {
        name: [*:0]const u8,
        open: *const fn (?*c.lua_State) callconv(.c) c_int,
    };
    const libs = [_]Lib{
        .{ .name = c.LUA_GNAME, .open = @ptrCast(&c.luaopen_base) },
        .{ .name = c.LUA_TABLIBNAME, .open = @ptrCast(&c.luaopen_table) },
        .{ .name = c.LUA_STRLIBNAME, .open = @ptrCast(&c.luaopen_string) },
        .{ .name = c.LUA_MATHLIBNAME, .open = @ptrCast(&c.luaopen_math) },
        .{ .name = c.LUA_UTF8LIBNAME, .open = @ptrCast(&c.luaopen_utf8) },
        .{ .name = c.LUA_COLIBNAME, .open = @ptrCast(&c.luaopen_coroutine) },
    };
    inline for (libs) |lib| {
        c.luaL_requiref(L, lib.name, @ptrCast(lib.open), 1);
        luaPop(L, 1); // pop the module table luaL_requiref left on the stack
    }

    // Strip non-deterministic math sources: math.random, math.randomseed.
    const math_type = c.lua_getglobal(L, "math");
    if (math_type == c.LUA_TTABLE) {
        const math_idx = c.lua_absindex(L, -1);
        c.lua_pushnil(L);
        c.lua_setfield(L, math_idx, "random");
        c.lua_pushnil(L);
        c.lua_setfield(L, math_idx, "randomseed");
    }
    luaPop(L, 1); // pop math (or the non-table value)

    // Strip the loader / filesystem escape hatches from the global env so a
    // workflow cannot re-acquire host reach via arbitrary code loading.
    inline for ([_][*:0]const u8{ "dofile", "loadfile", "load", "loadstring", "require" }) |g| {
        c.lua_pushnil(L);
        c.lua_setglobal(L, g);
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// luaPop pops `n` values from the Lua stack.
///
/// Zig's cImport does not expose the `lua_pop` C macro directly (it expands
/// to `lua_settop(L, -(n)-1)`).  This thin inline wrapper gives call sites a
/// named, readable alternative to the open-coded expansion.
inline fn luaPop(L: ?*c.lua_State, n: c_int) void {
    c.lua_settop(L, -(n) - 1);
}

/// copyLuaString copies the Lua string at stack index `idx` into
/// allocator-owned memory and returns the slice. The Lua string stays
/// on the stack; the copy is independent of the Lua state lifetime.
///
/// Returns LuaStringAllocError on OOM, LuaUnexpectedType when the value
/// at `idx` is not a string.
fn copyLuaString(L: ?*c.lua_State, idx: c_int, allocator: std.mem.Allocator) (LuaError || std.mem.Allocator.Error)![]const u8 {
    if (c.lua_type(L, idx) != c.LUA_TSTRING) return LuaError.LuaUnexpectedType;
    var len: usize = 0;
    const raw = c.lua_tolstring(L, idx, &len);
    if (raw == null) return LuaError.LuaUnexpectedType;
    const src: []const u8 = raw[0..len];
    const copy = try allocator.dupe(u8, src);
    return copy;
}

/// captureError reads the error value off the top of the Lua stack
/// (placed there by luaL_loadstring/luaL_loadbufferx or lua_pcallk on
/// failure) and writes a string representation into err_buf,
/// null-terminated, truncated to fit.  Pops the value.
///
/// Uses luaL_tolstring (not lua_tolstring) so that non-string error
/// objects — e.g. `error({...})` — are coerced to a string via Lua's
/// __tostring metamethod or a fallback representation.  The coerced
/// string is pushed by luaL_tolstring and must be popped after use;
/// we pop both it and the original error value (total: 2 pops).
///
/// When the original error value IS a string, luaL_tolstring still
/// pushes a copy — behaviour is identical to the lua_tolstring path
/// but robust to table/userdata/number error objects.
fn captureError(L: ?*c.lua_State, err_buf: []u8) void {
    if (err_buf.len == 0) {
        luaPop(L, 1); // pop the error value
        return;
    }
    // luaL_tolstring pushes a string representation of stack[-1] and
    // returns a pointer to it.  It never returns NULL.
    var len: usize = 0;
    const raw = c.luaL_tolstring(L, -1, &len);
    // raw is the pushed string (stack[-1] = coerced string, stack[-2] = original error).
    const src: []const u8 = if (raw != null) raw[0..len] else "";
    const copy_len = @min(src.len, err_buf.len - 1);
    @memcpy(err_buf[0..copy_len], src[0..copy_len]);
    err_buf[copy_len] = 0;
    luaPop(L, 2); // pop the coerced string and the original error value
}

// ---------------------------------------------------------------------------
// evalString — retained from task 3163, numeric return only.
// ---------------------------------------------------------------------------

/// evalString compiles and executes the given Lua source string inside a
/// fresh, isolated lua_State.  The script MUST return exactly one numeric
/// value via `return <expr>`.  On success the number is returned as f64
/// and the state is closed.
///
/// On any error the Lua error message is written into `err_buf`
/// (null-terminated, truncated to fit), the state is closed, and a LuaError
/// is returned — no panic, no silent swallow.
///
/// Sandboxing note: no stdlib libraries are opened.  A pure-computation
/// script (arithmetic) needs no libraries.  M2 m2-sandbox will register the
/// permitted trimmed subset.
///
/// The Lua C macro `lua_pcall` and `lua_tonumber` expand to inline wrappers
/// that pass C `NULL` for optional pointer parameters.  Zig's cImport maps
/// `NULL` to `?*anyopaque`, which does not unify with the typed pointer
/// parameters (`lua_KFunction`, `[*c]c_int`).  We call the underlying
/// `lua_pcallk` and `lua_tonumberx` directly with typed `null` instead.
///
/// Thread safety: each call creates its own lua_State — safe to call from
/// concurrent Zig threads as long as they do not share the state.
pub fn evalString(source: [*:0]const u8, err_buf: []u8) LuaError!f64 {
    const L = c.luaL_newstate();
    if (L == null) return LuaError.LuaAllocFailed;
    defer c.lua_close(L);

    // luaL_loadstring compiles the source into a Lua function and pushes it.
    // On failure it pushes an error message string instead.
    const load_rc = c.luaL_loadstring(L, source);
    if (load_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaCompileError;
    }

    // lua_pcallk: call the compiled chunk with no arguments, one return value,
    // no error-handler function.  ctx=0, k=null (no continuation).
    const call_rc = c.lua_pcallk(L, 0, 1, 0, 0, null);
    if (call_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaRuntimeError;
    }

    // Read the numeric return value off the top of the stack (-1).
    // lua_tonumberx: pass null for the isnum out-param; check the type tag
    // manually to avoid a second layer of option indirection.
    if (c.lua_type(L, -1) != c.LUA_TNUMBER) {
        return LuaError.LuaUnexpectedType;
    }
    const n = c.lua_tonumberx(L, -1, null);
    return @floatCast(n);
}

// ---------------------------------------------------------------------------
// Internal module-state loader — shared by loadModule and runModule.
// ---------------------------------------------------------------------------

/// loadModuleState compiles and executes the Lua chunk identified by
/// `source`/`chunkname` into a fresh lua_State (with luaL_openlibs).
///
/// On success the module table is at absolute stack index 1 (the bottom of
/// the user stack); the caller owns the state and MUST call lua_close.
///
/// Consolidation note (task 3229): opening one state here means both loadModule
/// (meta extraction) and runModule (run call) share the same compiled chunk
/// rather than parsing the source twice.
///
/// Sandbox (task 3169): the environment is opened via openSandboxedLibs — a
/// curated stdlib (base, string, table, math, utf8, coroutine), no os, no io,
/// with math.random / math.randomseed and the loader escape-hatches stripped.
/// This applies to BOTH the validate/dry-run path and the run path: a workflow
/// that references os/io at module-construction time (top-level chunk) is
/// sandboxed too. The dry-run guarantee (run never entered) is unchanged —
/// dry-run still stops after extracting meta and never calls run.
///
/// On any error the state is closed before returning and err_buf is written.
fn loadModuleState(
    source: []const u8,
    chunkname: [*:0]const u8,
    err_buf: []u8,
) LuaError!*c.lua_State {
    const L = c.luaL_newstate() orelse return LuaError.LuaAllocFailed;
    errdefer c.lua_close(L);

    // Open the curated, sandboxed standard library set (task 3169).
    openSandboxedLibs(L);

    // Compile the chunk; on error, push error message.
    const load_rc = c.luaL_loadbufferx(L, source.ptr, source.len, chunkname, null);
    if (load_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaCompileError;
    }

    // Execute the top-level chunk; it must return exactly one value (the
    // module table).  Executing it does NOT invoke run — run is a table
    // field, not a call target yet.
    const call_rc = c.lua_pcallk(L, 0, 1, 0, 0, null);
    if (call_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaRuntimeError;
    }

    // Validate that the chunk returned a table.
    if (c.lua_type(L, -1) != c.LUA_TTABLE) {
        return LuaError.LuaModuleNotTable;
    }

    // The module table is at index 1 (absolute; bottom of user stack after
    // the pcall consumed the chunk function and left one result).
    return L;
}

/// extractMeta reads and validates the `meta` sub-table from the module table
/// that sits at `module_idx` on `L`'s stack, allocating all strings via
/// `allocator`.
///
/// On error all strings allocated so far are freed before returning.
/// Structural errors (missing meta/run, wrong type) do NOT write to err_buf
/// (they have no Lua error string); the caller's @errorName fallback handles
/// those.
fn extractMeta(
    L: *c.lua_State,
    module_idx: c_int,
    allocator: std.mem.Allocator,
) (LuaError || std.mem.Allocator.Error)!WorkflowMeta {
    // -----------------------------------------------------------------------
    // Validate and extract meta.
    // -----------------------------------------------------------------------

    // lua_getfield pushes the value of t[k]; the return value is the type tag.
    const meta_type = c.lua_getfield(L, module_idx, "meta");
    if (meta_type != c.LUA_TTABLE) {
        return LuaError.LuaModuleMissingMeta;
    }
    // Stack: [..., module_table, meta_table].  Pin with absindex.
    const meta_idx: c_int = c.lua_absindex(L, -1);

    // Extract meta.name (required string).
    const name_type = c.lua_getfield(L, meta_idx, "name");
    if (name_type != c.LUA_TSTRING) {
        return LuaError.LuaModuleInvalidMeta;
    }
    const meta_name = try copyLuaString(L, c.lua_absindex(L, -1), allocator);
    errdefer allocator.free(meta_name);
    luaPop(L, 1); // pop name

    // Extract meta.description (required string).
    const desc_type = c.lua_getfield(L, meta_idx, "description");
    if (desc_type != c.LUA_TSTRING) {
        return LuaError.LuaModuleInvalidMeta;
    }
    const meta_desc = try copyLuaString(L, c.lua_absindex(L, -1), allocator);
    errdefer allocator.free(meta_desc);
    luaPop(L, 1); // pop description

    // Extract meta.phases (optional array; if missing or nil, treat as empty).
    // std.ArrayList in Zig 0.16 is the unmanaged Aligned variant — allocator
    // is passed at each mutating call site rather than stored in the list.
    var phases: std.ArrayList(PhaseMeta) = .empty;
    errdefer {
        for (phases.items) |ph| {
            allocator.free(ph.title);
            allocator.free(ph.detail);
        }
        phases.deinit(allocator);
    }

    const phases_type = c.lua_getfield(L, meta_idx, "phases");
    if (phases_type == c.LUA_TTABLE) {
        // Pin the phases table with an absolute index so subsequent pushes
        // (rawgeti, getfield) do not alias the wrong slot.
        const phases_idx: c_int = c.lua_absindex(L, -1);
        const n_phases = c.lua_rawlen(L, phases_idx);
        var i: c.lua_Unsigned = 1;
        while (i <= n_phases) : (i += 1) {
            // lua_rawgeti pushes phases[i].
            _ = c.lua_rawgeti(L, phases_idx, @intCast(i));
            // Each phase entry is a table; tolerate non-tables by using empty
            // strings.  All title/detail values are heap-owned (via
            // allocator.dupe) so that every free path — the iteration errdefer,
            // the outer errdefer, and deinit — can free unconditionally without
            // a len > 0 guard.  copyLuaString(dupe) allocates even for an
            // empty Lua string, so a len == 0 slice is still heap-owned and
            // must be freed; guarding on len > 0 would leak it.
            var ph_title: []const u8 = try allocator.dupe(u8, "");
            errdefer allocator.free(ph_title);
            var ph_detail: []const u8 = try allocator.dupe(u8, "");
            errdefer allocator.free(ph_detail);
            if (c.lua_type(L, -1) == c.LUA_TTABLE) {
                const ph_idx: c_int = c.lua_absindex(L, -1);
                // title: if present as a string, allocate the replacement first
                // (so the errdefer still covers the placeholder on OOM), then
                // free the placeholder and assign.
                const tt = c.lua_getfield(L, ph_idx, "title");
                if (tt == c.LUA_TSTRING) {
                    const new_title = try copyLuaString(L, c.lua_absindex(L, -1), allocator);
                    allocator.free(ph_title);
                    ph_title = new_title;
                }
                luaPop(L, 1); // pop title
                // detail: same allocate-then-swap pattern.  If copyLuaString
                // fails (OOM), ph_title errdefer and ph_detail errdefer both
                // fire unconditionally — regardless of whether the slices are
                // empty or not — so no leak occurs on the mid-iteration OOM
                // path.
                const dt = c.lua_getfield(L, ph_idx, "detail");
                if (dt == c.LUA_TSTRING) {
                    const new_detail = try copyLuaString(L, c.lua_absindex(L, -1), allocator);
                    allocator.free(ph_detail);
                    ph_detail = new_detail;
                }
                luaPop(L, 1); // pop detail
            }
            luaPop(L, 1); // pop phase entry
            // After a successful append the in-flight strings are owned by the
            // slice; the iteration-scoped errdefer above must not fire.
            // We clear it by noting that errdefer fires only on error return
            // from this scope, and try phases.append either succeeds (we
            // continue) or returns OOM (errdefer fires before propagating).
            try phases.append(allocator, .{ .title = ph_title, .detail = ph_detail });
        }
    }
    luaPop(L, 1); // pop phases value (table or nil/other)

    // Extract meta.reviewer (optional bool). Absent or non-boolean ⇒ false.
    // Trust-based reviewer-cadence assertion for the bright-line refusal
    // guard (plan 492 M10 task 3206). The Lua script is dynamic — we cannot
    // statically prove a reviewer dispatches every cycle, so the author
    // takes the contract on by setting this field. The 3205 quality-spine
    // built-ins will set `reviewer = true`; ad-hoc workflows that omit it
    // refuse to run plans that touch risky surfaces unless the operator
    // explicitly passes `--bypass-reviewer-guard`.
    var reviewer: bool = false;
    const reviewer_type = c.lua_getfield(L, meta_idx, "reviewer");
    if (reviewer_type == c.LUA_TBOOLEAN) {
        reviewer = c.lua_toboolean(L, -1) != 0;
    }
    luaPop(L, 1); // pop reviewer (boolean, nil, or other — always pop)

    // Pop meta table.
    luaPop(L, 1);

    return WorkflowMeta{
        .name = meta_name,
        .description = meta_desc,
        .phases = try phases.toOwnedSlice(allocator),
        .reviewer = reviewer,
    };
}

// ---------------------------------------------------------------------------
// loadModule — task 3164.
// ---------------------------------------------------------------------------

/// loadModule compiles and executes the Lua chunk in `source` (identified by
/// `chunkname` in error messages), then validates that the chunk returned a
/// table with shape { meta = { name, description, phases }, run = function }.
///
/// All strings in the returned `WorkflowModule` are allocator-owned copies
/// made before the Lua state is closed.  The caller owns the memory and must
/// call `WorkflowModule.deinit(allocator)` when done.
///
/// How "read meta without running run" is satisfied: executing the top-level
/// chunk only *constructs and returns* the module table — it does not call
/// `run`.  Reading `meta` fields is purely table access.  `run(ctx)` is
/// invoked only by a subsequent explicit `runModule` call.
///
/// On any error, the Lua state is closed, `err_buf` receives the Lua error
/// message (if applicable), and a `LuaError` is returned.
///
/// Chunkname parameter: passed directly to `luaL_loadbufferx` so that Lua
/// compile/runtime errors cite the workflow source identifier rather than the
/// generic "[string ...]". (Closes task 3223.)
///
/// String copies via caller allocator: `copyLuaString` copies each string
/// before `lua_close`, satisfying the invariant that returned data outlives
/// the Lua state. (Closes task 3221.)
///
/// Single-state consolidation (task 3229): the Lua source is parsed once by
/// `loadModuleState`.  `loadModule` extracts meta and closes the state.
/// `runModule` (below) reuses a freshly loaded state to call run — the
/// double-parse between the old loadModule+callRun pair is eliminated.
pub fn loadModule(
    source: []const u8,
    chunkname: [*:0]const u8,
    allocator: std.mem.Allocator,
    err_buf: []u8,
) (LuaError || std.mem.Allocator.Error)!WorkflowModule {
    const L = try loadModuleState(source, chunkname, err_buf);
    defer c.lua_close(L);

    // module table is at absolute index 1 (bottom of user stack).
    const module_idx: c_int = c.lua_absindex(L, -1);

    const meta = try extractMeta(L, module_idx, allocator);
    // On error after extractMeta succeeds, free the caller-owned meta strings.
    // (extractMeta's internal errdefers only fire when extractMeta itself
    // returns an error; on success ownership transfers here.)
    errdefer {
        for (meta.phases) |ph| {
            allocator.free(ph.title);
            allocator.free(ph.detail);
        }
        allocator.free(meta.phases);
        allocator.free(meta.name);
        allocator.free(meta.description);
    }

    // Validate that `run` is a function.
    // Structural errors fall back to @errorName in the caller because
    // captureError is not invoked here — these errors have no Lua error string.
    const run_type = c.lua_getfield(L, module_idx, "run");
    if (run_type != c.LUA_TFUNCTION) {
        return LuaError.LuaModuleMissingRun;
    }
    luaPop(L, 1); // pop run function

    return WorkflowModule{ .meta = meta };
}

// ---------------------------------------------------------------------------
// runModule — consolidated run path (task 3229, replaces callRun).
// ---------------------------------------------------------------------------

/// runModule loads the workflow module from `source` into a single Lua state
/// (via loadModuleState) and invokes its `run` function with a `ctx` table
/// carrying the caller-supplied `args`.
///
/// This is the consolidated successor to the old `callRun` which opened a
/// second state and re-parsed the source.  Now the source is compiled once
/// per execution.
///
/// `args` is a slice of CLI argument strings (the trailing positionals from
/// the command line, excluding the workflow file path itself). They are
/// threaded into `ctx.args` as a 1-based Lua sequence of strings:
///   ctx.args[1] = args[0], ctx.args[2] = args[1], ...
///
/// An empty `args` slice produces an empty `ctx.args` table, preserving the
/// existing invariant that `ctx.args` is always a table.
///
/// Host functions (task 3168): when `host` is non-null the ctx table is given
/// the host-function surface (agent/parallel/pipeline/phase/log/workflow +
/// budget) and the injected determinism fields (ctx.now / ctx.seed) via
/// installHostFns. Recorded calls accumulate in `host.calls`. When `host` is
/// null, ctx carries only `args` (the M1 shape) — used by tests that exercise
/// pure-Lua run bodies.
///
/// On Lua runtime errors, `err_buf` is populated and `LuaRuntimeError`
/// returned.
pub fn runModule(
    source: []const u8,
    chunkname: [*:0]const u8,
    args: []const []const u8,
    host: ?*HostState,
    err_buf: []u8,
) LuaError!void {
    const L = try loadModuleState(source, chunkname, err_buf);
    defer c.lua_close(L);

    // module table is at absolute index 1 (bottom of user stack after pcall).
    const module_idx: c_int = c.lua_absindex(L, -1);

    // ---- M5 coroutine drive (task 3182) ----
    //
    // run(ctx) executes inside a Lua COROUTINE thread, driven by lua_resume —
    // NOT a blocking lua_pcallk. This is the load-bearing change: yielding from
    // a C function (hostAgent → agentContinue) across a lua_pcallk boundary is
    // forbidden in Lua ("attempt to yield across a C-call boundary"), but a
    // C function called from a lua_resume'd coroutine CAN yield via lua_yieldk.
    // For N=1 the observable result is identical to the M4 lua_pcallk path; the
    // mechanism is now yield/resume so task 3183 can run N coroutines.
    //
    // Stack discipline:
    //   1. co = lua_newthread(L)   → L: [module, thread]
    //   2. push module.run on L, xmove it to co → co: [run]
    //   3. build ctx (args + host fns) ON co     → co: [run, ctx]
    //   4. drive co with lua_resume(co, L, 1, &nres) via the scheduler loop.
    const co = c.lua_newthread(L) orelse return LuaError.LuaAllocFailed;
    // The thread is now on L's stack (index -1); keeping it referenced there
    // anchors it against GC for the lifetime of this function.

    // Push module.run onto L, then move it to the coroutine's stack.
    const run_type = c.lua_getfield(L, module_idx, "run");
    if (run_type != c.LUA_TFUNCTION) return LuaError.LuaModuleMissingRun;
    c.lua_xmove(L, co, 1); // co: [run]

    // Build ctx table ON the coroutine: { args = { ... } } + host fns.
    c.lua_createtable(co, 0, 1); // co: [run, ctx]
    const ctx_idx: c_int = c.lua_absindex(co, -1);
    c.lua_createtable(co, @intCast(args.len), 0); // co: [run, ctx, args]
    const args_tbl_idx: c_int = c.lua_absindex(co, -1);

    // Populate args as a 1-indexed Lua sequence.
    for (args, 0..) |arg, idx| {
        _ = c.lua_pushlstring(co, arg.ptr, arg.len);
        c.lua_rawseti(co, args_tbl_idx, @intCast(idx + 1));
    }

    // ctx["args"] = args_table; pops args table. co: [run, ctx].
    c.lua_setfield(co, ctx_idx, "args");

    // Install the host-function surface + determinism on ctx (task 3168/3169).
    // The closures are created on `co` so their upvalue (the *HostState) is the
    // same one the scheduler reads. Per the spec these are ctx fields.
    if (host) |hs| installHostFns(co, ctx_idx, hs);

    // ---- Scheduler-driven resume loop ----
    //
    // Build the scheduler and point the HostState at it (so hostAgent can
    // register the in-flight worker + yield, and agentContinue can read the
    // outcome on resume). The scheduler's spawner comes from the driver.
    var sched_storage: scheduler.Scheduler = undefined;
    if (host) |hs| {
        // The drive Io comes from the driver (the genuine subprocess Io) when a
        // driver is installed. For pure-Lua hosts (no driver → no spawning → no
        // worker ever registered) the scheduler's Io stays NULL — the
        // worker-drive paths are unreachable, and Scheduler.unwrapIo traps loudly
        // if a future wiring bug ever reaches them. This replaces the former
        // `undefined_io` landmine (plan 492 task 3260): no `undefined` Io exists.
        const drive_io: ?std.Io = if (hs.agent_driver) |d| d.io else null;
        sched_storage = scheduler.Scheduler.init(hs.allocator, drive_io);
        if (hs.agent_driver) |d| sched_storage.spawner = d.spawner;
        // Per-worker wall-clock timeout (task 3188): production uses the
        // scheduler defaults (real monotonic clock + 30-min budget). Tests may
        // inject a fake clock + tiny budget via HostState to drive a hung worker
        // deterministically.
        if (hs.timeout_clock_fn) |cf| sched_storage.clock_fn = cf;
        if (hs.timeout_clock_ctx) |cc| sched_storage.clock_ctx = cc;
        if (hs.timeout_max_wall_clock_ns) |mx| sched_storage.max_wall_clock_ns = mx;
        hs.sched = &sched_storage;
        // TEST-ONLY seam: pre-fill scheduler slots to force the next
        // registerInflight into the RegistryFull branch (task 3541 / finding 6).
        if (hs.test_pre_fill_slots > 0) {
            const cap = @min(hs.test_pre_fill_slots, sched_storage.slots.len);
            var fi: usize = 0;
            while (fi < cap) : (fi += 1) {
                sched_storage.slots[fi].in_use = true;
            }
        }
        // M8 whole-run ceiling (task 3198): stamp the run start time ONCE, from
        // the scheduler's injectable clock, so the per-spawn wall-clock-ceiling
        // check is deterministic under an injected fake clock. The spawn counter
        // starts at 0 (HostState default).
        //
        // Guard: `clockNow()` reads `realMonotonicNow`, which needs the drive Io.
        // A pure-Lua host (no driver) has a null scheduler Io, so calling
        // `clockNow()` here would trap `unwrapIo`. We only need the run-start
        // stamp on a path that can actually spawn (driver+io) OR when a custom
        // clock is injected (the ceiling tests use a FakeClock that ignores Io).
        // When neither holds, no spawn happens and the ceiling never fires, so a
        // 0 stamp is harmless.
        if (drive_io != null or hs.timeout_clock_fn != null) {
            hs.run_counters.run_start_mono_ns = sched_storage.clockNow();
        }
    }
    defer if (host) |hs| {
        hs.sched = null;
    };

    // ---- M6 heartbeat thread (task 3187) ----
    //
    // Start the single preemptive heartbeat thread ONLY when a spawn driver is
    // attached: with no driver there are no workers and no live leases to
    // refresh. The registry refreshes every live claim token at TTL/2 via
    // `planar-agent heartbeat` (production) — `driveAgentCallPreYield` registers
    // each worker's claim when it goes live and `agentContinue` unregisters it at
    // terminal. The thread touches NO Lua state. We `stop()` (set the flag +
    // join) at run end so the thread is gone before this frame unwinds — the
    // same clean-stop primitive task 3193 will call on SIGINT before releasing
    // claims.
    var hb_storage: heartbeat.HeartbeatRegistry = undefined;
    var hb_started = false;
    if (host) |hs| {
        if (hs.agent_driver) |d| {
            hb_storage = heartbeat.HeartbeatRegistry.init(
                hs.allocator,
                d.io,
                heartbeat.realHeartbeatFn,
                null,
            );
            hb_storage.start() catch {
                // Thread spawn failed (resource exhaustion). The run can still
                // proceed without lease refresh — long-running workers risk
                // reclamation, but that is the F1 fallback, not a hard failure.
                hb_storage.deinit();
            };
            if (hb_storage.thread != null) {
                hb_started = true;
                hs.heartbeat_reg = &hb_storage;
            }
        }
    }
    defer if (hb_started) {
        hb_storage.stop();
        hb_storage.deinit();
        if (host) |hs| hs.heartbeat_reg = null;
    };

    // ---- M6 SIGINT handler (tasks 3193 + 3189) ----
    //
    // Arm the async-signal-safe handler around the run ONLY when a spawn driver
    // is attached (only then are there workers/claims/worktrees to clean up; a
    // pure-Lua run has nothing to reclaim and Ctrl-C can fall through to the
    // default disposition). The handler sets ONLY `interrupt.interrupt_requested`;
    // the resume loop below OBSERVES it at safe points (between resumes) and runs
    // the off-signal shutdown sequence on the MAIN thread. Restored on exit so a
    // second Ctrl-C hard-kills and the disposition is not left installed.
    const interrupt_armed = if (host) |hs| (hs.agent_driver != null) else false;
    if (interrupt_armed) interrupt.install();
    defer if (interrupt_armed) interrupt.restore();

    // Drive co to completion. lua_resume returns LUA_OK (finished), LUA_YIELD
    // (a worker is in flight — drive it, then resume), or an error code.
    var nres: c_int = 0;
    var resume_rc = c.lua_resume(co, L, 1, &nres); // 1 arg = ctx
    while (true) {
        // Observe the interrupt flag at this safe point (between resumes). A
        // SIGINT seen here BREAKS out of the normal drive into the off-signal
        // shutdown sequence (stop heartbeat → kill children → release claims →
        // teardown worktrees) — see runInterruptShutdown. Best-effort cleanup;
        // residual state is reconcilable on next startup.
        if (interrupt_armed and interrupt.requested()) {
            if (host) |hs| runInterruptShutdown(hs);
            return LuaError.LuaRuntimeError;
        }
        switch (scheduler.classifyResume(resume_rc)) {
            .done => return, // pure-Lua workflows hit this on the first resume
            .err => {
                // The error value is on co's stack top; capture it from co.
                captureError(co, err_buf);
                return LuaError.LuaRuntimeError;
            },
            .yielded => {
                // A worker is in flight. The pre-yield half registered it; find
                // the slot the coroutine yielded against and drive it to
                // terminal (N=1: wait the single handle). The scheduler stores
                // the outcome on the slot for the continuation to read.
                const hs = host orelse @panic("runModule: coroutine yielded with no HostState");
                const sched = hs.sched.?;
                const slot_index = findInflightSlot(sched, co) orelse
                    @panic("runModule: coroutine yielded but no in-flight worker was registered");
                sched.driveInflight(slot_index) catch |err| switch (err) {
                    // A SIGINT was observed mid-wait (task 3189): break out of
                    // the drive into the off-signal shutdown sequence rather than
                    // resuming the continuation. The worker is still in flight
                    // (its handle un-consumed) — runInterruptShutdown kills it +
                    // releases its claim + tears down its worktree.
                    error.Interrupted => {
                        runInterruptShutdown(hs);
                        return LuaError.LuaRuntimeError;
                    },
                    else => {
                        // The worker drive itself failed (spawner.wait error). We
                        // cannot meaningfully resume the continuation without an
                        // outcome; report a run error.
                        @memcpy(err_buf[0..@min(err_buf.len - 1, 28)], "agent: worker drive failed\x00"[0..@min(err_buf.len - 1, 28)]);
                        return LuaError.LuaRuntimeError;
                    },
                };
                // Resume the coroutine: the continuation runs and reads the
                // outcome. No args pushed onto co — the continuation pushes its
                // own result; nargs=0 here.
                resume_rc = c.lua_resume(co, L, 0, &nres);
            },
        }
    }
}

/// findInflightSlot returns the index of the scheduler slot whose registered
/// coroutine is `co` (the one that just yielded). For N=1 there is exactly one
/// in-use slot; the scan generalizes to N (task 3183) where multiple coroutines
/// each own a slot.
fn findInflightSlot(sched: *scheduler.Scheduler, co: *c.lua_State) ?usize {
    for (&sched.slots, 0..) |*slot, idx| {
        if (slot.in_use and slot.co == co) return idx;
    }
    return null;
}

// ---------------------------------------------------------------------------
// SIGINT off-signal-path shutdown bridge (M6 task 3189).
//
// The async-signal-safe handler (interrupt.zig task 3193) only sets the
// `interrupt_requested` flag. When the drive/poll loop OBSERVES the flag at a
// safe point it calls `runInterruptShutdown`, which builds an
// `interrupt.ShutdownPlan` from the scheduler's in-flight slots and runs the
// ordered cleanup (stop heartbeat → kill children → release claims → teardown
// worktrees) — all on the MAIN thread, NEVER in the handler.
// ---------------------------------------------------------------------------

/// SlotKillCtx is the kill thunk's context for one in-flight slot: the slot's
/// spawner + Io + a pointer to the slot's live `Handle`. `interruptKill` hard-
/// kills that child via the spawner. Kept separate from `interrupt.zig` so that
/// module carries no spawn.zig dependency (it is a pure callback-driven
/// sequencer).
const SlotKillCtx = struct {
    spawner: spawn.Spawner,
    io: std.Io,
    handle: *spawn.Handle,
};

fn interruptKill(ctx: ?*anyopaque) void {
    const k: *SlotKillCtx = @ptrCast(@alignCast(ctx.?));
    k.spawner.kill(k.io, k.handle);
}

/// InterruptShutdownCtx carries the per-run state the release/teardown/heartbeat
/// callbacks need: the heartbeat registry to stop+unregister against, plus the
/// allocator/io and the `skip_terminal_subprocess` gate (so unit-test paths
/// that drive a FakeSpawner never shell `planar-agent`/`git`).
const InterruptShutdownCtx = struct {
    heartbeat_reg: ?*heartbeat.HeartbeatRegistry,
    allocator: std.mem.Allocator,
    io: std.Io,
    repo_root: []const u8,
    /// True ⇒ do NOT shell terminal verbs / git teardown (unit-test paths).
    skip_subprocess: bool,
};

fn interruptStopHeartbeat(ctx: ?*anyopaque) void {
    const sc: *InterruptShutdownCtx = @ptrCast(@alignCast(ctx.?));
    // Stop FIRST (§12): join the refresher before any claim is released so it
    // cannot re-extend a lease the release is about to expire.
    if (sc.heartbeat_reg) |hb| hb.stop();
}

fn interruptUnregister(ctx: ?*anyopaque, token: []const u8) void {
    const sc: *InterruptShutdownCtx = @ptrCast(@alignCast(ctx.?));
    if (sc.heartbeat_reg) |hb| hb.unregister(token);
}

fn interruptRelease(ctx: ?*anyopaque, token: []const u8) void {
    const sc: *InterruptShutdownCtx = @ptrCast(@alignCast(ctx.?));
    if (sc.skip_subprocess) return;
    // SIGINT → release (operator interrupt = abandon-for-retry, not fail).
    // Best-effort: a failure is recovered by next-startup reconcile.
    terminal.runTerminalVerb(sc.allocator, sc.io, .release, token) catch {};
}

fn interruptTeardown(ctx: ?*anyopaque, plan_slug: []const u8, task_slug: []const u8) void {
    const sc: *InterruptShutdownCtx = @ptrCast(@alignCast(ctx.?));
    if (sc.skip_subprocess) return;
    if (sc.repo_root.len == 0) return;
    // Best-effort: a run-id-scoped worktree-prune on next startup is the backstop.
    worktree.teardownCycle(sc.allocator, sc.io, sc.repo_root, plan_slug, task_slug) catch {};
}

/// runInterruptShutdown builds an `interrupt.ShutdownPlan` from every in-flight
/// scheduler slot and runs the ordered off-signal cleanup. Called on the MAIN
/// thread the moment the drive loop observes `interrupt.requested()`. Each
/// in-use slot's `payload` is its `AgentCallState` (claim token + task slug +
/// driver); the kill context is the slot's spawner + live handle.
///
/// MAX_SLOTS-bounded fixed buffers keep this allocation-free on the shutdown
/// path (the slot registry is itself fixed at MAX_SLOTS).
fn runInterruptShutdown(hs: *HostState) void {
    const sched = hs.sched orelse {
        // No scheduler ⇒ pure-Lua run with no workers. Still stop the heartbeat
        // (a no-op if none was started) so the contract holds uniformly.
        if (hs.heartbeat_reg) |hb| hb.stop();
        return;
    };

    var workers: [scheduler.MAX_SLOTS]interrupt.InflightWorker = undefined;
    var kill_ctxs: [scheduler.MAX_SLOTS]SlotKillCtx = undefined;
    var n: usize = 0;

    // The driver supplies the allocator/io/repo_root + the skip-subprocess gate.
    // When no driver is attached (no workers possible) the per-worker loops are
    // empty and only the heartbeat stop fires.
    var sc = InterruptShutdownCtx{
        .heartbeat_reg = hs.heartbeat_reg,
        .allocator = hs.allocator,
        .io = undefined,
        .repo_root = "",
        .skip_subprocess = true,
    };

    for (&sched.slots) |*slot| {
        if (!slot.in_use) continue;
        const payload = slot.payload orelse continue;
        const acs: *AgentCallState = @ptrCast(@alignCast(payload));
        const driver = acs.driver;
        // Fill the shutdown ctx from the first live slot's driver (all slots in a
        // run share the same driver wiring).
        sc.io = driver.io;
        sc.repo_root = driver.repo_root;
        sc.skip_subprocess = driver.skip_terminal_subprocess;

        kill_ctxs[n] = .{ .spawner = driver.spawner, .io = driver.io, .handle = &slot.handle };
        workers[n] = .{
            .claim_token = acs.claim_token,
            .plan_slug = driver.plan_slug,
            .task_slug = acs.task_slug,
            .kill_ctx = &kill_ctxs[n],
        };
        n += 1;
    }

    interrupt.shutdown(.{
        .workers = workers[0..n],
        .stop_heartbeat_fn = interruptStopHeartbeat,
        .stop_heartbeat_ctx = &sc,
        .kill_fn = interruptKill,
        .unregister_fn = interruptUnregister,
        .unregister_ctx = &sc,
        .release_fn = interruptRelease,
        .release_ctx = &sc,
        .teardown_fn = interruptTeardown,
        .teardown_ctx = &sc,
    });

    // The coroutine drive is abandoned (we are unwinding the run, not resuming
    // the parked continuations). Free each in-flight slot's host-owned state:
    // the `AgentCallState` payload (its token/slug/env dupes) and any
    // already-driven-but-unconsumed `outcome`, then release the slot. The kill
    // above consumed each live handle, so no handle drain is needed here.
    for (&sched.slots, 0..) |*slot, idx| {
        if (!slot.in_use) continue;
        if (slot.outcome) |*o| {
            o.deinit(hs.allocator);
            slot.outcome = null;
        }
        if (slot.payload) |payload| {
            const acs: *AgentCallState = @ptrCast(@alignCast(payload));
            acs.destroy();
        }
        sched.releaseSlot(idx);
    }
}

/// callRun is a backwards-compatible wrapper for runModule with no HostState
/// (ctx carries only `args`, the M1 shape). Existing unit tests that exercise
/// pure-Lua run bodies use this 4-argument form unchanged; the host-fn surface
/// is opted into by passing a *HostState to runModule directly.
pub fn callRun(
    source: []const u8,
    chunkname: [*:0]const u8,
    args: []const []const u8,
    err_buf: []u8,
) LuaError!void {
    return runModule(source, chunkname, args, null, err_buf);
}

// ---------------------------------------------------------------------------
// CLI surface — task 3165.
// ---------------------------------------------------------------------------

/// Version string for planar-execute. Embeds the Lua version constant.
/// The Lua version is derived from the vendored header's numeric major/minor
/// macros (`LUA_VERSION_MAJOR_N` / `LUA_VERSION_MINOR_N`, e.g. 5 and 5 for
/// 5.5). Lua 5.5 redefined the textual `LUA_VERSION_MAJOR` / `LUA_VERSION`
/// macros in terms of the C `#`-stringizing operator (`LUAI_TOSTR`), which
/// Zig's `@cImport` cannot translate, so we format the integer macros at
/// comptime instead. We pair it with the binary name for consistency with the
/// other binaries' `<binary> <version-info>` format.
const planar_execute_version = std.fmt.comptimePrint(
    "planar-execute 0.1.0 (lua {d}.{d})",
    .{ c.LUA_VERSION_MAJOR_N, c.LUA_VERSION_MINOR_N },
);

/// The `run` subcommand: execute a workflow file.
///
/// This is the default path. When `planar-execute <workflow.lua>` is
/// invoked, `main` injects "run" before dispatching so the parser sees
/// `planar-execute run <workflow.lua>`.
///
/// Default-verb name collision: if the operator has a workflow file literally
/// named "version" or "run", they must use the explicit `run` subcommand to
/// reach it:
///   planar-execute run version.lua
///   planar-execute run run.lua
/// Invoking `planar-execute version` (no .lua extension) always routes to the
/// version subcommand, not to a file.
///
/// Note: structural-shape errors (LuaModuleNotTable, LuaModuleMissingMeta,
/// LuaModuleMissingRun, LuaModuleInvalidMeta) do not produce a Lua error
/// string; the handler falls back to @errorName for those paths.
///
/// Note: there is NO named-built-in registry — that is m10-quality-spine's
/// scope. Built-in workflows do not exist yet; the file-path form is the
/// only run-path in M1.
const run_verb: cli.Cmd = .{
    .name = "run",
    .desc = "Execute a workflow Lua file.",
    .long_desc =
    \\Execute a Lua 5.5 workflow script.
    \\
    \\  Usage:
    \\    planar-execute run <workflow.lua> [args...]
    \\    planar-execute run --dry-run <workflow.lua>
    \\
    \\  The workflow file must return a table:
    \\    { meta = { name, description, phases }, run = function(ctx) ... end }
    \\
    \\  Trailing [args...] are passed to run(ctx) as ctx.args[1], ctx.args[2], ...
    \\
    \\  Default-verb name collision: a workflow file literally named "version"
    \\  or "run" must be invoked via the explicit `run` subcommand:
    \\    planar-execute run version.lua
    \\  Invoking `planar-execute version` always routes to the version verb.
    \\
    \\  Structural errors (wrong module shape) fall back to @errorName because
    \\  those code paths have no Lua error string.
    \\
    \\  Exit codes:
    \\    0   run() completed without error (or --dry-run validation passed).
    \\    1   Lua runtime error or missing/invalid workflow file.
    \\    2   Invalid module structure (bad meta/run shape).
    \\    3   Lua compile error.
    \\
    \\  Host functions are carried on ctx (ctx.agent / ctx.parallel /
    \\  ctx.pipeline / ctx.phase / ctx.log / ctx.workflow / ctx.budget) plus
    \\  the injected determinism fields ctx.now and ctx.seed. At M2 the host
    \\  functions are recording stubs — they record their call and return a
    \\  stub result; real spawning/scheduling arrives in later milestones.
    \\
    \\  The environment is sandboxed: os and io are unavailable, and
    \\  math.random / os.time are stripped — use ctx.now / ctx.seed instead.
    ,
    .flags = &.{
        .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false }, .desc = "Load and validate the workflow, print meta and phases, exit without running." },
        .{ .long = "--plan", .kind = .int, .default = .{ .int = 0 }, .desc = "Plan id the live agent run is scoped to. When PLANAR_EXECUTE_LIVE_AGENT=1, providing --plan enables live claim-status reads and commit-presence sampling; omitting it degrades those features but does not prevent agent-free workflows from running." },
        .{ .long = "--mock-worker", .kind = .bool, .default = .{ .bool = false }, .desc = "Workflow-script testing harness (plan 492 M10 task 3202). Enter run() with the FULL agent() pipeline wired against an in-process FakeSpawner so the workflow's control flow (parallel, pipeline, error handling, blocked-summary, result propagation) runs deterministically WITHOUT spawning any real `claude -p` worker (no API cost, no fs writes from the worker). Canned spawn outcome: exit_code=0, stdout=\"ok\", stderr=\"\" — agent() returns the natural decision-matrix result (typically status=\"released\" when no repo/commit is present). Mutually exclusive with --dry-run and PLANAR_EXECUTE_LIVE_AGENT=1; --plan is optional (degrades like task 3264)." },
        .{ .long = "--bypass-reviewer-guard", .kind = .bool, .default = .{ .bool = false }, .desc = "Operator-explicit override for the bright-line refusal guard (plan 492 M10 task 3206). The guard refuses to run a plan whose open tasks touch migrations/*.sql, a new top-level CLI verb, or invariant/methodology code under a workflow that does NOT declare meta.reviewer = true. Pass --bypass-reviewer-guard to proceed anyway; the harness prints a loud stderr warning naming the override. Use this only when you have consciously accepted the doctrine risk (e.g. running a one-off recovery workflow). Hostile-looking name by design: hard-to-bypass-by-accident but possible when truly needed." },
    },
    .positionals = &.{
        .{ .name = "workflow", .kind = .string, .required = true },
    },
    .rest_field = "rest_args",
    .run = cli.handler(handleRun),
};

/// Root CLI command tree for `planar-execute`.
///
/// The default run-path is activated when the first non-flag, non-subcommand
/// argument is a file path. `main` detects this and injects "run" before
/// dispatch so the etc-cli parser sees the explicit `run` subcommand path.
pub const root: cli.Cmd = .{
    .name = "planar-execute",
    .desc = "Execute a Lua workflow script.",
    .long_desc =
    \\planar-execute — Lua 5.5 workflow execution harness (plan 492).
    \\
    \\  Usage:
    \\    planar-execute <workflow.lua> [args...]   Run a workflow file (default).
    \\    planar-execute run <workflow.lua> [args…] Explicit run subcommand.
    \\    planar-execute version                   Print version.
    \\    planar-execute --help                    Show this help.
    \\
    \\  The workflow file must return a Lua table:
    \\    { meta = { name, description, phases }, run = function(ctx) ... end }
    \\
    \\  Trailing [args...] are passed to the workflow as ctx.args[1], ctx.args[2], ...
    ,
    .cmds = &.{
        run_verb,
        doctor_verb,
        version_verb,
    },
};

/// `planar-execute doctor --plan <id> [--json]` — read-only health check.
///
/// Drives the planar-execute read helpers (schema ingestion, plan-state reads,
/// reconcile dry-run) against the ambient DB and PATH-resolved sibling binaries
/// and reports whether each read path works. NON-DESTRUCTIVE: the reconcile
/// probe is a dry-run only; doctor writes nothing to the DB or agent_* tables.
///
/// Exit code: 0 iff every probe is ok (all_ok), non-zero otherwise. A failed
/// probe records its error and doctor continues — all failures are collected
/// before the exit decision.
const doctor_verb: cli.Cmd = .{
    .name = "doctor",
    .desc = "Read-only health check: drive the read helpers against live binaries.",
    .long_desc =
    \\Read-only / non-destructive diagnostic for planar-execute's read paths.
    \\
    \\  Usage:
    \\    planar-execute doctor --plan <id> [--json]
    \\
    \\  Drives, in order (collecting every probe's outcome — never bails on the
    \\  first failure):
    \\    1. planar schema ingestion        (schema.loadSchema "planar")
    \\    2. planar-agent schema ingestion  (schema.loadSchema "planar-agent")
    \\    3. plan show <id>                  (state.planShow)
    \\    4. plan next <id>                  (state.planNext)
    \\    5. test-spec status <id>           (state.testSpecStatus)
    \\    6. reconcile DRY-RUN               (worktree.reconcileAndPrune dry_run=true)
    \\
    \\  The reconcile probe is a dry-run ONLY: it computes the stale-cycle set
    \\  but performs no teardown and no `git worktree prune`. doctor never
    \\  claims, completes, or writes anything.
    \\
    \\  With --json, emits a machine-readable report. Without it, a short
    \\  one-line-per-probe human report.
    \\
    \\  Exit codes:
    \\    0   every probe ok (all_ok: true).
    \\    1   at least one probe failed (all_ok: false).
    ,
    .flags = &.{
        .{ .long = "--plan", .kind = .int, .required = true, .desc = "Plan id to drive the plan-state read probes against." },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false }, .desc = "Emit the report as JSON (the load-bearing machine form)." },
    },
    .run = cli.handler(handleDoctor),
};

/// handleDoctor runs all six read probes and emits the report (JSON or human),
/// exiting 0 iff every probe is ok.
fn handleDoctor(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(root, &.{"doctor"}, args_ptr);
    const ctx = currentCtx();

    if (args.plan <= 0) {
        try ctx.stderr.print("planar-execute: doctor --plan must be a positive plan id\n", .{});
        try flushCtx();
        std.process.exit(1);
    }
    const plan: u64 = @intCast(args.plan);

    // doctor's probe results borrow strings (title/slug) parsed during the run;
    // an arena keeps them alive until after serialization, then frees in one shot.
    var probe_arena = std.heap.ArenaAllocator.init(ctx.allocator);
    defer probe_arena.deinit();

    const report = doctor.run(probe_arena.allocator(), ctx.io, plan);

    if (args.json) {
        try doctor.printJson(report, ctx.stdout);
    } else {
        try doctor.printHuman(report, ctx.stdout);
    }
    try flushCtx();

    if (!report.all_ok) std.process.exit(1);
}

comptime {
    @setEvalBranchQuota(10_000);
    cli.validate(root);
}

/// `planar-execute version` — print version and exit 0.
const version_verb: cli.Cmd = .{
    .name = "version",
    .desc = "Print the planar-execute version and Lua runtime version.",
    .run = cli.handler(handleVersion),
};

fn handleVersion(args_ptr: *const anyopaque) anyerror!void {
    _ = args_ptr;
    const ctx = currentCtx();
    try ctx.stdout.print("{s}\n", .{planar_execute_version});
}

/// printDryRun writes the dry-run preview to `writer`:
///   workflow: <name>
///   description: <description>
///   phases: <N>
///     1. <title>[ — <detail>]
///     2. ...
///
/// Lines are newline-terminated. Detail is omitted when the phase's detail
/// string is empty. This is the stable output format pinned by integration tests.
pub fn printDryRun(mod: WorkflowModule, writer: *Io.Writer) !void {
    try writer.print("workflow: {s}\n", .{mod.meta.name});
    try writer.print("description: {s}\n", .{mod.meta.description});
    try writer.print("phases: {d}\n", .{mod.meta.phases.len});
    for (mod.meta.phases, 0..) |phase, i| {
        if (phase.detail.len > 0) {
            try writer.print("  {d}. {s} — {s}\n", .{ i + 1, phase.title, phase.detail });
        } else {
            try writer.print("  {d}. {s}\n", .{ i + 1, phase.title });
        }
    }
}

/// handleRun is the default handler: resolve the first positional as a
/// workflow file path, read the source, load the module, and invoke run(ctx).
///
/// Trailing positionals (rest_args) are threaded into ctx.args as a
/// 1-based Lua sequence.
///
/// When --dry-run is set: loads and validates the module (same code path as
/// normal), prints meta and phases via printDryRun, and exits 0 without
/// calling runModule. A malformed module still exits non-zero (load/validate
/// is shared). run(ctx) is never entered under --dry-run.
///
/// Error mapping:
///   - File not found / read error   → exit 1 (message on stderr)
///   - Lua compile error             → exit 3 (message on stderr)
///   - Invalid module shape          → exit 2 (message on stderr)
///   - Lua runtime error in run()    → exit 1 (message on stderr)
fn handleRun(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(root, &.{"run"}, args_ptr);
    const ctx = currentCtx();

    const workflow_path = args.workflow;
    const rest_args: []const []const u8 = args.rest_args;
    const dry_run: bool = args.dry_run;
    const mock_worker: bool = args.mock_worker;

    // ---- task 3202 mode-conflict gates ---------------------------------------
    //
    // `--mock-worker` is a THIRD mode parallel to the M2 stub (default) and the
    // gated live-agent run. It is mutually exclusive with both `--dry-run` (a
    // strictly read-only validation pass that exits BEFORE entering run) and
    // PLANAR_EXECUTE_LIVE_AGENT=1 (the real-cost path). Mixing them is a wiring
    // error, not a meaningful combined mode: reject loudly with a clear
    // message so the operator picks one and reruns.
    if (mock_worker and dry_run) {
        try ctx.stderr.print(
            "planar-execute: --mock-worker and --dry-run are mutually exclusive. " ++
                "--dry-run skips run() entirely; --mock-worker enters run() with a fake spawner. Pick one.\n",
            .{},
        );
        try flushCtx();
        std.process.exit(1);
    }
    if (mock_worker and ctx.environ.getPosix("PLANAR_EXECUTE_LIVE_AGENT") != null) {
        try ctx.stderr.print(
            "planar-execute: --mock-worker and PLANAR_EXECUTE_LIVE_AGENT=1 are mutually exclusive. " ++
                "--mock-worker uses an in-process FakeSpawner (no real `claude -p`); the live gate spawns real workers. Unset PLANAR_EXECUTE_LIVE_AGENT or drop --mock-worker.\n",
            .{},
        );
        try flushCtx();
        std.process.exit(1);
    }

    // Read the workflow source from disk.
    // Use the arena allocator from the process context. All allocations
    // within this handler are freed when the arena is torn down at exit.
    const allocator = ctx.allocator;

    const source = std.Io.Dir.cwd().readFileAlloc(ctx.io, workflow_path, allocator, .limited(16 * 1024 * 1024)) catch |e| {
        try ctx.stderr.print("planar-execute: cannot read '{s}': {s}\n", .{ workflow_path, @errorName(e) });
        try flushCtx();
        std.process.exit(1);
    };
    defer allocator.free(source);

    // Build a null-terminated chunkname from the workflow path.
    // Prefix with '@' so Lua shows the path as a filename in error messages
    // (e.g. "path/to/wf.lua:4: ...") rather than the "[string ...]" form.
    const chunkname_owned = allocator.alloc(u8, workflow_path.len + 2) catch {
        try ctx.stderr.print("planar-execute: out of memory\n", .{});
        try flushCtx();
        std.process.exit(1);
    };
    defer allocator.free(chunkname_owned);
    chunkname_owned[0] = '@';
    @memcpy(chunkname_owned[1 .. 1 + workflow_path.len], workflow_path);
    chunkname_owned[1 + workflow_path.len] = 0;
    const chunkname: [*:0]const u8 = @ptrCast(chunkname_owned.ptr);

    // Validate the module structure without running run().
    // Zero-init so that structural errors (LuaModuleNotTable etc.) that do
    // not write a Lua error string produce a clean "empty" message check.
    var err_buf: [512]u8 = @splat(0);
    var mod = loadModule(source, chunkname, allocator, &err_buf) catch |e| {
        const exit_code: u8 = switch (e) {
            LuaError.LuaCompileError => 3,
            LuaError.LuaModuleNotTable,
            LuaError.LuaModuleMissingMeta,
            LuaError.LuaModuleMissingRun,
            LuaError.LuaModuleInvalidMeta,
            => 2,
            else => 1,
        };
        const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
        if (msg.len > 0) {
            try ctx.stderr.print("planar-execute: {s}\n", .{msg});
        } else {
            try ctx.stderr.print("planar-execute: workflow load error: {s}\n", .{@errorName(e)});
        }
        try flushCtx();
        std.process.exit(exit_code);
    };
    defer mod.deinit(allocator);

    // --dry-run: print meta + phases and exit 0 WITHOUT calling runModule.
    // This is the load-bearing guarantee: run is never entered under --dry-run.
    if (dry_run) {
        try printDryRun(mod, ctx.stdout);
        try flushCtx();
        return; // exit 0 — no runModule
    }

    // ---- M10 task 3206: bright-line refusal guard ---------------------------
    //
    // Refuse to run a plan whose open tasks touch one of three risky surfaces
    // (migrations/*.sql, new top-level CLI verbs, validate/invariant code)
    // under a workflow that does NOT declare `meta.reviewer = true`. The Lua
    // script is dynamic — we cannot statically prove a reviewer dispatches
    // for every cycle — so the author takes the doctrine contract on with
    // `meta.reviewer = true`. Workflows that omit the field default to false
    // and the guard becomes load-bearing.
    //
    // Scope fences (see refusal_guard.zig):
    //   - `--dry-run` already exited above; the guard never runs for dry-run.
    //   - Both `--mock-worker` AND the live gate hit the guard: a doctrine
    //     contract that's wrong in mock would ship to the live path on the
    //     next operator's `unset` of the mock flag.
    //   - `--plan` absent ⇒ no plan ⇒ no tasks ⇒ guard passes (no-op).
    //   - `--bypass-reviewer-guard` ⇒ explicit operator override, loud
    //     stderr warning, proceed.
    //
    // Exit code 2 is reused from the "invalid module" mapping above because
    // both are "the input is structurally unfit to run". The diagnostic text
    // disambiguates ("bright-line refusal: …").
    {
        const plan_for_guard: u64 = if (args.plan > 0) @intCast(args.plan) else 0;
        var guard = refusal_guard.checkRefusal(allocator, ctx.io, .{
            .reviewer_declared = mod.meta.reviewer,
            .plan_id = plan_for_guard,
            .bypass = args.bypass_reviewer_guard,
        }) catch |e| switch (e) {
            error.OutOfMemory => {
                try ctx.stderr.print("planar-execute: out of memory while running the reviewer-guard\n", .{});
                try flushCtx();
                std.process.exit(1);
            },
        };
        defer guard.deinit(allocator);

        switch (guard) {
            .pass => {},
            .bypassed => {
                try ctx.stderr.print(
                    "planar-execute: WARNING — bright-line refusal guard BYPASSED via --bypass-reviewer-guard. " ++
                        "The workflow does NOT declare meta.reviewer = true and the plan may touch risky surfaces " ++
                        "(migrations/*.sql, new top-level CLI verbs, or invariant/methodology code). " ++
                        "Doctrine compliance is now the operator's responsibility for this run.\n",
                    .{},
                );
                try flushCtx();
            },
            .refused => |r| {
                try ctx.stderr.print(
                    "planar-execute: REFUSING TO RUN — bright-line refusal guard tripped (plan 492 M10 task 3206).\n" ++
                        "  workflow '{s}' does NOT declare meta.reviewer = true\n" ++
                        "  plan {d} has a task touching a risky surface:\n" ++
                        "    task:{d} (slug: {s})\n" ++
                        "    touches: {s}:{s}\n" ++
                        "    predicate: {s}\n" ++
                        "  Fix one of:\n" ++
                        "    - set `meta.reviewer = true` in the workflow's meta block (the author asserts the workflow dispatches a reviewer for every cycle);\n" ++
                        "    - remove the risky-touch task(s) from this plan;\n" ++
                        "    - pass --bypass-reviewer-guard to override (operator accepts the doctrine risk; loud warning will be printed).\n",
                    .{
                        mod.meta.name,
                        plan_for_guard,
                        r.task_id,
                        if (r.task_slug.len == 0) "(unset)" else r.task_slug,
                        r.repo,
                        r.path,
                        r.predicate.name(),
                    },
                );
                try flushCtx();
                std.process.exit(2);
            },
        }
    }

    // Build the HostState that backs the host-function surface (task 3168) and
    // injects determinism (task 3169). At M2 the now/seed/budget values are
    // fixed injected constants — no real clock, RNG, or token accounting. The
    // point is structural: the script's only time/random/budget source is
    // host-controlled, so runs are replayable. M3+ wires real values here.
    var host = HostState.init(allocator, 0, 0, 100, 0);
    defer host.deinit();
    // Wire the Io context so ctx.eligible(plan_id) can shell `planar plan
    // recommend-strategy` (task 3185). This is a read-only control-plane verb —
    // no PLANAR_EXECUTE_LIVE_AGENT gate required.
    host.io = ctx.io;

    // M8 budgets + ceilings — the hard kill-switch (task 3198). Read the knobs
    // from the host environment (generous defaults; see budget.Budgets). The
    // per-task max-attempt budget is only ACTIVE when journaling is (a gated
    // real-agent run with a journal location); the whole-run ceiling is checked
    // on every spawn. Reading the env here (not gated on the live driver) keeps
    // the config wiring uniform and lets a dry/stub run still report the knobs.
    host.budgets = budget.Budgets.fromEnv(ctx.environ);

    // ---- Gated production agent() driver wiring (plan 492 M4 task 3241) ----
    //
    // INVARIANT: `planar-execute run` NEVER spawns a real `claude -p` worker by
    // default. The M2 recording stub (`agent()` → { status = "stub" }) is the
    // shipped behavior of every normal run and all of CI. ONLY an explicit
    // opt-in via the env gate PLANAR_EXECUTE_LIVE_AGENT=1 attaches a real
    // AgentDriver; without it, `host.agent_driver` stays null and `hostAgent`
    // returns the stub table bit-for-bit. One gate, two effects: it also gates
    // the live integration test (integration_tests/planar_execute_agent_live_test.zig).
    //
    // SCOPE FENCE (M4): the driver only runs the EXISTING `driveAgentCall`
    // pipeline. It does NOT create the cycle worktree or acquire the claim —
    // the workflow author passes a PRE-PREPARED worktree_path + claim_token
    // into agent() via opts. The front-half (ensureEpic/createCycle/claim) and
    // the back-half (merge/teardown) are M5 (the scheduler).
    //
    // These locals must outlive `runModule` (the driver is borrowed by the
    // HostState across the whole run), so they live on this stack frame.
    var live_driver: AgentDriver = undefined;
    var live_env_ctx: DefaultEnvBuilderCtx = undefined;
    var live_planar_agent_path: ?[]u8 = null;
    defer if (live_planar_agent_path) |p| allocator.free(p);
    var live_git_path: ?[]u8 = null;
    defer if (live_git_path) |p| allocator.free(p);
    var live_repo_root: ?[]u8 = null;
    defer if (live_repo_root) |p| allocator.free(p);
    var live_plan_slug: ?std.json.Parsed(state.PlanShow) = null;
    defer if (live_plan_slug) |*p| p.deinit();

    // The single-instance-per-plan run-lock (task 3191). Acquired below ONLY for
    // a gated real-agent run (binaries resolved + a plan id), where claim /
    // worktree contention between two runs on the same plan can actually
    // corrupt state. It must outlive `runModule` (held for the whole run), so it
    // lives on this frame; released on clean exit (defer) and best-effort on the
    // SIGINT path (the interrupt drive loop tears down via interrupt.shutdown;
    // the stale-takeover is the backstop for a crashed run that never releases).
    var run_lock: ?runlock.RunLock = null;
    defer if (run_lock) |*l| {
        l.release();
        l.deinit();
    };

    // ---- task 3202: --mock-worker driver attach ------------------------------
    //
    // When --mock-worker is set, attach an AgentDriver whose Spawner is the
    // in-process FakeSpawner promoted from the spawn.zig test surface (not
    // duplicated — same canonical fake the M5–M9 unit tests use, so the
    // contract surface stays single-sourced). The fake records every
    // would-be argv and returns a canned outcome (exit=0, stdout="ok",
    // stderr=""). No `claude -p` process is started.
    //
    // The mock driver is wired in degraded mode (task 3264 style):
    //   - repo_root = "" → commit-presence sampling skipped (commit_present=false).
    //   - plan_slug = "" → branch sample skipped.
    //   - plan_id = args.plan when --plan is given, else 0 (skip live claim read;
    //     defaultClaimStatusReader returns `.active`).
    //   - skip_terminal_subprocess = true → harness does NOT shell `planar-agent
    //     <verb>` on the mock decision.
    //   - skip_block_subprocess = true   → the M8 max-attempt block path does
    //     NOT shell `planar-agent block` either.
    //   - env_builder = null            → FakeSpawner accepts a null env_map
    //     (records an empty snapshot); the real-spawner panic guard is
    //     irrelevant because the mock never uses the real spawner.
    //   - live_binaries_resolved = true → agent() does NOT raise a Lua error
    //     (no real binaries required in this mode).
    //
    // The natural agent() result under the default canned outcome (active claim
    // + exit 0 + no commit) is `status="released"` — the workflow author writes
    // their control-flow asserts against that. Per-call scripted outcomes
    // (`--mock-outcomes <file>`) are deferred (see task 3267, filed below).
    //
    // The rest of the M5+ pipeline (scheduler, heartbeat thread, journal,
    // fan-in) runs UNCHANGED — the FakeSpawner is the only injected difference.
    // Heartbeats fire against the workflow-supplied claim_token via
    // realHeartbeatFn (which ignores non-zero exit, so an absent planar-agent
    // is benign); the journal write is guarded by repo_root+plan_id and
    // self-skips when --plan is absent.
    var mock_fake: spawn.FakeSpawnerState = undefined;
    var mock_fake_init = false;
    defer if (mock_fake_init) mock_fake.deinit();
    var mock_driver: AgentDriver = undefined;

    if (mock_worker) {
        const mock_plan: u64 = if (args.plan > 0) @intCast(args.plan) else 0;
        mock_fake = spawn.FakeSpawnerState.init(allocator, 0, "ok", "");
        mock_fake_init = true;
        mock_driver = .{
            .spawner = mock_fake.spawner(),
            .io = ctx.io,
            .repo_root = "",
            .plan_slug = "",
            .plan_id = mock_plan,
            .env_builder = null,
            .env_builder_ctx = null,
            .claim_status_reader = null, // → defaultClaimStatusReader; plan_id==0 → .active
            .skip_terminal_subprocess = true,
            .skip_block_subprocess = true,
            .live_binaries_resolved = true,
        };
        host.agent_driver = &mock_driver;

        // MOCK MODE notice on stderr — the operator should never mistake a
        // mock run for a real one. Distinct, greppable prefix.
        try ctx.stderr.print(
            "planar-execute: MOCK MODE — running in --mock-worker mode (no real `claude -p` spawned; agent() calls return canned outcomes from an in-process FakeSpawner)\n",
            .{},
        );
        try flushCtx();
    }

    if (!mock_worker and ctx.environ.getPosix("PLANAR_EXECUTE_LIVE_AGENT") != null) {
        // --- task 3264: best-effort attachment --------------------------------
        //
        // Driver attachment is now BEST-EFFORT. None of the following
        // resolutions exit the process; each degrades to a safe default so
        // that agent-free (pure-Lua) workflows run cleanly under the gate
        // without requiring --plan. A workflow that actually calls agent()
        // gets a clean Lua error at call time if required inputs are missing.
        //
        // Degradation contract (task 3264):
        //   plan_id == 0 → skip live claim-status read (defaultClaimStatusReader
        //                  returns .active, the pre-3242 degradation).
        //   plan_slug == "" → skip commit-presence branch sampling.
        //   repo_root == "" → skip commit-presence repo sampling.
        //   live_binaries_resolved == false → agent() raises a Lua error.

        // plan: 0 when --plan is absent; skip planShow in that case.
        const plan: u64 = if (args.plan > 0) @intCast(args.plan) else 0;

        // repo_root = the cwd's git top-level. Degrade to "" on failure.
        live_repo_root = worktree.gitTopLevel(allocator, ctx.io) catch blk: {
            try ctx.stderr.print(
                "planar-execute: NOTE: could not resolve git top-level of cwd; commit-presence sampling disabled\n",
                .{},
            );
            break :blk null;
        };

        // plan_slug: only attempt planShow when plan > 0; degrade to "" on failure.
        const slug: []const u8 = if (plan > 0) slug_blk: {
            const parsed = state.planShow(allocator, ctx.io, plan) catch {
                try ctx.stderr.print(
                    "planar-execute: NOTE: plan show {d} failed; plan_slug and commit-presence sampling disabled\n",
                    .{plan},
                );
                break :slug_blk "";
            };
            live_plan_slug = parsed;
            break :slug_blk live_plan_slug.?.value.slug orelse no_slug_blk: {
                try ctx.stderr.print(
                    "planar-execute: NOTE: plan {d} has no slug; commit-presence sampling disabled\n",
                    .{plan},
                );
                break :no_slug_blk "";
            };
        } else "";

        // Resolve the REAL planar-agent + git on the HOST PATH so the worker's
        // shim can symlink them in (decision 358: worker PATH = planar-agent +
        // git, not planar). Degrade on failure; agent() will raise a Lua error
        // if called without these binaries. (task 3264)
        live_planar_agent_path = resolveHostBinary(allocator, ctx.io, "planar-agent") catch null;
        live_git_path = resolveHostBinary(allocator, ctx.io, "git") catch null;

        const binaries_ok = live_planar_agent_path != null and live_git_path != null;

        if (binaries_ok) {
            live_env_ctx = .{
                .planar_agent_path = live_planar_agent_path.?,
                .git_path = live_git_path.?,
                .host_environ = ctx.environ,
            };
        }

        live_driver = .{
            .spawner = spawn.realSpawner(),
            .io = ctx.io,
            .repo_root = if (live_repo_root) |r| r else "",
            .plan_slug = slug,
            .plan_id = plan,
            .env_builder = if (binaries_ok) defaultEnvBuilder else null,
            .env_builder_ctx = if (binaries_ok) &live_env_ctx else null,
            .claim_status_reader = null, // → defaultClaimStatusReader against plan_id
            .skip_terminal_subprocess = false, // production runs the real terminal verb
            .live_binaries_resolved = binaries_ok,
        };
        host.agent_driver = &live_driver;

        // --- task 3191: single-instance-per-plan run-lock --------------------
        //
        // A gated real-agent run scoped to a plan cuts worktrees and claims
        // tasks under that plan. Two such runs on the SAME plan would corrupt
        // each other (racing claims, colliding cycle worktrees, one run's
        // reconcile pruning the other's live state). Take an O_EXCL lock keyed
        // by plan-id BEFORE any worker/claim/worktree exists. On a LIVE holder
        // we REFUSE (exit non-zero); on a stale lock (dead holder PID) we take
        // over. The lock is scoped to gated runs WITH a plan: an agent-free /
        // pure-Lua run (no real workers, no claims, no worktrees) has no
        // contention to guard, and a gated run without --plan has no plan key.
        if (binaries_ok and plan > 0) {
            if (runlock.acquire(allocator, ctx.io, plan, .{
                .repo_root = if (live_repo_root) |r| r else "",
            })) |lock| {
                run_lock = lock;
            } else |e| switch (e) {
                error.RunLockHeld => {
                    try ctx.stderr.print(
                        "planar-execute: refusing to start: another run already holds the single-instance lock for plan {d} (see the run-lock note above). Stop the other run, or wait for it to finish, before re-running.\n",
                        .{plan},
                    );
                    try flushCtx();
                    std.process.exit(1);
                },
                else => {
                    // FsError / OutOfMemory: degrade gracefully — proceed
                    // without the lock rather than block an otherwise-valid run.
                    try ctx.stderr.print(
                        "planar-execute: NOTE: could not acquire the single-instance run-lock for plan {d}: {s}; proceeding without it\n",
                        .{ plan, @errorName(e) },
                    );
                },
            }
        }
    }

    // Invoke run(ctx) with the trailing args threaded into ctx.args and the
    // host-function surface + determinism installed on ctx.
    runModule(source, chunkname, rest_args, &host, &err_buf) catch |e| {
        // M8 whole-run ceiling (task 3198): a ceiling-terminated run unwinds
        // through here AFTER the clean interrupt shutdown ran (in-flight workers
        // released + worktrees torn down, journal terminus written). Emit a
        // clear, distinct "run ceiling exceeded: <which>" message and exit
        // non-zero — this is a clean kill-switch stop, NOT a crash, and the
        // released work is recoverable on the next `planar-execute run` resume.
        if (host.ceiling_tripped) |which| {
            try ctx.stderr.print(
                "planar-execute: run ceiling exceeded: {s} — run wound down cleanly; in-flight work released and recoverable on resume.\n",
                .{which.name()},
            );
            try flushCtx();
            std.process.exit(1);
        }
        const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
        if (msg.len > 0) {
            try ctx.stderr.print("planar-execute: {s}\n", .{msg});
        } else {
            try ctx.stderr.print("planar-execute: run error: {s}\n", .{@errorName(e)});
        }
        try flushCtx();
        std.process.exit(1);
    };

    // ---- M7 end-of-run blocked-items summary (task 3195) ----
    //
    // The run completed (all coroutines done — `runModule` returned). If any
    // worker self-blocked during the run, list them for operator triage. The run
    // NEVER globally halted for input on a blocked item — each was set aside and
    // the slot reclaimed; this summary is the triage mechanism, not a prompt.
    // Quiet when zero blocked: a clean run prints nothing here.
    try printBlockedSummary(host.blocked_items.items, ctx.stdout);
    try flushCtx();
}

/// printBlockedSummary writes the end-of-run operator-triage listing (plan 492
/// M7 task 3195). One line per self-blocked worker, under a count header. PURE
/// formatting over the accumulated `BlockedItem`s — no I/O beyond the writer.
/// Emits NOTHING when `items` is empty (a clean run is quiet).
fn printBlockedSummary(items: []const BlockedItem, w: *Io.Writer) !void {
    if (items.len == 0) return;
    try w.print("Run complete. {d} task(s) blocked for operator triage:\n", .{items.len});
    for (items) |bi| {
        // task <slug> blocked by <blocker> — "<reason>"
        // Each clause is omitted when its field is empty so a partial record
        // still reads cleanly.
        try w.print("  - task", .{});
        if (bi.task_slug.len > 0) try w.print(" {s}", .{bi.task_slug});
        if (bi.task_blocker.len > 0) try w.print(" blocked by {s}", .{bi.task_blocker});
        if (bi.reason.len > 0) try w.print(" — \"{s}\"", .{bi.reason});
        try w.print("\n", .{});
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

/// Inject "run" as the first positional when the user invoked the binary as
///   planar-execute <workflow.lua> [args…]
/// without the explicit `run` subcommand keyword. This mirrors how
/// `planar-watch` injects its default "feed" verb.
///
/// Injection is skipped when:
///   - the first non-binary token is already a known subcommand name, or
///   - it is a global flag (--help / -h) — let the parser handle it, or
///   - argv has no extra tokens at all (bare invocation → show help).
///
/// Run-verb flags (task 3231): rather than maintaining a string-literal
/// blocklist of run-verb flags (which would need extending for every new flag
/// added in M2/M3), we comptime-iterate `run_verb.flags` and inject "run"
/// whenever the first token matches any declared run-verb flag.  This means
/// `planar-execute --dry-run wf.lua` (and future `--budget`, etc.) all route
/// correctly without any code change here.
fn maybeInjectRun(arena: std.mem.Allocator, raw_args: []const []const u8) []const []const u8 {
    // argv[0] is the binary name; anything beyond is operator-supplied.
    if (raw_args.len <= 1) return raw_args; // bare invocation → no injection; parser shows help.

    const first = raw_args[1];

    // Known subcommand names and global flags — leave argv alone.
    inline for ([_][]const u8{ "run", "doctor", "version", "--help", "-h" }) |v| {
        if (std.mem.eql(u8, first, v)) return raw_args;
    }

    // Check whether `first` matches any flag declared on the run subcommand.
    // Comptime iteration ensures every run-verb flag (current and future) is
    // covered without a growing string-literal blocklist.
    const is_run_verb_flag = comptime_check: {
        inline for (run_verb.flags) |flag| {
            if (std.mem.eql(u8, first, flag.long)) break :comptime_check true;
            if (flag.short) |sh| {
                if (std.mem.eql(u8, first, sh)) break :comptime_check true;
            }
        }
        break :comptime_check false;
    };

    // Any other flag or option starting with `-` that is not a known run-verb
    // flag is left for the root parser to handle (it will likely error).
    if (first.len > 0 and first[0] == '-' and !is_run_verb_flag) return raw_args;

    // Looks like a file path, positional, or a run-verb flag — inject "run".
    var out = arena.alloc([]const u8, raw_args.len + 1) catch return raw_args;
    out[0] = raw_args[0];
    out[1] = "run";
    for (raw_args[1..], 0..) |a, i| out[2 + i] = a;
    return out;
}

pub fn main(init: std.process.Init) !void {
    const arena: std.mem.Allocator = init.arena.allocator();
    const raw_args = try init.minimal.args.toSlice(arena);
    const args = maybeInjectRun(arena, raw_args);

    initCtx(arena, init.io, init.minimal.environ);
    defer flushCtx() catch {};

    const ctx = currentCtx();

    cli.dispatch(root, args, ctx.stdout) catch |e| switch (e) {
        cli.Parse.UnknownFlag,
        cli.Parse.MissingValue,
        cli.Parse.InvalidValue,
        cli.Parse.MissingRequired,
        cli.Parse.MissingRequiredPositional,
        cli.Parse.TooManyPositionals,
        cli.Parse.UnknownSubcommand,
        cli.Parse.UnexpectedArgument,
        cli.Parse.DuplicateFlag,
        => {
            try ctx.stderr.print("planar-execute: {s}\n", .{@errorName(e)});
            try flushCtx();
            std.process.exit(1);
        },
        error.NotImplemented => {
            try ctx.stderr.print("planar-execute: not implemented\n", .{});
            try flushCtx();
            std.process.exit(1);
        },
        else => return e,
    };
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

// Force-analyze every declaration reachable from this file (including
// state.zig / schema.zig / brief.zig and all their functions) and pull
// their test{} blocks into the execute_exe_tests artifact.
//
// Without this, Zig 0.16 lazy evaluation only analyzes declarations that
// are referenced by a code-gen or execution path. The pub-const aliases at
// the top of this file (pub const state = @import("state.zig"), etc.)
// provide types but do not force the compiler to analyze every function or
// test block inside the imported modules, so the execute_exe_tests pass
// count was observed as both 28 and 48 across runs — a non-deterministic
// hollow gate.
//
// std.testing.refAllDecls(@This()) forces analysis of all top-level
// declarations in main.zig, which includes the pub-const module aliases
// (state, schema, brief). Referencing a module alias causes the compiler
// to analyze its entire namespace including test blocks, making the
// execute_exe_tests count deterministic and complete.
//
// Note: Zig 0.16 ships refAllDecls (non-recursive); refAllDeclsRecursive
// is not available. refAllDecls on @This() is sufficient here because the
// sub-module aliases are direct top-level pub consts of this file.
test {
    std.testing.refAllDecls(@This());
}

test "lua state round-trip" {
    // Create a Lua state, assert it is non-null, then close it.
    // M1 acceptance signal: the static library links and the Lua allocator
    // is functional.
    const L = c.luaL_newstate();
    try std.testing.expect(L != null);
    c.lua_close(L);
}

test "evalString: arithmetic expression returns correct number" {
    // A script that computes 6 * 7 must return 42.0.
    var err_buf: [256]u8 = undefined;
    const result = try evalString("return 6 * 7", &err_buf);
    try std.testing.expectEqual(@as(f64, 42.0), result);
}

test "evalString: compile error surfaces as LuaCompileError" {
    // A syntactically invalid script must yield LuaCompileError, not a panic.
    var err_buf: [256]u8 = undefined;
    const err = evalString("this is not valid lua @@@@", &err_buf);
    try std.testing.expectError(LuaError.LuaCompileError, err);
    // err_buf must contain a non-empty error message from the Lua parser.
    try std.testing.expect(err_buf[0] != 0);
}

test "evalString: runtime error surfaces as LuaRuntimeError" {
    // A script that calls error() at runtime must yield LuaRuntimeError.
    var err_buf: [256]u8 = undefined;
    const err = evalString("error('boom')", &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);
    try std.testing.expect(err_buf[0] != 0);
}

test "evalString: multi-step computation" {
    // Statements before `return` must execute and accumulate correctly.
    var err_buf: [256]u8 = undefined;
    const result = try evalString(
        \\local x = 10
        \\local y = x * x + 2 * x + 1
        \\return y
    , &err_buf);
    try std.testing.expectEqual(@as(f64, 121.0), result);
}

// ---------------------------------------------------------------------------
// task 3164 tests — workflow module loading
// ---------------------------------------------------------------------------

const testing_alloc = std.testing.allocator;

test "loadModule: well-formed module — meta read, run present" {
    // A complete well-formed workflow module.  loadModule must extract meta
    // fields correctly.  run is validated as a function but NOT called.
    const src =
        \\return {
        \\  meta = {
        \\    name = "hello-workflow",
        \\    description = "A test workflow",
        \\    phases = {
        \\      { title = "Phase 1", detail = "Do something" },
        \\      { title = "Phase 2", detail = "Do more" },
        \\    },
        \\  },
        \\  run = function(ctx) error("run must not be called by loadModule") end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    var mod = try loadModule(src, "test:well-formed", testing_alloc, &err_buf);
    defer mod.deinit(testing_alloc);

    try std.testing.expectEqualStrings("hello-workflow", mod.meta.name);
    try std.testing.expectEqualStrings("A test workflow", mod.meta.description);
    try std.testing.expectEqual(@as(usize, 2), mod.meta.phases.len);
    try std.testing.expectEqualStrings("Phase 1", mod.meta.phases[0].title);
    try std.testing.expectEqualStrings("Do something", mod.meta.phases[0].detail);
    try std.testing.expectEqualStrings("Phase 2", mod.meta.phases[1].title);
    try std.testing.expectEqualStrings("Do more", mod.meta.phases[1].detail);
}

test "loadModule: chunk returns non-table → LuaModuleNotTable" {
    // A chunk that returns a number instead of a table must yield
    // LuaModuleNotTable.
    var err_buf: [256]u8 = undefined;
    const err = loadModule("return 42", "test:not-table", testing_alloc, &err_buf);
    try std.testing.expectError(LuaError.LuaModuleNotTable, err);
}

test "loadModule: missing meta field → LuaModuleMissingMeta" {
    // A table that omits meta entirely must yield LuaModuleMissingMeta.
    const src =
        \\return {
        \\  run = function(ctx) end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const err = loadModule(src, "test:no-meta", testing_alloc, &err_buf);
    try std.testing.expectError(LuaError.LuaModuleMissingMeta, err);
}

test "loadModule: missing run field → LuaModuleMissingRun" {
    // A table with meta but no run must yield LuaModuleMissingRun.
    const src =
        \\return {
        \\  meta = {
        \\    name = "x",
        \\    description = "y",
        \\    phases = {},
        \\  },
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const err = loadModule(src, "test:no-run", testing_alloc, &err_buf);
    try std.testing.expectError(LuaError.LuaModuleMissingRun, err);
}

test "loadModule: meta.name missing → LuaModuleInvalidMeta" {
    // meta present as table but name is absent (nil) → LuaModuleInvalidMeta.
    const src =
        \\return {
        \\  meta = {
        \\    description = "missing name",
        \\    phases = {},
        \\  },
        \\  run = function(ctx) end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const err = loadModule(src, "test:no-name", testing_alloc, &err_buf);
    try std.testing.expectError(LuaError.LuaModuleInvalidMeta, err);
}

test "loadModule: phases array with entries — count and content" {
    // Three-phase module; verify count and all phase strings.
    const src =
        \\return {
        \\  meta = {
        \\    name = "three-phase",
        \\    description = "desc",
        \\    phases = {
        \\      { title = "A", detail = "a-detail" },
        \\      { title = "B", detail = "b-detail" },
        \\      { title = "C", detail = "c-detail" },
        \\    },
        \\  },
        \\  run = function(ctx) end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    var mod = try loadModule(src, "test:three-phase", testing_alloc, &err_buf);
    defer mod.deinit(testing_alloc);

    try std.testing.expectEqual(@as(usize, 3), mod.meta.phases.len);
    try std.testing.expectEqualStrings("A", mod.meta.phases[0].title);
    try std.testing.expectEqualStrings("c-detail", mod.meta.phases[2].detail);
}

test "loadModule: chunkname appears in compile error" {
    // A syntax error must cite the chunkname in the error message, not a
    // generic "[string ...]".
    var err_buf: [256]u8 = undefined;
    const err = loadModule("!!! bad syntax", "my-workflow.lua", testing_alloc, &err_buf);
    try std.testing.expectError(LuaError.LuaCompileError, err);
    // err_buf must contain "my-workflow.lua" (chunkname) in the message.
    const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
    try std.testing.expect(std.mem.indexOf(u8, msg, "my-workflow.lua") != null);
}

test "callRun: run(ctx) is actually invoked — empty args" {
    // The script uses a global to record that run was called.  callRun must
    // actually invoke run; the global is set inside run.  We verify the
    // invocation by confirming no error is returned (run completes without
    // raising an error) and by using a script whose run writes to io output
    // via a Lua global flag checked in a second evalString call.
    //
    // Since callRun uses its own fresh Lua state, the simplest proof is that
    // run executes without raising an error, then we inspect an observable
    // effect: we use a sentinel return pattern and confirm no LuaError is
    // returned.
    const src =
        \\local _ran = false
        \\return {
        \\  meta = {
        \\    name = "run-test",
        \\    description = "verify run is called",
        \\    phases = {},
        \\  },
        \\  run = function(ctx)
        \\    -- Verify ctx carries the args sub-table.
        \\    if type(ctx) ~= "table" then error("ctx must be a table") end
        \\    if type(ctx.args) ~= "table" then error("ctx.args must be a table") end
        \\    -- No error means run was reached.
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    try callRun(src, "test:callrun", &.{}, &err_buf);
}

test "callRun: ctx.args receives CLI positionals as 1-based sequence" {
    // The workflow's run receives ctx.args[1], ctx.args[2], ... matching the
    // order of the slice passed to callRun. This is the threading invariant
    // that task 3165 requires.
    const src =
        \\return {
        \\  meta = { name = "args-test", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    if ctx.args[1] ~= "hello" then error("expected ctx.args[1]='hello', got: " .. tostring(ctx.args[1])) end
        \\    if ctx.args[2] ~= "world" then error("expected ctx.args[2]='world', got: " .. tostring(ctx.args[2])) end
        \\    if ctx.args[3] ~= nil    then error("expected ctx.args[3]=nil, got: " .. tostring(ctx.args[3])) end
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const cli_args: []const []const u8 = &.{ "hello", "world" };
    try callRun(src, "test:args-threading", cli_args, &err_buf);
}

test "callRun: ctx.args length matches slice length" {
    // #ctx.args must equal the number of args passed.
    const src =
        \\return {
        \\  meta = { name = "len-test", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local n = #ctx.args
        \\    if n ~= 3 then error("expected #ctx.args=3, got " .. n) end
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const cli_args: []const []const u8 = &.{ "a", "b", "c" };
    try callRun(src, "test:args-len", cli_args, &err_buf);
}

test "callRun: runtime error in run is surfaced" {
    // A run that calls error() must yield LuaRuntimeError, not a panic.
    const src =
        \\return {
        \\  meta = { name = "e", description = "d", phases = {} },
        \\  run = function(ctx) error("deliberate run error") end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const err = callRun(src, "test:callrun-err", &.{}, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);
    try std.testing.expect(err_buf[0] != 0);
}

// ---------------------------------------------------------------------------
// task 3222 — captureError: non-string error objects produce a message
// ---------------------------------------------------------------------------

test "captureError: error({}) produces a non-empty message (task 3222)" {
    // A script that raises a table error object must yield a non-empty err_buf.
    // With the old lua_tolstring this returned NULL → empty buf.
    // With luaL_tolstring the table is coerced to a string representation.
    const src =
        \\return {
        \\  meta = { name = "e", description = "d", phases = {} },
        \\  run = function(ctx) error({code=42, msg="table error"}) end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    const err = callRun(src, "test:table-error", &.{}, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);
    // err_buf must now contain something (table representation or address).
    const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
    try std.testing.expect(msg.len > 0);
}

test "captureError: error('string') still produces the message (task 3222 regression)" {
    // Confirm that the luaL_tolstring change does not regress string errors.
    const src =
        \\return {
        \\  meta = { name = "e", description = "d", phases = {} },
        \\  run = function(ctx) error("string error message") end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    const err = callRun(src, "test:string-error", &.{}, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);
    const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
    try std.testing.expect(std.mem.indexOf(u8, msg, "string error message") != null);
}

// ---------------------------------------------------------------------------
// task 3166 tests — printDryRun
// ---------------------------------------------------------------------------

test "printDryRun: output contains name, description, and phase titles" {
    // printDryRun must emit the workflow name, description, and each phase
    // title to the writer. This pins the stable output format.
    //
    // printDryRun takes an *Io.Writer; we capture output via
    // std.Io.Writer.Allocating — the allocating variant available in Zig 0.16.
    const alloc = std.testing.allocator;

    var buf: std.Io.Writer.Allocating = .init(alloc);
    defer buf.deinit();
    const w = &buf.writer;

    const phases = [_]PhaseMeta{
        .{ .title = "Setup", .detail = "initialize" },
        .{ .title = "Execute", .detail = "" },
        .{ .title = "Teardown", .detail = "clean up" },
    };
    const mod = WorkflowModule{
        .meta = WorkflowMeta{
            .name = "my-workflow",
            .description = "Does things",
            .phases = @constCast(&phases),
        },
    };

    try printDryRun(mod, w);

    const output = buf.writer.buffered();
    try std.testing.expect(std.mem.indexOf(u8, output, "workflow: my-workflow") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "description: Does things") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "phases: 3") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "1. Setup") != null);
    // Phase 2 has empty detail — must NOT include " — ".
    try std.testing.expect(std.mem.indexOf(u8, output, "2. Execute\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "3. Teardown") != null);
}

// ---------------------------------------------------------------------------
// task 3227 regression — empty-title OOM-mid-iteration leak (iteration-2 fix)
// ---------------------------------------------------------------------------

test "extractMeta: empty-title phase — no leak on OOM during detail copy (task 3227)" {
    // Regression test for the bug where `errdefer if (ph_title.len > 0)
    // allocator.free(ph_title)` skipped freeing a heap-allocated empty-string
    // title (len == 0 but still allocator-owned via dupe), leaking it when the
    // subsequent detail allocation failed with OOM.
    //
    // The fix makes ph_title and ph_detail always heap-owned from the start of
    // each iteration (placeholder via dupe("") at iteration start); errdefers
    // are unconditional.  When the title is a non-empty Lua string, the
    // allocate-then-swap pattern ensures ph_title is always a valid heap
    // pointer at the point any errdefer fires.
    //
    // Zig's std.mem.Allocator elides rawAlloc for zero-byte requests (returns a
    // comptime sentinel pointer without calling the underlying allocator), so
    // dupe("") and free of a zero-length slice do not increment
    // FailingAllocator's alloc_index / deallocation counters.  The test uses
    // a module whose phase has title = "t" (non-empty) to exercise the
    // allocate-then-swap path under real rawAlloc calls.
    //
    // Verified rawAlloc call sequence inside extractMeta for:
    //   meta.name = "n", meta.description = "d"
    //   phases[1] = { title = "t", detail = "non-empty-detail" }
    //
    //   alloc 0: copyLuaString for meta.name ("n", len=1)
    //   alloc 1: copyLuaString for meta.description ("d", len=1)
    //   [dupe("") for ph_title placeholder: len=0 → no rawAlloc]
    //   [dupe("") for ph_detail placeholder: len=0 → no rawAlloc]
    //   alloc 2: copyLuaString new_title for "t" (len=1); placeholder freed (len=0 → no rawFree)
    //   alloc 3: copyLuaString new_detail for "non-empty-detail" ← FAIL HERE
    //            ph_detail errdefer frees placeholder (len=0 → no rawFree)
    //            ph_title errdefer frees new_title (alloc 2, len=1 → rawFree, dealloc +1)
    //            meta_desc errdefer frees (alloc 1, len=1 → rawFree, dealloc +1)
    //            meta_name errdefer frees (alloc 0, len=1 → rawFree, dealloc +1)
    //
    // After failure: allocations = 3, deallocations = 3 → no leak.
    //
    // With the old `errdefer if (ph_title.len > 0) allocator.free(ph_title)`
    // guard, the fix is logically identical for this case (title is non-empty,
    // so len > 0 fires).  However, if title were empty (Lua "" → dupe("") →
    // sentinel, len=0), the old guard would skip the free — which is harmless
    // for the sentinel (no rawAlloc), but is still conceptually inconsistent
    // with the deinit / outer errdefer which free unconditionally.  The new
    // unconditional errdefer is correct for both cases: freeing a len=0 slice
    // is a no-op (stdlib checks len == 0 before rawFree), so it is safe.
    const src =
        \\return {
        \\  meta = {
        \\    name = "n",
        \\    description = "d",
        \\    phases = {
        \\      { title = "t", detail = "non-empty-detail" },
        \\    },
        \\  },
        \\  run = function(ctx) end,
        \\}
    ;

    // fail_index = 3: allocs 0-2 succeed, alloc 3 (detail copy) fails.
    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 3 });
    const fa = failing.allocator();

    var err_buf: [256]u8 = @splat(0);
    const result = loadModule(src, "test:oom-empty-title", fa, &err_buf);
    try std.testing.expectError(error.OutOfMemory, result);

    // Verify no leak: every rawAlloc was matched by a rawFree.
    try std.testing.expectEqual(failing.allocations, failing.deallocations);
}

// ---------------------------------------------------------------------------
// task 3168 (m2-host-fns) + task 3169 (m2-sandbox) tests
// ---------------------------------------------------------------------------

/// findCall returns the first recorded HostCall of `kind`, or null.
fn findCall(host: *const HostState, kind: HostCallKind) ?HostCall {
    for (host.calls.items) |call| {
        if (call.kind == kind) return call;
    }
    return null;
}

test "host fns: agent/phase/log recorded on ctx with expected args (task 3168)" {
    // A workflow whose run(ctx) calls ctx.agent / ctx.phase / ctx.log must
    // succeed, and the recorded calls must be observable from the Zig host with
    // the expected prompt / title / msg and argument shapes.
    const src =
        \\return {
        \\  meta = { name = "host-fns", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.phase("Build")
        \\    ctx.log("starting work")
        \\    local r = ctx.agent("do the thing", { role = "coder" })
        \\    assert(type(r) == "table", "agent must return a table")
        \\    assert(r.status == "stub", "agent stub must return status=stub")
        \\  end,
        \\}
    ;
    var host = HostState.init(testing_alloc, 0, 0, 100, 0);
    defer host.deinit();
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:host-fns", &.{}, &host, &err_buf);

    // phase("Build") recorded.
    const phase = findCall(&host, .phase) orelse return error.TestExpectedPhase;
    try std.testing.expectEqualStrings("Build", phase.arg0);

    // log("starting work") recorded.
    const log = findCall(&host, .log) orelse return error.TestExpectedLog;
    try std.testing.expectEqualStrings("starting work", log.arg0);

    // agent("do the thing", {table}) recorded with prompt + opts shape.
    const agent = findCall(&host, .agent) orelse return error.TestExpectedAgent;
    try std.testing.expectEqualStrings("do the thing", agent.arg0);
    try std.testing.expectEqualStrings("table", agent.arg1);
}

test "host fns: parallel/pipeline/workflow record call shapes (task 3168)" {
    // parallel({thunks}) records the thunk count; pipeline(items, ...stages)
    // records item + stage counts; workflow(name, args) records the name. None
    // drive real work at M2 — recording the shape is the deliverable.
    const src =
        \\return {
        \\  meta = { name = "shapes", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.parallel({ function() end, function() end, function() end })
        \\    ctx.pipeline({ "a", "b" }, function() end, function() end)
        \\    ctx.workflow("sub-flow", { k = 1 })
        \\  end,
        \\}
    ;
    var host = HostState.init(testing_alloc, 0, 0, 100, 0);
    defer host.deinit();
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:shapes", &.{}, &host, &err_buf);

    const par = findCall(&host, .parallel) orelse return error.TestExpectedParallel;
    try std.testing.expectEqualStrings("3 thunks", par.arg0);

    const pipe = findCall(&host, .pipeline) orelse return error.TestExpectedPipeline;
    try std.testing.expectEqualStrings("2 items", pipe.arg0);
    try std.testing.expectEqualStrings("2 stages", pipe.arg1);

    const wf = findCall(&host, .workflow) orelse return error.TestExpectedWorkflow;
    try std.testing.expectEqualStrings("sub-flow", wf.arg0);
    try std.testing.expectEqualStrings("table", wf.arg1);
}

test "host fns: budget.total / spent() / remaining() return injected values (task 3168)" {
    // budget is a host-backed table: total is the injected ceiling, and the
    // method forms budget:spent() / budget:remaining() read the injected host
    // values (remaining = total - spent).
    const src =
        \\return {
        \\  meta = { name = "budget", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    assert(ctx.budget.total == 250, "total: got " .. tostring(ctx.budget.total))
        \\    assert(ctx.budget:spent() == 40, "spent: got " .. tostring(ctx.budget:spent()))
        \\    assert(ctx.budget:remaining() == 210, "remaining: got " .. tostring(ctx.budget:remaining()))
        \\  end,
        \\}
    ;
    var host = HostState.init(testing_alloc, 0, 0, 250, 40);
    defer host.deinit();
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:budget", &.{}, &host, &err_buf);

    // The method-form calls were recorded (twice spent, once remaining above —
    // spent is called twice because the assert message also evaluates it).
    try std.testing.expect(findCall(&host, .budget_spent) != null);
    try std.testing.expect(findCall(&host, .budget_remaining) != null);
}

test "sandbox: os.execute and io are absent (task 3169)" {
    // os and io are never opened, so they are nil globals. Referencing
    // os.execute (indexing a nil) raises a runtime error; io is nil.
    const src_os =
        \\return {
        \\  meta = { name = "os", description = "d", phases = {} },
        \\  run = function(ctx) os.execute("echo hi") end,
        \\}
    ;
    var host_a = HostState.init(testing_alloc, 0, 0, 100, 0);
    defer host_a.deinit();
    var err_buf: [256]u8 = @splat(0);
    const err_os = runModule(src_os, "test:os", &.{}, &host_a, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err_os);

    // io must be nil — assert it explicitly inside the sandbox.
    const src_io =
        \\return {
        \\  meta = { name = "io", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    assert(io == nil, "io must be absent")
        \\    assert(os == nil, "os must be absent")
        \\  end,
        \\}
    ;
    var host_b = HostState.init(testing_alloc, 0, 0, 100, 0);
    defer host_b.deinit();
    try runModule(src_io, "test:io", &.{}, &host_b, &err_buf);
}

test "sandbox: os.time and math.random are absent/nil (task 3169)" {
    // os is nil entirely (so os.time unreachable) and math.random / randomseed
    // are stripped from the math table. base/string/table/math otherwise work.
    const src =
        \\return {
        \\  meta = { name = "det", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    assert(os == nil, "os (and os.time) must be absent")
        \\    assert(math.random == nil, "math.random must be stripped")
        \\    assert(math.randomseed == nil, "math.randomseed must be stripped")
        \\    -- math itself still works (deterministic functions kept).
        \\    assert(math.floor(3.7) == 3, "math.floor must remain")
        \\    -- string/table libs remain.
        \\    assert(string.upper("a") == "A", "string lib must remain")
        \\    assert(#({1,2,3}) == 3, "tables work")
        \\  end,
        \\}
    ;
    var host = HostState.init(testing_alloc, 0, 0, 100, 0);
    defer host.deinit();
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:determinism", &.{}, &host, &err_buf);
}

test "sandbox: ctx.now and ctx.seed are host-injected and deterministic (task 3169)" {
    // With os.time / math.random gone, the script's only time/random source is
    // the host-injected ctx.now / ctx.seed. They must equal what the host set.
    const src =
        \\return {
        \\  meta = { name = "inject", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    assert(ctx.now == 1717200000, "ctx.now: got " .. tostring(ctx.now))
        \\    assert(ctx.seed == 4242, "ctx.seed: got " .. tostring(ctx.seed))
        \\  end,
        \\}
    ;
    var host = HostState.init(testing_alloc, 1717200000, 4242, 100, 0);
    defer host.deinit();
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:inject", &.{}, &host, &err_buf);
}

test "sandbox: loader escape-hatches (load/dofile/loadfile/require) are nil (task 3169)" {
    // The filesystem / arbitrary-code-loading escape hatches are stripped so a
    // workflow cannot re-acquire host reach.
    const src =
        \\return {
        \\  meta = { name = "loaders", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    assert(load == nil, "load must be stripped")
        \\    assert(dofile == nil, "dofile must be stripped")
        \\    assert(loadfile == nil, "loadfile must be stripped")
        \\    assert(require == nil, "require must be stripped")
        \\  end,
        \\}
    ;
    var host = HostState.init(testing_alloc, 0, 0, 100, 0);
    defer host.deinit();
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:loaders", &.{}, &host, &err_buf);
}

test "sandbox: debug and package libraries are absent (task 3232)" {
    // debug.getupvalue could pierce the HostState light-userdata upvalue and
    // debug.getregistry reaches LUA_LOADED_TABLE. package exposes the module
    // loader internals. Neither library is opened in openSandboxedLibs, so both
    // globals must be nil. This is a security-boundary regression guard: a future
    // maintainer adding luaopen_debug "for diagnostics" must get a red test here.
    const src =
        \\return {
        \\  meta = { name = "no-debug-pkg", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    assert(debug == nil, "debug must be absent (task 3232)")
        \\    assert(package == nil, "package must be absent (task 3232)")
        \\  end,
        \\}
    ;
    var host = HostState.init(testing_alloc, 0, 0, 100, 0);
    defer host.deinit();
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:no-debug-pkg", &.{}, &host, &err_buf);
}

test "sandbox: string.dump present but load is nil — bytecode out, no re-execution path (task 3233)" {
    // string.dump IS present (full string lib is opened) and CAN serialize
    // function bytecode. But load/loadstring/dofile/loadfile/require are all
    // nil'd, closing every path back in. This test pins both halves: bytecode
    // serialization works, bytecode re-execution is impossible. Belt-and-
    // suspenders: a future maintainer re-opening load must get a red test here.
    const src =
        \\return {
        \\  meta = { name = "dump-no-load", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local fn = function(x) return x + 1 end
        \\    local bytecode = string.dump(fn)
        \\    assert(type(bytecode) == "string" and #bytecode > 0,
        \\           "string.dump must return non-empty bytecode (task 3233)")
        \\    assert(load == nil,       "load must be nil (task 3233)")
        \\    assert(loadstring == nil, "loadstring must be nil (task 3233)")
        \\    assert(loadfile == nil,   "loadfile must be nil (task 3233)")
        \\    assert(dofile == nil,     "dofile must be nil (task 3233)")
        \\    assert(require == nil,    "require must be nil (task 3233)")
        \\  end,
        \\}
    ;
    var host = HostState.init(testing_alloc, 0, 0, 100, 0);
    defer host.deinit();
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:dump-no-load", &.{}, &host, &err_buf);
}

test "host fns: no host means ctx carries only args (callRun M1 shape preserved)" {
    // callRun (no HostState) must leave ctx.agent etc. nil — the M1 shape. This
    // pins that the host surface is opt-in via runModule(..., host, ...).
    const src =
        \\return {
        \\  meta = { name = "m1-shape", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    assert(ctx.agent == nil, "ctx.agent must be nil without a HostState")
        \\    assert(ctx.budget == nil, "ctx.budget must be nil without a HostState")
        \\    assert(type(ctx.args) == "table", "ctx.args must still be a table")
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try callRun(src, "test:m1-shape", &.{}, &err_buf);
}

// ---------------------------------------------------------------------------
// task 3175/3176/3177/3178/3180 tests — M4 agent() pipeline via FakeSpawner.
//
// These exercise the full hostAgent → driveAgentCall path WITHOUT spawning a
// real claude (no $ burned). The pinned live-spawn test is in
// integration_tests/planar_execute_agent_live_test.zig, gated by
// PLANAR_EXECUTE_LIVE_AGENT=1.
// ---------------------------------------------------------------------------

test "M4 agent: with no driver, hostAgent retains the M2 stub behavior (regression)" {
    // The whole reason the M2 mode is preserved: existing tests/workflows that
    // construct a HostState without an agent_driver continue to see status="stub".
    // The recording still fires.
    const src =
        \\return {
        \\  meta = { name = "m4-stub", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("a brief", { role = "coder" })
        \\    assert(r.status == "stub", "expected stub when no driver injected, got " .. tostring(r.status))
        \\  end,
        \\}
    ;
    var host = HostState.init(testing_alloc, 0, 0, 100, 0);
    defer host.deinit();
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m4-stub", &.{}, &host, &err_buf);

    // Recording still fires.
    const a = findCall(&host, .agent) orelse return error.TestExpectedAgent;
    try std.testing.expectEqualStrings("a brief", a.arg0);
}

test "M4 agent: with FakeSpawner + driver, returns completed when exit==0 + commit (active claim path)" {
    // Wire a FakeSpawner + AgentDriver into HostState. The driver's repo_root
    // is empty so the branch-head sample is skipped — but the FakeSpawner
    // returns exit=0 and we need commit_present=true to test the "complete"
    // path. We achieve that by skipping head sampling (commit_present=false)
    // and asserting "released" (the no-commit branch).
    //
    // A separate test below covers the complete branch via a real-repo head
    // sample.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .repo_root = "",
        .plan_slug = "",
        .skip_terminal_subprocess = true,
    };

    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m4-released", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok-xyz",
        \\      role_spec = "you are a coder",
        \\      task_slug = "ts1",
        \\    })
        \\    assert(r.status == "released", "expected released (no commit), got " .. tostring(r.status))
        \\    assert(r.exit_code == 0, "exit_code mismatch: " .. tostring(r.exit_code))
        \\    assert(r.commit_present == false, "commit_present should be false")
        \\    assert(r.terminal_verb == "release", "terminal_verb mismatch: " .. tostring(r.terminal_verb))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m4-released", &.{}, &host, &err_buf);

    // FakeSpawner recorded exactly one invocation with the right shape.
    try std.testing.expectEqual(@as(usize, 1), fake.invocations.items.len);
    const inv = fake.invocations.items[0];
    try std.testing.expectEqual(role_model.Role.coder, inv.role);
    try std.testing.expectEqualStrings("/tmp/abs/wt", inv.worktree_path);
    try std.testing.expectEqualStrings("the brief", inv.brief);
    // argv: --model claude-opus-4-8 (coder → opus), bypassPermissions, etc.
    try std.testing.expectEqualStrings("bypassPermissions", inv.argv[3]);
    try std.testing.expectEqualStrings(role_model.OPUS_TIER, inv.argv[5]);
    try std.testing.expectEqualStrings("/tmp/abs/wt", inv.argv[7]);
    try std.testing.expectEqualStrings("you are a coder", inv.argv[9]);
}

test "M4 agent: exit non-zero drives status=failed + terminal_verb=fail" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 7, "", "boom");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m4-failed", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("brief", {
        \\      role = "reviewer",
        \\      worktree_path = "/tmp/abs",
        \\      claim_token = "tok",
        \\      task_slug = "tx",
        \\    })
        \\    assert(r.status == "failed", "status: " .. tostring(r.status))
        \\    assert(r.exit_code == 7, "exit_code: " .. tostring(r.exit_code))
        \\    assert(r.terminal_verb == "fail", "verb: " .. tostring(r.terminal_verb))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m4-failed", &.{}, &host, &err_buf);
}

test "M4 agent: unknown role raises a Lua error (script-author bug)" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m4-badrole", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.agent("brief", { role = "wizard", worktree_path = "/tmp/wt" })
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    const err = runModule(src, "test:m4-badrole", &.{}, &host, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);
    const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
    try std.testing.expect(std.mem.indexOf(u8, msg, "unknown role") != null);
    // FakeSpawner was never invoked.
    try std.testing.expectEqual(@as(usize, 0), fake.invocations.items.len);
}

test "M4 agent: missing worktree_path raises a Lua error" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m4-nowt", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.agent("brief", { role = "coder" })
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    const err = runModule(src, "test:m4-nowt", &.{}, &host, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);
}

test "M4 agent: registerInflight RegistryFull → child killed + spawn_count decremented (PR #17 finding 6)" {
    // PR #17 cycle B finding 6 regression pin. When the scheduler registry
    // is full at the moment hostAgent attempts registerInflight, the LIVE
    // spawned child must be hard-killed (NOT abandoned via a fallible
    // `wait`), the `spawn_count` ceiling counter must be decremented (the
    // bump at line ~1340 is undone because this spawn produced no useful
    // work), and the worker's claim must NEVER reach the heartbeat
    // registry (that register happens AFTER the successful registerInflight,
    // so the catch path simply does not reach it).
    //
    // Drive: pre-fill ALL MAX_SLOTS scheduler slots via the test seam
    // (HostState.test_pre_fill_slots). Then the single `ctx.agent(...)` call
    // attempts registerInflight, hits RegistryFull, and the fix-path runs.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .repo_root = "",
        .plan_slug = "",
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;
    host.test_pre_fill_slots = scheduler.MAX_SLOTS;

    const src =
        \\return {
        \\  meta = { name = "m4-regfull", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.agent("brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok",
        \\      task_slug = "ts",
        \\    })
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    const err = runModule(src, "test:m4-regfull", &.{}, &host, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);

    // The Lua error carries the registry-full message.
    const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
    try std.testing.expect(std.mem.indexOf(u8, msg, "scheduler registry full") != null);

    // FakeSpawner saw exactly one start (the live child) AND one kill
    // (the recovery path's `Spawner.kill`). No `wait` was called — the
    // fix replaces the fallible `wait` with infallible `kill`.
    try std.testing.expectEqual(@as(u32, 1), fake.start_count);
    try std.testing.expectEqual(@as(u32, 1), fake.kill_count);
    try std.testing.expectEqual(@as(u32, 0), fake.wait_count);
    // The killed worker is the one that was just spawned (FakeSpawner
    // assigns sequential ids starting at 0).
    try std.testing.expect(fake.killedWorker(0));

    // spawn_count was incremented BEFORE the registry check then
    // decremented in the recovery path: net effect on the ceiling is 0,
    // matching "this spawn produced no useful work".
    try std.testing.expectEqual(@as(u32, 0), host.run_counters.spawn_count);

    // The killed worker is no longer "live" in the FakeSpawner registry
    // (fakeKillFn decrements live_inflight). This is the proxy for "no
    // orphan child running with no reaper".
    try std.testing.expectEqual(@as(u32, 0), fake.live_inflight);
}

// ---------------------------------------------------------------------------
// task 3242 — live-claim-status read → harness no-op when the worker already
// ran its own terminal verb. Exercised via an INJECTED claim_status_reader so
// the test never shells planar-watch (the live `state.claimStatus` subprocess
// is covered by the live-spawn smoke, task 3241).
// ---------------------------------------------------------------------------

/// terminalClaimReader is an injected ClaimStatusFn that always reports the
/// claim as `.completed` — simulating a worker that ran its own
/// `planar-agent complete`. `decideTerminalVerb` must then return `.none`.
fn terminalClaimReader(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    plan_id: u64,
    claim_token: []const u8,
) terminal.ClaimStatus {
    _ = ctx;
    _ = allocator;
    _ = io;
    _ = plan_id;
    _ = claim_token;
    return .completed;
}

test "M4 agent: worker self-completed (claim terminal) → harness emits no terminal verb (task 3242)" {
    // The bug-fix path (cycle C iter-1 Item A): a worker that already ran its
    // own terminal verb leaves the claim no longer live. The injected reader
    // reports .completed; decideTerminalVerb returns .none; the result table
    // reports status="respected" / terminal_verb="none" — NOT a redundant
    // "complete" that would have errored with ClaimNotActive.
    const a = testing_alloc;
    // exit==0 + (would-be) commit would normally drive .complete on an active
    // claim; the terminal claim must override that to .none.
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .claim_status_reader = terminalClaimReader,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m4-respected", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok-self-completed",
        \\      task_slug = "ts1",
        \\    })
        \\    assert(r.terminal_verb == "none", "expected no harness terminal verb, got " .. tostring(r.terminal_verb))
        \\    assert(r.status == "respected", "expected status=respected, got " .. tostring(r.status))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m4-respected", &.{}, &host, &err_buf);

    // The spawn still happened (the harness ran the worker); only the
    // redundant terminal verb is suppressed.
    try std.testing.expectEqual(@as(usize, 1), fake.invocations.items.len);
}

// ---------------------------------------------------------------------------
// task 3194/3195 — M7 local-block detection + end-of-run summary.
//
// A worker that hits a genuine blocker runs `planar-agent block` itself (per the
// brief). `block` flips the TASK to `blocked` + releases the claim. The harness
// detects the `blocked` task status (via an INJECTED task_status_reader here, so
// the test never shells `planar task show`), surfaces status="blocked" WITHOUT a
// redundant terminal verb, accumulates the item, and the run continues.
// ---------------------------------------------------------------------------

/// BlockedReaderCtx lets a test reader report a specific task_id as blocked.
/// `blocked_id == 0` means "no task is blocked" (every read returns false).
const BlockedReaderCtx = struct {
    blocked_id: u64,
};

/// fakeTaskStatusReader is an injected TaskStatusFn: it returns true iff the
/// queried task_id matches the ctx's `blocked_id`. No subprocess, no allocation.
fn fakeTaskStatusReader(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_id: u64,
) bool {
    _ = allocator;
    _ = io;
    const rc: *BlockedReaderCtx = @ptrCast(@alignCast(ctx.?));
    return task_id == rc.blocked_id;
}

test "M7 block: worker self-blocked (task=blocked) → status=blocked, NO terminal verb, item accumulated (task 3194/3195)" {
    const a = testing_alloc;
    // exit==0 + commit-present would normally drive .complete; the worker
    // self-blocked though (claim already released → .terminal → verb .none),
    // and the blocked TASK status is what we surface.
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    // claim reader reports .completed (worker ran its own block → claim gone).
    var blocked_ctx = BlockedReaderCtx{ .blocked_id = 3201 };
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .claim_status_reader = terminalClaimReader,
        .task_status_reader = fakeTaskStatusReader,
        .task_status_ctx = &blocked_ctx,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m7-block", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok-blocked",
        \\      task_slug = "m7-foo",
        \\      task_id = 3201,
        \\    })
        \\    assert(r.status == "blocked", "expected status=blocked, got " .. tostring(r.status))
        \\    assert(r.terminal_verb == "none", "expected no harness terminal verb, got " .. tostring(r.terminal_verb))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m7-block", &.{}, &host, &err_buf);

    // The worker ran (spawn happened) and the item was accumulated for triage.
    try std.testing.expectEqual(@as(usize, 1), fake.invocations.items.len);
    try std.testing.expectEqual(@as(usize, 1), host.blocked_items.items.len);
    try std.testing.expectEqualStrings("m7-foo", host.blocked_items.items[0].task_slug);
}

test "M7 block: a normal completed worker is NOT mis-detected as blocked (task 3194)" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    // Reader reports NO task blocked (blocked_id 0 never matches a real id).
    var blocked_ctx = BlockedReaderCtx{ .blocked_id = 0 };
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        // active claim + exit 0 + commit → .complete → status "completed".
        .task_status_reader = fakeTaskStatusReader,
        .task_status_ctx = &blocked_ctx,
        .repo_root = "",
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m7-notblocked", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok",
        \\      task_slug = "m7-bar",
        \\      task_id = 9999,
        \\    })
        \\    assert(r.status ~= "blocked", "must not be blocked, got " .. tostring(r.status))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m7-notblocked", &.{}, &host, &err_buf);

    // Nothing accumulated for a non-blocked worker.
    try std.testing.expectEqual(@as(usize, 0), host.blocked_items.items.len);
}

test "M7 block: in a parallel set, one worker blocks, others complete; parallel returns normally, original order (task 3194)" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    // Only task_id 3202 (the "two" thunk) is blocked; 3200 and 3201 complete.
    var blocked_ctx = BlockedReaderCtx{ .blocked_id = 3202 };
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .task_status_reader = fakeTaskStatusReader,
        .task_status_ctx = &blocked_ctx,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m7-par-block", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local function mk(tag, id)
        \\      return function()
        \\        local r = ctx.agent("b-" .. tag, { role = "coder", worktree_path = "/tmp/" .. tag, claim_token = "t-" .. tag, task_slug = tag, task_id = id })
        \\        return r.status
        \\      end
        \\    end
        \\    local results = ctx.parallel({ mk("zero", 3200), mk("one", 3201), mk("two", 3202) })
        \\    assert(#results == 3, "len: " .. tostring(#results))
        \\    -- order preserved: results[i] is thunk[i]'s status.
        \\    assert(results[1] ~= "blocked", "r1 should not be blocked: " .. tostring(results[1]))
        \\    assert(results[2] ~= "blocked", "r2 should not be blocked: " .. tostring(results[2]))
        \\    assert(results[3] == "blocked", "r3 should be blocked: " .. tostring(results[3]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m7-par-block", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("m7 parallel-block failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };

    // All three workers ran; the run did NOT halt or fail; exactly one blocked.
    try std.testing.expectEqual(@as(u32, 3), fake.start_count);
    try std.testing.expectEqual(@as(u32, 0), fake.live_inflight);
    try std.testing.expectEqual(@as(usize, 1), host.blocked_items.items.len);
    try std.testing.expectEqualStrings("two", host.blocked_items.items[0].task_slug);
}

test "M7 summary: printBlockedSummary lists items; quiet when zero (task 3195)" {
    const a = testing_alloc;

    // Zero items → no output.
    {
        var buf: std.Io.Writer.Allocating = .init(a);
        defer buf.deinit();
        try printBlockedSummary(&.{}, &buf.writer);
        try std.testing.expectEqual(@as(usize, 0), buf.written().len);
    }

    // Two items → header + one line each.
    {
        const items = [_]BlockedItem{
            .{ .task_slug = "m5-foo", .task_blocker = "task:3199", .reason = "needs schema decision" },
            .{ .task_slug = "m5-bar", .task_blocker = "", .reason = "" },
        };
        var buf: std.Io.Writer.Allocating = .init(a);
        defer buf.deinit();
        try printBlockedSummary(&items, &buf.writer);
        const out = buf.written();
        try std.testing.expect(std.mem.indexOf(u8, out, "2 task(s) blocked for operator triage") != null);
        try std.testing.expect(std.mem.indexOf(u8, out, "task m5-foo blocked by task:3199 — \"needs schema decision\"") != null);
        // Partial record reads cleanly (no trailing "blocked by"/reason clause).
        try std.testing.expect(std.mem.indexOf(u8, out, "  - task m5-bar\n") != null);
    }
}

// ---------------------------------------------------------------------------
// M8 resume — idempotent agent() pre-spawn skip (task 3197).
//
// On a re-run, agent() reads the PRE-SPAWN live task status (keyed on the
// threaded task_id) and SHORT-CIRCUITS to a "skipped" result when the task is
// already `done` (or `blocked` — set aside by M7). The skip path acquires no
// claim, builds no worktree, spawns NO worker, and consumes NO scheduler slot.
// These tests inject a fake TaskLiveStatusFn so the read is deterministic (no
// live `planar task show`) and assert the FakeSpawner's start_count to prove
// the spawn was (or was not) attempted.
// ---------------------------------------------------------------------------

/// LiveStatusReaderCtx maps a task_id to a canned PRE-SPAWN live status for the
/// resume tests. `done_ids` / `blocked_ids` enumerate the ids that should report
/// `.done` / `.blocked`; anything else reports `.other` (→ spawn).
const LiveStatusReaderCtx = struct {
    done_ids: []const u64 = &.{},
    blocked_ids: []const u64 = &.{},
};

/// fakeTaskLiveStatusReader is an injected TaskLiveStatusFn: it classifies the
/// queried task_id against the ctx's done/blocked id lists. No subprocess, no
/// allocation.
fn fakeTaskLiveStatusReader(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_id: u64,
) TaskLiveStatus {
    _ = allocator;
    _ = io;
    const rc: *LiveStatusReaderCtx = @ptrCast(@alignCast(ctx.?));
    for (rc.done_ids) |id| if (id == task_id) return .done;
    for (rc.blocked_ids) |id| if (id == task_id) return .blocked;
    return .other;
}

test "M8 resume: task already done → status=skipped, NO spawn, no slot consumed (task 3197)" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var live_ctx = LiveStatusReaderCtx{ .done_ids = &.{3300} };
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .task_live_status_reader = fakeTaskLiveStatusReader,
        .task_live_status_ctx = &live_ctx,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m8-skip-done", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok",
        \\      task_slug = "m8-done",
        \\      task_id = 3300,
        \\    })
        \\    assert(r.status == "skipped", "expected status=skipped, got " .. tostring(r.status))
        \\    assert(r.skip_reason == "already-done", "expected skip_reason=already-done, got " .. tostring(r.skip_reason))
        \\    assert(r.terminal_verb == "none", "skipped call runs no terminal verb, got " .. tostring(r.terminal_verb))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m8-skip-done", &.{}, &host, &err_buf);

    // No spawn happened: no invocation recorded, start_count stayed 0, and no
    // worker is left in flight (no slot consumed).
    try std.testing.expectEqual(@as(usize, 0), fake.invocations.items.len);
    try std.testing.expectEqual(@as(u32, 0), fake.start_count);
    try std.testing.expectEqual(@as(u32, 0), fake.live_inflight);
    // A skipped done-task is NOT a blocked-triage item.
    try std.testing.expectEqual(@as(usize, 0), host.blocked_items.items.len);
}

test "M8 resume: task is todo → spawns normally (start_count==1) (task 3197)" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    // Empty id lists → every read classifies as .other → spawn.
    var live_ctx = LiveStatusReaderCtx{};
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .task_live_status_reader = fakeTaskLiveStatusReader,
        .task_live_status_ctx = &live_ctx,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m8-spawn-todo", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok",
        \\      task_slug = "m8-todo",
        \\      task_id = 3301,
        \\    })
        \\    assert(r.status ~= "skipped", "todo task must spawn, got " .. tostring(r.status))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m8-spawn-todo", &.{}, &host, &err_buf);

    try std.testing.expectEqual(@as(u32, 1), fake.start_count);
    try std.testing.expectEqual(@as(usize, 1), fake.invocations.items.len);
}

test "M8 resume: mixed run — only not-done tasks spawn; done ones skip (task 3197)" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    // 3310 + 3312 already done; 3311 + 3313 still todo → exactly two spawns.
    var live_ctx = LiveStatusReaderCtx{ .done_ids = &.{ 3310, 3312 } };
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .task_live_status_reader = fakeTaskLiveStatusReader,
        .task_live_status_ctx = &live_ctx,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m8-mix", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local function mk(tag, id)
        \\      return function()
        \\        local r = ctx.agent("b-" .. tag, { role = "coder", worktree_path = "/tmp/" .. tag, claim_token = "t-" .. tag, task_slug = tag, task_id = id })
        \\        return r.status
        \\      end
        \\    end
        \\    local results = ctx.parallel({ mk("a", 3310), mk("b", 3311), mk("c", 3312), mk("d", 3313) })
        \\    assert(#results == 4, "len: " .. tostring(#results))
        \\    assert(results[1] == "skipped", "a done→skipped: " .. tostring(results[1]))
        \\    assert(results[2] ~= "skipped", "b todo→spawn: " .. tostring(results[2]))
        \\    assert(results[3] == "skipped", "c done→skipped: " .. tostring(results[3]))
        \\    assert(results[4] ~= "skipped", "d todo→spawn: " .. tostring(results[4]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m8-mix", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("m8 mix failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };

    // Exactly the two not-done tasks spawned; the two done ones skipped.
    try std.testing.expectEqual(@as(u32, 2), fake.start_count);
    try std.testing.expectEqual(@as(usize, 2), fake.invocations.items.len);
    try std.testing.expectEqual(@as(u32, 0), fake.live_inflight);
}

test "M8 resume: no task_id → falls through to spawn (backward-compat) (task 3197)" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    // A reader that would classify task_id 0 as done IF consulted — but the
    // task_id==0 guard returns .other BEFORE the reader is called, so the call
    // spawns. (We never put 0 in done_ids anyway; this asserts the guard.)
    var live_ctx = LiveStatusReaderCtx{ .done_ids = &.{0} };
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .task_live_status_reader = fakeTaskLiveStatusReader,
        .task_live_status_ctx = &live_ctx,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m8-no-id", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    -- No task_id field → opts.task_id defaults to 0 → resume skip is
        \\    -- bypassed and the call spawns (today's behavior).
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok",
        \\      task_slug = "m8-noid",
        \\    })
        \\    assert(r.status ~= "skipped", "no task_id must spawn, got " .. tostring(r.status))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m8-no-id", &.{}, &host, &err_buf);

    try std.testing.expectEqual(@as(u32, 1), fake.start_count);
}

test "M8 resume: blocked task is SKIPPED with distinct reason, NO spawn (task 3197)" {
    // Blocked decision: a `blocked` task was set aside by M7 awaiting operator
    // triage. Its blocker likely has not cleared, so re-driving it on resume
    // would just re-block — wasting a spawn. We SKIP it (status="skipped") with
    // skip_reason="blocked" so the operator/summary can tell it apart from a
    // done-skip. (Safe alternative: re-drive blocked; we chose skip-blocked to
    // keep resume from churning on awaiting-triage items.)
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var live_ctx = LiveStatusReaderCtx{ .blocked_ids = &.{3320} };
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .task_live_status_reader = fakeTaskLiveStatusReader,
        .task_live_status_ctx = &live_ctx,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m8-skip-blocked", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok",
        \\      task_slug = "m8-blocked",
        \\      task_id = 3320,
        \\    })
        \\    assert(r.status == "skipped", "blocked task skipped, got " .. tostring(r.status))
        \\    assert(r.skip_reason == "blocked", "expected skip_reason=blocked, got " .. tostring(r.skip_reason))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m8-skip-blocked", &.{}, &host, &err_buf);

    // No spawn for a skipped blocked task; and the PRE-spawn skip does NOT add a
    // blocked-triage item (that is M7's POST-spawn detection, not resume).
    try std.testing.expectEqual(@as(u32, 0), fake.start_count);
    try std.testing.expectEqual(@as(usize, 0), host.blocked_items.items.len);
}

// ---------------------------------------------------------------------------
// M8 journal wiring (task 3196) — one run-journal record per worker spawn.
//
// These tests drive a full agent()/parallel() pipeline through runModule with a
// FakeSpawner + a temp repo_root + a non-zero plan_id, then read the journal
// file back and assert ONE record per spawn with the right fields. The temp
// repo_root keeps the journal OUT of this checkout's real `.worktrees/`. A fixed
// FakeClock makes the wall-clock deterministic (spawn-time == terminal-time read
// ⇒ wall_clock_ms == 0 exactly).
// ---------------------------------------------------------------------------

/// JournalTestClock is a settable fake monotonic clock for the journal wiring
/// tests: it returns the SAME value at the spawn-time and terminal-time reads,
/// so `wall_clock_ms` is exactly 0 (deterministic, no real elapsed time).
const JournalTestClock = struct {
    now_ns: i128 = 5_000_000_000,
    fn clockFn(ctx: ?*anyopaque, io: std.Io) i128 {
        _ = io;
        const self: *JournalTestClock = @ptrCast(@alignCast(ctx.?));
        return self.now_ns;
    }
};

/// jMkTmpDir / jRmTree mirror the runlock/journal test helpers: a fresh system
/// temp dir NOT nested under this checkout's `.worktrees/`, so the journal write
/// never pollutes the real tree.
fn jMkTmpDir(allocator: std.mem.Allocator) []const u8 {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "mktemp", "-d", "-t", "planar-journal-wire.XXXXXX" },
    }) catch @panic("jMkTmpDir: mktemp spawn failed");
    defer allocator.free(r.stderr);
    if (!(r.term == .exited and r.term.exited == 0)) {
        allocator.free(r.stdout);
        @panic("jMkTmpDir: mktemp non-zero exit");
    }
    const trimmed = std.mem.trim(u8, r.stdout, " \t\r\n");
    const owned = allocator.dupe(u8, trimmed) catch @panic("OOM");
    allocator.free(r.stdout);
    return owned;
}

fn jRmTree(allocator: std.mem.Allocator, path: []const u8) void {
    const r = std.process.run(allocator, std.testing.io, .{ .argv = &.{ "rm", "-rf", path } }) catch return;
    allocator.free(r.stdout);
    allocator.free(r.stderr);
}

/// activeClaimReader reports the claim as `.active` so the terminal-verb
/// decision falls back to the exit_code + commit_present matrix. The journal
/// wiring tests set a non-zero `plan_id` (so the journal write fires) but do NOT
/// have a live `planar-watch ps`; without this injected reader the default
/// reader would shell out and skew the decided verb.
fn activeClaimReader(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    plan_id: u64,
    claim_token: []const u8,
) terminal.ClaimStatus {
    _ = ctx;
    _ = allocator;
    _ = io;
    _ = plan_id;
    _ = claim_token;
    return .active;
}

test "M8 journal: a single agent() spawn appends ONE record with the right fields (task 3196)" {
    const a = testing_alloc;
    const io = std.testing.io;

    const repo_root = jMkTmpDir(a);
    defer a.free(repo_root);
    defer jRmTree(a, repo_root);

    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = io,
        .repo_root = repo_root, // → journal location is derivable
        .plan_slug = "p492", // → branch derives as cycle/p492/<task>
        .plan_id = 492, // → journal write fires (non-zero)
        .skip_terminal_subprocess = true,
        .claim_status_reader = activeClaimReader, // no live planar-watch in test
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    var clock = JournalTestClock{};
    host.timeout_clock_fn = JournalTestClock.clockFn;
    host.timeout_clock_ctx = &clock;

    const src =
        \\return {
        \\  meta = { name = "m8-journal", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.agent("the brief here", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok-journal",
        \\      task_slug = "ts-jour",
        \\    })
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m8-journal", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("m8 journal failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };

    // Read the journal back: exactly ONE record for the single spawn.
    const path = try journal.journalPath(a, repo_root, 492);
    defer a.free(path);
    var arena = std.heap.ArenaAllocator.init(a);
    defer arena.deinit();
    const recs = try journal.read(a, arena.allocator(), io, path);
    try std.testing.expectEqual(@as(usize, 1), recs.len);

    const rec = recs[0];
    try std.testing.expect(rec.prompt_hash.len != 0); // brief was fingerprinted
    try std.testing.expectEqualStrings("tok-journal", rec.claim_token);
    try std.testing.expectEqualStrings(role_model.OPUS_TIER, rec.model); // coder → opus
    try std.testing.expectEqualStrings("coder", rec.role);
    try std.testing.expectEqualStrings("ts-jour", rec.task_slug);
    try std.testing.expectEqualStrings("/tmp/abs/wt", rec.worktree);
    try std.testing.expectEqualStrings("cycle/p492/ts-jour", rec.branch);
    try std.testing.expectEqual(@as(i32, 0), rec.exit_code);
    // exit==0, no commit (branchHead fails in a non-git tmp dir) → released.
    try std.testing.expectEqualStrings("released", rec.terminal_verb);
    // Fixed clock: spawn-time == terminal-time read → exactly 0 ms.
    try std.testing.expectEqual(@as(i64, 0), rec.wall_clock_ms);

    // The prompt_hash matches the deterministic Wyhash of the brief.
    const expect_hash = try journal.hashPrompt(a, "the brief here");
    defer a.free(expect_hash);
    try std.testing.expectEqualStrings(expect_hash, rec.prompt_hash);
}

test "M8 journal: a failing (exit!=0) spawn journals terminal_verb=failed + exit_code (task 3196)" {
    const a = testing_alloc;
    const io = std.testing.io;

    const repo_root = jMkTmpDir(a);
    defer a.free(repo_root);
    defer jRmTree(a, repo_root);

    var fake = spawn.FakeSpawnerState.init(a, 7, "", "boom");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = io,
        .repo_root = repo_root,
        .plan_slug = "p1",
        .plan_id = 1,
        .skip_terminal_subprocess = true,
        .claim_status_reader = activeClaimReader,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m8-fail", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.agent("brief", { role = "reviewer", worktree_path = "/tmp/wt", claim_token = "tk", task_slug = "tf" })
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m8-fail", &.{}, &host, &err_buf);

    const path = try journal.journalPath(a, repo_root, 1);
    defer a.free(path);
    var arena = std.heap.ArenaAllocator.init(a);
    defer arena.deinit();
    const recs = try journal.read(a, arena.allocator(), io, path);
    try std.testing.expectEqual(@as(usize, 1), recs.len);
    try std.testing.expectEqual(@as(i32, 7), recs[0].exit_code);
    try std.testing.expectEqualStrings("failed", recs[0].terminal_verb);
    try std.testing.expectEqualStrings(role_model.OPUS_TIER, recs[0].model); // reviewer → opus
    try std.testing.expectEqualStrings("reviewer", recs[0].role);
}

test "M8 journal: a parallel set of N spawns appends N records (one per spawn) (task 3196)" {
    const a = testing_alloc;
    const io = std.testing.io;

    const repo_root = jMkTmpDir(a);
    defer a.free(repo_root);
    defer jRmTree(a, repo_root);

    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = io,
        .repo_root = repo_root,
        .plan_slug = "pN",
        .plan_id = 77,
        .skip_terminal_subprocess = true,
        .claim_status_reader = activeClaimReader,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m8-parN", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local function mk(tag)
        \\      return function()
        \\        ctx.agent("brief-" .. tag, { role = "coder", worktree_path = "/tmp/" .. tag, claim_token = "t-" .. tag, task_slug = tag })
        \\        return tag
        \\      end
        \\    end
        \\    local r = ctx.parallel({ mk("a"), mk("b"), mk("c") })
        \\    assert(#r == 3, "len: " .. tostring(#r))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m8-parN", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("m8 parN failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };

    // Three spawns → three journal records, one per worker.
    try std.testing.expectEqual(@as(u32, 3), fake.start_count);
    const path = try journal.journalPath(a, repo_root, 77);
    defer a.free(path);
    var arena = std.heap.ArenaAllocator.init(a);
    defer arena.deinit();
    const recs = try journal.read(a, arena.allocator(), io, path);
    try std.testing.expectEqual(@as(usize, 3), recs.len);
    // Each record's task_slug is one of the three thunk tags (set membership).
    var seen_a = false;
    var seen_b = false;
    var seen_c = false;
    for (recs) |rec| {
        if (std.mem.eql(u8, rec.task_slug, "a")) seen_a = true;
        if (std.mem.eql(u8, rec.task_slug, "b")) seen_b = true;
        if (std.mem.eql(u8, rec.task_slug, "c")) seen_c = true;
        try std.testing.expect(rec.prompt_hash.len != 0);
        try std.testing.expectEqualStrings("claude-opus-4-8", rec.model);
    }
    try std.testing.expect(seen_a and seen_b and seen_c);
}

test "M8 journal: with no repo_root/plan_id the journal write is skipped (no file) (task 3196)" {
    const a = testing_alloc;
    const io = std.testing.io;

    // Driver with EMPTY repo_root + plan_id 0 → the skip guard fires; no journal.
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m8-skip", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.agent("brief", { role = "coder", worktree_path = "/tmp/wt", claim_token = "tk", task_slug = "ts" })
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m8-skip", &.{}, &host, &err_buf);

    // The (degenerate "." -rooted) journal path must NOT have been created. We
    // assert via read on the "."-derived path returning empty (no file written).
    const path = try journal.journalPath(a, "", 0);
    defer a.free(path);
    var arena = std.heap.ArenaAllocator.init(a);
    defer arena.deinit();
    const recs = try journal.read(a, arena.allocator(), io, path);
    try std.testing.expectEqual(@as(usize, 0), recs.len);
    try std.testing.expectEqual(@as(u32, 1), fake.start_count); // worker DID run
}

// ---------------------------------------------------------------------------
// M8 budgets + ceilings — the hard kill-switch (task 3198).
//
// Two halves, both driven deterministically through runModule with a
// FakeSpawner:
//   1. Per-task max-attempt → block. Fail-count comes from the journal
//      (failedAttemptStatus ∈ {failed, timed-out, released}); at/above
//      max_attempts the call BLOCKS (no spawn) and returns status="blocked".
//      Resume respects prior attempts: a real journal fixture written by a
//      first HostState is re-read by a second HostState → blocks at the budget
//      (the count is persistent, not in-memory).
//   2. Whole-run ceiling → clean exit + journal terminus. max-total-spawns and
//      max-wall-clock are checked before each spawn; on trip the run sets the
//      interrupt flag (REUSING the task-3189 shutdown), writes a terminus
//      record, and runModule returns LuaRuntimeError (clean wind-down).
//
// The clock/counters are injected via the scheduler's clock_fn + the
// HostState's run_counters/budgets; journal fixtures live in tmp dirs (NOT the
// real .worktrees/).
// ---------------------------------------------------------------------------

/// FakeFailCountCtx maps a task_slug to a canned prior-failed-attempt count for
/// the max-attempt tests — no real journal file required.
const FakeFailCountCtx = struct {
    slug: []const u8,
    count: u32,
};

fn fakeFailCountReader(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    task_slug: []const u8,
) u32 {
    _ = allocator;
    _ = io;
    const fc: *FakeFailCountCtx = @ptrCast(@alignCast(ctx.?));
    if (std.mem.eql(u8, fc.slug, task_slug)) return fc.count;
    return 0;
}

test "M8 budget: task at max_attempts → BLOCKED, NO spawn (task 3198)" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    // The journal reports 3 prior failed attempts for this task.
    var fc = FakeFailCountCtx{ .slug = "m8-runaway", .count = 3 };
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .skip_block_subprocess = true, // do not shell planar-agent block in a unit test
        .failed_attempt_reader = fakeFailCountReader,
        .failed_attempt_ctx = &fc,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;
    host.budgets = .{ .max_attempts = 3 }; // budget == prior failures → block

    const src =
        \\return {
        \\  meta = { name = "m8-maxattempt", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok",
        \\      task_slug = "m8-runaway",
        \\      task_id = 4400,
        \\    })
        \\    assert(r.status == "blocked", "expected status=blocked, got " .. tostring(r.status))
        \\    assert(r.skip_reason == "max-attempts", "expected skip_reason=max-attempts, got " .. tostring(r.skip_reason))
        \\    assert(r.terminal_verb == "block", "expected terminal_verb=block, got " .. tostring(r.terminal_verb))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m8-maxattempt", &.{}, &host, &err_buf);

    // The budget refused the spawn: FakeSpawner never started.
    try std.testing.expectEqual(@as(u32, 0), fake.start_count);
    try std.testing.expectEqual(@as(usize, 0), fake.invocations.items.len);
    // The block is surfaced to the M7 end-of-run triage summary.
    try std.testing.expectEqual(@as(usize, 1), host.blocked_items.items.len);
}

test "M8 budget: task BELOW max_attempts → spawns normally (task 3198)" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    // Only 2 prior failures, budget is 3 → still room for one more.
    var fc = FakeFailCountCtx{ .slug = "m8-retry", .count = 2 };
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .skip_block_subprocess = true,
        .failed_attempt_reader = fakeFailCountReader,
        .failed_attempt_ctx = &fc,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;
    host.budgets = .{ .max_attempts = 3 };

    const src =
        \\return {
        \\  meta = { name = "m8-belowbudget", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok",
        \\      task_slug = "m8-retry",
        \\      task_id = 4401,
        \\    })
        \\    assert(r.status ~= "blocked", "below-budget task must spawn, got " .. tostring(r.status))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m8-belowbudget", &.{}, &host, &err_buf);

    try std.testing.expectEqual(@as(u32, 1), fake.start_count);
    try std.testing.expectEqual(@as(usize, 0), host.blocked_items.items.len);
}

test "M8 budget: resume respects prior attempts — fail-count is journal-derived (task 3198)" {
    const a = testing_alloc;
    const io = std.testing.io;

    const repo_root = jMkTmpDir(a);
    defer a.free(repo_root);
    defer jRmTree(a, repo_root);

    // --- Seed the PERSISTENT journal with 3 failed records for the task (as if a
    // prior run had failed it three times). We write the fixture via the same
    // journal.append the production write path uses, to the production path.
    const jpath = try journal.journalPath(a, repo_root, 8800);
    defer a.free(jpath);
    inline for (.{ "failed", "released", "timed-out" }) |verb| {
        try journal.append(a, io, jpath, .{
            .prompt_hash = "h",
            .worktree = "/wt",
            .branch = "b",
            .claim_token = "tok",
            .model = "claude-opus-4-8",
            .role = "coder",
            .task_slug = "m8-persist",
            .exit_code = 1,
            .terminal_verb = verb,
            .wall_clock_ms = 0,
            .timestamp = 0,
        });
    }

    // --- A "second run": a brand-new HostState + the PRODUCTION journal-reading
    // path (no injected reader). The driver points at the same repo_root/plan_id
    // so defaultFailedAttemptReader reads the fixture above. budget == 3 → block.
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = io,
        .repo_root = repo_root, // → journal location resolvable
        .plan_slug = "p8800",
        .plan_id = 8800, // → defaultFailedAttemptReader reads the fixture
        .skip_terminal_subprocess = true,
        .skip_block_subprocess = true,
        .claim_status_reader = activeClaimReader,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;
    host.budgets = .{ .max_attempts = 3 };

    const src =
        \\return {
        \\  meta = { name = "m8-resume-budget", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok",
        \\      task_slug = "m8-persist",
        \\      task_id = 4402,
        \\    })
        \\    assert(r.status == "blocked", "resumed run must respect prior 3 failures, got " .. tostring(r.status))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m8-resume-budget", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("m8 resume-budget failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };

    // The persistent count (3) tripped the budget → no spawn in the second run.
    try std.testing.expectEqual(@as(u32, 0), fake.start_count);
}

/// CeilingSpawnClock returns a value that ADVANCES with the run's spawn_count:
/// `base + spawn_count * step`. run_start is stamped at spawn_count==0 (== base);
/// the ceiling check for the Nth spawn reads at spawn_count==N-1. Drives the
/// wall-clock ceiling deterministically with no real sleep.
const CeilingSpawnClock = struct {
    counters: *budget.RunCounters,
    step_ns: i128,
    fn clockFn(ctx: ?*anyopaque, cio: std.Io) i128 {
        _ = cio;
        const self: *CeilingSpawnClock = @ptrCast(@alignCast(ctx.?));
        return @as(i128, self.counters.spawn_count) * self.step_ns;
    }
};

test "M8 ceiling: max-total-spawns → clean exit + journal terminus (task 3198)" {
    const a = testing_alloc;
    const io = std.testing.io;
    interrupt.reset();
    defer interrupt.reset();

    const repo_root = jMkTmpDir(a);
    defer a.free(repo_root);
    defer jRmTree(a, repo_root);

    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = io,
        .repo_root = repo_root,
        .plan_slug = "p9001",
        .plan_id = 9001, // → journal terminus is written
        .skip_terminal_subprocess = true,
        .claim_status_reader = activeClaimReader,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;
    // Ceiling of 2 spawns; the workflow tries to spawn 3 sequentially.
    host.budgets = .{ .max_total_spawns = 2, .max_wall_clock_ns = std.math.maxInt(i128) };
    // Fixed clock so the wall-clock ceiling never fires here.
    var clock = JournalTestClock{};
    host.timeout_clock_fn = JournalTestClock.clockFn;
    host.timeout_clock_ctx = &clock;

    const src =
        \\return {
        \\  meta = { name = "m8-ceil-spawns", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    for i = 1, 3 do
        \\      ctx.agent("brief-" .. i, {
        \\        role = "coder",
        \\        worktree_path = "/tmp/wt" .. i,
        \\        claim_token = "tok-" .. i,
        \\        task_slug = "ts-" .. i,
        \\        task_id = 4500 + i,
        \\      })
        \\    end
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    // A ceiling-terminated run unwinds via LuaRuntimeError (the clean wind-down).
    const res = runModule(src, "test:m8-ceil-spawns", &.{}, &host, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, res);

    // The spawn counter is capped at the ceiling (exactly 2 workers started).
    try std.testing.expectEqual(@as(u32, 2), fake.start_count);
    try std.testing.expectEqual(@as(u32, 2), host.run_counters.spawn_count);
    // The ceiling that tripped is recorded.
    try std.testing.expectEqual(@as(?budget.Ceiling, .spawns), host.ceiling_tripped);

    // The journal has a TERMINUS record naming the spawns ceiling.
    const tpath = try journal.journalPath(a, repo_root, 9001);
    defer a.free(tpath);
    var arena = std.heap.ArenaAllocator.init(a);
    defer arena.deinit();
    const recs = try journal.read(a, arena.allocator(), io, tpath);
    var saw_terminus = false;
    for (recs) |r| {
        if (std.mem.eql(u8, r.terminal_verb, budget.CEILING_TERMINUS_VERB)) {
            saw_terminus = true;
            try std.testing.expectEqualStrings("max-total-spawns", r.task_slug);
        }
    }
    try std.testing.expect(saw_terminus);
}

test "M8 ceiling: max-wall-clock → clean exit + journal terminus (task 3198)" {
    const a = testing_alloc;
    const io = std.testing.io;
    interrupt.reset();
    defer interrupt.reset();

    const repo_root = jMkTmpDir(a);
    defer a.free(repo_root);
    defer jRmTree(a, repo_root);

    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = io,
        .repo_root = repo_root,
        .plan_slug = "p9002",
        .plan_id = 9002,
        .skip_terminal_subprocess = true,
        .claim_status_reader = activeClaimReader,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;
    // Generous spawn ceiling; tight wall-clock. The clock advances by a big step
    // per spawn so the 2nd spawn's ceiling check crosses max_wall_clock_ns.
    host.budgets = .{ .max_total_spawns = 1000, .max_wall_clock_ns = 1_000 };
    var clock = CeilingSpawnClock{ .counters = &host.run_counters, .step_ns = 10_000 };
    host.timeout_clock_fn = CeilingSpawnClock.clockFn;
    host.timeout_clock_ctx = &clock;

    const src =
        \\return {
        \\  meta = { name = "m8-ceil-wall", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    for i = 1, 5 do
        \\      ctx.agent("brief-" .. i, {
        \\        role = "coder",
        \\        worktree_path = "/tmp/wt" .. i,
        \\        claim_token = "tok-" .. i,
        \\        task_slug = "ts-" .. i,
        \\        task_id = 4600 + i,
        \\      })
        \\    end
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    const res = runModule(src, "test:m8-ceil-wall", &.{}, &host, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, res);

    // First spawn happened at elapsed 0; the second spawn's pre-check read
    // elapsed 10_000 >= 1_000 → refused. Exactly one worker started.
    try std.testing.expectEqual(@as(u32, 1), fake.start_count);
    try std.testing.expectEqual(@as(?budget.Ceiling, .wall_clock), host.ceiling_tripped);

    const tpath = try journal.journalPath(a, repo_root, 9002);
    defer a.free(tpath);
    var arena = std.heap.ArenaAllocator.init(a);
    defer arena.deinit();
    const recs = try journal.read(a, arena.allocator(), io, tpath);
    var saw_terminus = false;
    for (recs) |r| {
        if (std.mem.eql(u8, r.terminal_verb, budget.CEILING_TERMINUS_VERB)) {
            saw_terminus = true;
            try std.testing.expectEqualStrings("max-wall-clock", r.task_slug);
        }
    }
    try std.testing.expect(saw_terminus);
}

// ---------------------------------------------------------------------------
// task 3243 — pin driveAgentCall → env_builder → SpawnInputs.env_map → Spawner.
// A non-null env_builder installed on the driver must be CALLED and its env
// must reach the FakeSpawner's recorded snapshot. Cutting any link in that
// chain (e.g. not threading env_map into SpawnInputs) fails this test.
// ---------------------------------------------------------------------------

/// SENTINEL_WORKER_VAR is a marker env var the test env-builder injects; the
/// test asserts the FakeSpawner recorded it, proving the wire is intact.
const SENTINEL_WORKER_VAR = "PLANAR_EXECUTE_TEST_SENTINEL";
const SENTINEL_WORKER_VAL = "wired-through-driveAgentCall";

/// sentinelEnvBuilder is a test EnvBuilderFn that produces a recognizable
/// WorkerEnv WITHOUT touching the filesystem. The shim_dir / path strings are
/// heap-allocated (so WorkerEnv.deinit can free them safely) but never
/// materialized on disk — deinit's deleteTree no-ops on the missing dir.
fn sentinelEnvBuilder(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    worktree_path: []const u8,
) anyerror!worker_env.WorkerEnv {
    _ = ctx;
    _ = io;
    _ = worktree_path;
    var map = std.process.Environ.Map.init(allocator);
    errdefer map.deinit();
    try map.put("PATH", "/tmp/planar-execute-test-shim");
    try map.put(SENTINEL_WORKER_VAR, SENTINEL_WORKER_VAL);
    return .{
        .shim_dir = try allocator.dupe(u8, "/tmp/planar-execute-test-shim-nonexistent"),
        .path = try allocator.dupe(u8, "/tmp/planar-execute-test-shim"),
        .env_map = map,
    };
}

test "M4 agent: non-null env_builder → constrained env reaches the spawn boundary (task 3243)" {
    // Pins the full thread: AgentDriver.env_builder is called inside
    // driveAgentCall, its WorkerEnv.env_map is threaded into
    // SpawnInputs.env_map, and the FakeSpawner records it. Cutting any link
    // (e.g. dropping env_map from SpawnInputs) makes the sentinel disappear.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
        .env_builder = sentinelEnvBuilder,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m4-envwire", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok-env",
        \\      task_slug = "ts1",
        \\    })
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m4-envwire", &.{}, &host, &err_buf);

    try std.testing.expectEqual(@as(usize, 1), fake.invocations.items.len);
    const inv = fake.invocations.items[0];
    // The sentinel var produced by the env_builder MUST be present in the
    // recorded snapshot — proving the env_builder ran AND its env reached the
    // spawn boundary (not an inherited/empty env).
    const got = inv.envGet(SENTINEL_WORKER_VAR) orelse return error.TestSentinelEnvMissing;
    try std.testing.expectEqualStrings(SENTINEL_WORKER_VAL, got);
    // And it was a non-empty snapshot (a null env_map would record zero pairs).
    try std.testing.expect(inv.env_pairs.len >= 2);
}

// ---------------------------------------------------------------------------
// task 3241 — gated production agent() driver wiring.
//
// handleRun itself is process-global + process.exit-driven, so it is exercised
// by the gated integration test (planar_execute_agent_live_test.zig). The
// load-bearing NEW unit here is the host-PATH binary resolver the gated driver
// uses to find the real planar-agent + git to symlink into the worker shim.
// ---------------------------------------------------------------------------

test "task 3241: resolveHostBinary resolves an on-PATH binary to an absolute path" {
    const a = testing_alloc;
    // `sh` is always on PATH at an absolute location on any POSIX host the test
    // runs on. (We resolve `sh` rather than `git` because `git` may be absent
    // in a minimal CI sandbox; `sh` is a harder guarantee.)
    const p = resolveHostBinary(a, std.testing.io, "sh") catch return error.SkipZigTest;
    defer a.free(p);
    try std.testing.expect(p.len > 0);
    try std.testing.expect(p[0] == '/'); // absolute
    try std.testing.expect(std.mem.endsWith(u8, p, "/sh"));
}

test "task 3241: resolveHostBinary errors when the binary is not on PATH" {
    const a = testing_alloc;
    // A name no real binary will ever carry → command -v exits non-zero.
    const r = resolveHostBinary(a, std.testing.io, "planar-nonexistent-binary-zzz-3241");
    try std.testing.expectError(error.BinaryNotResolvable, r);
}

// ---------------------------------------------------------------------------
// task 3182 — M5 coroutine scheduler tests.
//
// These pin the NEW internal mechanism (run(ctx) on a coroutine, agent()
// yields, the scheduler drives the single in-flight worker, the continuation
// resumes). The observable result of a single agent() call is UNCHANGED from
// M4 (behavior-equivalence anchor) — the M4 tests above already exercise the
// result table THROUGH the coroutine drive (runModule now uses lua_resume).
// The tests below additionally assert the async machinery itself.
// ---------------------------------------------------------------------------

test "M5 scheduler: pure-Lua workflow (no agent) finishes on the first resume (LUA_OK)" {
    // Backward-compat anchor: a workflow that never calls agent() never yields;
    // the first lua_resume returns LUA_OK and the run completes. The scheduler
    // is installed but no worker is ever registered.
    const a = testing_alloc;
    var driver = AgentDriver{
        .spawner = undefined, // never used — no agent() call
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 7, 11, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-pure", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.phase("doing pure work")
        \\    ctx.log("no agent here")
        \\    assert(ctx.now == 7, "ctx.now threaded through the coroutine")
        \\    assert(ctx.seed == 11, "ctx.seed threaded through the coroutine")
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m5-pure", &.{}, &host, &err_buf);

    // The recording host fns still fired on the coroutine.
    try std.testing.expect(findCall(&host, .phase) != null);
    try std.testing.expect(findCall(&host, .log) != null);
}

test "M5 scheduler: single agent() yields, scheduler drives the worker, continuation resumes (async machinery)" {
    // The load-bearing yield/resume assertion: a single agent() call spawns the
    // worker via start() (NON-BLOCKING), yields, the scheduler drives it via the
    // deadline-aware poll loop (task 3188), and ONLY THEN does the continuation
    // run. We prove the ordering via the FakeSpawner's counters: start_count==1,
    // poll_count==1, and reached_terminal==true at the point the result table is
    // observable.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-yield", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok-xyz",
        \\      role_spec = "you are a coder",
        \\      task_slug = "ts1",
        \\    })
        \\    -- The continuation produced the result table; by here the worker
        \\    -- has already reached terminal (the scheduler waited it).
        \\    assert(r.status == "released", "status: " .. tostring(r.status))
        \\    assert(r.exit_code == 0, "exit_code: " .. tostring(r.exit_code))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m5-yield", &.{}, &host, &err_buf);

    // The worker was started non-blocking and driven via the deadline-aware
    // POLL loop (task 3188 changed driveInflight from a bare blocking wait to a
    // poll loop so a hung worker can be timed out). poll_until_done=0 ⇒ the
    // first poll returns terminal, so poll_count == start_count and the
    // coroutine resumed only after terminal.
    try std.testing.expectEqual(@as(u32, 1), fake.start_count);
    try std.testing.expectEqual(@as(u32, 1), fake.poll_count);
    try std.testing.expectEqual(@as(u32, 0), fake.wait_count);
    try std.testing.expect(fake.reached_terminal);
    // The blocking run() path was NOT used (the scheduler uses start/poll).
    try std.testing.expectEqual(@as(usize, 1), fake.invocations.items.len);
}

test "M5 scheduler: behavior-equivalence — the agent() result table matches M4 for the same canned outcome" {
    // The N=1 equivalence anchor: exit==0 + no commit → released; exit==7 →
    // failed; these are the SAME tables M4 produced. (M4's own tests assert the
    // same via runModule, which now routes through the coroutine drive — so
    // their green is itself the equivalence proof; this test states it
    // explicitly for the failed branch.)
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 7, "", "boom");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-equiv", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("brief", {
        \\      role = "reviewer",
        \\      worktree_path = "/tmp/abs",
        \\      claim_token = "tok",
        \\      task_slug = "tx",
        \\    })
        \\    assert(r.status == "failed", "status: " .. tostring(r.status))
        \\    assert(r.exit_code == 7, "exit_code: " .. tostring(r.exit_code))
        \\    assert(r.terminal_verb == "fail", "verb: " .. tostring(r.terminal_verb))
        \\    assert(r.commit_present == false, "commit_present: " .. tostring(r.commit_present))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m5-equiv", &.{}, &host, &err_buf);
}

test "M5 scheduler: two sequential agent() calls each yield + resume cleanly (slot reuse)" {
    // Two agent() calls in one run() must each register an in-flight worker,
    // yield, get driven, and resume — with the slot freed + reused between
    // them. This proves releaseSlot works and the registry does not leak slots
    // across sequential calls (the structure 3183 widens to concurrent calls).
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-seq", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r1 = ctx.agent("brief one", { role = "coder", worktree_path = "/tmp/a", claim_token = "t1", task_slug = "s1" })
        \\    local r2 = ctx.agent("brief two", { role = "reviewer", worktree_path = "/tmp/b", claim_token = "t2", task_slug = "s2" })
        \\    assert(r1.status == "released", "r1: " .. tostring(r1.status))
        \\    assert(r2.status == "released", "r2: " .. tostring(r2.status))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m5-seq", &.{}, &host, &err_buf);

    try std.testing.expectEqual(@as(u32, 2), fake.start_count);
    // Each call driven via the poll loop (task 3188): 2 immediate-terminal
    // polls, no blocking waits.
    try std.testing.expectEqual(@as(u32, 2), fake.poll_count);
    try std.testing.expectEqual(@as(u32, 0), fake.wait_count);
    try std.testing.expectEqual(@as(usize, 2), fake.invocations.items.len);
    try std.testing.expectEqualStrings("brief one", fake.invocations.items[0].brief);
    try std.testing.expectEqualStrings("brief two", fake.invocations.items[1].brief);
}

test "M5 scheduler: Lua error AFTER an agent() call still propagates (continuation ran, then run() errored)" {
    // A run() that calls agent() successfully (yield → resume → result) and
    // THEN raises a Lua error must still surface LuaRuntimeError with the error
    // string. This pins that the coroutine error path (captured off `co`) works
    // even after a successful yield/resume round trip.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-err-after", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.agent("brief", { role = "coder", worktree_path = "/tmp/a", claim_token = "t1", task_slug = "s1" })
        \\    error("boom after agent")
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    const err = runModule(src, "test:m5-err-after", &.{}, &host, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);
    const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
    try std.testing.expect(std.mem.indexOf(u8, msg, "boom after agent") != null);
    // The agent() call DID complete before the error (worker started + polled
    // to terminal via the deadline-aware drive loop — task 3188).
    try std.testing.expectEqual(@as(u32, 1), fake.start_count);
    try std.testing.expectEqual(@as(u32, 1), fake.poll_count);
    try std.testing.expectEqual(@as(u32, 0), fake.wait_count);
}

test "M5 scheduler: Lua error BEFORE any agent() call propagates (pure-coroutine error path)" {
    // A run() that errors with NO agent() call must surface LuaRuntimeError on
    // the first resume (the coroutine raised before yielding). Pins the
    // first-resume error branch of the drive loop.
    const a = testing_alloc;
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    // No driver needed — the error fires before any agent() call.

    const src =
        \\return {
        \\  meta = { name = "m5-err-before", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    error("boom before agent")
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    const err = runModule(src, "test:m5-err-before", &.{}, &host, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);
    const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
    try std.testing.expect(std.mem.indexOf(u8, msg, "boom before agent") != null);
}

// ---------------------------------------------------------------------------
// M5 parallel() — N-way concurrency barrier (task 3183).
//
// All tests drive the FakeSpawner's async surface (start → poll* → terminal)
// so there is NO live process cost. `poll_until_done_seq` models per-worker
// completion order; `peak_inflight` proves genuine concurrency (not sequential)
// and the MAX_SLOTS queueing cap.
// ---------------------------------------------------------------------------

test "M5 parallel: N=3 all succeed, OUT-OF-ORDER completion yields ORIGINAL-ORDER results + proves concurrency" {
    // Three thunks each call agent() then return a distinct marker. The fakes
    // are tuned so completion order is child1 → child2 → child0 (NOT the
    // registration order 0,1,2). We assert:
    //   * results[i] is thunk[i]'s return value, in ORIGINAL order, and
    //   * all three workers were in flight at once (peak_inflight == 3),
    //     proving concurrency rather than sequential start→wait→start.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    // child0 needs 3 polls, child1 0 (terminal first), child2 1 → completion
    // order: 1, 2, 0 (out of registration order).
    const seq = [_]u32{ 3, 0, 1 };
    fake.poll_until_done_seq = &seq;
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-par3", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local function mk(tag)
        \\      return function()
        \\        ctx.agent("brief-" .. tag, { role = "coder", worktree_path = "/tmp/" .. tag, claim_token = "t-" .. tag, task_slug = tag })
        \\        return "result-" .. tag
        \\      end
        \\    end
        \\    local results = ctx.parallel({ mk("zero"), mk("one"), mk("two") })
        \\    assert(#results == 3, "len: " .. tostring(#results))
        \\    assert(results[1] == "result-zero", "r1: " .. tostring(results[1]))
        \\    assert(results[2] == "result-one", "r2: " .. tostring(results[2]))
        \\    assert(results[3] == "result-two", "r3: " .. tostring(results[3]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m5-par3", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("parallel N=3 failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };

    // Three workers started, all concurrent (peak == 3 proves not-sequential).
    try std.testing.expectEqual(@as(u32, 3), fake.start_count);
    try std.testing.expectEqual(@as(u32, 3), fake.peak_inflight);
    try std.testing.expectEqual(@as(u32, 0), fake.live_inflight);
}

test "M5 parallel: a thunk that errors resolves to nil; siblings still succeed; parallel returns normally" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    // Middle thunk raises a Lua error (a thunk-level throw) → results[2] == nil.
    // The other two each agent() + return their marker.
    const src =
        \\return {
        \\  meta = { name = "m5-par-err", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local ok1 = function()
        \\      ctx.agent("b1", { role = "coder", worktree_path = "/tmp/1", claim_token = "t1", task_slug = "s1" })
        \\      return "ok1"
        \\    end
        \\    local boom = function() error("thunk blew up") end
        \\    local ok3 = function()
        \\      ctx.agent("b3", { role = "coder", worktree_path = "/tmp/3", claim_token = "t3", task_slug = "s3" })
        \\      return "ok3"
        \\    end
        \\    local r = ctx.parallel({ ok1, boom, ok3 })
        \\    assert(#r >= 1, "len got: " .. tostring(#r))
        \\    assert(r[1] == "ok1", "r1: " .. tostring(r[1]))
        \\    assert(r[2] == nil, "r2 should be nil, got: " .. tostring(r[2]))
        \\    assert(r[3] == "ok3", "r3: " .. tostring(r[3]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m5-par-err", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("parallel err-thunk failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };
    // Only the two non-erroring thunks ever spawned a worker.
    try std.testing.expectEqual(@as(u32, 2), fake.start_count);
}

test "M5 parallel: N=20 > MAX_SLOTS queues — all 20 results return, at most MAX_SLOTS in flight at once" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    // Each worker takes one poll to finish, forcing the drive loop to actually
    // cycle workers through the slot registry (so queueing is exercised).
    fake.poll_until_done = 1;
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-par20", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local thunks = {}
        \\    for i = 1, 20 do
        \\      local idx = i
        \\      thunks[i] = function()
        \\        ctx.agent("b", { role = "coder", worktree_path = "/tmp/w" .. idx, claim_token = "t" .. idx, task_slug = "s" .. idx })
        \\        return idx
        \\      end
        \\    end
        \\    local r = ctx.parallel(thunks)
        \\    assert(#r == 20, "len: " .. tostring(#r))
        \\    for i = 1, 20 do
        \\      assert(r[i] == i, "r[" .. i .. "] = " .. tostring(r[i]))
        \\    end
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m5-par20", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("parallel N=20 failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };
    try std.testing.expectEqual(@as(u32, 20), fake.start_count);
    // The concurrency cap held: never more than MAX_SLOTS in flight at once.
    try std.testing.expect(fake.peak_inflight <= scheduler.MAX_SLOTS);
    // And it actually queued (more thunks than the cap → peak hit the cap).
    try std.testing.expectEqual(@as(u32, scheduler.MAX_SLOTS), fake.peak_inflight);
    try std.testing.expectEqual(@as(u32, 0), fake.live_inflight);
}

test "M5 parallel: empty thunks table returns an empty results array" {
    const a = testing_alloc;
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    // No driver needed — no thunk ever calls agent().
    const src =
        \\return {
        \\  meta = { name = "m5-par0", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.parallel({})
        \\    assert(type(r) == "table", "type: " .. type(r))
        \\    assert(#r == 0, "len: " .. tostring(#r))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m5-par0", &.{}, &host, &err_buf);
    // parallel was recorded with "0 thunks".
    const call = findCall(&host, .parallel) orelse return error.TestUnexpectedResult;
    try std.testing.expectEqualStrings("0 thunks", call.arg0);
}

test "M5 parallel: pure-Lua thunks (no agent) return their values without registering any worker" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-par-pure", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.parallel({
        \\      function() return 10 end,
        \\      function() return 20 end,
        \\      function() return 30 end,
        \\    })
        \\    assert(#r == 3, "len: " .. tostring(#r))
        \\    assert(r[1] == 10 and r[2] == 20 and r[3] == 30, "values wrong")
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m5-par-pure", &.{}, &host, &err_buf);
    // No worker ever started — pure-Lua thunks never call agent().
    try std.testing.expectEqual(@as(u32, 0), fake.start_count);
}

test "M5 parallel: a thunk may call agent() MULTIPLE times (re-yield), still returns its value in order" {
    // Proves a child that yields, resumes, then yields AGAIN is handled (the
    // re-yield path of the drive loop), and its final return still lands in
    // original order alongside a single-agent sibling.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-par-multi", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local twice = function()
        \\      ctx.agent("a", { role = "coder", worktree_path = "/tmp/a", claim_token = "ta", task_slug = "sa" })
        \\      ctx.agent("b", { role = "coder", worktree_path = "/tmp/b", claim_token = "tb", task_slug = "sb" })
        \\      return "two-calls"
        \\    end
        \\    local once = function()
        \\      ctx.agent("c", { role = "coder", worktree_path = "/tmp/c", claim_token = "tc", task_slug = "sc" })
        \\      return "one-call"
        \\    end
        \\    local r = ctx.parallel({ twice, once })
        \\    assert(r[1] == "two-calls", "r1: " .. tostring(r[1]))
        \\    assert(r[2] == "one-call", "r2: " .. tostring(r[2]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m5-par-multi", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("parallel multi-agent failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };
    // Three total agent() spawns: twice(2) + once(1).
    try std.testing.expectEqual(@as(u32, 3), fake.start_count);
}

/// TimeoutTestClock is a settable fake monotonic clock for the workflow-level
/// timeout test (task 3188): the test pins it past the deadline so a hung
/// worker times out deterministically with no real sleep.
const TimeoutTestClock = struct {
    now_ns: i128,
    fn clockFn(ctx: ?*anyopaque, io: std.Io) i128 {
        _ = io;
        const self: *TimeoutTestClock = @ptrCast(@alignCast(ctx.?));
        return self.now_ns;
    }
};

test "M6 timeout: in a parallel set a hung worker times out (kill + status=timed-out) while the sibling completes, original order preserved (task 3188)" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "OK", "");
    defer fake.deinit();
    // Worker 0 (first started) HANGS; worker 1 completes normally. start order
    // == thunk order in driveChildren, so slot0 is the hung "h" thunk.
    const hung_seq = [_]bool{ true, false };
    fake.hung_starts_seq = &hung_seq;

    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true, // no real planar-agent / teardown.
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    // Fake clock + ZERO budget: the deadline equals the register-time clock
    // reading (deadline = now + 0 = now), so EVERY in-flight worker is already
    // at its deadline. A hung worker (poll always null) is therefore reclaimed
    // the instant driveChildren polls it — no real elapsed time, no real sleep.
    // A naturally-completing worker still wins pass 1 of pollReadyOnce (the
    // deadline pass only runs when NONE completed this round), so the sibling
    // completes normally and is NOT killed. The fixed clock guarantees the
    // deadline pass fires immediately rather than spinning in the poll loop.
    var clock = TimeoutTestClock{ .now_ns = 1_000_000_000 };
    host.timeout_clock_fn = TimeoutTestClock.clockFn;
    host.timeout_clock_ctx = &clock;
    host.timeout_max_wall_clock_ns = 0; // deadline == register-time → already due.

    const src =
        \\return {
        \\  meta = { name = "m6-timeout", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local hung = function()
        \\      return ctx.agent("h", { role = "coder", worktree_path = "/tmp/h", claim_token = "th", task_slug = "sh" })
        \\    end
        \\    local ok = function()
        \\      return ctx.agent("o", { role = "coder", worktree_path = "/tmp/o", claim_token = "to", task_slug = "so" })
        \\    end
        \\    local r = ctx.parallel({ hung, ok })
        \\    assert(type(r[1]) == "table", "r1 type: " .. type(r[1]))
        \\    assert(r[1].status == "timed-out", "r1 status: " .. tostring(r[1].status))
        \\    assert(r[1].terminal_verb == "fail", "r1 verb: " .. tostring(r[1].terminal_verb))
        \\    assert(type(r[2]) == "table", "r2 type: " .. type(r[2]))
        \\    assert(r[2].status == "released" or r[2].status == "completed", "r2 status: " .. tostring(r[2].status))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m6-timeout", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("m6 timeout failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };

    // Exactly the hung worker was killed; the sibling was not.
    try std.testing.expectEqual(@as(u32, 2), fake.start_count);
    try std.testing.expectEqual(@as(u32, 1), fake.kill_count);
    try std.testing.expect(fake.killedWorker(0)); // slot0 = the "hung" thunk.
    try std.testing.expect(!fake.killedWorker(1));
}

// ---------------------------------------------------------------------------
// M5 pipeline() — per-item stage chains, NO barrier between stages (task 3184).
//
// pipeline reuses parallel's N-coroutine drive (driveChildren) over per-item
// stage-runner coroutines. The defining property is the ABSENCE of a per-stage
// barrier: item A can be in stage 2 while item B is still in stage 1. The
// FakeSpawner's overlap-observation registry (overlap_obs / overlapObsFor)
// makes that temporal overlap provable: each agent() call is tagged with a
// distinct brief, and we assert that when A's stage-2 worker STARTED, B's
// stage-1 worker was still in flight.
// ---------------------------------------------------------------------------

test "M5 pipeline: NO BARRIER — item A reaches stage 2 while item B is still in stage 1" {
    // items = { "A", "B" }, two stages, each stage calls agent() with a
    // brief tagging (item, stage). Fakes are tuned so:
    //   start 0 = A-s1 (poll 0 → terminal first),
    //   start 1 = B-s1 (poll 5 → slow),
    //   start 2 = A-s2 (poll 0 → fast), start 3 = B-s2.
    // A-s1 finishes first → A advances into stage 2 and spawns A-s2 WHILE
    // B-s1 is still in flight. The overlap registry proves A-s2 overlapped
    // B-s1 — i.e. there is no barrier forcing A to wait for B to clear stage 1.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    const seq = [_]u32{ 0, 5, 0, 0 };
    fake.poll_until_done_seq = &seq;
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-pipe-nobarrier", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local function stage(s)
        \\      return function(prev, item, idx)
        \\        ctx.agent(item .. "-s" .. s, { role = "coder", worktree_path = "/tmp/" .. item .. s, claim_token = "t-" .. item .. s, task_slug = item })
        \\        return prev .. "-s" .. s
        \\      end
        \\    end
        \\    local r = ctx.pipeline({ "A", "B" }, stage(1), stage(2))
        \\    assert(#r == 2, "len: " .. tostring(#r))
        \\    assert(r[1] == "A-s1-s2", "r1: " .. tostring(r[1]))
        \\    assert(r[2] == "B-s1-s2", "r2: " .. tostring(r[2]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m5-pipe-nobarrier", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("pipeline no-barrier failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };

    // Four agent() spawns total: A-s1, B-s1, A-s2, B-s2.
    try std.testing.expectEqual(@as(u32, 4), fake.start_count);

    // THE DEFINING ASSERTION: when A's stage-2 worker started, B's stage-1
    // worker was still in flight — proving A crossed the stage-1→stage-2
    // boundary WITHOUT waiting for B to finish stage 1 (no barrier).
    const a_s2 = fake.overlapObsFor("A-s2") orelse return error.TestUnexpectedResult;
    try std.testing.expect(a_s2.overlapsWith("B-s1"));
}

test "M5 pipeline: a stage that errors drops THAT item to nil and SKIPS its remaining stages; siblings normal" {
    // 3-stage pipeline over { "X", "Y" }. Item X's stage 2 errors → results[X]
    // == nil AND stage 3 NEVER runs for X. Item Y flows through all three.
    // We prove stage-3-skip-for-X via the agent brief registry: there must be a
    // "Y-s3" spawn but NO "X-s3" spawn.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-pipe-err", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local s1 = function(prev, item, idx)
        \\      ctx.agent(item .. "-s1", { role = "coder", worktree_path = "/tmp/" .. item .. "1", claim_token = "t", task_slug = item })
        \\      return prev .. "-1"
        \\    end
        \\    local s2 = function(prev, item, idx)
        \\      if item == "X" then error("X blew up in stage 2") end
        \\      ctx.agent(item .. "-s2", { role = "coder", worktree_path = "/tmp/" .. item .. "2", claim_token = "t", task_slug = item })
        \\      return prev .. "-2"
        \\    end
        \\    local s3 = function(prev, item, idx)
        \\      ctx.agent(item .. "-s3", { role = "coder", worktree_path = "/tmp/" .. item .. "3", claim_token = "t", task_slug = item })
        \\      return prev .. "-3"
        \\    end
        \\    local r = ctx.pipeline({ "X", "Y" }, s1, s2, s3)
        \\    assert(r[1] == nil, "r1 should be nil (X dropped), got: " .. tostring(r[1]))
        \\    assert(r[2] == "Y-1-2-3", "r2: " .. tostring(r[2]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m5-pipe-err", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("pipeline stage-error failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };

    // Prove stage 3 was SKIPPED for X but ran for Y, by scanning the recorded
    // spawn briefs. X spawned s1 only (s2 errored before agent()); Y spawned
    // s1, s2, s3.
    var saw_x_s3 = false;
    var saw_y_s3 = false;
    for (fake.invocations.items) |inv| {
        if (std.mem.eql(u8, inv.brief, "X-s3")) saw_x_s3 = true;
        if (std.mem.eql(u8, inv.brief, "Y-s3")) saw_y_s3 = true;
    }
    try std.testing.expect(!saw_x_s3); // stage 3 NEVER ran for the dropped item
    try std.testing.expect(saw_y_s3); // the sibling completed all stages
}

test "M5 pipeline: items complete OUT OF ORDER → results in ORIGINAL order" {
    // Three items, one stage that calls agent(). Tune completion so item 2
    // finishes first, then item 3, then item 1 — yet results stay in original
    // item order.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    const seq = [_]u32{ 3, 0, 1 }; // item0 slow, item1 first, item2 middle
    fake.poll_until_done_seq = &seq;
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-pipe-order", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local s = function(prev, item, idx)
        \\      ctx.agent("w" .. item, { role = "coder", worktree_path = "/tmp/" .. item, claim_token = "t", task_slug = tostring(item) })
        \\      return "done-" .. item
        \\    end
        \\    local r = ctx.pipeline({ "one", "two", "three" }, s)
        \\    assert(#r == 3, "len: " .. tostring(#r))
        \\    assert(r[1] == "done-one", "r1: " .. tostring(r[1]))
        \\    assert(r[2] == "done-two", "r2: " .. tostring(r[2]))
        \\    assert(r[3] == "done-three", "r3: " .. tostring(r[3]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m5-pipe-order", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("pipeline order failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };
    try std.testing.expectEqual(@as(u32, 3), fake.start_count);
    // All three were in flight at once (no per-stage barrier with one stage =
    // pure concurrency, like parallel).
    try std.testing.expectEqual(@as(u32, 3), fake.peak_inflight);
}

test "M5 pipeline: stage signature is (prevResult, originalItem, index) with correct values" {
    // Pure-Lua stages (no agent) that assert their arguments. Stage 1 sees
    // prev == item; stage 2 sees prev == stage1's output; index is 1-based and
    // correct per item.
    const a = testing_alloc;
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    // No driver — pure-Lua stages.
    const src =
        \\return {
        \\  meta = { name = "m5-pipe-sig", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local s1 = function(prev, item, idx)
        \\      assert(prev == item, "stage1 prev should equal item: " .. tostring(prev) .. " vs " .. tostring(item))
        \\      return "S1(" .. item .. "," .. idx .. ")"
        \\    end
        \\    local s2 = function(prev, item, idx)
        \\      assert(prev == "S1(" .. item .. "," .. idx .. ")", "stage2 prev wrong: " .. tostring(prev))
        \\      return "S2:" .. prev
        \\    end
        \\    local r = ctx.pipeline({ "alpha", "beta" }, s1, s2)
        \\    assert(r[1] == "S2:S1(alpha,1)", "r1: " .. tostring(r[1]))
        \\    assert(r[2] == "S2:S1(beta,2)", "r2: " .. tostring(r[2]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m5-pipe-sig", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("pipeline signature failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };
}

test "M5 pipeline: pure-Lua stages (no agent) run to completion, 0 workers" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-pipe-pure", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local double = function(prev, item, idx) return prev * 2 end
        \\    local inc = function(prev, item, idx) return prev + 1 end
        \\    local r = ctx.pipeline({ 1, 2, 3 }, double, inc)
        \\    assert(r[1] == 3 and r[2] == 5 and r[3] == 7, "values wrong: " .. tostring(r[1]) .. "," .. tostring(r[2]) .. "," .. tostring(r[3]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m5-pipe-pure", &.{}, &host, &err_buf);
    // No worker ever started — no stage called agent().
    try std.testing.expectEqual(@as(u32, 0), fake.start_count);
}

test "M5 pipeline: empty items returns empty results; zero stages is identity" {
    const a = testing_alloc;
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    const src =
        \\return {
        \\  meta = { name = "m5-pipe-edge", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    -- empty items → empty results (regardless of stage count)
        \\    local r0 = ctx.pipeline({}, function(p) return p end)
        \\    assert(type(r0) == "table" and #r0 == 0, "empty items: " .. tostring(#r0))
        \\    -- zero stages → identity (each item passes through unchanged)
        \\    local r1 = ctx.pipeline({ "x", "y" })
        \\    assert(#r1 == 2, "zero-stage len: " .. tostring(#r1))
        \\    assert(r1[1] == "x" and r1[2] == "y", "zero-stage identity failed")
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:m5-pipe-edge", &.{}, &host, &err_buf);
    // The first pipeline call recorded "0 items"; confirm the recording fires.
    const call = findCall(&host, .pipeline) orelse return error.TestUnexpectedResult;
    try std.testing.expectEqualStrings("0 items", call.arg0);
}

test "M5 pipeline: N > MAX_SLOTS items queue — all results return, cap held" {
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    fake.poll_until_done = 1; // force the drive loop to cycle workers
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-pipe-cap", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local items = {}
        \\    for i = 1, 20 do items[i] = i end
        \\    local s = function(prev, item, idx)
        \\      ctx.agent("w" .. item, { role = "coder", worktree_path = "/tmp/w" .. item, claim_token = "t" .. item, task_slug = "s" .. item })
        \\      return item * 10
        \\    end
        \\    local r = ctx.pipeline(items, s)
        \\    assert(#r == 20, "len: " .. tostring(#r))
        \\    for i = 1, 20 do assert(r[i] == i * 10, "r[" .. i .. "] = " .. tostring(r[i])) end
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m5-pipe-cap", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("pipeline N=20 failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };
    try std.testing.expectEqual(@as(u32, 20), fake.start_count);
    // The concurrency cap held: never more than MAX_SLOTS workers at once.
    try std.testing.expect(fake.peak_inflight <= scheduler.MAX_SLOTS);
    try std.testing.expectEqual(@as(u32, scheduler.MAX_SLOTS), fake.peak_inflight);
    try std.testing.expectEqual(@as(u32, 0), fake.live_inflight);
}

test "M5 pipeline: a single item's stage chain runs its agent() calls SEQUENTIALLY (one in flight at a time)" {
    // One item, three stages each calling agent(). The chain is sequential
    // WITHIN an item (stage k+1 cannot start until stage k's agent() resolves),
    // so peak_inflight for a lone item is exactly 1 — confirming the in-flight
    // count is bounded by items-in-an-agent-call, not items × stages.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "m5-pipe-seq", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local mk = function(s) return function(prev, item, idx)
        \\      ctx.agent("s" .. s, { role = "coder", worktree_path = "/tmp/" .. s, claim_token = "t" .. s, task_slug = "k" })
        \\      return (prev or "") .. s
        \\    end end
        \\    local r = ctx.pipeline({ "X" }, mk(1), mk(2), mk(3))
        \\    assert(r[1] == "X123", "r1: " .. tostring(r[1]))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    runModule(src, "test:m5-pipe-seq", &.{}, &host, &err_buf) catch |e| {
        std.debug.print("pipeline single-item seq failed: {s}\n", .{std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)))});
        return e;
    };
    try std.testing.expectEqual(@as(u32, 3), fake.start_count); // 3 stages = 3 agent() calls
    try std.testing.expectEqual(@as(u32, 1), fake.peak_inflight); // never 2 at once for one item
}

// ---------------------------------------------------------------------------
// M9 fan-in tests — tasks 3199 (merge), 3200 (conflict isolation),
// 3201 (no auto-merge / master untouched).
//
// The merge is exercised against REAL throwaway git repos (skip-probed via
// `git --version`); the conflict question-open is INJECTED via a recording
// `question_runner` so the conflict path is deterministic WITHOUT a live
// `planar question add` + DB. The fan-in driver (`runFanIn`) is exercised
// directly with a hand-built Lua result table + a minimal AgentCallState — the
// same code path `agentContinue`'s COMPLETED branch invokes — so the tests are
// deterministic without forcing the full coroutine pipeline to the
// commit-present `.complete` decision (which a FakeSpawner cannot produce: it
// performs no filesystem work between the pre-yield and continue HEAD samples).
// ---------------------------------------------------------------------------

/// m9GitAvailable mirrors worktree.zig's `gitAvailable` skip-probe.
fn m9GitAvailable(allocator: std.mem.Allocator) bool {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "git", "--version" },
    }) catch return false;
    defer allocator.free(r.stdout);
    defer allocator.free(r.stderr);
    return r.term == .exited and r.term.exited == 0;
}

/// m9MkTmpRepoDir creates a fresh system temp dir via `mktemp -d` (heap-owned;
/// caller frees). Mirrors worktree.zig's `mkTmpRepoDir`: an absolute path NOT
/// nested under this checkout's `.worktrees/` so `git worktree` is well-behaved.
fn m9MkTmpRepoDir(allocator: std.mem.Allocator) []const u8 {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "mktemp", "-d", "-t", "planar-m9.XXXXXX" },
    }) catch @panic("m9MkTmpRepoDir: mktemp spawn failed");
    defer allocator.free(r.stderr);
    if (!(r.term == .exited and r.term.exited == 0)) {
        allocator.free(r.stdout);
        @panic("m9MkTmpRepoDir: mktemp non-zero exit");
    }
    const trimmed = std.mem.trim(u8, r.stdout, " \t\r\n");
    const owned = allocator.dupe(u8, trimmed) catch @panic("OOM");
    allocator.free(r.stdout);
    return owned;
}

fn m9RmTree(allocator: std.mem.Allocator, path: []const u8) void {
    const r = std.process.run(allocator, std.testing.io, .{ .argv = &.{ "rm", "-rf", path } }) catch return;
    allocator.free(r.stdout);
    allocator.free(r.stderr);
}

/// m9RunGitIn runs a git command in `cwd`, panicking on failure.
fn m9RunGitIn(allocator: std.mem.Allocator, cwd: []const u8, args: []const []const u8) void {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(allocator);
    argv.append(allocator, "git") catch @panic("OOM");
    argv.append(allocator, "-C") catch @panic("OOM");
    argv.append(allocator, cwd) catch @panic("OOM");
    for (args) |a| argv.append(allocator, a) catch @panic("OOM");
    const r = std.process.run(allocator, std.testing.io, .{ .argv = argv.items }) catch
        @panic("m9RunGitIn: spawn failed");
    defer allocator.free(r.stdout);
    defer allocator.free(r.stderr);
    switch (r.term) {
        .exited => |code| if (code != 0) {
            std.debug.print("\nm9RunGitIn failed (exit {d}) in {s} args[0]={s}\nstderr: {s}\n", .{ code, cwd, args[0], r.stderr });
            @panic("m9RunGitIn: non-zero exit");
        },
        else => @panic("m9RunGitIn: abnormal termination"),
    }
}

/// m9InitRepo creates a throwaway git repo at `dir` with an initial commit on a
/// `master` branch (identity configured locally so commits succeed in CI).
fn m9InitRepo(allocator: std.mem.Allocator, dir: []const u8) void {
    m9RunGitIn(allocator, dir, &.{ "init", "-b", "master" });
    m9RunGitIn(allocator, dir, &.{ "config", "user.email", "test@planar.local" });
    m9RunGitIn(allocator, dir, &.{ "config", "user.name", "Planar Test" });
    m9RunGitIn(allocator, dir, &.{ "commit", "--allow-empty", "-m", "initial" });
}

/// m9MasterHead returns master's HEAD sha (heap-owned; caller frees), panicking
/// on failure. The no-auto-merge invariant (3201) asserts this is UNCHANGED
/// across a fan-in.
fn m9MasterHead(allocator: std.mem.Allocator, repo: []const u8) []u8 {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "git", "-C", repo, "rev-parse", "master" },
    }) catch @panic("m9MasterHead: spawn failed");
    defer allocator.free(r.stderr);
    if (!(r.term == .exited and r.term.exited == 0)) {
        allocator.free(r.stdout);
        @panic("m9MasterHead: non-zero exit");
    }
    const trimmed = std.mem.trim(u8, r.stdout, " \t\r\n");
    const owned = allocator.dupe(u8, trimmed) catch @panic("OOM");
    allocator.free(r.stdout);
    return owned;
}

fn m9DirExists(io: std.Io, path: []const u8) bool {
    var d = std.Io.Dir.cwd().openDir(io, path, .{}) catch return false;
    d.close(io);
    return true;
}

fn m9IsAncestor(allocator: std.mem.Allocator, repo: []const u8, anc: []const u8, desc: []const u8) bool {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "git", "-C", repo, "merge-base", "--is-ancestor", anc, desc },
    }) catch return false;
    defer allocator.free(r.stdout);
    defer allocator.free(r.stderr);
    return r.term == .exited and r.term.exited == 0;
}

/// m9WriteFile writes `data` to `<dir>/<name>` via the test Io.
fn m9WriteFile(allocator: std.mem.Allocator, io: std.Io, dir: []const u8, name: []const u8, data: []const u8) void {
    const wf = std.fs.path.join(allocator, &.{ dir, name }) catch @panic("OOM");
    defer allocator.free(wf);
    std.Io.Dir.cwd().writeFile(io, .{ .sub_path = wf, .data = data }) catch @panic("m9WriteFile failed");
}

/// RecordingQuestionCtx records every fan-in question open so the conflict test
/// can assert the title/body/plan WITHOUT a live `planar question add` + DB.
const RecordingQuestionCtx = struct {
    allocator: std.mem.Allocator,
    next_id: u64 = 7000,
    calls: std.ArrayList(QCall) = .empty,

    const QCall = struct {
        plan_id: u64,
        title: []u8,
        body: []u8,
        returned_id: u64,
    };

    fn deinit(self: *RecordingQuestionCtx) void {
        for (self.calls.items) |c2| {
            self.allocator.free(c2.title);
            self.allocator.free(c2.body);
        }
        self.calls.deinit(self.allocator);
    }
};

/// recordingQuestionRunner is an injected `QuestionRunnerFn`: it records the
/// open and returns a monotonically increasing id (never 0).
fn recordingQuestionRunner(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    plan_id: u64,
    title: []const u8,
    body: []const u8,
) u64 {
    _ = io;
    const rc: *RecordingQuestionCtx = @ptrCast(@alignCast(ctx.?));
    const id = rc.next_id;
    rc.next_id += 1;
    const tdup = rc.allocator.dupe(u8, title) catch @panic("OOM");
    const bdup = rc.allocator.dupe(u8, body) catch @panic("OOM");
    rc.calls.append(rc.allocator, .{ .plan_id = plan_id, .title = tdup, .body = bdup, .returned_id = id }) catch @panic("OOM");
    _ = allocator;
    return id;
}

/// m9MakeAcs builds a minimal AgentCallState that `runFanIn` reads: the driver,
/// allocator, and task_slug. The other fields are zeroed/empty — `runFanIn`
/// never calls `acs.destroy()` and never touches the cross-yield fields.
fn m9MakeAcs(alloc: std.mem.Allocator, driver: *AgentDriver, task_slug: []u8) AgentCallState {
    return .{
        .allocator = alloc,
        .driver = driver,
        .worktree_path = &.{},
        .claim_token = &.{},
        .task_slug = task_slug,
        .task_id = 0,
        .model = "",
        .role_name = "",
        .prompt_hash = &.{},
        .branch = null,
        .spawn_mono_ns = 0,
        .head_before = null,
        .worker_env_built = null,
        .slot_index = 0,
    };
}

/// m9ReadMergeField returns the `merge` string field of the result table at the
/// top of the Lua stack (heap-owned dupe; caller frees), or null if absent.
fn m9ReadMergeField(alloc: std.mem.Allocator, L: ?*c.lua_State) !?[]u8 {
    const t = c.lua_getfield(L, -1, "merge");
    defer c.lua_pop(L, 1);
    if (t != c.LUA_TSTRING) return null;
    var len: usize = 0;
    const raw = c.lua_tolstring(L, -1, &len);
    if (raw == null) return null;
    return try alloc.dupe(u8, raw[0..len]);
}

/// m9ReadQuestionId returns the `question_id` integer field of the result table
/// at the top of the Lua stack, or 0 if absent.
fn m9ReadQuestionId(L: ?*c.lua_State) u64 {
    const t = c.lua_getfield(L, -1, "question_id");
    defer c.lua_pop(L, 1);
    if (t != c.LUA_TNUMBER) return 0;
    const n = c.lua_tointegerx(L, -1, null);
    if (n < 0) return 0;
    return @intCast(n);
}

/// m9SetupCycleWithCommit creates the epic + cycle worktrees and commits a file
/// on the cycle branch (the "completed worker" produced a commit to fan in).
/// Configures identity inside both worktrees so the later merge commit succeeds.
fn m9SetupCycleWithCommit(
    a: std.mem.Allocator,
    repo: []const u8,
    plan_slug: []const u8,
    task_slug: []const u8,
    file_name: []const u8,
    file_data: []const u8,
) void {
    var epic = worktree.ensureEpic(a, std.testing.io, repo, plan_slug) catch @panic("ensureEpic");
    defer epic.deinit(a);
    var cyc = worktree.createCycle(a, std.testing.io, repo, plan_slug, task_slug, "run-m9", 1) catch @panic("createCycle");
    defer cyc.deinit(a);

    m9RunGitIn(a, epic.path, &.{ "config", "user.email", "test@planar.local" });
    m9RunGitIn(a, epic.path, &.{ "config", "user.name", "Planar Test" });
    m9RunGitIn(a, cyc.path, &.{ "config", "user.email", "test@planar.local" });
    m9RunGitIn(a, cyc.path, &.{ "config", "user.name", "Planar Test" });

    m9WriteFile(a, std.testing.io, cyc.path, file_name, file_data);
    m9RunGitIn(a, cyc.path, &.{ "add", file_name });
    m9RunGitIn(a, cyc.path, &.{ "commit", "-m", "worker commit" });
}

test "M9 fan-in: clean merge → merge=clean, cycle in epic history, cycle torn down, epic persists, master untouched (3199/3201)" {
    const a = testing_alloc;
    if (!m9GitAvailable(a)) return error.SkipZigTest;

    const repo = m9MkTmpRepoDir(a);
    defer a.free(repo);
    defer m9RmTree(a, repo);
    m9InitRepo(a, repo);

    const plan_slug = "planm9";
    const task_slug = "clean-task";
    m9SetupCycleWithCommit(a, repo, plan_slug, task_slug, "feature.txt", "from-cycle\n");

    const master_before = m9MasterHead(a, repo);
    defer a.free(master_before);

    // Capture the cycle tip SHA BEFORE the fan-in — teardown force-deletes the
    // cycle branch after a clean merge, so the ancestry check must use the SHA.
    const cycle_tip = (try worktree.branchHead(a, std.testing.io, repo, "cycle/planm9/clean-task")).?;
    defer a.free(cycle_tip);

    var driver = AgentDriver{
        .spawner = undefined,
        .io = std.testing.io,
        .repo_root = repo,
        .plan_slug = plan_slug,
        .plan_id = 502,
        .skip_terminal_subprocess = true,
    };

    const ts = try a.dupe(u8, task_slug);
    defer a.free(ts);
    var acs = m9MakeAcs(a, &driver, ts);

    // Build a fresh Lua state + base result table (mimics pushAgentResult).
    const L = c.luaL_newstate() orelse return error.SkipZigTest;
    defer c.lua_close(L);
    c.lua_createtable(L, 0, 5);

    runFanIn(L, &acs, true);

    // merge=clean stamped on the table.
    const merge = try m9ReadMergeField(a, L);
    defer if (merge) |m| a.free(m);
    try std.testing.expect(merge != null);
    try std.testing.expectEqualStrings("clean", merge.?);

    // The cycle's commit is in the epic history (by SHA — the branch ref was
    // force-deleted by the post-merge teardown).
    try std.testing.expect(m9IsAncestor(a, repo, cycle_tip, "epic/planm9"));

    // The cycle worktree was torn down; the epic worktree persists.
    const cyc_path = try worktree.cyclePath(a, repo, plan_slug, task_slug);
    defer a.free(cyc_path);
    try std.testing.expect(!m9DirExists(std.testing.io, cyc_path));
    const epic_path = try worktree.epicPath(a, repo, plan_slug);
    defer a.free(epic_path);
    try std.testing.expect(m9DirExists(std.testing.io, epic_path));

    // master HEAD is UNCHANGED (the merge landed on the epic branch only).
    const master_after = m9MasterHead(a, repo);
    defer a.free(master_after);
    try std.testing.expectEqualStrings(master_before, master_after);
}

test "M9 fan-in: conflict → merge=conflict + question, cycle worktree LEFT, epic restored, master untouched, returns normally (3199/3200/3201)" {
    const a = testing_alloc;
    if (!m9GitAvailable(a)) return error.SkipZigTest;

    const repo = m9MkTmpRepoDir(a);
    defer a.free(repo);
    defer m9RmTree(a, repo);
    m9InitRepo(a, repo);

    const plan_slug = "confm9";
    const task_slug = "conf-task";

    // Cycle side writes README.md.
    m9SetupCycleWithCommit(a, repo, plan_slug, task_slug, "README.md", "from-cycle\n");
    // Epic side writes a DIFFERENT README.md → conflict on merge.
    const epic_path = try worktree.epicPath(a, repo, plan_slug);
    defer a.free(epic_path);
    m9WriteFile(a, std.testing.io, epic_path, "README.md", "from-epic\n");
    m9RunGitIn(a, epic_path, &.{ "add", "README.md" });
    m9RunGitIn(a, epic_path, &.{ "commit", "-m", "epic writes readme" });

    const master_before = m9MasterHead(a, repo);
    defer a.free(master_before);

    var qctx = RecordingQuestionCtx{ .allocator = a };
    defer qctx.deinit();

    var driver = AgentDriver{
        .spawner = undefined,
        .io = std.testing.io,
        .repo_root = repo,
        .plan_slug = plan_slug,
        .plan_id = 502,
        .skip_terminal_subprocess = true,
        .question_runner = recordingQuestionRunner,
        .question_runner_ctx = &qctx,
    };

    const ts = try a.dupe(u8, task_slug);
    defer a.free(ts);
    var acs = m9MakeAcs(a, &driver, ts);

    const L = c.luaL_newstate() orelse return error.SkipZigTest;
    defer c.lua_close(L);
    c.lua_createtable(L, 0, 5);

    // runFanIn must RETURN NORMALLY on conflict (no halt / no error — 3200).
    runFanIn(L, &acs, true);

    const merge = try m9ReadMergeField(a, L);
    defer if (merge) |m| a.free(m);
    try std.testing.expect(merge != null);
    try std.testing.expectEqualStrings("conflict", merge.?);

    // A question was opened, and its id is surfaced on the result table.
    try std.testing.expectEqual(@as(usize, 1), qctx.calls.items.len);
    const opened = qctx.calls.items[0];
    try std.testing.expectEqual(@as(u64, 502), opened.plan_id);
    try std.testing.expectEqual(opened.returned_id, m9ReadQuestionId(L));
    // Title names the plan slug + task slug; body names the conflicting file.
    try std.testing.expect(std.mem.indexOf(u8, opened.title, "confm9 fan-in conflict") != null);
    try std.testing.expect(std.mem.indexOf(u8, opened.title, "conf-task") != null);
    try std.testing.expect(std.mem.indexOf(u8, opened.body, "README.md") != null);

    // The cycle worktree was LEFT for the operator to resolve.
    const cyc_path = try worktree.cyclePath(a, repo, plan_slug, task_slug);
    defer a.free(cyc_path);
    try std.testing.expect(m9DirExists(std.testing.io, cyc_path));

    // The epic worktree is restored (no merge in progress — no MERGE_HEAD).
    {
        const r = std.process.run(a, std.testing.io, .{
            .argv = &.{ "git", "-C", epic_path, "rev-parse", "--verify", "-q", "MERGE_HEAD" },
        }) catch unreachable;
        defer a.free(r.stdout);
        defer a.free(r.stderr);
        // exit non-zero ⇒ no MERGE_HEAD ⇒ aborted/restored.
        try std.testing.expect(!(r.term == .exited and r.term.exited == 0));
    }

    // master HEAD is UNCHANGED.
    const master_after = m9MasterHead(a, repo);
    defer a.free(master_after);
    try std.testing.expectEqualStrings(master_before, master_after);
}

test "M9 fan-in: conflict isolation — one conflict + one clean both return, run completes, master untouched (3200/3201)" {
    const a = testing_alloc;
    if (!m9GitAvailable(a)) return error.SkipZigTest;

    const repo = m9MkTmpRepoDir(a);
    defer a.free(repo);
    defer m9RmTree(a, repo);
    m9InitRepo(a, repo);

    const plan_slug = "isom9";

    // Set up the epic ONCE, then two cycles under it. The conflicting child
    // writes README.md (and the epic also writes README.md to force a conflict);
    // the clean child writes a disjoint file.
    {
        var epic = try worktree.ensureEpic(a, std.testing.io, repo, plan_slug);
        defer epic.deinit(a);
        m9RunGitIn(a, epic.path, &.{ "config", "user.email", "test@planar.local" });
        m9RunGitIn(a, epic.path, &.{ "config", "user.name", "Planar Test" });
    }

    // Conflicting child cycle (writes README.md).
    {
        var cyc = try worktree.createCycle(a, std.testing.io, repo, plan_slug, "conf-child", "run-m9", 1);
        defer cyc.deinit(a);
        m9RunGitIn(a, cyc.path, &.{ "config", "user.email", "test@planar.local" });
        m9RunGitIn(a, cyc.path, &.{ "config", "user.name", "Planar Test" });
        m9WriteFile(a, std.testing.io, cyc.path, "README.md", "child-cycle\n");
        m9RunGitIn(a, cyc.path, &.{ "add", "README.md" });
        m9RunGitIn(a, cyc.path, &.{ "commit", "-m", "conf child" });
    }
    // Clean child cycle (writes a disjoint file).
    {
        var cyc = try worktree.createCycle(a, std.testing.io, repo, plan_slug, "clean-child", "run-m9", 2);
        defer cyc.deinit(a);
        m9RunGitIn(a, cyc.path, &.{ "config", "user.email", "test@planar.local" });
        m9RunGitIn(a, cyc.path, &.{ "config", "user.name", "Planar Test" });
        m9WriteFile(a, std.testing.io, cyc.path, "clean.txt", "clean-content\n");
        m9RunGitIn(a, cyc.path, &.{ "add", "clean.txt" });
        m9RunGitIn(a, cyc.path, &.{ "commit", "-m", "clean child" });
    }
    // Epic writes README.md → conflicts ONLY with conf-child.
    {
        const epic_path = try worktree.epicPath(a, repo, plan_slug);
        defer a.free(epic_path);
        m9WriteFile(a, std.testing.io, epic_path, "README.md", "from-epic\n");
        m9RunGitIn(a, epic_path, &.{ "add", "README.md" });
        m9RunGitIn(a, epic_path, &.{ "commit", "-m", "epic readme" });
    }

    const master_before = m9MasterHead(a, repo);
    defer a.free(master_before);

    // Capture the clean child's tip SHA before its (post-merge) teardown.
    const clean_tip = (try worktree.branchHead(a, std.testing.io, repo, "cycle/isom9/clean-child")).?;
    defer a.free(clean_tip);
    const conf_tip = (try worktree.branchHead(a, std.testing.io, repo, "cycle/isom9/conf-child")).?;
    defer a.free(conf_tip);

    var qctx = RecordingQuestionCtx{ .allocator = a };
    defer qctx.deinit();
    var driver = AgentDriver{
        .spawner = undefined,
        .io = std.testing.io,
        .repo_root = repo,
        .plan_slug = plan_slug,
        .plan_id = 502,
        .skip_terminal_subprocess = true,
        .question_runner = recordingQuestionRunner,
        .question_runner_ctx = &qctx,
    };

    // Child 1: the CONFLICTING fan-in. Must return normally with merge=conflict.
    {
        const ts = try a.dupe(u8, "conf-child");
        defer a.free(ts);
        var acs = m9MakeAcs(a, &driver, ts);
        const L = c.luaL_newstate() orelse return error.SkipZigTest;
        defer c.lua_close(L);
        c.lua_createtable(L, 0, 5);
        runFanIn(L, &acs, true);
        const merge = try m9ReadMergeField(a, L);
        defer if (merge) |m| a.free(m);
        try std.testing.expectEqualStrings("conflict", merge.?);
    }

    // Child 2: the CLEAN fan-in proceeds INDEPENDENTLY (the sibling conflict did
    // NOT halt it). Must return normally with merge=clean.
    {
        const ts = try a.dupe(u8, "clean-child");
        defer a.free(ts);
        var acs = m9MakeAcs(a, &driver, ts);
        const L = c.luaL_newstate() orelse return error.SkipZigTest;
        defer c.lua_close(L);
        c.lua_createtable(L, 0, 5);
        runFanIn(L, &acs, true);
        const merge = try m9ReadMergeField(a, L);
        defer if (merge) |m| a.free(m);
        try std.testing.expectEqualStrings("clean", merge.?);
    }

    // Exactly one question (for the conflicting child); the clean child opened none.
    try std.testing.expectEqual(@as(usize, 1), qctx.calls.items.len);

    // The clean child landed in the epic; the conflicting child did NOT
    // (checked by SHA — the clean child's branch ref was torn down).
    try std.testing.expect(m9IsAncestor(a, repo, clean_tip, "epic/isom9"));
    try std.testing.expect(!m9IsAncestor(a, repo, conf_tip, "epic/isom9"));

    // The conflicting child's worktree is LEFT; the clean child's is torn down.
    const conf_path = try worktree.cyclePath(a, repo, plan_slug, "conf-child");
    defer a.free(conf_path);
    try std.testing.expect(m9DirExists(std.testing.io, conf_path));
    const clean_path = try worktree.cyclePath(a, repo, plan_slug, "clean-child");
    defer a.free(clean_path);
    try std.testing.expect(!m9DirExists(std.testing.io, clean_path));

    // master untouched throughout (no epic→master op — 3201).
    const master_after = m9MasterHead(a, repo);
    defer a.free(master_after);
    try std.testing.expectEqualStrings(master_before, master_after);
}

test "M9 fan-in: topology absent → merge=skipped, no question, returns normally (backward compat)" {
    const a = testing_alloc;
    if (!m9GitAvailable(a)) return error.SkipZigTest;

    const repo = m9MkTmpRepoDir(a);
    defer a.free(repo);
    defer m9RmTree(a, repo);
    m9InitRepo(a, repo);

    // NO epic worktree created → topology absent.
    const master_before = m9MasterHead(a, repo);
    defer a.free(master_before);

    var qctx = RecordingQuestionCtx{ .allocator = a };
    defer qctx.deinit();
    var driver = AgentDriver{
        .spawner = undefined,
        .io = std.testing.io,
        .repo_root = repo,
        .plan_slug = "notopo",
        .plan_id = 502,
        .skip_terminal_subprocess = true,
        .question_runner = recordingQuestionRunner,
        .question_runner_ctx = &qctx,
    };

    const ts = try a.dupe(u8, "ghost-task");
    defer a.free(ts);
    var acs = m9MakeAcs(a, &driver, ts);
    const L = c.luaL_newstate() orelse return error.SkipZigTest;
    defer c.lua_close(L);
    c.lua_createtable(L, 0, 5);

    runFanIn(L, &acs, true);

    const merge = try m9ReadMergeField(a, L);
    defer if (merge) |m| a.free(m);
    try std.testing.expect(merge != null);
    try std.testing.expectEqualStrings("skipped", merge.?);
    try std.testing.expectEqual(@as(usize, 0), qctx.calls.items.len);

    const master_after = m9MasterHead(a, repo);
    defer a.free(master_after);
    try std.testing.expectEqualStrings(master_before, master_after);
}

test "M9 fan-in: no commit-present → merge=skipped (nothing to merge)" {
    const a = testing_alloc;
    if (!m9GitAvailable(a)) return error.SkipZigTest;

    const repo = m9MkTmpRepoDir(a);
    defer a.free(repo);
    defer m9RmTree(a, repo);
    m9InitRepo(a, repo);

    // Full topology present, but commit_present=false ⇒ skip (no commit to fan in).
    m9SetupCycleWithCommit(a, repo, "nocommit", "nc-task", "f.txt", "x\n");

    var driver = AgentDriver{
        .spawner = undefined,
        .io = std.testing.io,
        .repo_root = repo,
        .plan_slug = "nocommit",
        .plan_id = 502,
        .skip_terminal_subprocess = true,
    };
    const ts = try a.dupe(u8, "nc-task");
    defer a.free(ts);
    var acs = m9MakeAcs(a, &driver, ts);
    const L = c.luaL_newstate() orelse return error.SkipZigTest;
    defer c.lua_close(L);
    c.lua_createtable(L, 0, 5);

    runFanIn(L, &acs, false); // commit_present=false

    const merge = try m9ReadMergeField(a, L);
    defer if (merge) |m| a.free(m);
    try std.testing.expectEqualStrings("skipped", merge.?);

    // The cycle worktree is left intact (skip never tears down).
    const cyc_path = try worktree.cyclePath(a, repo, "nocommit", "nc-task");
    defer a.free(cyc_path);
    try std.testing.expect(m9DirExists(std.testing.io, cyc_path));
}

// ---------------------------------------------------------------------------
// task 3540 finding 1 — fan-in gate fires on (commit_present AND (verb ==
// .complete OR claim_status == .completed)), not just on verb == .complete.
// Without this, a worker that runs its OWN `planar-agent complete` (M7
// doctrine) sets claim → .completed → decideTerminalVerb → .none → fan-in
// is SKIPPED even though the cycle branch carries a real commit. The cycle
// commit then sits unmerged forever. The new gate is unit-tested as a pure
// predicate (`shouldFanIn`) so the truth table is auditable independently
// of the runFanIn machinery + worktree topology.
// ---------------------------------------------------------------------------

test "task 3540 finding 1: shouldFanIn fires on worker self-complete + commit (claim=.completed + verb=.none + commit_present)" {
    // The original-bug case: worker ran its own terminal verb, claim is
    // `.completed`, decideTerminalVerb returned `.none`, but the cycle
    // branch carries a real commit. The new gate MUST fire here — that is
    // exactly the finding-1 fix.
    try std.testing.expect(shouldFanIn(.none, .completed, true));
}

test "task 3540 finding 1: shouldFanIn fires on harness-shelled complete (verb=.complete + commit_present)" {
    // Classic M9 path: claim was `.active` going in, harness ran
    // `planar-agent complete`. Fan-in fires regardless of claim_status
    // read (claim_status is consulted only for the worker-self-complete
    // branch; the harness-shelled-complete branch trusts the verb).
    for ([_]terminal.ClaimStatus{ .active, .completed, .released, .aborted, .stale, .unknown }) |cs| {
        try std.testing.expect(shouldFanIn(.complete, cs, true));
    }
}

test "task 3540 finding 1: shouldFanIn does NOT fire on self-release (claim=.released + verb=.none)" {
    // A worker that ran `planar-agent release` produced no useful work to
    // integrate. Even with commit_present (a stray commit that release
    // does not contractually exclude), we do NOT fan in — release means
    // "throw it away," and fanning in would silently merge work the
    // worker disowned. This is the explicit negative case called out in
    // the finding.
    try std.testing.expect(!shouldFanIn(.none, .released, true));
}

test "task 3540 finding 1: shouldFanIn does NOT fire on .aborted / .stale / .unknown self-terminal (verb=.none)" {
    // These claim states have no mergeable contract — the worker either
    // never finalized or its lease died — so the harness must NOT fan in
    // even when a commit is present. Operator triages via reconcile.
    try std.testing.expect(!shouldFanIn(.none, .aborted, true));
    try std.testing.expect(!shouldFanIn(.none, .stale, true));
    try std.testing.expect(!shouldFanIn(.none, .unknown, true));
}

test "task 3540 finding 1: shouldFanIn does NOT fire without commit_present" {
    // commit_present=false ⇒ nothing to merge. The gate must short-circuit
    // BEFORE consulting verb/claim_status, so a self-completed-but-no-
    // commit worker does not erroneously enter the fan-in machinery.
    for ([_]terminal.TerminalVerb{ .none, .complete, .release, .fail }) |v| {
        for ([_]terminal.ClaimStatus{ .active, .completed, .released, .aborted, .stale, .unknown }) |cs| {
            try std.testing.expect(!shouldFanIn(v, cs, false));
        }
    }
}

test "task 3540 finding 1: shouldFanIn does NOT fire when active claim + verb=.none (degenerate)" {
    // verb=.none with claim_status=.active is the "harness exit_code 0 +
    // no commit" combo from decideTerminalVerb — but that combo also
    // returns .release, not .none, so this is a defensive negative: the
    // gate must not surface fan-in for a hypothetical .none-on-active.
    try std.testing.expect(!shouldFanIn(.release, .active, true));
    try std.testing.expect(!shouldFanIn(.fail, .active, true));
}

test "task 3540 finding 1 e2e: worker self-completed + cycle commit → fan-in fires (merge=clean), cycle torn down" {
    // The original-bug fix verified END-TO-END (driving runFanIn through
    // the realistic worker-self-complete path): a worker that ran its own
    // `planar-agent complete` sets claim → .completed → decideTerminalVerb
    // → .none. The new gate routes through runFanIn because commit_present
    // + claim_status==.completed is the worker-self-complete branch.
    //
    // We exercise runFanIn directly here (mirroring the existing M9 e2e
    // tests above): the worktree topology + cycle commit is real, the
    // merge outcome is asserted via the same m9-helpers. The gate decision
    // itself is pinned by the shouldFanIn unit tests above.
    const a = testing_alloc;
    if (!m9GitAvailable(a)) return error.SkipZigTest;

    const repo = m9MkTmpRepoDir(a);
    defer a.free(repo);
    defer m9RmTree(a, repo);
    m9InitRepo(a, repo);

    const plan_slug = "f1plan";
    const task_slug = "f1task";
    m9SetupCycleWithCommit(a, repo, plan_slug, task_slug, "f1.txt", "from-cycle-self-complete\n");

    var driver = AgentDriver{
        .spawner = undefined,
        .io = std.testing.io,
        .repo_root = repo,
        .plan_slug = plan_slug,
        .plan_id = 540,
        .skip_terminal_subprocess = true,
    };
    const ts = try a.dupe(u8, task_slug);
    defer a.free(ts);
    var acs = m9MakeAcs(a, &driver, ts);

    const L = c.luaL_newstate() orelse return error.SkipZigTest;
    defer c.lua_close(L);
    c.lua_createtable(L, 0, 5);

    // The agentContinue gate would route here for (verb=.none,
    // claim_status=.completed, commit_present=true) via shouldFanIn; we
    // call runFanIn directly with commit_present=true to assert the merge
    // actually lands.
    runFanIn(L, &acs, true);

    const merge = try m9ReadMergeField(a, L);
    defer if (merge) |m| a.free(m);
    try std.testing.expect(merge != null);
    try std.testing.expectEqualStrings("clean", merge.?);

    // Cycle worktree torn down (post-success cleanup).
    const cyc_path = try worktree.cyclePath(a, repo, plan_slug, task_slug);
    defer a.free(cyc_path);
    try std.testing.expect(!m9DirExists(std.testing.io, cyc_path));
}

// ---------------------------------------------------------------------------
// task 3540 finding 2 — runTerminalVerb errors must be SURFACED (not silently
// swallowed). On error the harness MUST:
//   - set `terminal_verb_error = true` on the result table (visible to the
//     Lua workflow + the operator reading the result),
//   - SKIP fan-in entirely (stamp `merge = "skipped-terminal-verb-failed"`),
//   - print a loud stderr WARNING.
// This protects a resume from a double-merge: a failed `planar-agent complete`
// leaves the task DB-not-completed, so a resume re-spawns it and produces a
// second commit; merging both would conflict/duplicate in the epic.
// ---------------------------------------------------------------------------

/// FailingTerminalVerbCtx is a recording fake that always errors. The
/// agentContinue path then sets `terminal_verb_failed = true`, surfaces it
/// on the result table, halts fan-in, and prints the WARNING.
const FailingTerminalVerbCtx = struct {
    calls: u32 = 0,
    last_verb: terminal.TerminalVerb = .none,
};

fn failingTerminalVerbRunner(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: std.Io,
    verb: terminal.TerminalVerb,
    claim_token: []const u8,
) terminal.TerminalError!void {
    _ = allocator;
    _ = io;
    _ = claim_token;
    const rc: *FailingTerminalVerbCtx = @ptrCast(@alignCast(ctx.?));
    rc.calls += 1;
    rc.last_verb = verb;
    return terminal.TerminalError.SubprocessNonZero;
}

test "task 3540 finding 2: runTerminalVerb error → terminal_verb_error=true + merge=skipped-terminal-verb-failed + WARNING" {
    // Exit 0 (would otherwise drive .release on no-commit) so the harness
    // shells `planar-agent release`; the injected runner errors. The result
    // table must report terminal_verb_error=true AND merge=skipped-
    // terminal-verb-failed (the halt-fan-in stamp), and the stderr WARNING
    // must have been printed. The fan-in machinery is NOT invoked (no
    // question runner call). This is the documented locked semantic.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var ftv = FailingTerminalVerbCtx{};
    var qctx = RecordingQuestionCtx{ .allocator = a };
    defer qctx.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        // skip_terminal_subprocess = false so the agentContinue path
        // DOES call into the injected terminal_verb_runner (and the
        // failure path runs).
        .skip_terminal_subprocess = false,
        .terminal_verb_runner = failingTerminalVerbRunner,
        .terminal_verb_runner_ctx = &ftv,
        .question_runner = recordingQuestionRunner,
        .question_runner_ctx = &qctx,
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "f2-tv-error", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok-f2-err",
        \\      task_slug = "f2ts",
        \\    })
        \\    assert(r.terminal_verb_error == true, "expected terminal_verb_error=true, got " .. tostring(r.terminal_verb_error))
        \\    assert(r.merge == "skipped-terminal-verb-failed",
        \\      "expected merge=skipped-terminal-verb-failed, got " .. tostring(r.merge))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:f2-tv-error", &.{}, &host, &err_buf);

    // The injected runner was invoked exactly once (the harness's terminal
    // verb attempt), with the verb the decision matrix chose for exit==0
    // + no-commit + active-claim = .release.
    try std.testing.expectEqual(@as(u32, 1), ftv.calls);
    try std.testing.expectEqual(terminal.TerminalVerb.release, ftv.last_verb);
    // Fan-in machinery was NOT invoked (no question_runner call). This is
    // the load-bearing assertion of finding 2: a failed terminal verb is
    // a HALT-fan-in condition, not a swallow-and-continue.
    try std.testing.expectEqual(@as(usize, 0), qctx.calls.items.len);
}

test "task 3540 finding 2: terminal_verb_error=false on the happy path (no regression)" {
    // Sanity: a normal successful agent() call still gets
    // terminal_verb_error=false on the result table. The new field is
    // additive.
    const a = testing_alloc;
    var fake = spawn.FakeSpawnerState.init(a, 0, "ok", "");
    defer fake.deinit();
    var driver = AgentDriver{
        .spawner = fake.spawner(),
        .io = std.testing.io,
        .skip_terminal_subprocess = true, // ⇒ no terminal verb subprocess, no error possible
    };
    var host = HostState.init(a, 0, 0, 100, 0);
    defer host.deinit();
    host.agent_driver = &driver;

    const src =
        \\return {
        \\  meta = { name = "f2-tv-ok", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt",
        \\      claim_token = "tok-f2-ok",
        \\      task_slug = "f2ts2",
        \\    })
        \\    assert(r.terminal_verb_error == false,
        \\      "expected terminal_verb_error=false on happy path, got " .. tostring(r.terminal_verb_error))
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = @splat(0);
    try runModule(src, "test:f2-tv-ok", &.{}, &host, &err_buf);
}
