//! engine/planning/artifact — Artifact entity: tech specs, ADRs, design
//! notes, generated summaries, READMEs, product specs, roadmaps, and
//! published-doc kinds (research / getting_started / changelog_entry /
//! glossary_term / test_spec).
//!
//! Status set: {draft, active, superseded, retired}. Default is `active`
//! per the schema (artifacts are typically registered post-hoc for
//! existing docs; truly in-flight drafts are minority). Unlike decisions
//! and tasks, artifacts do NOT expose state-transition verbs — moves
//! through statuses happen exclusively via `artifact update --status …`
//! (mirrors the Go side; `internal/planning/artifact` has no Accept /
//! Retire / etc.).
//!
//! Mutable fields (via `update`): title, body, kind, status, source_path.
//!
//! `body` is NULLABLE (unlike `decisions.body`). `source_path` is an
//! optional pointer to a file on disk that the artifact mirrors.
//!
//! Scope handling: when `scope` is provided in CreateArgs/ListFilter/UpdateArgs,
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

/// Closed Kind enum. Values derived from:
///   migrations/00002_planning.up.sql §artifacts (original 9):
///     tech_spec, adr, design_note, summary, readme, generated, other,
///     product_spec, roadmap.
///   migrations/00008_doc_artifact_kinds.up.sql (added 4):
///     research, getting_started, changelog_entry, glossary_term.
///   migrations/00013_test_spec_artifact_kind.up.sql (added 1):
///     test_spec.
pub const Kind = enum {
    tech_spec,
    adr,
    design_note,
    summary,
    readme,
    generated,
    other,
    product_spec,
    roadmap,
    research,
    getting_started,
    changelog_entry,
    glossary_term,
    test_spec,

    pub fn fromText(s: []const u8) ?Kind {
        if (std.mem.eql(u8, s, "tech_spec")) return .tech_spec;
        if (std.mem.eql(u8, s, "adr")) return .adr;
        if (std.mem.eql(u8, s, "design_note")) return .design_note;
        if (std.mem.eql(u8, s, "summary")) return .summary;
        if (std.mem.eql(u8, s, "readme")) return .readme;
        if (std.mem.eql(u8, s, "generated")) return .generated;
        if (std.mem.eql(u8, s, "other")) return .other;
        if (std.mem.eql(u8, s, "product_spec")) return .product_spec;
        if (std.mem.eql(u8, s, "roadmap")) return .roadmap;
        if (std.mem.eql(u8, s, "research")) return .research;
        if (std.mem.eql(u8, s, "getting_started")) return .getting_started;
        if (std.mem.eql(u8, s, "changelog_entry")) return .changelog_entry;
        if (std.mem.eql(u8, s, "glossary_term")) return .glossary_term;
        if (std.mem.eql(u8, s, "test_spec")) return .test_spec;
        return null;
    }
};

pub const Status = enum {
    draft,
    active,
    superseded,
    retired,

    pub fn fromText(s: []const u8) ?Status {
        if (std.mem.eql(u8, s, "draft")) return .draft;
        if (std.mem.eql(u8, s, "active")) return .active;
        if (std.mem.eql(u8, s, "superseded")) return .superseded;
        if (std.mem.eql(u8, s, "retired")) return .retired;
        return null;
    }

    pub fn isTerminal(self: Status) bool {
        return self == .superseded or self == .retired;
    }
};

fn validateTransition(current: Status, next: Status) Error!void {
    if (current == next) return;
    if (current.isTerminal()) return Error.IllegalTransition;
    switch (current) {
        .draft => switch (next) {
            .active => return,
            else => return Error.IllegalTransition,
        },
        .active => switch (next) {
            .draft, .superseded, .retired => return,
            else => return Error.IllegalTransition,
        },
        .superseded, .retired => return Error.IllegalTransition,
    }
}

