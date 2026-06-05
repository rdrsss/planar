//! planar-execute — Lua 5.4 script execution harness (plan 492).
//!
//! Fifth binary in the Planar family. Unlike the other binaries, this one
//! does NOT open SQLite and does NOT link the runtime / engine / db modules.
//! Its sole dependency beyond the standard library is liblua54 (vendored
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
const Io = std.Io;

const cli = @import("cli");

/// State-read helpers — subprocess + JSON parse layer (task 3170 m2-state-reads).
/// Imported here so its `test` blocks run under the `execute_exe_tests` target.
pub const state = @import("state.zig");

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

/// lua — the single shared `@cImport` of the vendored Lua 5.4 C API. Both this
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
};

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
        };
    }
    return .{
        .role = luaOptString(L, opts_idx, "role", ""),
        .worktree_path = luaOptString(L, opts_idx, "worktree_path", ""),
        .claim_token = luaOptString(L, opts_idx, "claim_token", ""),
        .role_spec = luaOptString(L, opts_idx, "role_spec", ""),
        .task_slug = luaOptString(L, opts_idx, "task_slug", ""),
    };
}

/// pushAgentResult builds the Lua return table for one agent() call:
///   { status = "...", exit_code = N, commit_present = bool,
///     terminal_verb = "complete"|"release"|"fail"|"none" }
/// `status` is "completed" / "released" / "failed" / "respected" / "stub"
/// matching the harness's decision summary.
fn pushAgentResult(
    L: ?*c.lua_State,
    status: []const u8,
    exit_code: u32,
    commit_present: bool,
    verb: terminal.TerminalVerb,
) void {
    c.lua_createtable(L, 0, 4);
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
fn driveAgentCallPreYield(
    L: ?*c.lua_State,
    hs: *HostState,
    driver: *AgentDriver,
    prompt: []const u8,
    opts: AgentCallOpts,
) c_int {
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

    // 1) Sample the pre-spawn cycle branch HEAD (commit-presence proxy).
    if (driver.repo_root.len > 0 and driver.plan_slug.len > 0 and acs.task_slug.len > 0) {
        const branch = worktree.cycleBranch(alloc, driver.plan_slug, acs.task_slug) catch {
            acs.destroy();
            _ = c.luaL_error(L, "agent: out of memory");
            return 0;
        };
        defer alloc.free(branch);
        acs.head_before = worktree.branchHead(alloc, io, driver.repo_root, branch) catch null;
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

    // 4) Register the in-flight worker + this call state with the scheduler.
    const slot_index = sched.registerInflight(L.?, handle, acs) catch {
        // Registry full — N>MAX_SLOTS concurrent workers. Impossible at N=1.
        // Drain the handle so the child is not orphaned, then error.
        var h = handle;
        var drained = driver.spawner.wait(alloc, io, &h) catch {
            acs.destroy();
            _ = c.luaL_error(L, "agent: scheduler registry full");
            return 0;
        };
        drained.deinit(alloc);
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

        pushAgentResult(L, "timed-out", outcome.exit_code, commit_present, .fail);
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

    // 5) Optionally apply the terminal verb.
    if (!driver.skip_terminal_subprocess and acs.claim_token.len > 0) {
        terminal.runTerminalVerb(alloc, io, verb, acs.claim_token) catch {
            // The harness tried its best; operator recovers via reconcile. The
            // returned Lua table still carries the decision.
        };
    }

    // 6) Build the result table and push (one return value to the script).
    const status_str: []const u8 = switch (verb) {
        .none => "respected",
        .complete => "completed",
        .release => "released",
        .fail => "failed",
    };
    pushAgentResult(L, status_str, outcome.exit_code, commit_present, verb);
    return 1;
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
///   - Lua 5.4's `pcall` is YIELDABLE: when a stage calls `agent()` (which
///     `lua_yield`s across the C boundary), the yield propagates out THROUGH the
///     pcall to the scheduler, and the scheduler's resume re-enters the pcall on
///     continuation. This is why a stage may spawn a worker even though it runs
///     under pcall — the runner-with-pcall shape is correct precisely because
///     5.4 made pcall continuation-aware.
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

    // Pop meta table.
    luaPop(L, 1);

    return WorkflowMeta{
        .name = meta_name,
        .description = meta_desc,
        .phases = try phases.toOwnedSlice(allocator),
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
    // forbidden in Lua 5.4 ("attempt to yield across a C-call boundary"), but a
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
/// The Lua version string is defined as a comptime constant in lua.h:
///   #define LUA_VERSION "Lua 5.4"
/// We pair it with the binary name for consistency with the other binaries'
/// `<binary> <version-info>` format.
const planar_execute_version = "planar-execute 0.1.0 (lua " ++ c.LUA_VERSION_MAJOR ++ "." ++ c.LUA_VERSION_MINOR ++ ")";

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
    \\Execute a Lua 5.4 workflow script.
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
    \\planar-execute — Lua 5.4 workflow execution harness (plan 492).
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

    if (ctx.environ.getPosix("PLANAR_EXECUTE_LIVE_AGENT") != null) {
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
    }

    // Invoke run(ctx) with the trailing args threaded into ctx.args and the
    // host-function surface + determinism installed on ctx.
    runModule(source, chunkname, rest_args, &host, &err_buf) catch |e| {
        const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
        if (msg.len > 0) {
            try ctx.stderr.print("planar-execute: {s}\n", .{msg});
        } else {
            try ctx.stderr.print("planar-execute: run error: {s}\n", .{@errorName(e)});
        }
        try flushCtx();
        std.process.exit(1);
    };
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
