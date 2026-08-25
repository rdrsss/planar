//! engine/planning/annotation — Annotation entity: line-anchored notes
//! on code with anchor-drift fields, optional slug, tags, FTS5 indexing.
//!
//! Status set: {active, resolved, dismissed, archived}.
//!
//! `archived` is the single final retention state (plan 692 decision).
//! `resolved` and `dismissed` are outcome states — they are NOT final:
//! both may still progress to `archived` (the retention-tier model).
//! The full lifecycle matrix:
//!   active    → {resolved, dismissed, archived}
//!   resolved  → {archived}   (retention-tier progression)
//!   dismissed → {archived}   (retention-tier progression)
//!   archived  → terminal (no outgoing edges)
//!
//! Lifecycle transitions go through `resolve` / `dismiss` / `archive`
//! verbs (or via `update --status …` for symmetry with the Go side,
//! which permits both shapes).
//!
//! Anchor fields (path required; line range, commit sha, text hash,
//! text snippet all optional) describe WHERE in the source tree the
//! annotation hangs. Line columns are NULLABLE — a zero line value at
//! the Zig API surface maps to SQL NULL (file-level annotation).
//!
//! `body` is NOT NULL (defaults to ''). `title` and `slug` are nullable.
//! `vendor` is NOT NULL (defaults to ''). `plan_id` / `task_id` are
//! direct FK columns (unlike other planning entities, which link via
//! entity_links).
//!
//! Tags live in `annotation_tags` (composite PK (annotation_id, tag)).
//! Add / Remove / List operate against that table directly; cascade
//! delete on the parent annotation removes its tag rows automatically.
//!
//! FTS5 sync: migration 12 installs triggers on annotations(insert,
//! delete, update) that maintain `search_annotations`. The engine writes
//! only to `annotations`; the triggers do the index work.
//!
//! Scope handling: when `scope` is provided in CreateArgs/ListFilter/UpdateArgs,
//! it is resolved via engine.identity.scope.resolveSlug. Mirrors the M3
//! Cycle B pattern established by artifact.zig.

const std = @import("std");
const db = @import("db");
const identity = @import("../identity.zig");
const policy = @import("../policy.zig");

// =========================================================================
// Types
// =========================================================================

pub const ScopeKind = enum {
    global,
    repo,
    association,

    pub fn fromText(s: []const u8) ?ScopeKind {
        if (std.mem.eql(u8, s, "global")) return .global;
        if (std.mem.eql(u8, s, "repo")) return .repo;
        if (std.mem.eql(u8, s, "association")) return .association;
        return null;
    }
};

pub const Status = enum {
    active,
    resolved,
    dismissed,
    archived,

    pub fn fromText(s: []const u8) ?Status {
        if (std.mem.eql(u8, s, "active")) return .active;
        if (std.mem.eql(u8, s, "resolved")) return .resolved;
        if (std.mem.eql(u8, s, "dismissed")) return .dismissed;
        if (std.mem.eql(u8, s, "archived")) return .archived;
        return null;
    }

    /// Returns true when this status cannot accept a `resolve` or `dismiss`
    /// transition — i.e. the row is already at or past an outcome state.
    ///
    /// Under the retention-tier model (plan 692): `resolved` and `dismissed`
    /// are outcome states that may still progress to `archived`, but they
    /// cannot go back to `active` or across to each other.  `archived` is
    /// the sole final state with no outgoing edges.  All three are equally
    /// ineligible as targets for `resolve`/`dismiss` bulk operations, which
    /// is the only call-site meaning of this predicate (bulk.zig).
    ///
    /// Do NOT use this to mean "no outgoing edges" — `archived` is the only
    /// status with no outgoing edges.  Use `self == .archived` for that test.
    pub fn isTerminal(self: Status) bool {
        return self == .resolved or self == .dismissed or self == .archived;
    }
};

/// Anchor descriptor — where the annotation hangs in the source tree.
/// `line_start` / `line_end` of zero on input mean "file-level"; on
/// readback they are surfaced as `null` to match SQL NULL.
pub const AnchorFields = struct {
    path: []const u8,
    line_start: ?i64 = null,
    line_end: ?i64 = null,
    commit_sha: []const u8 = "",
    text_hash: []const u8 = "",
    text: []const u8 = "",
};

pub const Annotation = struct {
    id: i64,
    scope_kind: ScopeKind,
    scope_id: ?i64,
    anchor: AnchorFields,
    title: ?[]const u8,
    slug: ?[]const u8,
    body: []const u8,
    status: Status,
    vendor: []const u8,
    plan_id: ?i64,
    task_id: ?i64,
    /// Tags attached to the annotation, lexicographically sorted ascending.
    tags: []const []const u8,
    created_at: []const u8,
    updated_at: []const u8,
};

pub fn deinit(a: Annotation, allocator: std.mem.Allocator) void {
    allocator.free(a.anchor.path);
    allocator.free(a.anchor.commit_sha);
    allocator.free(a.anchor.text_hash);
    allocator.free(a.anchor.text);
    if (a.title) |s| allocator.free(s);
    if (a.slug) |s| allocator.free(s);
    allocator.free(a.body);
    allocator.free(a.vendor);
    for (a.tags) |t| allocator.free(t);
    allocator.free(a.tags);
    allocator.free(a.created_at);
    allocator.free(a.updated_at);
}

pub fn deinitMany(items: []const Annotation, allocator: std.mem.Allocator) void {
    for (items) |a| deinit(a, allocator);
    allocator.free(items);
}

pub const CreateArgs = struct {
    /// Required — where the annotation hangs in the source tree.
    anchor: AnchorFields,
    title: ?[]const u8 = null,
    slug: ?[]const u8 = null,
    body: []const u8 = "",
    status: Status = .active,
    vendor: []const u8 = "",
    plan_id: ?i64 = null,
    task_id: ?i64 = null,
    tags: []const []const u8 = &.{},
    scope: ?[]const u8 = null,
};

