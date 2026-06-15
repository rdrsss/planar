//! handlers/reconcile — `planar-agent reconcile [--dry-run] [--stale-after]`
//!
//! Operator recovery: mark expired active claims stale, close orphaned
//! actions whose owning session has ended, and mark workflow_runs rows
//! `abandoned` when the harness process died without calling `run end`.
//!
//! JSON: { ok, claims_marked_stale, actions_closed, runs_abandoned, candidates? }.
//!
//! Run sweep (Q597): for each `workflow_runs` row with status = 'running',
//! probe the stored `pid` with `kill(pid, 0)` (POSIX liveness check).
//! ESRCH ⇒ the harness is gone ⇒ mark `abandoned` + set `ended_at`.
//! The run sweep is additive — it does NOT change the existing claim/action
//! sweep behaviour. Both sweeps run inside the same `BEGIN IMMEDIATE`
//! transaction.

const std = @import("std");
const builtin = @import("builtin");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const json = @import("json.zig");

const store = engine.runtime.agentactivity.store;

pub const verb: cli.Cmd = .{
    .name = "reconcile",
    .desc = "Operator recovery: mark expired claims stale, close orphaned actions, abandon dead runs.",
    .flags = &.{
        .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false }, .desc = "Report candidates without writing" },
        .{ .long = "--stale-after", .kind = .string, .default = .{ .string = "0" }, .desc = "Additional grace beyond lease expiry (default 0s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)" },
        .{ .long = "--session", .kind = .int, .default = .{ .int = 0 }, .desc = "Scope the sweep to a single session id (0 = global sweep, the default)" },
        .{ .long = "--plan", .kind = .int, .desc = "Scope the sweep to claims/actions/runs belonging to this plan id (0 or absent = global sweep)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

/// pidAlive returns true if a process with `pid` is currently alive.
/// Uses `kill(pid, 0)` (POSIX signal-0 liveness probe). POSIX-only;
/// always returns false on Windows (conservative: no "alive" misjudgment).
/// A non-positive pid is treated as dead — `kill(-1, 0)` / `kill(0, 0)`
/// do NOT mean "process not found" on POSIX.
fn pidAlive(pid: i64) bool {
    if (builtin.os.tag == .windows) return false;
    if (pid <= 0) return false; // guard: non-positive pids are stale.
    std.posix.kill(@intCast(pid), @enumFromInt(0)) catch |err| switch (err) {
        error.ProcessNotFound => return false, // ESRCH — gone.
        error.PermissionDenied => return true, // EPERM — exists, other owner.
        else => return true, // conservative: unknown errno → alive.
    };
    return true; // SUCCESS — process exists.
}

/// A workflow_runs row candidate for the dry-run report.
const RunCandidate = struct {
    id: i64,
    run_identifier: []const u8,
    pid: i64,
    plan_id: i64,

    fn deinit(self: RunCandidate, allocator: std.mem.Allocator) void {
        allocator.free(self.run_identifier);
    }
};

/// reconcileRuns finds running workflow_runs rows whose harness PID is dead
/// and marks them `abandoned`. Returns the count of rows abandoned.
/// In dry_run mode returns the candidates slice and does NOT write.
/// When plan_id is non-null, only rows belonging to that plan are considered.
/// MUST be called inside `BEGIN IMMEDIATE` (when not dry_run) — the caller
/// in `handle` owns the transaction.
fn reconcileRuns(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    dry_run: bool,
    plan_id: ?i64,
) !struct { abandoned: i64, candidates: []RunCandidate } {
    // Select all 'running' rows (there should rarely be more than a handful).
    // When plan_id is set, scope to that plan.
    var sel_buf: [256]u8 = undefined;
    const sel_sql: [:0]const u8 = if (plan_id) |pid|
        std.fmt.bufPrintZ(
            &sel_buf,
            "select id, run_identifier, pid, plan_id from workflow_runs" ++
                " where status = 'running' and plan_id = {d}",
            .{pid},
        ) catch return error.QueryFailed
    else
        "select id, run_identifier, pid, plan_id from workflow_runs where status = 'running'";
    var stmt = try d.prepare(sel_sql);
    defer stmt.finalize();

    var dead: std.ArrayList(RunCandidate) = .empty;
    errdefer {
        for (dead.items) |c| c.deinit(allocator);
        dead.deinit(allocator);
    }

    while (true) {
        switch (try stmt.step()) {
            .done => break,
            .row => {
                const row_id = stmt.columnInt(0);
                const rid = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(rid);
                const pid = stmt.columnInt(2);
                const row_plan_id = stmt.columnInt(3);

                if (!pidAlive(pid)) {
                    try dead.append(allocator, .{
                        .id = row_id,
                        .run_identifier = rid,
                        .pid = pid,
                        .plan_id = row_plan_id,
                    });
                } else {
                    allocator.free(rid);
                }
            },
        }
    }

    if (dry_run) {
        return .{ .abandoned = 0, .candidates = try dead.toOwnedSlice(allocator) };
    }

    // Mark each dead row abandoned within the caller's transaction.
    var abandoned_count: i64 = 0;
    for (dead.items) |c| {
        _ = d.execParams(
            \\update workflow_runs
            \\set status = 'abandoned', ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
            \\where id = ? and status = 'running'
        , &.{.{ .int = c.id }}) catch return error.QueryFailed;
        abandoned_count += 1;
    }

    const owned = try dead.toOwnedSlice(allocator);
    return .{ .abandoned = abandoned_count, .candidates = owned };
}

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"reconcile"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const stale_after_secs = cli.duration.parseSeconds(args.stale_after) catch |e|
        exit.die(ctx, e, "invalid --stale-after '{s}': expected bare seconds (e.g. 0) or suffixed duration (e.g. 10m, 1h, 500ms)", .{args.stale_after});

    // Wrap in BEGIN IMMEDIATE for non-dry-run so both sweeps (claims + runs)
    // are atomic (partial failure leaves no half-marked-stale claims).
    if (!args.dry_run) {
        d.exec("BEGIN IMMEDIATE") catch |e| exit.die(ctx, e, "BEGIN IMMEDIATE: {s}", .{@errorName(e)});
    }

    const plan_id_opt: ?i64 = if (args.plan) |p| (if (p > 0) p else null) else null;

    const result = store.reconcileStale(d, ctx.allocator, .{
        .stale_after_secs = stale_after_secs,
        .dry_run = args.dry_run,
        .session_id = if (args.session > 0) args.session else null,
        .plan_id = plan_id_opt,
    }) catch |e| {
        if (!args.dry_run) d.exec("ROLLBACK") catch {};
        exit.die(ctx, e, "reconcile: {s}", .{@errorName(e)});
    };
    defer result.deinit(ctx.allocator);

    // Run sweep: probe pid liveness for each `running` workflow_runs row.
    const run_result = reconcileRuns(d, ctx.allocator, args.dry_run, plan_id_opt) catch |e| {
        if (!args.dry_run) d.exec("ROLLBACK") catch {};
        exit.die(ctx, e, "reconcile runs: {s}", .{@errorName(e)});
    };
    defer {
        for (run_result.candidates) |c| c.deinit(ctx.allocator);
        ctx.allocator.free(run_result.candidates);
    }

    if (!args.dry_run) {
        d.exec("COMMIT") catch |e| exit.die(ctx, e, "COMMIT: {s}", .{@errorName(e)});
    }

    if (args.json) {
        const w = ctx.stdout;
        try w.print(
            "{{\"ok\":true,\"claims_marked_stale\":{d},\"actions_closed\":{d},\"runs_abandoned\":{d}",
            .{ result.claims_marked_stale, result.actions_closed, run_result.abandoned },
        );
        if (args.dry_run) {
            try w.print(",\"candidates\":[", .{});
            for (result.candidates, 0..) |c, i| {
                if (i > 0) try w.print(",", .{});
                try w.print(
                    "{{\"kind\":\"{s}\",\"id\":{d},\"claim\":",
                    .{ c.entity_kind.toText(), c.entity_id },
                );
                try json.writeClaim(w, c, null);
                try w.print("}}", .{});
            }
            try w.print("]", .{});
            try w.print(",\"run_candidates\":[", .{});
            for (run_result.candidates, 0..) |rc, i| {
                if (i > 0) try w.print(",", .{});
                try w.print(
                    "{{\"id\":{d},\"run_identifier\":\"{s}\",\"pid\":{d},\"plan_id\":{d}}}",
                    .{ rc.id, rc.run_identifier, rc.pid, rc.plan_id },
                );
            }
            try w.print("]", .{});
        }
        try w.print("}}\n", .{});
    } else {
        if (args.dry_run) {
            try ctx.stdout.print(
                "dry-run: {d} claim candidate(s), {d} run candidate(s)\n",
                .{ result.candidates.len, run_result.candidates.len },
            );
            for (result.candidates) |c| {
                try ctx.stdout.print("  {s}:{d} token:{s}\n", .{ c.entity_kind.toText(), c.entity_id, c.claim_token });
            }
            for (run_result.candidates) |rc| {
                try ctx.stdout.print("  run:{d} pid:{d} identifier:{s}\n", .{ rc.id, rc.pid, rc.run_identifier });
            }
        } else {
            try ctx.stdout.print(
                "reconciled: {d} claim(s) stale, {d} action(s) closed, {d} run(s) abandoned\n",
                .{ result.claims_marked_stale, result.actions_closed, run_result.abandoned },
            );
        }
    }
}

