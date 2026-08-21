//! engine/planning/plan — Plan entity: CRUD against the `plans` table.
//!
//! Scope handling: when `scope` is provided in CreateArgs/ListFilter/UpdateArgs,
//! it is resolved via engine.identity.scope.resolveSlug to a (kind, id) pair
//! and stored/filtered in scope_kind/scope_id columns. The "global" literal,
//! bare association slugs ("acme", "assoc:acme"), and repo slugs
//! ("repo:acme") are supported. Unknown slugs return SlugNotFound.

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
    active,
    paused,
    done,
    abandoned,

    pub fn fromText(s: []const u8) ?Status {
        if (std.mem.eql(u8, s, "draft")) return .draft;
        if (std.mem.eql(u8, s, "active")) return .active;
        if (std.mem.eql(u8, s, "paused")) return .paused;
        if (std.mem.eql(u8, s, "done")) return .done;
        if (std.mem.eql(u8, s, "abandoned")) return .abandoned;
        return null;
    }
};

/// One row from the `plans` table. String fields are owned by the
/// allocator passed to the function that produced this Plan — the
/// caller frees them (or lets the process arena reclaim them).
pub const Plan = struct {
    id: i64,
    scope_kind: ScopeKind,
    scope_id: ?i64,
    title: []const u8,
    slug: []const u8,
    summary: ?[]const u8,
    status: Status,
    parent_plan_id: ?i64,
    created_at: []const u8,
    updated_at: []const u8,
};

pub const CreateArgs = struct {
    title: []const u8,
    /// Optional explicit slug. Derived from `title` if null.
    slug: ?[]const u8 = null,
    summary: ?[]const u8 = null,
    status: Status = .draft,
    parent_plan_id: ?i64 = null,
    /// Scope slug accepted by identity.scope.resolveSlug.
    scope: ?[]const u8 = null,
};

pub const UpdateArgs = struct {
    title: ?[]const u8 = null,
    slug: ?[]const u8 = null,
    summary: ?[]const u8 = null,
    status: ?Status = null,
    parent_plan_id: ?i64 = null,
    clear_parent: bool = false,
    /// Scope slug accepted by identity.scope.resolveSlug.
    scope: ?[]const u8 = null,
};

/// Release the allocator-owned string fields on a Plan. Engine
/// internal callers (e.g. `update` snapshotting before mutating) and
/// test code call this on every Plan they don't otherwise hand back
/// to the caller.
pub fn deinit(plan: Plan, allocator: std.mem.Allocator) void {
    allocator.free(plan.title);
    allocator.free(plan.slug);
    if (plan.summary) |s| allocator.free(s);
    allocator.free(plan.created_at);
    allocator.free(plan.updated_at);
}

/// Free a slice of Plans plus the slice header itself.
pub fn deinitMany(plans: []const Plan, allocator: std.mem.Allocator) void {
    for (plans) |p| deinit(p, allocator);
    allocator.free(plans);
}

pub const ListFilter = struct {
    statuses: []const Status = &.{},
    parent_plan_id: ?i64 = null,
    /// Legacy single-scope filter retained for handler compatibility.
    scope: ?[]const u8 = null,
    /// Multi-scope filter. Values are scope slugs accepted by resolveSlug().
    scopes: []const []const u8 = &.{},
};

pub const Error =
    error{
        NotFound,
        SlugConflict,
        UnsupportedScope,
        SlugNotFound,
        InvalidStatus,
        InvalidParentCycle,
        QueryFailed,
    } ||
    std.mem.Allocator.Error ||
    policy.scope_guard.Error ||
    policy.status.Error ||
    policy.audit.Error;

const session = @import("../runtime/session.zig");

// =========================================================================
// CRUD
// =========================================================================

pub fn create(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: CreateArgs) Error!Plan {
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

    const slug = if (args.slug) |s| try allocator.dupe(u8, s) else try slugify(allocator, args.title);
    defer allocator.free(slug); // bound by reference into execParams; safe to free after.

    const insert_sql: [:0]const u8 =
        \\insert into plans (scope_kind, scope_id, title, slug, summary, status, parent_plan_id)
        \\values (?, ?, ?, ?, ?, ?, ?)
    ;
    const id = d.execParams(insert_sql, &.{
        .{ .text = scope_kind_str },
        if (scope_ref.id) |sid| .{ .int = sid } else .{ .null = {} },
        .{ .text = args.title },
        .{ .text = slug },
        if (args.summary) |s| .{ .text = s } else .{ .null = {} },
        .{ .text = @tagName(args.status) },
        if (args.parent_plan_id) |p| .{ .int = p } else .{ .null = {} },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.SlugConflict;
        std.log.err("plan.create exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const summary = try summarizeCreate(allocator, args.title);
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "plan", .id = id },
        .scope = null,
        .summary = summary,
    });

    return try show(d, allocator, id);
}

pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Plan {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => return Error.NotFound,
        .row => return try readRow(&stmt, allocator),
    }
}

pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, filter: ListFilter) Error![]Plan {
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
        try sql_buf.appendSlice(allocator, " and status in ('draft','active','paused')");
    } else {
        try sql_buf.appendSlice(allocator, " and status in (");
        for (filter.statuses, 0..) |s, i| {
            if (i > 0) try sql_buf.appendSlice(allocator, ",");
            try sql_buf.appendSlice(allocator, "?");
            try params.append(allocator, .{ .text = @tagName(s) });
        }
        try sql_buf.appendSlice(allocator, ")");
    }
    if (filter.parent_plan_id) |p| {
        try sql_buf.appendSlice(allocator, " and parent_plan_id = ?");
        try params.append(allocator, .{ .int = p });
    }
    if (scope_refs.items.len > 0) {
        try sql_buf.appendSlice(allocator, " and (");
        for (scope_refs.items, 0..) |ref, i| {
            if (i > 0) try sql_buf.appendSlice(allocator, " or ");
            switch (ref.kind) {
                .global => try sql_buf.appendSlice(allocator, "scope_kind = 'global'"),
                .association => {
                    try sql_buf.appendSlice(allocator, "(scope_kind = 'association' and scope_id = ?)");
                    try params.append(allocator, .{ .int = ref.id.? });
                },
                .repo => {
                    try sql_buf.appendSlice(allocator, "(scope_kind = 'repo' and scope_id = ?)");
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

    var out: std.ArrayList(Plan) = .empty;
    errdefer {
        // Free any rows accumulated before the failure.
        for (out.items) |p| deinit(p, allocator);
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

/// listTouching returns plans scoped directly to repo_id OR linked by
/// entity_links(relationship='touches') to repo_id, with additional filters.
pub fn listTouching(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    repo_id: i64,
    filter: ListFilter,
) Error![]Plan {
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
        if (filter.parent_plan_id) |_| {
            try sql.appendSlice(allocator, " and parent_plan_id=?");
        }
        if (filter.statuses.len == 0) {
            try sql.appendSlice(allocator, " and status in ('draft','active','paused')");
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
    try sql.appendSlice(allocator, " and id in (select from_id from entity_links where from_kind='plan' and to_kind='repo' and to_id=? and relationship='touches')");
    if (filter.parent_plan_id) |_| {
        try sql.appendSlice(allocator, " and parent_plan_id=?");
    }
    if (filter.statuses.len == 0) {
        try sql.appendSlice(allocator, " and status in ('draft','active','paused')");
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
                .global => try sql.appendSlice(allocator, "scope_kind = 'global'"),
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
        if (filter.parent_plan_id) |p| try params.append(allocator, .{ .int = p });
        for (filter.statuses) |s| try params.append(allocator, .{ .text = @tagName(s) });
    }
    try params.append(allocator, .{ .int = repo_id });
    if (filter.parent_plan_id) |p| try params.append(allocator, .{ .int = p });
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

    var out: std.ArrayList(Plan) = .empty;
    errdefer {
        for (out.items) |p| deinit(p, allocator);
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

/// childPlans returns direct children of a plan.
pub fn childPlans(d: *db.sqlite.Db, allocator: std.mem.Allocator, parent_id: i64) Error![]Plan {
    return list(d, allocator, .{
        .parent_plan_id = parent_id,
        .statuses = &.{ .draft, .active, .paused, .done, .abandoned },
    });
}

pub fn update(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    patch: UpdateArgs,
) Error!Plan {
    const scope_ref: ?identity.scope.ScopeRef = if (patch.scope) |s|
        identity.scope.resolveSlug(d, allocator, s) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        }
    else
        null;

    d.savepoint(allocator, "plan_update") catch return Error.QueryFailed;
    var update_done = false;
    defer if (!update_done) {
        d.rollbackToSavepoint(allocator, "plan_update") catch {};
        d.releaseSavepoint(allocator, "plan_update") catch {};
    };

    const current = try show(d, allocator, id);
    defer deinit(current, allocator); // snapshot — not returned to caller.
    try policy.scope_guard.check(null, null);

    if (patch.status) |new_status| {
        try policy.status.check(.plan, @tagName(current.status), @tagName(new_status), false);
    }
    if (patch.parent_plan_id) |parent_id| {
        if (try wouldCreateParentCycle(d, id, parent_id)) return Error.InvalidParentCycle;
    }

    // Build the SET clause dynamically so untouched columns keep their
    // value (and updated_at refreshes to now). Same readability tradeoff
    // as in `list`.
    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);
    try sql_buf.appendSlice(allocator, "update plans set ");

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
    if (patch.summary) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "summary = ?");
        try params.append(allocator, .{ .text = s });
    }
    if (patch.status) |s| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "status = ?");
        try params.append(allocator, .{ .text = @tagName(s) });
    }
    if (patch.parent_plan_id) |p| {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "parent_plan_id = ?");
        try params.append(allocator, .{ .int = p });
    } else if (patch.clear_parent) {
        try appendSep(&sql_buf, &first, allocator);
        try sql_buf.appendSlice(allocator, "parent_plan_id = null");
    }

    // No-op update — `current` is already deferred-freed above, so
    // refetch a fresh snapshot for the caller rather than handing out
    // an about-to-be-dangling Plan.
    if (first) {
        d.releaseSavepoint(allocator, "plan_update") catch return Error.QueryFailed;
        update_done = true;
        return try show(d, allocator, id);
    }

    try appendSep(&sql_buf, &first, allocator);
    try sql_buf.appendSlice(allocator, "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
    try params.append(allocator, .{ .int = id });

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    _ = d.execParams(sql_z, params.items) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.SlugConflict;
        std.log.err("plan.update exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const verb: policy.audit.Verb = if (patch.status != null) .status_change else .update;
    try policy.audit.record(d, .{
        .verb = verb,
        .entity = .{ .kind = "plan", .id = id },
        .scope = null,
        .summary = null,
    });
    const updated = try show(d, allocator, id);
    errdefer deinit(updated, allocator);
    d.releaseSavepoint(allocator, "plan_update") catch return Error.QueryFailed;
    update_done = true;
    return updated;
}

fn wouldCreateParentCycle(d: *db.sqlite.Db, plan_id: i64, parent_id: i64) Error!bool {
    var stmt = d.prepare(
        \\with recursive ancestors(id, parent_plan_id) as (
        \\  select id, parent_plan_id from plans where id = ?
        \\  union
        \\  select p.id, p.parent_plan_id from plans p
        \\  join ancestors a on p.id = a.parent_plan_id
        \\)
        \\select count(*) from ancestors where id = ?
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = parent_id }, .{ .int = plan_id } }) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => false,
        .row => stmt.columnInt(0) > 0,
    };
}

