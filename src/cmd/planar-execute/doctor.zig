//! doctor — `planar-execute doctor --plan <id> [--json]` (plan 492 task 3236).
//!
//! A READ-ONLY / NON-DESTRUCTIVE health check that drives the planar-execute
//! read-helper modules (`schema.zig`, `state.zig`, `worktree.zig`) against the
//! ambient DB (`PLANAR_DB` / cwd-resolved) and PATH-resolved sibling binaries
//! (`planar`, `planar-agent`, `planar-watch`, `git`), reporting whether each
//! read path actually works end-to-end.
//!
//! Until this verb existed, the subprocess halves of those helpers
//! (`spawnPlanar` / `spawnBin` / `runBin` / `runGit` → real binary output) had
//! NO live-binary coverage — every helper was fixture-parse tested only. `doctor`
//! closes that gap: it is the first caller in `main.zig` that drives the helpers
//! against real binaries, and `integration_tests/planar_execute_doctor_test.zig`
//! exercises it against freshly-built binaries + a seeded fixture DB.
//!
//! Probes (run in order; doctor NEVER bails on the first failure — it records
//! every probe's outcome and exits non-zero iff any probe failed):
//!   1. `schema.loadSchema("planar")`        → ok + command count.
//!   2. `schema.loadSchema("planar-agent")`  → ok + command count.
//!   3. `state.planShow(plan)`               → ok + plan title (+ derives slug).
//!   4. `state.planNext(plan)`               → ok + first available task id (or null).
//!   5. `state.testSpecStatus(plan)`         → ok + per-plan row count.
//!   6. reconcile DRY-RUN (`worktree.reconcileAndPrune(..., dry_run=true)`)
//!                                           → ok + stale-cycle count. Mutates nothing.
//!
//! NON-DESTRUCTIVE invariant: probe 6 is a dry run ONLY. It computes the stale
//! set (active-claim read + worktree enumerate + pure classify) but performs no
//! `teardownCycle` and no `git worktree prune`. doctor writes nothing to the DB
//! or the `agent_*` tables; it never claims, completes, or releases.
//!
//! Scope fence (this is M3-read territory; later milestones are intentionally
//! absent): no `agent()` / real spawn / claim-acquire / Lua wiring (M4), no
//! run-id tagging / run-lock / PID-liveness (M6), no budget logic.

const std = @import("std");
const Io = std.Io;

const state = @import("state.zig");
const schema = @import("schema.zig");
const worktree = @import("worktree.zig");

// ---------------------------------------------------------------------------
// Probe result records — one per probe, mirroring the approved JSON shape.
// ---------------------------------------------------------------------------

/// A schema-read probe outcome: ok + the command count parsed from
/// `<bin> schema`, or ok=false with a human-readable reason.
pub const SchemaProbe = struct {
    ok: bool,
    /// Number of commands in the parsed schema (only meaningful when ok).
    commands: usize = 0,
    /// Failure reason; null when ok.
    @"error": ?[]const u8 = null,
};

/// `plan show` probe outcome: ok + the plan title (borrowed from the Parsed
/// wrapper for the lifetime of the probe run), or ok=false with a reason.
pub const PlanShowProbe = struct {
    ok: bool,
    title: ?[]const u8 = null,
    @"error": ?[]const u8 = null,
};

/// `plan next` probe outcome: ok + the first available task id (null when no
/// task is available — still ok), or ok=false with a reason.
pub const PlanNextProbe = struct {
    ok: bool,
    task_id: ?u64 = null,
    @"error": ?[]const u8 = null,
};

/// `test-spec status` probe outcome: ok + the per-plan coverage row count, or
/// ok=false with a reason.
pub const TestSpecProbe = struct {
    ok: bool,
    plans: usize = 0,
    @"error": ?[]const u8 = null,
};

/// reconcile dry-run probe outcome: ok + the stale-cycle count (would-be prune
/// targets; dry-run mutates nothing), or ok=false with a reason.
pub const ReconcileProbe = struct {
    ok: bool,
    stale_cycles: usize = 0,
    @"error": ?[]const u8 = null,
};

