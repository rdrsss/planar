//! engine/tree — hierarchical plan → task → linked-entity traversal.
//!
//! Builds an in-memory tree of Planar entities rooted at one or more scopes
//! and returns it as a `[]Node` slice. The walker is read-only: it issues SQL
//! queries against the live DB and returns an owned Node graph that the caller
//! renders as text (renderText) or serialises as JSON via std.json.Stringify.
//!
//! The walk follows three relationship channels (mirrors Go's src/internal/tree):
//!   - plans.parent_plan_id            → plan → plan
//!   - tasks.plan_id / parent_task_id  → plan → task → subtask
//!   - entity_links(derives-from)      → plan ↔ artifact/decision/scenario/question
//!
//! Filters (depth, kind, status) apply during the walk so excluded subtrees
//! never enter the result graph.
//!
//! D-engine-pattern: pure functions over *db.sqlite.Db, no goroutines, no
//! interfaces, no audit calls, no writes.

const std = @import("std");
const db = @import("db");
const identity = @import("identity.zig");

// =========================================================================
// Public constants
// =========================================================================

pub const valid_kinds = [_][]const u8{
    "plan", "task", "question", "scenario", "decision", "artifact",
};

// =========================================================================
// Public types
// =========================================================================

/// Node is a single entity in the tree. All slice fields are owned by the
/// allocator passed to `build`; release the entire tree via `deinitNodes`.
///
/// Field names mirror Go's render_json.go `jsonNode` wire shape for JSON
/// parity. `children` always points to a valid (possibly empty) slice.
pub const Node = struct {
    kind: []const u8,
    id: i64,
    title: []const u8,
    /// Set for plans; empty string for other kinds.
    slug: []const u8,
    status: []const u8,
    /// Set for tasks; zero for other kinds.
    priority: i64,
    /// Set for artifacts; empty string for other kinds.
    artifact_kind: []const u8,
    /// Set for scope roots (kind = "scope"); empty for other nodes.
    scope_kind: []const u8,
    scope_id: ?i64,
    scope_label: []const u8,
    created_at: []const u8,
    updated_at: []const u8,
    /// Ordered child list. Walker preserves DB insertion order (id ASC).
    children: []Node,

    /// Custom JSON serializer (task 2377): scope nodes emit only the
    /// scope-relevant fields; entity nodes emit the full shape. Without
    /// this branch the default struct serializer leaked zero-valued
    /// entity fields (id=0, slug="", status="", ...) onto scope roots,
    /// polluting `tree --json` output. See parity-triage.md
    /// §E-tree-cwd-derive (the JSON-pollution sub-fix) and task 2377.
    pub fn jsonStringify(self: Node, jws: anytype) !void {
        try jws.beginObject();
        try jws.objectField("kind");
        try jws.write(self.kind);
        if (std.mem.eql(u8, self.kind, "scope")) {
            try jws.objectField("title");
            try jws.write(self.title);
            try jws.objectField("scope_kind");
            try jws.write(self.scope_kind);
            try jws.objectField("scope_id");
            try jws.write(self.scope_id);
            try jws.objectField("scope_label");
            try jws.write(self.scope_label);
            try jws.objectField("children");
            try jws.write(self.children);
        } else {
            try jws.objectField("id");
            try jws.write(self.id);
            try jws.objectField("title");
            try jws.write(self.title);
            try jws.objectField("slug");
            try jws.write(self.slug);
            try jws.objectField("status");
            try jws.write(self.status);
            try jws.objectField("priority");
            try jws.write(self.priority);
            try jws.objectField("artifact_kind");
            try jws.write(self.artifact_kind);
            try jws.objectField("created_at");
            try jws.write(self.created_at);
            try jws.objectField("updated_at");
            try jws.write(self.updated_at);
            try jws.objectField("children");
            try jws.write(self.children);
        }
        try jws.endObject();
    }
};

/// Filter controls which entities are traversed. Zero value → global scope,
/// no filters, unbounded depth.
pub const Filter = struct {
    /// Scope slug. Null → global. Ignored when all_scopes is true.
    scope: ?[]const u8 = null,
    /// Walk every scope (global + every association + every project).
    all_scopes: bool = false,
    /// Max depth. -1 (or any ≤ 0) means unbounded. Top-level plan = depth 0;
    /// its child plan or task = depth 1; etc.
    max_depth: i64 = -1,
    /// Restrict to these entity kinds. Empty → all kinds.
    kinds: []const []const u8 = &.{},
    /// Restrict by status across all kinds. Empty → any status.
    statuses: []const []const u8 = &.{},
    /// Sort key: null / "id" (default asc-by-id), "updated", "created".
    sort: ?[]const u8 = null,
    /// When non-null, start from this plan id only, skip other top-level plans.
    root_plan_id: ?i64 = null,
};

pub const Error = error{
    QueryFailed,
    UnsupportedScope,
    SlugNotFound,
    UnknownKind,
} || std.mem.Allocator.Error;

// =========================================================================
// Deinit helpers
// =========================================================================

/// Release all memory owned by a Node and its subtree.
pub fn deinitNode(n: Node, allocator: std.mem.Allocator) void {
    allocator.free(n.kind);
    allocator.free(n.title);
    allocator.free(n.slug);
    allocator.free(n.status);
    allocator.free(n.artifact_kind);
    allocator.free(n.scope_kind);
    allocator.free(n.scope_label);
    allocator.free(n.created_at);
    allocator.free(n.updated_at);
    for (n.children) |child| deinitNode(child, allocator);
    allocator.free(n.children);
}

/// Release all memory owned by a slice of root nodes.
pub fn deinitNodes(nodes: []const Node, allocator: std.mem.Allocator) void {
    for (nodes) |n| deinitNode(n, allocator);
    allocator.free(nodes);
}

// =========================================================================
// Public entry point
// =========================================================================

