//! handlers/run/start — `planar-agent run start` verb.
//!
//! Inserts a `workflow_runs` row in `running` status. The caller
//! (planar-execute) supplies the harness pid, run-identifier, and
//! repo_root via flags so crash-reconciliation can probe the HARNESS
//! process, not the planar-agent subprocess that ran this verb.
//!
//! JSON output: { ok, run_id, run }.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const runtime = @import("runtime");

const main = @import("../../main.zig");
const exit = @import("../../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "start",
    .desc = "Insert a workflow_runs row in running status.",
    .flags = &.{
        .{ .long = "--plan", .kind = .string, .required = true, .desc = "Plan id the run belongs to" },
        .{ .long = "--workflow", .kind = .string, .required = true, .desc = "Workflow name (e.g. isolated-sequential)" },
        .{ .long = "--run-id", .kind = .string, .required = true, .desc = "Unique run identifier (run-<pid>-<nanos>)" },
        .{ .long = "--pid", .kind = .string, .required = true, .desc = "PID of the planar-execute harness process" },
        .{ .long = "--repo-root", .kind = .string, .required = true, .desc = "Absolute path of the repo root the harness is driving" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "run", "start" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const plan_id = std.fmt.parseInt(i64, args.plan, 10) catch
        exit.die(ctx, error.InvalidInput, "invalid --plan '{s}': expected integer plan id", .{args.plan});

    const pid = std.fmt.parseInt(i64, args.pid, 10) catch
        exit.die(ctx, error.InvalidInput, "invalid --pid '{s}': expected integer process id", .{args.pid});

    // Validate: plan must exist.
    {
        var stmt = d.prepare("select count(*) from plans where id = ?") catch |e|
            exit.die(ctx, e, "prepare plan check: {s}", .{@errorName(e)});
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch |e|
            exit.die(ctx, e, "bind plan check: {s}", .{@errorName(e)});
        const step_result = stmt.step() catch |e|
            exit.die(ctx, e, "step plan check: {s}", .{@errorName(e)});
        if (step_result != .row) exit.die(ctx, error.NotFound, "plan not found", .{});
        if (stmt.columnInt(0) == 0) exit.die(ctx, error.NotFound, "plan {d} not found", .{plan_id});
    }

    // run_identifier must be unique (the schema has a unique index on it).
    const run_db_id = d.execParams(
        \\insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root)
        \\values (?, ?, ?, ?, ?)
    , &.{
        .{ .int = plan_id },
        .{ .text = args.workflow },
        .{ .text = args.run_id },
        .{ .int = pid },
        .{ .text = args.repo_root },
    }) catch |e| exit.die(ctx, e, "insert workflow_runs: {s}", .{@errorName(e)});

    if (args.json) {
        const w = ctx.stdout;
        try w.print(
            "{{\"ok\":true,\"run_id\":{d},\"run\":{{" ++
                "\"id\":{d}," ++
                "\"plan_id\":{d}," ++
                "\"workflow_name\":\"{s}\"," ++
                "\"run_identifier\":\"{s}\"," ++
                "\"pid\":{d}," ++
                "\"repo_root\":\"{s}\"," ++
                "\"status\":\"running\"" ++
                "}}}}\n",
            .{
                run_db_id,
                run_db_id,
                plan_id,
                args.workflow,
                args.run_id,
                pid,
                args.repo_root,
            },
        );
    } else {
        try ctx.stdout.print(
            "run:{d} plan:{d} workflow:{s} pid:{d} status:running\n",
            .{ run_db_id, plan_id, args.workflow, pid },
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

test "run start: inserts workflow_runs row in running status" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    // Seed a plan.
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'test plan', 'test-plan', 'active')",
        &.{},
    );
    const plan_id = try d.intQuery("select last_insert_rowid()");

    // Insert a workflow_runs row.
    const run_id = try d.execParams(
        \\insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root)
        \\values (?, ?, ?, ?, ?)
    , &.{
        .{ .int = plan_id },
        .{ .text = "isolated-sequential" },
        .{ .text = "run-1234-999" },
        .{ .int = 1234 },
        .{ .text = "/repo" },
    });

    // Verify status = running and all columns.
    var stmt = try d.prepare(
        "select plan_id, workflow_name, run_identifier, pid, repo_root, status from workflow_runs where id = ?",
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = run_id }});
    try testing.expect(try stmt.step() == .row);
    try testing.expectEqual(plan_id, stmt.columnInt(0));
    const wf = try stmt.columnTextAlloc(1, a);
    defer a.free(wf);
    try testing.expectEqualStrings("isolated-sequential", wf);
    const rid = try stmt.columnTextAlloc(2, a);
    defer a.free(rid);
    try testing.expectEqualStrings("run-1234-999", rid);
    try testing.expectEqual(@as(i64, 1234), stmt.columnInt(3));
    const rr = try stmt.columnTextAlloc(4, a);
    defer a.free(rr);
    try testing.expectEqualStrings("/repo", rr);
    const st = try stmt.columnTextAlloc(5, a);
    defer a.free(st);
    try testing.expectEqualStrings("running", st);
}

test "run start: duplicate run_identifier is rejected by unique index" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'p', 'sl', 'active')",
        &.{},
    );
    const plan_id = try d.intQuery("select last_insert_rowid()");

    _ = try d.execParams(
        "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) values (?, 'wf', 'run-dup', 1, '/r')",
        &.{.{ .int = plan_id }},
    );
    // Second insert with the same run_identifier must fail (unique constraint).
    // execParams returns error.StepFailed when SQLite returns a constraint error.
    const result = d.execParams(
        "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) values (?, 'wf', 'run-dup', 2, '/r')",
        &.{.{ .int = plan_id }},
    );
    try testing.expectError(db.sqlite.Error.StepFailed, result);
}
