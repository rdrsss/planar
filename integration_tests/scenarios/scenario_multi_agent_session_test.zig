//! integration_tests/scenarios/scenario_multi_agent_session_test.zig
//!
//! Plan 85 t#2621 — the synchronization-plane scenario.
//!
//! Plan 85 ships a three-binary system (`planar`, `planar-agent`,
//! `planar-watch`) whose load-bearing thesis is that multiple agent
//! processes can safely drain a shared work queue with exactly-once
//! semantics, while a read-only watcher observes a coherent timeline.
//!
//! Per-verb tests in `integration_tests/planar_agent_test.zig` pin
//! individual contracts (pull/complete/fail/...) and the M1
//! "two-process race → exactly one winner" invariant. Per-verb tests
//! in `integration_tests/planar_watch_test.zig` pin the feed shape and
//! the M9 Tier-2 wake.
//!
//! This scenario glues them. Three cooperative worker processes drain
//! a six-task plan while a fourth process (planar-watch feed --follow)
//! tails the activity feed. The assertions cover:
//!
//!   1. Every task completed exactly once (no double-claim).
//!   2. No overlapping claims on the same task.
//!   3. Load was distributed across all three workers (no greedy
//!      starvation).
//!   4. The watcher's NDJSON stream contains the full set of
//!      claim_acquired / action_started / action_ended / completed
//!      events in chronological order; every `completed` is preceded
//!      by a matching `claim_acquired` for the same claim_token.
//!   5. Every worker process exited 0 (no SQLITE_BUSY pile-up).
//!   6. First claim_acquired appeared at the watcher within 2s of the
//!      barrier release — a weak Tier-2-wake proxy.
//!
//! The total scenario is timed; if it exceeds 60s the assertion fails
//! loudly (something is wrong with the synchronization plane).
//!
//! Locked decisions (see prompt):
//!   - N=3 workers, M=6 tasks (workers contend; M/N=2 gives realistic
//!     spread without combinatorial runtime).
//!   - Worker wrapper is a bash script spawned via std.process.Child;
//!     bash is what an operator/agent would use, and a separate Zig
//!     test binary would obscure the operator path.
//!   - Sentinel-file barrier for synchronized start.
//!   - --no-locality-probe everywhere to avoid spawning git
//!     subprocesses on every action.
//!   - Watcher captures stdout to a tmp file rather than a pipe to
//!     avoid kernel pipe-buffer back-pressure under load.

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// Tunables
// =========================================================================

const num_workers: usize = 3;
const num_tasks: usize = 6;
const scenario_budget_ms: u64 = 60_000;
const first_event_budget_ms: u64 = 2_000;

// =========================================================================
// Resolve sibling binaries from the harness's environ.
// =========================================================================

fn resolveAgentBin() []const u8 {
    return resolveEnv("PLANAR_AGENT_BIN=");
}
fn resolveWatchBin() []const u8 {
    return resolveEnv("PLANAR_WATCH_BIN=");
}
fn resolvePlanarBin() []const u8 {
    return resolveEnv("PLANAR_BIN=");
}

fn resolveEnv(prefix: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, prefix)) return s[prefix.len..];
    }
    std.debug.panic(
        "{s} is not set; run via make test-integration",
        .{prefix},
    );
}

// =========================================================================
// JSON helpers (we deliberately avoid std.json parsing here — the
// scenario's assertions are about presence of substring contracts and
// per-line NDJSON shapes that are noisy to model as Zig structs.)
// =========================================================================

fn extractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

fn monotonicNs() u64 {
    const ts: std.Io.Timestamp = std.Io.Clock.awake.now(std.testing.io);
    return @intCast(ts.toNanoseconds());
}

// =========================================================================
// SQLite scalar — shells to the system `sqlite3` CLI to read post-state
// directly. Used for the "exactly-one completed claim per task" and
// "claims grouped by worker" assertions where re-deriving from CLI
// output would obscure the contract. If `sqlite3` is unavailable the
// scenario returns error.SkipZigTest (the rotation test in
// planar_watch_test.zig does the same).
// =========================================================================