/// Build the entity tree according to `filter` and return a slice of scope
/// roots. Each root has kind = "scope" with entity descendants as children.
/// Returns an empty-children scope node (not null) when nothing matches.
/// Caller owns the returned slice; use deinitNodes to release.
pub fn build(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: Filter,
) Error![]Node {
    // Validate kind filter upfront.
    for (filter.kinds) |k| {
        var found = false;
        for (valid_kinds) |vk| {
            if (std.mem.eql(u8, k, vk)) {
                found = true;
                break;
            }
        }
        if (!found) return Error.UnknownKind;
    }

    if (filter.all_scopes) {
        return buildAllScopes(d, allocator, filter);
    }

    // Resolve scope.
    var scope_kind: []const u8 = "global";
    var scope_id: ?i64 = null;

    if (filter.scope) |slug| {
        const ref = identity.scope.resolveSlug(d, allocator, slug) catch |e| switch (e) {
            error.UnsupportedScope => return Error.UnsupportedScope,
            error.SlugNotFound => return Error.SlugNotFound,
            else => return Error.QueryFailed,
        };
        switch (ref.kind) {
            .global => scope_kind = "global",
            .association => {
                scope_kind = "association";
                scope_id = ref.id;
            },
            .repo => return Error.UnsupportedScope,
        }
    }

    const scope_label = try buildScopeLabel(allocator, scope_kind, scope_id, d);
    errdefer allocator.free(scope_label);

    const children = try walkScope(d, allocator, scope_kind, scope_id, filter, 0);
    errdefer {
        for (children) |c| deinitNode(c, allocator);
        allocator.free(children);
    }

    const scope_node = try makeScopeNode(allocator, scope_kind, scope_id, scope_label, children);
    const roots = try allocator.alloc(Node, 1);
    roots[0] = scope_node;
    return roots;
}

// =========================================================================
// Internal: all-scopes
// =========================================================================

fn buildAllScopes(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: Filter,
) Error![]Node {
    var roots: std.ArrayList(Node) = .empty;
    errdefer {
        for (roots.items) |n| deinitNode(n, allocator);
        roots.deinit(allocator);
    }

    // Global scope.
    {
        const label = try buildScopeLabel(allocator, "global", null, d);
        errdefer allocator.free(label);
        const ch = try walkScope(d, allocator, "global", null, filter, 0);
        errdefer {
            for (ch) |c| deinitNode(c, allocator);
            allocator.free(ch);
        }
        try roots.append(allocator, try makeScopeNode(allocator, "global", null, label, ch));
    }

    // Associations, slug-ordered.
    {
        var stmt = d.prepare("select id from associations order by slug") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{}) catch return Error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => break,
                .row => {
                    const aid = stmt.columnInt(0);
                    const label = try buildScopeLabel(allocator, "association", aid, d);
                    errdefer allocator.free(label);
                    const ch = try walkScope(d, allocator, "association", aid, filter, 0);
                    errdefer {
                        for (ch) |c| deinitNode(c, allocator);
                        allocator.free(ch);
                    }
                    try roots.append(allocator, try makeScopeNode(allocator, "association", aid, label, ch));
                },
            }
        }
    }

    // Projects, slug-ordered.
    {
        var stmt = d.prepare("select id from projects order by slug") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{}) catch return Error.QueryFailed;
        while (true) {
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => break,
                .row => {
                    const pid = stmt.columnInt(0);
                    const label = try buildScopeLabel(allocator, "repo", pid, d);
                    errdefer allocator.free(label);
                    const ch = try walkScope(d, allocator, "repo", pid, filter, 0);
                    errdefer {
                        for (ch) |c| deinitNode(c, allocator);
                        allocator.free(ch);
                    }
                    try roots.append(allocator, try makeScopeNode(allocator, "repo", pid, label, ch));
                },
            }
        }
    }

    return try roots.toOwnedSlice(allocator);
}

fn makeScopeNode(
    allocator: std.mem.Allocator,
    scope_kind: []const u8,
    scope_id: ?i64,
    scope_label: []const u8,
    children: []Node,
) std.mem.Allocator.Error!Node {
    return Node{
        .kind = try allocator.dupe(u8, "scope"),
        .id = 0,
        .title = try allocator.dupe(u8, scope_label),
        .slug = try allocator.dupe(u8, ""),
        .status = try allocator.dupe(u8, ""),
        .priority = 0,
        .artifact_kind = try allocator.dupe(u8, ""),
        .scope_kind = try allocator.dupe(u8, scope_kind),
        .scope_id = scope_id,
        .scope_label = scope_label,
        .created_at = try allocator.dupe(u8, ""),
        .updated_at = try allocator.dupe(u8, ""),
        .children = children,
    };
}

// =========================================================================
// Internal: scope label
// =========================================================================

fn buildScopeLabel(
    allocator: std.mem.Allocator,
    scope_kind: []const u8,
    scope_id: ?i64,
    d: *db.sqlite.Db,
) Error![]u8 {
    if (std.mem.eql(u8, scope_kind, "global")) {
        return allocator.dupe(u8, "global");
    }
    const table: [:0]const u8 = if (std.mem.eql(u8, scope_kind, "association"))
        "associations"
    else
        "projects";
    const prefix: []const u8 = if (std.mem.eql(u8, scope_kind, "association"))
        "assoc:"
    else
        "repo:";

    if (scope_id) |sid| {
        // Build sentinel-terminated SQL via a fixed buffer.
        var sql_buf: [128]u8 = undefined;
        const sql_slice = std.fmt.bufPrint(&sql_buf, "select slug from {s} where id = ?", .{table}) catch return Error.QueryFailed;
        // Null-terminate manually.
        if (sql_slice.len >= sql_buf.len) return Error.QueryFailed;
        sql_buf[sql_slice.len] = 0;
        const sql_z: [:0]const u8 = sql_buf[0..sql_slice.len :0];
        var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = sid }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => {},
            .row => {
                const slug = try stmt.columnTextAlloc(0, allocator);
                defer allocator.free(slug);
                return std.fmt.allocPrint(allocator, "{s}{s}", .{ prefix, slug });
            },
        }
    }
    return std.fmt.allocPrint(allocator, "{s}unknown", .{prefix});
}