// =========================================================================
// Output rendering — called by output.emit when --json is off.
// =========================================================================

pub fn renderText(plan: Plan, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("id:       {d}\n", .{plan.id});
    try writer.print("title:    {s}\n", .{plan.title});
    try writer.print("slug:     {s}\n", .{plan.slug});
    try writer.print("status:   {s}\n", .{@tagName(plan.status)});
    try writer.print("scope:    {s}", .{@tagName(plan.scope_kind)});
    if (plan.scope_id) |sid| {
        try writer.print(":{d}", .{sid});
    }
    try writer.print("\n", .{});
    if (plan.parent_plan_id) |pid| {
        try writer.print("parent:   {d}\n", .{pid});
    }
    if (plan.summary) |s| {
        try writer.print("summary:  {s}\n", .{s});
    }
    try writer.print("created:  {s}\n", .{plan.created_at});
    try writer.print("updated:  {s}\n", .{plan.updated_at});
}

/// Render a list of plans as a one-line-per-plan table. Columns:
/// id, status, slug, title.
pub fn renderListText(plans: []const Plan, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    if (plans.len == 0) {
        try writer.print("(no plans)\n", .{});
        return;
    }
    for (plans) |p| {
        // {d:>5} on signed i64 emits a leading '+' on positive values
        // (Zig 0.16 behavior); cast to unsigned for the display width.
        try writer.print("{d:>5}  {s:<10}  {s:<24}  {s}\n", .{
            @as(u64, @intCast(p.id)), @tagName(p.status), p.slug, p.title,
        });
    }
}

// =========================================================================
// Internals
// =========================================================================

const select_columns = "id, scope_kind, scope_id, title, slug, summary, status, parent_plan_id, created_at, updated_at";

const select_one_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from plans where id = ?";

const select_all_prefix =
    "select " ++ select_columns ++ " from plans where 1 = 1";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Plan {
    const scope_kind_text = try stmt.columnTextAlloc(1, allocator);
    defer allocator.free(scope_kind_text);
    const scope_kind = ScopeKind.fromText(scope_kind_text) orelse return Error.QueryFailed;

    const status_text = try stmt.columnTextAlloc(6, allocator);
    defer allocator.free(status_text);
    const status = Status.fromText(status_text) orelse return Error.QueryFailed;

    return .{
        .id = stmt.columnInt(0),
        .scope_kind = scope_kind,
        .scope_id = stmt.columnIntOpt(2),
        .title = try stmt.columnTextAlloc(3, allocator),
        .slug = try stmt.columnTextAlloc(4, allocator),
        .summary = try stmt.columnTextOpt(5, allocator),
        .status = status,
        .parent_plan_id = stmt.columnIntOpt(7),
        .created_at = try stmt.columnTextAlloc(8, allocator),
        .updated_at = try stmt.columnTextAlloc(9, allocator),
    };
}

// =========================================================================
// recomputeStatus — plan-status auto-promotion invariant (plan 304)
// =========================================================================

