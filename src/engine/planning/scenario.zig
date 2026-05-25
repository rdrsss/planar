//! engine/planning/scenario — Test scenario entity: CRUD + verify / retire.
//!
//! Status set: {draft, ready, verified, failing, retired}. The schema
//! also tracks `last_run_at` + `last_outcome` independently of status —
//! a run is recorded every time the scenario is executed, even if it
//! doesn't change status (e.g. a failing scenario re-run that still
//! fails). `verify` is the explicit "passed — flip to verified"
//! transition; `retire` is the "stop running this one" terminal state.
//!
//! Scope handling: when `scope` is provided in CreateArgs/ListFilter,
//! it is resolved via engine.identity.scope.resolveSlug. See plan.zig
//! for the full design rationale.

const std = @import("std");
const db = @import("db");
const identity = @import("../identity.zig");
const policy = @import("../policy.zig");

// =========================================================================
// Types
// =========================================================================

pub const ScopeKind = enum {
    repo,
    association,
    global,

    pub fn fromText(s: []const u8) ?ScopeKind {
        if (std.mem.eql(u8, s, "repo")) return .repo;
        if (std.mem.eql(u8, s, "association")) return .association;
        if (std.mem.eql(u8, s, "global")) return .global;
        return null;
    }
};

pub const Status = enum {
    draft,
    ready,
    verified,
    failing,
    retired,

    pub fn fromText(s: []const u8) ?Status {
        if (std.mem.eql(u8, s, "draft")) return .draft;
        if (std.mem.eql(u8, s, "ready")) return .ready;
        if (std.mem.eql(u8, s, "verified")) return .verified;
        if (std.mem.eql(u8, s, "failing")) return .failing;
        if (std.mem.eql(u8, s, "retired")) return .retired;
        return null;
    }
};

pub const Outcome = enum {
    pass,
    fail,
    @"error",
    skipped,

    pub fn fromText(s: []const u8) ?Outcome {
        if (std.mem.eql(u8, s, "pass")) return .pass;
        if (std.mem.eql(u8, s, "fail")) return .fail;
        if (std.mem.eql(u8, s, "error")) return .@"error";
        if (std.mem.eql(u8, s, "skipped")) return .skipped;
        return null;
    }
};

pub const Scenario = struct {
    id: i64,
    scope_kind: ScopeKind,
    scope_id: ?i64,
    title: []const u8,
    body: ?[]const u8,
    status: Status,
    related_artifact_id: ?i64,
    last_run_at: ?[]const u8,
    last_outcome: ?Outcome,
    created_at: []const u8,
    updated_at: []const u8,
};

pub fn deinit(s: Scenario, allocator: std.mem.Allocator) void {
    allocator.free(s.title);
    if (s.body) |t| allocator.free(t);
    if (s.last_run_at) |t| allocator.free(t);
    allocator.free(s.created_at);
    allocator.free(s.updated_at);
}

pub fn deinitMany(items: []const Scenario, allocator: std.mem.Allocator) void {
    for (items) |s| deinit(s, allocator);
    allocator.free(items);
}

pub const CreateArgs = struct {
    title: []const u8,
    body: ?[]const u8 = null,
    related_artifact_id: ?i64 = null,
    /// `--plan` on the CLI doesn't map to a column on test_scenarios
    /// (the link is via entity_links). Accepted by the handler and
    /// stored for future use; currently ignored by the engine.
    plan_id: ?i64 = null,
    scope: ?[]const u8 = null,
};

pub const ListFilter = struct {
    statuses: []const Status = &.{},
    related_artifact_id: ?i64 = null,
    scope: ?[]const u8 = null,
    scopes: []const []const u8 = &.{},
};

pub const Error =
    error{
        NotFound,
        UnsupportedScope,
        SlugNotFound,
        QueryFailed,
    } ||
    std.mem.Allocator.Error ||
    policy.scope_guard.Error ||
    policy.status.Error ||
    policy.audit.Error;

// =========================================================================
// CRUD
// =========================================================================