// =========================================================================
// Internal: flat plan row
// =========================================================================

const PlanRow = struct {
    id: i64,
    title: []const u8,
    slug: []const u8,
    status: []const u8,
    created_at: []const u8,
    updated_at: []const u8,
};

fn freePlanRow(r: PlanRow, allocator: std.mem.Allocator) void {
    allocator.free(r.title);
    allocator.free(r.slug);
    allocator.free(r.status);
    allocator.free(r.created_at);
    allocator.free(r.updated_at);
}

// =========================================================================
// Internal: scope walk
// =========================================================================

/// Walk top-level plans (parent_plan_id IS NULL) for the given scope.
fn walkScope(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    scope_kind: []const u8,
    scope_id: ?i64,
    filter: Filter,
    depth: i64,
) Error![]Node {
    var sql_buf: std.ArrayList(u8) = .empty;
    defer sql_buf.deinit(allocator);

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);

    try sql_buf.appendSlice(allocator, "select id, title, slug, status," ++
        " coalesce(created_at,''), coalesce(updated_at,'')" ++
        " from plans where parent_plan_id is null and scope_kind = ?");
    try params.append(allocator, .{ .text = scope_kind });

    if (scope_id) |sid| {
        try sql_buf.appendSlice(allocator, " and scope_id = ?");
        try params.append(allocator, .{ .int = sid });
    } else {
        try sql_buf.appendSlice(allocator, " and scope_id is null");
    }
    try sql_buf.appendSlice(allocator, " order by id");

    const sql_z = try allocator.dupeZ(u8, sql_buf.items);
    defer allocator.free(sql_z);

    var stmt = d.prepare(sql_z) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(params.items) catch return Error.QueryFailed;

    // Collect all plan rows first (before calling buildPlanNode which needs the DB).
    var rows: std.ArrayList(PlanRow) = .empty;
    defer {
        for (rows.items) |r| freePlanRow(r, allocator);
        rows.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const row = PlanRow{
                    .id = stmt.columnInt(0),
                    .title = try stmt.columnTextAlloc(1, allocator),
                    .slug = try stmt.columnTextAlloc(2, allocator),
                    .status = try stmt.columnTextAlloc(3, allocator),
                    .created_at = try stmt.columnTextAlloc(4, allocator),
                    .updated_at = try stmt.columnTextAlloc(5, allocator),
                };
                try rows.append(allocator, row);
            },
        }
    }

    var out: std.ArrayList(Node) = .empty;
    errdefer {
        for (out.items) |n| deinitNode(n, allocator);
        out.deinit(allocator);
    }

    for (rows.items) |row| {
        // If root_plan_id set, skip other plans.
        if (filter.root_plan_id) |root_id| {
            if (row.id != root_id) continue;
        }
        const node_opt = try buildPlanNode(d, allocator, row, filter, depth);
        if (node_opt) |node| try out.append(allocator, node);
    }

    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Internal: plan node builder
// =========================================================================

fn buildPlanNode(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    row: PlanRow,
    filter: Filter,
    depth: i64,
) Error!?Node {
    const keep_self = kindAllowed("plan", filter.kinds) and
        statusAllowed(row.status, filter.statuses);

    var children: std.ArrayList(Node) = .empty;
    errdefer {
        for (children.items) |c| deinitNode(c, allocator);
        children.deinit(allocator);
    }

    if (!depthAtMax(depth, filter.max_depth)) {
        // Child plans.
        const cps = try queryChildPlans(d, allocator, row.id, filter, depth + 1);
        defer allocator.free(cps);
        try children.appendSlice(allocator, cps);

        // Top-level tasks.
        const tasks = try queryTopTasks(d, allocator, row.id, filter, depth + 1);
        defer allocator.free(tasks);
        try children.appendSlice(allocator, tasks);

        // Derived entities.
        const derived = try queryDerived(d, allocator, row.id, filter);
        defer allocator.free(derived);
        try children.appendSlice(allocator, derived);
    }

    // Scaffold logic (mirrors Go's C-11 fix):
    //   - If keep_self: always keep.
    //   - If NOT keep_self and no children: drop.
    //   - If NOT keep_self and has children: keep only if at least one child
    //     is a plan or task (not just derived entities).
    if (!keep_self) {
        if (children.items.len == 0) return null;
        var has_plan_or_task = false;
        for (children.items) |c| {
            if (std.mem.eql(u8, c.kind, "plan") or std.mem.eql(u8, c.kind, "task")) {
                has_plan_or_task = true;
                break;
            }
        }
        if (!has_plan_or_task) return null;
    }

    return Node{
        .kind = try allocator.dupe(u8, "plan"),
        .id = row.id,
        .title = try allocator.dupe(u8, row.title),
        .slug = try allocator.dupe(u8, row.slug),
        .status = try allocator.dupe(u8, row.status),
        .priority = 0,
        .artifact_kind = try allocator.dupe(u8, ""),
        .scope_kind = try allocator.dupe(u8, ""),
        .scope_id = null,
        .scope_label = try allocator.dupe(u8, ""),
        .created_at = try allocator.dupe(u8, row.created_at),
        .updated_at = try allocator.dupe(u8, row.updated_at),
        .children = try children.toOwnedSlice(allocator),
    };
}

// =========================================================================
// Internal: child plan query
// =========================================================================

fn queryChildPlans(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    parent_id: i64,
    filter: Filter,
    depth: i64,
) Error![]Node {
    var stmt = d.prepare(
        "select id, title, slug, status," ++
            " coalesce(created_at,''), coalesce(updated_at,'')" ++
            " from plans where parent_plan_id = ? order by id",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = parent_id }}) catch return Error.QueryFailed;

    var rows: std.ArrayList(PlanRow) = .empty;
    defer {
        for (rows.items) |r| freePlanRow(r, allocator);
        rows.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                try rows.append(allocator, .{
                    .id = stmt.columnInt(0),
                    .title = try stmt.columnTextAlloc(1, allocator),
                    .slug = try stmt.columnTextAlloc(2, allocator),
                    .status = try stmt.columnTextAlloc(3, allocator),
                    .created_at = try stmt.columnTextAlloc(4, allocator),
                    .updated_at = try stmt.columnTextAlloc(5, allocator),
                });
            },
        }
    }

    var out: std.ArrayList(Node) = .empty;
    errdefer {
        for (out.items) |n| deinitNode(n, allocator);
        out.deinit(allocator);
    }

    for (rows.items) |row| {
        const node_opt = try buildPlanNode(d, allocator, row, filter, depth);
        if (node_opt) |node| try out.append(allocator, node);
    }

    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Internal: task rows