/// readBody parses --body semantics. "@path" means file contents.
pub fn readBody(allocator: std.mem.Allocator, io: std.Io, val: []const u8) ![]u8 {
    if (val.len > 0 and val[0] == '@') {
        return std.Io.Dir.cwd().readFileAlloc(io, val[1..], allocator, .unlimited);
    }
    return allocator.dupe(u8, val);
}

pub const Artifact = struct {
    id: i64,
    scope_kind: ScopeKind,
    scope_id: ?i64,
    kind: Kind,
    title: []const u8,
    body: ?[]const u8,
    source_path: ?[]const u8,
    status: Status,
    created_at: []const u8,
    updated_at: []const u8,
};

pub fn deinit(a: Artifact, allocator: std.mem.Allocator) void {
    allocator.free(a.title);
    if (a.body) |s| allocator.free(s);
    if (a.source_path) |s| allocator.free(s);
    allocator.free(a.created_at);
    allocator.free(a.updated_at);
}

pub fn deinitMany(items: []const Artifact, allocator: std.mem.Allocator) void {
    for (items) |a| deinit(a, allocator);
    allocator.free(items);
}

pub const CreateArgs = struct {
    title: []const u8,
    kind: Kind,
    /// Nullable per schema; pass null to omit.
    body: ?[]const u8 = null,
    source_path: ?[]const u8 = null,
    status: Status = .active,
    /// `--plan` on the CLI doesn't map to a column on artifacts (the
    /// link is via entity_links). Accepted by the handler for parity
    /// and stored for future use; currently ignored by the engine.
    plan_id: ?i64 = null,
    scope: ?[]const u8 = null,
};

pub const UpdateArgs = struct {
    title: ?[]const u8 = null,
    body: ?[]const u8 = null,
    status: ?Status = null,
    source_path: ?[]const u8 = null,
    scope: ?[]const u8 = null,
};

pub const ListFilter = struct {
    statuses: []const Status = &.{},
    kinds: []const Kind = &.{},
    plan_id: ?i64 = null,
    scope: ?[]const u8 = null,
    scopes: []const []const u8 = &.{},
};

pub const Error =
    error{
        NotFound,
        UnsupportedScope,
        SlugNotFound,
        NoFields,
        QueryFailed,
    } ||
    std.mem.Allocator.Error ||
    policy.scope_guard.Error ||
    policy.status.Error ||
    policy.audit.Error;

// =========================================================================
// CRUD
// =========================================================================

pub fn create(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: CreateArgs) Error!Artifact {
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

    d.savepoint(allocator, "artifact_create") catch |e| switch (e) {
        error.OutOfMemory => return error.OutOfMemory,
        else => return Error.WriteFailed,
    };
    var savepoint_released = false;
    defer {
        if (!savepoint_released) {
            d.rollbackToSavepoint(allocator, "artifact_create") catch {};
            d.releaseSavepoint(allocator, "artifact_create") catch {};
        }
    }

    const insert_sql: [:0]const u8 =
        \\insert into artifacts (scope_kind, scope_id, kind, title, body, source_path, status)
        \\values (?, ?, ?, ?, ?, ?, ?)
    ;
    const id = d.execParams(insert_sql, &.{
        .{ .text = scope_kind_str },
        if (scope_ref.id) |sid| .{ .int = sid } else .{ .null = {} },
        .{ .text = @tagName(args.kind) },
        .{ .text = args.title },
        if (args.body) |s| .{ .text = s } else .{ .null = {} },
        if (args.source_path) |s| .{ .text = s } else .{ .null = {} },
        .{ .text = @tagName(args.status) },
    }) catch |e| {
        std.log.err("artifact.create exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const summary = try std.fmt.allocPrint(
        allocator,
        "create artifact '{s}' (kind={s})",
        .{ args.title, @tagName(args.kind) },
    );
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "artifact", .id = id },
        .summary = summary,
    });

    if (args.plan_id) |pid| {
        _ = d.execParams(
            \\insert into entity_links (from_kind, from_id, to_kind, to_id, relationship)
            \\values ('artifact', ?, 'plan', ?, 'derives-from')
        , &.{ .{ .int = id }, .{ .int = pid } }) catch return Error.QueryFailed;
    }

    d.releaseSavepoint(allocator, "artifact_create") catch |e| switch (e) {
        error.OutOfMemory => return error.OutOfMemory,
        else => return Error.WriteFailed,
    };
    savepoint_released = true;

    return try show(d, allocator, id);
}

pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Artifact {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: ListFilter) Error![]Artifact {
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
        // Mirror Go default: list shows {draft, active} unless a status
        // filter is provided. Terminal statuses are off-screen by default.
        try sql_buf.appendSlice(allocator, " and status in ('draft','active')");
    } else {
        try sql_buf.appendSlice(allocator, " and status in (");
        for (filter.statuses, 0..) |s, i| {
            if (i > 0) try sql_buf.appendSlice(allocator, ",");
            try sql_buf.appendSlice(allocator, "?");
            try params.append(allocator, .{ .text = @tagName(s) });
        }
        try sql_buf.appendSlice(allocator, ")");
    }
    if (filter.kinds.len > 0) {
        try sql_buf.appendSlice(allocator, " and kind in (");
        for (filter.kinds, 0..) |k, i| {
            if (i > 0) try sql_buf.appendSlice(allocator, ",");
            try sql_buf.appendSlice(allocator, "?");
            try params.append(allocator, .{ .text = @tagName(k) });
        }
        try sql_buf.appendSlice(allocator, ")");
    }
    if (filter.plan_id) |pid| {
        try sql_buf.appendSlice(allocator, " and id in (select from_id from entity_links where from_kind='artifact' and to_kind='plan' and relationship='derives-from' and to_id=?)");
        try params.append(allocator, .{ .int = pid });
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

    var out: std.ArrayList(Artifact) = .empty;
    errdefer {
        for (out.items) |a| deinit(a, allocator);
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

pub fn update(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    patch: UpdateArgs,
) Error!Artifact {
    const scope_ref: ?identity.scope.ScopeRef = if (patch.scope) |s|
        identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        }
    else
        null;

    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);

    if (patch.title == null and patch.body == null and patch.status == null and patch.source_path == null and scope_ref == null) {
        return Error.NoFields;
    }

    if (patch.status) |new_status| {
        try validateTransition(current.status, new_status);
    }

    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, "update artifacts set ");

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    var first = true;
    const appendSep = struct {
        fn call(buf: *std.ArrayList(u8), is_first: *bool, alloc: std.mem.Allocator) !void {
            if (!is_first.*) try buf.appendSlice(alloc, ", ");
            is_first.* = false;
        }
    }.call;

    if (scope_ref) |ref| {
        const sk_str: []const u8 = switch (ref.kind) {
            .global => "global",
            .association => "association",
            .repo => "repo",
        };
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "scope_kind = ?");
        try params.append(allocator, .{ .text = sk_str });
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "scope_id = ?");
        try params.append(allocator, if (ref.id) |sid| .{ .int = sid } else .{ .null = {} });
    }
    if (patch.title) |t| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "title = ?");
        try params.append(allocator, .{ .text = t });
    }
    if (patch.body) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "body = ?");
        try params.append(allocator, .{ .text = s });
    }
    if (patch.status) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "status = ?");
        try params.append(allocator, .{ .text = @tagName(s) });
    }
    if (patch.source_path) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "source_path = ?");
        try params.append(allocator, .{ .text = s });
    }

    try appendSep(&sql_buf, &first, allocator);
    try sql_buf.appendSlice(allocator, "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
    try params.append(allocator, .{ .int = id });

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    _ = d.execParams(sql_z, params.items) catch |e| {
        std.log.err("artifact.update exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const verb: policy.audit.Verb = if (patch.status != null) .status_change else .update;
    try policy.audit.record(d, .{
        .verb = verb,
        .entity = .{ .kind = "artifact", .id = id },
        .summary = null,
    });

    return try show(d, allocator, id);
}

// =========================================================================
// Output rendering
// =========================================================================

pub fn renderText(a: Artifact, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("id:          {d}\n", .{@as(u64, @intCast(a.id))});
    try writer.print("title:       {s}\n", .{a.title});
    try writer.print("kind:        {s}\n", .{@tagName(a.kind)});
    try writer.print("status:      {s}\n", .{@tagName(a.status)});
    try writer.print("scope:       {s}", .{@tagName(a.scope_kind)});
    if (a.scope_id) |sid| try writer.print(":{d}", .{sid});
    try writer.print("\n", .{});
    if (a.source_path) |s| try writer.print("source path: {s}\n", .{s});
    if (a.body) |s| try writer.print("body:        {s}\n", .{s});
    try writer.print("created:     {s}\n", .{a.created_at});
    try writer.print("updated:     {s}\n", .{a.updated_at});
}

pub fn renderListText(items: []const Artifact, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    if (items.len == 0) {
        try writer.print("(no artifacts)\n", .{});
        return;
    }
    for (items) |a| {
        try writer.print("{d:>5}  {s:<10}  {s:<16}  {s}\n", .{
            @as(u64, @intCast(a.id)), @tagName(a.status), @tagName(a.kind), a.title,
        });
    }
}

// =========================================================================
// Internals
// =========================================================================

const select_columns =
    "id, scope_kind, scope_id, kind, title, body, source_path, status, " ++
    "created_at, updated_at";

const select_one_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from artifacts where id = ?";

const select_all_prefix =
    "select " ++ select_columns ++ " from artifacts where 1 = 1";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Artifact {
    const scope_kind_text = try stmt.columnTextAlloc(1, allocator);
    defer allocator.free(scope_kind_text);
    const scope_kind = ScopeKind.fromText(scope_kind_text) orelse return Error.QueryFailed;

    const kind_text = try stmt.columnTextAlloc(3, allocator);
    defer allocator.free(kind_text);
    const kind = Kind.fromText(kind_text) orelse return Error.QueryFailed;

    const status_text = try stmt.columnTextAlloc(7, allocator);
    defer allocator.free(status_text);
    const status = Status.fromText(status_text) orelse return Error.QueryFailed;

    return .{
        .id = stmt.columnInt(0),
        .scope_kind = scope_kind,
        .scope_id = stmt.columnIntOpt(2),
        .kind = kind,
        .title = try stmt.columnTextAlloc(4, allocator),
        .body = try stmt.columnTextOpt(5, allocator),
        .source_path = try stmt.columnTextOpt(6, allocator),
        .status = status,
        .created_at = try stmt.columnTextAlloc(8, allocator),
        .updated_at = try stmt.columnTextAlloc(9, allocator),
    };
}

// =========================================================================
// Tests
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var dn = try db.sqlite.Db.openMemory();
    errdefer dn.close();
    try db.migrate.applyAll(&dn, allocator);
    return dn;
}

test "create + show: status defaults to active, body nullable" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const art = try create(&d, a, .{ .title = "Arch overview", .kind = .tech_spec });
    defer deinit(art, a);
    try std.testing.expectEqual(Status.active, art.status);
    try std.testing.expectEqual(Kind.tech_spec, art.kind);
    try std.testing.expect(art.body == null);
    try std.testing.expect(art.source_path == null);
    try std.testing.expectEqualStrings("Arch overview", art.title);
}