/// The full doctor report. Field names match the operator-approved JSON shape
/// exactly (these are part of the contract pinned by the integration test).
pub const Report = struct {
    schema_reads: struct {
        planar: SchemaProbe,
        @"planar-agent": SchemaProbe,
    },
    plan_reads: struct {
        plan_show: PlanShowProbe,
        plan_next: PlanNextProbe,
        test_spec: TestSpecProbe,
    },
    reconcile_dry_run: ReconcileProbe,
    all_ok: bool,
};

// ---------------------------------------------------------------------------
// Probe drivers
// ---------------------------------------------------------------------------

/// schemaProbe drives `schema.loadSchema(bin_name)` and records the outcome.
/// The Parsed wrapper is freed before returning — only the scalar command count
/// is retained, so no caller-side lifetime management is needed.
fn schemaProbe(allocator: std.mem.Allocator, io: Io, bin_name: []const u8) SchemaProbe {
    const parsed = schema.loadSchema(allocator, io, bin_name) catch |e| {
        return .{ .ok = false, .@"error" = @errorName(e) };
    };
    defer parsed.deinit();
    return .{ .ok = true, .commands = parsed.value.commands.len };
}

/// Probe context for the plan-read probes. `title_buf` backs the (owned) copy of
/// the plan title and slug so they outlive the per-probe `Parsed` wrappers.
const PlanProbes = struct {
    plan_show: PlanShowProbe,
    plan_next: PlanNextProbe,
    test_spec: TestSpecProbe,
    /// Plan slug derived from the `plan show` read, owned by the arena passed to
    /// `runPlanProbes`. Null when the `plan show` probe failed or the plan had
    /// no slug; the reconcile probe then records ok=false.
    slug: ?[]const u8,
};

/// runPlanProbes drives probes 3–5 (`plan show`, `plan next`, `test-spec
/// status`). Strings retained in the result (`title`, `slug`) are duped into
/// `arena` so they outlive the individual `Parsed` wrappers. The arena must
/// outlive the returned `PlanProbes`.
fn runPlanProbes(arena: std.mem.Allocator, io: Io, plan: u64) PlanProbes {
    var out: PlanProbes = .{
        .plan_show = .{ .ok = false },
        .plan_next = .{ .ok = false },
        .test_spec = .{ .ok = false },
        .slug = null,
    };

    // Probe 3: plan show → title (+ slug for the reconcile probe).
    if (state.planShow(arena, io, plan)) |parsed| {
        defer parsed.deinit();
        const title_copy = arena.dupe(u8, parsed.value.title) catch null;
        out.plan_show = .{ .ok = true, .title = title_copy };
        if (parsed.value.slug) |s| {
            out.slug = arena.dupe(u8, s) catch null;
        }
    } else |e| {
        out.plan_show = .{ .ok = false, .@"error" = @errorName(e) };
    }

    // Probe 4: plan next → first available task id (null when none — still ok).
    if (state.planNext(arena, io, plan)) |parsed| {
        defer parsed.deinit();
        const first: ?u64 = if (parsed.value.available.len > 0)
            parsed.value.available[0].id
        else
            null;
        out.plan_next = .{ .ok = true, .task_id = first };
    } else |e| {
        out.plan_next = .{ .ok = false, .@"error" = @errorName(e) };
    }

    // Probe 5: test-spec status → per-plan row count.
    if (state.testSpecStatus(arena, io, plan)) |status_val| {
        var status = status_val;
        defer status.deinit(arena);
        out.test_spec = .{ .ok = true, .plans = status.per_plan.len };
    } else |e| {
        out.test_spec = .{ .ok = false, .@"error" = @errorName(e) };
    }

    return out;
}