// =========================================================================

const TaskRow = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
    priority: i64,
    created_at: []const u8,
    updated_at: []const u8,
};

fn freeTaskRow(r: TaskRow, allocator: std.mem.Allocator) void {
    allocator.free(r.title);
    allocator.free(r.status);
    allocator.free(r.created_at);
    allocator.free(r.updated_at);
}

fn queryTopTasks(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
    filter: Filter,
    depth: i64,
) Error![]Node {
    // Mirrors Go's queryTopTasksForPlan: DISTINCT + LEFT JOIN for derives-from.
    var stmt = d.prepare(
        "select distinct t.id, t.title, coalesce(t.status,''), t.priority," ++
            " coalesce(t.created_at,''), coalesce(t.updated_at,'')" ++
            " from tasks t" ++
            " left join entity_links el" ++
            "   on el.from_kind = 'task' and el.from_id = t.id" ++
            "  and el.to_kind = 'plan' and el.to_id = ?" ++
            "  and el.relationship = 'derives-from'" ++
            " where t.parent_task_id is null" ++
            "   and (t.plan_id = ? or el.id is not null)" ++
            " order by t.id",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return Error.QueryFailed;

    var rows: std.ArrayList(TaskRow) = .empty;
    defer {
        for (rows.items) |r| freeTaskRow(r, allocator);
        rows.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                try rows.append(allocator, .{
                    .id = stmt.columnInt(0),
                    .title = try stmt.columnTextAlloc(1, allocator),
                    .status = try stmt.columnTextAlloc(2, allocator),
                    .priority = stmt.columnInt(3),
                    .created_at = try stmt.columnTextAlloc(4, allocator),
                    .updated_at = try stmt.columnTextAlloc(5, allocator),
                });
            },
        }
    }

    var out: std.ArrayList(Node) = .empty;
    errdefer {
        for (out.items) |n| deinitNode(n, allocator);
        out.deinit(allocator);
    }

    for (rows.items) |row| {
        const node_opt = try buildTaskNode(d, allocator, row, filter, depth);
        if (node_opt) |node| try out.append(allocator, node);
    }

    return try out.toOwnedSlice(allocator);
}

fn buildTaskNode(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    row: TaskRow,
    filter: Filter,
    depth: i64,
) Error!?Node {
    const keep_self = kindAllowed("task", filter.kinds) and
        statusAllowed(row.status, filter.statuses);

    var children: std.ArrayList(Node) = .empty;
    errdefer {
        for (children.items) |c| deinitNode(c, allocator);
        children.deinit(allocator);
    }

    if (!depthAtMax(depth, filter.max_depth)) {
        const subs = try querySubtasks(d, allocator, row.id, filter, depth + 1);
        defer allocator.free(subs);
        try children.appendSlice(allocator, subs);
    }

    if (!keep_self and children.items.len == 0) return null;

    return Node{
        .kind = try allocator.dupe(u8, "task"),
        .id = row.id,
        .title = try allocator.dupe(u8, row.title),
        .slug = try allocator.dupe(u8, ""),
        .status = try allocator.dupe(u8, row.status),
        .priority = row.priority,
        .artifact_kind = try allocator.dupe(u8, ""),
        .scope_kind = try allocator.dupe(u8, ""),
        .scope_id = null,
        .scope_label = try allocator.dupe(u8, ""),
        .created_at = try allocator.dupe(u8, row.created_at),
        .updated_at = try allocator.dupe(u8, row.updated_at),
        .children = try children.toOwnedSlice(allocator),
    };
}

fn querySubtasks(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    parent_task_id: i64,
    filter: Filter,
    depth: i64,
) Error![]Node {
    var stmt = d.prepare(
        "select id, title, coalesce(status,''), priority," ++
            " coalesce(created_at,''), coalesce(updated_at,'')" ++
            " from tasks where parent_task_id = ? order by id",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = parent_task_id }}) catch return Error.QueryFailed;

    var rows: std.ArrayList(TaskRow) = .empty;
    defer {
        for (rows.items) |r| freeTaskRow(r, allocator);
        rows.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                try rows.append(allocator, .{
                    .id = stmt.columnInt(0),
                    .title = try stmt.columnTextAlloc(1, allocator),
                    .status = try stmt.columnTextAlloc(2, allocator),
                    .priority = stmt.columnInt(3),
                    .created_at = try stmt.columnTextAlloc(4, allocator),
                    .updated_at = try stmt.columnTextAlloc(5, allocator),
                });
            },
        }
    }

    var out: std.ArrayList(Node) = .empty;
    errdefer {
        for (out.items) |n| deinitNode(n, allocator);
        out.deinit(allocator);
    }

    for (rows.items) |row| {
        const node_opt = try buildTaskNode(d, allocator, row, filter, depth);
        if (node_opt) |node| try out.append(allocator, node);
    }

    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Internal: derived entities (artifact / decision / scenario / question)
// =========================================================================