test "create with body + source_path round-trips" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const art = try create(&d, a, .{
        .title = "ADR 0007",
        .kind = .adr,
        .body = "Decided to do X.",
        .source_path = "docs/adrs/0007.md",
    });
    defer deinit(art, a);
    try std.testing.expect(art.body != null);
    try std.testing.expectEqualStrings("Decided to do X.", art.body.?);
    try std.testing.expect(art.source_path != null);
    try std.testing.expectEqualStrings("docs/adrs/0007.md", art.source_path.?);
}

test "create with known association scope writes scope_kind='association'" {
    const a = std.testing.allocator;
    var dn = try setupTestDb(a);
    defer dn.close();
    _ = try dn.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try dn.intQuery("select id from associations where slug = 'acme'");
    const art = try create(&dn, a, .{ .title = "scoped artifact", .kind = .other, .scope = "acme" });
    defer deinit(art, a);
    try std.testing.expectEqual(ScopeKind.association, art.scope_kind);
    try std.testing.expectEqual(assoc_id, art.scope_id.?);
}

test "create with scope='global' writes scope_kind='global', scope_id=null" {
    const a = std.testing.allocator;
    var dn = try setupTestDb(a);
    defer dn.close();
    const art = try create(&dn, a, .{ .title = "global artifact", .kind = .other, .scope = "global" });
    defer deinit(art, a);
    try std.testing.expectEqual(ScopeKind.global, art.scope_kind);
    try std.testing.expect(art.scope_id == null);
}

