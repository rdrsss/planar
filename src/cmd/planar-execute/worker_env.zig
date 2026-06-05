//! worker_env.zig — Constrained environment builder for `claude -p` workers.
//!
//! Builds the environment a `planar-execute` worker subprocess inherits when
//! it is spawned as a `claude -p` invocation. The contract is **load-bearing
//! security**: the worker MUST be able to invoke `planar-agent` (the atomic
//! claim/heartbeat/terminal-verb binary) and `git` (for diff / status / commit
//! during cycle work), but MUST NOT be able to invoke `planar` — the operator
//! binary that would let the worker shell `planar task done`,
//! `planar plan update`, `planar workbench push`, etc., bypassing the claim
//! ritual that the entire harness design rests on.
//!
//! Tech-spec § "Worker invocation contract" (~L312–321) is the authoritative
//! source. Decision 371 (accepted, supersedes 358) locks the binary policy.
//!
//! ## Capability boundary
//!
//! This module holds NO SQLite handle and imports NO db/engine/runtime module.
//! It is a pure process driver: it manipulates filesystem entries (a per-worker
//! shim directory) and constructs an `std.process.Environ.Map`. No subprocess
//! spawn lives here — the M4 spawner cycle (task 3175 et al.) consumes the
//! `WorkerEnv` this module returns and threads it into a `std.process.run` call.
//!
//! ## What this module is NOT
//!
//! - **Not the spawner.** No `claude -p` invocation, no argv assembly, no
//!   `--model` / `--append-system-prompt` plumbing. Those land in 3175/3176.
//! - **Not the brief delivery channel.** Brief stdin / file delivery is the
//!   spawner's job; the env-builder only constructs the env map.
//! - **Not the terminal-verb fallback.** The harness-side rescue path (task
//!   3180) does not live here.
//! - **Not the filesystem-scope containment layer.** PATH controls process
//!   resolution, not filesystem writes. Worktree-scope containment is
//!   enforced by the spawner via `cwd` (the worker is launched with
//!   `cwd = cycle worktree path`) and by the operator's expectation that the
//!   worker writes inside that cwd. Tests asserting "worker cannot mutate
//!   sibling worktree" belong to the spawner cycle, NOT this module.
//!
//! ## Binary policy — explicit decision
//!
//! The contract NAMES what is REQUIRED (`planar-agent`, `git`) and FORBIDS
//! what is DANGEROUS (`planar` — the operator planning binary). It is SILENT
//! on the other planar-family binaries (`planar-watch`, `planar-doc`,
//! `planar-execute`). The safe reading — adopted here — is:
//!
//!   ALLOW (whitelisted): `planar-agent`, `git`.
//!   DENY (everything else from the planar family, including `planar`,
//!         `planar-watch`, `planar-doc`, `planar-execute`).
//!
//! Rationale for denying the silent cases:
//!
//! - `planar-watch` is read-only by capability, but threading it through the
//!   PATH still lets a worker construct rich state-read shapes the harness
//!   does not anticipate; the spec's state-read path is `planar-execute` itself
//!   parsing JSON, not the worker peeking. Keep the surface narrow.
//! - `planar-doc` does not exist as a separate binary today, but a future
//!   binary by that name should be opted IN by an explicit contract change,
//!   not by being silently exposed because it happens to ship in the same
//!   directory.
//! - `planar-execute` is the harness binary; a recursively-spawned worker
//!   running another harness is a foot-gun.
//!
//! The deny posture is enforced MECHANICALLY by the shim-directory design
//! (below), not by a deny-list lookup at PATH-build time.
//!
//! ## Mechanism — per-worker shim directory
//!
//! A naive "set PATH = parent_dir(planar-agent):parent_dir(git)" approach is
//! UNSAFE when `planar-agent` and `planar` share an install directory (the
//! default on operator machines is `~/.planar/bin/planar-agent` next to
//! `~/.planar/bin/planar`). Adding that parent to PATH exposes both binaries.
//!
//! Instead, `buildWorkerEnv` materializes a **per-worker shim directory** under
//! a caller-supplied root (typically `<cycle-worktree>/.planar-execute/shim/`).
//! The shim contains symlinks pointing at the absolute paths of the allowed
//! binaries:
//!
//!     <shim>/planar-agent  ->  /abs/path/to/planar-agent
//!     <shim>/git           ->  /abs/path/to/git
//!
//! PATH is then set to the **shim directory FIRST**, followed by the standard
//! system bin dirs (`/usr/bin:/bin:/usr/sbin:/sbin`). Bare `which planar-agent`
//! returns the shim's symlink; bare `which planar` returns nothing because the
//! shim contains no `planar` entry AND `planar` installs to `~/.planar/bin`
//! (not a system dir). The OS resolves shim symlinks at exec time; the real
//! binary runs normally.
//!
//! The system bin dirs are present so a spawned `claude -p` worker can reach
//! its toolchain — notably `/usr/bin/security` (macOS Keychain credential
//! helper that `claude` needs for auth). Without them, `claude` reports "Not
//! logged in" and no-ops (discovered during task 3241 live-spawn). See
//! decision 371 (supersedes 358) for the full rationale and the security
//! framing. `/usr/local/bin` is deliberately NOT appended (it is where many
//! operator-installed tools land, including some `planar` builds).
//!
//! The load-bearing containment invariant (decision 371) is:
//!   **`planar` (the operator binary) is NOT reachable on the worker PATH.**
//! This is what prevents a worker from bypassing the claim ritual with a bare
//! `planar task done` / `planar plan update`. It holds because `planar`
//! installs to `~/.planar/bin` or `~/.local/bin`, neither of which is a system
//! dir, and `/usr/local/bin` is not appended. The invariant is pinned by a
//! negative test that proves `planar` stays unreachable even with system dirs
//! on PATH (see test "negative: bare `planar` is unreachable on the constrained
//! PATH" — the worker env built there uses the FULL shim+system PATH).
//!
//! This design is robust to the install-layout question: it works whether the
//! two binaries are co-located or in different directories, and it is opt-in
//! by construction (a new binary cannot leak in by being installed in the same
//! dir as an allowed one).
//!
//! ## Env-var policy — ALLOW-LIST (deny-by-default for the `PLANAR_*` namespace)
//!
//! Inheriting the entire host env is generally the right default (the worker
//! needs `HOME`, `USER`, `TMPDIR`, `LANG`, terminal/TTY hints, etc., to behave
//! like a normal `claude -p` invocation). The env-builder does three things:
//!
//! 1. **Copies the host env** as the baseline.
//! 2. **Overrides `PATH`** to the shim-first + system-dirs value (decision 371).
//! 3. **Strips ALL `PLANAR_*` env vars by default**, allowing only the small
//!    explicitly-justified set in `ALLOWED_PLANAR_VARS`.
//!
//! ### Why an allow-list, not a deny-list (the inversion)
//!
//! This module previously enumerated a deny-list of the handful of
//! `PLANAR_*` vars known to be dangerous. That posture is **latent-by-default
//! unsafe**: the runtime honors a growing set of `PLANAR_*` env vars
//! (`PLANAR_BIN`, `PLANAR_HOME`, `PLANAR_DB`, `PLANAR_CONFIG_PATH`,
//! `PLANAR_TEMPLATES_DIR`, `PLANAR_DISABLE_WORKTREE_GATE`, `PLANAR_SCOPE`,
//! `PLANAR_LOCAL_HOME`, `PLANAR_VENDOR`, `PLANAR_VENDOR_SESSION_ID`,
//! `PLANAR_TEMPLATES_DEFAULT_SET`, `PLANAR_GITHUB_AUTH`,
//! `PLANAR_GITHUB_PROJECTS_PARENT_FIELDS`, `PLANAR_LLM_PROVIDER`,
//! `PLANAR_EDITOR`, …), and every one of them is a potential steer-point for
//! an unconstrained worker. Under a deny-list, the next contributor who adds
//! `PLANAR_FOO` to the runtime silently ships an env-steerable worker
//! behavior unless they remember to update THIS file.
//!
//! Inverting to an allow-list closes that gap by construction: any `PLANAR_*`
//! var — known TODAY or added TOMORROW — is stripped unless it appears in
//! `ALLOWED_PLANAR_VARS`. New `PLANAR_*` vars are denied by default; opting a
//! var IN is an explicit, reviewable edit to the allow-set. The strip is a
//! prefix match (`std.mem.startsWith(u8, key, "PLANAR_")`), not an enumerated
//! lookup.
//!
//! ### The allow-set
//!
//! `ALLOWED_PLANAR_VARS` is deliberately minimal. Each member needs a
//! standing justification:
//!
//!    - `PLANAR_WORKBENCH_ROOT` — the worker may need to read workbench
//!      content for spec citations the brief references. The worker's
//!      mutation path is `planar-agent`, which carries its own write
//!      discipline; reading the workbench is not a privilege escalation.
//!      This is the SOLE justified carve-out today.
//!
//! Everything else in the `PLANAR_*` namespace is stripped, including the
//! historically-catastrophic cases the old deny-list named explicitly —
//! e.g. `PLANAR_DB` (would direct the worker's `planar-agent` writes to the
//! operator's real DB instead of the cycle-scoped one) and `PLANAR_BIN`
//! (alt-binary pointer that bypasses the shim PATH).
//!
//! `PATH` is overwritten, not "stripped + re-added", so any host-PATH surface
//! that happened to expose `planar` is replaced wholesale. `PATH` is not a
//! `PLANAR_*` var; the shim-dir PATH override (step 2) is a separate
//! mechanism and is unaffected by the prefix strip.
//!
//! Non-`PLANAR_*` vars (`HOME`, `USER`, `TMPDIR`, `LANG`, `EDITOR`, …) are
//! inherited untouched — the inversion is scoped to the `PLANAR_*` namespace
//! only; the worker still needs the rest of a normal host env to behave.
//!
//! ## Memory ownership
//!
//! `buildWorkerEnv` returns a `WorkerEnv` whose `path` string and `env_map`
//! are heap-owned and the shim directory is materialized on disk. The caller
//! MUST call `WorkerEnv.deinit(allocator, io)` when done, which frees the
//! strings, deinits the env map, and removes the shim directory tree.
//!
//! ## Error mapping
//!
//! All filesystem and allocator errors are mapped into `WorkerEnvError`. No
//! error is silently swallowed.