fn queryDerived(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
    filter: Filter,
) Error![]Node {
    var stmt = d.prepare(
        "select el.from_kind, el.from_id" ++
            " from entity_links el" ++
            " where el.relationship = 'derives-from'" ++
            "   and el.to_kind = 'plan' and el.to_id = ?" ++
            "   and el.from_kind in ('artifact','decision','test_scenario','question')" ++
            " order by el.from_kind, el.from_id",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;

    const LinkRow = struct { db_kind: []const u8, id: i64 };
    var links: std.ArrayList(LinkRow) = .empty;
    defer {
        for (links.items) |l| allocator.free(l.db_kind);
        links.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const kind = try stmt.columnTextAlloc(0, allocator);
                try links.append(allocator, .{ .db_kind = kind, .id = stmt.columnInt(1) });
            },
        }
    }

    var out: std.ArrayList(Node) = .empty;
    errdefer {
        for (out.items) |n| deinitNode(n, allocator);
        out.deinit(allocator);
    }

    for (links.items) |link| {
        // test_scenario → "scenario" for display + filter lookup.
        const display_kind: []const u8 = if (std.mem.eql(u8, link.db_kind, "test_scenario"))
            "scenario"
        else
            link.db_kind;

        if (!kindAllowed(display_kind, filter.kinds)) continue;

        const node_opt = try hydrateDerived(d, allocator, link.db_kind, display_kind, link.id, filter);
        if (node_opt) |node| try out.append(allocator, node);
    }

    return try out.toOwnedSlice(allocator);
}

fn hydrateDerived(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    db_kind: []const u8,
    display_kind: []const u8,
    id: i64,
    filter: Filter,
) Error!?Node {
    if (std.mem.eql(u8, db_kind, "artifact")) {
        var stmt = d.prepare(
            "select title, status, kind, coalesce(created_at,''), coalesce(updated_at,'')" ++
                " from artifacts where id = ?",
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return null,
            .row => {
                const title = try stmt.columnTextAlloc(0, allocator);
                const status = try stmt.columnTextAlloc(1, allocator);
                const akind = try stmt.columnTextAlloc(2, allocator);
                const created_at = try stmt.columnTextAlloc(3, allocator);
                const updated_at = try stmt.columnTextAlloc(4, allocator);
                if (!statusAllowed(status, filter.statuses)) {
                    allocator.free(title);
                    allocator.free(status);
                    allocator.free(akind);
                    allocator.free(created_at);
                    allocator.free(updated_at);
                    return null;
                }
                return makeLeafNode(allocator, display_kind, id, title, status, akind, created_at, updated_at);
            },
        }
    }

    const table: [:0]const u8 = if (std.mem.eql(u8, db_kind, "decision"))
        "decisions"
    else if (std.mem.eql(u8, db_kind, "test_scenario"))
        "test_scenarios"
    else if (std.mem.eql(u8, db_kind, "question"))
        "questions"
    else
        return null;

    var sql_buf2: [256]u8 = undefined;
    const sql_slice2 = std.fmt.bufPrint(&sql_buf2, "select title, status, coalesce(created_at,''), coalesce(updated_at,'')" ++
        " from {s} where id = ?", .{table}) catch return Error.QueryFailed;
    if (sql_slice2.len >= sql_buf2.len) return Error.QueryFailed;
    sql_buf2[sql_slice2.len] = 0;
    const sql2: [:0]const u8 = sql_buf2[0..sql_slice2.len :0];

    var stmt = d.prepare(sql2) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => return null,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            const status = try stmt.columnTextAlloc(1, allocator);
            const created_at = try stmt.columnTextAlloc(2, allocator);
            const updated_at = try stmt.columnTextAlloc(3, allocator);
            if (!statusAllowed(status, filter.statuses)) {
                allocator.free(title);
                allocator.free(status);
                allocator.free(created_at);
                allocator.free(updated_at);
                return null;
            }
            const empty_akind = try allocator.dupe(u8, "");
            return makeLeafNode(allocator, display_kind, id, title, status, empty_akind, created_at, updated_at);
        },
    }
}

fn makeLeafNode(
    allocator: std.mem.Allocator,
    kind: []const u8,
    id: i64,
    title: []const u8,
    status: []const u8,
    artifact_kind: []const u8,
    created_at: []const u8,
    updated_at: []const u8,
) Error!?Node {
    const empty_children = try allocator.alloc(Node, 0);
    return Node{
        .kind = try allocator.dupe(u8, kind),
        .id = id,
        .title = title,
        .slug = try allocator.dupe(u8, ""),
        .status = status,
        .priority = 0,
        .artifact_kind = artifact_kind,
        .scope_kind = try allocator.dupe(u8, ""),
        .scope_id = null,
        .scope_label = try allocator.dupe(u8, ""),
        .created_at = created_at,
        .updated_at = updated_at,
        .children = empty_children,
    };
}

// =========================================================================
// Internal: filter helpers
// =========================================================================

/// depthAtMax returns true when depth ≥ max_depth. max_depth ≤ 0 → unbounded.
fn depthAtMax(depth: i64, max_depth: i64) bool {
    if (max_depth <= 0) return false;
    return depth >= max_depth;
}

fn kindAllowed(kind: []const u8, kinds: []const []const u8) bool {
    if (kinds.len == 0) return true;
    for (kinds) |k| {
        if (std.mem.eql(u8, k, kind)) return true;
    }
    return false;
}

fn statusAllowed(status: []const u8, statuses: []const []const u8) bool {
    if (statuses.len == 0) return true;
    for (statuses) |s| {
        if (std.mem.eql(u8, s, status)) return true;
    }
    return false;
}

// =========================================================================
// Text renderer
// =========================================================================

/// renderText writes an indented box-drawing tree (Unicode charset, dirs-first
/// grouping) to writer, mirroring Go's tree.Render.
///
/// Per-kind label format (matches Go's formatNodeLabel in render.go):
///   plan:<id> [<status>]  <title>
///   task:<id>  <title>  [<status>, pri:<priority>]
///   artifact:<id>  <title>  [<artifact_kind>, <status>]
///   decision:<id>  <title>  [<status>]
///   scenario:<id>  <title>  [<status>]
///   question:<id>  <title>  [<status>]
pub fn renderText(roots: []const Node, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    var counts = NodeCounts{};
    for (roots, 0..) |root, i| {
        if (i > 0) try writer.print("\n", .{});
        try renderRoot(root, writer, &counts);
    }
    // Summary footer (mirrors Go's nodeCounts.summary()).
    try writer.print("\n", .{});
    try writer.print(
        "{d} plan{s}, {d} task{s}, {d} artifact{s}, {d} decision{s}, {d} scenario{s}, {d} question{s}\n",
        .{
            counts.plans,     if (counts.plans == 1) "" else "s",
            counts.tasks,     if (counts.tasks == 1) "" else "s",
            counts.artifacts, if (counts.artifacts == 1) "" else "s",
            counts.decisions, if (counts.decisions == 1) "" else "s",
            counts.scenarios, if (counts.scenarios == 1) "" else "s",
            counts.questions, if (counts.questions == 1) "" else "s",
        },
    );
}