pub const UpdateArgs = struct {
    title: ?[]const u8 = null,
    slug: ?[]const u8 = null,
    body: ?[]const u8 = null,
    status: ?Status = null,
    plan_id: ?i64 = null,
    task_id: ?i64 = null,
    /// Replace the full anchor descriptor; null leaves it untouched.
    anchor: ?AnchorFields = null,
    scope: ?[]const u8 = null,
};

pub const ListFilter = struct {
    anchor_path: ?[]const u8 = null,
    status: ?Status = null,
    plan_id: ?i64 = null,
    task_id: ?i64 = null,
    vendor: ?[]const u8 = null,
    tag: ?[]const u8 = null,
    scope: ?[]const u8 = null,
};

pub const Error =
    error{
        NotFound,
        UnsupportedScope,
        SlugNotFound,
        TerminalStatus,
        SlugConflict,
        EmptyTag,
        QueryFailed,
    } ||
    std.mem.Allocator.Error ||
    policy.scope_guard.Error ||
    policy.status.Error ||
    policy.audit.Error;

// =========================================================================
// CRUD
// =========================================================================

pub fn create(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: CreateArgs) Error!Annotation {
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

    const insert_sql: [:0]const u8 =
        \\insert into annotations (
        \\  scope_kind, scope_id,
        \\  anchor_path, anchor_line_start, anchor_line_end,
        \\  anchor_commit_sha, anchor_text_hash, anchor_text,
        \\  title, slug, body, status, vendor, plan_id, task_id
        \\) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ;

    const id = d.execParams(insert_sql, &.{
        .{ .text = scope_kind_str },
        if (scope_ref.id) |sid| .{ .int = sid } else .{ .null = {} },
        .{ .text = args.anchor.path },
        if (args.anchor.line_start) |n| .{ .int = n } else .{ .null = {} },
        if (args.anchor.line_end) |n| .{ .int = n } else .{ .null = {} },
        .{ .text = args.anchor.commit_sha },
        .{ .text = args.anchor.text_hash },
        .{ .text = args.anchor.text },
        if (args.title) |s| .{ .text = s } else .{ .null = {} },
        if (args.slug) |s| .{ .text = s } else .{ .null = {} },
        .{ .text = args.body },
        .{ .text = @tagName(args.status) },
        .{ .text = args.vendor },
        if (args.plan_id) |n| .{ .int = n } else .{ .null = {} },
        if (args.task_id) |n| .{ .int = n } else .{ .null = {} },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.SlugConflict;
        std.log.err("annotation.create exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    // Insert tags (dedup + trim) inside the same logical op. Mirrors
    // annotate.dedupedTags on the Go side. Errors here are surfaced
    // wholesale — the annotation row is already committed, but a tag
    // failure is rare (and the operator can retry tag insertion via
    // addTag).
    var seen: std.StringHashMap(void) = .init(allocator);
    defer seen.deinit();
    for (args.tags) |raw| {
        const trimmed = std.mem.trim(u8, raw, " \t\r\n");
        if (trimmed.len == 0) continue;
        if (seen.contains(trimmed)) continue;
        try seen.put(trimmed, {});
        _ = d.execParams(
            "insert into annotation_tags (annotation_id, tag) values (?, ?)",
            &.{ .{ .int = id }, .{ .text = trimmed } },
        ) catch return Error.QueryFailed;
    }

    const summary_title = args.title orelse args.anchor.path;
    const summary = try std.fmt.allocPrint(
        allocator,
        "create annotation '{s}'",
        .{summary_title},
    );
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "annotation", .id = id },
        .summary = summary,
    });

    return try show(d, allocator, id);
}

pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Annotation {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => blk: {
            var row = try readRow(&stmt, allocator);
            errdefer deinit(row, allocator);
            row.tags = try loadTags(d, allocator, row.id);
            break :blk row;
        },
    };
}

/// Look up an annotation by its (optional) slug. Returns NotFound when
/// no annotation has the given slug.
pub fn showBySlug(d: *db.sqlite.Db, allocator: std.mem.Allocator, slug: []const u8) Error!Annotation {
    var stmt = d.prepare("select id from annotations where slug = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try show(d, allocator, stmt.columnInt(0)),
    };
}

pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: ListFilter) Error![]Annotation {
    const scope_ref: ?identity.scope.ScopeRef = if (filter.scope) |s|
        identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        }
    else
        null;

    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, select_all_prefix);

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    if (filter.anchor_path) |p| {
        try sql_buf.appendSlice(allocator, " and anchor_path = ?");
        try params.append(allocator, .{ .text = p });
    }
    if (filter.status) |s| {
        try sql_buf.appendSlice(allocator, " and status = ?");
        try params.append(allocator, .{ .text = @tagName(s) });
    }
    if (filter.plan_id) |n| {
        try sql_buf.appendSlice(allocator, " and plan_id = ?");
        try params.append(allocator, .{ .int = n });
    }
    if (filter.task_id) |n| {
        try sql_buf.appendSlice(allocator, " and task_id = ?");
        try params.append(allocator, .{ .int = n });
    }
    if (filter.vendor) |v| {
        try sql_buf.appendSlice(allocator, " and vendor = ?");
        try params.append(allocator, .{ .text = v });
    }
    if (filter.tag) |t| {
        try sql_buf.appendSlice(
            allocator,
            " and id in (select annotation_id from annotation_tags where tag = ?)",
        );
        try params.append(allocator, .{ .text = t });
    }
    if (scope_ref) |ref| {
        switch (ref.kind) {
            .global => try sql_buf.appendSlice(allocator, " and scope_kind = 'global'"),
            .association => {
                try sql_buf.appendSlice(allocator, " and scope_kind = 'association' and scope_id = ?");
                try params.append(allocator, .{ .int = ref.id.? });
            },
            .repo => {
                try sql_buf.appendSlice(allocator, " and scope_kind = 'repo' and scope_id = ?");
                try params.append(allocator, .{ .int = ref.id.? });
            },
        }
    }
    try sql_buf.appendSlice(allocator, " order by id");

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var out: std.ArrayList(Annotation) = .empty;
    errdefer {
        for (out.items) |x| deinit(x, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                var row = try readRow(&stmt, allocator);
                errdefer deinit(row, allocator);
                row.tags = try loadTags(d, allocator, row.id);
                try out.append(allocator, row);
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn update(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    patch: UpdateArgs,
) Error!Annotation {
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

    if (patch.status) |new_status| {
        try policy.status.check(.annotation, @tagName(current.status), @tagName(new_status), false);
    }

    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, "update annotations set ");

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
    if (patch.slug) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "slug = ?");
        try params.append(allocator, .{ .text = s });
    }
    if (patch.body) |b| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "body = ?");
        try params.append(allocator, .{ .text = b });
    }
    if (patch.status) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "status = ?");
        try params.append(allocator, .{ .text = @tagName(s) });
    }
    if (patch.plan_id) |n| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "plan_id = ?");
        try params.append(allocator, .{ .int = n });
    }
    if (patch.task_id) |n| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "task_id = ?");
        try params.append(allocator, .{ .int = n });
    }
    if (patch.anchor) |an| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "anchor_path = ?, anchor_line_start = ?, anchor_line_end = ?, " ++
            "anchor_commit_sha = ?, anchor_text_hash = ?, anchor_text = ?");
        try params.append(allocator, .{ .text = an.path });
        try params.append(allocator, if (an.line_start) |n| .{ .int = n } else .{ .null = {} });
        try params.append(allocator, if (an.line_end) |n| .{ .int = n } else .{ .null = {} });
        try params.append(allocator, .{ .text = an.commit_sha });
        try params.append(allocator, .{ .text = an.text_hash });
        try params.append(allocator, .{ .text = an.text });
    }

    // No-op update — return a fresh snapshot.
    if (first) return try show(d, allocator, id);

    try appendSep(&sql_buf, &first, allocator);
    try sql_buf.appendSlice(allocator, "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
    try params.append(allocator, .{ .int = id });

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    _ = d.execParams(sql_z, params.items) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.SlugConflict;
        std.log.err("annotation.update exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const verb: policy.audit.Verb = if (patch.status != null) .status_change else .update;
    try policy.audit.record(d, .{
        .verb = verb,
        .entity = .{ .kind = "annotation", .id = id },
        .summary = null,
    });

    return try show(d, allocator, id);
}

/// Delete the annotation with `id`. Cascade on annotation_tags removes
/// tag rows automatically; FTS5 delete trigger evicts the index entry.
pub fn remove(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!void {
    // Verify existence (so we can return NotFound rather than a silent
    // no-op) and to validate scope before the destructive op.
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);

    _ = d.execParams("delete from annotations where id = ?", &.{.{ .int = id }}) catch |e| {
        std.log.err("annotation.remove exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    try policy.audit.record(d, .{
        .verb = .delete,
        .entity = .{ .kind = "annotation", .id = id },
        .summary = null,
    });
}

// =========================================================================
// State transitions
// =========================================================================

pub fn resolve(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Annotation {
    return try transition(d, allocator, id, .resolved, "resolve");
}

pub fn dismiss(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Annotation {
    return try transition(d, allocator, id, .dismissed, "dismiss");
}

pub fn archive(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Annotation {
    return try transition(d, allocator, id, .archived, "archive");
}

fn transition(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    new_status: Status,
    verb_label: []const u8,
) Error!Annotation {
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);
    try policy.scope_guard.check(null, null);
    // The .annotation arm in policy.status is now authoritative for the
    // terminal guard.  Map its IllegalTransition back to TerminalStatus so
    // callers (handlers, bulk.zig, sweep.zig) observe the same error name
    // as before this consolidation.
    policy.status.check(.annotation, @tagName(current.status), @tagName(new_status), false) catch |e| switch (e) {
        error.IllegalTransition => return Error.TerminalStatus,
        error.UnknownStatus => return Error.TerminalStatus,
    };

    _ = d.execParams(
        \\update annotations
        \\set status = ?,
        \\    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        \\where id = ?
    , &.{ .{ .text = @tagName(new_status) }, .{ .int = id } }) catch return Error.QueryFailed;

    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "annotation", .id = id },
        .summary = verb_label,
    });

    return try show(d, allocator, id);
}

// =========================================================================
// Sweep
// =========================================================================

/// Filter for `sweep` / `sweepCandidates`.
pub const SweepFilter = struct {
    /// Staleness cutoff in days. A row is eligible only when
    /// `julianday('now') - julianday(updated_at)` is STRICTLY greater
    /// than this, so a row exactly `since_days` old is not swept.
    since_days: i64 = 30,
    /// Scope-ref slug. `null` means "every scope" — the historical
    /// behaviour, retained for a bare `annotate sweep`. When set, the
    /// sweep is restricted to that scope with the same predicate `list`
    /// uses, so a scoped sweep can never reach a sibling scope's rows.
    scope: ?[]const u8 = null,
};