/// The task-status histogram for a plan. Mirrors Go's TaskAggregate.
const TaskAggregate = struct {
    todo: i64 = 0,
    doing: i64 = 0,
    blocked: i64 = 0,
    done: i64 = 0,
    cancelled: i64 = 0,

    fn total(self: TaskAggregate) i64 {
        return self.todo + self.doing + self.blocked + self.done + self.cancelled;
    }

    /// True when every task is terminal (done/cancelled) AND plan is non-empty.
    fn allTerminal(self: TaskAggregate) bool {
        return self.total() > 0 and self.todo == 0 and self.doing == 0 and self.blocked == 0;
    }

    /// True when at least one task is mid-flight (doing or blocked).
    fn anyActive(self: TaskAggregate) bool {
        return self.doing > 0 or self.blocked > 0;
    }
};

/// The result of a recomputeStatus call.
///
/// Single-plan only (D-recompute-single-plan): this covers exactly the
/// plan_id passed in. Multi-plan "--all" walks are a Cycle B handler
/// concern, not an engine concern. This matches Go's RecomputeStatus
/// which operates on one plan and never walks parent_plan_id.
pub const RecomputeResult = struct {
    plan_id: i64,
    status_before: Status,
    status_after: Status,
    flipped: bool,

    pub fn deinit(self: RecomputeResult, allocator: std.mem.Allocator) void {
        _ = self;
        _ = allocator;
    }
};

/// Recompute the auto-promotion status for THIS plan only.
///
/// Applies the transition matrix (mirrors Go's recompute.go exactly):
///
///   from \ aggregate    | empty | all todo | any active | all terminal
///   --------------------+-------+----------+------------+--------------
///   draft               | -     | -        | active     | done*
///   active              | -     | -        | -          | done*
///   paused              | -     | -        | -          | -   (operator)
///   done                | -     | active   | active     | -
///   abandoned           | -     | -        | -          | -   (terminal)
///
///   * anchor plans (parent_plan_id IS NULL) NEVER auto-promote to done;
///     they auto-promote to active at most.
///
/// Single-plan semantics (D-recompute-single-plan): reads only tasks WHERE
/// plan_id = ?, updates only this plan, never walks parent_plan_id. Multi-
/// plan "--all" walks are a Cycle B handler concern. This matches Go's
/// RecomputeStatus which is single-plan by design.
///
/// On transition, an audit_log row is recorded (always) and a best-effort
/// session_entries note is appended with prefix='note' and a body whose
/// first line is "plan_status: <id>" — the grep-recoverable forensic
/// sentinel that matches Go's formatStatusEntryBody contract.
///
/// Idempotent: calling on an already-correct plan is a no-op (no write,
/// flipped=false).
pub fn recomputeStatus(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) Error!RecomputeResult {
    // Read current plan context (status, is_anchor). No child plan walk.
    const PlanCtx = struct { status: Status, is_anchor: bool, title: []const u8 };
    const ctx = blk: {
        var stmt = d.prepare(
            "select status, parent_plan_id, title from plans where id = ?",
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return Error.NotFound,
            .row => {
                const st_text = try stmt.columnTextAlloc(0, allocator);
                defer allocator.free(st_text);
                const st = Status.fromText(st_text) orelse return Error.QueryFailed;
                const parent = stmt.columnIntOpt(1);
                const title = try stmt.columnTextAlloc(2, allocator);
                break :blk PlanCtx{
                    .status = st,
                    .is_anchor = (parent == null),
                    .title = title,
                };
            },
        }
    };
    defer allocator.free(ctx.title);

    // Paused and abandoned are strict no-ops regardless of aggregate.
    if (ctx.status == .paused or ctx.status == .abandoned) {
        return RecomputeResult{
            .plan_id = plan_id,
            .status_before = ctx.status,
            .status_after = ctx.status,
            .flipped = false,
        };
    }

    // Build task aggregate — only tasks WHERE plan_id = ? (no child plans join).
    const agg = try readTaskAggregate(d, allocator, plan_id);

    // Compute target. Null = no transition warranted.
    const target_opt = computeTarget(ctx.status, agg, ctx.is_anchor);
    if (target_opt == null or target_opt.? == ctx.status) {
        return RecomputeResult{
            .plan_id = plan_id,
            .status_before = ctx.status,
            .status_after = ctx.status,
            .flipped = false,
        };
    }

    const target = target_opt.?;

    // Apply the UPDATE — INTENTIONAL bypass of policy.status.check.
    //
    // recomputeStatus is an engine-internal aggregate roll-up driven by
    // computeTarget(), not an operator transition.  computeTarget() only
    // emits edges that the aggregate matrix considers valid (e.g. active →
    // done when all tasks are terminal, draft → active when the first task
    // becomes active), so it cannot produce an illegal transition by
    // construction.  Routing it through the operator-transition validator
    // would add noise with no safety benefit — the validator is there to
    // catch illegal operator inputs, not internal engine moves.
    //
    // Mirrors Go's recompute.go apply path (plan 692 decision: bypass stays).
    _ = d.execParams(
        "update plans set status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
        &.{ .{ .text = @tagName(target) }, .{ .int = plan_id } },
    ) catch return Error.QueryFailed;

    // audit_log row (always-required signal).
    const audit_summary = try std.fmt.allocPrint(
        allocator,
        "recompute plan {d}: {s} → {s}; tasks todo={d} doing={d} blocked={d} done={d} cancelled={d}",
        .{ plan_id, @tagName(ctx.status), @tagName(target), agg.todo, agg.doing, agg.blocked, agg.done, agg.cancelled },
    );
    defer allocator.free(audit_summary);
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "plan", .id = plan_id },
        .summary = audit_summary,
    });

    // session_entries note — best-effort forensic sentinel (F2 / Go contract).
    // Body schema mirrors Go's formatStatusEntryBody:
    //   plan_status: <id>
    //   plan_title: <title>
    //   from_status: <status>
    //   to_status: <status>
    //   trigger: recompute
    //   trigger_task_id: 0
    //   task_aggregate: todo=N doing=N blocked=N done=N cancelled=N
    // First line "plan_status: <id>" is the grep-recoverable sentinel:
    //   planar audit trail <plan> --grep "^plan_status:"
    emitStatusSessionEntry(d, allocator, plan_id, ctx.title, ctx.status, target, agg) catch {};

    return RecomputeResult{
        .plan_id = plan_id,
        .status_before = ctx.status,
        .status_after = target,
        .flipped = true,
    };
}

