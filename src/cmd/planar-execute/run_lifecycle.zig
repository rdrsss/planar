//! run_lifecycle.zig — `planar-execute` run-row bracketing (plan 585 task 3922).
//!
//! ## What this is
//!
//! `planar-execute` holds NO DB handle (decision 444; capability boundary). To
//! open and close a `workflow_runs` row around each orchestrator run it SHELLS
//! `planar-agent run start` / `planar-agent run end` — the same subprocess
//! pattern used by `terminal.zig` (terminal verbs), `heartbeat.zig` (lease
//! refresh), and the reconcile path.
//!
//! ## The run-row contract (plan 585, Q597)
//!
//!   - `run start` inserts a `workflow_runs` row in `running` status. It
//!     requires `--plan`, `--workflow`, `--run-id`, `--pid`, `--repo-root`,
//!     and `--json`. The JSON output carries the DB row id (`run_id` field).
//!   - `run end --run-id <string> --status <completed|interrupted>` transitions
//!     the row to the given terminal status and stamps `ended_at`.
//!
//! ## Robustness contract
//!
//!   - A failure from EITHER verb is logged and silently swallowed — the run
//!     lifecycle row is AUDIT METADATA, not the control path. A missing or
//!     unclosed row does NOT prevent the workflow from running or cause
//!     correctness issues; `planar-agent reconcile` sweeps stale rows.
//!   - Dry-run creates NO run row (the caller gates on `dry_run` before
//!     calling `runStart`).
//!   - Live AND mock-worker modes DO call `runStart`/`runEnd` — the verbs are
//!     run-lifecycle control calls independent of worker spawning. This makes
//!     mock mode testable: an integration test can run `--mock-worker` and
//!     assert the `workflow_runs` row was created and closed.
//!   - A missing or zero `plan_id` yields no call (nothing to scope the row to).
//!
//! ## JSON parsing
//!
//! `runStart` parses `run start --json` output with `.alloc_always` per the
//! known UAF class in this binary (memory: json_parsed_alloc_always): strings
//! returned by `std.json.Parsed(T)` using `.alloc_if_needed` may dangle when
//! the source stdout buffer is freed. We parse into an owned arena and return
//! a `RunStartResult` that the caller frees with `deinit`.

const std = @import("std");
const Io = std.Io;

const log = std.log.scoped(.planar_execute_run_lifecycle);

/// Errors the run lifecycle surface can return. Callers treat these as
/// best-effort (log-and-continue); they are surfaced so tests can assert.
pub const RunLifecycleError = error{
    /// The subprocess spawn itself failed (OOM, binary not found, etc.).
    SubprocessFailed,
    /// `planar-agent run start` or `run end` exited non-zero.
    SubprocessNonZero,
    /// Allocator returned OOM while building argv or parsing JSON.
    OutOfMemory,
    /// `run start --json` output could not be parsed or lacked the `run_id`
    /// field. The run_identifier is still valid for `run end` even if the DB
    /// id is not available (use 0 as the sentinel in that case).
    ParseFailed,
};

/// The result of a successful `run start` call. Carries the
/// `run_identifier` (the string we passed — the harness's own run_id) so
/// `runEnd` can reference the same row, and the DB row integer `run_db_id`
/// so it can be threaded into journal records and banners.
pub const RunStartResult = struct {
    /// The string identifier we passed to `--run-id` (heap-owned by the
    /// caller's allocator — duped at call time). Threaded into journal
    /// records and the `[dispatch]` banner.
    run_identifier: []u8,
    /// The DB row id returned by `run start --json` (`run_id` field). Used
    /// as a carry-over label for operator-facing output. 0 when the JSON
    /// parse did not yield an integer.
    run_db_id: i64,
    allocator: std.mem.Allocator,

    /// deinit frees the heap-owned `run_identifier` string.
    pub fn deinit(self: *RunStartResult) void {
        self.allocator.free(self.run_identifier);
    }
};