/// Ids `sweep` would archive: status in {resolved, dismissed} AND
/// `updated_at` older than the cutoff, optionally restricted to one
/// scope. Caller owns the returned slice.
pub fn sweepCandidates(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: SweepFilter,
) Error![]i64 {
    const scope_ref: ?identity.scope.ScopeRef = if (filter.scope) |s|
        identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        }
    else
        null;

    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    // `since_days` is an integer, so interpolating it carries no
    // injection surface; the scope id is bound like everywhere else.
    var head_buf: [256]u8 = undefined;
    const head = std.fmt.bufPrint(
        &head_buf,
        "select id from annotations" ++
            " where status in ('resolved','dismissed')" ++
            "   and (julianday('now') - julianday(updated_at)) > {d}",
        .{filter.since_days},
    ) catch return Error.QueryFailed;
    try sql_buf.appendSlice(allocator, head);

    if (scope_ref) |ref| {
        switch (ref.kind) {
            .global => try sql_buf.appendSlice(allocator, " and scope_kind = 'global'"),
            .association => {
                try sql_buf.appendSlice(allocator, " and scope_kind = 'association' and scope_id = ?");
                try params.append(allocator, .{ .int = ref.id.? });
            },
            .repo => {
                try sql_buf.appendSlice(allocator, " and scope_kind = 'repo' and scope_id = ?");
                try params.append(allocator, .{ .int = ref.id.? });
            },
        }
    }

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    var ids: std.ArrayList(i64) = .empty;
    errdefer ids.deinit(allocator);
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try ids.append(allocator, stmt.columnInt(0)),
        }
    }
    return try ids.toOwnedSlice(allocator);
}

/// Archive every `sweepCandidates` row, returning the transitioned count.
/// Like the bulk leaves this is NOT transactional and swallows per-row
/// `TerminalStatus` — the defensive guard for a row archived concurrently
/// between the SELECT and the UPDATE.
pub fn sweep(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: SweepFilter,
) Error!usize {
    const ids = try sweepCandidates(d, allocator, filter);
    defer allocator.free(ids);

    var count: usize = 0;
    for (ids) |id| {
        const arc = archive(d, allocator, id) catch |e| switch (e) {
            error.TerminalStatus => continue,
            else => return e,
        };
        deinit(arc, allocator);
        count += 1;
    }
    return count;
}

// =========================================================================
// Tag ops
// =========================================================================

/// Attach `tag` to annotation `id`. Duplicate (annotation, tag) pairs
/// are a no-op (PRIMARY KEY collision absorbed). Empty tags rejected.
pub fn addTag(d: *db.sqlite.Db, allocator: std.mem.Allocator, ann_id: i64, tag: []const u8) Error!void {
    const trimmed = std.mem.trim(u8, tag, " \t\r\n");
    if (trimmed.len == 0) return Error.EmptyTag;

    // Verify the annotation exists — otherwise the FK is fine but the
    // operator should hear NotFound rather than a silent insert with a
    // dangling-looking row.
    const current = try show(d, allocator, ann_id);
    defer deinit(current, allocator);

    _ = d.execParams(
        \\insert into annotation_tags (annotation_id, tag) values (?, ?)
        \\on conflict(annotation_id, tag) do nothing
    , &.{ .{ .int = ann_id }, .{ .text = trimmed } }) catch return Error.QueryFailed;
}

/// Detach `tag` from annotation `id`. Removing a missing tag is a no-op.
pub fn removeTag(d: *db.sqlite.Db, ann_id: i64, tag: []const u8) Error!void {
    const trimmed = std.mem.trim(u8, tag, " \t\r\n");
    if (trimmed.len == 0) return Error.EmptyTag;
    _ = d.execParams(
        "delete from annotation_tags where annotation_id = ? and tag = ?",
        &.{ .{ .int = ann_id }, .{ .text = trimmed } },
    ) catch return Error.QueryFailed;
}

/// List tags for annotation `id`, lexicographically ascending. Caller
/// owns the returned slice and each element.
pub fn listTags(d: *db.sqlite.Db, allocator: std.mem.Allocator, ann_id: i64) Error![][]const u8 {
    return try loadTags(d, allocator, ann_id);
}