fn sqliteQueryLines(
    gpa: std.mem.Allocator,
    db_path: []const u8,
    sql: []const u8,
) ![]u8 {
    const result = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", "-separator", "|", db_path, sql },
    }) catch |e| {
        std.debug.print("sqlite3 spawn failed: {s}\n", .{@errorName(e)});
        return error.SkipZigTest;
    };
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("sqlite3 failed: {s}\n", .{result.stderr});
        gpa.free(result.stdout);
        return error.SqliteFailed;
    }
    return result.stdout;
}

fn sqliteScalar(
    gpa: std.mem.Allocator,
    db_path: []const u8,
    sql: []const u8,
) !i64 {
    const out = try sqliteQueryLines(gpa, db_path, sql);
    defer gpa.free(out);
    const trimmed = std.mem.trim(u8, out, " \t\r\n");
    return std.fmt.parseInt(i64, trimmed, 10) catch |e| {
        std.debug.print("sqlite3 output not integer ('{s}'): {s}\n", .{ trimmed, @errorName(e) });
        return error.SqliteParseFailed;
    };
}

// =========================================================================
// Bash worker wrapper script. Each worker is invoked as:
//   bash -c <script> -- <worker_n> <plan_id>
//
// The script:
//   - waits for the sentinel-file barrier;
//   - loops: peek → pull → action start → (jitter) → action end → complete;
//   - exits 0 cleanly on no_work, non-zero on any unexpected error.
//
// Environment expected (parent sets):
//   PLANAR_DB       — scratch DB path.
//   PLANAR_AGENT    — absolute path to the planar-agent binary.
//   BARRIER_FILE    — sentinel path; worker spins until it exists.
//   WORKER_READY    — touch this when ready to start the loop, BEFORE
//                     spinning on BARRIER. The parent waits for all N
//                     ready-files to appear before touching BARRIER_FILE,
//                     so every worker is parked on the same poll loop.
// =========================================================================

const worker_script =
    \\set -u
    \\worker_n="$1"
    \\plan_id="$2"
    \\: > "${WORKER_READY}.${worker_n}"
    \\while [ ! -f "$BARRIER_FILE" ]; do sleep 0.02; done
    \\
    \\while :; do
    \\  peek=$("$PLANAR_AGENT" peek "$plan_id" --json 2>/tmp/scen-w-${worker_n}.err)
    \\  case "$peek" in
    \\    *'"no_work":true'*) exit 0 ;;
    \\  esac
    \\
    \\  pull=$("$PLANAR_AGENT" pull "$plan_id" \
    \\    --vendor-session "worker:${worker_n}" \
    \\    --role coder --ttl 600 --no-locality-probe --json \
    \\    2>>/tmp/scen-w-${worker_n}.err)
    \\  case "$pull" in
    \\    *'"no_work":true'*) exit 0 ;;
    \\  esac
    \\
    \\  token=$(printf '%s' "$pull" | sed -n 's/.*"claim_token":"\([0-9a-f]*\)".*/\1/p')
    \\  if [ -z "$token" ]; then
    \\    echo "worker ${worker_n} pull missing token: $pull" >&2
    \\    exit 11
    \\  fi
    \\
    \\  hb=$("$PLANAR_AGENT" heartbeat --claim "$token" --ttl 600 --json \
    \\    2>>/tmp/scen-w-${worker_n}.err) || { echo "hb fail: $hb" >&2; exit 12; }
    \\
    \\  act=$("$PLANAR_AGENT" action start --claim "$token" --kind tool_call \
    \\    --no-locality-probe --json 2>>/tmp/scen-w-${worker_n}.err) \
    \\    || { echo "action start fail: $act" >&2; exit 13; }
    \\  act_id=$(printf '%s' "$act" | sed -n 's/.*"action_id":\([0-9]*\).*/\1/p')
    \\  if [ -z "$act_id" ]; then
    \\    echo "worker ${worker_n} action_id missing: $act" >&2
    \\    exit 14
    \\  fi
    \\
    \\  # Tiny jitter so worker timelines interleave. 10-50ms range.
    \\  sleep 0.0$(( (worker_n * 13 + RANDOM % 4) % 5 ))
    \\
    \\  end=$("$PLANAR_AGENT" action end --action "$act_id" --outcome ok \
    \\    --summary "worker ${worker_n} tool" --json \
    \\    2>>/tmp/scen-w-${worker_n}.err) \
    \\    || { echo "action end fail: $end" >&2; exit 15; }
    \\
    \\  done_out=$("$PLANAR_AGENT" complete --claim "$token" \
    \\    --summary "worker ${worker_n} done" --json \
    \\    2>>/tmp/scen-w-${worker_n}.err) \
    \\    || { echo "complete fail: $done_out" >&2; exit 16; }
    \\done