const std = @import("std");
const Io = std.Io;
const builtin = @import("builtin");

// ---------------------------------------------------------------------------
// Error set
// ---------------------------------------------------------------------------

/// All errors this module can surface. Callers inspect these rather than
/// catching `anyerror`.
pub const WorkerEnvError = error{
    /// Allocator returned OOM while building a path, env, or shim entry.
    OutOfMemory,
    /// One of the supplied binary paths was not absolute, was empty, or
    /// otherwise rejected (e.g. parent-of-root). The contract requires
    /// caller-resolved absolute paths so the env builder stays a pure
    /// function over its inputs.
    InvalidBinaryPath,
    /// A filesystem operation (mkdir for the shim dir, symlink creation,
    /// rm-rf during deinit) failed.
    FsError,
    /// The host environment could not be read (`std.process.Environ.global`
    /// → `createMap` failed for an OS-level reason).
    HostEnvUnreadable,
};

// ---------------------------------------------------------------------------
// Constants — the binary names that may appear in the shim.
// ---------------------------------------------------------------------------

/// The exact filename the `planar-agent` symlink takes inside the shim
/// directory. Lookups via `which planar-agent` from the worker resolve this
/// entry.
pub const SHIM_PLANAR_AGENT: []const u8 = "planar-agent";

/// The exact filename the `git` symlink takes inside the shim directory.
pub const SHIM_GIT: []const u8 = "git";

/// The `PLANAR_` prefix. Every env key starting with this is in the
/// planar-internal namespace and is stripped from the worker env UNLESS it
/// appears in `ALLOWED_PLANAR_VARS`. See top-of-file "Env-var policy" for the
/// allow-list rationale.
pub const PLANAR_ENV_PREFIX: []const u8 = "PLANAR_";

/// The ALLOW-LIST: the only `PLANAR_*` env vars a constrained worker is
/// permitted to inherit. Everything else in the `PLANAR_*` namespace is
/// stripped by default (deny-by-default), so a future `PLANAR_FOO` added to
/// the runtime is denied automatically without any edit to this file. Each
/// member here needs a standing justification (see top-of-file doc block).
pub const ALLOWED_PLANAR_VARS = [_][]const u8{
    // The worker may need to read workbench content for spec citations the
    // brief references. Reading the workbench is not a privilege escalation;
    // the worker's mutation path is the constrained `planar-agent`.
    "PLANAR_WORKBENCH_ROOT",
};

