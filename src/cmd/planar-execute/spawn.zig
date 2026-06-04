//! spawn.zig — `claude -p` spawn driver for the `agent()` host function
//! (plan 492 M4 tasks 3175 + 3176 + 3178).
//!
//! This module owns the worker-invocation contract for a single blocking
//! `agent(prompt, opts)` call. It exposes:
//!
//!   - `buildSpawnArgv` — PURE function that returns the exact argv shape the
//!     worker is spawned with, given a role + worktree path + resolved tier.
//!     Unit-tested in isolation (task 3176's deliverable).
//!   - `Spawner` — injectable interface with two variants:
//!       * `realSpawner()`  — runs the real `claude --print ...` subprocess.
//!       * `fakeSpawner(...)` — records the argv it would have run and returns
//!         a caller-controlled canned outcome. Tests use this exclusively.
//!   - `SpawnOutcome` — `{ exit_code, stdout, stderr }`. The terminal-fallback
//!     decision (`terminal.zig:decideTerminalVerb`) consumes this together with
//!     the post-spawn claim/commit state to pick the terminal verb.
//!
//! ## Design decisions (sub-decisions pinned in this module)
//!
//! ### Brief delivery channel — STDIN
//!
//! The brief (the methodology-compliant text the planar-execute brief compiler
//! emits) is delivered via STDIN, not argv. `claude --print` with the default
//! `--input-format text` reads its prompt from stdin when no positional prompt
//! is supplied. Briefs are multi-KB and growing; argv has hard OS limits (256 KB
//! ARG_MAX on macOS). Stdin is the only safe channel for the brief.
//!
//! ### Role injection — `--append-system-prompt <role-spec>`
//!
//! The role specification (the contents of `agents/<role>.md`) is appended to
//! the worker's system prompt via `--append-system-prompt`. The alternative
//! (`--agents <json>`) is a JSON-encoded custom-agent definition surface — heavier
//! and noisier for what is fundamentally a text channel. `--append-system-prompt`
//! is exactly the right shape: text in, text appended to the system prompt.
//!
//! ### Permission mode — `--permission-mode bypassPermissions` (decision 365)
//!
//! Locked by decision 365. The A3 probe demonstrated that the `default` and
//! `acceptEdits` modes silently no-op the worker (exit 0, no commit). Only
//! `bypassPermissions` produces a working coder.
//!
//! ### Worktree cwd — `--add-dir <worktree>` + `cwd = <worktree>`
//!
//! The spawn's `cwd` is the cycle worktree path. `--add-dir <worktree>` is also
//! passed so that even if claude's tool sandboxing tries to constrain edit
//! reach to cwd, the worktree is explicitly allowed. (Tech-spec § "How the three
//! pains become impossible" § Caveat: the worktree is pwd-hygiene, NOT a jail —
//! containment is the constrained PATH from `worker_env.zig`.)
//!
//! ## Async surface (plan 492 M5 task 3182 — coroutine scheduler)
//!
//! The scheduler needs NON-BLOCKING spawn semantics so a yielded `agent()`
//! coroutine can be parked while its `claude -p` worker runs, and resumed when
//! the worker reaches a terminal state. The `Spawner` therefore grows three
//! methods alongside the legacy blocking `run`:
//!
//!   - `start(allocator, io, inputs) -> Handle` — launches the worker WITHOUT
//!     waiting. Returns an in-flight handle. RealSpawner uses
//!     `std.process.spawn` (NOT `std.process.run`) per tech-spec L160-164 so a
//!     supervisor can wait on its child while (in M6) a heartbeat thread
//!     refreshes the lease.
//!   - `poll(handle) -> ?SpawnOutcome` — non-blocking probe: `null` while the
//!     worker is still running, an Outcome once it is terminal.
//!   - `wait(handle) -> SpawnOutcome` — blocks until THIS handle completes.
//!
//! For N=1 (this cycle) the scheduler simply `wait`s the single handle. The
//! poll surface exists so task 3183 (`parallel`, N concurrent coroutines) can
//! wait-for-any across N handles (poll-loop with a small sleep, or block on
//! whichever completes first). The legacy `run` is retained for the
//! buildSpawnArgv unit tests and any direct blocking call site.
//!
//! ## What this module does NOT do
//!
//! - **No N-way parallel scheduling** (3183). The async surface supports it but
//!   this cycle only exercises one in-flight handle.
//! - **No heartbeat thread** (M6). The caller heartbeats the claim from the
//!   harness's main thread between Lua call returns.
//! - **No run-id tagging / O_EXCL lock** (M6).
//! - **No journal** (M8).
//!
//! ## Testability seam — the load-bearing piece
//!
//! Real `claude -p` spawns cost $0.10–$0.50 each and cannot run in CI. The
//! `Spawner` interface lets every code path that drives the spawn be exercised
//! by `FakeSpawner` (which records its argv + returns a canned outcome) instead.
//! The single live-spawn smoke test is gated by `PLANAR_EXECUTE_LIVE_AGENT=1`
//! (default off) — documented at the test header.

const std = @import("std");
const Io = std.Io;
const builtin = @import("builtin");
const posix = std.posix;

const role_model = @import("role_model.zig");
const worker_env = @import("worker_env.zig");

// ---------------------------------------------------------------------------
// Error set
// ---------------------------------------------------------------------------

/// All errors `spawn.zig` can surface. Distinct from `worker_env`'s set so the
/// caller (hostAgent) can route per-class.
pub const SpawnError = error{
    /// Allocator returned OOM building argv, env, or owned outcome strings.
    OutOfMemory,
    /// `std.process.run` on the real spawner failed (could not exec the
    /// child, broken pipe, etc.). Distinct from "ran and exited non-zero",
    /// which is an SpawnOutcome with non-zero exit_code.
    SubprocessFailed,
    /// A path argument was empty, not absolute, or contained an embedded NUL.
    /// (Mirrors `worker_env.WorkerEnvError.InvalidBinaryPath`.)
    InvalidPath,
    /// The recorded role is not in `role_model.Role` (unknown role string).
    UnknownRole,
    /// Writing the brief to the child's stdin failed (broken pipe). Distinct
    /// from SubprocessFailed because the child may have exited cleanly before
    /// reading all input.
    StdinWriteFailed,
};

// ---------------------------------------------------------------------------
// SpawnOutcome — observable result of one worker invocation.
// ---------------------------------------------------------------------------

/// The observable outcome of a single `claude -p` spawn. The `stdout` and
/// `stderr` slices are heap-owned (by the same allocator the spawner was
/// invoked with); free via `deinit`.
///
/// `exit_code` is the literal child exit code (0..255). On abnormal termination
/// (signal, crash) `exit_code` is set to 255 (a sentinel; the actual signal is
/// not preserved at this layer because the terminal-fallback decision matrix
/// (`terminal.zig`) only branches on exit==0 vs not).
pub const SpawnOutcome = struct {
    exit_code: u32,
    stdout: []u8,
    stderr: []u8,

    pub fn deinit(self: *SpawnOutcome, allocator: std.mem.Allocator) void {
        allocator.free(self.stdout);
        allocator.free(self.stderr);
        self.* = undefined;
    }
};