pub fn create(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: CreateArgs) Error!Scenario {
    const scope_ref = if (args.scope) |s|
        identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        }
    else
        identity.scope.ScopeRef{ .kind = .global, .id = null };

    const scope_kind_str: []const u8 = switch (scope_ref.kind) {
        .global => "global",
        .association => "association",
        .repo => "repo",
    };

    try policy.scope_guard.check(null, null);

    const id = d.execParams(
        \\insert into test_scenarios (scope_kind, scope_id, title, body, related_artifact_id)
        \\values (?, ?, ?, ?, ?)
    , &.{
        .{ .text = scope_kind_str },
        if (scope_ref.id) |sid| .{ .int = sid } else .{ .null = {} },
        .{ .text = args.title },
        if (args.body) |s| .{ .text = s } else .{ .null = {} },
        if (args.related_artifact_id) |a| .{ .int = a } else .{ .null = {} },
    }) catch |e| {
        std.log.err("scenario.create exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const summary = try std.fmt.allocPrint(allocator, "create scenario '{s}'", .{args.title});
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "scenario", .id = id },
        .summary = summary,
    });

    // --plan currently has no schema column to land on; warn-and-ignore
    // is the handler's job. Document intent here for the future
    // entity_links wiring.
    _ = args.plan_id;

    return try show(d, allocator, id);
}

pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Scenario {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: ListFilter) Error![]Scenario {
    var scope_refs: std.ArrayList(identity.scope.ScopeRef) = .empty;
    defer scope_refs.deinit(allocator);
    if (filter.scope) |s| {
        try scope_refs.append(allocator, identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        });
    }
    for (filter.scopes) |s| {
        try scope_refs.append(allocator, identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        });
    }

    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, select_all_prefix);

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);
    if (filter.statuses.len > 0) {
        try sql_buf.appendSlice(allocator, " and status in (");
        for (filter.statuses, 0..) |s, i| {
            if (i > 0) try sql_buf.appendSlice(allocator, ",");
            try sql_buf.appendSlice(allocator, "?");
            try params.append(allocator, .{ .text = @tagName(s) });
        }
        try sql_buf.appendSlice(allocator, ")");
    }
    if (filter.related_artifact_id) |aid| {
        try sql_buf.appendSlice(allocator, " and related_artifact_id = ?");
        try params.append(allocator, .{ .int = aid });
    }
    if (scope_refs.items.len > 0) {
        try sql_buf.appendSlice(allocator, " and (");
        for (scope_refs.items, 0..) |ref, i| {
            if (i > 0) try sql_buf.appendSlice(allocator, " or ");
            switch (ref.kind) {
                .global => try sql_buf.appendSlice(allocator, "scope_kind='global'"),
                .association => {
                    try sql_buf.appendSlice(allocator, "(scope_kind='association' and scope_id=?)");
                    try params.append(allocator, .{ .int = ref.id.? });
                },
                .repo => {
                    try sql_buf.appendSlice(allocator, "(scope_kind='repo' and scope_id=?)");
                    try params.append(allocator, .{ .int = ref.id.? });
                },
            }
        }
        try sql_buf.appendSlice(allocator, ")");
    }
    try sql_buf.appendSlice(allocator, " order by id");

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var out: std.ArrayList(Scenario) = .empty;
    errdefer {
        for (out.items) |s| deinit(s, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn listTouching(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    repo_id: i64,
    filter: ListFilter,
) Error![]Scenario {
    var scope_refs: std.ArrayList(identity.scope.ScopeRef) = .empty;
    defer scope_refs.deinit(allocator);
    if (filter.scope) |s| {
        try scope_refs.append(allocator, identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        });
    }
    for (filter.scopes) |s| {
        try scope_refs.append(allocator, identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        });
    }

    var branch1_active = true;
    if (scope_refs.items.len > 0) {
        branch1_active = false;
        for (scope_refs.items) |ref| {
            if (ref.kind != .repo) continue;
            if (ref.id == null or ref.id.? == repo_id) {
                branch1_active = true;
                break;
            }
        }
    }

    var sql: std.ArrayList(u8) = .empty;
    defer sql.deinit(allocator);
    try sql.appendSlice(allocator, "select * from (");
    if (branch1_active) {
        try sql.appendSlice(allocator, select_all_prefix);
        try sql.appendSlice(allocator, " and scope_kind='repo' and scope_id=?");
        if (filter.statuses.len > 0) {
            try sql.appendSlice(allocator, " and status in (");
            for (filter.statuses, 0..) |_, i| {
                if (i > 0) try sql.appendSlice(allocator, ",");
                try sql.appendSlice(allocator, "?");
            }
            try sql.appendSlice(allocator, ")");
        }
        if (filter.related_artifact_id) |_| try sql.appendSlice(allocator, " and related_artifact_id=?");
    } else {
        try sql.appendSlice(allocator, select_all_prefix);
        try sql.appendSlice(allocator, " and 1=0");
    }

    try sql.appendSlice(allocator, " union ");
    try sql.appendSlice(allocator, select_all_prefix);
    try sql.appendSlice(allocator,
        " and id in (select from_id from entity_links where from_kind='test_scenario' and to_kind='repo' and to_id=? and relationship='touches')");
    if (filter.statuses.len > 0) {
        try sql.appendSlice(allocator, " and status in (");
        for (filter.statuses, 0..) |_, i| {
            if (i > 0) try sql.appendSlice(allocator, ",");
            try sql.appendSlice(allocator, "?");
        }
        try sql.appendSlice(allocator, ")");
    }
    if (filter.related_artifact_id) |_| try sql.appendSlice(allocator, " and related_artifact_id=?");
    if (scope_refs.items.len > 0) {
        try sql.appendSlice(allocator, " and (");
        for (scope_refs.items, 0..) |ref, i| {
            if (i > 0) try sql.appendSlice(allocator, " or ");
            switch (ref.kind) {
                .global => try sql.appendSlice(allocator, "scope_kind='global'"),
                .association => try sql.appendSlice(allocator, "(scope_kind='association' and scope_id=?)"),
                .repo => try sql.appendSlice(allocator, "(scope_kind='repo' and scope_id=?)"),
            }
        }
        try sql.appendSlice(allocator, ")");
    }
    try sql.appendSlice(allocator, ") order by id");

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);
    if (branch1_active) {
        try params.append(allocator, .{ .int = repo_id });
        for (filter.statuses) |s| try params.append(allocator, .{ .text = @tagName(s) });
        if (filter.related_artifact_id) |aid| try params.append(allocator, .{ .int = aid });
    }
    try params.append(allocator, .{ .int = repo_id });
    for (filter.statuses) |s| try params.append(allocator, .{ .text = @tagName(s) });
    if (filter.related_artifact_id) |aid| try params.append(allocator, .{ .int = aid });
    for (scope_refs.items) |ref| {
        switch (ref.kind) {
            .global => {},
            .association, .repo => try params.append(allocator, .{ .int = ref.id.? }),
        }
    }

    const sql_z = try allocator.dupeZ(u8, sql.items);
    defer allocator.free(sql_z);
    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var out: std.ArrayList(Scenario) = .empty;
    errdefer {
        for (out.items) |s| deinit(s, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// State transitions
// =========================================================================

/// Record a successful test run: status='verified', last_outcome='pass',
/// last_run_at=now. Optional `summary` is included in the audit row.
pub fn verify(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    summary_text: ?[]const u8,
) Error!Scenario {
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    try policy.status.check(.scenario, @tagName(current.status), "verified");

    _ = d.execParams(
        \\update test_scenarios
        \\set status = 'verified',
        \\    last_outcome = 'pass',
        \\    last_run_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'),
        \\    updated_at  = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        \\where id = ?
    , &.{.{ .int = id }}) catch return Error.QueryFailed;

    const audit_summary = if (summary_text) |s|
        try std.fmt.allocPrint(allocator, "verify: {s}", .{s})
    else
        try allocator.dupe(u8, "verify: pass");
    defer allocator.free(audit_summary);
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "scenario", .id = id },
        .summary = audit_summary,
    });

    return try show(d, allocator, id);
}

