//! CLI invocation-history cockpit queries and filters.

const std = @import("std");
const db = @import("db");
const testing = std.testing;

// =========================================================================
// CLI Invocation History view-model (tasks 4037, 4038)
// =========================================================================
//
// Schema from migrations/00020_cli_invocations.up.sql:
//   cli_invocations(id, verb_path, args_shape, exit_code, error_category,
//                   scope_slug, duration_ms, recorded_at)
//
// MEMORY GUARD (brief rule (c)):
//   - All string fields in CliInvocationRow are freshly heap-allocated
//     via dupe/allocPrint. No field aliases into a query-result list.
//   - CliHistoryFilter holds owned copies of verb and scope strings
//     (dupe on set, free on replace/reset). Never borrows from rows[].

/// One row in the CLI History view (maps one cli_invocations row).
pub const CliInvocationRow = struct {
    id: i64,
    /// Command path, e.g. "task add", "workbench push". Never null (NOT NULL in schema).
    verb_path: []const u8,
    /// Args shape summary, e.g. "<pos:1> --plan --json". Never null (NOT NULL DEFAULT '').
    args_shape: []const u8,
    /// 0 on success, non-zero on failure.
    exit_code: i64,
    /// Null when exit_code=0. One of: usage|scope|not_found|conflict|
    /// validation|io|db|internal.
    error_category: ?[]const u8,
    /// Resolved scope slug at the time of the invocation, or null.
    scope_slug: ?[]const u8,
    /// Duration in milliseconds raw value, or null (kept for tests).
    duration_ms: ?i64,
    /// Pre-formatted duration string for the detail pane, e.g. "123ms" or null.
    /// Heap-allocated. MEMORY GUARD (rule (c)): the renderer uses this instead of
    /// a stack-local format buffer whose grapheme pointer would dangle after
    /// renderDetail returns (printSegment stores the pointer into the cell, not a copy).
    duration_display: ?[]const u8,
    /// ISO-8601 timestamp when this invocation was recorded.
    recorded_at: []const u8,
    /// Pre-formatted navigator display text. Format:
    ///   "[OK]  verb_path  args_shape  ts_short"   (exit_code=0)
    ///   "[ERR] verb_path  args_shape  ts_short"   (exit_code≠0)
    /// Heap-allocated so grapheme pointers remain valid for the render lifetime.
    display_text: []const u8,

    pub fn deinit(self: CliInvocationRow, allocator: std.mem.Allocator) void {
        allocator.free(self.verb_path);
        allocator.free(self.args_shape);
        if (self.error_category) |s| allocator.free(s);
        if (self.scope_slug) |s| allocator.free(s);
        if (self.duration_display) |s| allocator.free(s);
        allocator.free(self.recorded_at);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []CliInvocationRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Time-bucket filter for the CLI History view.
///
///   .all    — no time restriction; returns all rows.
///   .hour   — recorded_at >= now - 1 hour.
///   .day    — recorded_at >= now - 24 hours.
pub const CliTimeFilter = enum {
    all,
    hour,
    day,

    /// Return a human-readable label for the active time bucket.
    pub fn label(self: CliTimeFilter) []const u8 {
        return switch (self) {
            .all => "all time",
            .hour => "last hour",
            .day => "last 24h",
        };
    }

    /// Cycle to the next value: all → hour → day → all → …
    pub fn next(self: CliTimeFilter) CliTimeFilter {
        return switch (self) {
            .all => .hour,
            .hour => .day,
            .day => .all,
        };
    }
};

/// Composite filter for the CLI History view.
///
/// OWNERSHIP CONTRACT (memory guard rule (c)):
///   - `verb`  — when non-null, must be an independently heap-allocated
///               duplicate. Never alias into rows[].verb_path.
///   - `scope` — when non-null, must be an independently heap-allocated
///               duplicate. Never alias into rows[].scope_slug.
///   Both are managed by the view state (CliHistoryState) via its
///   setFilterVerb / setFilterScope / resetVerbFilter / resetScopeFilter
///   helpers. Query code treats them as read-only borrowed slices.
pub const CliHistoryFilter = struct {
    /// When non-null, restrict to rows where verb_path = verb.
    /// Heap-allocated (owned by the view state, see ownership contract above).
    verb: ?[]const u8 = null,
    /// When non-null, restrict to rows where scope_slug = scope.
    /// Heap-allocated (owned by the view state, see ownership contract above).
    scope: ?[]const u8 = null,
    /// Time-bucket restriction.
    time: CliTimeFilter = .all,
};

/// Query cli_invocations rows, newest-first, applying the given filter.
///
/// Returns a heap-allocated slice; caller owns and must release via
/// `CliInvocationRow.deinitMany`.
///
/// Column order: id(0), verb_path(1), args_shape(2), exit_code(3),
///               error_category(4), scope_slug(5), duration_ms(6),
///               recorded_at(7).
///
/// MEMORY GUARD (rule (c)): every string field is freshly duped or
/// allocPrint-ed. No field aliases into other fields or into the filter.
pub fn queryCliHistory(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: CliHistoryFilter,
) ![]CliInvocationRow {
    var rows: std.ArrayList(CliInvocationRow) = .empty;
    errdefer {
        for (rows.items) |r| r.deinit(allocator);
        rows.deinit(allocator);
    }

    // Build the WHERE clause from the filter. We compose the SQL string
    // dynamically using an ArrayList(u8) writer, then allocate a
    // sentinel-terminated copy (required by db.sqlite.Db.prepare).
    var sql_builder: std.ArrayListUnmanaged(u8) = .empty;
    defer sql_builder.deinit(allocator);

    try sql_builder.appendSlice(allocator,
        \\select id, coalesce(verb_path,''), coalesce(args_shape,''), exit_code,
        \\       error_category, scope_slug, duration_ms, coalesce(recorded_at,'')
        \\from cli_invocations
        \\where 1=1
    );

    if (filter.verb != null) {
        try sql_builder.appendSlice(allocator, "\nand verb_path = ?");
    }
    if (filter.scope != null) {
        try sql_builder.appendSlice(allocator, "\nand scope_slug = ?");
    }
    switch (filter.time) {
        .all => {},
        .hour => {
            try sql_builder.appendSlice(
                allocator,
                "\nand recorded_at >= datetime('now', '-1 hour')",
            );
        },
        .day => {
            try sql_builder.appendSlice(
                allocator,
                "\nand recorded_at >= datetime('now', '-24 hours')",
            );
        },
    }
    try sql_builder.appendSlice(allocator, "\norder by recorded_at desc, id desc");

    // prepare() requires a sentinel-terminated string.
    const sql_z = try allocator.dupeZ(u8, sql_builder.items);
    defer allocator.free(sql_z);

    var stmt = try d.prepare(sql_z);
    defer stmt.finalize();

    // Bind verb and scope parameters positionally.
    var bind_args: [2]db.sqlite.Param = undefined;
    var bind_count: usize = 0;
    if (filter.verb) |v| {
        bind_args[bind_count] = .{ .text = v };
        bind_count += 1;
    }
    if (filter.scope) |s| {
        bind_args[bind_count] = .{ .text = s };
        bind_count += 1;
    }
    try stmt.bind(bind_args[0..bind_count]);

    while (true) {
        switch (try stmt.step()) {
            .done => break,
            .row => {
                const row = try readCliInvocationRow(&stmt, allocator);
                errdefer row.deinit(allocator);
                try rows.append(allocator, row);
            },
        }
    }

    return rows.toOwnedSlice(allocator);
}

/// Read one CliInvocationRow from the current statement row.
/// All string fields are freshly heap-allocated; no aliasing.
fn readCliInvocationRow(
    stmt: *db.sqlite.Stmt,
    allocator: std.mem.Allocator,
) !CliInvocationRow {
    const id = stmt.columnInt(0);

    const verb_path = try stmt.columnTextAlloc(1, allocator);
    errdefer allocator.free(verb_path);

    const args_shape = try stmt.columnTextAlloc(2, allocator);
    errdefer allocator.free(args_shape);

    const exit_code = stmt.columnInt(3);

    const error_category: ?[]const u8 = if (stmt.columnIsNull(4))
        null
    else
        try stmt.columnTextAlloc(4, allocator);
    errdefer if (error_category) |s| allocator.free(s);

    const scope_slug: ?[]const u8 = if (stmt.columnIsNull(5))
        null
    else
        try stmt.columnTextAlloc(5, allocator);
    errdefer if (scope_slug) |s| allocator.free(s);

    const duration_ms: ?i64 = if (stmt.columnIsNull(6))
        null
    else
        stmt.columnInt(6);

    // Pre-format duration_display as a heap-allocated string (MEMORY GUARD rule (c)):
    // renderDetail uses this instead of a stack-local format buffer because
    // printSegment stores a grapheme pointer into the text slice — a stack buffer
    // would dangle after renderDetail returns.
    const duration_display: ?[]const u8 = if (duration_ms) |ms|
        try std.fmt.allocPrint(allocator, "{d}ms", .{ms})
    else
        null;
    errdefer if (duration_display) |s| allocator.free(s);

    const recorded_at = try stmt.columnTextAlloc(7, allocator);
    errdefer allocator.free(recorded_at);

    // Pre-format the navigator display_text (heap-allocated).
    // Format: "[OK]  verb_path  args_shape  ts_short"
    //      or "[ERR] verb_path  args_shape  ts_short"
    // ts_short = first 19 chars of recorded_at (YYYY-MM-DDTHH:MM:SS).
    const ts_short = if (recorded_at.len >= 19) recorded_at[0..19] else recorded_at;
    const outcome_tag: []const u8 = if (exit_code == 0) "[OK] " else "[ERR]";
    // args_shape may be empty — only add a space separator when non-empty.
    const display_text = if (args_shape.len > 0)
        try std.fmt.allocPrint(
            allocator,
            "{s} {s}  {s}  {s}",
            .{ outcome_tag, verb_path, args_shape, ts_short },
        )
    else
        try std.fmt.allocPrint(
            allocator,
            "{s} {s}  {s}",
            .{ outcome_tag, verb_path, ts_short },
        );
    errdefer allocator.free(display_text);

    return .{
        .id = id,
        .verb_path = verb_path,
        .args_shape = args_shape,
        .exit_code = exit_code,
        .error_category = error_category,
        .scope_slug = scope_slug,
        .duration_ms = duration_ms,
        .duration_display = duration_display,
        .recorded_at = recorded_at,
        .display_text = display_text,
    };
}

/// Query all distinct verb_path values present in cli_invocations,
/// sorted alphabetically. Used by verb-filter cycling.
/// Returns a heap-allocated slice of owned strings; caller must free
/// via the provided helper or manually.
pub fn queryCliVerbs(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![][]const u8 {
    var verbs: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (verbs.items) |v| allocator.free(v);
        verbs.deinit(allocator);
    }

    var stmt = try d.prepare(
        \\select distinct verb_path from cli_invocations order by verb_path asc
    );
    defer stmt.finalize();
    try stmt.bind(&.{});
    while (true) {
        const step = try stmt.step();
        if (step == .done) break;
        const v = try stmt.columnTextAlloc(0, allocator);
        errdefer allocator.free(v);
        try verbs.append(allocator, v);
    }

    return verbs.toOwnedSlice(allocator);
}

/// Query all distinct scope_slug values present in cli_invocations,
/// sorted alphabetically. Used by scope-filter cycling.
/// Returns a heap-allocated slice of owned strings; caller must free
/// via the provided helper or manually.
pub fn queryCliScopes(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![][]const u8 {
    var scopes: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (scopes.items) |s| allocator.free(s);
        scopes.deinit(allocator);
    }

    var stmt = try d.prepare(
        \\select distinct scope_slug from cli_invocations
        \\where scope_slug is not null
        \\order by scope_slug asc
    );
    defer stmt.finalize();
    try stmt.bind(&.{});
    while (true) {
        const step = try stmt.step();
        if (step == .done) break;
        const s = try stmt.columnTextAlloc(0, allocator);
        errdefer allocator.free(s);
        try scopes.append(allocator, s);
    }

    return scopes.toOwnedSlice(allocator);
}

// =========================================================================
// CLI History view-model tests (tasks 4037, 4038)
// =========================================================================

fn setupTestDbCli(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "view_model: queryCliHistory on empty DB returns empty slice (task 4037 empty)" {
    const a = testing.allocator;
    var d = try setupTestDbCli(a);
    defer d.close();

    const rows = try queryCliHistory(&d, a, .{});
    defer CliInvocationRow.deinitMany(rows, a);
    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: queryCliHistory returns rows newest-first (task 4037)" {
    const a = testing.allocator;
    var d = try setupTestDbCli(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '<pos:1> --plan', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('workbench push', '', 0, '2025-06-01T00:00:00.000Z')",
        &.{},
    );

    const rows = try queryCliHistory(&d, a, .{});
    defer CliInvocationRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 2), rows.len);
    // Newest-first: workbench push before task add.
    try testing.expectEqualStrings("workbench push", rows[0].verb_path);
    try testing.expectEqualStrings("task add", rows[1].verb_path);
}

test "view_model: queryCliHistory row fields populated (task 4037)" {
    const a = testing.allocator;
    var d = try setupTestDbCli(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, scope_slug, duration_ms, recorded_at) values ('task list', '--plan --json', 0, null, 'myrepo', 42, '2026-01-15T10:30:00.000Z')",
        &.{},
    );

    const rows = try queryCliHistory(&d, a, .{});
    defer CliInvocationRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    const r = rows[0];
    try testing.expectEqualStrings("task list", r.verb_path);
    try testing.expectEqualStrings("--plan --json", r.args_shape);
    try testing.expectEqual(@as(i64, 0), r.exit_code);
    try testing.expect(r.error_category == null);
    try testing.expectEqualStrings("myrepo", r.scope_slug.?);
    try testing.expectEqual(@as(?i64, 42), r.duration_ms);
    try testing.expectEqualStrings("2026-01-15T10:30:00.000Z", r.recorded_at);
    // display_text must contain verb_path and outcome tag.
    try testing.expect(std.mem.indexOf(u8, r.display_text, "task list") != null);
    try testing.expect(std.mem.indexOf(u8, r.display_text, "[OK]") != null);
}

