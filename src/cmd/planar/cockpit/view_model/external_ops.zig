//! External operational-plane cockpit queries and their tests.

const std = @import("std");
const db = @import("db");
const testing = std.testing;
const resolveEntityTitle = @import("entity_title.zig").resolve;

// =========================================================================
// External / Ops Plane view-model  (tasks 4029, 4030, 4031)
// =========================================================================
//
// Schema reference: migrations/00006_external.up.sql
//
//   external_systems (id, kind, slug, base_url, default_project,
//                     auth_method, auth_ref, created_at, updated_at)
//     kind CHECK: 'jira','github-issues','gitlab-issues','linear'
//
//   external_links (id, entity_kind, entity_id, system_id, external_id,
//                   external_url, link_role, sync_direction,
//                   last_synced_at, last_sync_status, config_json,
//                   created_at)
//     last_sync_status CHECK: 'ok','conflict','error','never'
//     link_role CHECK: 'mirror','parent','child','reference'
//
//   sync_events (id, link_id, scope, direction, outcome, fields_changed,
//                detail, context_json, at)
//     outcome CHECK: 'ok','conflict','error','noop','resolved-fs',
//                    'resolved-db','partial','success','failure',
//                    'strategy-abandoned','counterpart-missing'
//     UNRESOLVED CONFLICTS: outcome = 'conflict' (no subsequent
//       'resolved-fs' or 'resolved-db' for the same link_id)
//
// Conflict resolution: A sync_event with outcome='conflict' is
// UNRESOLVED when there is no later sync_event on the same link_id
// with outcome in ('resolved-fs','resolved-db').  That is:
//   select se.* from sync_events se
//   where se.outcome = 'conflict'
//     and not exists (
//       select 1 from sync_events r
//       where r.link_id = se.link_id
//         and r.outcome in ('resolved-fs','resolved-db')
//         and r.at > se.at
//     )
// When link_id is null (workbench conflicts) the conflict is always
// surfaced because there is no link_id to correlate a resolution against.

