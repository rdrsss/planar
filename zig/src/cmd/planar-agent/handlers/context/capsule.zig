//! handlers/context/capsule — `planar-agent context capsule` verb.
//!
//! Writes a `context_records` row with kind='capsule', run-keyed (not
//! claim-keyed). Used by stage-close compaction (plan 585 task 3905)
//! after the raw records for a stage have been transitioned to 'consumed'
//! via `context resolve --run --stage`.
//!
//! Decision 456: the capsule write is keyed to the workflow_runs row, not
//! to a worker claim — `claim_id` is NULL. Migration 00024 makes
//! `context_records.claim_id` nullable to support this.
//!
//! Invariants:
//!   - `--run` is the `workflow_runs.id` integer. Must reference an existing
//!     running/completed run.
//!   - `--stage` is the stage name (same free-text that raw records carry).
//!   - `--body` is the compiled capsule text.
//!   - `--compiled-from` is a comma-separated list of raw record ids that
//!     were distilled into this capsule (provenance back-link, decision 446).
//!     Optional but highly encouraged.
//!   - `--session` is optional. When provided, stored as `session_id`. When
//!     omitted, a minimal ephemeral session row is created and used.
//!   - `status = 'active'` always on insert (Q603: the capsule is inserted
//!     AFTER the raw records are marked 'consumed', so the capsule is never
//!     swept by the `context resolve` call that precedes it).
//!   - `claim_id = NULL` always (run-keyed, not claim-keyed — decision 456).
//!
//! JSON output: { ok, id, record }.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../../main.zig");
const exit = @import("../../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "capsule",
    .desc = "Write a compiled capsule context_records row, run-keyed (claim_id=NULL, decision 456).",
    .flags = &.{
        .{ .long = "--run", .kind = .string, .required = true, .desc = "workflow_runs.id — the run that owns this capsule" },
        .{ .long = "--stage", .kind = .string, .required = true, .desc = "Stage name this capsule distills (e.g. 'plan', 'code')" },
        .{ .long = "--body", .kind = .string, .required = true, .desc = "Compiled capsule body text" },
        .{ .long = "--compiled-from", .kind = .string, .required = false, .desc = "Comma-separated context_record ids this capsule distills (provenance)" },
        .{ .long = "--session", .kind = .string, .required = false, .desc = "session_id (integer). Optional — an ephemeral session is created when omitted." },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "context", "capsule" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Parse --run.
    const run_id_str = args.run;
    const run_id = std.fmt.parseInt(i64, run_id_str, 10) catch
        exit.die(ctx, error.InvalidInput, "invalid --run '{s}': expected integer", .{run_id_str});

    // Verify the run exists.
    const run_exists = blk: {
        var stmt = d.prepare("select count(*) from workflow_runs where id = ?") catch |e|
            exit.die(ctx, e, "prepare run check: {s}", .{@errorName(e)});
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = run_id }}) catch |e|
            exit.die(ctx, e, "bind run check: {s}", .{@errorName(e)});
        const step = stmt.step() catch |e|
            exit.die(ctx, e, "step run check: {s}", .{@errorName(e)});
        if (step != .row) break :blk false;
        break :blk stmt.columnInt(0) > 0;
    };
    if (!run_exists)
        exit.die(ctx, error.NotFound, "workflow_run {d} not found", .{run_id});

    // Resolve session_id: use --session if provided, otherwise create an ephemeral session.
    const session_id: i64 = blk: {
        const session_str = args.session orelse "";
        if (session_str.len > 0) {
            const sid = std.fmt.parseInt(i64, session_str, 10) catch
                exit.die(ctx, error.InvalidInput, "invalid --session '{s}': expected integer", .{session_str});
            break :blk sid;
        }
        // Create a minimal ephemeral session row for the compactor write.
        const sid = d.execParams(
            "insert into sessions (vendor) values ('compactor')",
            &.{},
        ) catch |e| exit.die(ctx, e, "insert ephemeral session: {s}", .{@errorName(e)});
        break :blk sid;
    };

    // --compiled-from: optional provenance string.
    const compiled_from_val: []const u8 = args.compiled_from orelse "";
    const compiled_from: ?[]const u8 = if (compiled_from_val.len > 0) compiled_from_val else null;

    // Insert the capsule row. claim_id is NULL (decision 456).
    const capsule_id = d.execParams(
        \\insert into context_records
        \\  (run_id, stage, session_id, claim_id, kind, body, status, compiled_from)
        \\values (?, ?, ?, NULL, 'capsule', ?, 'active', ?)
    , &.{
        .{ .int = run_id },
        .{ .text = args.stage },
        .{ .int = session_id },
        .{ .text = args.body },
        if (compiled_from) |cf| .{ .text = cf } else .{ .null = {} },
    }) catch |e| exit.die(ctx, e, "insert capsule: {s}", .{@errorName(e)});

    if (args.json) {
        const w = ctx.stdout;
        try w.print(
            "{{\"ok\":true,\"id\":{d},\"record\":{{" ++
                "\"id\":{d}," ++
                "\"run_id\":{d}," ++
                "\"stage\":\"{s}\"," ++
                "\"session_id\":{d}," ++
                "\"claim_id\":null," ++
                "\"kind\":\"capsule\"," ++
                "\"status\":\"active\"" ++
                "}}}}\n",
            .{
                capsule_id,
                capsule_id,
                run_id,
                args.stage,
                session_id,
            },
        );
    } else {
        try ctx.stdout.print(
            "capsule:{d} run:{d} stage:{s} kind:capsule status:active\n",
            .{ capsule_id, run_id, args.stage },
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

fn seedPlan(d: *db.sqlite.Db) !i64 {
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'p', 'cap-sl', 'active')",
        &.{},
    );
    return try d.intQuery("select last_insert_rowid()");
}

