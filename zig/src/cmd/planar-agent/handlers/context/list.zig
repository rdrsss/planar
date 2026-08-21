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

        try w.print("{{\"id\":{d},\"run_id\":{d},\"stage\":", .{ rec_id, rec_run_id });
        try writeJsonString(w, rec_stage);
        try w.print(",\"session_id\":{d},\"claim_id\":{d},\"kind\":", .{ rec_session_id, rec_claim_id });
        try writeJsonString(w, rec_kind);
        try w.writeAll(",\"body\":");
        // body may contain arbitrary text -- use JSON string encoding.
        try writeJsonString(w, rec_body);
        try w.writeAll(",\"status\":");
        try writeJsonString(w, rec_status);
        try w.writeAll(",\"compiled_from\":");
        if (rec_compiled_from) |cf| {
            try writeJsonString(w, cf);
        } else {
            try w.writeAll("null");
        }
        try w.writeAll(",\"created_at\":");
        try writeJsonString(w, rec_created_at);
        try w.writeByte('}');
    }

    try w.writeAll("]}\n");
}

/// Write a JSON-safe string (double-quoted, with control characters escaped).
///
/// Covers the full C0 control range (< 0x20) plus the two structural
/// characters that must always be escaped in a JSON string value:
///   - Backspace    (0x08) -> \b
///   - Form feed    (0x0C) -> \f
///   - Newline      (0x0A) -> \n
///   - Carriage ret (0x0D) -> \r
///   - Tab          (0x09) -> \t
///   - Other C0     (< 0x20, excluding the above) -> \uXXXX
///   - Backslash    (0x5C) -> \\
///   - Double quote (0x22) -> \"
fn writeJsonString(w: anytype, s: []const u8) !void {
    try w.writeAll("\"");
    for (s) |c| {
        switch (c) {
            '"' => try w.writeAll("\\\""),
            '\\' => try w.writeAll("\\\\"),
            '\x08' => try w.writeAll("\\b"),
            '\x0C' => try w.writeAll("\\f"),
            '\n' => try w.writeAll("\\n"),
            '\r' => try w.writeAll("\\r"),
            '\t' => try w.writeAll("\\t"),
            0x00...0x07, 0x0B, 0x0E...0x1F => {
                // Remaining C0 control characters: emit \uXXXX.
                var buf: [7]u8 = undefined;
                const escape = std.fmt.bufPrint(&buf, "\\u{X:0>4}", .{c}) catch unreachable;
                try w.writeAll(escape);
            },
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

// Minimal writer for unit-testing writeJsonString without a full runtime.
const TestWriter = struct {
    slice: *[512]u8,
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

test "writeJsonString escapes special characters" {
    var buf: [512]u8 = undefined;
    var out_len: usize = 0;
    const w = TestWriter{ .slice = &buf, .len = &out_len };
    try writeJsonString(w, "hello \"world\"\nnewline\\backslash");
    const got = buf[0..out_len];
    try std.testing.expectEqualStrings(
        "\"hello \\\"world\\\"\\nnewline\\\\backslash\"",
        got,
    );
}

test "writeJsonString escapes full C0 control range" {
    // Test each named escape and the generic \uXXXX fallback.
    var buf: [512]u8 = undefined;

    // backspace (0x08) -> \b
    var len: usize = 0;
    try writeJsonString(TestWriter{ .slice = &buf, .len = &len }, "\x08");
    try std.testing.expectEqualStrings("\"\\b\"", buf[0..len]);

    // form feed (0x0C) -> \f
    len = 0;
    try writeJsonString(TestWriter{ .slice = &buf, .len = &len }, "\x0C");
    try std.testing.expectEqualStrings("\"\\f\"", buf[0..len]);

    // newline (0x0A) -> \n
    len = 0;
    try writeJsonString(TestWriter{ .slice = &buf, .len = &len }, "\n");
    try std.testing.expectEqualStrings("\"\\n\"", buf[0..len]);

    // carriage return (0x0D) -> \r
    len = 0;
    try writeJsonString(TestWriter{ .slice = &buf, .len = &len }, "\r");
    try std.testing.expectEqualStrings("\"\\r\"", buf[0..len]);

    // tab (0x09) -> \t
    len = 0;
    try writeJsonString(TestWriter{ .slice = &buf, .len = &len }, "\t");
    try std.testing.expectEqualStrings("\"\\t\"", buf[0..len]);

    // NUL (0x00) -> 0x00
    len = 0;
    try writeJsonString(TestWriter{ .slice = &buf, .len = &len }, "\x00");
    try std.testing.expectEqualStrings("\"\\u0000\"", buf[0..len]);

    // SOH (0x01) -> 0x01
    len = 0;
    try writeJsonString(TestWriter{ .slice = &buf, .len = &len }, "\x01");
    try std.testing.expectEqualStrings("\"\\u0001\"", buf[0..len]);

    // US (0x1F) -> 0x1F
    len = 0;
    try writeJsonString(TestWriter{ .slice = &buf, .len = &len }, "\x1F");
    try std.testing.expectEqualStrings("\"\\u001F\"", buf[0..len]);

    // Verify a body containing a mix of control chars produces valid JSON-escaped output.
    len = 0;
    try writeJsonString(TestWriter{ .slice = &buf, .len = &len }, "a\x01b\x1Fc");
    try std.testing.expectEqualStrings("\"a\\u0001b\\u001Fc\"", buf[0..len]);
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
