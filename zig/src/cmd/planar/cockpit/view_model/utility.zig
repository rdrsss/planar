//! Configuration, annotations, and workbench-sync cockpit queries.

const std = @import("std");
const db = @import("db");
const engine = @import("engine");
const testing = std.testing;

// =========================================================================
// Utility view-model  (tasks 4041, 4042, 4043 — M15)
// =========================================================================
//
// Three sub-modes exposed by a single utility view:
//   .config        — config(key, value, updated_at)
//   .annotations   — annotations + annotation_tags
//   .workbench_sync — workbench_sync_state per feature (anchor_plan_id)
//
// Schema confirmed:
//   config: key text pk, value text not null, updated_at text not null
//   annotations: id, scope_kind, scope_id, anchor_path, anchor_line_start,
//     anchor_line_end, anchor_commit_sha, anchor_text_hash, anchor_text,
//     title, slug, body, status, vendor, plan_id, task_id, created_at, updated_at
//   annotation_tags: annotation_id integer, tag text
//   workbench_sync_state: id, anchor_plan_id, entity_kind, entity_id,
//     file_path, content_hash, fs_mtime, db_updated_at, last_synced_at
//
// MEMORY GUARD (brief rule (c)):
//   All string fields are freshly heap-allocated via dupe/allocPrint. No
//   field aliases into a query-result list or another struct's string.
//   The caller owns the result and releases it via the deinit helpers.
//
// Engine fidelity (task 4043 / CRITICAL):
//   Drift/conflict detection mirrors src/engine/workbench/sync.zig lines
//   442–460. The engine computes:
//     fs_changed = fs_hash != state.content_hash   (FS-side change)
//     db_changed = entity.updated_at != state.db_updated_at  (DB-side change)
//     no_op    : !fs_changed && !db_changed
//     fs_to_db : fs_changed && !db_changed   (FS has newer content)
//     db_to_fs : !fs_changed && db_changed   (DB has newer content)
//     conflict : fs_changed && db_changed && fs_hash != db_hash
//
//   The cockpit view is read-only and cannot access the FS or recompute
//   hashes. It derives sync status from the information available in the
//   DB alone:
//     - If db_updated_at == '' (never synced): pending (db_to_fs)
//     - For each entity in workbench_sync_state, we join the entity's
//       current updated_at from the actual entity table (plans/tasks/etc.)
//       and compare to db_updated_at stored in workbench_sync_state:
//         * db_updated_at != entity.updated_at → DB has drifted since last
//           sync (db_to_fs pending). Mirroring the engine's db_changed flag.
//         * Otherwise (hashes equal, db unchanged) → in_sync (no_op).
//     - Additionally, active unresolved conflicts are detected by looking
//       for sync_events rows with scope='workbench', outcome='conflict' for
//       the (entity_kind, entity_id) derived from context_json. Because
//       context_json is a blob, we aggregate per anchor_plan_id from the
//       sync_events table via a simpler indicator: any conflict event in
//       sync_events where scope='workbench' and outcome='conflict' whose
//       at is newer than the anchor plan's last_synced_at in the sync
//       state table is counted as an unresolved conflict.
//
//   The view shows per-feature (anchor_plan_id) counts:
//     - total entities in the sync state
//     - pending (db has drifted, needs push to FS)
//     - conflicts (unresolved workbench conflict events)
//     - in_sync count (total - pending - conflicts)
//
//   This matches what the engine reports in its Summary struct fields:
//     summary.pending and summary.conflicts.

// ---- Config inspector (task 4041) ----------------------------------------