fn seedSession(d: *db.sqlite.Db) !i64 {
    return try d.execParams(
        "insert into sessions (vendor) values ('test')",
        &.{},
    );
}

fn seedWorkflowRun(d: *db.sqlite.Db, plan_id: i64) !i64 {
    return try d.execParams(
        \\insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root)
        \\values (?, 'wf', 'run-cap-1', 999, '/r')
    , &.{.{ .int = plan_id }});
}

test "capsule: insert with explicit session stores claim_id=NULL" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    const session_id = try seedSession(&d);
    const run_id = try seedWorkflowRun(&d, plan_id);

    // Seed two raw records.
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global', 't', 'todo')",
        &.{},
    );
    const claim_id = try d.execParams(
        \\insert into agent_work_claims
        \\  (claim_token, session_id, entity_kind, entity_id, claim_scope, status, vendor, lease_expires_at, run_id, stage)
        \\values ('tok-cap1', ?, 'task', ?, 'exclusive', 'active', 'test',
        \\   strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 seconds'), ?, 'plan')
    , &.{ .{ .int = session_id }, .{ .int = task_id }, .{ .int = run_id } });

    const raw1 = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'plan', ?, ?, 'finding', 'raw 1', 'consumed')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );
    const raw2 = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'plan', ?, ?, 'risk', 'raw 2', 'consumed')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );

    // Insert a capsule row with claim_id=NULL.
    const provenance = try std.fmt.allocPrint(a, "{d},{d}", .{ raw1, raw2 });
    defer a.free(provenance);

    const capsule_id = try d.execParams(
        \\insert into context_records
        \\  (run_id, stage, session_id, claim_id, kind, body, status, compiled_from)
        \\values (?, 'plan', ?, NULL, 'capsule', 'compiled capsule body', 'active', ?)
    , &.{
        .{ .int = run_id },
        .{ .int = session_id },
        .{ .text = provenance },
    });

    // Verify: claim_id is NULL, kind='capsule', status='active'.
    var stmt = try d.prepare(
        "select run_id, stage, session_id, claim_id, kind, status, compiled_from from context_records where id = ?",
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = capsule_id }});
    try testing.expect(try stmt.step() == .row);
    try testing.expectEqual(run_id, stmt.columnInt(0));

    const stg = try stmt.columnTextAlloc(1, a);
    defer a.free(stg);
    try testing.expectEqualStrings("plan", stg);

    try testing.expectEqual(session_id, stmt.columnInt(2));
    try testing.expect(stmt.columnIsNull(3)); // claim_id IS NULL

    const kind = try stmt.columnTextAlloc(4, a);
    defer a.free(kind);
    try testing.expectEqualStrings("capsule", kind);

    const status = try stmt.columnTextAlloc(5, a);
    defer a.free(status);
    try testing.expectEqualStrings("active", status);

    // compiled_from carries the provenance ids.
    try testing.expect(!stmt.columnIsNull(6));
    const cf = try stmt.columnTextAlloc(6, a);
    defer a.free(cf);
    try testing.expectEqualStrings(provenance, cf);
}