/// Reports whether `key` is in the planar-internal namespace
/// (`PLANAR_`-prefixed) but NOT in the allow-list — i.e. whether the
/// constrain step should strip it.
fn isStrippedPlanarVar(key: []const u8) bool {
    if (!std.mem.startsWith(u8, key, PLANAR_ENV_PREFIX)) return false;
    for (ALLOWED_PLANAR_VARS) |allowed| {
        if (std.mem.eql(u8, key, allowed)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// WorkerEnv — heap-owned env state for a single worker invocation.
// ---------------------------------------------------------------------------

/// A built worker environment: the constrained PATH value, the env map that
/// includes it, and the on-disk shim directory whose lifetime is bound to the
/// env. The caller MUST call `deinit` when done to free strings, the env map,
/// AND remove the shim directory tree.
pub const WorkerEnv = struct {
    /// Absolute path to the shim directory on disk. Owned (heap-allocated
    /// by `allocator`).
    shim_dir: []const u8,
    /// The PATH value as set inside `env_map`: the shim dir FIRST, then the
    /// standard system bin dirs (per decision 371). Owned (heap-allocated by
    /// `allocator`); freed in `deinit`. Kept as a separate field so callers /
    /// tests can assert on it without walking the env map.
    path: []const u8,
    /// The full env map the worker is spawned with. Allocator is captured
    /// internally by `std.process.Environ.Map`.
    env_map: std.process.Environ.Map,

    /// Free heap-owned strings, deinit the env map, and remove the shim
    /// directory tree. Safe to call even if `shim_dir` no longer exists.
    pub fn deinit(self: *WorkerEnv, allocator: std.mem.Allocator, io: Io) void {
        // Best-effort recursive removal of the shim directory. Failures here
        // are NOT surfaced — the directory lives under a caller-chosen root
        // (typically a worktree under `.planar-execute/`) that the harness
        // will clean up on cycle teardown; a stranded symlink-only tree is
        // not a correctness issue.
        std.Io.Dir.cwd().deleteTree(io, self.shim_dir) catch {};

        allocator.free(self.shim_dir);
        allocator.free(self.path);
        self.env_map.deinit();
        self.* = undefined;
    }
};

// ---------------------------------------------------------------------------
// Path-validation helper
// ---------------------------------------------------------------------------

/// Validates a binary path. Required: absolute, non-empty, no embedded NUL.
/// Returns `WorkerEnvError.InvalidBinaryPath` on any failure.
fn validateBinaryPath(path: []const u8) WorkerEnvError!void {
    if (path.len == 0) return WorkerEnvError.InvalidBinaryPath;
    if (!std.fs.path.isAbsolute(path)) return WorkerEnvError.InvalidBinaryPath;
    if (std.mem.indexOfScalar(u8, path, 0) != null) return WorkerEnvError.InvalidBinaryPath;
}

// ---------------------------------------------------------------------------
// buildWorkerPath — pure: returns the PATH value for the constrained worker.
// ---------------------------------------------------------------------------

/// The standard system bin directories appended AFTER the shim dir on the
/// worker PATH. These hold the OS toolchain a real `claude -p` worker depends
/// on at runtime — notably the macOS Keychain credential helper
/// (`/usr/bin/security`) that `claude` shells out to for auth, plus `/bin/sh`,
/// `node` shims, coreutils, etc. Decision 371 (supersedes 358) re-scoped the
/// containment invariant from "only the shim is on PATH" to "the `planar`
/// operator binary is NOT reachable on the worker PATH". The invariant holds
/// because `planar` installs to `~/.planar/bin` / `~/.local/bin`, neither of
/// which is a system dir, and `/usr/local/bin` is deliberately NOT listed here.
///
/// Verified empirically (task 3241 live-spawn): with PATH = shim-only, `claude`
/// on macOS reports "Not logged in" and does nothing; with the system dirs
/// appended it authenticates and commits. Decision 371 formalizes this policy.
const SYSTEM_BIN_DIRS = [_][]const u8{ "/usr/bin", "/bin", "/usr/sbin", "/sbin" };

/// Returns the PATH value the worker should see: the per-worker shim directory
/// FIRST (so its curated `planar-agent` + `git` symlinks always win), followed
/// by the standard system bin dirs (`SYSTEM_BIN_DIRS`). The caller owns the
/// returned slice and MUST free it. This is the pure-function piece of
/// `buildWorkerEnv`; it does not touch the filesystem.
///
/// `planar` is NOT reachable on this PATH: it lives in `~/.planar/bin`, which
/// is not among the appended system dirs, and the shim dir holds no `planar`
/// entry. Decision 371's containment invariant (worker cannot invoke the
/// operator binary to bypass the claim ritual) is therefore preserved; the
/// worker-scoped negative tests still hold (they fabricate `planar` in a tmp
/// bin dir, never a system dir, and use the full shim+system PATH from
/// `buildWorkerEnv` to prove the invariant is not a tautology — task 3244).
///
/// Validates that `shim_dir` is absolute.
pub fn buildWorkerPath(
    allocator: std.mem.Allocator,
    shim_dir: []const u8,
) WorkerEnvError![]u8 {
    try validateBinaryPath(shim_dir);
    // shim_dir : /usr/bin : /bin : /usr/sbin : /sbin
    var parts = std.ArrayList([]const u8).empty;
    defer parts.deinit(allocator);
    parts.append(allocator, shim_dir) catch return WorkerEnvError.OutOfMemory;
    for (SYSTEM_BIN_DIRS) |d| parts.append(allocator, d) catch return WorkerEnvError.OutOfMemory;
    return std.mem.join(allocator, ":", parts.items) catch return WorkerEnvError.OutOfMemory;
}

// ---------------------------------------------------------------------------
// materializeShimDir — creates the shim directory + symlinks on disk.
// ---------------------------------------------------------------------------

/// Materializes a per-worker shim directory at `shim_dir` containing symlinks
/// to the two allowed binaries. If `shim_dir` already exists, its contents
/// are replaced (delete-then-create) so the directory is always exactly the
/// two expected entries; a previous run's stale symlinks do not leak through.
///
/// Both `planar_agent_path` and `git_path` must be absolute, non-empty, and
/// resolvable (the caller resolved them via PATH lookup at startup); this
/// function does not probe for existence — it materializes the symlinks
/// unconditionally.
fn materializeShimDir(
    allocator: std.mem.Allocator,
    io: Io,
    shim_dir: []const u8,
    planar_agent_path: []const u8,
    git_path: []const u8,
) WorkerEnvError!void {
    _ = allocator;

    try validateBinaryPath(shim_dir);
    try validateBinaryPath(planar_agent_path);
    try validateBinaryPath(git_path);

    const cwd = std.Io.Dir.cwd();

    // Replace any prior contents wholesale so we never re-use stale links.
    cwd.deleteTree(io, shim_dir) catch {};

    cwd.createDirPath(io, shim_dir) catch return WorkerEnvError.FsError;

    var shim = cwd.openDir(io, shim_dir, .{}) catch return WorkerEnvError.FsError;
    defer shim.close(io);

    // Create the two symlinks. Note: the symlink TARGET is the absolute path
    // of the real binary; the LINK NAME is the bare shim filename. We do not
    // care if a prior symlink existed because deleteTree above wiped the dir.
    shim.symLink(io, planar_agent_path, SHIM_PLANAR_AGENT, .{}) catch
        return WorkerEnvError.FsError;
    shim.symLink(io, git_path, SHIM_GIT, .{}) catch
        return WorkerEnvError.FsError;
}

// ---------------------------------------------------------------------------
// buildWorkerEnv — top-level: build a complete WorkerEnv ready for spawn.
// ---------------------------------------------------------------------------

/// Builds a `WorkerEnv` for a fresh `claude -p` worker invocation. The
/// returned struct owns its on-disk shim directory, its PATH string, and its
/// env map; the caller MUST `deinit` it.
///
/// `shim_dir` is the absolute path at which the per-worker shim directory
/// should be created. The caller chooses this location — typically a
/// per-cycle subdir under the cycle worktree (e.g.
/// `<cycle-worktree>/.planar-execute/shim/`). Using a per-cycle path keeps
/// concurrent cycle workers from racing on a shared shim.
///
/// `planar_agent_path` and `git_path` are the absolute filesystem paths of
/// the two allowed binaries. The caller is responsible for resolving these
/// (typically via a PATH lookup at harness startup or a config knob) — the
/// env-builder does not probe PATH itself, so it stays a pure function over
/// its inputs.
pub fn buildWorkerEnv(
    allocator: std.mem.Allocator,
    io: Io,
    host_environ: std.process.Environ,
    shim_dir: []const u8,
    planar_agent_path: []const u8,
    git_path: []const u8,
) WorkerEnvError!WorkerEnv {
    // 1) Validate inputs. validateBinaryPath inside materializeShimDir also
    //    runs, but we want to fail fast before mutating the filesystem.
    try validateBinaryPath(shim_dir);
    try validateBinaryPath(planar_agent_path);
    try validateBinaryPath(git_path);

    // 2) Materialize the on-disk shim dir + symlinks.
    try materializeShimDir(allocator, io, shim_dir, planar_agent_path, git_path);
    errdefer std.Io.Dir.cwd().deleteTree(io, shim_dir) catch {};

    // 3) Build the PATH value (shim dir first, then system bin dirs — decision 371).
    const path_value = try buildWorkerPath(allocator, shim_dir);
    errdefer allocator.free(path_value);

    // 4) Build the env map: start from host env, strip dangerous vars,
    //    overwrite PATH.
    var env_map = createConstrainedEnvMap(allocator, host_environ, path_value) catch |err| switch (err) {
        error.OutOfMemory => return WorkerEnvError.OutOfMemory,
        error.HostEnvUnreadable => return WorkerEnvError.HostEnvUnreadable,
    };
    errdefer env_map.deinit();

    // 5) Dupe the shim_dir for ownership.
    const shim_dir_owned = allocator.dupe(u8, shim_dir) catch return WorkerEnvError.OutOfMemory;
    errdefer allocator.free(shim_dir_owned);

    return .{
        .shim_dir = shim_dir_owned,
        .path = path_value,
        .env_map = env_map,
    };
}

/// Internal: read the host env into a Map, then delegate to `constrainEnvMap`.
fn createConstrainedEnvMap(
    allocator: std.mem.Allocator,
    host_environ: std.process.Environ,
    path_value: []const u8,
) error{ OutOfMemory, HostEnvUnreadable }!std.process.Environ.Map {
    var map = host_environ.createMap(allocator) catch |err| switch (err) {
        error.OutOfMemory => return error.OutOfMemory,
        error.Unexpected => return error.HostEnvUnreadable,
    };
    errdefer map.deinit();

    constrainEnvMap(&map, path_value) catch |err| switch (err) {
        error.OutOfMemory => return error.OutOfMemory,
    };

    return map;
}

/// Applies the allow-list policy: strips EVERY `PLANAR_*` env var that is not
/// in `ALLOWED_PLANAR_VARS`, then overrides `PATH` to `path_value`. This is a
/// deny-by-default posture over the `PLANAR_*` namespace — an unknown future
/// `PLANAR_FOO` is stripped without any code change here. Pure operation on an
/// already-populated Map; lets tests drive the constrain step from a synthetic
/// Map without needing to mutate the live process environment.
///
/// Mutating a Map while iterating it is unsafe, so we collect the keys to
/// strip in a first pass (into a temporary `ArrayList`) and remove them in a
/// second pass. The collected key slices are borrowed from the map's own
/// storage; we must NOT use them after `swapRemove` frees them, so the second
/// pass removes-then-discards each in order.
///
/// `Map.swapRemove` (NOT the bare ArrayHashMap one) frees both halves of the
/// kv pair, so no manual ownership management of the entries is needed.
/// `Map.put` copies `path_value`, so ownership of the caller's slice is not
/// transferred. The temporary key list itself uses an arena tied to this call
/// so its backing storage is freed on return.
pub fn constrainEnvMap(
    map: *std.process.Environ.Map,
    path_value: []const u8,
) error{OutOfMemory}!void {
    // First pass: collect the keys to strip. We dupe each key into a
    // scratch arena because `swapRemove` in the second pass frees the map's
    // copy of the key, which would dangle a borrowed slice.
    var arena = std.heap.ArenaAllocator.init(map.allocator);
    defer arena.deinit();
    const scratch = arena.allocator();

    var to_strip = std.ArrayList([]const u8).empty;

    var it = map.iterator();
    while (it.next()) |entry| {
        const key = entry.key_ptr.*;
        if (isStrippedPlanarVar(key)) {
            const owned = scratch.dupe(u8, key) catch return error.OutOfMemory;
            to_strip.append(scratch, owned) catch return error.OutOfMemory;
        }
    }

    // Second pass: remove the collected keys (now safe — not iterating).
    for (to_strip.items) |key| {
        _ = map.swapRemove(key);
    }

    try map.put("PATH", path_value);
}

// ---------------------------------------------------------------------------
// assertSpawnArgv — placeholder for the M4 spawner cycle (task 3176).
// ---------------------------------------------------------------------------

/// Asserts that an argv slice destined for `claude -p` carries the expected
/// `--model <tier>` flag pair. STUB — the M4 spawner cycle (task 3176) fills
/// this in. Surfaced here so tests in adjacent modules can target a stable
/// import path once the spawner lands.
///
/// Until then, the function unconditionally returns `WorkerEnvError.InvalidBinaryPath`
/// (a deliberate sentinel) so any caller wiring it ahead of the spawner cycle
/// fails loudly.
pub fn assertSpawnArgv(argv: []const []const u8, expected_model: []const u8) WorkerEnvError!void {
    _ = argv;
    _ = expected_model;
    return WorkerEnvError.InvalidBinaryPath;
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

/// shellAvailable reports whether `/bin/sh` is usable for the negative tests.
/// On any POSIX host this is trivially true; the probe exists so the tests
/// degrade gracefully on exotic CI environments.
fn shellAvailable(allocator: std.mem.Allocator) bool {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "/bin/sh", "-c", "exit 0" },
    }) catch return false;
    defer allocator.free(r.stdout);
    defer allocator.free(r.stderr);
    return r.term == .exited and r.term.exited == 0;
}

/// mkTmpDir creates a fresh system temp directory via `mktemp -d` and returns
/// its absolute path (heap-owned; caller frees). Mirrors the
/// `mkTmpRepoDir` pattern in worktree.zig.
fn mkTmpDir(allocator: std.mem.Allocator) []const u8 {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "mktemp", "-d", "-t", "planar-wenv.XXXXXX" },
    }) catch @panic("mkTmpDir: mktemp spawn failed");
    defer allocator.free(r.stderr);
    if (!(r.term == .exited and r.term.exited == 0)) {
        allocator.free(r.stdout);
        @panic("mkTmpDir: mktemp non-zero exit");
    }
    const trimmed = std.mem.trim(u8, r.stdout, " \t\r\n");
    const owned = allocator.dupe(u8, trimmed) catch @panic("OOM");
    allocator.free(r.stdout);
    return owned;
}