test "view_model: queryCliHistory failure row has error_category (task 4037)" {
    const a = testing.allocator;
    var d = try setupTestDbCli(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at) values ('task show', '<pos:1>', 1, 'not_found', '2026-02-01T08:00:00.000Z')",
        &.{},
    );

    const rows = try queryCliHistory(&d, a, .{});
    defer CliInvocationRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    const r = rows[0];
    try testing.expectEqual(@as(i64, 1), r.exit_code);
    try testing.expectEqualStrings("not_found", r.error_category.?);
    // display_text must show [ERR] for non-zero exit_code.
    try testing.expect(std.mem.indexOf(u8, r.display_text, "[ERR]") != null);
}

test "view_model: queryCliHistory verb filter narrows rows (task 4038)" {
    const a = testing.allocator;
    var d = try setupTestDbCli(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '<pos:1>', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '--plan', 0, '2025-02-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('plan show', '<pos:1>', 0, '2025-03-01T00:00:00.000Z')",
        &.{},
    );

    // Filter to "task add" only.
    const filtered = try queryCliHistory(&d, a, .{ .verb = "task add" });
    defer CliInvocationRow.deinitMany(filtered, a);

    try testing.expectEqual(@as(usize, 2), filtered.len);
    for (filtered) |r| {
        try testing.expectEqualStrings("task add", r.verb_path);
    }

    // All rows.
    const all_rows = try queryCliHistory(&d, a, .{});
    defer CliInvocationRow.deinitMany(all_rows, a);
    try testing.expectEqual(@as(usize, 3), all_rows.len);
}