/// StartArgs carries the inputs `runStart` needs to call `planar-agent run
/// start`. All string slices are borrowed for the duration of the call.
pub const StartArgs = struct {
    /// The numeric plan id (required — zero means "no call; return null").
    plan_id: u64,
    /// The workflow name (e.g. "isolated-sequential").
    workflow_name: []const u8,
    /// The run identifier string (run-<pid>-<nanos> from runlock). Unique
    /// per run on a host.
    run_identifier: []const u8,
    /// The PID of the harness process (for reconcile liveness probing).
    pid: i32,
    /// The absolute repo root the harness is driving.
    repo_root: []const u8,
};

/// runStart shells `planar-agent run start` and returns the opened run row.
///
/// Returns `null` when `args.plan_id` is zero (no call needed). On any other
/// failure the error is logged and `null` is returned — callers treat this as
/// best-effort and continue without a run row.
///
/// The returned `RunStartResult.run_identifier` is heap-owned; the caller
/// MUST call `res.deinit()` when done.
pub fn runStart(
    allocator: std.mem.Allocator,
    io: Io,
    args: StartArgs,
) ?RunStartResult {
    if (args.plan_id == 0) return null;

    const plan_str = std.fmt.allocPrint(allocator, "{d}", .{args.plan_id}) catch {
        log.warn("run start: OOM allocating plan_str; no run row opened", .{});
        return null;
    };
    defer allocator.free(plan_str);

    const pid_str = std.fmt.allocPrint(allocator, "{d}", .{args.pid}) catch {
        log.warn("run start: OOM allocating pid_str; no run row opened", .{});
        return null;
    };
    defer allocator.free(pid_str);

    const argv = [_][]const u8{
        "planar-agent",     "run",      "start",
        "--plan",           plan_str,   "--workflow",
        args.workflow_name, "--run-id", args.run_identifier,
        "--pid",            pid_str,    "--repo-root",
        args.repo_root,     "--json",
    };

    const result = std.process.run(allocator, io, .{
        .argv = &argv,
        .stdout_limit = Io.Limit.limited(4096),
        .stderr_limit = Io.Limit.limited(1024),
    }) catch |e| {
        log.warn("run start: subprocess spawn failed: {s}; no run row opened", .{@errorName(e)});
        return null;
    };
    defer allocator.free(result.stderr);
    defer allocator.free(result.stdout);

    if (result.term != .exited or result.term.exited != 0) {
        log.warn("run start: planar-agent exited non-zero (term={any}); no run row opened", .{result.term});
        return null;
    }

    // Parse the JSON output to extract `run_id` (the DB row integer).
    // Use .alloc_always per the known UAF class (stdout is freed above).
    const RunStartJson = struct {
        run_id: i64 = 0,
    };

    // We need a copy of stdout before the defer-free above fires, but we
    // used `defer` on stdout. Re-parse before the defer fires: we are still
    // in the scope where result.stdout is valid (the defers run at scope exit,
    // which is the end of the function — so result.stdout is valid here).
    var db_id: i64 = 0;
    {
        var arena = std.heap.ArenaAllocator.init(allocator);
        defer arena.deinit();
        const parsed = std.json.parseFromSlice(RunStartJson, arena.allocator(), result.stdout, .{
            .ignore_unknown_fields = true,
            .allocate = .alloc_always,
        }) catch {
            log.warn("run start: could not parse JSON output; run_db_id will be 0", .{});
            // run_identifier is still valid even if we can't get the DB id;
            // return early with run_db_id=0.
            const run_id_owned_fallback = allocator.dupe(u8, args.run_identifier) catch {
                log.warn("run start: OOM duping run_identifier after parse failure; no run row tracked", .{});
                return null;
            };
            return .{ .run_identifier = run_id_owned_fallback, .run_db_id = 0, .allocator = allocator };
        };
        db_id = parsed.value.run_id;
    }

    const run_id_owned = allocator.dupe(u8, args.run_identifier) catch {
        log.warn("run start: OOM duping run_identifier; no run row tracked", .{});
        return null;
    };

    return .{ .run_identifier = run_id_owned, .run_db_id = db_id, .allocator = allocator };
}