/// rmTree removes `path` and its contents via `rm -rf` (best-effort cleanup).
fn rmTree(allocator: std.mem.Allocator, path: []const u8) void {
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "rm", "-rf", path },
    }) catch return;
    allocator.free(r.stdout);
    allocator.free(r.stderr);
}

/// writeStubBinary writes a tiny shell script at `path` that exits 0 and
/// chmods it executable. Used by the negative tests to fabricate "real"
/// binaries to symlink into the shim — we never need them to actually
/// execute meaningful logic; the tests only assert which entries resolve on
/// PATH.
fn writeStubBinary(allocator: std.mem.Allocator, path: []const u8) void {
    std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = path,
        .data = "#!/bin/sh\nexit 0\n",
    }) catch @panic("writeStubBinary: writeFile failed");

    // Run chmod explicitly so the file is executable regardless of umask /
    // default permissions.
    const r = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "chmod", "+x", path },
    }) catch @panic("writeStubBinary: chmod spawn failed");
    defer allocator.free(r.stdout);
    defer allocator.free(r.stderr);
}

test "buildWorkerPath: shim dir FIRST, then the system bin dirs" {
    const a = std.testing.allocator;
    const p = try buildWorkerPath(a, "/tmp/shim-xyz");
    defer a.free(p);
    // The shim dir is the leading PATH entry (its curated symlinks always win).
    try std.testing.expectEqualStrings("/tmp/shim-xyz:/usr/bin:/bin:/usr/sbin:/sbin", p);
    // And it is the FIRST entry, so a shim `git`/`planar-agent` shadows any
    // system copy.
    try std.testing.expect(std.mem.startsWith(u8, p, "/tmp/shim-xyz:"));
    // `planar` is NOT reachable: none of the appended dirs is ~/.planar/bin.
    try std.testing.expect(std.mem.indexOf(u8, p, ".planar/bin") == null);
}