// ---------------------------------------------------------------------------
// SpawnInputs — everything `buildSpawnArgv` + the spawner need.
// ---------------------------------------------------------------------------

/// All inputs to a single spawn invocation. The caller (hostAgent) gathers
/// these from the Lua opts table, the harness's resolved binary paths, and the
/// resolved cycle worktree.
pub const SpawnInputs = struct {
    /// The role (from `opts.role`). Determines the model tier and which agent
    /// spec is appended as the system prompt.
    role: role_model.Role,
    /// Absolute path to the cycle worktree the worker should run inside. Passed
    /// as `--add-dir <worktree>` AND as the child's `cwd`.
    worktree_path: []const u8,
    /// The brief text the worker reads from its stdin. Multi-KB; never on argv.
    brief: []const u8,
    /// The role spec text (e.g. the contents of `agents/coder.md`) appended to
    /// the worker's system prompt via `--append-system-prompt`. May be empty
    /// (the worker still functions; role-specific guidance just isn't injected).
    role_spec: []const u8,
    /// The constrained environment map the worker is spawned with. Built by
    /// `worker_env.buildWorkerEnv` ahead of the spawn (per-cycle shim PATH
    /// exposing only `planar-agent` + `git`, planar-internal env vars stripped
    /// per decisions 358 + 365). When non-null the real spawner passes it to
    /// `std.process.spawn` as `environ_map`; when null the real spawner
    /// PANICS at the call site rather than silently inheriting the parent's
    /// env (that is the iter-1 regression Item J closes). FakeSpawner records
    /// a snapshot for unit-test assertions.
    env_map: ?*const std.process.Environ.Map,
};

// ---------------------------------------------------------------------------
// buildSpawnArgv — pure: returns the exact argv shape for one spawn.
// ---------------------------------------------------------------------------

/// Builds the argv slice the worker is spawned with. PURE — no I/O, no
/// allocation beyond the returned slice (and its element strings, which are
/// either constants or duped slices of the inputs). The caller owns the
/// returned slice and MUST free it via `freeSpawnArgv`.
///
/// The exact shape (in order):
///
/// ```
/// claude
///   --print
///   --permission-mode bypassPermissions   (decision 365)
///   --model <tier>                         (per-role, from role_model)
///   --add-dir <worktree_path>              (allow edits inside the worktree)
///   --append-system-prompt <role_spec>     (role injection; only when non-empty)
/// ```
///
/// Note that the brief is NOT on argv — it is fed to the child's stdin by the
/// spawner. The role spec IS on argv (it is bounded text; agents/coder.md is
/// ~10 KB which fits comfortably in ARG_MAX). When `role_spec` is empty, the
/// `--append-system-prompt` flag is OMITTED rather than passed with an empty
/// value, to avoid a no-op system-prompt extension.
pub fn buildSpawnArgv(
    allocator: std.mem.Allocator,
    inputs: SpawnInputs,
) SpawnError![][]const u8 {
    if (inputs.worktree_path.len == 0) return SpawnError.InvalidPath;
    if (!std.fs.path.isAbsolute(inputs.worktree_path)) return SpawnError.InvalidPath;
    if (std.mem.indexOfScalar(u8, inputs.worktree_path, 0) != null) return SpawnError.InvalidPath;

    const tier = role_model.modelForRole(inputs.role);

    // Compute the argv length. The base shape is 8 elements; the
    // `--append-system-prompt <spec>` pair (2 elements) is added when
    // role_spec is non-empty.
    const base: usize = 8;
    const extra: usize = if (inputs.role_spec.len > 0) 2 else 0;

    var argv = allocator.alloc([]const u8, base + extra) catch return SpawnError.OutOfMemory;
    errdefer allocator.free(argv);

    var i: usize = 0;
    argv[i] = "claude";
    i += 1;
    argv[i] = "--print";
    i += 1;
    argv[i] = "--permission-mode";
    i += 1;
    argv[i] = "bypassPermissions";
    i += 1;
    argv[i] = "--model";
    i += 1;
    argv[i] = tier;
    i += 1;
    argv[i] = "--add-dir";
    i += 1;
    argv[i] = inputs.worktree_path;
    i += 1;

    if (inputs.role_spec.len > 0) {
        argv[i] = "--append-system-prompt";
        i += 1;
        argv[i] = inputs.role_spec;
        i += 1;
    }

    std.debug.assert(i == argv.len);
    return argv;
}

/// freeSpawnArgv frees the argv slice returned by `buildSpawnArgv`. The element
/// strings are borrowed (either string constants or borrows into SpawnInputs),
/// so only the outer slice is freed.
pub fn freeSpawnArgv(allocator: std.mem.Allocator, argv: [][]const u8) void {
    allocator.free(argv);
}

// ---------------------------------------------------------------------------
// Spawner interface — function-pointer indirection so tests can swap in a fake.
// ---------------------------------------------------------------------------

/// A Spawner runs one worker invocation and returns its outcome. Implemented as
/// a struct carrying a function pointer + a context pointer, so both the real
/// subprocess driver AND the test fake satisfy the same shape.
///
/// The `run` function takes ownership of computing argv, materializing env,
/// streaming `brief` into stdin, and waiting for the child. The returned
/// `SpawnOutcome` is heap-owned by `allocator`; caller calls `outcome.deinit`.
pub const Spawner = struct {
    /// Opaque context pointer. Real spawner uses null; fake spawner stashes its
    /// recorded-argv buffer pointer here.
    ctx: ?*anyopaque,

    /// Run one spawn. `inputs.brief` is fed to stdin; argv is derived from the
    /// other inputs. BLOCKING — retained for direct call sites and the
    /// buildSpawnArgv unit tests. The scheduler path uses start/poll/wait.
    runFn: *const fn (
        ctx: ?*anyopaque,
        allocator: std.mem.Allocator,
        io: Io,
        inputs: SpawnInputs,
    ) SpawnError!SpawnOutcome,

    /// Launch one worker WITHOUT waiting. Returns an in-flight `Handle` the
    /// scheduler parks the coroutine against. NON-BLOCKING.
    startFn: *const fn (
        ctx: ?*anyopaque,
        allocator: std.mem.Allocator,
        io: Io,
        inputs: SpawnInputs,
    ) SpawnError!Handle,

    /// Probe an in-flight handle without blocking. Returns `null` while the
    /// worker is still running, the `SpawnOutcome` once it is terminal. After a
    /// non-null return the handle is consumed (its resources freed) and MUST NOT
    /// be polled/waited again.
    pollFn: *const fn (
        ctx: ?*anyopaque,
        allocator: std.mem.Allocator,
        io: Io,
        handle: *Handle,
    ) SpawnError!?SpawnOutcome,

    /// Block until `handle` completes and return its outcome. Consumes the
    /// handle.
    waitFn: *const fn (
        ctx: ?*anyopaque,
        allocator: std.mem.Allocator,
        io: Io,
        handle: *Handle,
    ) SpawnError!SpawnOutcome,

    /// Drives the spawn (BLOCKING). Thin wrapper that just dispatches through
    /// `runFn`.
    pub fn run(
        self: Spawner,
        allocator: std.mem.Allocator,
        io: Io,
        inputs: SpawnInputs,
    ) SpawnError!SpawnOutcome {
        return self.runFn(self.ctx, allocator, io, inputs);
    }

    /// Launch a worker non-blocking; returns the in-flight handle.
    pub fn start(
        self: Spawner,
        allocator: std.mem.Allocator,
        io: Io,
        inputs: SpawnInputs,
    ) SpawnError!Handle {
        return self.startFn(self.ctx, allocator, io, inputs);
    }

    /// Non-blocking probe of an in-flight handle.
    pub fn poll(
        self: Spawner,
        allocator: std.mem.Allocator,
        io: Io,
        handle: *Handle,
    ) SpawnError!?SpawnOutcome {
        return self.pollFn(self.ctx, allocator, io, handle);
    }

    /// Block until `handle` completes.
    pub fn wait(
        self: Spawner,
        allocator: std.mem.Allocator,
        io: Io,
        handle: *Handle,
    ) SpawnError!SpawnOutcome {
        return self.waitFn(self.ctx, allocator, io, handle);
    }
};