/// Emit a session_entries note row for a plan-status transition.
/// Mirrors Go's emitStatusSessionEntry: best-effort, silently no-ops when
/// no active session exists (e.g., test fixtures that skip session schema).
fn emitStatusSessionEntry(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
    title: []const u8,
    from: Status,
    to: Status,
    agg: TaskAggregate,
) !void {
    // Resolve the active session id (most-recent open session).
    var session_id: i64 = undefined;
    {
        var stmt = d.prepare(
            "select id from sessions where ended_at is null order by id desc limit 1",
        ) catch return;
        defer stmt.finalize();
        stmt.bind(&.{}) catch return;
        switch (stmt.step() catch return) {
            .done => return, // no active session — silently skip
            .row => session_id = stmt.columnInt(0),
        }
    }

    // Build structured body matching Go's formatStatusEntryBody verbatim.
    const body = try std.fmt.allocPrint(
        allocator,
        "plan_status: {d}\nplan_title: {s}\nfrom_status: {s}\nto_status: {s}\ntrigger: recompute\ntrigger_task_id: 0\ntask_aggregate: todo={d} doing={d} blocked={d} done={d} cancelled={d}\n",
        .{ plan_id, title, @tagName(from), @tagName(to), agg.todo, agg.doing, agg.blocked, agg.done, agg.cancelled },
    );
    defer allocator.free(body);

    // Append via session helper (best-effort; any error is swallowed by caller).
    try session.appendEntry(d, session_id, "note", body);
}

fn readTaskAggregate(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) Error!TaskAggregate {
    var agg = TaskAggregate{};
    var stmt = d.prepare(
        "select status, count(*) from tasks where plan_id = ? group by status",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const st = try stmt.columnTextAlloc(0, allocator);
                defer allocator.free(st);
                const n = stmt.columnInt(1);
                if (std.mem.eql(u8, st, "todo")) {
                    agg.todo = n;
                } else if (std.mem.eql(u8, st, "doing")) {
                    agg.doing = n;
                } else if (std.mem.eql(u8, st, "blocked")) {
                    agg.blocked = n;
                } else if (std.mem.eql(u8, st, "done")) {
                    agg.done = n;
                } else if (std.mem.eql(u8, st, "cancelled")) {
                    agg.cancelled = n;
                }
            },
        }
    }
    return agg;
}

/// Compute the target status. Returns null when no transition is warranted.
fn computeTarget(current: Status, agg: TaskAggregate, is_anchor: bool) ?Status {
    if (agg.total() == 0) return null;
    return switch (current) {
        .draft => {
            if (agg.allTerminal()) {
                return if (is_anchor) .active else .done;
            }
            if (agg.anyActive()) return .active;
            return null;
        },
        .active => {
            if (agg.allTerminal()) {
                return if (is_anchor) null else .done;
            }
            return null;
        },
        .done => {
            // Reverse transition: task moved out of terminal state.
            if (!agg.allTerminal()) return .active;
            return null;
        },
        else => null,
    };
}

fn slugify(allocator: std.mem.Allocator, title: []const u8) std.mem.Allocator.Error![]const u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var last_dash = true; // suppress leading dashes
    for (title) |ch| {
        const lower = std.ascii.toLower(ch);
        if (std.ascii.isAlphanumeric(lower)) {
            try out.append(allocator, lower);
            last_dash = false;
        } else if (!last_dash) {
            try out.append(allocator, '-');
            last_dash = true;
        }
    }
    // Strip trailing dash if any.
    if (out.items.len > 0 and out.items[out.items.len - 1] == '-') {
        _ = out.pop();
    }
    if (out.items.len == 0) try out.append(allocator, '_');
    return try out.toOwnedSlice(allocator);
}