test "buildWorkerPath: rejects non-absolute paths" {
    const a = std.testing.allocator;
    try std.testing.expectError(WorkerEnvError.InvalidBinaryPath, buildWorkerPath(a, "shim"));
    try std.testing.expectError(WorkerEnvError.InvalidBinaryPath, buildWorkerPath(a, ""));
}

test "validateBinaryPath: rejects empty / relative / NUL-bearing paths" {
    try std.testing.expectError(WorkerEnvError.InvalidBinaryPath, validateBinaryPath(""));
    try std.testing.expectError(WorkerEnvError.InvalidBinaryPath, validateBinaryPath("git"));
    try std.testing.expectError(WorkerEnvError.InvalidBinaryPath, validateBinaryPath("relative/path"));
    try std.testing.expectError(WorkerEnvError.InvalidBinaryPath, validateBinaryPath("/abs/with\x00nul"));
    try validateBinaryPath("/usr/bin/git");
}

test "materializeShimDir: creates the dir + two symlinks; idempotent" {
    const a = std.testing.allocator;
    if (!shellAvailable(a)) return error.SkipZigTest;

    const root = mkTmpDir(a);
    defer a.free(root);
    defer rmTree(a, root);

    // Stub binaries to point the symlinks at.
    const bin_dir = try std.fmt.allocPrint(a, "{s}/realbin", .{root});
    defer a.free(bin_dir);
    std.Io.Dir.cwd().createDirPath(std.testing.io, bin_dir) catch @panic("mkdir realbin");

    const stub_agent = try std.fmt.allocPrint(a, "{s}/planar-agent-real", .{bin_dir});
    defer a.free(stub_agent);
    const stub_git = try std.fmt.allocPrint(a, "{s}/git-real", .{bin_dir});
    defer a.free(stub_git);
    writeStubBinary(a, stub_agent);
    writeStubBinary(a, stub_git);

    const shim = try std.fmt.allocPrint(a, "{s}/shim", .{root});
    defer a.free(shim);

    try materializeShimDir(a, std.testing.io, shim, stub_agent, stub_git);

    // Both symlinks resolve to executable files.
    const sh_cmd = try std.fmt.allocPrint(a, "PATH={s} command -v planar-agent", .{shim});
    defer a.free(sh_cmd);
    const r1 = try std.process.run(a, std.testing.io, .{
        .argv = &.{ "/bin/sh", "-c", sh_cmd },
    });
    defer a.free(r1.stdout);
    defer a.free(r1.stderr);
    try std.testing.expect(r1.term == .exited and r1.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, r1.stdout, "planar-agent") != null);

    // Second call wipes + recreates cleanly.
    try materializeShimDir(a, std.testing.io, shim, stub_agent, stub_git);
}

test "buildWorkerEnv: constructs a complete env; deinit cleans up the shim dir" {
    const a = std.testing.allocator;
    if (!shellAvailable(a)) return error.SkipZigTest;

    const root = mkTmpDir(a);
    defer a.free(root);
    defer rmTree(a, root);

    const bin_dir = try std.fmt.allocPrint(a, "{s}/realbin", .{root});
    defer a.free(bin_dir);
    std.Io.Dir.cwd().createDirPath(std.testing.io, bin_dir) catch @panic("mkdir realbin");

    const stub_agent = try std.fmt.allocPrint(a, "{s}/planar-agent-real", .{bin_dir});
    defer a.free(stub_agent);
    const stub_git = try std.fmt.allocPrint(a, "{s}/git-real", .{bin_dir});
    defer a.free(stub_git);
    writeStubBinary(a, stub_agent);
    writeStubBinary(a, stub_git);

    const shim = try std.fmt.allocPrint(a, "{s}/shim", .{root});
    defer a.free(shim);

    var env = try buildWorkerEnv(a, std.testing.io, std.testing.environ, shim, stub_agent, stub_git);
    defer env.deinit(a, std.testing.io);

    // env.path leads with the shim dir, then the system bin dirs.
    const shim_prefix = try std.fmt.allocPrint(a, "{s}:", .{shim});
    defer a.free(shim_prefix);
    try std.testing.expect(std.mem.startsWith(u8, env.path, shim_prefix));
    try std.testing.expect(std.mem.indexOf(u8, env.path, "/usr/bin") != null);

    // PATH is in the env map and matches env.path.
    const path_in_map = env.env_map.get("PATH") orelse @panic("PATH missing");
    try std.testing.expectEqualStrings(env.path, path_in_map);

    // Shim dir exists.
    var d = std.Io.Dir.cwd().openDir(std.testing.io, shim, .{}) catch
        @panic("shim dir missing");
    d.close(std.testing.io);
}