test "view_model: queryCliHistory scope filter narrows rows (task 4038)" {
    const a = testing.allocator;
    var d = try setupTestDbCli(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task list', '', 0, 'repo-a', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task list', '', 0, 'repo-b', '2025-02-01T00:00:00.000Z')",
        &.{},
    );

    const filtered = try queryCliHistory(&d, a, .{ .scope = "repo-a" });
    defer CliInvocationRow.deinitMany(filtered, a);

    try testing.expectEqual(@as(usize, 1), filtered.len);
    try testing.expectEqualStrings("repo-a", filtered[0].scope_slug.?);
}

test "view_model: queryCliVerbs returns distinct verbs sorted (task 4038)" {
    const a = testing.allocator;
    var d = try setupTestDbCli(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '', 0, '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('plan show', '', 0, '2025-02-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('task add', '--plan', 0, '2025-03-01T00:00:00.000Z')",
        &.{},
    );

    const verbs = try queryCliVerbs(&d, a);
    defer {
        for (verbs) |v| a.free(v);
        a.free(verbs);
    }

    try testing.expectEqual(@as(usize, 2), verbs.len);
    // Sorted alphabetically: "plan show" < "task add".
    try testing.expectEqualStrings("plan show", verbs[0]);
    try testing.expectEqualStrings("task add", verbs[1]);
}

