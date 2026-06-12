//! handlers/context/list — `planar-agent context list` verb.
//!
//! Filtered read of `context_records`. Requires `--run <id>`;
//! optional `--stage`, `--status`, `--kind` narrow the result set.
//! Output is always JSON (the JSON array shape is the only consumer-facing
//! read surface for context records).
//!
//! JSON output: { ok, records: [...] }.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const runtime = @import("runtime");

const main = @import("../../main.zig");
const exit = @import("../../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "list",
    .desc = "List context_records for a run, with optional stage/status/kind filters.",
    .flags = &.{
        .{ .long = "--run", .kind = .string, .required = true, .desc = "Run id (integer) to query" },
        .{ .long = "--stage", .kind = .string, .required = false, .desc = "Filter to records from this stage" },
        .{ .long = "--status", .kind = .string, .required = false, .desc = "Filter by status: active|consumed|superseded" },
        .{ .long = "--kind", .kind = .string, .required = false, .desc = "Filter by kind: finding|risk|artifact|followup|summary|capsule" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "context", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const run_id = std.fmt.parseInt(i64, args.run, 10) catch
        exit.die(ctx, error.InvalidInput, "invalid --run '{s}': expected integer run id", .{args.run});

    const has_stage = args.stage != null and args.stage.?.len > 0;
    const has_status = args.status != null and args.status.?.len > 0;
    const has_kind = args.kind != null and args.kind.?.len > 0;

    // Build SQL and params dynamically into fixed-size buffers.
    // Maximum of 4 WHERE clauses: run_id + stage + status + kind.
    var params: [4]db.sqlite.Param = undefined;
    var param_count: usize = 0;

    // Fixed SQL buffer: base query + up to 4 WHERE conditions + ORDER.
    var sql_buf: [1024]u8 = undefined;
    var pos: usize = 0;

    const base =
        "select id, run_id, stage, session_id, claim_id, kind, body, status, compiled_from, created_at " ++
        "from context_records where run_id = ?";
    @memcpy(sql_buf[0..base.len], base);
    pos = base.len;

    params[param_count] = .{ .int = run_id };
    param_count += 1;

    if (has_stage) {
        const clause = " and stage = ?";
        @memcpy(sql_buf[pos .. pos + clause.len], clause);
        pos += clause.len;
        params[param_count] = .{ .text = args.stage.? };
        param_count += 1;
    }
    if (has_status) {
        const clause = " and status = ?";
        @memcpy(sql_buf[pos .. pos + clause.len], clause);
        pos += clause.len;
        params[param_count] = .{ .text = args.status.? };
        param_count += 1;
    }
    if (has_kind) {
        const clause = " and kind = ?";
        @memcpy(sql_buf[pos .. pos + clause.len], clause);
        pos += clause.len;
        params[param_count] = .{ .text = args.kind.? };
        param_count += 1;
    }

    const order = " order by id asc";
    @memcpy(sql_buf[pos .. pos + order.len], order);
    pos += order.len;
    sql_buf[pos] = 0;

    const sql_z: [:0]const u8 = sql_buf[0..pos :0];

    var stmt = d.prepare(sql_z) catch |e|
        exit.die(ctx, e, "prepare context list: {s}", .{@errorName(e)});
    defer stmt.finalize();

    stmt.bind(params[0..param_count]) catch |e|
        exit.die(ctx, e, "bind context list params: {s}", .{@errorName(e)});

    const w = ctx.stdout;
    try w.writeAll("{\"ok\":true,\"records\":[");
    var first = true;

    while (true) {
        const step = stmt.step() catch |e|
            exit.die(ctx, e, "step context list: {s}", .{@errorName(e)});
        if (step == .done) break;

        const rec_id = stmt.columnInt(0);
        const rec_run_id = stmt.columnInt(1);
        const rec_stage = stmt.columnTextAlloc(2, ctx.allocator) catch |e|
            exit.die(ctx, e, "read stage: {s}", .{@errorName(e)});
        defer ctx.allocator.free(rec_stage);
        const rec_session_id = stmt.columnInt(3);
        const rec_claim_id = stmt.columnInt(4);
        const rec_kind = stmt.columnTextAlloc(5, ctx.allocator) catch |e|
            exit.die(ctx, e, "read kind: {s}", .{@errorName(e)});
        defer ctx.allocator.free(rec_kind);
        const rec_body = stmt.columnTextAlloc(6, ctx.allocator) catch |e|
            exit.die(ctx, e, "read body: {s}", .{@errorName(e)});
        defer ctx.allocator.free(rec_body);
        const rec_status = stmt.columnTextAlloc(7, ctx.allocator) catch |e|
            exit.die(ctx, e, "read status: {s}", .{@errorName(e)});
        defer ctx.allocator.free(rec_status);
        const rec_compiled_from = stmt.columnTextOpt(8, ctx.allocator) catch |e|
            exit.die(ctx, e, "read compiled_from: {s}", .{@errorName(e)});
        defer if (rec_compiled_from) |cf| ctx.allocator.free(cf);
        const rec_created_at = stmt.columnTextAlloc(9, ctx.allocator) catch |e|
            exit.die(ctx, e, "read created_at: {s}", .{@errorName(e)});
        defer ctx.allocator.free(rec_created_at);

        if (!first) try w.writeAll(",");
        first = false;

        try w.print(
            "{{\"id\":{d}," ++
                "\"run_id\":{d}," ++
                "\"stage\":\"{s}\"," ++
                "\"session_id\":{d}," ++
                "\"claim_id\":{d}," ++
                "\"kind\":\"{s}\"," ++
                "\"body\":",
            .{ rec_id, rec_run_id, rec_stage, rec_session_id, rec_claim_id, rec_kind },
        );
        // body may contain arbitrary text — use JSON string encoding.
        try writeJsonString(w, rec_body);
        try w.print(
            ",\"status\":\"{s}\"" ++
                ",\"compiled_from\":",
            .{rec_status},
        );
        if (rec_compiled_from) |cf| {
            try w.writeAll("\"");
            try w.writeAll(cf);
            try w.writeAll("\"");
        } else {
            try w.writeAll("null");
        }
        try w.print(",\"created_at\":\"{s}\"}}", .{rec_created_at});
    }

    try w.writeAll("]}\n");
}

/// Write a JSON-safe string (double-quoted, with control characters escaped).
fn writeJsonString(w: anytype, s: []const u8) !void {
    try w.writeAll("\"");
    for (s) |c| {
        switch (c) {
            '"' => try w.writeAll("\\\""),
            '\\' => try w.writeAll("\\\\"),
            '\n' => try w.writeAll("\\n"),
            '\r' => try w.writeAll("\\r"),
            '\t' => try w.writeAll("\\t"),
            else => try w.writeByte(c),
        }
    }
    try w.writeAll("\"");
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

test "writeJsonString escapes special characters" {
    var buf: [256]u8 = undefined;
    var out_len: usize = 0;
    const Writer = struct {
        slice: *[256]u8,
        len: *usize,
        pub fn writeAll(self: @This(), s: []const u8) !void {
            @memcpy(self.slice[self.len.* .. self.len.* + s.len], s);
            self.len.* += s.len;
        }
        pub fn writeByte(self: @This(), c: u8) !void {
            self.slice[self.len.*] = c;
            self.len.* += 1;
        }
    };
    const w = Writer{ .slice = &buf, .len = &out_len };
    try writeJsonString(w, "hello \"world\"\nnewline\\backslash");
    const got = buf[0..out_len];
    try std.testing.expectEqualStrings(
        "\"hello \\\"world\\\"\\nnewline\\\\backslash\"",
        got,
    );
}

test "context list: filters return correct rows from DB" {
    const testing = std.testing;
    const a = testing.allocator;

    var d = try setupTestDb(a);
    defer d.close();

    // Seed plan, session, task, run, claim.
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
        "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) values (?, 'wf', 'run-list-1', 1, '/r')",
        &.{.{ .int = plan_id }},
    );
    const claim_id = try d.execParams(
        \\insert into agent_work_claims
        \\  (claim_token, session_id, entity_kind, entity_id, claim_scope, status, vendor, lease_expires_at, run_id, stage)
        \\values ('tok-list', ?, 'task', ?, 'exclusive', 'active', 'test',
        \\   strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 seconds'), ?, 'plan')
    , &.{ .{ .int = session_id }, .{ .int = task_id }, .{ .int = run_id } });

    // Insert records across two stages.
    _ = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'plan', ?, ?, 'finding', 'f1', 'active')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );
    _ = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'plan', ?, ?, 'risk', 'r1', 'consumed')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );
    _ = try d.execParams(
        "insert into context_records (run_id, stage, session_id, claim_id, kind, body, status) values (?, 'code', ?, ?, 'artifact', 'a1', 'active')",
        &.{ .{ .int = run_id }, .{ .int = session_id }, .{ .int = claim_id } },
    );

    // Assert counts via SQL to verify the schema is correct.
    const total = try d.intQuery("select count(*) from context_records");
    try testing.expectEqual(@as(i64, 3), total);

    const plan_stage = try d.intQuery("select count(*) from context_records where stage = 'plan'");
    try testing.expectEqual(@as(i64, 2), plan_stage);

    const active_only = try d.intQuery("select count(*) from context_records where status = 'active'");
    try testing.expectEqual(@as(i64, 2), active_only);

    const code_artifacts = try d.intQuery("select count(*) from context_records where stage = 'code' and kind = 'artifact'");
    try testing.expectEqual(@as(i64, 1), code_artifacts);
}