const NodeCounts = struct {
    plans: usize = 0,
    tasks: usize = 0,
    artifacts: usize = 0,
    decisions: usize = 0,
    scenarios: usize = 0,
    questions: usize = 0,

    fn bump(self: *NodeCounts, kind: []const u8) void {
        if (std.mem.eql(u8, kind, "plan")) self.plans += 1 else if (std.mem.eql(u8, kind, "task")) self.tasks += 1 else if (std.mem.eql(u8, kind, "artifact")) self.artifacts += 1 else if (std.mem.eql(u8, kind, "decision")) self.decisions += 1 else if (std.mem.eql(u8, kind, "scenario")) self.scenarios += 1 else if (std.mem.eql(u8, kind, "question")) self.questions += 1;
    }
};

fn renderRoot(root: Node, writer: *std.Io.Writer, counts: *NodeCounts) std.Io.Writer.Error!void {
    const label = if (root.scope_label.len > 0) root.scope_label else root.title;
    try writer.print("{s}\n", .{label});
    try renderChildList(root.children, "", writer, counts);
}

/// Render a list of children in dirs-first order (plans, tasks, rest),
/// preserving relative order within each group.
fn renderChildList(
    children: []const Node,
    prefix: []const u8,
    writer: *std.Io.Writer,
    counts: *NodeCounts,
) std.Io.Writer.Error!void {
    // Build an ordered index list: plans first, then tasks, then rest.
    // We do three passes to avoid allocating.
    const max_children = 4096;
    var order: [max_children]usize = undefined;
    var n: usize = 0;

    // Pass 1: plans.
    for (children, 0..) |c, i| {
        if (std.mem.eql(u8, c.kind, "plan")) {
            order[n] = i;
            n += 1;
        }
    }
    // Pass 2: tasks.
    for (children, 0..) |c, i| {
        if (std.mem.eql(u8, c.kind, "task")) {
            order[n] = i;
            n += 1;
        }
    }
    // Pass 3: rest.
    for (children, 0..) |c, i| {
        if (!std.mem.eql(u8, c.kind, "plan") and !std.mem.eql(u8, c.kind, "task")) {
            order[n] = i;
            n += 1;
        }
    }

    for (order[0..n], 0..) |child_idx, pos| {
        const child = children[child_idx];
        const is_last = pos == n - 1;
        try renderNode(child, prefix, is_last, writer, counts);
    }
}

fn renderNode(
    n: Node,
    prefix: []const u8,
    is_last: bool,
    writer: *std.Io.Writer,
    counts: *NodeCounts,
) std.Io.Writer.Error!void {
    counts.bump(n.kind);

    // Unicode box-drawing glyphs (mirrors Go's charsetFor(CharsetUnicode)).
    const connector: []const u8 = if (is_last) "└── " else "├── ";
    const label = formatNodeLabel(n);
    try writer.print("{s}{s}{s}\n", .{ prefix, connector, label });

    // Build child prefix.
    const ext: []const u8 = if (is_last) "    " else "│   ";
    var child_prefix_buf: [512]u8 = undefined;
    const pfx_len = @min(prefix.len, child_prefix_buf.len - ext.len);
    @memcpy(child_prefix_buf[0..pfx_len], prefix[0..pfx_len]);
    @memcpy(child_prefix_buf[pfx_len..][0..ext.len], ext);
    const child_prefix = child_prefix_buf[0 .. pfx_len + ext.len];

    try renderChildList(n.children, child_prefix, writer, counts);
}

/// Format the per-line label for a non-scope node.
/// Matches Go's formatNodeLabel in render.go byte-for-byte.
fn formatNodeLabel(n: Node) []const u8 {
    const S = struct {
        var buf: [512]u8 = undefined;
    };
    const r = if (std.mem.eql(u8, n.kind, "plan"))
        std.fmt.bufPrint(&S.buf, "plan:{d} [{s}]  {s}", .{ n.id, n.status, n.title })
    else if (std.mem.eql(u8, n.kind, "task"))
        std.fmt.bufPrint(&S.buf, "task:{d}  {s}  [{s}, pri:{d}]", .{ n.id, n.title, n.status, n.priority })
    else if (std.mem.eql(u8, n.kind, "artifact"))
        std.fmt.bufPrint(&S.buf, "artifact:{d}  {s}  [{s}, {s}]", .{ n.id, n.title, n.artifact_kind, n.status })
    else if (std.mem.eql(u8, n.kind, "decision"))
        std.fmt.bufPrint(&S.buf, "decision:{d}  {s}  [{s}]", .{ n.id, n.title, n.status })
    else if (std.mem.eql(u8, n.kind, "scenario"))
        std.fmt.bufPrint(&S.buf, "scenario:{d}  {s}  [{s}]", .{ n.id, n.title, n.status })
    else if (std.mem.eql(u8, n.kind, "question"))
        std.fmt.bufPrint(&S.buf, "question:{d}  {s}  [{s}]", .{ n.id, n.title, n.status })
    else
        std.fmt.bufPrint(&S.buf, "{s}:{d}  {s}", .{ n.kind, n.id, n.title });
    return r catch n.title;
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

var plan_slug_counter: i32 = 0;

fn insertPlan(d: *db.sqlite.Db, allocator: std.mem.Allocator, title: []const u8, scope_kind: []const u8) !i64 {
    plan_slug_counter +%= 1;
    const slug = try std.fmt.allocPrint(allocator, "tree-test-plan-{d}", .{plan_slug_counter});
    defer allocator.free(slug);
    return try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values (?, ?, ?, 'active')",
        &.{ .{ .text = scope_kind }, .{ .text = title }, .{ .text = slug } },
    );
}