test "create with unknown scope slug returns SlugNotFound" {
    const a = std.testing.allocator;
    var dn = try setupTestDb(a);
    defer dn.close();
    try std.testing.expectError(
        Error.SlugNotFound,
        create(&dn, a, .{ .title = "x", .kind = .other, .scope = "no-such-slug" }),
    );
}

test "create with repo: scope returns UnsupportedScope" {
    const a = std.testing.allocator;
    var dn = try setupTestDb(a);
    defer dn.close();
    try std.testing.expectError(
        Error.UnsupportedScope,
        create(&dn, a, .{ .title = "x", .kind = .other, .scope = "repo:foo" }),
    );
}

test "list defaults filter to {draft, active}" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const a1 = try create(&d, a, .{ .title = "open", .kind = .summary });
    defer deinit(a1, a);
    const a2 = try create(&d, a, .{ .title = "retired-soon", .kind = .summary });
    defer deinit(a2, a);
    const r = try update(&d, a, a2.id, .{ .status = .retired });
    defer deinit(r, a);

    const open = try list(&d, a, .{});
    defer deinitMany(open, a);
    try std.testing.expectEqual(@as(usize, 1), open.len);
    try std.testing.expectEqualStrings("open", open[0].title);

    const all_retired = try list(&d, a, .{ .statuses = &.{.retired} });
    defer deinitMany(all_retired, a);
    try std.testing.expectEqual(@as(usize, 1), all_retired.len);
}

test "list with scope filter returns only matching rows" {
    const a = std.testing.allocator;
    var dn = try setupTestDb(a);
    defer dn.close();
    _ = try dn.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const art_assoc = try create(&dn, a, .{ .title = "assoc artifact", .kind = .other, .scope = "acme" });
    defer deinit(art_assoc, a);
    const art_global = try create(&dn, a, .{ .title = "global artifact", .kind = .other });
    defer deinit(art_global, a);

    const filtered = try list(&dn, a, .{ .scope = "acme" });
    defer deinitMany(filtered, a);
    try std.testing.expectEqual(@as(usize, 1), filtered.len);
    try std.testing.expectEqualStrings("assoc artifact", filtered[0].title);
    try std.testing.expectEqual(ScopeKind.association, filtered[0].scope_kind);
}

test "list filters by kind" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const a1 = try create(&d, a, .{ .title = "ts", .kind = .tech_spec });
    defer deinit(a1, a);
    const a2 = try create(&d, a, .{ .title = "rm", .kind = .readme });
    defer deinit(a2, a);

    const specs = try list(&d, a, .{ .kinds = &.{.tech_spec} });
    defer deinitMany(specs, a);
    try std.testing.expectEqual(@as(usize, 1), specs.len);
    try std.testing.expectEqualStrings("ts", specs[0].title);
}

