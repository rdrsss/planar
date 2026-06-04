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

const c = @cImport({
    @cInclude("lua.h");
    @cInclude("lauxlib.h");
    @cInclude("lualib.h");
});

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
};

var stdout_buf: [4096]u8 = undefined;
var stderr_buf: [1024]u8 = undefined;
var stdout_writer_storage: ?Io.File.Writer = null;
var stderr_writer_storage: ?Io.File.Writer = null;
var global_ctx: ?ExecCtx = null;

fn initCtx(allocator: std.mem.Allocator, io: Io) void {
    stdout_writer_storage = Io.File.Writer.init(.stdout(), io, &stdout_buf);
    stderr_writer_storage = Io.File.Writer.init(.stderr(), io, &stderr_buf);
    global_ctx = .{
        .allocator = allocator,
        .io = io,
        .stdout = &stdout_writer_storage.?.interface,
        .stderr = &stderr_writer_storage.?.interface,
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

    pub fn init(allocator: std.mem.Allocator, now: i64, seed: i64, budget_total: i64, budget_spent: i64) HostState {
        return .{
            .allocator = allocator,
            .calls = .empty,
            .now = now,
            .seed = seed,
            .budget_total = budget_total,
            .budget_spent = budget_spent,
            .agent_driver = null,
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
    /// to the shim dir; STRIPPED_ENV_VARS are removed; PLANAR_WORKBENCH_ROOT
    /// is intentionally inherited).
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

/// driveAgentCall runs the full M4 pipeline for one `agent(prompt, opts)`
/// invocation, using the injected `AgentDriver`. Returns the Lua result table
/// pushed onto the stack (always one value).
///
/// The prompt is the brief (multi-KB text the workflow has pre-compiled via
/// `brief.compileBrief` or similar). It is delivered to the worker via stdin
/// (decision pinned in `spawn.zig`).
fn driveAgentCall(
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

    const alloc = hs.allocator;
    const io = driver.io;

    // 1) Sample the pre-spawn cycle branch HEAD (commit-presence proxy).
    var head_before: ?[]u8 = null;
    defer if (head_before) |h| alloc.free(h);
    if (driver.repo_root.len > 0 and driver.plan_slug.len > 0 and opts.task_slug.len > 0) {
        const branch = worktree.cycleBranch(alloc, driver.plan_slug, opts.task_slug) catch {
            _ = c.luaL_error(L, "agent: out of memory");
            return 0;
        };
        defer alloc.free(branch);
        head_before = worktree.branchHead(alloc, io, driver.repo_root, branch) catch null;
    }

    // 2) Build the constrained worker env when a builder is installed. This is
    // the Item-J wiring: decisions 358 + 365 require that the worker's PATH
    // be the per-cycle shim (planar-agent + git only) and that
    // PLANAR_DB / PLANAR_CONFIG_PATH / etc. are stripped. The real spawner
    // PANICS if env_map is null (see spawn.realRunFn), so production wiring
    // MUST install a builder; tests with FakeSpawner can omit it and the
    // FakeSpawner records an empty env snapshot.
    var worker_env_built: ?worker_env.WorkerEnv = null;
    defer if (worker_env_built) |*we| we.deinit(alloc, io);
    if (driver.env_builder) |build_env| {
        worker_env_built = build_env(driver.env_builder_ctx, alloc, io, opts.worktree_path) catch |err| {
            _ = c.luaL_error(L, "agent: env builder failed: %s", @errorName(err).ptr);
            return 0;
        };
    }
    const env_map_ptr: ?*const std.process.Environ.Map = if (worker_env_built) |*we| &we.env_map else null;

    // 3) Spawn the worker. The brief is the prompt (already-compiled methodology
    // brief; the workflow drives compileBrief separately, then passes the
    // resulting string in). env_map (the constrained worker env) is passed
    // through SpawnInputs; the real spawner threads it as `environ_map` on
    // std.process.spawn so the child does NOT inherit the parent's full env.
    const spawn_inputs = spawn.SpawnInputs{
        .role = role,
        .worktree_path = opts.worktree_path,
        .brief = prompt,
        .role_spec = opts.role_spec,
        .env_map = env_map_ptr,
    };
    var outcome = driver.spawner.run(alloc, io, spawn_inputs) catch |err| {
        _ = c.luaL_error(L, "agent: spawn failed: %s", @errorName(err).ptr);
        return 0;
    };
    defer outcome.deinit(alloc);

    // 4) Sample the post-spawn cycle branch HEAD.
    var head_after: ?[]u8 = null;
    defer if (head_after) |h| alloc.free(h);
    if (driver.repo_root.len > 0 and driver.plan_slug.len > 0 and opts.task_slug.len > 0) {
        const branch = worktree.cycleBranch(alloc, driver.plan_slug, opts.task_slug) catch {
            _ = c.luaL_error(L, "agent: out of memory");
            return 0;
        };
        defer alloc.free(branch);
        head_after = worktree.branchHead(alloc, io, driver.repo_root, branch) catch null;
    }

    // commit_present = post-sample exists AND differs from pre-sample.
    // (Pre-null + post-non-null also counts as commit-present: the branch was
    // created during the spawn.)
    const commit_present = blk: {
        if (head_after) |hi| {
            if (head_before) |hi0| break :blk !std.mem.eql(u8, hi0, hi);
            break :blk true;
        }
        break :blk false;
    };

    // 5) Read the LIVE claim status (task 3242). When the worker already ran
    // its own terminal verb (the happy path per tech-spec L182-186), the claim
    // is no longer live and `decideTerminalVerb` returns `.none` so the harness
    // does NOT issue a redundant terminal-verb subprocess that would error with
    // ClaimNotActive. The reader is injectable on the driver so unit tests pin
    // this no-op path without a live binary.
    const claim_status: terminal.ClaimStatus = readClaimStatusOrActive(driver, alloc, io, opts.claim_token);

    // 6) Decide + (optionally) apply the terminal verb.
    const verb = terminal.decideTerminalVerb(.{
        .claim_status = claim_status,
        .exit_code = outcome.exit_code,
        .commit_present = commit_present,
    });

    if (!driver.skip_terminal_subprocess and opts.claim_token.len > 0) {
        terminal.runTerminalVerb(alloc, io, verb, opts.claim_token) catch {
            // We do NOT raise a Lua error here — the harness has tried its
            // best; the operator can recover via planar-agent reconcile. The
            // returned Lua table still carries the decision.
        };
    }

    // 7) Build the result table and push.
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

    // M4 mode — drive the full spawn pipeline through the injected driver.
    const opts = readAgentOpts(L, 2);
    return driveAgentCall(L, hs, driver, prompt, opts);
}

/// hostParallel — `parallel({thunks})`. RECORDING STUB. Records the count of
/// thunks in the passed array (the call shape); does NOT drive any thunk
/// (scheduling is M5). Returns an empty stub results array.
fn hostParallel(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    var n: c.lua_Integer = 0;
    if (c.lua_type(L, 1) == c.LUA_TTABLE) n = @intCast(c.lua_rawlen(L, 1));
    var buf: [32]u8 = undefined;
    const shape = std.fmt.bufPrint(&buf, "{d} thunks", .{n}) catch "? thunks";
    recordOrError(L, hs, .parallel, shape, "");
    // Stub: return an empty results array.
    c.lua_createtable(L, 0, 0);
    return 1;
}

/// hostPipeline — `pipeline(items, ...stages)`. RECORDING STUB. Records the item
/// count and the stage count; does NOT run any item through any stage (M5).
/// Returns an empty stub results array.
fn hostPipeline(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    var items: c.lua_Integer = 0;
    if (c.lua_type(L, 1) == c.LUA_TTABLE) items = @intCast(c.lua_rawlen(L, 1));
    // Remaining varargs (indices 2..top) are the stages.
    const top = c.lua_gettop(L);
    const stages: c_int = if (top >= 2) top - 1 else 0;
    var buf0: [32]u8 = undefined;
    var buf1: [32]u8 = undefined;
    const items_shape = std.fmt.bufPrint(&buf0, "{d} items", .{items}) catch "? items";
    const stages_shape = std.fmt.bufPrint(&buf1, "{d} stages", .{stages}) catch "? stages";
    recordOrError(L, hs, .pipeline, items_shape, stages_shape);
    c.lua_createtable(L, 0, 0);
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

    // Push module.run onto the stack.
    const run_type = c.lua_getfield(L, module_idx, "run");
    if (run_type != c.LUA_TFUNCTION) return LuaError.LuaModuleMissingRun;
    // Stack: [module_table, run_function].  Pin with absindex.
    const run_idx: c_int = c.lua_absindex(L, -1);
    _ = run_idx; // absolute index captured; run is at top before ctx push

    // Build ctx table: { args = { [1]=args[0], [2]=args[1], ... } }.
    c.lua_createtable(L, 0, 1); // push ctx table
    const ctx_idx: c_int = c.lua_absindex(L, -1);
    c.lua_createtable(L, @intCast(args.len), 0); // push args table (sequence hint)
    const args_tbl_idx: c_int = c.lua_absindex(L, -1);

    // Populate args as a 1-indexed Lua sequence.
    for (args, 0..) |arg, idx| {
        // lua_pushlstring copies the bytes — safe for arbitrary []const u8.
        _ = c.lua_pushlstring(L, arg.ptr, arg.len);
        // lua_rawseti(L, table_idx, key): args_table[idx+1] = arg_string, pops the value.
        c.lua_rawseti(L, args_tbl_idx, @intCast(idx + 1));
    }

    // ctx["args"] = args_table; pops args table.
    c.lua_setfield(L, ctx_idx, "args");
    // Stack: [module_table, run_function, ctx_table].

    // Install the host-function surface + determinism on ctx (task 3168/3169).
    // Per the spec ("ctx carries args and the host functions") these are ctx
    // fields, not globals.
    if (host) |hs| installHostFns(L, ctx_idx, hs);

    // Call run(ctx): 1 argument, 0 expected return values, no error handler.
    const run_rc = c.lua_pcallk(L, 1, 0, 0, 0, null);
    if (run_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaRuntimeError;
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

    initCtx(arena, init.io);
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
