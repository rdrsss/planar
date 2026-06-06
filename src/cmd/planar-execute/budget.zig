//! budget.zig — budgets and ceilings (the hard kill-switch) for `planar-execute`
//! (plan 492 M8 task 3198 m8-budget-ceilings; tech-spec addendum 267 §5).
//!
//! ## Why this exists — the kill-switch
//!
//! Without a kill-switch the orchestrator's `fail → todo → respawn` loop is
//! UNBOUNDED: a task that keeps failing would be re-spawned forever, and a
//! runaway run could spawn unbounded workers or run for unbounded wall-clock.
//! These guards are what make an UNATTENDED run safe — they catch runaways,
//! they are NOT throughput tuning. The defaults are deliberately generous.
//!
//! ## Two halves
//!
//!   1. **Per-task max-attempt → block.** Before each spawn (at the SAME
//!      pre-spawn point as the task-3197 resume skip-when-done check), count
//!      this task's PRIOR FAILED attempts from the run journal. When the count
//!      reaches `max_attempts`, do NOT spawn — `block` the task instead (the
//!      roadmap says "→ block (not re-queue)") so the misbehaving task is set
//!      aside for operator triage and the workflow moves on.
//!
//!   2. **Whole-run ceiling → clean exit + journal terminus.** Track per-run a
//!      spawn counter + the run start time (monotonic, via the scheduler's
//!      injectable clock). Before each spawn, if `spawn_count >=
//!      max_total_spawns` OR `now - run_start >= max_wall_clock_ns` the run has
//!      hit its ceiling: write a journal TERMINUS record and initiate the clean
//!      interrupt shutdown (REUSE the task-3189 machinery). The run winds down
//!      cleanly (in-flight workers released, recoverable on the next resume) —
//!      it is NOT a crash.
//!
//! The per-worker USD cost cap remains intentionally DEFERRED / optional: it
//! requires parsing `total_cost_usd` from the worker's stream-json telemetry,
//! while the current 3190 plumbing treats stdout as liveness-only and keeps
//! task outcomes DB-sourced.
//!
//! ## Resume respects prior attempts — load-bearing
//!
//! The per-task fail-count comes from the PERSISTENT journal file, NOT an
//! in-memory counter. So a RESUMED run (`planar-execute run --plan <id>` after
//! an interrupted first run) reads the prior run's failed records and does NOT
//! reset the budget — three failures across two runs still trips a
//! max_attempts=3 budget. This is the whole reason the count is journal-derived.
//!
//! ## What counts as a "failed attempt"
//!
//! `failedAttemptCount` counts journal records whose `terminal_verb` is one of
//! the FAILED-ATTEMPT statuses (`failedAttemptStatus`):
//!   - `"failed"`    — the worker crashed / exited non-zero (or the harness
//!                     fail-fallback fired). Unambiguously a failed attempt.
//!   - `"timed-out"` — the per-worker wall-clock timeout (task 3188) killed a
//!                     hung worker. A hang is a failed attempt.
//!   - `"released"`  — a release is RETRYABLE (exit 0 + no commit = no-op, or a
//!                     SIGINT abandon-for-retry). A retryable outcome that keeps
//!                     re-occurring is exactly the runaway the budget must catch,
//!                     so `released` DOES count toward the attempt budget.
//! Records with `"completed"`, `"respected"`, `"blocked"`, or the `"ceiling"`
//! terminus do NOT count: `completed`/`respected` are successes, a `blocked`
//! task is already set aside (the resume skip handles it), and the terminus is
//! a run-level marker, not a task attempt.

const std = @import("std");

const journal = @import("journal.zig");