/// Handle is an in-flight worker — the result of `Spawner.start`. It carries the
/// per-spawner state needed to poll/wait the worker to completion. A tagged
/// union so RealSpawner (a live `std.process.Child`) and FakeSpawner (a canned
/// outcome with a countdown) share one type at the scheduler boundary.
///
/// The scheduler treats a Handle opaquely: it `poll`s or `wait`s it via the
/// owning Spawner and never inspects the variant.
pub const Handle = union(enum) {
    /// A live `std.process.spawn`'d child. stdout/stderr are drained at
    /// poll/wait time (after the child exits) so start() stays non-blocking.
    real: RealHandle,
    /// A fake in-flight worker: returns its canned outcome after `poll_until`
    /// polls report "still running". `wait` returns it immediately.
    fake: FakeHandle,
};

/// RealHandle is the live-child state for an in-flight RealSpawner worker.
pub const RealHandle = struct {
    /// The spawned child process. stdin has already been written + closed at
    /// start() time; stdout/stderr are drained at poll/wait time.
    child: std.process.Child,
};

/// FakeHandle is the in-flight state for a FakeSpawner worker. It models an
/// async transition: `poll_remaining` non-blocking probes report "still
/// running" (null) before the handle reports its canned outcome. `wait` returns
/// the canned outcome immediately (the N=1 scheduler path).
pub const FakeHandle = struct {
    /// Pointer back to the FakeSpawnerState so poll/wait can read the canned
    /// outcome + record that the transition was observed.
    state: *FakeSpawnerState,
    /// Number of `poll` calls that still report "still running" before the
    /// handle reports terminal. Zero ⇒ the next poll returns the outcome.
    poll_remaining: u32,
};

// ---------------------------------------------------------------------------
// RealSpawner — runs the actual `claude --print ...` subprocess.
// ---------------------------------------------------------------------------

/// realRunFn is the runFn for the real spawner. It builds the argv via
/// `buildSpawnArgv`, spawns `claude` as a child process, writes the brief to
/// the child's stdin (closing stdin afterward so the child sees EOF), and waits
/// for completion. Stdout/stderr are captured and returned as the
/// `SpawnOutcome`.
///
/// Note: this is the LIVE-COST path. It is reachable only when the test or
/// caller explicitly opts in (e.g. `PLANAR_EXECUTE_LIVE_AGENT=1`). In CI the
/// FakeSpawner is used instead.
fn realRunFn(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    inputs: SpawnInputs,
) SpawnError!SpawnOutcome {
    // The blocking path is now start() + wait() — identical observable
    // behavior to the pre-M5 inline spawn+drain+wait, just routed through the
    // async surface so there is a single code path to maintain.
    var handle = try realStartFn(ctx, allocator, io, inputs);
    return realWaitFn(ctx, allocator, io, &handle);
}

/// realStartFn launches the worker WITHOUT waiting (the M5 non-blocking start).
/// It builds the argv, spawns `claude` via `std.process.spawn`, writes the
/// brief to the child's stdin and closes it (so the child sees EOF and begins
/// work), then returns the in-flight `Handle`. stdout/stderr are NOT drained
/// here — that happens in `realWaitFn`/`realPollFn` after the child exits, so
/// `start` stays non-blocking.
fn realStartFn(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    inputs: SpawnInputs,
) SpawnError!Handle {
    _ = ctx;

    const argv = try buildSpawnArgv(allocator, inputs);
    defer freeSpawnArgv(allocator, argv);

    // Decisions 358 + 365 (worker invocation contract): the real spawn path
    // MUST use a constrained env map (shim PATH + stripped planar-internal
    // env vars). Inheriting the parent's env structurally bypasses the
    // worker_env contract — `planar` would be reachable on PATH and
    // PLANAR_DB / PLANAR_CONFIG_PATH would leak through. The caller
    // (driveAgentCall in main.zig) builds the env via worker_env.buildWorkerEnv
    // and threads it via inputs.env_map. A null here is a wiring bug: panic
    // loudly so a future regression is impossible to ship silently.
    const env_map = inputs.env_map orelse @panic(
        "spawn.realStartFn: inputs.env_map is null — the constrained worker env was not built. Decisions 358 + 365 require the worker to be spawned with worker_env.buildWorkerEnv. See AgentDriver.env_builder in main.zig.",
    );

    // Spawn the child with stdin piped (so we can stream the brief), stdout/
    // stderr piped (so we can capture them). cwd is the cycle worktree so any
    // unqualified file ops inside the worker land inside it. environ_map is the
    // constrained worker env (shim PATH = planar-agent + git only). The env
    // policy is an ALLOW-LIST, not a deny-list: ALL PLANAR_* vars are stripped
    // except those in worker_env.ALLOWED_PLANAR_VARS (currently just
    // PLANAR_WORKBENCH_ROOT), and PATH is overwritten with the shim dir.
    var child = std.process.spawn(io, .{
        .argv = argv,
        .cwd = .{ .path = inputs.worktree_path },
        .environ_map = env_map,
        .stdin = .pipe,
        .stdout = .pipe,
        .stderr = .pipe,
    }) catch return SpawnError.SubprocessFailed;
    errdefer child.kill(io);

    // Write the brief to the child's stdin, then close it so the child sees
    // EOF and starts processing. EPIPE (child already exited) is converted to
    // StdinWriteFailed; we still wait() to collect the term. The brief is
    // bounded (multi-KB) so a single write+close is acceptable at start; the
    // OS pipe buffer holds it while the worker runs.
    if (child.stdin) |stdin_file| {
        var write_failed = false;
        stdin_file.writeStreamingAll(io, inputs.brief) catch {
            write_failed = true;
        };
        stdin_file.close(io);
        child.stdin = null;
        if (write_failed) {
            _ = child.wait(io) catch {};
            return SpawnError.StdinWriteFailed;
        }
    }

    return .{ .real = .{ .child = child } };
}