fn loadTags(d: *db.sqlite.Db, allocator: std.mem.Allocator, ann_id: i64) Error![][]const u8 {
    var stmt = d.prepare(
        "select tag from annotation_tags where annotation_id = ? order by tag",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = ann_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (out.items) |t| allocator.free(t);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try stmt.columnTextAlloc(0, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Output rendering
// =========================================================================

pub fn renderText(ann: Annotation, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("id:          {d}\n", .{@as(u64, @intCast(ann.id))});
    if (ann.title) |t| try writer.print("title:       {s}\n", .{t});
    if (ann.slug) |s| try writer.print("slug:        {s}\n", .{s});
    try writer.print("status:      {s}\n", .{@tagName(ann.status)});
    try writer.print("scope:       {s}", .{@tagName(ann.scope_kind)});
    if (ann.scope_id) |sid| try writer.print(":{d}", .{sid});
    try writer.print("\n", .{});
    try writer.print("anchor path: {s}\n", .{ann.anchor.path});
    if (ann.anchor.line_start) |ls| {
        if (ann.anchor.line_end) |le| {
            try writer.print("anchor line: {d}-{d}\n", .{ ls, le });
        } else {
            try writer.print("anchor line: {d}\n", .{ls});
        }
    }
    if (ann.anchor.commit_sha.len > 0) try writer.print("anchor sha:  {s}\n", .{ann.anchor.commit_sha});
    if (ann.anchor.text_hash.len > 0) try writer.print("anchor hash: {s}\n", .{ann.anchor.text_hash});
    if (ann.vendor.len > 0) try writer.print("vendor:      {s}\n", .{ann.vendor});
    if (ann.plan_id) |p| try writer.print("plan:        {d}\n", .{p});
    if (ann.task_id) |t| try writer.print("task:        {d}\n", .{t});
    if (ann.tags.len > 0) {
        try writer.print("tags:        ", .{});
        for (ann.tags, 0..) |t, i| {
            if (i > 0) try writer.print(", ", .{});
            try writer.print("{s}", .{t});
        }
        try writer.print("\n", .{});
    }
    if (ann.body.len > 0) try writer.print("body:        {s}\n", .{ann.body});
    try writer.print("created:     {s}\n", .{ann.created_at});
    try writer.print("updated:     {s}\n", .{ann.updated_at});
}

pub fn renderListText(items: []const Annotation, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    if (items.len == 0) {
        try writer.print("(no annotations)\n", .{});
        return;
    }
    for (items) |ann| {
        const title = ann.title orelse ann.anchor.path;
        try writer.print("{d:>5}  {s:<10}  {s}\n", .{
            @as(u64, @intCast(ann.id)), @tagName(ann.status), title,
        });
    }
}

// =========================================================================
// Internals
// =========================================================================

const select_columns =
    "id, scope_kind, scope_id, " ++
    "anchor_path, anchor_line_start, anchor_line_end, " ++
    "anchor_commit_sha, anchor_text_hash, anchor_text, " ++
    "title, slug, body, status, vendor, plan_id, task_id, " ++
    "created_at, updated_at";

const select_one_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from annotations where id = ?";

const select_all_prefix =
    "select " ++ select_columns ++ " from annotations where 1 = 1";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Annotation {
    const scope_kind_text = try stmt.columnTextAlloc(1, allocator);
    defer allocator.free(scope_kind_text);
    const scope_kind = ScopeKind.fromText(scope_kind_text) orelse return Error.QueryFailed;

    const status_text = try stmt.columnTextAlloc(12, allocator);
    defer allocator.free(status_text);
    const status = Status.fromText(status_text) orelse return Error.QueryFailed;

    return .{
        .id = stmt.columnInt(0),
        .scope_kind = scope_kind,
        .scope_id = stmt.columnIntOpt(2),
        .anchor = .{
            .path = try stmt.columnTextAlloc(3, allocator),
            .line_start = stmt.columnIntOpt(4),
            .line_end = stmt.columnIntOpt(5),
            .commit_sha = try stmt.columnTextAlloc(6, allocator),
            .text_hash = try stmt.columnTextAlloc(7, allocator),
            .text = try stmt.columnTextAlloc(8, allocator),
        },
        .title = try stmt.columnTextOpt(9, allocator),
        .slug = try stmt.columnTextOpt(10, allocator),
        .body = try stmt.columnTextAlloc(11, allocator),
        .status = status,
        .vendor = try stmt.columnTextAlloc(13, allocator),
        .plan_id = stmt.columnIntOpt(14),
        .task_id = stmt.columnIntOpt(15),
        // Tags are loaded by show()/list() in a follow-up query; readRow
        // returns an empty slice so the caller can free safely on error.
        .tags = &.{},
        .created_at = try stmt.columnTextAlloc(16, allocator),
        .updated_at = try stmt.columnTextAlloc(17, allocator),
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

test "create + show: default status active, anchor path round-trips" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{
        .anchor = .{ .path = "src/foo.zig", .line_start = 10, .line_end = 12 },
        .body = "Look here.",
    });
    defer deinit(ann, a);
    try std.testing.expectEqual(Status.active, ann.status);
    try std.testing.expectEqualStrings("src/foo.zig", ann.anchor.path);
    try std.testing.expectEqual(@as(?i64, 10), ann.anchor.line_start);
    try std.testing.expectEqual(@as(?i64, 12), ann.anchor.line_end);
    try std.testing.expectEqualStrings("Look here.", ann.body);
    try std.testing.expect(ann.title == null);
    try std.testing.expect(ann.slug == null);
}

test "create with known association scope writes scope_kind='association'" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'acme'");
    const ann = try create(&d, a, .{
        .anchor = .{ .path = "src/foo.zig" },
        .scope = "acme",
    });
    defer deinit(ann, a);
    try std.testing.expectEqual(ScopeKind.association, ann.scope_kind);
    try std.testing.expectEqual(assoc_id, ann.scope_id.?);
}

test "create with scope='global' writes scope_kind='global', scope_id=null" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{
        .anchor = .{ .path = "src/foo.zig" },
        .scope = "global",
    });
    defer deinit(ann, a);
    try std.testing.expectEqual(ScopeKind.global, ann.scope_kind);
    try std.testing.expect(ann.scope_id == null);
}

test "create with unknown scope slug returns SlugNotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(
        Error.SlugNotFound,
        create(&d, a, .{
            .anchor = .{ .path = "src/foo.zig" },
            .scope = "no-such-slug",
        }),
    );
}

test "create with repo: scope writes scope_kind='repo'" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('foo', 'Foo', '/work/foo')",
        &.{},
    );
    const repo_id = try d.intQuery("select id from projects where slug = 'foo'");
    const ann = try create(&d, a, .{
        .anchor = .{ .path = "src/foo.zig" },
        .scope = "repo:foo",
    });
    defer deinit(ann, a);
    try std.testing.expectEqual(ScopeKind.repo, ann.scope_kind);
    try std.testing.expectEqual(repo_id, ann.scope_id.?);
}

test "list with scope filter returns only matching rows" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const ann_assoc = try create(&d, a, .{ .anchor = .{ .path = "x.zig" }, .scope = "acme" });
    defer deinit(ann_assoc, a);
    const ann_global = try create(&d, a, .{ .anchor = .{ .path = "y.zig" } });
    defer deinit(ann_global, a);

    const filtered = try list(&d, a, .{ .scope = "acme" });
    defer deinitMany(filtered, a);
    try std.testing.expectEqual(@as(usize, 1), filtered.len);
    try std.testing.expectEqual(ann_assoc.id, filtered[0].id);
    try std.testing.expectEqual(ScopeKind.association, filtered[0].scope_kind);
}