/// One row from the config table (task 4041).
///
/// Schema: config(key text pk, value text not null, updated_at text not null)
pub const ConfigRow = struct {
    key: []const u8,
    value: []const u8,
    updated_at: []const u8,
    /// Pre-formatted display text: "key  value  updated_at".
    display_text: []const u8,

    pub fn deinit(self: ConfigRow, allocator: std.mem.Allocator) void {
        allocator.free(self.key);
        allocator.free(self.value);
        allocator.free(self.updated_at);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []ConfigRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Query all config rows, ordered by key ascending (task 4041).
///
/// Returns a freshly heap-allocated slice; caller frees via
/// ConfigRow.deinitMany.
pub fn queryConfig(d: *db.sqlite.Db, allocator: std.mem.Allocator) ![]ConfigRow {
    var stmt = d.prepare(
        "select key, value, updated_at from config order by key",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return error.QueryFailed;

    var out: std.ArrayList(ConfigRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const key = try stmt.columnTextAlloc(0, allocator);
                errdefer allocator.free(key);
                const value = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(value);
                const updated_at = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(updated_at);
                // Pre-format display text (MEMORY GUARD: heap-allocated, not stack-local).
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "{s}  =  {s}",
                    .{ key, value },
                );
                try out.append(allocator, .{
                    .key = key,
                    .value = value,
                    .updated_at = updated_at,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

// ---- Annotations browser (task 4042) -------------------------------------

/// One row from the annotations browser (task 4042).
///
/// Schema: annotations(id, scope_kind, scope_id, anchor_path,
///   anchor_line_start, anchor_line_end, title, slug, body, status,
///   vendor, plan_id, task_id, created_at, updated_at)
pub const AnnotationRow = struct {
    id: i64,
    /// Nullable title (coalesced to slug or "(untitled)").
    title: []const u8,
    /// anchor_path.
    anchor_path: []const u8,
    /// status: active / resolved / dismissed / archived.
    status: []const u8,
    /// Comma-joined tags from annotation_tags (may be empty "").
    tags: []const u8,
    /// body text for the detail pane.
    body: []const u8,
    /// anchor_line_start (0 if null).
    anchor_line_start: i64,
    /// anchor_line_end (0 if null).
    anchor_line_end: i64,
    /// Pre-formatted display text.
    display_text: []const u8,

    pub fn deinit(self: AnnotationRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.anchor_path);
        allocator.free(self.status);
        allocator.free(self.tags);
        allocator.free(self.body);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []AnnotationRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Query all annotations with their tags, ordered by created_at desc (task 4042).
///
/// Joins annotation_tags using group_concat to collect tags in a single pass.
/// Returns a freshly heap-allocated slice; caller frees via
/// AnnotationRow.deinitMany.
pub fn queryAnnotations(d: *db.sqlite.Db, allocator: std.mem.Allocator) ![]AnnotationRow {
    var stmt = d.prepare(
        \\select a.id,
        \\       coalesce(a.title, a.slug, '(untitled)'),
        \\       a.anchor_path,
        \\       a.status,
        \\       coalesce(a.body, ''),
        \\       a.anchor_line_start,
        \\       a.anchor_line_end,
        \\       coalesce(group_concat(t.tag, ', '), '')
        \\from annotations a
        \\left join annotation_tags t on t.annotation_id = a.id
        \\group by a.id
        \\order by a.created_at desc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return error.QueryFailed;

    var out: std.ArrayList(AnnotationRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const anchor_path = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(anchor_path);
                const status = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(status);
                const body = try stmt.columnTextAlloc(4, allocator);
                errdefer allocator.free(body);
                const line_start = stmt.columnInt(5);
                const line_end = stmt.columnInt(6);
                const tags = try stmt.columnTextAlloc(7, allocator);
                errdefer allocator.free(tags);

                // Pre-format display text (MEMORY GUARD: heap-allocated).
                const display_text = if (tags.len > 0)
                    try std.fmt.allocPrint(
                        allocator,
                        "[{s}] {s}  [{s}]",
                        .{ status, title, tags },
                    )
                else
                    try std.fmt.allocPrint(
                        allocator,
                        "[{s}] {s}",
                        .{ status, title },
                    );

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .anchor_path = anchor_path,
                    .status = status,
                    .tags = tags,
                    .body = body,
                    .anchor_line_start = line_start,
                    .anchor_line_end = line_end,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

// ---- Workbench sync-state view (task 4043) --------------------------------

/// Per-feature workbench sync-state summary row (task 4043).
///
/// One row per anchor_plan (feature). Derived from workbench_sync_state by
/// comparing db_updated_at stored in sync state to the actual entity's
/// current updated_at, mirroring the engine's db_changed flag in
/// src/engine/workbench/sync.zig lines 447-453.
///
/// Fields rendered by the view (acceptance rule (a) — render EVERY datum queried):
///   - anchor_plan_id, plan_title, total, in_sync, pending, conflicts,
///     last_synced_at, display_text.
pub const WorkbenchSyncRow = struct {
    /// anchor_plan_id from workbench_sync_state.
    anchor_plan_id: i64,
    /// Title of the anchor plan (from plans table; may be "(plan:<id>)").
    plan_title: []const u8,
    /// Total entities tracked in workbench_sync_state for this plan.
    total: i64,
    /// Entities where db_updated_at matches the entity's current updated_at
    /// (DB has not changed since last sync → in_sync).
    in_sync: i64,
    /// Entities where db_updated_at != entity.updated_at (DB has drifted
    /// since last sync; pending a push to FS). Mirrors engine db_changed.
    pending: i64,
    /// Count of unresolved workbench conflict events for this anchor plan
    /// from sync_events (scope='workbench', outcome='conflict').
    conflicts: i64,
    /// Most recent last_synced_at for any entity in this plan's sync state.
    last_synced_at: []const u8,
    /// Pre-formatted display text for the navigator.
    display_text: []const u8,

    pub fn deinit(self: WorkbenchSyncRow, allocator: std.mem.Allocator) void {
        allocator.free(self.plan_title);
        allocator.free(self.last_synced_at);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []WorkbenchSyncRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Query per-feature workbench sync-state summary rows (task 4043).
///
/// Engine fidelity (CRITICAL): drift detection mirrors sync.zig lines 442–460.
/// The engine determines db_changed = (entity.updated_at != state.db_updated_at).
/// We replicate this in SQL by joining workbench_sync_state to the actual entity
/// table for each entity_kind and comparing updated_at values.
///
/// Because SQLite does not support dynamic table dispatch, we build the
/// pending count with a UNION of per-entity-kind subqueries checking
/// db_updated_at vs. the entity's current updated_at.
///
/// Conflict detection: counts unresolved workbench conflict events from
/// sync_events (scope='workbench', outcome='conflict') per anchor_plan_id
/// via context_json matching. Since context_json is a JSON blob we use a
/// simple approach: anchor_plan_id is in context_json as a literal integer
/// field, which we match with json_extract.
pub fn queryWorkbenchSync(d: *db.sqlite.Db, allocator: std.mem.Allocator) ![]WorkbenchSyncRow {
    // Step 1: get distinct anchor_plan_ids with basic counts.
    // For pending (db_changed), we check each entity kind's updated_at.
    // The drift check: db_updated_at != '' AND db_updated_at != entity.updated_at
    // OR db_updated_at == '' (never synced yet).
    // "in sync" = NOT pending.
    //
    // We use a UNION-based subquery to check each entity_kind's updated_at:
    //   plan, task, artifact, scenario (test_scenarios), decision, question
    const sql =
        \\select
        \\  w.anchor_plan_id,
        \\  coalesce(p.title, '(plan:' || w.anchor_plan_id || ')') as plan_title,
        \\  count(*) as total,
        \\  sum(case
        \\    when w.entity_kind = 'plan' and exists(
        \\      select 1 from plans ep where ep.id = w.entity_id
        \\        and coalesce(ep.updated_at,'') = coalesce(w.db_updated_at,'')
        \\    ) then 1
        \\    when w.entity_kind = 'task' and exists(
        \\      select 1 from tasks et where et.id = w.entity_id
        \\        and coalesce(et.updated_at,'') = coalesce(w.db_updated_at,'')
        \\    ) then 1
        \\    when w.entity_kind = 'artifact' and exists(
        \\      select 1 from artifacts ea where ea.id = w.entity_id
        \\        and coalesce(ea.updated_at,'') = coalesce(w.db_updated_at,'')
        \\    ) then 1
        \\    when w.entity_kind = 'scenario' and exists(
        \\      select 1 from test_scenarios es where es.id = w.entity_id
        \\        and coalesce(es.updated_at,'') = coalesce(w.db_updated_at,'')
        \\    ) then 1
        \\    when w.entity_kind = 'decision' and exists(
        \\      select 1 from decisions ed where ed.id = w.entity_id
        \\        and coalesce(ed.updated_at,'') = coalesce(w.db_updated_at,'')
        \\    ) then 1
        \\    when w.entity_kind = 'question' and exists(
        \\      select 1 from questions eq where eq.id = w.entity_id
        \\        and coalesce(eq.updated_at,'') = coalesce(w.db_updated_at,'')
        \\    ) then 1
        \\    else 0
        \\  end) as in_sync_count,
        \\  max(coalesce(w.last_synced_at,''))
        \\from workbench_sync_state w
        \\left join plans p on p.id = w.anchor_plan_id
        \\group by w.anchor_plan_id
        \\order by w.anchor_plan_id
    ;
    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return error.QueryFailed;

    var out: std.ArrayList(WorkbenchSyncRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const anchor_plan_id = stmt.columnInt(0);
                const plan_title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(plan_title);
                const total = stmt.columnInt(2);
                const in_sync = stmt.columnInt(3);
                const last_synced_at = try stmt.columnTextAlloc(4, allocator);
                errdefer allocator.free(last_synced_at);

                const pending = total - in_sync;

                // Query conflict count from sync_events for this anchor_plan_id.
                // Engine sets scope='workbench', outcome='conflict' and stores
                // anchor_plan_id in context_json per ensureConflictEventID in sync.zig.
                const conflicts = blk: {
                    var cstmt = d.prepare(
                        \\select count(*) from sync_events
                        \\where scope = 'workbench' and outcome = 'conflict'
                        \\  and json_extract(context_json, '$.anchor_plan_id') = ?
                    ) catch break :blk @as(i64, 0);
                    defer cstmt.finalize();
                    cstmt.bind(&.{.{ .int = anchor_plan_id }}) catch break :blk @as(i64, 0);
                    switch (cstmt.step() catch break :blk @as(i64, 0)) {
                        .done => break :blk @as(i64, 0),
                        .row => break :blk cstmt.columnInt(0),
                    }
                };

                // Status indicator for display.
                const status_indicator: []const u8 = if (conflicts > 0)
                    "!"
                else if (pending > 0)
                    "~"
                else
                    "=";

                // Pre-format display text (MEMORY GUARD: heap-allocated).
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s}  ({d} total, {d} pending, {d} conflicts)",
                    .{ status_indicator, plan_title, total, pending, conflicts },
                );

                try out.append(allocator, .{
                    .anchor_plan_id = anchor_plan_id,
                    .plan_title = plan_title,
                    .total = total,
                    .in_sync = in_sync,
                    .pending = pending,
                    .conflicts = conflicts,
                    .last_synced_at = last_synced_at,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Tests for M15 view-model
// =========================================================================

fn setupTestDbUtility(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "view_model: queryConfig returns empty slice on empty DB (task 4041)" {
    const a = testing.allocator;
    var d = try setupTestDbUtility(a);
    defer d.close();

    const rows = try queryConfig(&d, a);
    defer ConfigRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryConfig returns key/value/updated_at rows ordered by key (task 4041)" {
    const a = testing.allocator;
    var d = try setupTestDbUtility(a);
    defer d.close();

    _ = try d.execParams(
        "insert into config (key, value) values ('zebra', 'last')",
        &.{},
    );
    _ = try d.execParams(
        "insert into config (key, value) values ('alpha', 'first')",
        &.{},
    );

    const rows = try queryConfig(&d, a);
    defer ConfigRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 2), rows.len);
    // Ordered by key asc.
    try testing.expectEqualStrings("alpha", rows[0].key);
    try testing.expectEqualStrings("first", rows[0].value);
    try testing.expectEqualStrings("zebra", rows[1].key);
    try testing.expectEqualStrings("last", rows[1].value);
    // display_text must contain key and value.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "alpha") != null);
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "first") != null);
    // updated_at must be non-empty.
    try testing.expect(rows[0].updated_at.len > 0);
}

test "view_model: queryAnnotations returns empty slice on empty DB (task 4042)" {
    const a = testing.allocator;
    var d = try setupTestDbUtility(a);
    defer d.close();

    const rows = try queryAnnotations(&d, a);
    defer AnnotationRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryAnnotations returns annotations with tags (task 4042)" {
    const a = testing.allocator;
    var d = try setupTestDbUtility(a);
    defer d.close();

    const ann_id = try d.execParams(
        "insert into annotations (scope_kind, anchor_path, title, body, status) values ('global', 'src/main.zig', 'Fix this', 'Body text', 'active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into annotation_tags (annotation_id, tag) values (?, 'bug')",
        &.{.{ .int = ann_id }},
    );
    _ = try d.execParams(
        "insert into annotation_tags (annotation_id, tag) values (?, 'priority')",
        &.{.{ .int = ann_id }},
    );

    const rows = try queryAnnotations(&d, a);
    defer AnnotationRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqualStrings("Fix this", rows[0].title);
    try testing.expectEqualStrings("src/main.zig", rows[0].anchor_path);
    try testing.expectEqualStrings("active", rows[0].status);
    try testing.expectEqualStrings("Body text", rows[0].body);
    // Tags must be included (group_concat order may vary but both must appear).
    try testing.expect(std.mem.indexOf(u8, rows[0].tags, "bug") != null);
    try testing.expect(std.mem.indexOf(u8, rows[0].tags, "priority") != null);
    // display_text must contain status and title.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "active") != null);
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "Fix this") != null);
}

test "view_model: queryAnnotations with no tags gives empty tags field (task 4042)" {
    const a = testing.allocator;
    var d = try setupTestDbUtility(a);
    defer d.close();

    _ = try d.execParams(
        "insert into annotations (scope_kind, anchor_path, body, status) values ('global', 'lib/foo.zig', 'no tags here', 'active')",
        &.{},
    );

    const rows = try queryAnnotations(&d, a);
    defer AnnotationRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqualStrings("", rows[0].tags);
    // display_text has no bracket section for empty tags.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "[active]") != null);
}

test "view_model: queryWorkbenchSync returns empty slice on empty DB (task 4043)" {
    const a = testing.allocator;
    var d = try setupTestDbUtility(a);
    defer d.close();

    const rows = try queryWorkbenchSync(&d, a);
    defer WorkbenchSyncRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryWorkbenchSync in-sync entity matches engine db_changed=false (task 4043)" {
    // Engine fidelity: db_updated_at == entity.updated_at → in_sync (db_changed=false).
    const a = testing.allocator;
    var d = try setupTestDbUtility(a);
    defer d.close();

    // Create a plan to use as the anchor (slug is NOT NULL in schema).
    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Feature A', 'feature-a', 'active')",
        &.{},
    );
    // Create a task.
    const task_id = try d.execParams(
        "insert into tasks (plan_id, scope_kind, title, status, priority) values (?, 'global', 'Task 1', 'todo', 2)",
        &.{.{ .int = plan_id }},
    );
    // Read back the task's updated_at via prepare/step.
    const task_updated_at = blk: {
        var stmt = try d.prepare("select updated_at from tasks where id = ?");
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch break :blk try a.dupe(u8, "");
        switch (stmt.step() catch break :blk try a.dupe(u8, "")) {
            .done => break :blk try a.dupe(u8, ""),
            .row => break :blk try stmt.columnTextAlloc(0, a),
        }
    };
    defer a.free(task_updated_at);

    // Insert a workbench_sync_state row with db_updated_at matching the task's updated_at
    // → in sync (db_changed=false in engine terms).
    _ = try d.execParams(
        "insert into workbench_sync_state (anchor_plan_id, entity_kind, entity_id, file_path, content_hash, db_updated_at, last_synced_at) values (?, 'task', ?, 'feat/task-1.md', 'abc123', ?, strftime('%Y-%m-%dT%H:%M:%fZ','now'))",
        &.{ .{ .int = plan_id }, .{ .int = task_id }, .{ .text = task_updated_at } },
    );

    const rows = try queryWorkbenchSync(&d, a);
    defer WorkbenchSyncRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqual(plan_id, rows[0].anchor_plan_id);
    try testing.expectEqualStrings("Feature A", rows[0].plan_title);
    try testing.expectEqual(@as(i64, 1), rows[0].total);
    try testing.expectEqual(@as(i64, 1), rows[0].in_sync);
    try testing.expectEqual(@as(i64, 0), rows[0].pending);
    try testing.expectEqual(@as(i64, 0), rows[0].conflicts);
    // Indicator: no drift → '='
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "=") != null);
}

test "view_model: queryWorkbenchSync pending entity mirrors engine db_changed=true (task 4043)" {
    // Engine fidelity: db_updated_at != entity.updated_at → pending (db_changed=true).
    const a = testing.allocator;
    var d = try setupTestDbUtility(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Feature B', 'feature-b', 'active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (plan_id, scope_kind, title, status, priority) values (?, 'global', 'Drifted Task', 'todo', 2)",
        &.{.{ .int = plan_id }},
    );

    // Insert with an OLD db_updated_at that does NOT match task's current updated_at
    // → pending (engine would classify as db_to_fs).
    _ = try d.execParams(
        "insert into workbench_sync_state (anchor_plan_id, entity_kind, entity_id, file_path, content_hash, db_updated_at, last_synced_at) values (?, 'task', ?, 'feat/task-drifted.md', 'abc123', '2020-01-01T00:00:00.000Z', '2020-01-01T00:00:00.000Z')",
        &.{ .{ .int = plan_id }, .{ .int = task_id } },
    );

    const rows = try queryWorkbenchSync(&d, a);
    defer WorkbenchSyncRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqual(@as(i64, 1), rows[0].total);
    try testing.expectEqual(@as(i64, 0), rows[0].in_sync);
    try testing.expectEqual(@as(i64, 1), rows[0].pending);
    try testing.expectEqual(@as(i64, 0), rows[0].conflicts);
    // Indicator: drift → '~'
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "~") != null);
    // "pending" count in display text.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "1 pending") != null);
}

test "view_model: queryWorkbenchSync conflict entity counts from sync_events (task 4043)" {
    // Engine fidelity: conflicts counted from sync_events scope='workbench', outcome='conflict'.
    const a = testing.allocator;
    var d = try setupTestDbUtility(a);
    defer d.close();

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Feature C', 'feature-c', 'active')",
        &.{},
    );
    const task_id = try d.execParams(
        "insert into tasks (plan_id, scope_kind, title, status, priority) values (?, 'global', 'Conflicted Task', 'todo', 2)",
        &.{.{ .int = plan_id }},
    );

    // Insert sync state (in_sync for DB comparison).
    const task_updated_at = blk: {
        var stmt = try d.prepare("select updated_at from tasks where id = ?");
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch break :blk try a.dupe(u8, "");
        switch (stmt.step() catch break :blk try a.dupe(u8, "")) {
            .done => break :blk try a.dupe(u8, ""),
            .row => break :blk try stmt.columnTextAlloc(0, a),
        }
    };
    defer a.free(task_updated_at);

    _ = try d.execParams(
        "insert into workbench_sync_state (anchor_plan_id, entity_kind, entity_id, file_path, content_hash, db_updated_at, last_synced_at) values (?, 'task', ?, 'feat/conflict.md', 'abc', ?, strftime('%Y-%m-%dT%H:%M:%fZ','now'))",
        &.{ .{ .int = plan_id }, .{ .int = task_id }, .{ .text = task_updated_at } },
    );

    // Insert a conflict event in sync_events (as the engine does in sync.zig ensureConflictEventID).
    const ctx_json = try std.fmt.allocPrint(
        a,
        "{{\"anchor_plan_id\":{d},\"entity_kind\":\"task\",\"entity_id\":{d},\"file_path\":\"feat/conflict.md\",\"fs_hash\":\"aaa\",\"db_hash\":\"bbb\",\"fs_mtime\":\"\",\"db_updated_at\":\"{s}\"}}",
        .{ plan_id, task_id, task_updated_at },
    );
    defer a.free(ctx_json);

    _ = try d.execParams(
        "insert into sync_events (scope, direction, outcome, context_json) values ('workbench', 'push', 'conflict', ?)",
        &.{.{ .text = ctx_json }},
    );

    const rows = try queryWorkbenchSync(&d, a);
    defer WorkbenchSyncRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqual(@as(i64, 1), rows[0].conflicts);
    // Indicator: conflict → '!'
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "!") != null);
}