fn insertChildPlan(d: *db.sqlite.Db, allocator: std.mem.Allocator, title: []const u8, scope_kind: []const u8, parent_id: i64) !i64 {
    plan_slug_counter +%= 1;
    const slug = try std.fmt.allocPrint(allocator, "tree-test-child-{d}", .{plan_slug_counter});
    defer allocator.free(slug);
    return try d.execParams(
        "insert into plans (scope_kind, title, slug, status, parent_plan_id) values (?, ?, ?, 'active', ?)",
        &.{ .{ .text = scope_kind }, .{ .text = title }, .{ .text = slug }, .{ .int = parent_id } },
    );
}

fn insertTask(d: *db.sqlite.Db, title: []const u8, plan_id: i64, status: []const u8) !i64 {
    return try d.execParams(
        "insert into tasks (scope_kind, title, status, priority, plan_id) values ('global', ?, ?, 100, ?)",
        &.{ .{ .text = title }, .{ .text = status }, .{ .int = plan_id } },
    );
}

fn insertQuestion(d: *db.sqlite.Db, title: []const u8) !i64 {
    return try d.execParams(
        "insert into questions (scope_kind, title, body, status) values ('global', ?, '', 'open')",
        &.{.{ .text = title }},
    );
}

fn insertDecision(d: *db.sqlite.Db, title: []const u8) !i64 {
    return try d.execParams(
        "insert into decisions (scope_kind, title, body, status) values ('global', ?, '', 'proposed')",
        &.{.{ .text = title }},
    );
}

fn insertArtifact(d: *db.sqlite.Db, title: []const u8) !i64 {
    return try d.execParams(
        "insert into artifacts (scope_kind, title, body, kind, status) values ('global', ?, '', 'other', 'draft')",
        &.{.{ .text = title }},
    );
}

fn insertScenario(d: *db.sqlite.Db, title: []const u8) !i64 {
    return try d.execParams(
        "insert into test_scenarios (scope_kind, title, body, status) values ('global', ?, '', 'draft')",
        &.{.{ .text = title }},
    );
}

fn insertLink(d: *db.sqlite.Db, from_kind: []const u8, from_id: i64, to_kind: []const u8, to_id: i64, rel: []const u8) !void {
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values (?, ?, ?, ?, ?)",
        &.{
            .{ .text = from_kind }, .{ .int = from_id },
            .{ .text = to_kind },   .{ .int = to_id },
            .{ .text = rel },
        },
    );
}

test "happy path: plan → child plan → task → linked question" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d, a, "Root Plan", "global");
    const child_plan_id = try insertChildPlan(&d, a, "Child Plan", "global", plan_id);
    _ = try insertTask(&d, "Top Task", plan_id, "todo");
    const q_id = try insertQuestion(&d, "Linked Question");
    try insertLink(&d, "question", q_id, "plan", plan_id, "derives-from");

    const roots = try build(&d, a, .{ .max_depth = -1 });
    defer deinitNodes(roots, a);

    try std.testing.expectEqual(@as(usize, 1), roots.len);
    try std.testing.expectEqualStrings("scope", roots[0].kind);

    var found_root_plan = false;
    for (roots[0].children) |c| {
        if (std.mem.eql(u8, c.kind, "plan") and c.id == plan_id) {
            found_root_plan = true;
            var found_child_plan = false;
            var found_task = false;
            var found_question = false;
            for (c.children) |gc| {
                if (std.mem.eql(u8, gc.kind, "plan") and gc.id == child_plan_id) found_child_plan = true;
                if (std.mem.eql(u8, gc.kind, "task")) found_task = true;
                if (std.mem.eql(u8, gc.kind, "question")) found_question = true;
            }
            try std.testing.expect(found_child_plan);
            try std.testing.expect(found_task);
            try std.testing.expect(found_question);
        }
    }
    try std.testing.expect(found_root_plan);
}

test "depth bound: depth=1 clips grandchildren" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d, a, "Root Plan Depth", "global");
    const child_id = try insertChildPlan(&d, a, "Child Plan Depth", "global", plan_id);
    _ = try insertChildPlan(&d, a, "Grandchild Plan Depth", "global", child_id);

    const roots = try build(&d, a, .{ .max_depth = 1 });
    defer deinitNodes(roots, a);

    var root_plan: ?Node = null;
    for (roots[0].children) |c| {
        if (c.id == plan_id) root_plan = c;
    }
    try std.testing.expect(root_plan != null);

    var found_child = false;
    for (root_plan.?.children) |c| {
        if (c.id == child_id) {
            found_child = true;
            // Grandchild must be absent (depth-capped at 1).
            try std.testing.expectEqual(@as(usize, 0), c.children.len);
        }
    }
    try std.testing.expect(found_child);
}

test "depth unbounded (-1): grandchildren present" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d, a, "Root Plan Unbounded", "global");
    const child_id = try insertChildPlan(&d, a, "Child Plan Unbounded", "global", plan_id);
    const grandchild_id = try insertChildPlan(&d, a, "Grandchild Plan Unbounded", "global", child_id);

    const roots = try build(&d, a, .{ .max_depth = -1 });
    defer deinitNodes(roots, a);

    var root_plan: ?Node = null;
    for (roots[0].children) |c| {
        if (c.id == plan_id) root_plan = c;
    }
    try std.testing.expect(root_plan != null);

    var found_child: ?Node = null;
    for (root_plan.?.children) |c| {
        if (c.id == child_id) found_child = c;
    }
    try std.testing.expect(found_child != null);

    var found_grandchild = false;
    for (found_child.?.children) |c| {
        if (c.id == grandchild_id) found_grandchild = true;
    }
    try std.testing.expect(found_grandchild);
}