fn summarizeCreate(allocator: std.mem.Allocator, title: []const u8) std.mem.Allocator.Error![]const u8 {
    return try std.fmt.allocPrint(allocator, "create plan '{s}'", .{title});
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

test "slugify produces sensible slugs" {
    const a = std.testing.allocator;
    {
        const s = try slugify(a, "Hello World!");
        defer a.free(s);
        try std.testing.expectEqualStrings("hello-world", s);
    }
    {
        const s = try slugify(a, "  --weird  Title   ");
        defer a.free(s);
        try std.testing.expectEqualStrings("weird-title", s);
    }
    {
        const s = try slugify(a, "!!!");
        defer a.free(s);
        try std.testing.expectEqualStrings("_", s);
    }
}

test "create + show round-trip a global plan" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try create(&d, a, .{ .title = "First plan", .summary = "A test." });
    defer deinit(p, a);

    try std.testing.expectEqualStrings("First plan", p.title);
    try std.testing.expectEqualStrings("first-plan", p.slug);
    try std.testing.expectEqual(Status.draft, p.status);
    try std.testing.expectEqual(ScopeKind.global, p.scope_kind);
    try std.testing.expect(p.scope_id == null);
    try std.testing.expect(p.summary != null);
    try std.testing.expectEqualStrings("A test.", p.summary.?);

    const fetched = try show(&d, a, p.id);
    defer deinit(fetched, a);
    try std.testing.expectEqual(p.id, fetched.id);
}

test "create with known association scope writes scope_kind='association'" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'acme'");
    const p = try create(&d, a, .{ .title = "scoped plan", .scope = "acme" });
    defer deinit(p, a);
    try std.testing.expectEqual(ScopeKind.association, p.scope_kind);
    try std.testing.expectEqual(assoc_id, p.scope_id.?);
}

test "create with scope='global' writes scope_kind='global', scope_id=null" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const p = try create(&d, a, .{ .title = "global plan", .scope = "global" });
    defer deinit(p, a);
    try std.testing.expectEqual(ScopeKind.global, p.scope_kind);
    try std.testing.expect(p.scope_id == null);
}

test "create with unknown scope slug returns SlugNotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(
        Error.SlugNotFound,
        create(&d, a, .{ .title = "x", .scope = "no-such-slug" }),
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
    const p = try create(&d, a, .{ .title = "repo plan", .scope = "repo:foo" });
    defer deinit(p, a);
    try std.testing.expectEqual(ScopeKind.repo, p.scope_kind);
    try std.testing.expectEqual(repo_id, p.scope_id.?);
}

test "create returns SlugConflict on duplicate slug" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try create(&d, a, .{ .title = "Same title" });
    defer deinit(p1, a);
    try std.testing.expectError(
        Error.SlugConflict,
        create(&d, a, .{ .title = "Same title" }),
    );
}

test "list returns plans in id order" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try create(&d, a, .{ .title = "Plan one" });
    defer deinit(p1, a);
    const p2 = try create(&d, a, .{ .title = "Plan two" });
    defer deinit(p2, a);

    const all = try list(&d, a, .{});
    defer deinitMany(all, a);
    try std.testing.expectEqual(@as(usize, 2), all.len);
    try std.testing.expect(all[0].id < all[1].id);
}

test "list filters by status" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p1 = try create(&d, a, .{ .title = "draft plan" });
    defer deinit(p1, a);
    const p2 = try create(&d, a, .{ .title = "active plan", .status = .active });
    defer deinit(p2, a);

    const drafts = try list(&d, a, .{ .statuses = &.{.draft} });
    defer deinitMany(drafts, a);
    try std.testing.expectEqual(@as(usize, 1), drafts.len);
    try std.testing.expectEqualStrings("draft plan", drafts[0].title);
}

test "list with scope filter returns only matching rows" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const p_assoc = try create(&d, a, .{ .title = "assoc plan", .scope = "acme" });
    defer deinit(p_assoc, a);
    const p_global = try create(&d, a, .{ .title = "global plan" });
    defer deinit(p_global, a);

    const filtered = try list(&d, a, .{ .scope = "acme" });
    defer deinitMany(filtered, a);
    try std.testing.expectEqual(@as(usize, 1), filtered.len);
    try std.testing.expectEqualStrings("assoc plan", filtered[0].title);
    try std.testing.expectEqual(ScopeKind.association, filtered[0].scope_kind);
}

test "listTouching suppresses direct-repo branch when scope excludes repo" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into projects (slug, name, root_path) values ('r1', 'R1', '/r1')", &.{});
    const repo_id = try d.intQuery("select id from projects where slug='r1'");
    _ = try d.execParams(
        \\insert into plans (scope_kind, scope_id, title, slug, status)
        \\values ('repo', ?, 'repo-scoped-plan', 'repo-scoped-plan', 'active')
    , &.{.{ .int = repo_id }});

    const all = try listTouching(&d, a, repo_id, .{});
    defer deinitMany(all, a);
    try std.testing.expectEqual(@as(usize, 1), all.len);

    const global_only = try listTouching(&d, a, repo_id, .{ .scope = "global" });
    defer deinitMany(global_only, a);
    try std.testing.expectEqual(@as(usize, 0), global_only.len);
}

