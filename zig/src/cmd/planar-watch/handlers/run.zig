//! handlers/run — `planar-watch run list` / `planar-watch run show <id>`
//!
//! Read-only observability for the `workflow_runs` and `context_records`
//! tables introduced in migration 00022 (workflow context plane), AND for
//! the `runs` table (op/bench-arm runs from `planar run start`).
//!
//! Verb group: `run`
//!   - `list [--plan <id>] [--status <s>] [--arm wf|op|all] [--json]`
//!       Lists runs, newest-first. --arm selects the source table:
//!         wf  — workflow_runs only (context-plane, written by planar-agent
//!               run start; these have context_records in planar-watch run show).
//!         op  — runs table only (op/workflow-arm, written by planar run start;
//!               detail via planar run show <run_uid>).
//!         all — (default) both sources combined; each row carries a "source"
//!               field ("wf" | "op") so callers can route to the right show verb.
//!       --plan <id>   restrict to runs for a given plan.
//!       --status <s>  restrict by run status; default: all.
//!       JSON shape: { generated_at: ISO8601, runs: [RunRow] }
//!
//!   - `show <id> [--json]`
//!       Shows one CONTEXT-PLANE (wf-source) run's full row PLUS its
//!       context_records. For op-source runs use `planar run show <run_uid>`.
//!       JSON shape: { run: RunRow, context_records: [ContextRecord] }
//!
//! Strict read-only: this module never opens a writable DB handle.
//! The DB is obtained via `runtime.ensureDbStrictReadOnly`, which uses
//! `SQLITE_OPEN_READONLY`. Any inadvertent write SQL is rejected at the
//! driver level — the second line of defense behind the capability invariant.
//!
//! JSON shapes are stable and operator-scriptable. Column names mirror the
//! SQL schema exactly (snake_case, same order as the migration).

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const ps = @import("ps.zig");

// ---------------------------------------------------------------------------
// Verb tree
// ---------------------------------------------------------------------------

