//! handlers/context/resolve — `planar-agent context resolve` verb.
//!
//! Lifecycle transition primitive for `context_records`. Transitions records
//! from `active` to `consumed` or `superseded`. Two modes:
//!
//!   Single-record form: `--id <id> --status consumed|superseded`
//!     Transitions one record by primary key. Fails if the record is not in
//!     `active` status (already consumed/superseded).
//!
//!   Bulk form: `--run <id> --stage <s> --status consumed|superseded`
//!     Transitions ALL active records for the given (run, stage) pair.
//!     Used by stage-close compaction (task 3905) to mark raw records
//!     consumed after writing a capsule.
//!
//! Exactly one of `--id` or `--run` is required.
//!
//! JSON output: { ok, updated }.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const runtime = @import("runtime");

const main = @import("../../main.zig");
const exit = @import("../../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "resolve",
    .desc = "Transition context_records active → consumed|superseded (single record or bulk stage sweep).",
    .flags = &.{
        .{ .long = "--id", .kind = .string, .required = false, .desc = "Single record id to transition" },
        .{ .long = "--run", .kind = .string, .required = false, .desc = "Run id for bulk stage sweep (use with --stage)" },
        .{ .long = "--stage", .kind = .string, .required = false, .desc = "Stage name for bulk sweep (use with --run)" },
        .{ .long = "--status", .kind = .string, .required = true, .desc = "Target status: consumed|superseded" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "context", "resolve" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Validate --status.
    if (!std.mem.eql(u8, args.status, "consumed") and !std.mem.eql(u8, args.status, "superseded"))
        exit.die(
            ctx,
            error.InvalidInput,
            "invalid --status '{s}': must be consumed|superseded",
            .{args.status},
        );

    const has_id = args.id != null and args.id.?.len > 0;
    const has_run = args.run != null and args.run.?.len > 0;

    // Exactly one of --id or --run must be provided.
    if (has_id and has_run)
        exit.die(ctx, error.InvalidInput, "provide either --id or --run, not both", .{});
    if (!has_id and !has_run)
        exit.die(ctx, error.InvalidInput, "provide either --id (single record) or --run (bulk stage sweep)", .{});

    var updated: i64 = 0;

    if (has_id) {
        // Single-record transition.
        const id_str = args.id.?;
        const rec_id = std.fmt.parseInt(i64, id_str, 10) catch
            exit.die(ctx, error.InvalidInput, "invalid --id '{s}': expected integer", .{id_str});

        // Verify the record exists and is active.
        var check_stmt = d.prepare(
            "select status from context_records where id = ?",
        ) catch |e| exit.die(ctx, e, "prepare record check: {s}", .{@errorName(e)});
        defer check_stmt.finalize();
        check_stmt.bind(&.{.{ .int = rec_id }}) catch |e|
            exit.die(ctx, e, "bind record check: {s}", .{@errorName(e)});

        const step = check_stmt.step() catch |e|
            exit.die(ctx, e, "step record check: {s}", .{@errorName(e)});
        if (step != .row)
            exit.die(ctx, error.NotFound, "context_records row {d} not found", .{rec_id});

        const current_status = check_stmt.columnTextAlloc(0, ctx.allocator) catch |e|
            exit.die(ctx, e, "read status: {s}", .{@errorName(e)});
        defer ctx.allocator.free(current_status);

        if (!std.mem.eql(u8, current_status, "active"))
            exit.die(
                ctx,
                error.InvalidInput,
                "record {d} is already in status '{s}'; can only resolve active records",
                .{ rec_id, current_status },
            );

        // Transition.
        _ = d.execParams(
            "update context_records set status = ? where id = ? and status = 'active'",
            &.{
                .{ .text = args.status },
                .{ .int = rec_id },
            },
        ) catch |e| exit.die(ctx, e, "update context_records: {s}", .{@errorName(e)});

        // Count: 1 if it was updated, 0 if the where-clause guard raced
        // (extremely unlikely for single-record; still correct).
        var count_stmt = d.prepare(
            "select count(*) from context_records where id = ? and status = ?",
        ) catch |e| exit.die(ctx, e, "count update: {s}", .{@errorName(e)});
        defer count_stmt.finalize();
        count_stmt.bind(&.{
            .{ .int = rec_id },
            .{ .text = args.status },
        }) catch |e| exit.die(ctx, e, "bind count: {s}", .{@errorName(e)});
        _ = count_stmt.step() catch {};
        updated = count_stmt.columnInt(0);
    } else {
        // Bulk stage sweep.
        const run_str = args.run.?;
        const run_id = std.fmt.parseInt(i64, run_str, 10) catch
            exit.die(ctx, error.InvalidInput, "invalid --run '{s}': expected integer run id", .{run_str});

        const stage_str = args.stage orelse "";
        if (stage_str.len == 0)
            exit.die(ctx, error.InvalidInput, "--stage is required when using --run for a bulk sweep", .{});

        // Count before update for the response.
        const before_count = blk: {
            var cnt_stmt = d.prepare(
                "select count(*) from context_records where run_id = ? and stage = ? and status = 'active'",
            ) catch |e| exit.die(ctx, e, "prepare count: {s}", .{@errorName(e)});
            defer cnt_stmt.finalize();
            cnt_stmt.bind(&.{
                .{ .int = run_id },
                .{ .text = stage_str },
            }) catch |e| exit.die(ctx, e, "bind count: {s}", .{@errorName(e)});
            const cs = cnt_stmt.step() catch |e| exit.die(ctx, e, "step count: {s}", .{@errorName(e)});
            if (cs != .row) break :blk @as(i64, 0);
            break :blk cnt_stmt.columnInt(0);
        };

        _ = d.execParams(
            "update context_records set status = ? where run_id = ? and stage = ? and status = 'active'",
            &.{
                .{ .text = args.status },
                .{ .int = run_id },
                .{ .text = stage_str },
            },
        ) catch |e| exit.die(ctx, e, "bulk update context_records: {s}", .{@errorName(e)});

        updated = before_count;
    }

    if (args.json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"updated\":{d},\"status\":\"{s}\"}}\n",
            .{ updated, args.status },
        );
    } else {
        try ctx.stdout.print(
            "updated:{d} status:{s}\n",
            .{ updated, args.status },
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

fn seedFixture(d: *db.sqlite.Db) !struct { run_id: i64, session_id: i64, claim_id: i64 } {
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','p','sl','active')",
        &.{},
    );
    const plan_id = try d.intQuery("select last_insert_rowid()");
    const session_id = try d.execParams("insert into sessions (vendor) values ('test')", &.{});
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','t','todo')",
        &.{},
    );
    const run_id = try d.execParams(
        "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) values (?, 'wf', 'run-res-1', 1, '/r')",
        &.{.{ .int = plan_id }},
    );
    const claim_id = try d.execParams(
        \\insert into agent_work_claims
        \\  (claim_token, session_id, entity_kind, entity_id, claim_scope, status, vendor, lease_expires_at, run_id, stage)
        \\values ('tok-resolve', ?, 'task', ?, 'exclusive', 'active', 'test',
        \\   strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 seconds'), ?, 'plan')
    , &.{ .{ .int = session_id }, .{ .int = task_id }, .{ .int = run_id } });
    return .{ .run_id = run_id, .session_id = session_id, .claim_id = claim_id };
}

test "context resolve: single record transitions active → consumed" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const f = try seedFixture(&d);

    const rec_id = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'plan', ?, ?, 'finding', 'body', 'active')",
        &.{ .{ .int = f.run_id }, .{ .int = f.session_id }, .{ .int = f.claim_id } },
    );

    // Transition to consumed.
    _ = try d.execParams(
        "update context_records set status = 'consumed' where id = ? and status = 'active'",
        &.{.{ .int = rec_id }},
    );

    var stmt = try d.prepare("select status from context_records where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = rec_id }});
    try testing.expect(try stmt.step() == .row);
    const st = try stmt.columnTextAlloc(0, a);
    defer a.free(st);
    try testing.expectEqualStrings("consumed", st);
}

test "context resolve: bulk stage sweep transitions all active in stage" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const f = try seedFixture(&d);

    // Seed three active records in 'plan' stage and one in 'code'.
    _ = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'plan', ?, ?, 'finding', '1', 'active')",
        &.{ .{ .int = f.run_id }, .{ .int = f.session_id }, .{ .int = f.claim_id } },
    );
    _ = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'plan', ?, ?, 'risk', '2', 'active')",
        &.{ .{ .int = f.run_id }, .{ .int = f.session_id }, .{ .int = f.claim_id } },
    );
    _ = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'code', ?, ?, 'artifact', '3', 'active')",
        &.{ .{ .int = f.run_id }, .{ .int = f.session_id }, .{ .int = f.claim_id } },
    );

    // Bulk mark 'plan' stage consumed.
    _ = try d.execParams(
        "update context_records set status = 'consumed' where run_id = ? and stage = 'plan' and status = 'active'",
        &.{.{ .int = f.run_id }},
    );

    const plan_consumed = try d.intQuery("select count(*) from context_records where stage = 'plan' and status = 'consumed'");
    try testing.expectEqual(@as(i64, 2), plan_consumed);

    const code_still_active = try d.intQuery("select count(*) from context_records where stage = 'code' and status = 'active'");
    try testing.expectEqual(@as(i64, 1), code_still_active);
}

test "context resolve: schema rejects invalid status via CHECK constraint" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const f = try seedFixture(&d);
    const rec_id = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'plan', ?, ?, 'finding', 'body', 'active')",
        &.{ .{ .int = f.run_id }, .{ .int = f.session_id }, .{ .int = f.claim_id } },
    );

    const result = d.execParams(
        "update context_records set status = 'badstatus' where id = ?",
        &.{.{ .int = rec_id }},
    );
    try testing.expectError(db.sqlite.Error.StepFailed, result);
}