// =========================================================================
// Unit tests for the run sweep (claim sweep tests live in store.zig)
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

fn seedPlan(d: *db.sqlite.Db) !i64 {
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'p', 'sl', 'active')",
        &.{},
    );
    return try d.intQuery("select last_insert_rowid()");
}

fn insertRunningRow(d: *db.sqlite.Db, plan_id: i64, run_id_str: []const u8, pid: i64) !i64 {
    return d.execParams(
        "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) values (?, 'wf', ?, ?, '/r')",
        &.{ .{ .int = plan_id }, .{ .text = run_id_str }, .{ .int = pid } },
    );
}

test "reconcileRuns: dead-pid row is marked abandoned (dry_run=false)" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    // Use PID 2147483600: overwhelmingly unlikely to exist (ESRCH ⇒ dead).
    const dead_pid: i64 = 2147483600;
    const row_id = try insertRunningRow(&d, plan_id, "run-dead-1", dead_pid);

    const res = try reconcileRuns(&d, a, false, null);
    defer {
        for (res.candidates) |c| c.deinit(a);
        a.free(res.candidates);
    }

    // The row is dead → abandoned.
    if (builtin.os.tag != .windows) {
        try testing.expectEqual(@as(i64, 1), res.abandoned);

        var stmt = try d.prepare("select status from workflow_runs where id = ?");
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = row_id }});
        try testing.expect(try stmt.step() == .row);
        const st = try stmt.columnTextAlloc(0, a);
        defer a.free(st);
        try testing.expectEqualStrings("abandoned", st);
    }
}