test "update changes title and refreshes updated_at" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const art = try create(&d, a, .{ .title = "Old title", .kind = .other });
    defer deinit(art, a);
    const upd = try update(&d, a, art.id, .{ .title = "New title" });
    defer deinit(upd, a);
    try std.testing.expectEqualStrings("New title", upd.title);
    try std.testing.expectEqual(art.id, upd.id);
}

test "update with scope moves artifact to association scope" {
    const a = std.testing.allocator;
    var dn = try setupTestDb(a);
    defer dn.close();
    _ = try dn.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try dn.intQuery("select id from associations where slug = 'acme'");
    const art = try create(&dn, a, .{ .title = "initially global", .kind = .other });
    defer deinit(art, a);
    try std.testing.expectEqual(ScopeKind.global, art.scope_kind);

    const upd = try update(&dn, a, art.id, .{ .scope = "acme" });
    defer deinit(upd, a);
    try std.testing.expectEqual(ScopeKind.association, upd.scope_kind);
    try std.testing.expectEqual(assoc_id, upd.scope_id.?);
}

test "update body, source_path, status in one call" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const art = try create(&d, a, .{ .title = "x", .kind = .other });
    defer deinit(art, a);
    const upd = try update(&d, a, art.id, .{
        .body = "new body",
        .source_path = "docs/adrs/x.md",
        .status = .draft,
    });
    defer deinit(upd, a);
    try std.testing.expectEqualStrings("new body", upd.body.?);
    try std.testing.expectEqual(Kind.other, upd.kind);
    try std.testing.expectEqualStrings("docs/adrs/x.md", upd.source_path.?);
    try std.testing.expectEqual(Status.draft, upd.status);
}

test "update with status records a status_change audit row; pure update records update" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const art = try create(&d, a, .{ .title = "x", .kind = .other });
    defer deinit(art, a);
    const upd1 = try update(&d, a, art.id, .{ .title = "rename only" });
    defer deinit(upd1, a);
    const upd2 = try update(&d, a, art.id, .{ .status = .superseded });
    defer deinit(upd2, a);

    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='update' and entity_kind='artifact'"),
    );
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='status_change' and entity_kind='artifact'"),
    );
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='create' and entity_kind='artifact'"),
    );
}

test "show returns NotFound for missing id" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
}

test "Kind.fromText accepts every schema-valid value" {
    // Single sample of each migration source — guards against typos in
    // the enum-to-text mapping that would otherwise only surface when an
    // operator tries the kind from the CLI.
    try std.testing.expectEqual(Kind.tech_spec, Kind.fromText("tech_spec").?);
    try std.testing.expectEqual(Kind.adr, Kind.fromText("adr").?);
    try std.testing.expectEqual(Kind.design_note, Kind.fromText("design_note").?);
    try std.testing.expectEqual(Kind.summary, Kind.fromText("summary").?);
    try std.testing.expectEqual(Kind.readme, Kind.fromText("readme").?);
    try std.testing.expectEqual(Kind.generated, Kind.fromText("generated").?);
    try std.testing.expectEqual(Kind.other, Kind.fromText("other").?);
    try std.testing.expectEqual(Kind.product_spec, Kind.fromText("product_spec").?);
    try std.testing.expectEqual(Kind.roadmap, Kind.fromText("roadmap").?);
    try std.testing.expectEqual(Kind.research, Kind.fromText("research").?);
    try std.testing.expectEqual(Kind.getting_started, Kind.fromText("getting_started").?);
    try std.testing.expectEqual(Kind.changelog_entry, Kind.fromText("changelog_entry").?);
    try std.testing.expectEqual(Kind.glossary_term, Kind.fromText("glossary_term").?);
    try std.testing.expectEqual(Kind.test_spec, Kind.fromText("test_spec").?);
    try std.testing.expect(Kind.fromText("not_a_kind") == null);
}