test "constrainEnvMap: strips PLANAR_BIN / PLANAR_HOME and overrides PATH on a synthetic Map" {
    const a = std.testing.allocator;

    // Build a synthetic Map populated with PLANAR_BIN, PLANAR_HOME, plus a
    // benign entry that must be preserved (HOME). We do NOT touch
    // std.testing.environ — this is a pure test of the constrain step.
    var map = std.process.Environ.Map.init(a);
    defer map.deinit();
    try map.put("PATH", "/usr/bin:/bin");
    try map.put("HOME", "/home/operator");
    try map.put("PLANAR_BIN", "/opt/leak/planar");
    try map.put("PLANAR_HOME", "/opt/leak/.planar");

    try constrainEnvMap(&map, "/tmp/shim-test");

    // Stripped:
    try std.testing.expect(map.get("PLANAR_BIN") == null);
    try std.testing.expect(map.get("PLANAR_HOME") == null);
    // Preserved:
    const home = map.get("HOME") orelse @panic("HOME missing");
    try std.testing.expectEqualStrings("/home/operator", home);
    // Overridden:
    const path = map.get("PATH") orelse @panic("PATH missing");
    try std.testing.expectEqualStrings("/tmp/shim-test", path);
}

test "constrainEnvMap: no-op strip when the keys are absent; PATH still overridden" {
    const a = std.testing.allocator;

    var map = std.process.Environ.Map.init(a);
    defer map.deinit();
    try map.put("HOME", "/home/operator");

    try constrainEnvMap(&map, "/tmp/shim2");

    try std.testing.expect(map.get("PLANAR_BIN") == null);
    try std.testing.expect(map.get("PLANAR_HOME") == null);
    const path = map.get("PATH") orelse @panic("PATH missing");
    try std.testing.expectEqualStrings("/tmp/shim2", path);
}

test "constrainEnvMap: strips a future-unknown PLANAR_ var without any code change (allow-list posture)" {
    // THE load-bearing allow-list test. PLANAR_QWERTY_FUTURE is a name that
    // does NOT appear anywhere in the codebase and is NOT in
    // ALLOWED_PLANAR_VARS. Under the OLD deny-list (STRIPPED_ENV_VARS, which
    // enumerated only 6 known vars), this key would have SURVIVED
    // constrainEnvMap — the deny-list only stripped the enumerated members.
    // Under the inverted allow-list, the prefix strip removes it by default.
    // This test is the proof of the future-proofing: it would FAIL under the
    // pre-inversion code.
    const a = std.testing.allocator;

    var map = std.process.Environ.Map.init(a);
    defer map.deinit();
    try map.put("PATH", "/usr/bin:/bin");
    try map.put("HOME", "/home/operator");
    try map.put("PLANAR_QWERTY_FUTURE", "/opt/leak/whatever");

    try constrainEnvMap(&map, "/tmp/shim-future");

    if (map.get("PLANAR_QWERTY_FUTURE") != null) {
        std.debug.print("\nPLANAR_QWERTY_FUTURE survived constrainEnvMap — allow-list strip regressed\n", .{});
    }
    try std.testing.expect(map.get("PLANAR_QWERTY_FUTURE") == null);

    // Non-PLANAR_ entry survives; PATH overridden.
    const home = map.get("HOME") orelse @panic("HOME missing");
    try std.testing.expectEqualStrings("/home/operator", home);
    const path = map.get("PATH") orelse @panic("PATH missing");
    try std.testing.expectEqualStrings("/tmp/shim-future", path);
}

test "constrainEnvMap: strips the old deny-list + newly-covered PLANAR_ vars (table-driven)" {
    const a = std.testing.allocator;

    // A fixture list mixing the 6 vars the old deny-list named explicitly
    // with several of the 7+ PLANAR_* vars the runtime honors that the old
    // deny-list MISSED. Under the allow-list, every one of these is stripped
    // by prefix because none are in ALLOWED_PLANAR_VARS.
    const should_strip = [_][]const u8{
        // Old deny-list members:
        "PLANAR_BIN",
        "PLANAR_HOME",
        "PLANAR_DB",
        "PLANAR_CONFIG_PATH",
        "PLANAR_TEMPLATES_DIR",
        "PLANAR_DISABLE_WORKTREE_GATE",
        // Newly-covered by the inversion (the old deny-list did NOT strip these):
        "PLANAR_SCOPE",
        "PLANAR_LOCAL_HOME",
        "PLANAR_VENDOR",
        "PLANAR_VENDOR_SESSION_ID",
        "PLANAR_TEMPLATES_DEFAULT_SET",
        "PLANAR_GITHUB_AUTH",
        "PLANAR_GITHUB_PROJECTS_PARENT_FIELDS",
        "PLANAR_LLM_PROVIDER",
        "PLANAR_EDITOR",
    };

    var map = std.process.Environ.Map.init(a);
    defer map.deinit();
    try map.put("PATH", "/usr/bin:/bin");
    try map.put("HOME", "/home/operator");
    try map.put("PLANAR_WORKBENCH_ROOT", "/home/operator/.planar/workbench");
    for (should_strip) |key| try map.put(key, "/opt/leak/value");

    try constrainEnvMap(&map, "/tmp/shim-table");

    // Every fixture key must be absent post-constrain.
    for (should_strip) |key| {
        if (map.get(key) != null) {
            std.debug.print("\nPLANAR_ var {s} survived constrainEnvMap\n", .{key});
        }
        try std.testing.expect(map.get(key) == null);
    }

    // Benign entries survive.
    const home = map.get("HOME") orelse @panic("HOME missing");
    try std.testing.expectEqualStrings("/home/operator", home);
    // PLANAR_WORKBENCH_ROOT is the deliberate carve-out — it MUST be
    // inherited, the worker reads workbench content via this path.
    const wb = map.get("PLANAR_WORKBENCH_ROOT") orelse @panic("PLANAR_WORKBENCH_ROOT must be inherited");
    try std.testing.expectEqualStrings("/home/operator/.planar/workbench", wb);

    // PATH overridden.
    const path = map.get("PATH") orelse @panic("PATH missing");
    try std.testing.expectEqualStrings("/tmp/shim-table", path);
}