/// Seed three eligible sweep candidates — one in association `acme`, one
/// in association `other`, one global — all backdated far past any cutoff
/// the callers use. Returns their ids in that order.
fn seedSweepFixture(d: *db.sqlite.Db, a: std.mem.Allocator) ![3]i64 {
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    _ = try d.execParams("insert into associations (slug, name, kind) values ('other', 'Other', 'org')", &.{});

    const in_acme = try create(d, a, .{ .anchor = .{ .path = "acme.zig" }, .scope = "acme" });
    defer deinit(in_acme, a);
    const in_other = try create(d, a, .{ .anchor = .{ .path = "other.zig" }, .scope = "other" });
    defer deinit(in_other, a);
    const in_global = try create(d, a, .{ .anchor = .{ .path = "global.zig" } });
    defer deinit(in_global, a);

    for ([_]i64{ in_acme.id, in_other.id, in_global.id }) |id| {
        const r = try resolve(d, a, id);
        deinit(r, a);
    }
    // Backdate past every cutoff the sweep tests use. `resolve` stamps
    // updated_at with `now`, and the sweep predicate is a STRICT `>`, so
    // a same-millisecond row would otherwise be ineligible at cutoff 0.
    _ = try d.execParams("update annotations set updated_at = '2000-01-01T00:00:00.000Z'", &.{});

    return .{ in_acme.id, in_other.id, in_global.id };
}

fn statusOf(d: *db.sqlite.Db, a: std.mem.Allocator, id: i64) !Status {
    const row = try show(d, a, id);
    defer deinit(row, a);
    return row.status;
}

// This is the arm that pins task 6150. `--scope` was declared on the leaf
// and ignored outright: a sweep named at one scope archived every eligible
// row in the database. It exited 0 and reported an ACCURATE count, so a
// count assertion passes just as happily against the broken behaviour —
// only asserting that the out-of-scope rows SURVIVE separates the two.
test "sweep with a scope filter leaves out-of-scope annotations untouched" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ids = try seedSweepFixture(&d, a);
    const in_acme, const in_other, const in_global = ids;

    const swept = try sweep(&d, a, .{ .since_days = 0, .scope = "acme" });

    // Survival FIRST, deliberately. The count assertion below passes just
    // as happily against the broken behaviour if it is checked first and
    // aborts the test — the arm that discriminates has to be the one that
    // runs.
    try std.testing.expectEqual(Status.resolved, try statusOf(&d, a, in_other));
    try std.testing.expectEqual(Status.resolved, try statusOf(&d, a, in_global));
    try std.testing.expectEqual(Status.archived, try statusOf(&d, a, in_acme));
    try std.testing.expectEqual(@as(usize, 1), swept);
}

test "sweep with scope='global' sweeps only global rows" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ids = try seedSweepFixture(&d, a);
    const in_acme, const in_other, const in_global = ids;

    const swept = try sweep(&d, a, .{ .since_days = 0, .scope = "global" });
    try std.testing.expectEqual(@as(usize, 1), swept);
    try std.testing.expectEqual(Status.archived, try statusOf(&d, a, in_global));
    try std.testing.expectEqual(Status.resolved, try statusOf(&d, a, in_acme));
    try std.testing.expectEqual(Status.resolved, try statusOf(&d, a, in_other));
}

test "sweep without a scope filter still spans every scope" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ids = try seedSweepFixture(&d, a);

    const swept = try sweep(&d, a, .{ .since_days = 0 });
    try std.testing.expectEqual(@as(usize, 3), swept);
    for (ids) |id| try std.testing.expectEqual(Status.archived, try statusOf(&d, a, id));
}

test "sweep with an unknown scope slug returns SlugNotFound and archives nothing" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ids = try seedSweepFixture(&d, a);

    try std.testing.expectError(Error.SlugNotFound, sweep(&d, a, .{ .since_days = 0, .scope = "nosuch" }));
    for (ids) |id| try std.testing.expectEqual(Status.resolved, try statusOf(&d, a, id));
}

test "sweep honours the staleness cutoff and skips active rows" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try seedSweepFixture(&d, a);
    // An active row is never eligible, whatever the cutoff.
    const still_active = try create(&d, a, .{ .anchor = .{ .path = "active.zig" } });
    defer deinit(still_active, a);
    _ = try d.execParams(
        "update annotations set updated_at = '2000-01-01T00:00:00.000Z' where id = ?",
        &.{.{ .int = still_active.id }},
    );

    // A cutoff far past the backdated stamp matches nothing.
    try std.testing.expectEqual(@as(usize, 0), try sweep(&d, a, .{ .since_days = 999_999 }));
    try std.testing.expectEqual(@as(usize, 3), try sweep(&d, a, .{ .since_days = 0 }));
    try std.testing.expectEqual(Status.active, try statusOf(&d, a, still_active.id));
}

test "update moves annotation to association scope" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'acme'");
    const ann = try create(&d, a, .{ .anchor = .{ .path = "x.zig" } });
    defer deinit(ann, a);
    try std.testing.expectEqual(ScopeKind.global, ann.scope_kind);

    const upd = try update(&d, a, ann.id, .{ .scope = "acme" });
    defer deinit(upd, a);
    try std.testing.expectEqual(ScopeKind.association, upd.scope_kind);
    try std.testing.expectEqual(assoc_id, upd.scope_id.?);
}

test "file-level annotation stores NULL for line columns" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{
        .anchor = .{ .path = "README.md" },
        .body = "whole-file",
    });
    defer deinit(ann, a);
    try std.testing.expect(ann.anchor.line_start == null);
    try std.testing.expect(ann.anchor.line_end == null);

    // Verify the SQL value is actually NULL, not zero.
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery(
            "select count(*) from annotations " ++
                "where anchor_line_start is null and anchor_line_end is null",
        ),
    );
}

