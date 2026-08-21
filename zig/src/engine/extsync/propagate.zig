//! engine/extsync/propagate — minimal feature-propagation orchestrator.
//!
//! Mirrors Go `internal/extsync/engine.go` (Propagate) at the surface the
//! M10 test-spec requires: walk the anchor plan's tree, render the
//! per-entity template, POST to the remote, record an external_links row.
//!
//! Out of scope for this slice (explicitly refused by the CLI layer until
//! the underlying behaviour ships):
//!   - --restrategize / strategy stickiness cache
//!   - --verify-counterparts / --unlink / --recreate
//!   - --github-strategy (parent-issue vs projects-v2 vs tracking-issue)
//!   - sub-issue / epic-child link wiring on the remote
//!
//! Strategy selection is fixed:
//!   - jira          → plan/child-plan: epic/story; task: sub-task
//!   - github-issues → plan/child-plan: parent-issue/issue; task: sub-task

const std = @import("std");
const db = @import("db");
const templates = @import("../templates.zig");
const external = @import("../external.zig");

pub const Operation = enum { created, skipped, failed };

pub const EntityResult = struct {
    entity_kind: []const u8,
    entity_id: i64,
    title: []const u8,
    op: Operation,
    external_id: []const u8 = "",
    external_url: []const u8 = "",
    error_message: []const u8 = "",
};

pub const Report = struct {
    anchor_plan_id: i64,
    system_slug: []const u8,
    strategy: []const u8,
    created: usize = 0,
    skipped: usize = 0,
    failed: usize = 0,
    results: []EntityResult = &.{},

    pub fn deinit(self: *Report, allocator: std.mem.Allocator) void {
        allocator.free(self.system_slug);
        allocator.free(self.strategy);
        for (self.results) |r| {
            allocator.free(r.entity_kind);
            allocator.free(r.title);
            allocator.free(r.external_id);
            allocator.free(r.external_url);
            allocator.free(r.error_message);
        }
        allocator.free(self.results);
    }
};

/// PropagateOpts captures the subset of Go's PropagateOpts the M10 slice
/// supports. `dry_run` prints what would be created without contacting the
/// remote. `sync_direction` defaults to "read-only" when empty.
pub const PropagateOpts = struct {
    system_slug: []const u8 = "",
    dry_run: bool = false,
    sync_direction: []const u8 = "",
};

/// Per-entity remote-create callback. The orchestrator builds the rendered
/// JSON payload and hands it to the caller, which performs the HTTP call
/// and returns the (external_id, external_url) it created.
pub const CreateRemoteFn = *const fn (
    user_ctx: *anyopaque,
    allocator: std.mem.Allocator,
    payload: []const u8,
    template_kind: []const u8,
) anyerror!CreatedRemote;

pub const CreatedRemote = struct {
    external_id: []u8,
    external_url: []u8,
};

/// Strategy fixes the (system_kind → template_kind) mapping per entity role.
pub const Strategy = struct {
    kind: []const u8,
    plan_anchor_kind: []const u8,
    plan_child_kind: []const u8,
    task_kind: []const u8,
};

/// strategyForSystem returns a default-shaped Strategy for the system kind
/// WITHOUT consulting the local DB. Used as a fallback when the caller has no
/// anchor plan handle (e.g. unit tests). Always returns
/// `github-parent-issue` for github-issues — callers that need ADR-0006
/// detection (zero-repo / parent-issue / projects-v2) MUST use
/// `selectStrategy` instead.
pub fn strategyForSystem(system_kind: []const u8) !Strategy {
    if (std.mem.eql(u8, system_kind, "jira")) {
        return .{
            .kind = "jira-epic",
            .plan_anchor_kind = "epic",
            .plan_child_kind = "story",
            .task_kind = "sub-task",
        };
    }
    if (std.mem.eql(u8, system_kind, "github-issues")) {
        return .{
            .kind = "github-parent-issue",
            .plan_anchor_kind = "parent-issue",
            .plan_child_kind = "issue",
            .task_kind = "sub-task",
        };
    }
    return error.UnsupportedSystemKind;
}

/// selectStrategy returns the propagation Strategy for `anchor_plan_id` against
/// the given system kind, implementing the ADR-0006 detection rule. Mirrors
/// Go `internal/extsync/selector.go::SelectStrategy`:
///
///   - "jira" → "jira-epic" regardless of repo count.
///   - "github-issues" → bucketed by `distinctReposInFeature` count:
///     - 0 repos  → "github-zero-repo"
///     - 1 repo   → "github-parent-issue"
///     - >=2 repos → "github-projects-v2"
///
/// Read-only against the local DB; no remote calls.
pub fn selectStrategy(
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
    system_kind: []const u8,
) !Strategy {
    if (std.mem.eql(u8, system_kind, "jira")) {
        return .{
            .kind = "jira-epic",
            .plan_anchor_kind = "epic",
            .plan_child_kind = "story",
            .task_kind = "sub-task",
        };
    }
    if (std.mem.eql(u8, system_kind, "github-issues")) {
        const count = try countDistinctReposInFeature(d, anchor_plan_id);
        const kind: []const u8 = if (count == 0)
            "github-zero-repo"
        else if (count == 1)
            "github-parent-issue"
        else
            "github-projects-v2";
        return .{
            .kind = kind,
            .plan_anchor_kind = "parent-issue",
            .plan_child_kind = "issue",
            .task_kind = "sub-task",
        };
    }
    return error.UnsupportedSystemKind;
}