test "capsule: raw records can be consumed before capsule insert (Q603 order)" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    const session_id = try seedSession(&d);
    const run_id = try seedWorkflowRun(&d, plan_id);
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global', 't2', 'todo')",
        &.{},
    );
    const claim_id = try d.execParams(
        \\insert into agent_work_claims
        \\  (claim_token, session_id, entity_kind, entity_id, claim_scope, status, vendor, lease_expires_at, run_id, stage)
        \\values ('tok-cap2', ?, 'task', ?, 'exclusive', 'active', 'test',
        \\   strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 seconds'), ?, 'code')
    , &.{ .{ .int = session_id }, .{ .int = task_id }, .{ .int = run_id } });

    // Seed 3 active records in 'code' stage.
    const r1 = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'code', ?, ?, 'finding', 'f1', 'active')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );
    const r2 = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'code', ?, ?, 'risk', 'r1', 'active')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );
    const r3 = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'code', ?, ?, 'followup', 'fo1', 'active')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );

    // Step 1 (Q603 ORDER): resolve the raw records to 'consumed'.
    _ = try d.execParams(
        "update context_records set status = 'consumed' where run_id = ? and stage = 'code' and status = 'active'",
        &.{.{ .int = run_id }},
    );

    // Verify all three are now consumed.
    const consumed_count = blk: {
        var st = try d.prepare("select count(*) from context_records where run_id = ? and stage = 'code' and status = 'consumed'");
        defer st.finalize();
        try st.bind(&.{.{ .int = run_id }});
        _ = try st.step();
        break :blk st.columnInt(0);
    };
    try testing.expectEqual(@as(i64, 3), consumed_count);

    // Step 2 (Q603 ORDER): insert the capsule AFTER.
    const prov = try std.fmt.allocPrint(a, "{d},{d},{d}", .{ r1, r2, r3 });
    defer a.free(prov);

    const cap_id = try d.execParams(
        \\insert into context_records
        \\  (run_id, stage, session_id, claim_id, kind, body, status, compiled_from)
        \\values (?, 'code', ?, NULL, 'capsule', 'distilled body', 'active', ?)
    , &.{
        .{ .int = run_id },
        .{ .int = session_id },
        .{ .text = prov },
    });

    // The capsule must be 'active' (not swept because it was inserted after the bulk resolve).
    var cap_stmt = try d.prepare("select status from context_records where id = ?");
    defer cap_stmt.finalize();
    try cap_stmt.bind(&.{.{ .int = cap_id }});
    try testing.expect(try cap_stmt.step() == .row);
    const cap_status = try cap_stmt.columnTextAlloc(0, a);
    defer a.free(cap_status);
    try testing.expectEqualStrings("active", cap_status);

    // Verify total record counts: 3 consumed raw + 1 active capsule.
    // Only one run in this test DB, so unqualified counts are safe here.
    try testing.expectEqual(
        @as(i64, 3),
        try d.intQuery("select count(*) from context_records where status = 'consumed'"),
    );
    try testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from context_records where status = 'active'"),
    );
}

test "capsule: ephemeral session insert (simulating omitted --session)" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    const run_id = try seedWorkflowRun(&d, plan_id);

    // Simulate what the handler does when --session is omitted:
    // create an ephemeral session.
    const ephemeral_session = try d.execParams(
        "insert into sessions (vendor) values ('compactor')",
        &.{},
    );

    // Insert capsule with the ephemeral session.
    const cap_id = try d.execParams(
        \\insert into context_records
        \\  (run_id, stage, session_id, claim_id, kind, body, status)
        \\values (?, 'plan', ?, NULL, 'capsule', 'ephemeral-session capsule', 'active')
    , &.{
        .{ .int = run_id },
        .{ .int = ephemeral_session },
    });

    var stmt = try d.prepare("select session_id, claim_id, kind from context_records where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = cap_id }});
    try testing.expect(try stmt.step() == .row);
    try testing.expectEqual(ephemeral_session, stmt.columnInt(0));
    try testing.expect(stmt.columnIsNull(1)); // claim_id NULL
    const kind = try stmt.columnTextAlloc(2, a);
    defer a.free(kind);
    try testing.expectEqualStrings("capsule", kind);
}