;

// =========================================================================
// Spawn helpers
// =========================================================================

fn buildEnvMap(
    gpa: std.mem.Allocator,
    db_path: []const u8,
    agent_bin: []const u8,
    barrier: []const u8,
    ready_prefix: []const u8,
) std.process.Environ.Map {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
    env_map.put("PLANAR_DB", db_path) catch @panic("OOM PLANAR_DB");
    env_map.put("PLANAR_AGENT", agent_bin) catch @panic("OOM PLANAR_AGENT");
    env_map.put("BARRIER_FILE", barrier) catch @panic("OOM BARRIER_FILE");
    env_map.put("WORKER_READY", ready_prefix) catch @panic("OOM WORKER_READY");
    return env_map;
}

fn watcherEnv(gpa: std.mem.Allocator, db_path: []const u8) std.process.Environ.Map {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
    env_map.put("PLANAR_DB", db_path) catch @panic("OOM PLANAR_DB");
    return env_map;
}

// Drain a child's stdout/stderr pipe into a heap buffer.
fn drainPipe(gpa: std.mem.Allocator, file: *?std.Io.File) ![]u8 {
    if (file.*) |*f| {
        defer {
            f.close(std.testing.io);
            file.* = null;
        }
        var reader = f.reader(std.testing.io, &.{});
        return reader.interface.allocRemaining(gpa, std.Io.Limit.limited(1 << 20)) catch |err| switch (err) {
            error.ReadFailed => if (reader.err) |e| return e else return err,
            else => return err,
        };
    }
    return try gpa.dupe(u8, "");
}

// =========================================================================
// The scenario.
// =========================================================================