/// drainAndBuildOutcome drains the (already-exited) child's stdout/stderr and
/// returns the captured `SpawnOutcome` with the given exit code. Shared by the
/// blocking `realWaitFn` and the non-blocking `realPollFn` so both produce an
/// identical outcome shape once the child is terminal. The caller has already
/// reaped the pid (wait or WNOHANG) and is responsible for cleaning up the
/// process handle (`child.id`); this function only drains the pipes (which
/// `drainPipe` closes + nulls).
fn drainAndBuildOutcome(
    allocator: std.mem.Allocator,
    io: Io,
    child: *std.process.Child,
    exit_code: u32,
) SpawnError!SpawnOutcome {
    // Drain stdout and stderr. Both are bounded at 4 MiB to avoid runaway
    // memory on a misbehaving worker.
    const max_capture: usize = 4 * 1024 * 1024;
    const stdout_slice = drainPipe(allocator, io, &child.stdout, max_capture) catch
        return SpawnError.SubprocessFailed;
    errdefer allocator.free(stdout_slice);
    const stderr_slice = drainPipe(allocator, io, &child.stderr, max_capture) catch
        return SpawnError.SubprocessFailed;
    errdefer allocator.free(stderr_slice);

    return .{
        .exit_code = exit_code,
        .stdout = stdout_slice,
        .stderr = stderr_slice,
    };
}

/// termExitCode maps a process `Term` to the SpawnOutcome exit code: the literal
/// exit status on normal termination, or the 255 sentinel for signal/abnormal
/// termination (the terminal-fallback matrix only branches on 0 vs non-zero).
fn termExitCode(term: std.process.Child.Term) u32 {
    return switch (term) {
        .exited => |code| code,
        else => 255,
    };
}

/// realWaitFn blocks until the in-flight child exits, drains its stdout/stderr,
/// and returns the captured `SpawnOutcome`. Consumes the handle.
fn realWaitFn(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    handle: *Handle,
) SpawnError!SpawnOutcome {
    _ = ctx;
    var child = &handle.real.child;
    errdefer child.kill(io);

    // Drain stdout and stderr BEFORE waiting so a worker that fills its pipe
    // buffer cannot deadlock against our wait. Both bounded at 4 MiB.
    const max_capture: usize = 4 * 1024 * 1024;
    const stdout_slice = drainPipe(allocator, io, &child.stdout, max_capture) catch {
        _ = child.wait(io) catch {};
        return SpawnError.SubprocessFailed;
    };
    errdefer allocator.free(stdout_slice);
    const stderr_slice = drainPipe(allocator, io, &child.stderr, max_capture) catch {
        _ = child.wait(io) catch {};
        return SpawnError.SubprocessFailed;
    };
    errdefer allocator.free(stderr_slice);

    const term = child.wait(io) catch return SpawnError.SubprocessFailed;

    return .{
        .exit_code = termExitCode(term),
        .stdout = stdout_slice,
        .stderr = stderr_slice,
    };
}

/// realPollFn is a GENUINE non-blocking probe (plan 492 M5 task 3183). It checks
/// whether the child has exited WITHOUT blocking, via `waitpid(pid, &status,
/// WNOHANG)` on POSIX:
///
///   - waitpid returns 0  → the child is still running → return `null` (the
///     scheduler's wait-for-any loop sleeps briefly and polls the next handle).
///   - waitpid returns pid → the child has exited; we have ALREADY reaped it
///     (WNOHANG consumed the zombie), so we must NOT call `child.wait` (that
///     would double-reap, an errnoBug panic in the stdlib). We drain the pipes,
///     null out `child.id`, and return the `SpawnOutcome`.
///   - waitpid errors      → SubprocessFailed.
///
/// This is what makes `parallel`'s wait-for-any correct: polling handle[i] never
/// stalls on a sibling that has not finished. The blocking `wait` remains for
/// the N=1 fast path. On Windows we fall back to the blocking `wait` (the M5
/// orchestrator is POSIX-only; documented so a future Windows port extends here).
fn realPollFn(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    handle: *Handle,
) SpawnError!?SpawnOutcome {
    if (builtin.os.tag == .windows) {
        // No non-blocking peek wired for Windows; the orchestrator is POSIX-only
        // this milestone. Fall back to the blocking wait so the contract holds.
        return try realWaitFn(ctx, allocator, io, handle);
    }

    const child = &handle.real.child;
    const pid = child.id orelse return SpawnError.SubprocessFailed;

    var status: c_int = 0;
    const rc = std.c.waitpid(pid, &status, @intCast(posix.W.NOHANG));
    if (rc == 0) {
        // Still running — non-blocking probe reports "not yet".
        return null;
    }
    if (rc < 0) {
        return SpawnError.SubprocessFailed;
    }

    // The child has exited and WE reaped it via WNOHANG. Null `child.id`
    // IMMEDIATELY so neither a later `child.wait`/`child.kill` nor the error
    // path below can double-reap the pid (a `wait4`/`waitpid` on a reaped pid
    // returns ECHILD, which the stdlib treats as an errnoBug panic). After this
    // point only the pipes need closing — `drainPipe` closes them per-stream;
    // on a drain failure we close any still-open pipe explicitly (no re-wait).
    child.id = null;
    const term = std.Io.Threaded.statusToTerm(@bitCast(status));
    const exit_code = termExitCode(term);

    return drainAndBuildOutcome(allocator, io, child, exit_code) catch |err| {
        if (child.stdin) |f| {
            f.close(io);
            child.stdin = null;
        }
        if (child.stdout) |f| {
            f.close(io);
            child.stdout = null;
        }
        if (child.stderr) |f| {
            f.close(io);
            child.stderr = null;
        }
        return err;
    };
}

/// drainPipe reads up to `max_bytes` from a piped child stream and returns the
/// captured bytes as an allocator-owned slice. The child's File handle is
/// closed afterward (and the field nulled). If the stream exceeds `max_bytes`
/// it is truncated to the limit (StreamTooLong is treated as "captured up to
/// the limit and stop" — the worker's stream contract is not load-bearing past
/// the cap; we just need the diagnostic).
fn drainPipe(
    allocator: std.mem.Allocator,
    io: Io,
    file_field: *?std.Io.File,
    max_bytes: usize,
) ![]u8 {
    const file = file_field.* orelse return try allocator.alloc(u8, 0);
    defer file.close(io);
    defer file_field.* = null;

    var read_buf: [4096]u8 = undefined;
    var reader_state = file.readerStreaming(io, &read_buf);
    const reader = &reader_state.interface;

    return reader.allocRemaining(allocator, .limited(max_bytes)) catch |err| switch (err) {
        // Treat StreamTooLong as "captured everything we cared about" — the
        // limit is a defense-against-OOM cap, not a correctness contract.
        error.StreamTooLong => try allocator.alloc(u8, 0),
        else => return err,
    };
}

/// Returns a Spawner backed by the real `claude --print ...` subprocess. Use
/// for actual end-to-end runs (the LIVE-COST path). Tests should use
/// `fakeSpawner` instead.
pub fn realSpawner() Spawner {
    return .{
        .ctx = null,
        .runFn = realRunFn,
        .startFn = realStartFn,
        .pollFn = realPollFn,
        .waitFn = realWaitFn,
    };
}