/// The run-ceiling sentinel that tripped. Distinct values so the terminus
/// record + the operator-facing exit message can name WHICH ceiling fired.
pub const Ceiling = enum {
    /// `spawn_count >= max_total_spawns` — too many workers spawned this run.
    spawns,
    /// `now - run_start >= max_wall_clock_ns` — the run ran too long.
    wall_clock,

    /// A short, stable, greppable name for the journal terminus + exit message.
    pub fn name(self: Ceiling) []const u8 {
        return switch (self) {
            .spawns => "max-total-spawns",
            .wall_clock => "max-wall-clock",
        };
    }
};

/// The journal terminus record's `terminal_verb` sentinel. A reader (resume /
/// post-mortem) recognizes this distinct value to know the run was
/// CEILING-TERMINATED (a clean kill-switch stop), not crashed mid-flight. It is
/// NOT a per-task attempt status (see `failedAttemptStatus`), so it never counts
/// toward any task's max-attempt budget.
pub const CEILING_TERMINUS_VERB: []const u8 = "ceiling";

/// Budgets bundles the kill-switch knobs. Sane GENEROUS defaults: these catch
/// runaways, they are not throughput tuning. Sourced from env vars in
/// production (see `fromEnv`) and injected directly in tests.
pub const Budgets = struct {
    /// Per-task max FAILED attempts before the task is blocked (not re-queued).
    /// Default 3: a task that has failed three times is misbehaving and wants
    /// operator triage, not a fourth automatic spawn.
    max_attempts: u32 = 3,
    /// Whole-run ceiling on the total number of worker spawns. Default 100: a
    /// generous cap that a well-behaved plan run stays well under but a runaway
    /// respawn loop blows through quickly.
    max_total_spawns: u32 = 100,
    /// Whole-run ceiling on wall-clock, in nanoseconds. Default 4h: a hard stop
    /// for a run that never terminates, generous enough never to clip a real
    /// unattended session.
    max_wall_clock_ns: i128 = @as(i128, 4) * 60 * 60 * std.time.ns_per_s,

    /// The default budgets (the generous kill-switch defaults).
    pub const default: Budgets = .{};

    /// Env-var names for the operator-tunable overrides. Each defaults to the
    /// corresponding `Budgets.default` field when unset or unparseable.
    pub const ENV_MAX_ATTEMPTS = "PLANAR_EXECUTE_MAX_ATTEMPTS";
    pub const ENV_MAX_TOTAL_SPAWNS = "PLANAR_EXECUTE_MAX_TOTAL_SPAWNS";
    /// Wall-clock override is supplied in SECONDS (operator-friendly) and
    /// converted to nanoseconds internally.
    pub const ENV_MAX_WALL_CLOCK_SECS = "PLANAR_EXECUTE_MAX_WALL_CLOCK_SECS";

    /// fromEnv builds a `Budgets` from the host environment, falling back to the
    /// generous defaults for any var that is unset or unparseable. A malformed
    /// value is NOT a hard error — the kill-switch must never refuse to run for a
    /// typo'd knob; it silently uses the default (the conservative safe choice).
    pub fn fromEnv(environ: std.process.Environ) Budgets {
        var b: Budgets = .default;
        if (parseU32Env(environ, ENV_MAX_ATTEMPTS)) |v| b.max_attempts = v;
        if (parseU32Env(environ, ENV_MAX_TOTAL_SPAWNS)) |v| b.max_total_spawns = v;
        if (parseU32Env(environ, ENV_MAX_WALL_CLOCK_SECS)) |secs| {
            b.max_wall_clock_ns = @as(i128, secs) * std.time.ns_per_s;
        }
        return b;
    }
};

/// parseU32Env reads `name` from `environ` and parses it as a base-10 u32.
/// Returns null when the var is unset, empty, or unparseable (the caller keeps
/// the default).
fn parseU32Env(environ: std.process.Environ, name: []const u8) ?u32 {
    const raw = environ.getPosix(name) orelse return null;
    const trimmed = std.mem.trim(u8, raw, " \t\r\n");
    if (trimmed.len == 0) return null;
    return std.fmt.parseInt(u32, trimmed, 10) catch null;
}