test "kind filter: kind=[plan,task] excludes questions and decisions" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d, a, "Kinded Plan", "global");
    _ = try insertTask(&d, "Kinded Task", plan_id, "todo");
    const q_id = try insertQuestion(&d, "Kinded Question");
    try insertLink(&d, "question", q_id, "plan", plan_id, "derives-from");
    const dec_id = try insertDecision(&d, "Kinded Decision");
    try insertLink(&d, "decision", dec_id, "plan", plan_id, "derives-from");

    const kinds = [_][]const u8{ "plan", "task" };
    const roots = try build(&d, a, .{ .kinds = &kinds });
    defer deinitNodes(roots, a);

    var found_plan: ?Node = null;
    for (roots[0].children) |c| {
        if (c.id == plan_id) found_plan = c;
    }
    try std.testing.expect(found_plan != null);

    for (found_plan.?.children) |c| {
        try std.testing.expect(!std.mem.eql(u8, c.kind, "question"));
        try std.testing.expect(!std.mem.eql(u8, c.kind, "decision"));
    }
}

test "status filter: done plans excluded when filtering for active" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const active_id = try insertPlan(&d, a, "Active Status Plan", "global");
    plan_slug_counter +%= 1;
    const done_slug = try std.fmt.allocPrint(a, "done-plan-{d}", .{plan_slug_counter});
    defer a.free(done_slug);
    _ = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Done Status Plan', ?, 'done')",
        &.{.{ .text = done_slug }},
    );

    const statuses = [_][]const u8{"active"};
    const roots = try build(&d, a, .{ .statuses = &statuses });
    defer deinitNodes(roots, a);

    var found_active = false;
    var found_done = false;
    for (roots[0].children) |c| {
        if (c.id == active_id) found_active = true;
        if (std.mem.eql(u8, c.status, "done")) found_done = true;
    }
    try std.testing.expect(found_active);
    try std.testing.expect(!found_done);
}

test "scope filter: limits to known association; unknown slug → SlugNotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('tree-test-org', 'Tree Org', 'org')",
        &.{},
    );
    const assoc_id = try d.intQuery("select id from associations where slug = 'tree-test-org'");
    _ = try d.execParams(
        "insert into plans (scope_kind, scope_id, title, slug, status) values ('association', ?, 'Scoped Plan', 'scoped-plan-tree', 'active')",
        &.{.{ .int = assoc_id }},
    );
    _ = try insertPlan(&d, a, "Global Plan Scope Test", "global");

    const roots = try build(&d, a, .{ .scope = "tree-test-org" });
    defer deinitNodes(roots, a);

    try std.testing.expectEqual(@as(usize, 1), roots[0].children.len);
    try std.testing.expectEqualStrings("Scoped Plan", roots[0].children[0].title);

    try std.testing.expectError(Error.SlugNotFound, build(&d, a, .{ .scope = "no-such-tree-slug" }));
}

test "all-scopes: returns global + association scope roots" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try insertPlan(&d, a, "Global Plan AllScopes", "global");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('tree-all-org', 'AllOrg', 'org')",
        &.{},
    );

    const roots = try build(&d, a, .{ .all_scopes = true });
    defer deinitNodes(roots, a);

    // Must include global + at least the association scope.
    try std.testing.expect(roots.len >= 2);
    var found_global = false;
    for (roots) |r| {
        if (std.mem.eql(u8, r.scope_kind, "global")) found_global = true;
    }
    try std.testing.expect(found_global);
}

test "empty result: global scope with no plans returns empty children" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const roots = try build(&d, a, .{});
    defer deinitNodes(roots, a);

    try std.testing.expectEqual(@as(usize, 1), roots.len);
    try std.testing.expectEqualStrings("scope", roots[0].kind);
    try std.testing.expectEqual(@as(usize, 0), roots[0].children.len);
}

test "root_plan_id: only the specified plan is returned" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_a_id = try insertPlan(&d, a, "Plan A Root Filter", "global");
    _ = try insertPlan(&d, a, "Plan B Root Filter", "global");

    const roots = try build(&d, a, .{ .root_plan_id = plan_a_id });
    defer deinitNodes(roots, a);

    try std.testing.expectEqual(@as(usize, 1), roots[0].children.len);
    try std.testing.expectEqual(plan_a_id, roots[0].children[0].id);
}

test "linked entities: artifact and scenario surface as plan children" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d, a, "Links Plan", "global");
    const art_id = try insertArtifact(&d, "Linked Artifact");
    try insertLink(&d, "artifact", art_id, "plan", plan_id, "derives-from");
    const scen_id = try insertScenario(&d, "Linked Scenario");
    try insertLink(&d, "test_scenario", scen_id, "plan", plan_id, "derives-from");
    // Anchor task so the plan satisfies scaffold rules.
    _ = try insertTask(&d, "Anchor Task", plan_id, "todo");

    const roots = try build(&d, a, .{});
    defer deinitNodes(roots, a);

    var found_artifact = false;
    var found_scenario = false;
    for (roots[0].children) |p| {
        if (p.id == plan_id) {
            for (p.children) |c| {
                if (std.mem.eql(u8, c.kind, "artifact")) found_artifact = true;
                if (std.mem.eql(u8, c.kind, "scenario")) found_scenario = true;
            }
        }
    }
    try std.testing.expect(found_artifact);
    try std.testing.expect(found_scenario);
}

test "unknown kind in filter → error.UnknownKind" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const kinds = [_][]const u8{"bogus_kind"};
    try std.testing.expectError(Error.UnknownKind, build(&d, a, .{ .kinds = &kinds }));
}

test "renderText: produces expected status indicator and label format" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d, a, "Render Plan", "global");
    _ = try insertTask(&d, "Render Task", plan_id, "todo");

    const roots = try build(&d, a, .{});
    defer deinitNodes(roots, a);

    var buf: [4096]u8 = undefined;
    var w = std.Io.Writer.fixed(&buf);
    try renderText(roots, &w);

    const out = w.buffered();
    // Plan format: plan:<id> [active]  Render Plan
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "plan:"));
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "[active]"));
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "Render Plan"));
    // Task format: task:<id>  Render Task  [todo, pri:100]
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "task:"));
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "[todo, pri:100]"));
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "Render Task"));
}