test "update changes title and refreshes updated_at" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try create(&d, a, .{ .title = "Old title" });
    defer deinit(p, a);
    const updated = try update(&d, a, p.id, .{ .title = "New title" });
    defer deinit(updated, a);

    try std.testing.expectEqualStrings("New title", updated.title);
    try std.testing.expectEqual(p.id, updated.id);
}

test "update with scope moves plan to association scope" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'acme'");
    const p = try create(&d, a, .{ .title = "initially global" });
    defer deinit(p, a);
    try std.testing.expectEqual(ScopeKind.global, p.scope_kind);

    const updated = try update(&d, a, p.id, .{ .scope = "acme" });
    defer deinit(updated, a);
    try std.testing.expectEqual(ScopeKind.association, updated.scope_kind);
    try std.testing.expectEqual(assoc_id, updated.scope_id.?);
}

test "update with status change records a status_change audit row" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try create(&d, a, .{ .title = "Workflow plan" });
    defer deinit(p, a);
    const updated = try update(&d, a, p.id, .{ .status = .active });
    defer deinit(updated, a);

    try std.testing.expectEqual(Status.active, updated.status);
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb = 'status_change' and entity_kind = 'plan'"),
    );
}

test "show returns NotFound for missing id" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
}

// =========================================================================
// recomputeStatus tests
// =========================================================================

fn insertTask(d: *db.sqlite.Db, plan_id: i64, status: []const u8) !i64 {
    return try d.execParams(
        "insert into tasks (scope_kind, title, status, priority, plan_id) values ('global', 'T', ?, 100, ?)",
        &.{ .{ .text = status }, .{ .int = plan_id } },
    );
}

test "recomputeStatus: all child tasks done → flips plan to done; flipped=true; audit row" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try create(&d, a, .{ .title = "Recompute test", .parent_plan_id = null, .status = .active });
    defer deinit(p, a);

    // Create a parent so this is not an anchor (anchors don't auto-promote to done).
    // Actually: we need this plan to have a parent so it can auto-promote to done.
    // Insert a parent manually.
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Parent', 'parent', 'active')",
        &.{},
    );
    const parent_id = try d.intQuery("select max(id) from plans where slug = 'parent'");

    // Create child plan with parent.
    const child = try create(&d, a, .{ .title = "Child plan", .status = .active, .parent_plan_id = parent_id });
    defer deinit(child, a);

    // Add tasks to child plan and mark them all done.
    _ = try insertTask(&d, child.id, "done");
    _ = try insertTask(&d, child.id, "done");

    // Before recompute: child is 'active'.
    const before = try show(&d, a, child.id);
    defer deinit(before, a);
    try std.testing.expectEqual(Status.active, before.status);

    const result = try recomputeStatus(&d, a, child.id);
    defer result.deinit(a);

    try std.testing.expectEqual(child.id, result.plan_id);
    try std.testing.expectEqual(Status.active, result.status_before);
    try std.testing.expectEqual(Status.done, result.status_after);
    try std.testing.expect(result.flipped);

    // Audit row for the status_change (fresh DB — only this plan's row).
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='status_change' and entity_kind='plan'"),
    );
}

test "recomputeStatus: some tasks still open → does NOT flip; flipped=false; no extra audit" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try create(&d, a, .{ .title = "Open tasks plan", .status = .active });
    defer deinit(p, a);

    _ = try insertTask(&d, p.id, "done");
    _ = try insertTask(&d, p.id, "todo"); // still open

    const result = try recomputeStatus(&d, a, p.id);
    defer result.deinit(a);

    try std.testing.expect(!result.flipped);
    try std.testing.expectEqual(Status.active, result.status_before);
    try std.testing.expectEqual(Status.active, result.status_after);

    // No status_change audit row.
    try std.testing.expectEqual(
        @as(i64, 0),
        try d.intQuery("select count(*) from audit_log where verb='status_change' and entity_kind='plan'"),
    );
}

test "recomputeStatus: idempotent — second call on already-done plan is no-op" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Create parent so child is not anchor.
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Par', 'par', 'active')",
        &.{},
    );
    const parent_id = try d.intQuery("select max(id) from plans");

    const child = try create(&d, a, .{ .title = "Idempotent plan", .status = .active, .parent_plan_id = parent_id });
    defer deinit(child, a);
    _ = try insertTask(&d, child.id, "done");

    const r1 = try recomputeStatus(&d, a, child.id);
    defer r1.deinit(a);
    try std.testing.expect(r1.flipped);

    // Second call: already done — should be a no-op.
    const r2 = try recomputeStatus(&d, a, child.id);
    defer r2.deinit(a);
    try std.testing.expect(!r2.flipped);
    try std.testing.expectEqual(Status.done, r2.status_before);
    try std.testing.expectEqual(Status.done, r2.status_after);

    // Only one status_change audit row for plans (from the first call only).
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='status_change' and entity_kind='plan'"),
    );
}