/// failedAttemptStatus returns true iff `verb` (a journal record's
/// `terminal_verb`) counts as a FAILED ATTEMPT for the per-task max-attempt
/// budget. See the module doc for the rationale of each membership decision.
pub fn failedAttemptStatus(verb: []const u8) bool {
    return std.mem.eql(u8, verb, "failed") or
        std.mem.eql(u8, verb, "timed-out") or
        std.mem.eql(u8, verb, "released");
}

/// failedAttemptCount counts how many journal records for `task_slug` represent
/// a prior FAILED attempt (per `failedAttemptStatus`). PURE over a record slice
/// — the caller reads the journal (`journal.read`) and passes the records in, so
/// this is trivially unit-testable with fixtures and so the count is derived
/// from the PERSISTENT journal (resume respects prior attempts).
pub fn failedAttemptCount(records: []const journal.JournalRecord, task_slug: []const u8) u32 {
    var n: u32 = 0;
    for (records) |rec| {
        if (!std.mem.eql(u8, rec.task_slug, task_slug)) continue;
        if (failedAttemptStatus(rec.terminal_verb)) n += 1;
    }
    return n;
}

/// RunCounters is the per-run, IN-MEMORY ceiling state: how many workers this
/// run has spawned so far + the run's monotonic start time. Distinct from the
/// per-task journal fail-count (which is persistent). The whole-run ceiling is
/// about THIS run's resource consumption, so an in-memory counter is correct —
/// a resume starts a fresh ceiling budget (the journal terminus tells the
/// operator the prior run was ceiling-stopped).
pub const RunCounters = struct {
    /// Workers spawned so far in this run (incremented at each actual spawn).
    spawn_count: u32 = 0,
    /// The run's monotonic start timestamp (nanoseconds), stamped once at run
    /// start from the scheduler's injectable clock.
    run_start_mono_ns: i128 = 0,
};

/// ceilingTripped returns the `Ceiling` that would be exceeded by performing
/// ONE MORE spawn given the current counters, or null when neither ceiling is
/// hit. PURE — the caller supplies `now_mono_ns` from the scheduler clock so the
/// check is deterministic under an injected fake clock.
///
/// Spawns is checked as `spawn_count >= max_total_spawns` (the Nth spawn is
/// refused once N spawns have already happened); wall-clock as
/// `now - run_start >= max_wall_clock_ns`. Spawns is checked first so an
/// over-spawn names the spawn ceiling even when both happen to be due.
pub fn ceilingTripped(
    counters: RunCounters,
    budgets: Budgets,
    now_mono_ns: i128,
) ?Ceiling {
    if (counters.spawn_count >= budgets.max_total_spawns) return .spawns;
    const elapsed = now_mono_ns - counters.run_start_mono_ns;
    if (elapsed >= budgets.max_wall_clock_ns) return .wall_clock;
    return null;
}

// ===========================================================================
// Tests — pure logic over journal fixtures + injected counters/clock.
// ===========================================================================

const testing = std.testing;

fn mkRec(task_slug: []const u8, verb: []const u8) journal.JournalRecord {
    return .{
        .prompt_hash = "h",
        .worktree = "/wt",
        .branch = "b",
        .claim_token = "tok",
        .model = "m",
        .role = "coder",
        .task_slug = task_slug,
        .exit_code = 0,
        .terminal_verb = verb,
        .wall_clock_ms = 0,
        .timestamp = 0,
    };
}

test "budget: failedAttemptStatus — failed/timed-out/released count; others do not" {
    try testing.expect(failedAttemptStatus("failed"));
    try testing.expect(failedAttemptStatus("timed-out"));
    try testing.expect(failedAttemptStatus("released"));
    try testing.expect(!failedAttemptStatus("completed"));
    try testing.expect(!failedAttemptStatus("respected"));
    try testing.expect(!failedAttemptStatus("blocked"));
    try testing.expect(!failedAttemptStatus("skipped"));
    try testing.expect(!failedAttemptStatus(CEILING_TERMINUS_VERB));
}