// ---------------------------------------------------------------------------
// FakeSpawner — records argv + brief, returns a canned outcome.
// ---------------------------------------------------------------------------

/// One recorded fake-spawn invocation: the argv the real spawner WOULD have
/// run, the role + worktree it was invoked for, and the brief it received on
/// stdin. All fields are heap-owned copies; freed by `FakeSpawnerState.deinit`.
pub const FakeInvocation = struct {
    /// The argv shape — exactly what `buildSpawnArgv` returned. Each element
    /// is a heap-owned dupe so the invocation outlives the inputs.
    argv: [][]const u8,
    /// The role the spawn was for.
    role: role_model.Role,
    /// The cycle worktree path the spawn would have used as cwd.
    worktree_path: []const u8,
    /// The brief that would have been written to stdin.
    brief: []const u8,
    /// Snapshot of the env_map the spawner would have passed to
    /// `std.process.spawn`. Captured as flat key/value pairs (heap-owned)
    /// so the invocation outlives the original map. Empty when the caller
    /// passed `env_map = null` (which the real spawner panics on; tests
    /// that don't drive the env-wiring path use null intentionally).
    env_pairs: []EnvPair,

    fn deinit(self: *FakeInvocation, allocator: std.mem.Allocator) void {
        for (self.argv) |a| allocator.free(a);
        allocator.free(self.argv);
        allocator.free(self.worktree_path);
        allocator.free(self.brief);
        for (self.env_pairs) |p| {
            allocator.free(p.key);
            allocator.free(p.value);
        }
        allocator.free(self.env_pairs);
    }

    /// Looks up an env var by key in the recorded snapshot. Returns null when
    /// the key is absent (the worker_env contract: stripped vars are gone).
    pub fn envGet(self: FakeInvocation, key: []const u8) ?[]const u8 {
        for (self.env_pairs) |p| {
            if (std.mem.eql(u8, p.key, key)) return p.value;
        }
        return null;
    }
};

/// A single recorded env-map entry on a FakeInvocation.
pub const EnvPair = struct {
    key: []const u8,
    value: []const u8,
};

/// State for a FakeSpawner: the canned outcome it returns AND the list of
/// invocations it has recorded. Tests inspect `invocations` after their run to
/// assert what the harness would have spawned.
pub const FakeSpawnerState = struct {
    allocator: std.mem.Allocator,
    /// The canned outcome every `run` returns. Cloned per-call so the caller
    /// can `deinit` each independently.
    canned_exit_code: u32,
    canned_stdout: []const u8,
    canned_stderr: []const u8,
    /// Recorded invocations, appended in spawn order (at `start` time).
    invocations: std.ArrayList(FakeInvocation),
    /// Forced error to return instead of an outcome (null = return outcome).
    /// Lets tests exercise the SubprocessFailed / StdinWriteFailed branches.
    forced_error: ?SpawnError = null,

    // ---- M5 async-model observability (task 3182) ----
    //
    // The scheduler must drive the worker async: start → (poll*) → terminal.
    // These counters let a unit test assert the scheduler actually drove the
    // handle through the async surface rather than blocking inline, and that
    // the coroutine resumed ONLY after the worker reached terminal.
    //
    /// How many `poll` calls report "still running" (null) before a polled
    /// handle reports its canned outcome. The fake handle starts with this
    /// value; each null-returning poll decrements it. `wait` ignores it (the
    /// N=1 scheduler path waits the single handle and gets the outcome at once).
    poll_until_done: u32 = 0,
    /// Per-start poll countdowns, consumed in `start` order (plan 492 task 3183
    /// N-way tests). When non-empty, the i-th `start` uses `poll_until_done_seq[i]`
    /// as its handle's `poll_remaining` instead of the scalar `poll_until_done`.
    /// This lets a test model OUT-OF-ORDER completion across N concurrent
    /// workers (worker 2 finishes before worker 0, etc.) so the wait-for-any +
    /// original-order-results contract can be asserted. Borrowed slice; not
    /// owned by the state. `start`s beyond its length fall back to the scalar.
    poll_until_done_seq: []const u32 = &.{},
    /// Number of times `start` was called.
    start_count: u32 = 0,
    /// Number of times `poll` was called.
    poll_count: u32 = 0,
    /// Number of times `wait` was called.
    wait_count: u32 = 0,
    /// Set true the first time a handle reported its terminal outcome (via
    /// poll or wait). A continuation that runs before this is set would mean
    /// the scheduler resumed the coroutine BEFORE the worker reached terminal —
    /// the ordering bug the async model exists to prevent.
    reached_terminal: bool = false,
    /// Live count of workers currently in flight: incremented at `start`,
    /// decremented when a handle reports terminal (poll/wait). `peak_inflight`
    /// records the high-water mark. Used by the N>MAX_SLOTS test to prove
    /// queueing (peak never exceeds the concurrency cap) and by the N-way test
    /// to prove genuine concurrency (peak > 1, not sequential).
    live_inflight: u32 = 0,
    peak_inflight: u32 = 0,

    pub fn init(
        allocator: std.mem.Allocator,
        canned_exit_code: u32,
        canned_stdout: []const u8,
        canned_stderr: []const u8,
    ) FakeSpawnerState {
        return .{
            .allocator = allocator,
            .canned_exit_code = canned_exit_code,
            .canned_stdout = canned_stdout,
            .canned_stderr = canned_stderr,
            .invocations = .empty,
        };
    }

    pub fn deinit(self: *FakeSpawnerState) void {
        for (self.invocations.items) |*inv| inv.deinit(self.allocator);
        self.invocations.deinit(self.allocator);
    }

    /// Build a Spawner that drives this state.
    pub fn spawner(self: *FakeSpawnerState) Spawner {
        return .{
            .ctx = self,
            .runFn = fakeRunFn,
            .startFn = fakeStartFn,
            .pollFn = fakePollFn,
            .waitFn = fakeWaitFn,
        };
    }
};