/// Mark a scenario retired. Optional `reason` lands in the audit
/// summary. `last_outcome` is left alone — retirement is independent
/// of the last run's result.
pub fn retire(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    reason: ?[]const u8,
) Error!Scenario {
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    try policy.status.check(.scenario, @tagName(current.status), "retired");

    _ = d.execParams(
        \\update test_scenarios
        \\set status = 'retired',
        \\    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        \\where id = ?
    , &.{.{ .int = id }}) catch return Error.QueryFailed;

    const summary = if (reason) |r|
        try std.fmt.allocPrint(allocator, "retire: {s}", .{r})
    else
        try allocator.dupe(u8, "retire");
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "scenario", .id = id },
        .summary = summary,
    });

    return try show(d, allocator, id);
}

// =========================================================================
// Output rendering
// =========================================================================

pub fn renderText(s: Scenario, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("id:         {d}\n", .{@as(u64, @intCast(s.id))});
    try writer.print("title:      {s}\n", .{s.title});
    try writer.print("status:     {s}\n", .{@tagName(s.status)});
    try writer.print("scope:      {s}", .{@tagName(s.scope_kind)});
    if (s.scope_id) |sid| try writer.print(":{d}", .{sid});
    try writer.print("\n", .{});
    if (s.related_artifact_id) |a| try writer.print("artifact:   {d}\n", .{a});
    if (s.last_outcome) |o| try writer.print("outcome:    {s}\n", .{@tagName(o)});
    if (s.last_run_at) |t| try writer.print("last run:   {s}\n", .{t});
    if (s.body) |b| try writer.print("body:       {s}\n", .{b});
    try writer.print("created:    {s}\n", .{s.created_at});
    try writer.print("updated:    {s}\n", .{s.updated_at});
}

pub fn renderListText(items: []const Scenario, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    if (items.len == 0) {
        try writer.print("(no scenarios)\n", .{});
        return;
    }
    for (items) |s| {
        const outcome_str: []const u8 = if (s.last_outcome) |o| @tagName(o) else "-";
        try writer.print("{d:>5}  {s:<10}  {s:<8}  {s}\n", .{
            @as(u64, @intCast(s.id)), @tagName(s.status), outcome_str, s.title,
        });
    }
}

// =========================================================================
// Internals
// =========================================================================

const select_columns =
    "id, scope_kind, scope_id, title, body, status, related_artifact_id, " ++
    "last_run_at, last_outcome, created_at, updated_at";

const select_one_sql: [:0]const u8 = "select " ++ select_columns ++ " from test_scenarios where id = ?";
const select_all_prefix = "select " ++ select_columns ++ " from test_scenarios where 1 = 1";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Scenario {
    const scope_kind_text = try stmt.columnTextAlloc(1, allocator);
    defer allocator.free(scope_kind_text);
    const scope_kind = ScopeKind.fromText(scope_kind_text) orelse return Error.QueryFailed;

    const status_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(status_text);
    const status = Status.fromText(status_text) orelse return Error.QueryFailed;

    var outcome: ?Outcome = null;
    if (!stmt.columnIsNull(8)) {
        const outcome_text = try stmt.columnTextAlloc(8, allocator);
        defer allocator.free(outcome_text);
        outcome = Outcome.fromText(outcome_text);
    }

    return .{
        .id = stmt.columnInt(0),
        .scope_kind = scope_kind,
        .scope_id = stmt.columnIntOpt(2),
        .title = try stmt.columnTextAlloc(3, allocator),
        .body = try stmt.columnTextOpt(4, allocator),
        .status = status,
        .related_artifact_id = stmt.columnIntOpt(6),
        .last_run_at = try stmt.columnTextOpt(7, allocator),
        .last_outcome = outcome,
        .created_at = try stmt.columnTextAlloc(9, allocator),
        .updated_at = try stmt.columnTextAlloc(10, allocator),
    };
}

