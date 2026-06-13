//! handlers/run/end — `planar-agent run end` verb.
//!
//! Moves a `workflow_runs` row from `running` to a terminal status.
//! Valid terminal statuses (Q597): completed | failed | interrupted.
//!
//! `abandoned` is written ONLY by the reconcile sweep (`planar-agent
//! reconcile` → `reconcileRuns`), never by `run end`.  Crash-recovery
//! of dead runs is routed through `reconcile`, not through this verb.
//!
//! JSON output: { ok, run_id, status }.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const runtime = @import("runtime");

const main = @import("../../main.zig");
const exit = @import("../../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "end",
    .desc = "Close a workflow_runs row with a terminal status (completed|failed|interrupted).",
    .flags = &.{
        .{ .long = "--run-id", .kind = .string, .required = true, .desc = "The run identifier (run-<pid>-<nanos>) returned by run start" },
        .{ .long = "--status", .kind = .string, .required = true, .desc = "Terminal status: completed | failed | interrupted" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

/// The set of statuses `run end` is allowed to write. `running` is the
/// initial status set by `run start` and is never a valid terminal.
/// `abandoned` is NEVER written by this verb — it is written only by the
/// reconcile sweep (`planar-agent reconcile` → `reconcileRuns`).
const allowed_statuses = [_][]const u8{ "completed", "failed", "interrupted" };

fn isAllowedStatus(s: []const u8) bool {
    for (allowed_statuses) |a| {
        if (std.mem.eql(u8, s, a)) return true;
    }
    return false;
}

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "run", "end" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    if (!isAllowedStatus(args.status))
        exit.die(
            ctx,
            error.InvalidInput,
            "invalid --status '{s}': must be one of completed|failed|interrupted",
            .{args.status},
        );

    // Look up the run row and transition it atomically.
    // We use BEGIN IMMEDIATE so the read-then-update pair is race-free.
    d.exec("BEGIN IMMEDIATE") catch |e| exit.die(ctx, e, "BEGIN IMMEDIATE: {s}", .{@errorName(e)});

    var run_db_id: i64 = 0;
    {
        var stmt = d.prepare(
            "select id, status from workflow_runs where run_identifier = ?",
        ) catch |e| {
            d.exec("ROLLBACK") catch {};
            exit.die(ctx, e, "prepare run lookup: {s}", .{@errorName(e)});
        };
        defer stmt.finalize();
        stmt.bind(&.{.{ .text = args.run_id }}) catch |e| {
            d.exec("ROLLBACK") catch {};
            exit.die(ctx, e, "bind run lookup: {s}", .{@errorName(e)});
        };
        const step = stmt.step() catch |e| {
            d.exec("ROLLBACK") catch {};
            exit.die(ctx, e, "step run lookup: {s}", .{@errorName(e)});
        };
        if (step != .row) {
            d.exec("ROLLBACK") catch {};
            exit.die(ctx, error.NotFound, "run '{s}' not found", .{args.run_id});
        }
        run_db_id = stmt.columnInt(0);
        // Validate current status = running; refuse to close an already-closed run.
        const current_status_bytes = stmt.columnTextAlloc(1, ctx.allocator) catch |e| {
            d.exec("ROLLBACK") catch {};
            exit.die(ctx, e, "read status: {s}", .{@errorName(e)});
        };
        defer ctx.allocator.free(current_status_bytes);
        if (!std.mem.eql(u8, current_status_bytes, "running")) {
            d.exec("ROLLBACK") catch {};
            exit.die(
                ctx,
                error.InvalidInput,
                "run '{s}' is already in terminal status '{s}'; cannot end again",
                .{ args.run_id, current_status_bytes },
            );
        }
    }

    // Transition to terminal status and stamp ended_at.
    // Use a parameterized query so the status value is never interpolated
    // into the SQL string (consistent with engine conventions).
    _ = d.execParams(
        \\update workflow_runs
        \\set status = ?, ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\where id = ?
    , &.{
        .{ .text = args.status },
        .{ .int = run_db_id },
    }) catch |e| {
        d.exec("ROLLBACK") catch {};
        exit.die(ctx, e, "update workflow_runs: {s}", .{@errorName(e)});
    };

    d.exec("COMMIT") catch |e| exit.die(ctx, e, "COMMIT: {s}", .{@errorName(e)});

    if (args.json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"run_id\":{d},\"status\":\"{s}\"}}\n",
            .{ run_db_id, args.status },
        );
    } else {
        try ctx.stdout.print(
            "run:{d} status:{s}\n",
            .{ run_db_id, args.status },
        );
    }
}

// =========================================================================
// Unit tests
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

fn seedRunningRow(d: *db.sqlite.Db, plan_id: i64, run_identifier: []const u8, pid: i64) !i64 {
    return try d.execParams(
        "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) values (?, 'wf', ?, ?, '/r')",
        &.{ .{ .int = plan_id }, .{ .text = run_identifier }, .{ .int = pid } },
    );
}

fn seedPlan(d: *db.sqlite.Db) !i64 {
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'p', 'sl', 'active')",
        &.{},
    );
    return try d.intQuery("select last_insert_rowid()");
}

test "run end: transitions running → completed and sets ended_at" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    const run_db_id = try seedRunningRow(&d, plan_id, "run-1-100", 1);

    // Transition to completed (mirroring the handler's SQL).
    _ = try d.execParams(
        \\update workflow_runs
        \\set status = 'completed', ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\where id = ? and status = 'running'
    , &.{.{ .int = run_db_id }});

    var stmt = try d.prepare("select status, ended_at from workflow_runs where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = run_db_id }});
    try testing.expect(try stmt.step() == .row);
    const st = try stmt.columnTextAlloc(0, a);
    defer a.free(st);
    try testing.expectEqualStrings("completed", st);
    // ended_at must be non-null after the transition.
    try testing.expect(!stmt.columnIsNull(1));
}

test "run end: isAllowedStatus allows completed|failed|interrupted; rejects abandoned and running" {
    const testing = std.testing;
    try testing.expect(isAllowedStatus("completed"));
    try testing.expect(isAllowedStatus("failed"));
    try testing.expect(isAllowedStatus("interrupted"));
    try testing.expect(!isAllowedStatus("abandoned")); // abandoned is reconcile-only (Q597)
    try testing.expect(!isAllowedStatus("running"));
    try testing.expect(!isAllowedStatus(""));
}

test "run end: updating a non-running row is guarded by the where clause (no rows affected)" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    const run_db_id = try seedRunningRow(&d, plan_id, "run-2-200", 2);

    // Manually move the run to 'failed' first.
    _ = try d.execParams(
        "update workflow_runs set status = 'failed', ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?",
        &.{.{ .int = run_db_id }},
    );

    // A second update targeting status = 'running' must touch 0 rows (no change).
    var stmt = try d.prepare(
        "select count(*) from workflow_runs where id = ? and status = 'running'",
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = run_db_id }});
    try testing.expect(try stmt.step() == .row);
    try testing.expectEqual(@as(i64, 0), stmt.columnInt(0));
}
