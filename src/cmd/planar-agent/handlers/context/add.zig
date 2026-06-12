//! handlers/context/add — `planar-agent context add` verb.
//!
//! Writes a `context_records` row. The caller supplies `--claim <token>`,
//! `--kind <kind>`, and `--body <text>`; `run_id`, `stage`, `session_id`,
//! and `claim_id` are stamped SERVER-SIDE from the claim row (decision 447).
//!
//! Invariants:
//!   - The claim MUST have a non-null `run_id`; context records are
//!     run-scoped (errors clearly if the claim is not run-associated).
//!   - `kind` must be one of the migration's CHECK values:
//!     finding | risk | artifact | followup | summary | capsule.
//!   - For `capsule` kind, `--compiled-from <id,id,...>` sets provenance.
//!   - `status = 'active'` always on insert (lifecycle via `context resolve`).
//!   - No uniqueness constraint: append-only per decision Q599.
//!
//! JSON output: { ok, id, record }.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const runtime = @import("runtime");

const main = @import("../../main.zig");
const exit = @import("../../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "add",
    .desc = "Write a context_records row stamped from the claim's run_id, stage, session_id.",
    .flags = &.{
        .{ .long = "--claim", .kind = .string, .required = true, .desc = "Claim token that owns this context record" },
        .{ .long = "--kind", .kind = .string, .required = true, .desc = "Record kind: finding|risk|artifact|followup|summary|capsule" },
        .{ .long = "--body", .kind = .string, .required = true, .desc = "Record body text" },
        .{ .long = "--compiled-from", .kind = .string, .required = false, .desc = "Comma-separated context_record ids this capsule was compiled from (capsule kind only)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

/// Valid kind values from the migration CHECK constraint.
const valid_kinds = [_][]const u8{
    "finding", "risk", "artifact", "followup", "summary", "capsule",
};

fn isValidKind(k: []const u8) bool {
    for (valid_kinds) |v| {
        if (std.mem.eql(u8, k, v)) return true;
    }
    return false;
}

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "context", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Validate kind.
    if (!isValidKind(args.kind))
        exit.die(
            ctx,
            error.InvalidInput,
            "invalid --kind '{s}': must be one of finding|risk|artifact|followup|summary|capsule",
            .{args.kind},
        );

    // Look up the claim by token.
    var claim_stmt = d.prepare(
        \\select id, run_id, stage, session_id
        \\from agent_work_claims
        \\where claim_token = ?
    ) catch |e| exit.die(ctx, e, "prepare claim lookup: {s}", .{@errorName(e)});
    defer claim_stmt.finalize();
    claim_stmt.bind(&.{.{ .text = args.claim }}) catch |e|
        exit.die(ctx, e, "bind claim lookup: {s}", .{@errorName(e)});

    const step = claim_stmt.step() catch |e|
        exit.die(ctx, e, "step claim lookup: {s}", .{@errorName(e)});
    if (step != .row)
        exit.die(ctx, error.NotFound, "claim token not found: '{s}'", .{args.claim});

    const claim_id = claim_stmt.columnInt(0);
    const run_id_opt = claim_stmt.columnIntOpt(1);
    const stage_opt = claim_stmt.columnTextOpt(2, ctx.allocator) catch |e|
        exit.die(ctx, e, "read claim stage: {s}", .{@errorName(e)});
    defer if (stage_opt) |s| ctx.allocator.free(s);
    const session_id = claim_stmt.columnInt(3);

    // Enforce run_id required (decision 447).
    const run_id = run_id_opt orelse exit.die(
        ctx,
        error.InvalidInput,
        "claim '{s}' has no run_id: context records are run-scoped; acquire the claim with --run <id>",
        .{args.claim},
    );

    // stage: use the claim's stage value; default to empty string when null.
    // The schema requires stage NOT NULL; blank is acceptable for claims
    // dispatched without --stage.
    const stage = stage_opt orelse "";

    // --compiled-from: optional comma-separated list of record ids.
    // Field name derived from --compiled-from by stripping -- and converting
    // hyphens to underscores (etc-cli convention: flagFieldName).
    const compiled_from_val: []const u8 = args.compiled_from orelse "";
    const compiled_from: ?[]const u8 = if (compiled_from_val.len > 0) compiled_from_val else null;

    // Insert the context_records row.
    const record_id = d.execParams(
        \\insert into context_records (run_id, stage, session_id, claim_id, kind, body, status, compiled_from)
        \\values (?, ?, ?, ?, ?, ?, 'active', ?)
    , &.{
        .{ .int = run_id },
        .{ .text = stage },
        .{ .int = session_id },
        .{ .int = claim_id },
        .{ .text = args.kind },
        .{ .text = args.body },
        if (compiled_from) |cf| .{ .text = cf } else .{ .null = {} },
    }) catch |e| exit.die(ctx, e, "insert context_records: {s}", .{@errorName(e)});

    if (args.json) {
        const w = ctx.stdout;
        try w.print(
            "{{\"ok\":true,\"id\":{d},\"record\":{{" ++
                "\"id\":{d}," ++
                "\"run_id\":{d}," ++
                "\"stage\":\"{s}\"," ++
                "\"session_id\":{d}," ++
                "\"claim_id\":{d}," ++
                "\"kind\":\"{s}\"," ++
                "\"status\":\"active\"" ++
                "}}}}\n",
            .{
                record_id,
                record_id,
                run_id,
                stage,
                session_id,
                claim_id,
                args.kind,
            },
        );
    } else {
        try ctx.stdout.print(
            "context:{d} run:{d} stage:{s} kind:{s} status:active\n",
            .{ record_id, run_id, stage, args.kind },
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
        "insert into plans (scope_kind, title, slug, status) values ('global', 'p', 'sl', 'active')",
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

fn seedTask(d: *db.sqlite.Db) !i64 {
    return try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global', 't', 'todo')",
        &.{},
    );
}

fn seedWorkflowRun(d: *db.sqlite.Db, plan_id: i64) !i64 {
    return try d.execParams(
        \\insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root)
        \\values (?, 'wf', 'run-test-1', 999, '/r')
    , &.{.{ .int = plan_id }});
}

test "isValidKind: accepts all migration CHECK values" {
    const testing = std.testing;
    try testing.expect(isValidKind("finding"));
    try testing.expect(isValidKind("risk"));
    try testing.expect(isValidKind("artifact"));
    try testing.expect(isValidKind("followup"));
    try testing.expect(isValidKind("summary"));
    try testing.expect(isValidKind("capsule"));
    try testing.expect(!isValidKind("note"));
    try testing.expect(!isValidKind(""));
    try testing.expect(!isValidKind("FINDING"));
}

test "context add: inserting a record stores correct columns" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    const session_id = try seedSession(&d);
    const task_id = try seedTask(&d);
    const run_id = try seedWorkflowRun(&d, plan_id);
    const claim_id = try d.execParams(
        \\insert into agent_work_claims
        \\  (claim_token, session_id, entity_kind, entity_id, claim_scope, status, vendor, lease_expires_at, run_id, stage)
        \\values ('tok-add', ?, 'task', ?, 'exclusive', 'active', 'test',
        \\   strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 seconds'), ?, 'code')
    , &.{ .{ .int = session_id }, .{ .int = task_id }, .{ .int = run_id } });

    // Insert a context record.
    const record_id = try d.execParams(
        \\insert into context_records (run_id, stage, session_id, claim_id, kind, body, status)
        \\values (?, 'code', ?, ?, 'finding', 'test body', 'active')
    , &.{
        .{ .int = run_id },
        .{ .int = session_id },
        .{ .int = claim_id },
    });

    // Verify all columns are stored correctly.
    var stmt = try d.prepare(
        "select run_id, stage, session_id, claim_id, kind, body, status, compiled_from from context_records where id = ?",
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = record_id }});
    try testing.expect(try stmt.step() == .row);
    try testing.expectEqual(run_id, stmt.columnInt(0));
    const st = try stmt.columnTextAlloc(1, a);
    defer a.free(st);
    try testing.expectEqualStrings("code", st);
    try testing.expectEqual(session_id, stmt.columnInt(2));
    try testing.expectEqual(claim_id, stmt.columnInt(3));
    const kind = try stmt.columnTextAlloc(4, a);
    defer a.free(kind);
    try testing.expectEqualStrings("finding", kind);
    const body = try stmt.columnTextAlloc(5, a);
    defer a.free(body);
    try testing.expectEqualStrings("test body", body);
    const status = try stmt.columnTextAlloc(6, a);
    defer a.free(status);
    try testing.expectEqualStrings("active", status);
    try testing.expect(stmt.columnIsNull(7)); // compiled_from null for non-capsule
}

test "context add: capsule kind with compiled_from stores provenance" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    const session_id = try seedSession(&d);
    const task_id = try seedTask(&d);
    const run_id = try seedWorkflowRun(&d, plan_id);
    const claim_id = try d.execParams(
        \\insert into agent_work_claims
        \\  (claim_token, session_id, entity_kind, entity_id, claim_scope, status, vendor, lease_expires_at, run_id)
        \\values ('tok-capsule', ?, 'task', ?, 'exclusive', 'active', 'test',
        \\   strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 seconds'), ?)
    , &.{ .{ .int = session_id }, .{ .int = task_id }, .{ .int = run_id } });

    // Insert raw records first.
    const raw1 = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'plan', ?, ?, 'finding', 'raw 1', 'active')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );
    const raw2 = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'plan', ?, ?, 'risk', 'raw 2', 'active')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );

    const provenance = std.fmt.allocPrint(a, "{d},{d}", .{ raw1, raw2 }) catch @panic("OOM");
    defer a.free(provenance);

    const capsule_id = try d.execParams(
        \\insert into context_records (run_id, stage, session_id, claim_id, kind, body, status, compiled_from)
        \\values (?, 'plan', ?, ?, 'capsule', 'compiled summary', 'active', ?)
    , &.{
        .{ .int = run_id },
        .{ .int = session_id },
        .{ .int = claim_id },
        .{ .text = provenance },
    });

    var stmt = try d.prepare("select compiled_from from context_records where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = capsule_id }});
    try testing.expect(try stmt.step() == .row);
    try testing.expect(!stmt.columnIsNull(0));
    const cf = try stmt.columnTextAlloc(0, a);
    defer a.free(cf);
    try testing.expectEqualStrings(provenance, cf);
}