test "constrainEnvMap: every ALLOWED_PLANAR_VARS member is preserved" {
    const a = std.testing.allocator;

    var map = std.process.Environ.Map.init(a);
    defer map.deinit();
    try map.put("PATH", "/usr/bin:/bin");
    // Populate one fixture value per allow-listed var. The assertion walks
    // ALLOWED_PLANAR_VARS so any future addition is exercised automatically.
    for (ALLOWED_PLANAR_VARS) |key| try map.put(key, "/allowed/value");

    try constrainEnvMap(&map, "/tmp/shim-allow");

    for (ALLOWED_PLANAR_VARS) |key| {
        const v = map.get(key) orelse {
            std.debug.print("\nALLOWED_PLANAR_VARS member {s} was wrongly stripped\n", .{key});
            return error.TestUnexpectedResult;
        };
        try std.testing.expectEqualStrings("/allowed/value", v);
    }
}

test "constrainEnvMap: non-PLANAR_ vars are inherited untouched" {
    const a = std.testing.allocator;

    var map = std.process.Environ.Map.init(a);
    defer map.deinit();
    try map.put("HOME", "/home/operator");
    try map.put("USER", "operator");
    try map.put("EDITOR", "vim");
    // A var whose name merely CONTAINS "PLANAR" but does not start with the
    // prefix must NOT be stripped — the policy is a prefix match.
    try map.put("MY_PLANAR_HINT", "keep-me");

    try constrainEnvMap(&map, "/tmp/shim-nonplanar");

    try std.testing.expectEqualStrings("/home/operator", map.get("HOME") orelse @panic("HOME missing"));
    try std.testing.expectEqualStrings("operator", map.get("USER") orelse @panic("USER missing"));
    try std.testing.expectEqualStrings("vim", map.get("EDITOR") orelse @panic("EDITOR missing"));
    try std.testing.expectEqualStrings("keep-me", map.get("MY_PLANAR_HINT") orelse @panic("MY_PLANAR_HINT missing"));
}

test "isStrippedPlanarVar: prefix + allow-list logic" {
    // PLANAR_-prefixed, not allowed -> strip.
    try std.testing.expect(isStrippedPlanarVar("PLANAR_DB"));
    try std.testing.expect(isStrippedPlanarVar("PLANAR_QWERTY_FUTURE"));
    // Allow-listed -> keep.
    try std.testing.expect(!isStrippedPlanarVar("PLANAR_WORKBENCH_ROOT"));
    // Not PLANAR_-prefixed -> keep.
    try std.testing.expect(!isStrippedPlanarVar("HOME"));
    try std.testing.expect(!isStrippedPlanarVar("PATH"));
    try std.testing.expect(!isStrippedPlanarVar("MY_PLANAR_HINT"));
    // Bare "PLANAR" without the underscore is NOT prefixed.
    try std.testing.expect(!isStrippedPlanarVar("PLANAR"));
}

test "constrainEnvMap: strips PLANAR_DB so worker planar-agent calls cannot reach operator DB" {
    // Focused regression test for the iter-2 finding: PLANAR_DB is honored
    // by runtime.resolveDbPath and inherited by both planar and planar-agent.
    // A worker that inherits PLANAR_DB from the harness (or from any operator
    // who has set it ambiently) would direct its planar-agent writes to the
    // operator's real DB instead of the cycle-scoped one. The constrain step
    // MUST drop it.
    const a = std.testing.allocator;

    var map = std.process.Environ.Map.init(a);
    defer map.deinit();
    try map.put("PATH", "/usr/bin:/bin");
    try map.put("PLANAR_DB", "/Users/operator/.planar/planar.db");

    try constrainEnvMap(&map, "/tmp/shim-db");

    try std.testing.expect(map.get("PLANAR_DB") == null);
    const path = map.get("PATH") orelse @panic("PATH missing");
    try std.testing.expectEqualStrings("/tmp/shim-db", path);
}

// ---------------------------------------------------------------------------
// Negative tests — Tests 1 & 2 from the task brief
//
// Each runs an actual subprocess with the constrained PATH and asserts the
// expected resolution outcome. A control invocation with the host (or a
// permissive) PATH proves the probe is meaningful.
//
// Test 3 ("out-of-worktree write refused") and its sibling-worktree variant
// belong to the SPAWNER cycle (M4 tasks 3175/3177), NOT here: PATH controls
// process resolution, not filesystem writes. The spawner enforces filesystem
// containment via the worker's `cwd` parameter. See top-of-file "What this
// module is NOT" for the explicit scoping note.
// ---------------------------------------------------------------------------

test "negative: bare `planar` is unreachable on the constrained PATH (task 3244, decision 371)" {
    // Decision 371 (supersedes 358): the load-bearing invariant is
    // "`planar` (the operator binary) is NOT reachable on the worker PATH",
    // even though the worker PATH now includes system bin dirs
    // (/usr/bin:/bin:/usr/sbin:/sbin) after the shim.
    //
    // This test proves the invariant is NOT a tautology: `planar` is
    // fabricated in a NON-system tmp dir (so it WOULD be reachable on a
    // permissive PATH — the control assertion below proves this). The constrained
    // PATH built by `buildWorkerEnv` includes the shim + system dirs but NOT
    // that tmp dir, so `planar` stays unreachable. The shim symlinks DO work
    // (`planar-agent` resolves) — proving the constrained PATH itself is not
    // broken, only `planar` is absent.
    const a = std.testing.allocator;
    if (!shellAvailable(a)) return error.SkipZigTest;

    const root = mkTmpDir(a);
    defer a.free(root);
    defer rmTree(a, root);

    const bin_dir = try std.fmt.allocPrint(a, "{s}/realbin", .{root});
    defer a.free(bin_dir);
    std.Io.Dir.cwd().createDirPath(std.testing.io, bin_dir) catch @panic("mkdir realbin");

    // Fabricate the dangerous landscape: a `planar` binary sitting next to
    // `planar-agent` in the SAME directory (the operator-machine default).
    // The constrained env must refuse `planar` even though it shares a
    // parent dir with `planar-agent`.
    const stub_planar = try std.fmt.allocPrint(a, "{s}/planar", .{bin_dir});
    defer a.free(stub_planar);
    const stub_agent = try std.fmt.allocPrint(a, "{s}/planar-agent", .{bin_dir});
    defer a.free(stub_agent);
    const stub_git = try std.fmt.allocPrint(a, "{s}/git", .{bin_dir});
    defer a.free(stub_git);
    writeStubBinary(a, stub_planar);
    writeStubBinary(a, stub_agent);
    writeStubBinary(a, stub_git);

    const shim = try std.fmt.allocPrint(a, "{s}/shim", .{root});
    defer a.free(shim);

    // `buildWorkerEnv` builds the FULL worker PATH: shim FIRST, then the
    // system bin dirs (/usr/bin:/bin:/usr/sbin:/sbin). `bin_dir` (where the
    // fake `planar` lives) is NOT in the system dirs and NOT in the shim,
    // so `planar` stays unreachable even though system dirs are on PATH.
    var env = try buildWorkerEnv(a, std.testing.io, std.testing.environ, shim, stub_agent, stub_git);
    defer env.deinit(a, std.testing.io);

    // Verify the PATH includes system dirs (so we are testing the real policy,
    // not a shim-only PATH that would trivially exclude planar too).
    try std.testing.expect(std.mem.indexOf(u8, env.path, "/usr/bin") != null);
    try std.testing.expect(std.mem.indexOf(u8, env.path, "/bin") != null);

    // Probe under the CONSTRAINED env: `command -v planar` must return
    // empty / non-zero.
    const probe_constrained = try std.process.run(a, std.testing.io, .{
        .argv = &.{ "/bin/sh", "-c", "command -v planar" },
        .environ_map = &env.env_map,
    });
    defer a.free(probe_constrained.stdout);
    defer a.free(probe_constrained.stderr);

    // `command -v` exits non-zero when the name does not resolve. Stdout is
    // empty. Either signal is sufficient — assert both for clarity.
    const constrained_ok = probe_constrained.term == .exited and probe_constrained.term.exited == 0;
    try std.testing.expect(!constrained_ok);
    const trimmed_stdout = std.mem.trim(u8, probe_constrained.stdout, " \t\r\n");
    try std.testing.expectEqualStrings("", trimmed_stdout);

    // Also assert that `planar-agent` IS reachable via the shim — this proves
    // the constrained PATH is not simply broken, only `planar` is absent.
    // (Decision 371: shim comes first so `planar-agent` + `git` always resolve.)
    const probe_agent = try std.process.run(a, std.testing.io, .{
        .argv = &.{ "/bin/sh", "-c", "command -v planar-agent" },
        .environ_map = &env.env_map,
    });
    defer a.free(probe_agent.stdout);
    defer a.free(probe_agent.stderr);
    try std.testing.expect(probe_agent.term == .exited and probe_agent.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, probe_agent.stdout, "planar-agent") != null);

    // Control: under a PERMISSIVE PATH that includes `bin_dir`, `planar`
    // resolves. This proves the probe is meaningful — the constrained
    // refusal above is not a tautology (the binary exists and works; it just
    // cannot be found on the constrained PATH).
    const cmd_control = try std.fmt.allocPrint(a, "PATH={s} command -v planar", .{bin_dir});
    defer a.free(cmd_control);
    const probe_control = try std.process.run(a, std.testing.io, .{
        .argv = &.{ "/bin/sh", "-c", cmd_control },
    });
    defer a.free(probe_control.stdout);
    defer a.free(probe_control.stderr);
    try std.testing.expect(probe_control.term == .exited and probe_control.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, probe_control.stdout, "planar") != null);
}

