//! engine/planning/question — Question entity: CRUD + answer / wontfix.
//!
//! Status set: {open, answered, wontfix}. The schema has a row-level
//! CHECK: status='answered' requires both `answer_body` and
//! `answered_at` to be non-null. The `answer` function below enforces
//! that contract (sets answer_body + answered_at atomically with the
//! status flip); operators can't reach the inconsistent state.
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
    open,
    answered,
    wontfix,

    pub fn fromText(s: []const u8) ?Status {
        if (std.mem.eql(u8, s, "open")) return .open;
        if (std.mem.eql(u8, s, "answered")) return .answered;
        if (std.mem.eql(u8, s, "wontfix")) return .wontfix;
        return null;
    }
};

pub const Question = struct {
    id: i64,
    scope_kind: ScopeKind,
    scope_id: ?i64,
    title: []const u8,
    body: ?[]const u8,
    status: Status,
    answer_body: ?[]const u8,
    answered_at: ?[]const u8,
    created_at: []const u8,
    updated_at: []const u8,
};

pub fn deinit(q: Question, allocator: std.mem.Allocator) void {
    allocator.free(q.title);
    if (q.body) |s| allocator.free(s);
    if (q.answer_body) |s| allocator.free(s);
    if (q.answered_at) |s| allocator.free(s);
    allocator.free(q.created_at);
    allocator.free(q.updated_at);
}

pub fn deinitMany(items: []const Question, allocator: std.mem.Allocator) void {
    for (items) |q| deinit(q, allocator);
    allocator.free(items);
}

pub const CreateArgs = struct {
    title: []const u8,
    body: ?[]const u8 = null,
    scope: ?[]const u8 = null,
    /// Optional plan to link this question to via an entity_links
    /// (`question -> plan`, relationship=`derives-from`) edge. Mirrors
    /// the artifact / decision / scenario one-shot create-and-link
    /// pattern so the question appears under the plan in `planar tree`
    /// output. Q237 follow-up (task 2372).
    plan_id: ?i64 = null,
};

pub const ListFilter = struct {
    statuses: []const Status = &.{},
    scope: ?[]const u8 = null,
    scopes: []const []const u8 = &.{},
};

pub const Error =
    error{
        NotFound,
        UnsupportedScope,
        SlugNotFound,
        QueryFailed,
        AnswerRequired,
    } ||
    std.mem.Allocator.Error ||
    policy.scope_guard.Error ||
    policy.status.Error ||
    policy.audit.Error;

// =========================================================================
// CRUD
// =========================================================================

pub fn create(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: CreateArgs) Error!Question {
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

    if (args.plan_id) |pid| {
        var stmt_plan = d.prepare("select count(*) from plans where id = ?") catch return Error.QueryFailed;
        defer stmt_plan.finalize();
        stmt_plan.bind(&.{.{ .int = pid }}) catch return Error.QueryFailed;
        const n = switch (stmt_plan.step() catch return Error.QueryFailed) {
            .done => return Error.QueryFailed,
            .row => stmt_plan.columnInt(0),
        };
        if (n == 0) return Error.NotFound;
    }

    const id = d.execParams(
        \\insert into questions (scope_kind, scope_id, title, body)
        \\values (?, ?, ?, ?)
    , &.{
        .{ .text = scope_kind_str },
        if (scope_ref.id) |sid| .{ .int = sid } else .{ .null = {} },
        .{ .text = args.title },
        if (args.body) |s| .{ .text = s } else .{ .null = {} },
    }) catch |e| {
        std.log.err("question.create exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const summary = try std.fmt.allocPrint(allocator, "create question '{s}'", .{args.title});
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "question", .id = id },
        .summary = summary,
    });

    if (args.plan_id) |pid| {
        _ = d.execParams(
            \\insert into entity_links (from_kind, from_id, to_kind, to_id, relationship)
            \\values ('question', ?, 'plan', ?, 'derives-from')
        , &.{ .{ .int = id }, .{ .int = pid } }) catch return Error.QueryFailed;
    }

    return try show(d, allocator, id);
}

pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Question {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: ListFilter) Error![]Question {
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
    if (filter.statuses.len == 0) {
        try sql_buf.appendSlice(allocator, " and status = 'open'");
    } else {
        try sql_buf.appendSlice(allocator, " and status in (");
        for (filter.statuses, 0..) |s, i| {
            if (i > 0) try sql_buf.appendSlice(allocator, ",");
            try sql_buf.appendSlice(allocator, "?");
            try params.append(allocator, .{ .text = @tagName(s) });
        }
        try sql_buf.appendSlice(allocator, ")");
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

    var out: std.ArrayList(Question) = .empty;
    errdefer {
        for (out.items) |q| deinit(q, allocator);
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
) Error![]Question {
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
        if (filter.statuses.len == 0) {
            try sql.appendSlice(allocator, " and status='open'");
        } else {
            try sql.appendSlice(allocator, " and status in (");
            for (filter.statuses, 0..) |_, i| {
                if (i > 0) try sql.appendSlice(allocator, ",");
                try sql.appendSlice(allocator, "?");
            }
            try sql.appendSlice(allocator, ")");
        }
    } else {
        try sql.appendSlice(allocator, select_all_prefix);
        try sql.appendSlice(allocator, " and 1=0");
    }
    try sql.appendSlice(allocator, " union ");
    try sql.appendSlice(allocator, select_all_prefix);
    try sql.appendSlice(allocator, " and id in (select from_id from entity_links where from_kind='question' and to_kind='repo' and to_id=? and relationship='touches')");
    if (filter.statuses.len == 0) {
        try sql.appendSlice(allocator, " and status='open'");
    } else {
        try sql.appendSlice(allocator, " and status in (");
        for (filter.statuses, 0..) |_, i| {
            if (i > 0) try sql.appendSlice(allocator, ",");
            try sql.appendSlice(allocator, "?");
        }
        try sql.appendSlice(allocator, ")");
    }
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
    }
    try params.append(allocator, .{ .int = repo_id });
    for (filter.statuses) |s| try params.append(allocator, .{ .text = @tagName(s) });
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

    var out: std.ArrayList(Question) = .empty;
    errdefer {
        for (out.items) |q| deinit(q, allocator);
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

/// Answer a question. Sets status='answered', stores the answer body
/// plus the current timestamp atomically — the schema CHECK refuses
/// the row otherwise. `answer` must be non-empty.
pub fn answer(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    answer_text: []const u8,
) Error!Question {
    if (answer_text.len == 0) return Error.AnswerRequired;

    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    try policy.status.check(.question, @tagName(current.status), "answered");

    _ = d.execParams(
        \\update questions
        \\set status = 'answered',
        \\    answer_body = ?,
        \\    answered_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'),
        \\    updated_at  = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        \\where id = ?
    , &.{ .{ .text = answer_text }, .{ .int = id } }) catch return Error.QueryFailed;

    const summary = try std.fmt.allocPrint(allocator, "answer: {s}", .{answer_text});
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "question", .id = id },
        .summary = summary,
    });

    return try show(d, allocator, id);
}

/// Mark a question wontfix. Optional `reason` lands in the audit
/// summary; doesn't go into a dedicated column (the schema has none).
pub fn wontfix(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    reason: ?[]const u8,
) Error!Question {
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    try policy.status.check(.question, @tagName(current.status), "wontfix");

    _ = d.execParams(
        \\update questions
        \\set status = 'wontfix',
        \\    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        \\where id = ?
    , &.{.{ .int = id }}) catch return Error.QueryFailed;

    const summary = if (reason) |r|
        try std.fmt.allocPrint(allocator, "wontfix: {s}", .{r})
    else
        try allocator.dupe(u8, "wontfix");
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "question", .id = id },
        .summary = summary,
    });

    return try show(d, allocator, id);
}

// =========================================================================
// Output rendering
// =========================================================================

pub fn renderText(q: Question, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("id:        {d}\n", .{@as(u64, @intCast(q.id))});
    try writer.print("title:     {s}\n", .{q.title});
    try writer.print("status:    {s}\n", .{@tagName(q.status)});
    try writer.print("scope:     {s}", .{@tagName(q.scope_kind)});
    if (q.scope_id) |sid| try writer.print(":{d}", .{sid});
    try writer.print("\n", .{});
    if (q.body) |s| try writer.print("body:      {s}\n", .{s});
    if (q.answer_body) |s| try writer.print("answer:    {s}\n", .{s});
    if (q.answered_at) |s| try writer.print("answered:  {s}\n", .{s});
    try writer.print("created:   {s}\n", .{q.created_at});
    try writer.print("updated:   {s}\n", .{q.updated_at});
}

pub fn renderListText(items: []const Question, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    if (items.len == 0) {
        try writer.print("(no questions)\n", .{});
        return;
    }
    for (items) |q| {
        try writer.print("{d:>5}  {s:<10}  {s}\n", .{
            @as(u64, @intCast(q.id)), @tagName(q.status), q.title,
        });
    }
}

// =========================================================================
// Internals
// =========================================================================

const select_columns =
    "id, scope_kind, scope_id, title, body, status, answer_body, answered_at, created_at, updated_at";