test "context add: schema rejects invalid kind via CHECK constraint" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    const session_id = try seedSession(&d);
    const task_id = try seedTask(&d);
    const run_id = try seedWorkflowRun(&d, plan_id);
    const claim_id = try d.execParams(
        \\insert into agent_work_claims
        \\  (claim_token, session_id, entity_kind, entity_id, claim_scope, status, vendor, lease_expires_at, run_id)
        \\values ('tok-bad', ?, 'task', ?, 'exclusive', 'active', 'test',
        \\   strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 seconds'), ?)
    , &.{ .{ .int = session_id }, .{ .int = task_id }, .{ .int = run_id } });

    const result = d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'x', ?, ?, 'badkind', 'body', 'active')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );
    try testing.expectError(db.sqlite.Error.StepFailed, result);
}

test "context add: records are append-only (no unique constraint)" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d);
    const session_id = try seedSession(&d);
    const task_id = try seedTask(&d);
    const run_id = try seedWorkflowRun(&d, plan_id);
    const claim_id = try d.execParams(
        \\insert into agent_work_claims
        \\  (claim_token, session_id, entity_kind, entity_id, claim_scope, status, vendor, lease_expires_at, run_id)
        \\values ('tok-dup', ?, 'task', ?, 'exclusive', 'active', 'test',
        \\   strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 seconds'), ?)
    , &.{ .{ .int = session_id }, .{ .int = task_id }, .{ .int = run_id } });

    // Two identical inserts must both succeed (no uniqueness constraint per Q599).
    _ = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 's', ?, ?, 'finding', 'dup body', 'active')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );
    _ = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 's', ?, ?, 'finding', 'dup body', 'active')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );

    var stmt = try d.prepare("select count(*) from context_records where kind = 'finding' and body = 'dup body'");
    defer stmt.finalize();
    _ = try stmt.step();
    try testing.expectEqual(@as(i64, 2), stmt.columnInt(0));
}