// =========================================================================
// Tests
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "create + show + default status=draft" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try create(&d, a, .{ .title = "Plan does X" });
    defer deinit(s, a);
    try std.testing.expectEqual(Status.draft, s.status);
    try std.testing.expect(s.last_run_at == null);
    try std.testing.expect(s.last_outcome == null);
}

test "create with known association scope writes scope_kind='association'" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'acme'");
    const s = try create(&d, a, .{ .title = "scoped scenario", .scope = "acme" });
    defer deinit(s, a);
    try std.testing.expectEqual(ScopeKind.association, s.scope_kind);
    try std.testing.expectEqual(assoc_id, s.scope_id.?);
}

test "create with scope='global' writes scope_kind='global', scope_id=null" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try create(&d, a, .{ .title = "global scenario", .scope = "global" });
    defer deinit(s, a);
    try std.testing.expectEqual(ScopeKind.global, s.scope_kind);
    try std.testing.expect(s.scope_id == null);
}

test "create with unknown scope slug returns SlugNotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.SlugNotFound, create(&d, a, .{ .title = "x", .scope = "no-such-slug" }));
}

test "create with repo: scope returns UnsupportedScope" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.UnsupportedScope, create(&d, a, .{ .title = "x", .scope = "repo:foo" }));
}

test "verify sets status + outcome + last_run_at" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try create(&d, a, .{ .title = "happy path" });
    defer deinit(s, a);
    const v = try verify(&d, a, s.id, "ran clean in CI");
    defer deinit(v, a);
    try std.testing.expectEqual(Status.verified, v.status);
    try std.testing.expectEqual(Outcome.pass, v.last_outcome.?);
    try std.testing.expect(v.last_run_at != null);
}

test "retire flips status; doesn't touch last_outcome" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try create(&d, a, .{ .title = "old test" });
    defer deinit(s, a);
    const v = try verify(&d, a, s.id, null);
    defer deinit(v, a);
    const r = try retire(&d, a, s.id, "feature removed");
    defer deinit(r, a);
    try std.testing.expectEqual(Status.retired, r.status);
    try std.testing.expectEqual(Outcome.pass, r.last_outcome.?);
}

test "list filters by status" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s1 = try create(&d, a, .{ .title = "to verify" });
    defer deinit(s1, a);
    const s2 = try create(&d, a, .{ .title = "drafting" });
    defer deinit(s2, a);
    const v = try verify(&d, a, s1.id, null);
    defer deinit(v, a);

    const verified = try list(&d, a, .{ .statuses = &.{.verified} });
    defer deinitMany(verified, a);
    try std.testing.expectEqual(@as(usize, 1), verified.len);
    try std.testing.expectEqualStrings("to verify", verified[0].title);
}

test "list with scope filter returns only matching rows" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const s_assoc = try create(&d, a, .{ .title = "assoc scenario", .scope = "acme" });
    defer deinit(s_assoc, a);
    const s_global = try create(&d, a, .{ .title = "global scenario" });
    defer deinit(s_global, a);

    const filtered = try list(&d, a, .{ .scope = "acme" });
    defer deinitMany(filtered, a);
    try std.testing.expectEqual(@as(usize, 1), filtered.len);
    try std.testing.expectEqualStrings("assoc scenario", filtered[0].title);
    try std.testing.expectEqual(ScopeKind.association, filtered[0].scope_kind);
}

test "listTouching suppresses direct-repo branch when scope excludes repo" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into projects (slug, name, root_path) values ('r1', 'R1', '/r1')", &.{});
    const repo_id = try d.intQuery("select id from projects where slug='r1'");
    _ = try d.execParams(
        \\insert into test_scenarios (scope_kind, scope_id, title, status)
        \\values ('repo', ?, 'repo-scoped-scenario', 'draft')
    , &.{.{ .int = repo_id }});

    const all = try listTouching(&d, a, repo_id, .{});
    defer deinitMany(all, a);
    try std.testing.expectEqual(@as(usize, 1), all.len);

    const global_only = try listTouching(&d, a, repo_id, .{ .scope = "global" });
    defer deinitMany(global_only, a);
    try std.testing.expectEqual(@as(usize, 0), global_only.len);
}

test "show returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
}