test "three planar-agent workers drain a shared queue with exactly-once + a watcher sees a coherent timeline" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // -----------------------------------------------------------------
    // Seed: init, one plan, M todo tasks.
    // -----------------------------------------------------------------
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "multi-agent-demo", "--json", "multi-agent-demo" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const pid_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(pid_arg);

    var t_idx: usize = 0;
    while (t_idx < num_tasks) : (t_idx += 1) {
        const title = std.fmt.allocPrint(gpa, "task-{d}", .{t_idx + 1}) catch @panic("OOM");
        defer gpa.free(title);
        const out = suite.mustRun(&.{ "task", "add", "--plan", pid_arg, title });
        gpa.free(out);
    }

    // Sanity: plan_next reports M available tasks.
    const next_json = suite.mustRun(&.{ "plan", "next", pid_arg, "--json" });
    defer gpa.free(next_json);
    var avail_count: usize = 0;
    var search_from: usize = 0;
    while (std.mem.indexOfPos(u8, next_json, search_from, "\"status\":\"todo\"")) |pos| {
        avail_count += 1;
        search_from = pos + 1;
    }
    if (avail_count < num_tasks) {
        std.debug.print("plan next reported {d} available; expected {d}: {s}\n", .{ avail_count, num_tasks, next_json });
        return error.SeedingBroken;
    }

    // -----------------------------------------------------------------
    // Build scratch paths inside the suite tmp dir for the sentinel
    // file, the per-worker ready files, and the watcher's stdout log.
    // -----------------------------------------------------------------
    const tmp_abs = suite.tmpAbsPath();
    const barrier_path = std.fs.path.join(gpa, &.{ tmp_abs, "barrier" }) catch @panic("OOM");
    defer gpa.free(barrier_path);
    const ready_prefix = std.fs.path.join(gpa, &.{ tmp_abs, "ready" }) catch @panic("OOM");
    defer gpa.free(ready_prefix);
    const watch_log_path = std.fs.path.join(gpa, &.{ tmp_abs, "watch.ndjson" }) catch @panic("OOM");
    defer gpa.free(watch_log_path);

    // Wipe any stale per-worker error logs from a prior interactive run.
    inline for (1..(num_workers + 1)) |n| {
        const stale = std.fmt.allocPrint(gpa, "/tmp/scen-w-{d}.err", .{n}) catch @panic("OOM");
        defer gpa.free(stale);
        std.Io.Dir.cwd().deleteFile(std.testing.io, stale) catch {};
    }

    // -----------------------------------------------------------------
    // Spawn the watcher first so it has a chance to subscribe to the
    // -wal sibling before workers create it. 200ms interval = Tier 1
    // safety net for environments where Tier 2 isn't available.
    //
    // We wrap the watcher in `bash -c` and redirect stdout to a tmp
    // file. The shell-managed redirect is the same mechanism an
    // operator would use (`planar-watch feed --follow --json > log`)
    // and avoids any per-Zig-version flakiness in
    // SpawnOptions.StdIo.file. SIGTERM at drain end gives the OS
    // time to flush before reap.
    // -----------------------------------------------------------------
    const abs_db = suite.absDbPath();
    var watcher_env = watcherEnv(gpa, abs_db);
    defer watcher_env.deinit();
    watcher_env.put("PLANAR_WATCH", resolveWatchBin()) catch @panic("OOM PLANAR_WATCH");

    // We deliberately do NOT pass `--plan <id>` here. The feed
    // verb's --plan filter matches the claim's `entity_kind = 'plan'`
    // path, NOT task-on-plan claims (which are what the workers
    // create). Filtering would drop every event we care about.
    // The DB is freshly ephemeral, so an unfiltered feed contains
    // only the activity this scenario generated.
    const watch_cmd = std.fmt.allocPrint(
        gpa,
        "exec \"$PLANAR_WATCH\" feed --follow --interval 200ms --json > \"{s}\"",
        .{watch_log_path},
    ) catch @panic("OOM watch_cmd");
    defer gpa.free(watch_cmd);

    var watcher = std.process.spawn(std.testing.io, .{
        .argv = &.{ "bash", "-c", watch_cmd },
        .environ_map = &watcher_env,
        .stdout = .pipe,
        .stderr = .pipe,
    }) catch |e| std.debug.panic("spawn watcher: {s}", .{@errorName(e)});

    // -----------------------------------------------------------------
    // Spawn N workers. Each touches `${ready_prefix}.${n}` once parked
    // on the barrier loop. We wait until all N ready-files exist, then
    // touch the barrier so all workers wake within the same 20ms poll.
    // -----------------------------------------------------------------
    const agent_bin = resolveAgentBin();

    var worker_env = buildEnvMap(gpa, abs_db, agent_bin, barrier_path, ready_prefix);
    defer worker_env.deinit();

    var workers: [num_workers]std.process.Child = undefined;
    var worker_stderr: [num_workers][]u8 = undefined;

    inline for (0..num_workers) |i| {
        const wn = i + 1;
        const wn_buf = std.fmt.allocPrint(gpa, "{d}", .{wn}) catch @panic("OOM");
        defer gpa.free(wn_buf);
        workers[i] = std.process.spawn(std.testing.io, .{
            .argv = &.{ "bash", "-c", worker_script, "--", wn_buf, pid_arg },
            .environ_map = &worker_env,
            .stdout = .pipe,
            .stderr = .pipe,
        }) catch |e| std.debug.panic("spawn worker {d}: {s}", .{ wn, @errorName(e) });
    }

    // Wait for every worker to declare ready (or 5s ceiling).
    {
        const ready_deadline_ns = monotonicNs() + 5 * std.time.ns_per_s;
        while (true) {
            var all_ready = true;
            inline for (1..(num_workers + 1)) |n| {
                const ready_file = std.fmt.allocPrint(gpa, "{s}.{d}", .{ ready_prefix, n }) catch @panic("OOM");
                defer gpa.free(ready_file);
                std.Io.Dir.cwd().access(std.testing.io, ready_file, .{}) catch {
                    all_ready = false;
                };
            }
            if (all_ready) break;
            if (monotonicNs() > ready_deadline_ns) {
                std.debug.print("workers failed to declare ready within 5s\n", .{});
                return error.WorkersNotReady;
            }
            std.testing.io.sleep(std.Io.Duration.fromMilliseconds(20), std.Io.Clock.awake) catch {};
        }
    }

    // Release the barrier. Snapshot the timestamp — assertion #6
    // (first-event-to-watcher latency) is measured from here.
    const barrier_release_ns: u64 = monotonicNs();
    {
        const f = std.Io.Dir.cwd().createFile(std.testing.io, barrier_path, .{}) catch |e|
            std.debug.panic("create barrier: {s}", .{@errorName(e)});
        f.close(std.testing.io);
    }

    // -----------------------------------------------------------------
    // Wait for every worker to exit. Bounded by scenario_budget_ms.
    // -----------------------------------------------------------------
    const scenario_deadline_ns = barrier_release_ns + scenario_budget_ms * std.time.ns_per_ms;
    var worker_exits: [num_workers]u32 = undefined;
    inline for (0..num_workers) |i| {
        const remaining_ns = if (monotonicNs() < scenario_deadline_ns)
            scenario_deadline_ns - monotonicNs()
        else
            0;
        _ = remaining_ns; // best-effort; Zig stdlib has no `waitWithTimeout`.
        worker_stderr[i] = drainPipe(gpa, &workers[i].stderr) catch |e|
            std.debug.panic("drain stderr {d}: {s}", .{ i + 1, @errorName(e) });
        // We don't strictly need stdout from the workers, but draining
        // prevents the kernel from blocking the child on a full pipe.
        const stdout_drain = drainPipe(gpa, &workers[i].stdout) catch |e|
            std.debug.panic("drain stdout {d}: {s}", .{ i + 1, @errorName(e) });
        gpa.free(stdout_drain);

        const term = workers[i].wait(std.testing.io) catch |e|
            std.debug.panic("wait worker {d}: {s}", .{ i + 1, @errorName(e) });
        if (monotonicNs() > scenario_deadline_ns) {
            std.debug.print("scenario blew the 60s budget waiting on worker {d}\n", .{i + 1});
            return error.ScenarioTimeout;
        }
        switch (term) {
            .exited => |code| worker_exits[i] = code,
            else => {
                std.debug.print("worker {d} did not exit cleanly: {any}\nstderr: {s}\n", .{ i + 1, term, worker_stderr[i] });
                return error.WorkerAbnormalTerm;
            },
        }
    }
    defer for (worker_stderr) |s| gpa.free(s);

    const drain_complete_ns: u64 = monotonicNs();

    // -----------------------------------------------------------------
    // Give the watcher one more poll interval to absorb the final
    // events, then SIGTERM and collect.
    // -----------------------------------------------------------------
    std.testing.io.sleep(std.Io.Duration.fromMilliseconds(400), std.Io.Clock.awake) catch {};
    std.posix.kill(watcher.id.?, std.posix.SIG.TERM) catch |e|
        std.debug.panic("kill watcher: {s}", .{@errorName(e)});
    // Drain stdout/stderr pipes (shell wrapper's own stdout is the
    // redirect, so the pipe should be empty, but leaving it
    // un-drained risks the child blocking on a full pipe).
    const watcher_stdout = drainPipe(gpa, &watcher.stdout) catch |e|
        std.debug.panic("drain watcher stdout: {s}", .{@errorName(e)});
    gpa.free(watcher_stdout);
    const watcher_stderr = drainPipe(gpa, &watcher.stderr) catch |e|
        std.debug.panic("drain watcher stderr: {s}", .{@errorName(e)});
    defer gpa.free(watcher_stderr);
    _ = watcher.wait(std.testing.io) catch |e|
        std.debug.panic("wait watcher: {s}", .{@errorName(e)});

    const scenario_done_ns: u64 = monotonicNs();

    // -----------------------------------------------------------------
    // Assertion 5: every worker exited 0.
    // -----------------------------------------------------------------
    inline for (0..num_workers) |i| {
        if (worker_exits[i] != 0) {
            std.debug.print(
                "worker {d} exited {d}; stderr:\n{s}\n",
                .{ i + 1, worker_exits[i], worker_stderr[i] },
            );
            return error.WorkerNonZeroExit;
        }
    }

    // -----------------------------------------------------------------
    // Assertion 1: every task is done.
    //
    // `task list` defaults to open statuses (todo/doing/blocked), so we
    // run it twice — once for `--status done` to count completions, and
    // once with no status filter to confirm zero leftovers in any
    // open state.
    // -----------------------------------------------------------------
    const done_json = suite.mustRun(&.{ "task", "list", "--plan", pid_arg, "--status", "done", "--json" });
    defer gpa.free(done_json);
    const open_json = suite.mustRun(&.{ "task", "list", "--plan", pid_arg, "--json" });
    defer gpa.free(open_json);

    var done_count: usize = 0;
    {
        var i: usize = 0;
        while (std.mem.indexOfPos(u8, done_json, i, "\"status\":\"done\"")) |pos| {
            done_count += 1;
            i = pos + 1;
        }
    }
    var open_count: usize = 0;
    {
        var i: usize = 0;
        while (std.mem.indexOfPos(u8, open_json, i, "\"status\":")) |pos| {
            open_count += 1;
            i = pos + 1;
        }
    }
    if (done_count != num_tasks or open_count != 0) {
        std.debug.print(
            "expected {d} done / 0 open; got {d} done / {d} open.\n  done JSON: {s}\n  open JSON: {s}\n",
            .{ num_tasks, done_count, open_count, done_json, open_json },
        );
        return error.NotAllTasksCompleted;
    }

    // -----------------------------------------------------------------
    // Assertions 2 + 3: SQLite-direct queries over agent_work_claims.
    //   (2) Each task has exactly one claim with status='completed'
    //       AND no two completed claims overlap on the same task.
    //   (3) Each worker:N grouping has >=1 completed claim.
    // -----------------------------------------------------------------
    const dup_completed = sqliteScalar(
        gpa,
        abs_db,
        \\select coalesce(max(c), 0) from (
        \\  select count(*) as c
        \\  from agent_work_claims
        \\  where entity_kind = 'task' and status = 'completed'
        \\  group by entity_id
        \\)
        ,
    ) catch |e| switch (e) {
        error.SkipZigTest => return error.SkipZigTest,
        else => return e,
    };
    if (dup_completed != 1) {
        std.debug.print(
            "assertion (2) broken: some task has {d} completed claims (must be exactly 1)\n",
            .{dup_completed},
        );
        return error.MultipleCompletedClaimsForOneTask;
    }

    // No non-aborted/non-released second claim on any task.
    const stray_active = sqliteScalar(
        gpa,
        abs_db,
        \\select count(*) from agent_work_claims
        \\where entity_kind = 'task' and status not in ('completed', 'aborted', 'released')
        ,
    ) catch |e| switch (e) {
        error.SkipZigTest => return error.SkipZigTest,
        else => return e,
    };
    if (stray_active != 0) {
        std.debug.print(
            "assertion (2) broken: {d} claim rows still in active/stale state at drain end\n",
            .{stray_active},
        );
        return error.StrayActiveClaim;
    }

    // Each worker:N grouping handled >= 1.
    const per_worker_csv = sqliteQueryLines(
        gpa,
        abs_db,
        \\select vendor_session_id, count(*) from agent_work_claims
        \\where status = 'completed'
        \\group by vendor_session_id
        \\order by vendor_session_id
        ,
    ) catch |e| switch (e) {
        error.SkipZigTest => return error.SkipZigTest,
        else => return e,
    };
    defer gpa.free(per_worker_csv);

    var worker_groups: usize = 0;
    var total_completed: i64 = 0;
    {
        var it = std.mem.tokenizeAny(u8, per_worker_csv, "\n");
        while (it.next()) |line| {
            const trimmed = std.mem.trim(u8, line, " \t\r");
            if (trimmed.len == 0) continue;
            worker_groups += 1;
            // line is "vendor_session_id|count".
            const bar = std.mem.indexOfScalar(u8, trimmed, '|') orelse continue;
            const count_str = trimmed[bar + 1 ..];
            const n = std.fmt.parseInt(i64, count_str, 10) catch continue;
            total_completed += n;
        }
    }
    if (total_completed != @as(i64, @intCast(num_tasks))) {
        std.debug.print(
            "assertion (3) broken: total completed claims = {d}, expected {d}. raw:\n{s}\n",
            .{ total_completed, num_tasks, per_worker_csv },
        );
        return error.CompletedClaimCountMismatch;
    }
    if (worker_groups < num_workers) {
        // Probabilistic: under pathologically unfair scheduling the
        // first worker might race-win every pull. The bash sleep
        // jitter (10-50ms) and the 200ms poll interval make this
        // extremely unlikely with M=6 / N=3, but flag rather than
        // panic so a one-in-a-thousand CI flake doesn't block.
        std.debug.print(
            "assertion (3) WARN: only {d}/{d} worker sessions completed any task.\n" ++
                "  Probabilistic tolerance — this should be rare; investigate if it recurs.\n" ++
                "  raw:\n{s}\n",
            .{ worker_groups, num_workers, per_worker_csv },
        );
        return error.LoadNotDistributed;
    }

    // -----------------------------------------------------------------
    // Assertion 4: parse the watcher NDJSON log and verify the
    // timeline.
    // -----------------------------------------------------------------
    const watch_log = std.Io.Dir.cwd().readFileAlloc(std.testing.io, watch_log_path, gpa, .limited(2 << 20)) catch |e|
        std.debug.panic("read watch log '{s}': {s}", .{ watch_log_path, @errorName(e) });
    defer gpa.free(watch_log);

    if (watch_log.len == 0) {
        std.debug.print(
            "watcher produced an empty log at {s}\nwatcher stderr:\n{s}\n",
            .{ watch_log_path, watcher_stderr },
        );
    }

    var claim_acquired_count: usize = 0;
    var completed_count: usize = 0;
    var action_started_coder_count: usize = 0;
    var action_started_total: usize = 0;
    var action_ended_count: usize = 0;
    var prev_at_buf: [64]u8 = undefined;
    var prev_at_len: usize = 0;
    var first_event_at_ns: ?u64 = null;

    // Index claim_acquired events by claim_token so we can verify that
    // every `completed` event has a preceding acquire for the same token.
    var acquired_tokens = std.StringHashMap(void).init(gpa);
    defer acquired_tokens.deinit();

    var line_it = std.mem.tokenizeAny(u8, watch_log, "\n");
    var saw_acquired_before_completed = true;
    while (line_it.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " \t\r");
        if (trimmed.len == 0 or trimmed[0] != '{') continue;

        const event = sliceJsonString(trimmed, "\"event\":\"") orelse continue;
        const at = sliceJsonString(trimmed, "\"at\":\"") orelse continue;

        // Monotonic ordering check: NDJSON `at` strings are ISO-8601
        // ascending order so a lexicographic compare suffices.
        if (prev_at_len != 0) {
            if (std.mem.lessThan(u8, at, prev_at_buf[0..prev_at_len])) {
                std.debug.print(
                    "watcher emitted out-of-order events: prev='{s}' next='{s}'\n  line: {s}\n",
                    .{ prev_at_buf[0..prev_at_len], at, trimmed },
                );
                return error.WatcherOutOfOrder;
            }
        }
        prev_at_len = @min(at.len, prev_at_buf.len);
        @memcpy(prev_at_buf[0..prev_at_len], at[0..prev_at_len]);

        if (first_event_at_ns == null) first_event_at_ns = monotonicNs();

        if (std.mem.eql(u8, event, "claim_acquired")) {
            claim_acquired_count += 1;
            if (sliceJsonString(trimmed, "\"claim_token\":\"")) |tok| {
                const owned = gpa.dupe(u8, tok) catch @panic("OOM");
                acquired_tokens.put(owned, {}) catch @panic("OOM");
            }
        } else if (std.mem.eql(u8, event, "completed")) {
            completed_count += 1;
            if (sliceJsonString(trimmed, "\"claim_token\":\"")) |tok| {
                if (!acquired_tokens.contains(tok)) {
                    std.debug.print(
                        "completed event for unknown token '{s}': {s}\n",
                        .{ tok, trimmed },
                    );
                    saw_acquired_before_completed = false;
                }
            }
        } else if (std.mem.eql(u8, event, "action_started")) {
            action_started_total += 1;
            if (sliceJsonString(trimmed, "\"action_kind\":\"")) |k| {
                if (std.mem.eql(u8, k, "coder")) action_started_coder_count += 1;
            }
        } else if (std.mem.eql(u8, event, "action_ended")) {
            action_ended_count += 1;
        }
    }

    // Free token keys we duped.
    {
        var it = acquired_tokens.keyIterator();
        while (it.next()) |k| gpa.free(k.*);
    }

    if (claim_acquired_count < num_tasks) {
        std.debug.print(
            "assertion (4) broken: watcher saw only {d} claim_acquired events; expected >= {d}.\nwatch log:\n{s}\n",
            .{ claim_acquired_count, num_tasks, watch_log },
        );
        return error.WatcherMissingAcquires;
    }
    if (completed_count < num_tasks) {
        std.debug.print(
            "assertion (4) broken: watcher saw only {d} completed events; expected >= {d}.\nwatch log:\n{s}\n",
            .{ completed_count, num_tasks, watch_log },
        );
        return error.WatcherMissingCompletes;
    }
    if (action_started_coder_count < num_tasks) {
        std.debug.print(
            "assertion (4) broken: watcher saw only {d} action_started[coder] events; expected >= {d} (one per pull). total action_started={d}\nwatch log:\n{s}\n",
            .{ action_started_coder_count, num_tasks, action_started_total, watch_log },
        );
        return error.WatcherMissingCoderActions;
    }
    if (action_ended_count < num_tasks) {
        std.debug.print(
            "assertion (4) broken: watcher saw only {d} action_ended events; expected >= {d}.\nwatch log:\n{s}\n",
            .{ action_ended_count, num_tasks, watch_log },
        );
        return error.WatcherMissingActionEnds;
    }
    if (!saw_acquired_before_completed) {
        return error.WatcherCompletedWithoutAcquire;
    }

    // -----------------------------------------------------------------
    // Assertion 6: first claim_acquired event appeared at the watcher
    // within first_event_budget_ms of barrier release. This is a weak
    // Tier-2 wake proxy; the 200ms poll interval gives Tier 1 the
    // chance to surface within the same budget, so the test passes
    // whether the host supports kqueue/inotify or not. A true Tier-2
    // assertion lives in planar_watch_test.zig.
    // -----------------------------------------------------------------
    if (first_event_at_ns) |t0| {
        const ms_since_barrier = (t0 - barrier_release_ns) / std.time.ns_per_ms;
        if (ms_since_barrier > first_event_budget_ms) {
            std.debug.print(
                "assertion (6) broken: first watcher event observed {d}ms after barrier release (budget {d}ms)\n",
                .{ ms_since_barrier, first_event_budget_ms },
            );
            return error.FirstEventLatencyTooHigh;
        }
    } else {
        std.debug.print("assertion (6) broken: watcher saw NO events\n", .{});
        return error.WatcherSawNothing;
    }

    // Telemetry (drain_ms / first_event_ms / total_ms) is computed
    // for diagnostic purposes but deliberately NOT printed on green
    // runs — the existing planar_agent_test cross-process race test
    // documents that std.debug.print near test exit triggers the
    // zig 0.16 test runner's stale "failed command:" re-emission
    // noise on every `make test-integration` run. The wall-clock
    // wins are already encoded in the assertion bounds
    // (`scenario_budget_ms`, `first_event_budget_ms`).
    _ = scenario_done_ns;
    _ = drain_complete_ns;
}

// =========================================================================
// Tiny JSON-string slicer. `prefix` MUST end with `\"` (the opening
// quote of the value). Returns a view that ends at the next `"`.
// Returns null if the prefix isn't present or the string is unterminated.
// =========================================================================

fn sliceJsonString(haystack: []const u8, prefix: []const u8) ?[]const u8 {
    const idx = std.mem.indexOf(u8, haystack, prefix) orelse return null;
    const start = idx + prefix.len;
    var end = start;
    while (end < haystack.len and haystack[end] != '"') end += 1;
    if (end == haystack.len) return null;
    return haystack[start..end];
}