const list_verb: cli.Cmd = .{
    .name = "list",
    .desc = "List workflow runs (filterable by plan, status, and source arm).",
    .long_desc = "Returns runs ordered by started_at descending.\n\n" ++
        "  --plan <id>    restrict to runs for the given plan.\n" ++
        "  --status <s>   restrict by status: running | completed | failed |\n" ++
        "                 interrupted | abandoned. Default: all.\n" ++
        "  --arm <a>      source table: wf (workflow_runs / context-plane),\n" ++
        "                 op (runs / op-arm), or all (default, both).\n" ++
        "  --json         emit a single JSON object instead of human text.",
    .flags = &.{
        .{ .long = "--plan", .kind = .int, .desc = "Filter by plan id" },
        .{ .long = "--status", .kind = .string, .desc = "Filter by status (default: all)" },
        .{ .long = "--arm", .kind = .string, .desc = "Source arm: wf | op | all (default: all)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handleList),
};

const show_verb: cli.Cmd = .{
    .name = "show",
    .desc = "Show one workflow run plus its context_records grouped by stage.",
    .long_desc = "Returns the full workflow_runs row for <id> plus all\n" ++
        "  context_records for that run, grouped and ordered by\n" ++
        "  stage then created_at.\n\n" ++
        "  Exits non-zero when the run id is unknown.",
    .positionals = &.{
        .{ .name = "id", .desc = "Workflow run id (integer)", .required = true },
    },
    .flags = &.{
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handleShow),
};

pub const verb: cli.Cmd = .{
    .name = "run",
    .desc = "Observe workflow runs and their context records.",
    .long_desc = "Read-only view of run tables. `list` covers both workflow_runs (wf)\n" ++
        "and the runs table (op-arm); `show` drills into wf-source runs only.\n\n" ++
        "  list  — list runs (--plan / --status / --arm filters).\n" ++
        "  show  — drill into one wf-source run's context records.",
    .cmds = &.{ list_verb, show_verb },
};

// ---------------------------------------------------------------------------
// Row types
// ---------------------------------------------------------------------------

/// One run row (from workflow_runs or runs). The `source` field indicates
/// which underlying table the row came from: "wf" for workflow_runs (context-
/// plane; use `planar-watch run show <id>` for detail) or "op" for the runs
/// table (op/workflow-arm; use `planar run show <run_identifier>` for detail).
const RunRow = struct {
    id: i64,
    plan_id: i64,
    workflow_name: []const u8,
    run_identifier: []const u8,
    pid: i64,
    repo_root: []const u8,
    started_at: []const u8,
    ended_at: ?[]const u8,
    status: []const u8,
    /// "wf" (workflow_runs table) or "op" (runs table).
    source: []const u8,

    fn deinit(self: RunRow, allocator: std.mem.Allocator) void {
        allocator.free(self.workflow_name);
        allocator.free(self.run_identifier);
        allocator.free(self.repo_root);
        allocator.free(self.started_at);
        if (self.ended_at) |s| allocator.free(s);
        allocator.free(self.status);
        // source is a string literal — do NOT free it.
        _ = self.source;
    }

    fn deinitMany(rows: []const RunRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
    }
};

/// One context_records row.
const ContextRecord = struct {
    id: i64,
    run_id: i64,
    stage: []const u8,
    session_id: i64,
    claim_id: i64,
    kind: []const u8,
    body: []const u8,
    status: []const u8,
    compiled_from: ?[]const u8,
    created_at: []const u8,

    fn deinit(self: ContextRecord, allocator: std.mem.Allocator) void {
        allocator.free(self.stage);
        allocator.free(self.kind);
        allocator.free(self.body);
        allocator.free(self.status);
        if (self.compiled_from) |s| allocator.free(s);
        allocator.free(self.created_at);
    }

    fn deinitMany(rows: []const ContextRecord, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
    }
};

// ---------------------------------------------------------------------------
// list handler
// ---------------------------------------------------------------------------

fn handleList(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "run", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Validate --arm: wf | op | all (default all).
    const arm_filter = args.arm orelse "all";
    if (!std.mem.eql(u8, arm_filter, "wf") and
        !std.mem.eql(u8, arm_filter, "op") and
        !std.mem.eql(u8, arm_filter, "all"))
    {
        exit.die(ctx, error.InvalidValue, "run list: --arm must be wf, op, or all (got '{s}')", .{arm_filter});
    }

    var all_rows = std.ArrayList(RunRow).empty;
    defer {
        RunRow.deinitMany(all_rows.items, ctx.allocator);
        all_rows.deinit(ctx.allocator);
    }

    // workflow_runs source (wf arm).
    // The returned slice's backing array is freed here; the row VALUE copies
    // (and their heap-allocated string fields) are owned by all_rows.
    if (std.mem.eql(u8, arm_filter, "wf") or std.mem.eql(u8, arm_filter, "all")) {
        const wf_rows = listWorkflowRuns(d, ctx.allocator, args.plan, args.status) catch |e|
            exit.die(ctx, e, "run list: {s}", .{@errorName(e)});
        for (wf_rows) |r| all_rows.append(ctx.allocator, r) catch |e|
            exit.die(ctx, e, "run list: append wf row: {s}", .{@errorName(e)});
        ctx.allocator.free(wf_rows);
    }

    // runs table source (op arm).
    if (std.mem.eql(u8, arm_filter, "op") or std.mem.eql(u8, arm_filter, "all")) {
        const op_rows = listOpRuns(d, ctx.allocator, args.plan, args.status) catch |e|
            exit.die(ctx, e, "run list (op): {s}", .{@errorName(e)});
        for (op_rows) |r| all_rows.append(ctx.allocator, r) catch |e|
            exit.die(ctx, e, "run list: append op row: {s}", .{@errorName(e)});
        ctx.allocator.free(op_rows);
    }

    // Sort combined results by started_at descending (newest first).
    // Simple insertion sort is fine for the small counts expected here.
    const rows = all_rows.items;
    if (rows.len > 1) {
        var i: usize = 1;
        while (i < rows.len) : (i += 1) {
            const key = rows[i];
            var j: usize = i;
            while (j > 0 and std.mem.lessThan(u8, rows[j - 1].started_at, key.started_at)) : (j -= 1) {
                rows[j] = rows[j - 1];
            }
            rows[j] = key;
        }
    }

    if (args.json) {
        try ctx.stdout.print("{{\"generated_at\":", .{});
        try ps.writeNowIso(ctx.stdout);
        try ctx.stdout.print(",\"runs\":[", .{});
        var first = true;
        for (rows) |r| {
            if (!first) try ctx.stdout.print(",", .{});
            first = false;
            try writeRunJson(ctx.stdout, r);
        }
        try ctx.stdout.print("]}}\n", .{});
    } else {
        try ctx.stdout.print("runs: {d}\n", .{rows.len});
        for (rows) |r| {
            const ended = r.ended_at orelse "-";
            try ctx.stdout.print(
                "  run:{d}  plan:{d}  status:{s}  source:{s}  started:{s}  ended:{s}  workflow:{s}\n",
                .{ r.id, r.plan_id, r.status, r.source, r.started_at, ended, r.workflow_name },
            );
        }
    }
    try ctx.stdout.flush();
}

// ---------------------------------------------------------------------------
// show handler
// ---------------------------------------------------------------------------

fn handleShow(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "run", "show" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const run_id = std.fmt.parseInt(i64, args.id, 10) catch
        exit.die(ctx, error.InvalidInput, "run show: id must be an integer", .{});

    const run = fetchRun(d, ctx.allocator, run_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "run show: run {d} not found", .{run_id}),
        else => exit.die(ctx, e, "run show: {s}", .{@errorName(e)}),
    };
    defer run.deinit(ctx.allocator);

    const records = listContextRecords(d, ctx.allocator, run_id) catch |e|
        exit.die(ctx, e, "run show: context_records: {s}", .{@errorName(e)});
    defer ContextRecord.deinitMany(records, ctx.allocator);

    if (args.json) {
        try ctx.stdout.print("{{\"run\":", .{});
        try writeRunJson(ctx.stdout, run);
        try ctx.stdout.print(",\"context_records\":[", .{});
        var first = true;
        for (records) |rec| {
            if (!first) try ctx.stdout.print(",", .{});
            first = false;
            try writeContextRecordJson(ctx.stdout, rec);
        }
        try ctx.stdout.print("]}}\n", .{});
    } else {
        // Human format: run summary, then records grouped by stage.
        const ended = run.ended_at orelse "-";
        try ctx.stdout.print(
            "run:{d}  plan:{d}  status:{s}  pid:{d}  workflow:{s}\n" ++
                "  started:{s}  ended:{s}\n" ++
                "  identifier:{s}\n" ++
                "  repo_root:{s}\n",
            .{
                run.id,         run.plan_id, run.status,         run.pid,       run.workflow_name,
                run.started_at, ended,       run.run_identifier, run.repo_root,
            },
        );
        if (records.len == 0) {
            try ctx.stdout.print("  (no context records)\n", .{});
        } else {
            try ctx.stdout.print("context_records: {d}\n", .{records.len});
            var cur_stage: ?[]const u8 = null;
            for (records) |rec| {
                if (cur_stage == null or !std.mem.eql(u8, cur_stage.?, rec.stage)) {
                    cur_stage = rec.stage;
                    try ctx.stdout.print("  [stage: {s}]\n", .{rec.stage});
                }
                // Body preview: first 80 bytes, truncated with ellipsis.
                const preview_limit = 80;
                const body_preview = if (rec.body.len <= preview_limit) rec.body else rec.body[0..preview_limit];
                const ellipsis: []const u8 = if (rec.body.len > preview_limit) "…" else "";
                try ctx.stdout.print(
                    "    record:{d}  kind:{s}  status:{s}  created:{s}\n" ++
                        "      body: {s}{s}\n",
                    .{
                        rec.id,       rec.kind, rec.status, rec.created_at,
                        body_preview, ellipsis,
                    },
                );
                if (rec.compiled_from) |cf| {
                    try ctx.stdout.print("      compiled_from: {s}\n", .{cf});
                }
            }
        }
    }
    try ctx.stdout.flush();
}

// ---------------------------------------------------------------------------
// SQL helpers
// ---------------------------------------------------------------------------

/// Query workflow_runs (context-plane / wf-arm rows).
fn listWorkflowRuns(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_filter: ?i64,
    status_filter: ?[]const u8,
) ![]RunRow {
    // Build the SQL dynamically based on which filters are set.
    var sql_buf: [768]u8 = undefined;
    var where_buf: [256]u8 = undefined;
    var where_len: usize = 0;

    if (plan_filter != null and status_filter != null) {
        const clause = "where plan_id = ? and status = ?";
        @memcpy(where_buf[0..clause.len], clause);
        where_len = clause.len;
    } else if (plan_filter != null) {
        const clause = "where plan_id = ?";
        @memcpy(where_buf[0..clause.len], clause);
        where_len = clause.len;
    } else if (status_filter != null) {
        const clause = "where status = ?";
        @memcpy(where_buf[0..clause.len], clause);
        where_len = clause.len;
    }

    const sql = try std.fmt.bufPrintZ(&sql_buf,
        \\select id, plan_id, workflow_name, run_identifier, pid,
        \\       repo_root, started_at, ended_at, status
        \\from workflow_runs
        \\{s}
        \\order by started_at desc, id desc
    , .{where_buf[0..where_len]});

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();

    if (plan_filter != null and status_filter != null) {
        stmt.bind(&.{ .{ .int = plan_filter.? }, .{ .text = status_filter.? } }) catch return error.QueryFailed;
    } else if (plan_filter != null) {
        stmt.bind(&.{.{ .int = plan_filter.? }}) catch return error.QueryFailed;
    } else if (status_filter != null) {
        stmt.bind(&.{.{ .text = status_filter.? }}) catch return error.QueryFailed;
    }

    var out: std.ArrayList(RunRow) = .empty;
    errdefer {
        RunRow.deinitMany(out.items, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readWorkflowRunRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query the runs table (op/workflow-arm rows written by `planar run start`).
/// Maps runs columns to RunRow: arm→workflow_name, run_uid→run_identifier,
/// pid=0 (sentinel), repo_root="" (not stored). source="op".
fn listOpRuns(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_filter: ?i64,
    status_filter: ?[]const u8,
) ![]RunRow {
    var sql_buf: [768]u8 = undefined;
    var where_buf: [256]u8 = undefined;
    var where_len: usize = 0;

    if (plan_filter != null and status_filter != null) {
        const clause = "where plan_id = ? and status = ?";
        @memcpy(where_buf[0..clause.len], clause);
        where_len = clause.len;
    } else if (plan_filter != null) {
        const clause = "where plan_id = ?";
        @memcpy(where_buf[0..clause.len], clause);
        where_len = clause.len;
    } else if (status_filter != null) {
        const clause = "where status = ?";
        @memcpy(where_buf[0..clause.len], clause);
        where_len = clause.len;
    }

    const sql = try std.fmt.bufPrintZ(&sql_buf,
        \\select id, plan_id, arm, run_uid,
        \\       started_at, ended_at, status
        \\from runs
        \\{s}
        \\order by started_at desc, id desc
    , .{where_buf[0..where_len]});

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();

    if (plan_filter != null and status_filter != null) {
        stmt.bind(&.{ .{ .int = plan_filter.? }, .{ .text = status_filter.? } }) catch return error.QueryFailed;
    } else if (plan_filter != null) {
        stmt.bind(&.{.{ .int = plan_filter.? }}) catch return error.QueryFailed;
    } else if (status_filter != null) {
        stmt.bind(&.{.{ .text = status_filter.? }}) catch return error.QueryFailed;
    }

    var out: std.ArrayList(RunRow) = .empty;
    errdefer {
        RunRow.deinitMany(out.items, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const workflow_name = try stmt.columnTextAlloc(2, allocator);
                const run_uid = try stmt.columnTextAlloc(3, allocator);
                const started_at = try stmt.columnTextAlloc(4, allocator);
                const ended_at = try stmt.columnTextOpt(5, allocator);
                const status = try stmt.columnTextAlloc(6, allocator);
                const repo_root = try allocator.dupe(u8, "");
                try out.append(allocator, .{
                    .id = stmt.columnInt(0),
                    .plan_id = stmt.columnInt(1),
                    .workflow_name = workflow_name,
                    .run_identifier = run_uid,
                    .pid = 0,
                    .repo_root = repo_root,
                    .started_at = started_at,
                    .ended_at = ended_at,
                    .status = status,
                    .source = "op",
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn fetchRun(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    run_id: i64,
) !RunRow {
    var stmt = d.prepare(
        \\select id, plan_id, workflow_name, run_identifier, pid,
        \\       repo_root, started_at, ended_at, status
        \\from workflow_runs
        \\where id = ?
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = run_id }}) catch return error.QueryFailed;
    switch (stmt.step() catch return error.QueryFailed) {
        .done => return error.NotFound,
        .row => return try readWorkflowRunRow(&stmt, allocator),
    }
}

fn readWorkflowRunRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) !RunRow {
    return .{
        .id = stmt.columnInt(0),
        .plan_id = stmt.columnInt(1),
        .workflow_name = try stmt.columnTextAlloc(2, allocator),
        .run_identifier = try stmt.columnTextAlloc(3, allocator),
        .pid = stmt.columnInt(4),
        .repo_root = try stmt.columnTextAlloc(5, allocator),
        .started_at = try stmt.columnTextAlloc(6, allocator),
        .ended_at = try stmt.columnTextOpt(7, allocator),
        .status = try stmt.columnTextAlloc(8, allocator),
        .source = "wf",
    };
}

fn listContextRecords(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    run_id: i64,
) ![]ContextRecord {
    var stmt = d.prepare(
        \\select id, run_id, stage, session_id, claim_id,
        \\       kind, body, status, compiled_from, created_at
        \\from context_records
        \\where run_id = ?
        \\order by stage asc, created_at asc, id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = run_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(ContextRecord) = .empty;
    errdefer {
        ContextRecord.deinitMany(out.items, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readContextRecordRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn readContextRecordRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) !ContextRecord {
    return .{
        .id = stmt.columnInt(0),
        .run_id = stmt.columnInt(1),
        .stage = try stmt.columnTextAlloc(2, allocator),
        .session_id = stmt.columnInt(3),
        .claim_id = stmt.columnInt(4),
        .kind = try stmt.columnTextAlloc(5, allocator),
        .body = try stmt.columnTextAlloc(6, allocator),
        .status = try stmt.columnTextAlloc(7, allocator),
        .compiled_from = try stmt.columnTextOpt(8, allocator),
        .created_at = try stmt.columnTextAlloc(9, allocator),
    };
}

// ---------------------------------------------------------------------------
// JSON writers
// ---------------------------------------------------------------------------

fn writeRunJson(w: *std.Io.Writer, r: RunRow) !void {
    try w.print("{{\"id\":{d}", .{r.id});
    try w.print(",\"plan_id\":{d}", .{r.plan_id});
    try w.print(",\"workflow_name\":", .{});
    try std.json.Stringify.encodeJsonString(r.workflow_name, .{}, w);
    try w.print(",\"run_identifier\":", .{});
    try std.json.Stringify.encodeJsonString(r.run_identifier, .{}, w);
    try w.print(",\"pid\":{d}", .{r.pid});
    try w.print(",\"repo_root\":", .{});
    try std.json.Stringify.encodeJsonString(r.repo_root, .{}, w);
    try w.print(",\"started_at\":", .{});
    try std.json.Stringify.encodeJsonString(r.started_at, .{}, w);
    if (r.ended_at) |s| {
        try w.print(",\"ended_at\":", .{});
        try std.json.Stringify.encodeJsonString(s, .{}, w);
    } else {
        try w.print(",\"ended_at\":null", .{});
    }
    try w.print(",\"status\":", .{});
    try std.json.Stringify.encodeJsonString(r.status, .{}, w);
    try w.print(",\"source\":", .{});
    try std.json.Stringify.encodeJsonString(r.source, .{}, w);
    try w.print("}}", .{});
}

fn writeContextRecordJson(w: *std.Io.Writer, r: ContextRecord) !void {
    try w.print("{{\"id\":{d}", .{r.id});
    try w.print(",\"run_id\":{d}", .{r.run_id});
    try w.print(",\"stage\":", .{});
    try std.json.Stringify.encodeJsonString(r.stage, .{}, w);
    try w.print(",\"session_id\":{d}", .{r.session_id});
    try w.print(",\"claim_id\":{d}", .{r.claim_id});
    try w.print(",\"kind\":", .{});
    try std.json.Stringify.encodeJsonString(r.kind, .{}, w);
    try w.print(",\"body\":", .{});
    try std.json.Stringify.encodeJsonString(r.body, .{}, w);
    try w.print(",\"status\":", .{});
    try std.json.Stringify.encodeJsonString(r.status, .{}, w);
    if (r.compiled_from) |cf| {
        try w.print(",\"compiled_from\":", .{});
        try std.json.Stringify.encodeJsonString(cf, .{}, w);
    } else {
        try w.print(",\"compiled_from\":null", .{});
    }
    try w.print(",\"created_at\":", .{});
    try std.json.Stringify.encodeJsonString(r.created_at, .{}, w);
    try w.print("}}", .{});
}