test "resolve / dismiss / archive each set status and record audit row" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const a1 = try create(&d, a, .{ .anchor = .{ .path = "x.zig" } });
    defer deinit(a1, a);
    const r = try resolve(&d, a, a1.id);
    defer deinit(r, a);
    try std.testing.expectEqual(Status.resolved, r.status);

    const a2 = try create(&d, a, .{ .anchor = .{ .path = "y.zig" } });
    defer deinit(a2, a);
    const dm = try dismiss(&d, a, a2.id);
    defer deinit(dm, a);
    try std.testing.expectEqual(Status.dismissed, dm.status);

    const a3 = try create(&d, a, .{ .anchor = .{ .path = "z.zig" } });
    defer deinit(a3, a);
    const ar = try archive(&d, a, a3.id);
    defer deinit(ar, a);
    try std.testing.expectEqual(Status.archived, ar.status);

    try std.testing.expectEqual(
        @as(i64, 3),
        try d.intQuery(
            "select count(*) from audit_log where verb='status_change' and entity_kind='annotation'",
        ),
    );
}

test "transition: resolved → archived is legal (retention-tier model)" {
    // Under plan 692: resolved and dismissed are outcome states that may
    // still progress to archived.  archive() from resolved must SUCCEED.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{ .anchor = .{ .path = "x.zig" } });
    defer deinit(ann, a);
    const r = try resolve(&d, a, ann.id);
    defer deinit(r, a);
    try std.testing.expectEqual(Status.resolved, r.status);
    // resolve → archive is now legal.
    const ar = try archive(&d, a, ann.id);
    defer deinit(ar, a);
    try std.testing.expectEqual(Status.archived, ar.status);
}

test "transition: dismissed → archived is legal (retention-tier model)" {
    // Under plan 692: dismissed may progress to archived.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{ .anchor = .{ .path = "y.zig" } });
    defer deinit(ann, a);
    const dm = try dismiss(&d, a, ann.id);
    defer deinit(dm, a);
    try std.testing.expectEqual(Status.dismissed, dm.status);
    const ar = try archive(&d, a, ann.id);
    defer deinit(ar, a);
    try std.testing.expectEqual(Status.archived, ar.status);
}

test "transition: archived is the sole final state — no outgoing edges" {
    // archived → anything must be refused (TerminalStatus).
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{ .anchor = .{ .path = "z.zig" } });
    defer deinit(ann, a);
    const ar = try archive(&d, a, ann.id);
    defer deinit(ar, a);
    try std.testing.expectError(Error.TerminalStatus, resolve(&d, a, ann.id));
    try std.testing.expectError(Error.TerminalStatus, dismiss(&d, a, ann.id));
}

test "transition: resolved → dismiss is still refused (no lateral move)" {
    // resolved cannot cross to dismissed — only → archived is legal.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{ .anchor = .{ .path = "w.zig" } });
    defer deinit(ann, a);
    const r = try resolve(&d, a, ann.id);
    defer deinit(r, a);
    try std.testing.expectError(Error.TerminalStatus, dismiss(&d, a, ann.id));
}

test "list filters by anchor_path, status, vendor" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const a1 = try create(&d, a, .{
        .anchor = .{ .path = "src/foo.zig" },
        .vendor = "claude",
    });
    defer deinit(a1, a);
    const a2 = try create(&d, a, .{
        .anchor = .{ .path = "src/bar.zig" },
        .vendor = "codex",
    });
    defer deinit(a2, a);
    const r = try resolve(&d, a, a2.id);
    defer deinit(r, a);

    const by_path = try list(&d, a, .{ .anchor_path = "src/foo.zig" });
    defer deinitMany(by_path, a);
    try std.testing.expectEqual(@as(usize, 1), by_path.len);

    const by_status = try list(&d, a, .{ .status = .resolved });
    defer deinitMany(by_status, a);
    try std.testing.expectEqual(@as(usize, 1), by_status.len);
    try std.testing.expectEqual(a2.id, by_status[0].id);

    const by_vendor = try list(&d, a, .{ .vendor = "claude" });
    defer deinitMany(by_vendor, a);
    try std.testing.expectEqual(@as(usize, 1), by_vendor.len);
    try std.testing.expectEqual(a1.id, by_vendor[0].id);
}

test "addTag + removeTag + listTags round-trip" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{ .anchor = .{ .path = "x.zig" } });
    defer deinit(ann, a);

    try addTag(&d, a, ann.id, "performance");
    try addTag(&d, a, ann.id, "follow-up");
    // Duplicate is a no-op.
    try addTag(&d, a, ann.id, "performance");

    {
        const tags = try listTags(&d, a, ann.id);
        defer {
            for (tags) |t| a.free(t);
            a.free(tags);
        }
        try std.testing.expectEqual(@as(usize, 2), tags.len);
        try std.testing.expectEqualStrings("follow-up", tags[0]);
        try std.testing.expectEqualStrings("performance", tags[1]);
    }

    try removeTag(&d, ann.id, "performance");
    {
        const tags = try listTags(&d, a, ann.id);
        defer {
            for (tags) |t| a.free(t);
            a.free(tags);
        }
        try std.testing.expectEqual(@as(usize, 1), tags.len);
        try std.testing.expectEqualStrings("follow-up", tags[0]);
    }

    // Removing a missing tag is a no-op.
    try removeTag(&d, ann.id, "performance");
}

test "create with tags dedupes and trims input" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{
        .anchor = .{ .path = "x.zig" },
        .tags = &.{ "bug", "bug", " fixme ", "" },
    });
    defer deinit(ann, a);
    try std.testing.expectEqual(@as(usize, 2), ann.tags.len);
    try std.testing.expectEqualStrings("bug", ann.tags[0]);
    try std.testing.expectEqualStrings("fixme", ann.tags[1]);
}