/// countDistinctReposInFeature returns the count of DISTINCT repos touched by
/// the feature anchored at `anchor_plan_id`, considering tasks attached via
/// either `tasks.plan_id` (direct FK) or `entity_links(task→plan,
/// derives-from)` recursively across the plan subtree, and including both
/// `tasks.scope_kind = 'repo'` and `entity_links(task→repo, touches)` as the
/// repo membership signal. Mirrors Go `tree.go::distinctReposInFeature`.
pub fn countDistinctReposInFeature(d: *db.sqlite.Db, anchor_plan_id: i64) !usize {
    const q =
        \\with recursive plan_tree(id) as (
        \\  select ? union all
        \\  select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id
        \\),
        \\tasks_in_tree as (
        \\  select t.id, t.scope_kind, t.scope_id
        \\  from tasks t join plan_tree pt on t.plan_id = pt.id
        \\  union
        \\  select t.id, t.scope_kind, t.scope_id
        \\  from tasks t
        \\  join entity_links el on el.from_kind = 'task' and el.from_id = t.id
        \\                       and el.to_kind = 'plan' and el.relationship = 'derives-from'
        \\  join plan_tree pt on el.to_id = pt.id
        \\),
        \\task_repos as (
        \\  select scope_id as repo_id from tasks_in_tree where scope_kind = 'repo'
        \\  union
        \\  select el2.to_id as repo_id
        \\  from tasks_in_tree tit
        \\  join entity_links el2 on el2.from_kind = 'task' and el2.from_id = tit.id
        \\                        and el2.to_kind = 'repo' and el2.relationship = 'touches'
        \\)
        \\select count(distinct repo_id) from task_repos where repo_id is not null
    ;
    var stmt = try d.prepare(q);
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = anchor_plan_id }});
    const step = try stmt.step();
    if (step == .done) return 0;
    const n = stmt.columnInt(0);
    if (n < 0) return 0;
    return @intCast(n);
}

/// TreeEntry describes one entity discovered while walking a feature tree.
pub const TreeEntry = struct {
    kind: enum { plan_anchor, plan_child, task },
    id: i64,
    title: []const u8,
};

/// freeTree releases the allocations owned by `entries`.
pub fn freeTree(entries: []const TreeEntry, allocator: std.mem.Allocator) void {
    for (entries) |e| allocator.free(e.title);
    allocator.free(entries);
}

/// walkTree enumerates the entities to propagate for `anchor_plan_id` in
/// top-down order: anchor → child plans (recursive) → tasks linked via
/// derives-from to any plan in the tree.
pub fn walkTree(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
) ![]TreeEntry {
    var entries: std.ArrayList(TreeEntry) = .empty;
    errdefer {
        for (entries.items) |e| allocator.free(e.title);
        entries.deinit(allocator);
    }

    // Anchor.
    {
        const title = try loadPlanTitle(allocator, d, anchor_plan_id);
        try entries.append(allocator, .{ .kind = .plan_anchor, .id = anchor_plan_id, .title = title });
    }

    // Child plans BFS by parent_plan_id.
    var frontier: std.ArrayList(i64) = .empty;
    defer frontier.deinit(allocator);
    try frontier.append(allocator, anchor_plan_id);

    while (frontier.items.len > 0) {
        const cur = frontier.orderedRemove(0);
        var stmt = try d.prepare("select id, coalesce(title,'') from plans where parent_plan_id = ? order by id");
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = cur }});
        while (true) {
            const step = try stmt.step();
            if (step == .done) break;
            const id = stmt.columnInt(0);
            const title = try stmt.columnTextAlloc(1, allocator);
            try entries.append(allocator, .{ .kind = .plan_child, .id = id, .title = title });
            try frontier.append(allocator, id);
        }
    }

    // Tasks derives-from any plan in the collected set.
    // Collect plan IDs into a comma-list (small N typical for features).
    var plan_ids: std.ArrayList(i64) = .empty;
    defer plan_ids.deinit(allocator);
    for (entries.items) |e| try plan_ids.append(allocator, e.id);

    for (plan_ids.items) |plan_id| {
        var stmt = try d.prepare(
            \\select distinct t.id, coalesce(t.title,'') from tasks t
            \\join entity_links el on el.from_kind='task' and el.from_id=t.id
            \\where el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'
            \\order by t.id
        );
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = plan_id }});
        while (true) {
            const step = try stmt.step();
            if (step == .done) break;
            const id = stmt.columnInt(0);
            const title = try stmt.columnTextAlloc(1, allocator);
            try entries.append(allocator, .{ .kind = .task, .id = id, .title = title });
        }
    }

    return try entries.toOwnedSlice(allocator);
}