/// eagerReconcile shells `planar-agent reconcile` as a best-effort call.
///
/// Used by the stale-runlock takeover path (task 3928 Q597): when
/// `planar-execute` detects a dead prior run at startup it shells
/// `planar-agent reconcile` so the existing reconcile sweep marks the dead
/// run `abandoned` via the established `reconcileRuns` path.  This preserves
/// Q597 — `run end` NEVER writes `abandoned`; reconcile does.
///
/// Best-effort: any failure is logged and swallowed; a failure must NOT block
/// the subsequent `runStart`. The lazy `planar-agent reconcile` sweep at the
/// next startup is the backstop.
pub fn eagerReconcile(allocator: std.mem.Allocator, io: Io) void {
    const argv = [_][]const u8{ "planar-agent", "reconcile" };
    const result = std.process.run(allocator, io, .{
        .argv = &argv,
        .stdout_limit = Io.Limit.limited(512),
        .stderr_limit = Io.Limit.limited(512),
    }) catch |e| {
        log.warn("eager-reconcile: subprocess spawn failed: {s}; lazy sweep is the backstop", .{@errorName(e)});
        return;
    };
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        log.warn("eager-reconcile: planar-agent reconcile exited non-zero (term={any}); lazy sweep is the backstop", .{result.term});
    }
}

/// runEnd shells `planar-agent run end --run-id <id> --status <status>`.
///
/// `run_identifier` is the string passed to `run start --run-id`. `status`
/// must be "completed" or "interrupted". (`abandoned` is written only by
/// `planar-agent reconcile` — see Q597 and `eagerReconcile`.)
///
/// Best-effort: any failure is logged and swallowed. This function does NOT
/// return an error to the caller because a failed `run end` does not affect
/// workflow correctness — `planar-agent reconcile` is the backstop for
/// unclosed rows.
pub fn runEnd(
    allocator: std.mem.Allocator,
    io: Io,
    run_identifier: []const u8,
    status: []const u8,
) void {
    const argv = [_][]const u8{
        "planar-agent", "run",          "end",
        "--run-id",     run_identifier, "--status",
        status,
    };

    const result = std.process.run(allocator, io, .{
        .argv = &argv,
        .stdout_limit = Io.Limit.limited(256),
        .stderr_limit = Io.Limit.limited(512),
    }) catch |e| {
        log.warn("run end: subprocess spawn failed: {s}; run row left open (reconcile will close it)", .{@errorName(e)});
        return;
    };
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    if (result.term != .exited or result.term.exited != 0) {
        log.warn("run end: planar-agent exited non-zero (term={any}); run row may be left open", .{result.term});
    }
}

// ===========================================================================
// Unit tests
// ===========================================================================

const testing = std.testing;

test "run_lifecycle: runEnd builds correct argv (smoke)" {
    // runEnd is best-effort and shells planar-agent; in a unit-test context
    // there is no real `planar-agent` on the PATH, so runEnd will log a
    // warning and return without panicking. This test just ensures the
    // compilation is correct and the function does not panic.
    // The integration test in workflow_run_lifecycle_test.zig is the
    // full-coverage gate.
    const gpa = testing.allocator;
    // This will fail to find `planar-agent` and log a warning — that is
    // acceptable for a unit test. The important invariant is: it does NOT
    // panic and does NOT return an error to the caller.
    runEnd(gpa, testing.io, "run-test-1234", "completed");
    // If we reach here without panic the function satisfies its best-effort
    // no-crash contract.
}

test "run_lifecycle: runStart with zero plan_id returns null (no call)" {
    const gpa = testing.allocator;
    const result = runStart(gpa, testing.io, .{
        .plan_id = 0,
        .workflow_name = "test-wf",
        .run_identifier = "run-0-0",
        .pid = 1234,
        .repo_root = "/tmp",
    });
    // Zero plan_id → immediate null, no subprocess.
    try testing.expect(result == null);
}
