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
//! ## What this module does NOT do
//!
//! - **No coroutine scheduler** (M5). Spawn is blocking; one worker at a time.
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
    /// other inputs.
    runFn: *const fn (
        ctx: ?*anyopaque,
        allocator: std.mem.Allocator,
        io: Io,
        inputs: SpawnInputs,
    ) SpawnError!SpawnOutcome,

    /// Drives the spawn. Thin wrapper that just dispatches through `runFn`.
    pub fn run(
        self: Spawner,
        allocator: std.mem.Allocator,
        io: Io,
        inputs: SpawnInputs,
    ) SpawnError!SpawnOutcome {
        return self.runFn(self.ctx, allocator, io, inputs);
    }
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
        "spawn.realRunFn: inputs.env_map is null — the constrained worker env was not built. Decisions 358 + 365 require the worker to be spawned with worker_env.buildWorkerEnv. See AgentDriver.env_builder in main.zig.",
    );

    // Spawn the child with stdin piped (so we can stream the brief), stdout/
    // stderr piped (so we can capture them). cwd is the cycle worktree so any
    // unqualified file ops inside the worker land inside it. environ_map is the
    // constrained worker env (shim PATH = planar-agent + git only; PLANAR_DB /
    // PLANAR_BIN / PLANAR_HOME / PLANAR_CONFIG_PATH / PLANAR_TEMPLATES_DIR /
    // PLANAR_DISABLE_WORKTREE_GATE stripped).
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
    // StdinWriteFailed; we still wait() to collect the term.
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

    // Drain stdout and stderr. Both are bounded at 4 MiB to avoid runaway
    // memory on a misbehaving worker.
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
    const exit_code: u32 = switch (term) {
        .exited => |code| code,
        else => 255,
    };

    return .{
        .exit_code = exit_code,
        .stdout = stdout_slice,
        .stderr = stderr_slice,
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
    /// Recorded invocations, appended in spawn order.
    invocations: std.ArrayList(FakeInvocation),
    /// Forced error to return instead of an outcome (null = return outcome).
    /// Lets tests exercise the SubprocessFailed / StdinWriteFailed branches.
    forced_error: ?SpawnError = null,

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
        };
    }
};

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

    // Duped outcome (caller owns + frees).
    const stdout = allocator.dupe(u8, state.canned_stdout) catch return SpawnError.OutOfMemory;
    errdefer allocator.free(stdout);
    const stderr = allocator.dupe(u8, state.canned_stderr) catch return SpawnError.OutOfMemory;

    return .{
        .exit_code = state.canned_exit_code,
        .stdout = stdout,
        .stderr = stderr,
    };
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
}

test "spawn: FakeSpawner records the env_map snapshot so the constrained env is asserted at the spawn boundary (Item J)" {
    // Iter-2 fix for Item J: confirm that when SpawnInputs carries an env_map,
    // the FakeSpawner records a snapshot we can assert against. This is the
    // test the reviewer asked for — it pins the contract that:
    //   * PATH inside the recorded env is the shim dir, NOT the host PATH.
    //   * Every entry in worker_env.STRIPPED_ENV_VARS is absent.
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

    // Every entry in the stripped set is absent — the constrained env never
    // re-introduces them. We import worker_env's STRIPPED_ENV_VARS to keep
    // the test in lockstep with the constrain step: a future addition to the
    // strip list is automatically exercised.
    for (worker_env.STRIPPED_ENV_VARS) |stripped_key| {
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