test "negative: `planar-agent` and `git` ARE reachable on the constrained PATH" {
    const a = std.testing.allocator;
    if (!shellAvailable(a)) return error.SkipZigTest;

    const root = mkTmpDir(a);
    defer a.free(root);
    defer rmTree(a, root);

    const bin_dir = try std.fmt.allocPrint(a, "{s}/realbin", .{root});
    defer a.free(bin_dir);
    std.Io.Dir.cwd().createDirPath(std.testing.io, bin_dir) catch @panic("mkdir realbin");

    const stub_agent = try std.fmt.allocPrint(a, "{s}/planar-agent", .{bin_dir});
    defer a.free(stub_agent);
    const stub_git = try std.fmt.allocPrint(a, "{s}/git", .{bin_dir});
    defer a.free(stub_git);
    writeStubBinary(a, stub_agent);
    writeStubBinary(a, stub_git);

    const shim = try std.fmt.allocPrint(a, "{s}/shim", .{root});
    defer a.free(shim);

    var env = try buildWorkerEnv(a, std.testing.io, std.testing.environ, shim, stub_agent, stub_git);
    defer env.deinit(a, std.testing.io);

    // planar-agent resolves.
    const probe_agent = try std.process.run(a, std.testing.io, .{
        .argv = &.{ "/bin/sh", "-c", "command -v planar-agent" },
        .environ_map = &env.env_map,
    });
    defer a.free(probe_agent.stdout);
    defer a.free(probe_agent.stderr);
    try std.testing.expect(probe_agent.term == .exited and probe_agent.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, probe_agent.stdout, "planar-agent") != null);

    // git resolves.
    const probe_git = try std.process.run(a, std.testing.io, .{
        .argv = &.{ "/bin/sh", "-c", "command -v git" },
        .environ_map = &env.env_map,
    });
    defer a.free(probe_git.stdout);
    defer a.free(probe_git.stderr);
    try std.testing.expect(probe_git.term == .exited and probe_git.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, probe_git.stdout, "git") != null);
}

test "negative: other planar-family binaries (planar-watch, planar-doc, planar-execute) are unreachable" {
    const a = std.testing.allocator;
    if (!shellAvailable(a)) return error.SkipZigTest;

    const root = mkTmpDir(a);
    defer a.free(root);
    defer rmTree(a, root);

    const bin_dir = try std.fmt.allocPrint(a, "{s}/realbin", .{root});
    defer a.free(bin_dir);
    std.Io.Dir.cwd().createDirPath(std.testing.io, bin_dir) catch @panic("mkdir realbin");

    // Co-locate every planar-family binary in one dir — the operator-default
    // install layout.
    const names = [_][]const u8{ "planar", "planar-agent", "planar-watch", "planar-doc", "planar-execute", "git" };
    var paths = std.ArrayList([]const u8).empty;
    defer {
        for (paths.items) |p| a.free(p);
        paths.deinit(a);
    }
    for (names) |n| {
        const p = try std.fmt.allocPrint(a, "{s}/{s}", .{ bin_dir, n });
        paths.append(a, p) catch @panic("OOM");
        writeStubBinary(a, p);
    }

    const stub_agent = paths.items[1];
    const stub_git = paths.items[5];

    const shim = try std.fmt.allocPrint(a, "{s}/shim", .{root});
    defer a.free(shim);

    var env = try buildWorkerEnv(a, std.testing.io, std.testing.environ, shim, stub_agent, stub_git);
    defer env.deinit(a, std.testing.io);

    // Each silently-denied name must fail to resolve.
    for ([_][]const u8{ "planar-watch", "planar-doc", "planar-execute" }) |denied| {
        const cmd = try std.fmt.allocPrint(a, "command -v {s}", .{denied});
        defer a.free(cmd);
        const probe = try std.process.run(a, std.testing.io, .{
            .argv = &.{ "/bin/sh", "-c", cmd },
            .environ_map = &env.env_map,
        });
        defer a.free(probe.stdout);
        defer a.free(probe.stderr);
        const ok = probe.term == .exited and probe.term.exited == 0;
        if (ok) {
            std.debug.print("\nbinary {s} unexpectedly resolved on constrained PATH; stdout: {s}\n", .{ denied, probe.stdout });
        }
        try std.testing.expect(!ok);
    }
}