/// recordFakeInvocation dupes the spawn inputs (argv, worktree, brief, env
/// snapshot) onto the state's allocator and appends a FakeInvocation. Shared by
/// the blocking `fakeRunFn` and the async `fakeStartFn` so both record an
/// identical observable invocation.
fn recordFakeInvocation(state: *FakeSpawnerState, inputs: SpawnInputs) SpawnError!void {
    // Build argv via the same pure function the real spawner uses, then dupe
    // each element so the invocation owns its storage (the borrowed inputs
    // may go out of scope before tests inspect the record).
    const argv = try buildSpawnArgv(state.allocator, inputs);
    defer freeSpawnArgv(state.allocator, argv);

    var owned_argv = state.allocator.alloc([]const u8, argv.len) catch
        return SpawnError.OutOfMemory;
    errdefer state.allocator.free(owned_argv);
    var filled: usize = 0;
    errdefer {
        for (owned_argv[0..filled]) |s| state.allocator.free(s);
    }
    for (argv, 0..) |a, idx| {
        owned_argv[idx] = state.allocator.dupe(u8, a) catch return SpawnError.OutOfMemory;
        filled = idx + 1;
    }

    const worktree_owned = state.allocator.dupe(u8, inputs.worktree_path) catch
        return SpawnError.OutOfMemory;
    errdefer state.allocator.free(worktree_owned);
    const brief_owned = state.allocator.dupe(u8, inputs.brief) catch
        return SpawnError.OutOfMemory;
    errdefer state.allocator.free(brief_owned);

    // Snapshot the env map (heap-owned key/value pairs) so the recorded
    // invocation outlives the original. When inputs.env_map is null we
    // record an empty snapshot — the real spawner panics on null but tests
    // that drive other branches deliberately leave it unset.
    var env_pairs_buf: std.ArrayList(EnvPair) = .empty;
    errdefer {
        for (env_pairs_buf.items) |p| {
            state.allocator.free(p.key);
            state.allocator.free(p.value);
        }
        env_pairs_buf.deinit(state.allocator);
    }
    if (inputs.env_map) |em| {
        const keys = em.keys();
        const values = em.values();
        env_pairs_buf.ensureTotalCapacity(state.allocator, keys.len) catch return SpawnError.OutOfMemory;
        var k_i: usize = 0;
        while (k_i < keys.len) : (k_i += 1) {
            const k_owned = state.allocator.dupe(u8, keys[k_i]) catch return SpawnError.OutOfMemory;
            errdefer state.allocator.free(k_owned);
            const v_owned = state.allocator.dupe(u8, values[k_i]) catch return SpawnError.OutOfMemory;
            env_pairs_buf.append(state.allocator, .{ .key = k_owned, .value = v_owned }) catch
                return SpawnError.OutOfMemory;
        }
    }
    const env_pairs_owned = env_pairs_buf.toOwnedSlice(state.allocator) catch
        return SpawnError.OutOfMemory;
    errdefer {
        for (env_pairs_owned) |p| {
            state.allocator.free(p.key);
            state.allocator.free(p.value);
        }
        state.allocator.free(env_pairs_owned);
    }

    state.invocations.append(state.allocator, .{
        .argv = owned_argv,
        .role = inputs.role,
        .worktree_path = worktree_owned,
        .brief = brief_owned,
        .env_pairs = env_pairs_owned,
    }) catch return SpawnError.OutOfMemory;
}

/// cannedOutcome produces a caller-owned copy of the state's canned outcome.
fn cannedOutcome(state: *FakeSpawnerState, allocator: std.mem.Allocator) SpawnError!SpawnOutcome {
    const stdout = allocator.dupe(u8, state.canned_stdout) catch return SpawnError.OutOfMemory;
    errdefer allocator.free(stdout);
    const stderr = allocator.dupe(u8, state.canned_stderr) catch return SpawnError.OutOfMemory;
    return .{
        .exit_code = state.canned_exit_code,
        .stdout = stdout,
        .stderr = stderr,
    };
}

/// fakeStartFn records the invocation and returns an in-flight FakeHandle. The
/// handle reports "still running" for `poll_until_done` poll probes before
/// reporting the canned outcome (so a test can assert the scheduler polled).
fn fakeStartFn(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    inputs: SpawnInputs,
) SpawnError!Handle {
    _ = allocator;
    _ = io;
    const state: *FakeSpawnerState = @ptrCast(@alignCast(ctx.?));
    const start_index = state.start_count;
    state.start_count += 1;
    if (state.forced_error) |e| return e;
    try recordFakeInvocation(state, inputs);
    // Per-start poll countdown (N-way out-of-order tests) when a sequence is
    // supplied; else the scalar default.
    const poll_remaining: u32 = if (start_index < state.poll_until_done_seq.len)
        state.poll_until_done_seq[start_index]
    else
        state.poll_until_done;
    // Track live + peak in-flight for the concurrency/queueing assertions.
    state.live_inflight += 1;
    if (state.live_inflight > state.peak_inflight) state.peak_inflight = state.live_inflight;
    return .{ .fake = .{ .state = state, .poll_remaining = poll_remaining } };
}

/// fakePollFn is the non-blocking probe. Returns null (still running) while
/// `poll_remaining > 0`, decrementing it each call; once it reaches zero the
/// next poll returns the canned outcome and marks the handle terminal.
fn fakePollFn(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    handle: *Handle,
) SpawnError!?SpawnOutcome {
    _ = io;
    const state: *FakeSpawnerState = @ptrCast(@alignCast(ctx.?));
    state.poll_count += 1;
    if (handle.fake.poll_remaining > 0) {
        handle.fake.poll_remaining -= 1;
        return null;
    }
    state.reached_terminal = true;
    if (state.live_inflight > 0) state.live_inflight -= 1;
    return try cannedOutcome(state, allocator);
}

/// fakeWaitFn blocks (trivially, for the fake) and returns the canned outcome.
/// Marks the handle terminal.
fn fakeWaitFn(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    handle: *Handle,
) SpawnError!SpawnOutcome {
    _ = io;
    _ = handle;
    const state: *FakeSpawnerState = @ptrCast(@alignCast(ctx.?));
    state.wait_count += 1;
    state.reached_terminal = true;
    if (state.live_inflight > 0) state.live_inflight -= 1;
    return try cannedOutcome(state, allocator);
}

/// fakeRunFn is the runFn for FakeSpawner. It builds the argv via
/// `buildSpawnArgv`, dupes it onto the state's allocator, records the
/// invocation, and returns a duped copy of the canned outcome.
fn fakeRunFn(
    ctx: ?*anyopaque,
    allocator: std.mem.Allocator,
    io: Io,
    inputs: SpawnInputs,
) SpawnError!SpawnOutcome {
    _ = io;
    const state: *FakeSpawnerState = @ptrCast(@alignCast(ctx.?));

    if (state.forced_error) |e| return e;
    try recordFakeInvocation(state, inputs);
    return cannedOutcome(state, allocator);
}

// ---------------------------------------------------------------------------
// Unit tests — pure / fake only. No subprocesses.
// ---------------------------------------------------------------------------

const testing = std.testing;

test "spawn: buildSpawnArgv emits exact shape for coder role (task 3176)" {
    const alloc = testing.allocator;

    const inputs = SpawnInputs{
        .role = .coder,
        .worktree_path = "/tmp/wt/cycle/plan-x/task-y",
        .brief = "ignored on argv",
        .role_spec = "you are a coder",
        .env_map = null,
    };
    const argv = try buildSpawnArgv(alloc, inputs);
    defer freeSpawnArgv(alloc, argv);

    // Exact shape: 10 elements, in this precise order.
    try testing.expectEqual(@as(usize, 10), argv.len);
    try testing.expectEqualStrings("claude", argv[0]);
    try testing.expectEqualStrings("--print", argv[1]);
    try testing.expectEqualStrings("--permission-mode", argv[2]);
    try testing.expectEqualStrings("bypassPermissions", argv[3]); // decision 365
    try testing.expectEqualStrings("--model", argv[4]);
    try testing.expectEqualStrings(role_model.OPUS_TIER, argv[5]); // coder → opus
    try testing.expectEqualStrings("--add-dir", argv[6]);
    try testing.expectEqualStrings("/tmp/wt/cycle/plan-x/task-y", argv[7]);
    try testing.expectEqualStrings("--append-system-prompt", argv[8]);
    try testing.expectEqualStrings("you are a coder", argv[9]);
}