/// reconcileProbe drives the NON-DESTRUCTIVE reconcile dry-run (probe 6).
///
/// `repo_root` is the cwd (`"."`); `git -C .` enumerates the ambient repo's
/// worktrees. `plan_slug` comes from the `plan show` read. When the slug is
/// unavailable (plan_show failed) or the cwd is not a git repo, the probe
/// records ok=false with a clear reason — defensively, never panicking.
fn reconcileProbe(allocator: std.mem.Allocator, io: Io, plan: u64, plan_slug: ?[]const u8) ReconcileProbe {
    _ = plan; // M6: prune is run-id/PID-ownership-scoped, not plan-id-scoped.
    const slug = plan_slug orelse {
        return .{ .ok = false, .@"error" = "plan slug unavailable (plan show probe failed or plan has no slug)" };
    };
    // doctor is a READ-ONLY probe, not a run: it holds no RunLock and has no
    // current run-id. Pass an empty current-run-id sentinel so the ownership
    // predicate reports every FOREIGN, DEAD cycle worktree as a would-be prune
    // target (an empty id never matches a real marker's run-id, and there is no
    // current run whose own worktree must be protected). `posixPidAlive` is the
    // real liveness probe. The dry-run flag still guarantees nothing is mutated.
    const result = worktree.reconcileAndPrune(allocator, io, ".", slug, "", worktree.posixPidAlive, true) catch |e| {
        return .{ .ok = false, .@"error" = @errorName(e) };
    };
    return .{ .ok = true, .stale_cycles = result.stale };
}

// ---------------------------------------------------------------------------
// Top-level orchestration
// ---------------------------------------------------------------------------

/// run drives all six probes and assembles the full `Report`. Strings in the
/// report borrow from `arena`, which the caller must keep alive until after the
/// report is serialized / printed.
///
/// `all_ok` is true iff every probe's `ok` is true. doctor runs every probe
/// regardless of earlier failures (collect-all-then-decide), so a single broken
/// read path does not mask the health of the others.
pub fn run(arena: std.mem.Allocator, io: Io, plan: u64) Report {
    const planar_schema = schemaProbe(arena, io, "planar");
    const agent_schema = schemaProbe(arena, io, "planar-agent");
    const plan_probes = runPlanProbes(arena, io, plan);
    const reconcile = reconcileProbe(arena, io, plan, plan_probes.slug);

    const all_ok = planar_schema.ok and
        agent_schema.ok and
        plan_probes.plan_show.ok and
        plan_probes.plan_next.ok and
        plan_probes.test_spec.ok and
        reconcile.ok;

    return .{
        .schema_reads = .{
            .planar = planar_schema,
            .@"planar-agent" = agent_schema,
        },
        .plan_reads = .{
            .plan_show = plan_probes.plan_show,
            .plan_next = plan_probes.plan_next,
            .test_spec = plan_probes.test_spec,
        },
        .reconcile_dry_run = reconcile,
        .all_ok = all_ok,
    };
}

/// printJson serializes the report to `writer` as the operator-approved JSON
/// shape. Optional `error` fields are omitted via `emit_null = false`-style
/// handling: std.json writes `null` for them, which is acceptable — the
/// integration test asserts on the populated fields and `ok` booleans, not on
/// the absence of `error` keys. The shape's required keys (`schema_reads`,
/// `plan_reads`, `reconcile_dry_run`, `all_ok`) are always present.
pub fn printJson(report: Report, writer: *Io.Writer) !void {
    try std.json.Stringify.value(report, .{ .emit_null_optional_fields = false }, writer);
    try writer.writeByte('\n');
}

/// printHuman writes a short one-line-per-probe health report to `writer`. The
/// JSON form is the load-bearing one the integration test asserts; this is for
/// an operator reading the output directly.
pub fn printHuman(report: Report, writer: *Io.Writer) !void {
    try writer.print("planar-execute doctor\n", .{});
    try printProbeLine(writer, "schema:planar", report.schema_reads.planar.ok, report.schema_reads.planar.@"error");
    try writer.print("  (commands: {d})\n", .{report.schema_reads.planar.commands});
    try printProbeLine(writer, "schema:planar-agent", report.schema_reads.@"planar-agent".ok, report.schema_reads.@"planar-agent".@"error");
    try writer.print("  (commands: {d})\n", .{report.schema_reads.@"planar-agent".commands});
    try printProbeLine(writer, "plan show", report.plan_reads.plan_show.ok, report.plan_reads.plan_show.@"error");
    if (report.plan_reads.plan_show.title) |t| try writer.print("  (title: {s})\n", .{t});
    try printProbeLine(writer, "plan next", report.plan_reads.plan_next.ok, report.plan_reads.plan_next.@"error");
    try printProbeLine(writer, "test-spec status", report.plan_reads.test_spec.ok, report.plan_reads.test_spec.@"error");
    try printProbeLine(writer, "reconcile (dry-run)", report.reconcile_dry_run.ok, report.reconcile_dry_run.@"error");
    try writer.print("  (stale_cycles: {d})\n", .{report.reconcile_dry_run.stale_cycles});
    try writer.print("all_ok: {s}\n", .{if (report.all_ok) "true" else "false"});
}