test "reconcileRuns: dry_run returns candidates without writing" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    const dead_pid: i64 = 2147483600;
    _ = try insertRunningRow(&d, plan_id, "run-dead-dry", dead_pid);

    const res = try reconcileRuns(&d, a, true, null);
    defer {
        for (res.candidates) |c| c.deinit(a);
        a.free(res.candidates);
    }

    // dry_run: no writes; abandoned still 0.
    try testing.expectEqual(@as(i64, 0), res.abandoned);

    // The row must still be 'running' (no write happened).
    const count = try d.intQuery("select count(*) from workflow_runs where status = 'running'");
    try testing.expectEqual(@as(i64, 1), count);

    if (builtin.os.tag != .windows) {
        // candidates slice has one entry for the dead pid.
        try testing.expectEqual(@as(usize, 1), res.candidates.len);
        try testing.expectEqual(dead_pid, res.candidates[0].pid);
    }
}

test "reconcileRuns: live-pid row is NOT marked abandoned" {
    const testing = std.testing;
    const a = testing.allocator;

    // This test pins the invariant: a row whose pid is this process (which
    // is clearly alive) must NOT be abandoned. It verifies that a false-
    // positive dead verdict does not strand a live run.
    if (builtin.os.tag == .windows) return error.SkipZigTest;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    // Use this process's PID — it is definitively alive.
    const live_pid: i64 = @intCast(std.c.getpid());
    _ = try insertRunningRow(&d, plan_id, "run-live-1", live_pid);

    const res = try reconcileRuns(&d, a, false, null);
    defer {
        for (res.candidates) |c| c.deinit(a);
        a.free(res.candidates);
    }

    // A live pid is never abandoned.
    try testing.expectEqual(@as(i64, 0), res.abandoned);
    const count = try d.intQuery("select count(*) from workflow_runs where status = 'running'");
    try testing.expectEqual(@as(i64, 1), count);
}

test "reconcileRuns: non-positive pid treated as dead (guard against kill(-1,0) / kill(0,0))" {
    const testing = std.testing;
    const a = testing.allocator;

    if (builtin.os.tag == .windows) return error.SkipZigTest;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    // Insert a row with pid=-1 (malformed/stale payload).
    _ = try insertRunningRow(&d, plan_id, "run-neg-pid", -1);

    const res = try reconcileRuns(&d, a, false, null);
    defer {
        for (res.candidates) |c| c.deinit(a);
        a.free(res.candidates);
    }

    // pid=-1 is treated as dead → abandoned.
    try testing.expectEqual(@as(i64, 1), res.abandoned);
}

test "pidAlive: this process is alive; pid <=0 is dead" {
    if (builtin.os.tag == .windows) return error.SkipZigTest;
    const testing = std.testing;
    // This process must be alive.
    try testing.expect(pidAlive(@intCast(std.c.getpid())));
    // Non-positive pids are treated as dead (guard).
    try testing.expect(!pidAlive(0));
    try testing.expect(!pidAlive(-1));
    // A very high PID is overwhelmingly unlikely to exist.
    try testing.expect(!pidAlive(2147483600));
}