/// One registered external system row (task 4029).
///
/// Derived from external_systems.
pub const ExtSystemRow = struct {
    id: i64,
    /// System kind: 'jira', 'github-issues', 'gitlab-issues', 'linear'.
    kind: []const u8,
    /// Human-readable slug (unique identifier).
    slug: []const u8,
    /// Optional base URL.
    base_url: ?[]const u8,
    /// Optional default project key/org-repo.
    default_project: ?[]const u8,
    /// Count of external_links rows referencing this system.
    link_count: i64,
    /// Count of links with last_sync_status = 'conflict'.
    conflict_count: i64,
    /// Count of links with last_sync_status = 'error'.
    error_count: i64,
    /// Pre-formatted display text: "[kind] slug (N links)" — heap-allocated
    /// so grapheme pointers remain valid after render.
    display_text: []const u8,

    pub fn deinit(self: ExtSystemRow, allocator: std.mem.Allocator) void {
        allocator.free(self.kind);
        allocator.free(self.slug);
        if (self.base_url) |s| allocator.free(s);
        if (self.default_project) |s| allocator.free(s);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []ExtSystemRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One external_links row with resolved local-entity title (task 4029).
pub const ExtLinkRow = struct {
    id: i64,
    /// system_id for grouping.
    system_id: i64,
    /// Entity kind (plan, task, etc.).
    entity_kind: []const u8,
    /// Entity id.
    entity_id: i64,
    /// Resolved local-entity title (best-effort; may be "(unknown)" on OOM).
    entity_title: []const u8,
    /// External ticket/issue id.
    external_id: []const u8,
    /// Optional external URL.
    external_url: ?[]const u8,
    /// Link role: 'mirror','parent','child','reference'.
    link_role: []const u8,
    /// Sync direction: 'read-only','write-back','two-way'.
    sync_direction: []const u8,
    /// Last sync timestamp (ISO-8601) or null if never synced.
    last_synced_at: ?[]const u8,
    /// Mapping/sync status: 'ok','conflict','error','never'.
    last_sync_status: []const u8,
    /// Pre-formatted display text: "entity_kind:id — status" — heap-allocated.
    display_text: []const u8,

    pub fn deinit(self: ExtLinkRow, allocator: std.mem.Allocator) void {
        allocator.free(self.entity_kind);
        allocator.free(self.entity_title);
        allocator.free(self.external_id);
        if (self.external_url) |s| allocator.free(s);
        allocator.free(self.link_role);
        allocator.free(self.sync_direction);
        if (self.last_synced_at) |s| allocator.free(s);
        allocator.free(self.last_sync_status);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []ExtLinkRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Latest sync event for a given external system (task 4030).
///
/// Surfaces the most recent sync event (by `at`) for any link belonging to
/// the system — giving a per-system "last-sync" summary row.
pub const ExtSystemSyncStatus = struct {
    system_id: i64,
    /// ISO-8601 timestamp of the most recent sync event.
    last_sync_at: []const u8,
    /// Outcome of the most recent sync event.
    last_outcome: []const u8,
    /// Direction of the most recent sync event: 'pull' or 'push'.
    last_direction: []const u8,

    pub fn deinit(self: ExtSystemSyncStatus, allocator: std.mem.Allocator) void {
        allocator.free(self.last_sync_at);
        allocator.free(self.last_outcome);
        allocator.free(self.last_direction);
    }
};

/// One unresolved sync conflict row (task 4031).
///
/// A sync_event with outcome='conflict' is UNRESOLVED when no later event
/// on the same link_id has outcome in ('resolved-fs','resolved-db').
/// When link_id is null the conflict is always unresolved.
pub const UnresolvedConflictRow = struct {
    id: i64,
    /// Nullable link_id.
    link_id: ?i64,
    /// Scope: 'external' or 'workbench'.
    scope: []const u8,
    /// Direction: 'pull' or 'push'.
    direction: []const u8,
    /// Optional detail text.
    detail: ?[]const u8,
    /// Conflict timestamp (ISO-8601).
    at: []const u8,
    /// Pre-formatted display text: "scope:direction [at]" — heap-allocated.
    display_text: []const u8,

    pub fn deinit(self: UnresolvedConflictRow, allocator: std.mem.Allocator) void {
        allocator.free(self.scope);
        allocator.free(self.direction);
        if (self.detail) |s| allocator.free(s);
        allocator.free(self.at);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []UnresolvedConflictRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Full snapshot for the External/Ops Plane view (tasks 4029, 4030, 4031).
pub const ExtOpsSnapshot = struct {
    /// All registered external systems (task 4029).
    systems: []ExtSystemRow,
    /// All external links across all systems (task 4029).
    links: []ExtLinkRow,
    /// Per-system latest sync status (task 4030).  May have fewer entries
    /// than systems when a system has no sync events yet.
    system_sync: []ExtSystemSyncStatus,
    /// Unresolved sync conflicts (task 4031).
    conflicts: []UnresolvedConflictRow,

    pub fn deinit(self: ExtOpsSnapshot, allocator: std.mem.Allocator) void {
        ExtSystemRow.deinitMany(self.systems, allocator);
        ExtLinkRow.deinitMany(self.links, allocator);
        for (self.system_sync) |ss| ss.deinit(allocator);
        allocator.free(self.system_sync);
        UnresolvedConflictRow.deinitMany(self.conflicts, allocator);
    }
};

/// Query all registered external systems, enriched with per-system link
/// counts and conflict/error tallies (task 4029).
pub fn queryExtSystems(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]ExtSystemRow {
    var stmt = d.prepare(
        \\select
        \\  es.id, es.kind, es.slug,
        \\  es.base_url, es.default_project,
        \\  (select count(*) from external_links el where el.system_id = es.id) as link_count,
        \\  (select count(*) from external_links el where el.system_id = es.id and el.last_sync_status = 'conflict') as conflict_count,
        \\  (select count(*) from external_links el where el.system_id = es.id and el.last_sync_status = 'error') as error_count
        \\from external_systems es
        \\order by es.id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(ExtSystemRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const kind = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(kind);
                const slug = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(slug);
                const base_url = try stmt.columnTextOpt(3, allocator);
                errdefer if (base_url) |s| allocator.free(s);
                const default_project = try stmt.columnTextOpt(4, allocator);
                errdefer if (default_project) |s| allocator.free(s);
                const link_count = stmt.columnInt(5);
                const conflict_count = stmt.columnInt(6);
                const error_count = stmt.columnInt(7);

                // Build display_text: "[kind] slug (N links)"
                // MEMORY GUARD (rule (c)): display_text is always freshly allocated;
                // never aliases kind or slug.
                const display_text = std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s} ({d} links)",
                    .{ kind, slug, link_count },
                ) catch try allocator.dupe(u8, slug);
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .kind = kind,
                    .slug = slug,
                    .base_url = base_url,
                    .default_project = default_project,
                    .link_count = link_count,
                    .conflict_count = conflict_count,
                    .error_count = error_count,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query all external_links rows, with resolved local-entity titles (task 4029).
///
/// Links are ordered by system_id asc, entity_kind asc, entity_id asc so the
/// caller can group them by system when rendering.
pub fn queryExtLinks(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]ExtLinkRow {
    var stmt = d.prepare(
        \\select
        \\  el.id, el.system_id,
        \\  el.entity_kind, el.entity_id,
        \\  el.external_id, el.external_url,
        \\  el.link_role, el.sync_direction,
        \\  el.last_synced_at, el.last_sync_status
        \\from external_links el
        \\order by el.system_id asc, el.entity_kind asc, el.entity_id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(ExtLinkRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const system_id = stmt.columnInt(1);
                const entity_kind = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(entity_kind);
                const entity_id = stmt.columnInt(3);
                const external_id = try stmt.columnTextAlloc(4, allocator);
                errdefer allocator.free(external_id);
                const external_url = try stmt.columnTextOpt(5, allocator);
                errdefer if (external_url) |s| allocator.free(s);
                const link_role = try stmt.columnTextAlloc(6, allocator);
                errdefer allocator.free(link_role);
                const sync_direction = try stmt.columnTextAlloc(7, allocator);
                errdefer allocator.free(sync_direction);
                const last_synced_at = try stmt.columnTextOpt(8, allocator);
                errdefer if (last_synced_at) |s| allocator.free(s);
                const last_sync_status = try stmt.columnTextAlloc(9, allocator);
                errdefer allocator.free(last_sync_status);

                // Resolve entity title via resolveEntityTitle. On OOM/error,
                // MEMORY GUARD (rule (c)): use a freshly duped placeholder,
                // never alias entity_kind.
                const entity_title = resolveEntityTitle(d, allocator, entity_kind, entity_id) orelse
                    allocator.dupe(u8, "(unknown)") catch try allocator.dupe(u8, "");
                errdefer allocator.free(entity_title);

                // Build display_text: "entity_kind:id — status"
                // MEMORY GUARD: always freshly allocated.
                const display_text = std.fmt.allocPrint(
                    allocator,
                    "{s}:{d} — {s}",
                    .{ entity_kind, entity_id, last_sync_status },
                ) catch try allocator.dupe(u8, last_sync_status);
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .system_id = system_id,
                    .entity_kind = entity_kind,
                    .entity_id = entity_id,
                    .entity_title = entity_title,
                    .external_id = external_id,
                    .external_url = external_url,
                    .link_role = link_role,
                    .sync_direction = sync_direction,
                    .last_synced_at = last_synced_at,
                    .last_sync_status = last_sync_status,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query the latest sync event per external system (task 4030).
///
/// For each system_id that has at least one sync_event (via external_links),
/// returns the most recent event. Systems with no events are omitted.
pub fn queryExtSystemSyncStatus(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]ExtSystemSyncStatus {
    // Correlated subquery: for each system, pick the sync_event with the
    // maximum `at` value (lexicographic ISO-8601 sort works correctly).
    // Tie-break by max(id) when two events share the same `at` timestamp
    // so the result is deterministic regardless of insertion order.
    var stmt = d.prepare(
        \\select
        \\  el.system_id,
        \\  se.at,
        \\  se.outcome,
        \\  se.direction
        \\from sync_events se
        \\join external_links el on el.id = se.link_id
        \\where se.id = (
        \\  select se2.id
        \\  from sync_events se2
        \\  join external_links el2 on el2.id = se2.link_id
        \\  where el2.system_id = el.system_id
        \\  order by se2.at desc, se2.id desc
        \\  limit 1
        \\)
        \\group by el.system_id
        \\order by el.system_id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(ExtSystemSyncStatus) = .empty;
    errdefer {
        for (out.items) |ss| ss.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const system_id = stmt.columnInt(0);
                const last_sync_at = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(last_sync_at);
                const last_outcome = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(last_outcome);
                const last_direction = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(last_direction);

                try out.append(allocator, .{
                    .system_id = system_id,
                    .last_sync_at = last_sync_at,
                    .last_outcome = last_outcome,
                    .last_direction = last_direction,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query unresolved sync conflicts (task 4031).
///
/// The two scope classes use different resolution semantics and are queried
/// separately, then unified via UNION ALL:
///
/// EXTERNAL conflicts: driven from external_links.last_sync_status='conflict'.
///   A conflicted external link is cleared by a later successful sync that
///   flips last_sync_status back to 'ok' (via updateSyncState in external/sync.zig).
///   The external sync Outcome enum never emits 'resolved-fs' / 'resolved-db';
///   those outcomes are workbench-only.  For display info (direction, detail, at)
///   we join to the latest sync_event with outcome='conflict' for that link,
///   ordered by at desc then id desc for determinism when timestamps tie.
///
/// WORKBENCH conflicts: from sync_events where scope='workbench' and outcome='conflict'.
///   Workbench conflicts are resolved by an in-place UPDATE of the sync_events row
///   to outcome='resolved-fs' or 'resolved-db' (workbench/sync.zig ~line 202-205).
///   A row still showing outcome='conflict' is genuinely unresolved.
///   Workbench conflict rows have link_id IS NULL.
///
/// Both the systems navigator's conflict_count (which reads external_links.last_sync_status)
/// and this surface now draw from the same signal for external links, so the two
/// panes cannot contradict each other.
///
/// Returns rows ordered by at desc (newest unresolved conflicts first).
pub fn queryUnresolvedConflicts(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]UnresolvedConflictRow {
    // UNION ALL of:
    //   Part A — external conflicts: one row per external_link at last_sync_status='conflict',
    //            with display info from the latest conflict sync_event for that link.
    //            Uses a correlated subquery with max(id) tiebreak for determinism.
    //   Part B — workbench conflicts: sync_event rows still at outcome='conflict'
    //            (in-place resolution means outcome='conflict' <=> unresolved).
    var stmt = d.prepare(
        \\select
        \\  coalesce(se.id, -el.id) as sort_id,
        \\  el.id          as link_id,
        \\  'external'     as scope,
        \\  coalesce(se.direction, 'pull') as direction,
        \\  se.detail,
        \\  coalesce(se.at, el.last_synced_at, el.created_at) as at
        \\from external_links el
        \\left join sync_events se
        \\  on se.id = (
        \\    select id from sync_events
        \\    where link_id = el.id
        \\      and outcome = 'conflict'
        \\    order by at desc, id desc
        \\    limit 1
        \\  )
        \\where el.last_sync_status = 'conflict'
        \\union all
        \\select
        \\  se.id as sort_id,
        \\  null  as link_id,
        \\  'workbench' as scope,
        \\  se.direction,
        \\  se.detail,
        \\  se.at
        \\from sync_events se
        \\where se.scope = 'workbench'
        \\  and se.outcome = 'conflict'
        \\order by at desc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(UnresolvedConflictRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const link_id = stmt.columnIntOpt(1);
                const scope = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(scope);
                const direction = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(direction);
                const detail = try stmt.columnTextOpt(4, allocator);
                errdefer if (detail) |s| allocator.free(s);
                const at = try stmt.columnTextAlloc(5, allocator);
                errdefer allocator.free(at);

                // Build display_text: "scope:direction [at-prefix]"
                // MEMORY GUARD: always freshly allocated; never aliases scope/direction/at.
                const at_prefix: []const u8 = if (at.len >= 16) at[0..16] else at;
                const display_text = std.fmt.allocPrint(
                    allocator,
                    "{s}:{s} [{s}]",
                    .{ scope, direction, at_prefix },
                ) catch try allocator.dupe(u8, scope);
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .link_id = link_id,
                    .scope = scope,
                    .direction = direction,
                    .detail = detail,
                    .at = at,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query the full ExtOpsSnapshot (tasks 4029, 4030, 4031).
/// Caller owns the result; free via `snapshot.deinit(allocator)`.
pub fn queryExtOpsSnapshot(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) !ExtOpsSnapshot {
    const systems = try queryExtSystems(d, allocator);
    errdefer ExtSystemRow.deinitMany(systems, allocator);

    const links = try queryExtLinks(d, allocator);
    errdefer ExtLinkRow.deinitMany(links, allocator);

    const system_sync = try queryExtSystemSyncStatus(d, allocator);
    errdefer {
        for (system_sync) |ss| ss.deinit(allocator);
        allocator.free(system_sync);
    }

    const conflicts = try queryUnresolvedConflicts(d, allocator);
    errdefer UnresolvedConflictRow.deinitMany(conflicts, allocator);

    return .{
        .systems = systems,
        .links = links,
        .system_sync = system_sync,
        .conflicts = conflicts,
    };
}

// =========================================================================
// view_model external-ops tests
// =========================================================================

fn setupTestDbExtOps(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "view_model: queryExtSystems on empty DB yields empty slice" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const rows = try queryExtSystems(&d, a);
    defer ExtSystemRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryExtSystems returns registered system with link counts (task 4029)" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        \\insert into external_systems (kind, slug, auth_method, auth_ref)
        \\values ('github-issues', 'gh-rdrsss', 'gh-cli', 'gh')
    , &.{});

    // Seed a task.
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Linked Task','todo')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status)
        \\values ('task', ?, ?, 'ISSUE-42', 'ok')
    , &.{ .{ .int = tid }, .{ .int = sys_id } });

    const rows = try queryExtSystems(&d, a);
    defer ExtSystemRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqualStrings("github-issues", rows[0].kind);
    try testing.expectEqualStrings("gh-rdrsss", rows[0].slug);
    try testing.expectEqual(@as(i64, 1), rows[0].link_count);
    try testing.expectEqual(@as(i64, 0), rows[0].conflict_count);
    try testing.expectEqual(@as(i64, 0), rows[0].error_count);
    // display_text must contain slug and link count.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "gh-rdrsss") != null);
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "1") != null);
}

test "view_model: queryExtLinks returns link with entity title (task 4029)" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-corp','token-env','JIRA_TOKEN')",
        &.{},
    );
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global','My Plan','my-plan','active')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into external_links
        \\  (entity_kind, entity_id, system_id, external_id, link_role, sync_direction, last_sync_status)
        \\values ('plan', ?, ?, 'PROJ-1', 'mirror', 'two-way', 'ok')
    , &.{ .{ .int = pid }, .{ .int = sys_id } });

    const links = try queryExtLinks(&d, a);
    defer ExtLinkRow.deinitMany(links, a);

    try testing.expectEqual(@as(usize, 1), links.len);
    try testing.expectEqualStrings("plan", links[0].entity_kind);
    try testing.expectEqual(pid, links[0].entity_id);
    // Entity title resolved from plans table.
    try testing.expectEqualStrings("My Plan", links[0].entity_title);
    try testing.expectEqualStrings("PROJ-1", links[0].external_id);
    try testing.expectEqualStrings("ok", links[0].last_sync_status);
    // display_text must contain entity_kind:id and status.
    try testing.expect(std.mem.indexOf(u8, links[0].display_text, "plan:") != null);
    try testing.expect(std.mem.indexOf(u8, links[0].display_text, "ok") != null);
}

test "view_model: queryExtLinks unknown entity title falls back to placeholder" {
    // When entity_kind is an entity type resolveEntityTitle cannot look up,
    // entity_title is the placeholder "(unknown)" — not an alias of entity_kind.
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','corp','token-env','T')",
        &.{},
    );
    // Insert with entity_kind='session' which resolveEntityTitle does not handle.
    _ = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('session', 1, ?, 'S-1', 'never')",
        &.{.{ .int = sys_id }},
    );

    const links = try queryExtLinks(&d, a);
    defer ExtLinkRow.deinitMany(links, a);

    try testing.expectEqual(@as(usize, 1), links.len);
    // Placeholder must be its own allocation, not a pointer alias.
    const title = links[0].entity_title;
    try testing.expect(title.len > 0); // either "(unknown)" or ""
    // The important invariant: entity_title != entity_kind pointer.
    // We verify by content — if it were an alias of entity_kind it would be "session".
    try testing.expect(!std.mem.eql(u8, title, "session"));
}

test "view_model: queryExtSystemSyncStatus returns latest sync event (task 4030)" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('github-issues','gh-test','gh-cli','gh')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-1', 'ok')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    // Insert two sync events; the later one should be returned.
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'pull', 'ok', '2024-01-01T10:00:00.000Z')",
        &.{.{ .int = link_id }},
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'push', 'ok', '2025-06-01T12:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    const sync_statuses = try queryExtSystemSyncStatus(&d, a);
    defer {
        for (sync_statuses) |ss| ss.deinit(a);
        a.free(sync_statuses);
    }

    try testing.expectEqual(@as(usize, 1), sync_statuses.len);
    try testing.expectEqual(sys_id, sync_statuses[0].system_id);
    // Latest event: the 2025-06-01 push.
    try testing.expect(std.mem.indexOf(u8, sync_statuses[0].last_sync_at, "2025") != null);
    try testing.expectEqualStrings("ok", sync_statuses[0].last_outcome);
    try testing.expectEqualStrings("push", sync_statuses[0].last_direction);
}

test "view_model: queryExtSystemSyncStatus id tiebreak is deterministic (task 4030)" {
    // When two sync_events share the same `at` timestamp, the one with the
    // higher id (inserted later) must win.  Without the `id desc` tiebreak
    // SQLite's arbitrary GROUP BY row selection could return either one.
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('github-issues','gh-tie','gh-cli','gh')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-T', 'ok')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    // Two events at the same timestamp; the second (higher id) should win.
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'pull', 'conflict', '2025-05-01T00:00:00.000Z')",
        &.{.{ .int = link_id }},
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'push', 'ok', '2025-05-01T00:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    const sync_statuses = try queryExtSystemSyncStatus(&d, a);
    defer {
        for (sync_statuses) |ss| ss.deinit(a);
        a.free(sync_statuses);
    }

    try testing.expectEqual(@as(usize, 1), sync_statuses.len);
    // Higher-id event (push / ok) must win the tiebreak.
    try testing.expectEqualStrings("ok", sync_statuses[0].last_outcome);
    try testing.expectEqualStrings("push", sync_statuses[0].last_direction);
}

test "view_model: queryUnresolvedConflicts returns unresolved conflict (task 4031)" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-c','token-env','J')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-1', 'conflict')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, detail, at) values (?, 'external', 'pull', 'conflict', 'field mismatch', '2025-01-01T10:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    const conflicts = try queryUnresolvedConflicts(&d, a);
    defer UnresolvedConflictRow.deinitMany(conflicts, a);

    try testing.expectEqual(@as(usize, 1), conflicts.len);
    try testing.expectEqualStrings("external", conflicts[0].scope);
    try testing.expectEqualStrings("pull", conflicts[0].direction);
    try testing.expect(conflicts[0].detail != null);
    try testing.expectEqualStrings("field mismatch", conflicts[0].detail.?);
    try testing.expect(std.mem.indexOf(u8, conflicts[0].at, "2025") != null);
    // display_text: must contain scope and direction.
    try testing.expect(std.mem.indexOf(u8, conflicts[0].display_text, "external") != null);
    try testing.expect(std.mem.indexOf(u8, conflicts[0].display_text, "pull") != null);
}

test "view_model: queryUnresolvedConflicts external link re-synced ok drops from surface (task 4031)" {
    // Real external clear path: a conflicted external link is cleared when
    // updateSyncState flips last_sync_status back to 'ok' and a later 'ok'
    // sync_event is appended.  The external Outcome enum never emits
    // 'resolved-fs'/'resolved-db' — those are workbench-only.
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-r','token-env','J')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    const link_id = try d.execParams(
        // last_sync_status='ok' — the re-sync cleared the conflict.
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-1', 'ok')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    // conflict event followed by a later ok event (real production shape).
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'pull', 'conflict', '2025-01-01T10:00:00.000Z')",
        &.{.{ .int = link_id }},
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, at) values (?, 'external', 'push', 'ok', '2025-01-02T10:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    const conflicts = try queryUnresolvedConflicts(&d, a);
    defer UnresolvedConflictRow.deinitMany(conflicts, a);

    // last_sync_status='ok' → not in the unresolved surface.
    try testing.expectEqual(@as(usize, 0), conflicts.len);
}

test "view_model: queryUnresolvedConflicts external link still at conflict remains (task 4031)" {
    // Counterpart: external link still at last_sync_status='conflict' (no re-sync yet)
    // MUST appear in the unresolved surface.  The systems navigator computes
    // conflict_count from the same column, so the two panes agree.
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    const sys_id = try d.execParams(
        "insert into external_systems (kind, slug, auth_method, auth_ref) values ('jira','jira-s','token-env','J')",
        &.{},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','T','todo')",
        &.{},
    );
    const link_id = try d.execParams(
        "insert into external_links (entity_kind, entity_id, system_id, external_id, last_sync_status) values ('task', ?, ?, 'I-2', 'conflict')",
        &.{ .{ .int = tid }, .{ .int = sys_id } },
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, detail, at) values (?, 'external', 'pull', 'conflict', 'field clash', '2025-03-01T09:00:00.000Z')",
        &.{.{ .int = link_id }},
    );

    const conflicts = try queryUnresolvedConflicts(&d, a);
    defer UnresolvedConflictRow.deinitMany(conflicts, a);

    try testing.expectEqual(@as(usize, 1), conflicts.len);
    try testing.expectEqualStrings("external", conflicts[0].scope);
    try testing.expect(conflicts[0].detail != null);
    try testing.expectEqualStrings("field clash", conflicts[0].detail.?);
}

test "view_model: queryUnresolvedConflicts workbench in-place resolved drops from surface (task 4031)" {
    // Real workbench clear path: the workbench sync engine updates the
    // sync_events row in place to outcome='resolved-fs' or 'resolved-db'
    // (workbench/sync.zig ~line 202-205).  A row at resolved-fs is cleared;
    // a row still at 'conflict' remains.
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    // Two workbench conflict rows: one resolved in-place, one still open.
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, detail, at) values (null, 'workbench', 'pull', 'resolved-fs', 'resolved', '2025-02-01T08:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into sync_events (link_id, scope, direction, outcome, detail, at) values (null, 'workbench', 'push', 'conflict', 'open wb conflict', '2025-02-02T08:00:00.000Z')",
        &.{},
    );

    const conflicts = try queryUnresolvedConflicts(&d, a);
    defer UnresolvedConflictRow.deinitMany(conflicts, a);

    // Only the still-at-conflict row is unresolved.
    try testing.expectEqual(@as(usize, 1), conflicts.len);
    try testing.expectEqualStrings("workbench", conflicts[0].scope);
    try testing.expect(conflicts[0].link_id == null);
    try testing.expectEqualStrings("open wb conflict", conflicts[0].detail.?);
}

test "view_model: queryExtOpsSnapshot on empty DB has all slices empty" {
    const a = testing.allocator;
    var d = try setupTestDbExtOps(a);
    defer d.close();

    var snap = try queryExtOpsSnapshot(&d, a);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 0), snap.systems.len);
    try testing.expectEqual(@as(usize, 0), snap.links.len);
    try testing.expectEqual(@as(usize, 0), snap.system_sync.len);
    try testing.expectEqual(@as(usize, 0), snap.conflicts.len);
}