test "showBySlug resolves and returns NotFound for missing" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{
        .anchor = .{ .path = "x.zig" },
        .slug = "investigate-foo",
    });
    defer deinit(ann, a);

    const by_slug = try showBySlug(&d, a, "investigate-foo");
    defer deinit(by_slug, a);
    try std.testing.expectEqual(ann.id, by_slug.id);

    try std.testing.expectError(Error.NotFound, showBySlug(&d, a, "does-not-exist"));
}

test "duplicate slug raises SlugConflict" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const a1 = try create(&d, a, .{
        .anchor = .{ .path = "x.zig" },
        .slug = "dup",
    });
    defer deinit(a1, a);
    try std.testing.expectError(
        Error.SlugConflict,
        create(&d, a, .{ .anchor = .{ .path = "y.zig" }, .slug = "dup" }),
    );
}

test "update with status records status_change; pure update records update" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{ .anchor = .{ .path = "x.zig" } });
    defer deinit(ann, a);
    const upd1 = try update(&d, a, ann.id, .{ .body = "new body" });
    defer deinit(upd1, a);
    const upd2 = try update(&d, a, ann.id, .{ .status = .resolved });
    defer deinit(upd2, a);

    try std.testing.expectEqualStrings("new body", upd2.body);
    try std.testing.expectEqual(Status.resolved, upd2.status);
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='update' and entity_kind='annotation'"),
    );
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='status_change' and entity_kind='annotation'"),
    );
}

test "remove cascades to annotation_tags" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const ann = try create(&d, a, .{
        .anchor = .{ .path = "x.zig" },
        .tags = &.{ "a", "b" },
    });
    defer deinit(ann, a);
    try remove(&d, a, ann.id);

    try std.testing.expectEqual(
        @as(i64, 0),
        try d.intQuery("select count(*) from annotations"),
    );
    try std.testing.expectEqual(
        @as(i64, 0),
        try d.intQuery("select count(*) from annotation_tags"),
    );
    try std.testing.expectError(Error.NotFound, show(&d, a, ann.id));
}

test "show returns NotFound for missing id" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
}

// ---- Task 2227: list() filter combinations not covered above ----

test "list filter by plan_id returns only annotations on that plan" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // annotations.plan_id references plans(id) — insert real plan rows.
    const plan1_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Plan One', 'plan-one', 'active')",
        &.{},
    );
    const plan2_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Plan Two', 'plan-two', 'active')",
        &.{},
    );

    const ann_on_plan1 = try create(&d, a, .{
        .anchor = .{ .path = "src/a.zig" },
        .plan_id = plan1_id,
    });
    defer deinit(ann_on_plan1, a);
    const ann_on_plan2 = try create(&d, a, .{
        .anchor = .{ .path = "src/b.zig" },
        .plan_id = plan2_id,
    });
    defer deinit(ann_on_plan2, a);
    const ann_no_plan = try create(&d, a, .{
        .anchor = .{ .path = "src/c.zig" },
    });
    defer deinit(ann_no_plan, a);

    const by_plan1 = try list(&d, a, .{ .plan_id = plan1_id });
    defer deinitMany(by_plan1, a);
    try std.testing.expectEqual(@as(usize, 1), by_plan1.len);
    try std.testing.expectEqual(ann_on_plan1.id, by_plan1[0].id);
}

test "list filter by task_id returns only annotations on that task" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // annotations.task_id references tasks(id) — insert real task rows.
    const task1_id = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global', 'Task Alpha', 'todo', 100)",
        &.{},
    );
    const task2_id = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global', 'Task Beta', 'todo', 100)",
        &.{},
    );

    const ann_on_task1 = try create(&d, a, .{
        .anchor = .{ .path = "src/x.zig" },
        .task_id = task1_id,
    });
    defer deinit(ann_on_task1, a);
    const ann_on_task2 = try create(&d, a, .{
        .anchor = .{ .path = "src/y.zig" },
        .task_id = task2_id,
    });
    defer deinit(ann_on_task2, a);
    const ann_no_task = try create(&d, a, .{
        .anchor = .{ .path = "src/z.zig" },
    });
    defer deinit(ann_no_task, a);

    const by_task1 = try list(&d, a, .{ .task_id = task1_id });
    defer deinitMany(by_task1, a);
    try std.testing.expectEqual(@as(usize, 1), by_task1.len);
    try std.testing.expectEqual(ann_on_task1.id, by_task1[0].id);
}

test "list filter by tag returns only annotations carrying that tag" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const tags_foo = [_][]const u8{"foo"};
    const tags_bar = [_][]const u8{"bar"};

    const ann_foo = try create(&d, a, .{
        .anchor = .{ .path = "src/tagged_foo.zig" },
        .tags = &tags_foo,
    });
    defer deinit(ann_foo, a);
    const ann_bar = try create(&d, a, .{
        .anchor = .{ .path = "src/tagged_bar.zig" },
        .tags = &tags_bar,
    });
    defer deinit(ann_bar, a);
    const ann_untagged = try create(&d, a, .{
        .anchor = .{ .path = "src/untagged.zig" },
    });
    defer deinit(ann_untagged, a);

    const by_foo = try list(&d, a, .{ .tag = "foo" });
    defer deinitMany(by_foo, a);
    try std.testing.expectEqual(@as(usize, 1), by_foo.len);
    try std.testing.expectEqual(ann_foo.id, by_foo[0].id);

    const by_bar = try list(&d, a, .{ .tag = "bar" });
    defer deinitMany(by_bar, a);
    try std.testing.expectEqual(@as(usize, 1), by_bar.len);
    try std.testing.expectEqual(ann_bar.id, by_bar[0].id);
}