fn loadPlanTitle(allocator: std.mem.Allocator, d: *db.sqlite.Db, plan_id: i64) ![]const u8 {
    var stmt = try d.prepare("select coalesce(title,'') from plans where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = plan_id }});
    const step = try stmt.step();
    if (step == .done) return error.NotFound;
    return try stmt.columnTextAlloc(0, allocator);
}

/// hasExistingMirrorLink returns true when an `external_links(link_role='mirror')`
/// row already exists for this entity and system.
pub fn hasExistingMirrorLink(
    d: *db.sqlite.Db,
    entity_kind: []const u8,
    entity_id: i64,
    system_id: i64,
) !bool {
    var stmt = try d.prepare(
        \\select coalesce(external_id, '') from external_links
        \\where entity_kind = ? and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .text = entity_kind }, .{ .int = entity_id }, .{ .int = system_id } });
    return (try stmt.step()) == .row;
}

/// loadExistingMirror returns the external_id of an existing mirror link
/// for the entity, or empty when none exists.
pub fn loadExistingMirror(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    entity_kind: []const u8,
    entity_id: i64,
    system_id: i64,
) ![]const u8 {
    var stmt = try d.prepare(
        \\select coalesce(external_id, '') from external_links
        \\where entity_kind = ? and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .text = entity_kind }, .{ .int = entity_id }, .{ .int = system_id } });
    const step = try stmt.step();
    if (step == .done) return try allocator.dupe(u8, "");
    return try stmt.columnTextAlloc(0, allocator);
}

test "strategyForSystem maps jira and github-issues" {
    const a = std.testing.allocator;
    _ = a;
    const jira = try strategyForSystem("jira");
    try std.testing.expectEqualStrings("jira-epic", jira.kind);
    try std.testing.expectEqualStrings("epic", jira.plan_anchor_kind);
    const gh = try strategyForSystem("github-issues");
    try std.testing.expectEqualStrings("github-parent-issue", gh.kind);
    try std.testing.expectEqualStrings("parent-issue", gh.plan_anchor_kind);
    try std.testing.expectError(error.UnsupportedSystemKind, strategyForSystem("acme"));
}

fn setupSelectStrategyDb() !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    try @import("db").migrate.applyAll(&d, std.testing.allocator);
    return d;
}

test "selectStrategy returns jira-epic regardless of repo count" {
    var d = try setupSelectStrategyDb();
    defer d.close();
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','a','a')", &.{});
    const s = try selectStrategy(&d, 1, "jira");
    try std.testing.expectEqualStrings("jira-epic", s.kind);
}

test "selectStrategy github-issues with no touched repos -> zero-repo" {
    var d = try setupSelectStrategyDb();
    defer d.close();
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});
    const s = try selectStrategy(&d, 1, "github-issues");
    try std.testing.expectEqualStrings("github-zero-repo", s.kind);
}

test "selectStrategy github-issues with one touched repo via touches link -> parent-issue" {
    var d = try setupSelectStrategyDb();
    defer d.close();
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/api','acme/api','https://github.com/acme/api.git')", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, title, plan_id) values ('global','t',1)", &.{});
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task',1,'repo',1,'touches')",
        &.{},
    );
    const s = try selectStrategy(&d, 1, "github-issues");
    try std.testing.expectEqualStrings("github-parent-issue", s.kind);
}

test "selectStrategy github-issues with one repo-scoped task -> parent-issue" {
    var d = try setupSelectStrategyDb();
    defer d.close();
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/api','acme/api','https://github.com/acme/api.git')", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, scope_id, title, plan_id) values ('repo',1,'t',1)", &.{});
    const s = try selectStrategy(&d, 1, "github-issues");
    try std.testing.expectEqualStrings("github-parent-issue", s.kind);
}

test "selectStrategy github-issues with two touched repos -> projects-v2" {
    var d = try setupSelectStrategyDb();
    defer d.close();
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/api','acme/api','https://github.com/acme/api.git')", &.{});
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/web','acme/web','https://github.com/acme/web.git')", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, title, plan_id) values ('global','t1',1)", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, title, plan_id) values ('global','t2',1)", &.{});
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task',1,'repo',1,'touches')",
        &.{},
    );
    _ = try d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task',2,'repo',2,'touches')",
        &.{},
    );
    const s = try selectStrategy(&d, 1, "github-issues");
    try std.testing.expectEqualStrings("github-projects-v2", s.kind);
}

test "selectStrategy rejects unsupported system kind" {
    var d = try setupSelectStrategyDb();
    defer d.close();
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','a','a')", &.{});
    try std.testing.expectError(error.UnsupportedSystemKind, selectStrategy(&d, 1, "acme"));
}