const select_one_sql: [:0]const u8 = "select " ++ select_columns ++ " from questions where id = ?";
const select_all_prefix = "select " ++ select_columns ++ " from questions where 1 = 1";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Question {
    const scope_kind_text = try stmt.columnTextAlloc(1, allocator);
    defer allocator.free(scope_kind_text);
    const scope_kind = ScopeKind.fromText(scope_kind_text) orelse return Error.QueryFailed;

    const status_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(status_text);
    const status = Status.fromText(status_text) orelse return Error.QueryFailed;

    return .{
        .id = stmt.columnInt(0),
        .scope_kind = scope_kind,
        .scope_id = stmt.columnIntOpt(2),
        .title = try stmt.columnTextAlloc(3, allocator),
        .body = try stmt.columnTextOpt(4, allocator),
        .status = status,
        .answer_body = try stmt.columnTextOpt(6, allocator),
        .answered_at = try stmt.columnTextOpt(7, allocator),
        .created_at = try stmt.columnTextAlloc(8, allocator),
        .updated_at = try stmt.columnTextAlloc(9, allocator),
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

test "create + show + default status=open" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const q = try create(&d, a, .{ .title = "what's the plan?", .body = "asking nicely" });
    defer deinit(q, a);
    try std.testing.expectEqual(Status.open, q.status);
    try std.testing.expect(q.answer_body == null);
    try std.testing.expect(q.answered_at == null);
}

test "create with known association scope writes scope_kind='association'" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'acme'");
    const q = try create(&d, a, .{ .title = "scoped question", .scope = "acme" });
    defer deinit(q, a);
    try std.testing.expectEqual(ScopeKind.association, q.scope_kind);
    try std.testing.expectEqual(assoc_id, q.scope_id.?);
}

test "create with scope='global' writes scope_kind='global', scope_id=null" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const q = try create(&d, a, .{ .title = "global question", .scope = "global" });
    defer deinit(q, a);
    try std.testing.expectEqual(ScopeKind.global, q.scope_kind);
    try std.testing.expect(q.scope_id == null);
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

test "answer sets status + answer_body + answered_at atomically" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const q = try create(&d, a, .{ .title = "do or do not" });
    defer deinit(q, a);
    const answered = try answer(&d, a, q.id, "do");
    defer deinit(answered, a);
    try std.testing.expectEqual(Status.answered, answered.status);
    try std.testing.expectEqualStrings("do", answered.answer_body.?);
    try std.testing.expect(answered.answered_at != null);
}

test "answer with empty string returns AnswerRequired" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const q = try create(&d, a, .{ .title = "?" });
    defer deinit(q, a);
    try std.testing.expectError(Error.AnswerRequired, answer(&d, a, q.id, ""));
}

test "wontfix records optional reason in audit summary" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const q = try create(&d, a, .{ .title = "deprecated" });
    defer deinit(q, a);
    const wf = try wontfix(&d, a, q.id, "answered elsewhere");
    defer deinit(wf, a);
    try std.testing.expectEqual(Status.wontfix, wf.status);
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery(
            "select count(*) from audit_log where verb='status_change' and entity_kind='question' " ++
                "and summary like 'wontfix: %'",
        ),
    );
}

test "list filters by status" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const q1 = try create(&d, a, .{ .title = "open one" });
    defer deinit(q1, a);
    const q2 = try create(&d, a, .{ .title = "to be answered" });
    defer deinit(q2, a);
    const a2 = try answer(&d, a, q2.id, "yes");
    defer deinit(a2, a);

    const open = try list(&d, a, .{ .statuses = &.{.open} });
    defer deinitMany(open, a);
    try std.testing.expectEqual(@as(usize, 1), open.len);
    try std.testing.expectEqualStrings("open one", open[0].title);
}

test "list with scope filter returns only matching rows" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const q_assoc = try create(&d, a, .{ .title = "assoc question", .scope = "acme" });
    defer deinit(q_assoc, a);
    const q_global = try create(&d, a, .{ .title = "global question" });
    defer deinit(q_global, a);

    const filtered = try list(&d, a, .{ .scope = "acme" });
    defer deinitMany(filtered, a);
    try std.testing.expectEqual(@as(usize, 1), filtered.len);
    try std.testing.expectEqualStrings("assoc question", filtered[0].title);
    try std.testing.expectEqual(ScopeKind.association, filtered[0].scope_kind);
}

test "listTouching suppresses direct-repo branch when scope excludes repo" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into projects (slug, name, root_path) values ('r1', 'R1', '/r1')", &.{});
    const repo_id = try d.intQuery("select id from projects where slug='r1'");
    _ = try d.execParams(
        \\insert into questions (scope_kind, scope_id, title, status)
        \\values ('repo', ?, 'repo-scoped-question', 'open')
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