test "recomputeStatus: anchor plan does not auto-promote to done; flips to active at most" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Anchor = no parent_plan_id. Status is draft.
    const anchor = try create(&d, a, .{ .title = "Anchor plan", .status = .draft });
    defer deinit(anchor, a);
    try std.testing.expect(anchor.parent_plan_id == null);

    // Add two done tasks — all terminal.
    _ = try insertTask(&d, anchor.id, "done");
    _ = try insertTask(&d, anchor.id, "done");

    const result = try recomputeStatus(&d, a, anchor.id);
    defer result.deinit(a);

    // Anchor: draft + all-terminal → active (not done).
    try std.testing.expect(result.flipped);
    try std.testing.expectEqual(Status.draft, result.status_before);
    try std.testing.expectEqual(Status.active, result.status_after);
}

test "recomputeStatus: empty plan (no tasks) → no flip regardless of status" {
    // Mirrors Go's agg.Total() == 0 short-circuit: empty plans never transition.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const p = try create(&d, a, .{ .title = "Empty plan", .status = .active });
    defer deinit(p, a);

    // No tasks inserted.
    const result = try recomputeStatus(&d, a, p.id);
    defer result.deinit(a);

    try std.testing.expect(!result.flipped);
    try std.testing.expectEqual(Status.active, result.status_before);
    try std.testing.expectEqual(Status.active, result.status_after);

    // No status_change audit rows.
    try std.testing.expectEqual(
        @as(i64, 0),
        try d.intQuery("select count(*) from audit_log where verb='status_change' and entity_kind='plan'"),
    );
}

test "recomputeStatus: single-plan only — parent plan is NOT touched by child recompute" {
    // D-recompute-single-plan: recomputeStatus operates on THIS plan only.
    // The parent plan's status must remain unchanged even if child flipped.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // parent (anchor, draft, with a done task of its own).
    const parent = try create(&d, a, .{ .title = "Parent plan", .status = .draft });
    defer deinit(parent, a);
    _ = try insertTask(&d, parent.id, "done");

    // child (non-anchor, active, with done tasks).
    const child = try create(&d, a, .{ .title = "Child plan", .status = .active, .parent_plan_id = parent.id });
    defer deinit(child, a);
    _ = try insertTask(&d, child.id, "done");

    // Recompute child only.
    const result = try recomputeStatus(&d, a, child.id);
    defer result.deinit(a);

    try std.testing.expect(result.flipped);
    try std.testing.expectEqual(Status.done, result.status_after);

    // Parent status must be unchanged (still draft): recomputeStatus does
    // not walk parent_plan_id. Only ONE status_change audit row total.
    const parent_after = try show(&d, a, parent.id);
    defer deinit(parent_after, a);
    try std.testing.expectEqual(Status.draft, parent_after.status);

    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where verb='status_change' and entity_kind='plan'"),
    );
}

test "recomputeStatus: audit row text records the aggregate reason" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Par2', 'par2', 'active')",
        &.{},
    );
    const parent_id = try d.intQuery("select max(id) from plans");

    const child = try create(&d, a, .{ .title = "Audit check", .status = .active, .parent_plan_id = parent_id });
    defer deinit(child, a);
    _ = try insertTask(&d, child.id, "done");
    _ = try insertTask(&d, child.id, "cancelled");

    const result = try recomputeStatus(&d, a, child.id);
    defer result.deinit(a);
    try std.testing.expect(result.flipped);

    // The audit summary should contain task counts.
    var stmt = d.prepare(
        "select summary from audit_log where verb='status_change' and entity_kind='plan' and entity_id = ?",
    ) catch return;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = child.id }}) catch return;
    switch (stmt.step() catch return) {
        .done => try std.testing.expect(false),
        .row => {
            const summary = try stmt.columnTextAlloc(0, a);
            defer a.free(summary);
            try std.testing.expect(std.mem.indexOf(u8, summary, "done=") != null);
            try std.testing.expect(std.mem.indexOf(u8, summary, "cancelled=") != null);
        },
    }
}

test "update rejects a parent cycle without mutating the plan" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const root = try create(&d, a, .{ .title = "Cycle root" });
    defer deinit(root, a);
    const child = try create(&d, a, .{ .title = "Cycle child", .parent_plan_id = root.id });
    defer deinit(child, a);

    try std.testing.expectError(
        Error.InvalidParentCycle,
        update(&d, a, root.id, .{ .parent_plan_id = child.id }),
    );
    const unchanged = try show(&d, a, root.id);
    defer deinit(unchanged, a);
    try std.testing.expectEqual(@as(?i64, null), unchanged.parent_plan_id);
}