test "spawn: buildSpawnArgv uses sonnet tier for documenter/test-coder (task 3176)" {
    const alloc = testing.allocator;
    inline for ([_]role_model.Role{ .documenter, .@"test-coder" }) |r| {
        const argv = try buildSpawnArgv(alloc, .{
            .role = r,
            .worktree_path = "/tmp/wt",
            .brief = "",
            .role_spec = "",
            .env_map = null,
        });
        defer freeSpawnArgv(alloc, argv);
        try testing.expectEqualStrings(role_model.SONNET_TIER, argv[5]);
    }
}

test "spawn: buildSpawnArgv omits --append-system-prompt when role_spec is empty" {
    const alloc = testing.allocator;
    const argv = try buildSpawnArgv(alloc, .{
        .role = .reviewer,
        .worktree_path = "/tmp/wt",
        .brief = "",
        .role_spec = "",
        .env_map = null,
    });
    defer freeSpawnArgv(alloc, argv);

    try testing.expectEqual(@as(usize, 8), argv.len);
    // Last pair is --add-dir <wt>; no --append-system-prompt trailing.
    try testing.expectEqualStrings("--add-dir", argv[6]);
    try testing.expectEqualStrings("/tmp/wt", argv[7]);
}

test "spawn: buildSpawnArgv rejects empty / relative / NUL paths" {
    const alloc = testing.allocator;
    try testing.expectError(SpawnError.InvalidPath, buildSpawnArgv(alloc, .{
        .role = .coder,
        .worktree_path = "",
        .brief = "",
        .role_spec = "",
        .env_map = null,
    }));
    try testing.expectError(SpawnError.InvalidPath, buildSpawnArgv(alloc, .{
        .role = .coder,
        .worktree_path = "relative/path",
        .brief = "",
        .role_spec = "",
        .env_map = null,
    }));
    try testing.expectError(SpawnError.InvalidPath, buildSpawnArgv(alloc, .{
        .role = .coder,
        .worktree_path = "/tmp/with\x00nul",
        .brief = "",
        .role_spec = "",
        .env_map = null,
    }));
}

test "spawn: FakeSpawner records argv + brief and returns canned outcome" {
    const alloc = testing.allocator;
    var fake = FakeSpawnerState.init(alloc, 0, "FAKE-STDOUT", "FAKE-STDERR");
    defer fake.deinit();

    const spawner = fake.spawner();
    const inputs = SpawnInputs{
        .role = .coder,
        .worktree_path = "/tmp/wt/cycle/plan-1/task-2",
        .brief = "BRIEF BODY",
        .role_spec = "ROLE SPEC",
        .env_map = null,
    };
    var outcome = try spawner.run(alloc, std.testing.io, inputs);
    defer outcome.deinit(alloc);

    // Outcome echoes the canned values.
    try testing.expectEqual(@as(u32, 0), outcome.exit_code);
    try testing.expectEqualStrings("FAKE-STDOUT", outcome.stdout);
    try testing.expectEqualStrings("FAKE-STDERR", outcome.stderr);

    // One invocation recorded, with the full argv shape.
    try testing.expectEqual(@as(usize, 1), fake.invocations.items.len);
    const inv = fake.invocations.items[0];
    try testing.expectEqual(role_model.Role.coder, inv.role);
    try testing.expectEqualStrings("/tmp/wt/cycle/plan-1/task-2", inv.worktree_path);
    try testing.expectEqualStrings("BRIEF BODY", inv.brief);
    try testing.expectEqual(@as(usize, 10), inv.argv.len);
    try testing.expectEqualStrings("claude", inv.argv[0]);
    try testing.expectEqualStrings("bypassPermissions", inv.argv[3]);
    try testing.expectEqualStrings("ROLE SPEC", inv.argv[9]);
}

test "spawn: FakeSpawner records multiple invocations in order" {
    const alloc = testing.allocator;
    var fake = FakeSpawnerState.init(alloc, 0, "", "");
    defer fake.deinit();
    const spawner = fake.spawner();

    inline for (.{ "coder", "reviewer", "documenter" }) |role_str| {
        var outcome = try spawner.run(alloc, std.testing.io, .{
            .role = try role_model.Role.fromString(role_str),
            .worktree_path = "/tmp/wt",
            .brief = role_str,
            .role_spec = "",
            .env_map = null,
        });
        defer outcome.deinit(alloc);
    }

    try testing.expectEqual(@as(usize, 3), fake.invocations.items.len);
    try testing.expectEqualStrings("coder", fake.invocations.items[0].brief);
    try testing.expectEqualStrings("reviewer", fake.invocations.items[1].brief);
    try testing.expectEqualStrings("documenter", fake.invocations.items[2].brief);
}

test "spawn: FakeSpawner forced_error short-circuits without recording" {
    const alloc = testing.allocator;
    var fake = FakeSpawnerState.init(alloc, 0, "", "");
    defer fake.deinit();
    fake.forced_error = SpawnError.SubprocessFailed;

    const spawner = fake.spawner();
    try testing.expectError(SpawnError.SubprocessFailed, spawner.run(alloc, std.testing.io, .{
        .role = .coder,
        .worktree_path = "/tmp/wt",
        .brief = "",
        .role_spec = "",
        .env_map = null,
    }));
    try testing.expectEqual(@as(usize, 0), fake.invocations.items.len);
}

test "spawn: realSpawner is constructible and exposes the right surface" {
    // We do NOT actually run it (would burn API credit). Just confirm it
    // satisfies the Spawner interface so a wiring change does not silently
    // break the contract.
    const s = realSpawner();
    try testing.expect(s.ctx == null);
    try testing.expect(@intFromPtr(s.runFn) != 0);
    try testing.expect(@intFromPtr(s.startFn) != 0);
    try testing.expect(@intFromPtr(s.pollFn) != 0);
    try testing.expect(@intFromPtr(s.waitFn) != 0);
}

test "spawn: FakeSpawner async surface — start records, wait returns canned outcome (task 3182)" {
    const alloc = testing.allocator;
    var fake = FakeSpawnerState.init(alloc, 0, "ASYNC-OUT", "ASYNC-ERR");
    defer fake.deinit();
    const spawner = fake.spawner();

    const inputs = SpawnInputs{
        .role = .coder,
        .worktree_path = "/tmp/wt/cycle/plan-1/task-2",
        .brief = "ASYNC BRIEF",
        .role_spec = "ROLE SPEC",
        .env_map = null,
    };

    // start() records the invocation up front (NON-BLOCKING).
    var handle = try spawner.start(alloc, std.testing.io, inputs);
    try testing.expectEqual(@as(u32, 1), fake.start_count);
    try testing.expectEqual(@as(usize, 1), fake.invocations.items.len);
    try testing.expect(!fake.reached_terminal); // not yet waited/polled

    // wait() returns the canned outcome and marks terminal.
    var outcome = try spawner.wait(alloc, std.testing.io, &handle);
    defer outcome.deinit(alloc);
    try testing.expectEqual(@as(u32, 1), fake.wait_count);
    try testing.expect(fake.reached_terminal);
    try testing.expectEqual(@as(u32, 0), outcome.exit_code);
    try testing.expectEqualStrings("ASYNC-OUT", outcome.stdout);

    // The recorded invocation matches the blocking path's shape.
    const inv = fake.invocations.items[0];
    try testing.expectEqualStrings("ASYNC BRIEF", inv.brief);
    try testing.expectEqualStrings("ROLE SPEC", inv.argv[9]);
}