test "view_model: queryCliScopes returns distinct non-null scopes sorted (task 4038)" {
    const a = testing.allocator;
    var d = try setupTestDbCli(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task add', '', 0, 'alpha', '2025-01-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task add', '', 0, null, '2025-02-01T00:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, scope_slug, recorded_at) values ('task add', '', 0, 'beta', '2025-03-01T00:00:00.000Z')",
        &.{},
    );

    const scopes = try queryCliScopes(&d, a);
    defer {
        for (scopes) |s| a.free(s);
        a.free(scopes);
    }

    // Only non-null scopes, sorted.
    try testing.expectEqual(@as(usize, 2), scopes.len);
    try testing.expectEqualStrings("alpha", scopes[0]);
    try testing.expectEqualStrings("beta", scopes[1]);
}

test "view_model: CliTimeFilter label and next cycle" {
    try testing.expectEqualStrings("all time", CliTimeFilter.all.label());
    try testing.expectEqualStrings("last hour", CliTimeFilter.hour.label());
    try testing.expectEqualStrings("last 24h", CliTimeFilter.day.label());

    try testing.expectEqual(CliTimeFilter.hour, CliTimeFilter.all.next());
    try testing.expectEqual(CliTimeFilter.day, CliTimeFilter.hour.next());
    try testing.expectEqual(CliTimeFilter.all, CliTimeFilter.day.next());
}

test "view_model: CliInvocationRow deinit handles null optional fields" {
    const a = testing.allocator;
    var d = try setupTestDbCli(a);
    defer d.close();

    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at) values ('init', '', 0, '2026-01-01T00:00:00.000Z')",
        &.{},
    );

    const rows = try queryCliHistory(&d, a, .{});
    defer CliInvocationRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expect(rows[0].error_category == null);
    try testing.expect(rows[0].scope_slug == null);
    try testing.expect(rows[0].duration_ms == null);
}