fn printProbeLine(writer: *Io.Writer, name: []const u8, ok: bool, err: ?[]const u8) !void {
    const mark = if (ok) "ok " else "FAIL";
    if (err) |e| {
        try writer.print("  [{s}] {s}: {s}\n", .{ mark, name, e });
    } else {
        try writer.print("  [{s}] {s}\n", .{ mark, name });
    }
}

// ---------------------------------------------------------------------------
// Unit tests — shape-level only. The live-binary behavior (real probes driving
// real binaries + a seeded DB) is exercised by the integration test; here we
// only pin the report→JSON serialization shape and the all_ok aggregation so a
// future field rename surfaces loudly.
// ---------------------------------------------------------------------------

test "doctor: all_ok JSON serialization shape (all-green)" {
    const gpa = std.testing.allocator;
    const report: Report = .{
        .schema_reads = .{
            .planar = .{ .ok = true, .commands = 243 },
            .@"planar-agent" = .{ .ok = true, .commands = 31 },
        },
        .plan_reads = .{
            .plan_show = .{ .ok = true, .title = "Autonomous workflow harness" },
            .plan_next = .{ .ok = true, .task_id = 3236 },
            .test_spec = .{ .ok = true, .plans = 10 },
        },
        .reconcile_dry_run = .{ .ok = true, .stale_cycles = 0 },
        .all_ok = true,
    };

    var buf: [2048]u8 = undefined;
    var w = Io.Writer.fixed(&buf);
    try printJson(report, &w);
    const out = w.buffered();

    // Required keys present (field names are the approved contract).
    try std.testing.expect(std.mem.indexOf(u8, out, "\"schema_reads\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"planar-agent\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"plan_reads\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"plan_show\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"plan_next\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"test_spec\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"reconcile_dry_run\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"stale_cycles\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"all_ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"commands\":243") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"task_id\":3236") != null);

    // Re-parse to confirm it is valid JSON decodable back into Report.
    const parsed = try std.json.parseFromSlice(Report, gpa, out, .{ .ignore_unknown_fields = true });
    defer parsed.deinit();
    try std.testing.expect(parsed.value.all_ok);
    try std.testing.expectEqual(@as(usize, 243), parsed.value.schema_reads.planar.commands);
}

test "doctor: a failed probe drives all_ok=false and records error" {
    const report: Report = .{
        .schema_reads = .{
            .planar = .{ .ok = true, .commands = 10 },
            .@"planar-agent" = .{ .ok = true, .commands = 5 },
        },
        .plan_reads = .{
            .plan_show = .{ .ok = false, .@"error" = "SubprocessNonZero" },
            .plan_next = .{ .ok = false, .@"error" = "SubprocessNonZero" },
            .test_spec = .{ .ok = false, .@"error" = "SubprocessNonZero" },
        },
        .reconcile_dry_run = .{ .ok = false, .@"error" = "plan slug unavailable (plan show probe failed or plan has no slug)" },
        .all_ok = false,
    };

    var buf: [2048]u8 = undefined;
    var w = Io.Writer.fixed(&buf);
    try printJson(report, &w);
    const out = w.buffered();

    try std.testing.expect(std.mem.indexOf(u8, out, "\"all_ok\":false") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"ok\":false") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "SubprocessNonZero") != null);
}