test "spawn: FakeSpawner poll reports still-running then terminal (task 3182)" {
    const alloc = testing.allocator;
    var fake = FakeSpawnerState.init(alloc, 0, "OUT", "");
    defer fake.deinit();
    // Two polls report "still running" before the third returns the outcome.
    fake.poll_until_done = 2;
    const spawner = fake.spawner();

    var handle = try spawner.start(alloc, std.testing.io, .{
        .role = .coder,
        .worktree_path = "/tmp/wt",
        .brief = "B",
        .role_spec = "",
        .env_map = null,
    });

    // First two polls: still running (null).
    try testing.expect((try spawner.poll(alloc, std.testing.io, &handle)) == null);
    try testing.expect(!fake.reached_terminal);
    try testing.expect((try spawner.poll(alloc, std.testing.io, &handle)) == null);
    try testing.expect(!fake.reached_terminal);

    // Third poll: terminal — returns the outcome.
    var maybe = try spawner.poll(alloc, std.testing.io, &handle);
    try testing.expect(maybe != null);
    try testing.expect(fake.reached_terminal);
    try testing.expectEqual(@as(u32, 3), fake.poll_count);
    maybe.?.deinit(alloc);
}

test "spawn: FakeSpawner async forced_error short-circuits at start (task 3182)" {
    const alloc = testing.allocator;
    var fake = FakeSpawnerState.init(alloc, 0, "", "");
    defer fake.deinit();
    fake.forced_error = SpawnError.SubprocessFailed;
    const spawner = fake.spawner();

    try testing.expectError(SpawnError.SubprocessFailed, spawner.start(alloc, std.testing.io, .{
        .role = .coder,
        .worktree_path = "/tmp/wt",
        .brief = "",
        .role_spec = "",
        .env_map = null,
    }));
    // start_count incremented but no invocation recorded.
    try testing.expectEqual(@as(u32, 1), fake.start_count);
    try testing.expectEqual(@as(usize, 0), fake.invocations.items.len);
}

test "spawn: FakeSpawner records the env_map snapshot so the constrained env is asserted at the spawn boundary (Item J)" {
    // Iter-2 fix for Item J: confirm that when SpawnInputs carries an env_map,
    // the FakeSpawner records a snapshot we can assert against. This is the
    // test the reviewer asked for — it pins the contract that:
    //   * PATH inside the recorded env is the shim dir, NOT the host PATH.
    //   * Every non-allow-listed PLANAR_* var is absent (allow-list posture).
    //   * PLANAR_WORKBENCH_ROOT (the deliberate carve-out) IS preserved when
    //     present in the host env.
    // Together with realRunFn's panic on null env_map, this makes the iter-1
    // regression structurally unshippable: a future RealSpawner that
    // forgets to thread the constrained env panics; a Spawner contract that
    // accepts an env_map records what was passed.
    const alloc = testing.allocator;

    // Build a synthetic Environ.Map shaped like the constrained worker env
    // would be (PATH = shim, PLANAR_WORKBENCH_ROOT preserved, planar-internal
    // vars absent, HOME preserved).
    var env_map = std.process.Environ.Map.init(alloc);
    defer env_map.deinit();
    try env_map.put("PATH", "/tmp/cycle-shim");
    try env_map.put("HOME", "/home/operator");
    try env_map.put("PLANAR_WORKBENCH_ROOT", "/home/operator/.planar/workbench");
    // The stripped vars are deliberately absent — worker_env.constrainEnvMap
    // removed them before this map reached the spawner.

    var fake = FakeSpawnerState.init(alloc, 0, "", "");
    defer fake.deinit();

    const spawner = fake.spawner();
    var outcome = try spawner.run(alloc, std.testing.io, .{
        .role = .coder,
        .worktree_path = "/tmp/wt",
        .brief = "BRIEF",
        .role_spec = "",
        .env_map = &env_map,
    });
    defer outcome.deinit(alloc);

    try testing.expectEqual(@as(usize, 1), fake.invocations.items.len);
    const inv = fake.invocations.items[0];

    // PATH is the constrained shim, not a host PATH.
    const path = inv.envGet("PATH") orelse @panic("PATH missing");
    try testing.expectEqualStrings("/tmp/cycle-shim", path);

    // The deliberate carve-out is preserved.
    const wb = inv.envGet("PLANAR_WORKBENCH_ROOT") orelse @panic("PLANAR_WORKBENCH_ROOT missing");
    try testing.expectEqualStrings("/home/operator/.planar/workbench", wb);

    // HOME (benign) survives.
    const home = inv.envGet("HOME") orelse @panic("HOME missing");
    try testing.expectEqualStrings("/home/operator", home);

    // Every planar-internal var is absent — the constrained env never
    // re-introduces them. worker_env now applies an ALLOW-LIST (strip all
    // PLANAR_* except worker_env.ALLOWED_PLANAR_VARS), so we assert a fixture
    // list of representative PLANAR_* names is absent. PLANAR_WORKBENCH_ROOT
    // (allow-listed, asserted preserved above) is intentionally excluded.
    const stripped_fixtures = [_][]const u8{
        "PLANAR_BIN",
        "PLANAR_HOME",
        "PLANAR_DB",
        "PLANAR_CONFIG_PATH",
        "PLANAR_TEMPLATES_DIR",
        "PLANAR_DISABLE_WORKTREE_GATE",
        "PLANAR_SCOPE",
        "PLANAR_GITHUB_AUTH",
    };
    for (stripped_fixtures) |stripped_key| {
        if (inv.envGet(stripped_key) != null) {
            std.debug.print(
                "\nFakeInvocation.env_pairs unexpectedly contains stripped key {s}\n",
                .{stripped_key},
            );
        }
        try testing.expect(inv.envGet(stripped_key) == null);
    }
}

test "spawn: SpawnInputs.env_map = null + FakeSpawner records an empty env snapshot (test-only path)" {
    // The null env_map is the test-only path: FakeSpawner records an empty
    // snapshot so tests that don't drive the env wiring can still run. The
    // real spawner panics on null (asserted by the @panic in realRunFn) —
    // documented at the call site there.
    const alloc = testing.allocator;
    var fake = FakeSpawnerState.init(alloc, 0, "", "");
    defer fake.deinit();
    const spawner = fake.spawner();
    var outcome = try spawner.run(alloc, std.testing.io, .{
        .role = .coder,
        .worktree_path = "/tmp/wt",
        .brief = "",
        .role_spec = "",
        .env_map = null,
    });
    defer outcome.deinit(alloc);

    try testing.expectEqual(@as(usize, 1), fake.invocations.items.len);
    try testing.expectEqual(@as(usize, 0), fake.invocations.items[0].env_pairs.len);
}
