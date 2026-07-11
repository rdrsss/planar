//! Decision-log and derives-from relationship cockpit queries.

const std = @import("std");
const db = @import("db");
const common = @import("common.zig");
const StatusBadge = common.StatusBadge;
const ScopeFilter = common.ScopeFilter;
const decisionStatusBadge = @import("scope_explorer.zig").decisionStatusBadge;
const resolveEntityTitle = @import("entity_title.zig").resolve;

// Decision Log view-model  (tasks 4021, 4022)
// =========================================================================

/// One row in the Decision Log navigator list. Ordered chronologically
/// (newest-first) so the most recent decision is at the top.
///
/// Schema reference: migrations/00002_planning.up.sql
///   decisions(id, title, body, status, decided_at, created_at, ...)
///   status check: ('proposed','accepted','superseded','withdrawn')
pub const DecisionLogRow = struct {
    id: i64,
    /// Decision title.
    title: []const u8,
    /// Status badge derived from the decisions.status column.
    badge: StatusBadge,
    /// ISO-8601 decided_at (nullable) or created_at as fallback for display.
    date_display: []const u8,
    /// Pre-formatted navigator display string: "[badge] YYYY-MM-DD  title".
    /// Heap-allocated so that grapheme pointers from printSegment remain valid
    /// after the render function returns (required for render-level tests).
    display_text: []const u8,

    pub fn deinit(self: DecisionLogRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.date_display);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []DecisionLogRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One derives-from target for a decision (from entity_links).
///
/// entity_links relationship = 'derives-from' means the decision was derived
/// from an artifact, plan, or spec. Per migration 00004_entity_links.up.sql
/// the valid relationships include 'derives-from'.
///
/// This struct surfaces both directions:
///   decision → artifact/plan (from_kind='decision', to_kind=<target>)
///   artifact/plan → decision is intentionally excluded (derives-from is
///   a directional edge — the decision "derives from" something).
pub const DecisionDerivesFromRow = struct {
    /// Target entity kind string, e.g. "artifact", "plan".
    target_kind: []const u8,
    /// Target entity id.
    target_id: i64,
    /// Human-readable label: "<kind>:<id> — <title>" or "<kind>:<id>".
    label: []const u8,

    pub fn deinit(self: DecisionDerivesFromRow, allocator: std.mem.Allocator) void {
        allocator.free(self.target_kind);
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []DecisionDerivesFromRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Detail pane data for a selected decision in the Decision Log.
///
/// Combines the decision's markdown body (task 4022) with the list of
/// derives-from targets sourced from entity_links (task 4022).
///
/// All fields are caller-owned; free via `DecisionLogDetail.deinit`.
pub const DecisionLogDetail = struct {
    /// Decision id.
    id: i64,
    /// Decision title.
    title: []const u8,
    /// Rendered markdown body: "**Status:** {s}\n\n{body_raw}".
    /// Always non-empty (decisions.body is NOT NULL in the schema).
    body: []const u8,
    /// derives-from targets from entity_links. May be empty.
    derives_from: []DecisionDerivesFromRow,

    pub fn deinit(self: DecisionLogDetail, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.body);
        DecisionDerivesFromRow.deinitMany(self.derives_from, allocator);
    }
};

/// Query all decisions, ordered newest-first (decided_at desc, created_at desc, id desc).
///
/// Uses decided_at as the primary sort key, falling back to created_at when
/// decided_at is NULL (proposed decisions have no decided_at).
///
/// Scope filter: when `filter` is `.repo`, includes only decisions whose
/// scope_kind='repo' and scope_id matches the project id, plus global decisions.
/// When `.all`, returns all decisions.
///
/// Caller owns the result; free via `DecisionLogRow.deinitMany`.
pub fn queryDecisionLog(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: ScopeFilter,
) ![]DecisionLogRow {
    const sql_all =
        \\select id, title, status,
        \\       coalesce(decided_at, created_at) as date_display
        \\from decisions
        \\order by coalesce(decided_at, created_at) desc, id desc
    ;
    const sql_repo =
        \\select id, title, status,
        \\       coalesce(decided_at, created_at) as date_display
        \\from decisions
        \\where (scope_kind = 'repo' and scope_id = ?) or scope_kind = 'global'
        \\order by coalesce(decided_at, created_at) desc, id desc
    ;

    var stmt = switch (filter) {
        .all => blk: {
            var s = d.prepare(sql_all) catch return error.QueryFailed;
            s.bind(&.{}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
        .repo => blk: {
            var s = d.prepare(sql_repo) catch return error.QueryFailed;
            s.bind(&.{.{ .int = filter.repo }}) catch {
                s.finalize();
                return error.QueryFailed;
            };
            break :blk s;
        },
    };
    defer stmt.finalize();

    var out: std.ArrayList(DecisionLogRow) = .empty;
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
                const status_text = try stmt.columnTextAlloc(2, allocator);
                defer allocator.free(status_text);
                const date_str = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(date_str);

                const badge = decisionStatusBadge(status_text);

                // Pre-format the navigator display string so that grapheme
                // pointers from printSegment remain valid after render returns.
                const date_slice = if (date_str.len >= 10) date_str[0..10] else date_str;
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s}  {s}",
                    .{ badge.glyph(), date_slice, title },
                );
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .badge = badge,
                    .date_display = date_str,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query derives-from targets for a decision from entity_links.
///
/// Finds entity_links rows where:
///   from_kind='decision', from_id=decision_id, relationship='derives-from'
///
/// Per migration 00004_entity_links.up.sql:
///   relationship check: ('derives-from','blocks','addresses','verifies','cites','supersedes','touches')
///
/// Resolves the target title by joining to the appropriate table.
/// Unresolvable targets (unknown kind) are surfaced with label "<kind>:<id>".
///
/// Caller owns the result; free via `DecisionDerivesFromRow.deinitMany`.
pub fn queryDecisionDerivesFrom(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    decision_id: i64,
) ![]DecisionDerivesFromRow {
    // Query the entity_links rows for this decision's derives-from edges.
    var stmt = d.prepare(
        \\select to_kind, to_id
        \\from entity_links
        \\where from_kind = 'decision'
        \\  and from_id = ?
        \\  and relationship = 'derives-from'
        \\order by to_kind asc, to_id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = decision_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(DecisionDerivesFromRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const to_kind = try stmt.columnTextAlloc(0, allocator);
                errdefer allocator.free(to_kind);
                const to_id = stmt.columnInt(1);

                // Resolve the title for this target entity.
                const title_opt = resolveEntityTitle(d, allocator, to_kind, to_id);
                // Build the label. On OOM, propagate the error rather than
                // aliasing to_kind into label — aliasing causes a double-free
                // in deinit (target_kind and label both freed separately).
                const label = if (title_opt) |t|
                    std.fmt.allocPrint(allocator, "{s}:{d} — {s}", .{ to_kind, to_id, t }) catch blk: {
                        allocator.free(t);
                        break :blk std.fmt.allocPrint(allocator, "{s}:{d}", .{ to_kind, to_id }) catch return error.OutOfMemory;
                    }
                else
                    std.fmt.allocPrint(allocator, "{s}:{d}", .{ to_kind, to_id }) catch return error.OutOfMemory;
                errdefer allocator.free(label);

                // Free the resolved title if it was successfully embedded in label.
                if (title_opt) |t| allocator.free(t);

                try out.append(allocator, .{
                    .target_kind = to_kind,
                    .target_id = to_id,
                    .label = label,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Attempt to resolve an entity title for a given kind+id. Returns a heap-
/// allocated string or null when the kind is not recognized or the row is not
/// found. Caller owns the returned string when non-null.
/// Query full Decision Log detail for a selected decision.
///
/// Combines the decision's body (markdown) with the derives-from
/// targets from entity_links (task 4022).
///
/// Returns null when the decision does not exist.
/// Caller owns the result; free via `DecisionLogDetail.deinit`.
pub fn queryDecisionLogDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    decision_id: i64,
) !?DecisionLogDetail {
    var stmt = d.prepare(
        "select title, body, status from decisions where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = decision_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return null,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            // decisions.body is NOT NULL per schema.
            const body_raw = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(body_raw);
            const status_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status_text);

            // Build the rendered body: prepend the status badge line.
            const body = try std.fmt.allocPrint(
                allocator,
                "**Status:** {s}\n\n{s}",
                .{ status_text, body_raw },
            );
            errdefer allocator.free(body);

            const derives_from = try queryDecisionDerivesFrom(d, allocator, decision_id);
            errdefer DecisionDerivesFromRow.deinitMany(derives_from, allocator);

            return .{
                .id = decision_id,
                .title = title,
                .body = body,
                .derives_from = derives_from,
            };
        },
    }
}

// =========================================================================
// View registry
// =========================================================================

// =========================================================================