test "budget: failedAttemptCount counts only this task's failed attempts" {
    const records = [_]journal.JournalRecord{
        mkRec("ts-a", "failed"),
        mkRec("ts-a", "completed"), // success → not counted
        mkRec("ts-b", "failed"), // different task → not counted
        mkRec("ts-a", "timed-out"),
        mkRec("ts-a", "released"),
        mkRec("ts-a", "blocked"), // already set aside → not counted
        mkRec("ts-a", CEILING_TERMINUS_VERB), // terminus → not counted
    };
    // ts-a: failed + timed-out + released = 3.
    try testing.expectEqual(@as(u32, 3), failedAttemptCount(&records, "ts-a"));
    // ts-b: one failed.
    try testing.expectEqual(@as(u32, 1), failedAttemptCount(&records, "ts-b"));
    // unknown task: zero.
    try testing.expectEqual(@as(u32, 0), failedAttemptCount(&records, "ts-z"));
}

test "budget: failedAttemptCount on an empty journal is zero (no prior attempts)" {
    const records = [_]journal.JournalRecord{};
    try testing.expectEqual(@as(u32, 0), failedAttemptCount(&records, "anything"));
}

test "budget: ceilingTripped — spawns ceiling at count >= max_total_spawns" {
    const budgets = Budgets{ .max_total_spawns = 2, .max_wall_clock_ns = std.math.maxInt(i128) };
    // 0 and 1 spawns: still room for one more.
    try testing.expectEqual(@as(?Ceiling, null), ceilingTripped(.{ .spawn_count = 0 }, budgets, 0));
    try testing.expectEqual(@as(?Ceiling, null), ceilingTripped(.{ .spawn_count = 1 }, budgets, 0));
    // 2 spawns already done → the 3rd is refused.
    try testing.expectEqual(@as(?Ceiling, .spawns), ceilingTripped(.{ .spawn_count = 2 }, budgets, 0));
}

test "budget: ceilingTripped — wall-clock ceiling at elapsed >= max_wall_clock_ns" {
    const budgets = Budgets{ .max_total_spawns = 1000, .max_wall_clock_ns = 100 };
    const counters = RunCounters{ .spawn_count = 0, .run_start_mono_ns = 1_000 };
    // elapsed 50 < 100 → ok.
    try testing.expectEqual(@as(?Ceiling, null), ceilingTripped(counters, budgets, 1_050));
    // elapsed exactly 100 → tripped.
    try testing.expectEqual(@as(?Ceiling, .wall_clock), ceilingTripped(counters, budgets, 1_100));
    // elapsed 200 > 100 → tripped.
    try testing.expectEqual(@as(?Ceiling, .wall_clock), ceilingTripped(counters, budgets, 1_200));
}

test "budget: ceilingTripped — spawns checked before wall-clock when both due" {
    const budgets = Budgets{ .max_total_spawns = 1, .max_wall_clock_ns = 1 };
    const counters = RunCounters{ .spawn_count = 5, .run_start_mono_ns = 0 };
    // Both ceilings exceeded; spawns is named (checked first).
    try testing.expectEqual(@as(?Ceiling, .spawns), ceilingTripped(counters, budgets, 1_000_000));
}

test "budget: Ceiling.name is stable + greppable" {
    try testing.expectEqualStrings("max-total-spawns", Ceiling.spawns.name());
    try testing.expectEqualStrings("max-wall-clock", Ceiling.wall_clock.name());
}

test "budget: default budgets are the generous kill-switch values" {
    const d = Budgets.default;
    try testing.expectEqual(@as(u32, 3), d.max_attempts);
    try testing.expectEqual(@as(u32, 100), d.max_total_spawns);
    try testing.expectEqual(@as(i128, @as(i128, 4) * 60 * 60 * std.time.ns_per_s), d.max_wall_clock_ns);
}
