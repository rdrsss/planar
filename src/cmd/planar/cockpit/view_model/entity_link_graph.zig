//! Bidirectional entity-link graph cockpit queries.

const std = @import("std");
const db = @import("db");
const resolveEntityTitle = @import("entity_title.zig").resolve;

// Entity-Link Graph view-model  (tasks 4027, 4028)
// =========================================================================

/// The relationship kind carried by an entity_links row.
///
/// These are the valid values from the CHECK constraint in
/// migrations/00004_entity_links.up.sql:
///   ('derives-from','blocks','addresses','verifies','cites','supersedes','touches')
pub const LinkRelationship = enum {
    derives_from,
    blocks,
    addresses,
    verifies,
    cites,
    supersedes,
    touches,

    pub fn fromText(s: []const u8) ?LinkRelationship {
        if (std.mem.eql(u8, s, "derives-from")) return .derives_from;
        if (std.mem.eql(u8, s, "blocks")) return .blocks;
        if (std.mem.eql(u8, s, "addresses")) return .addresses;
        if (std.mem.eql(u8, s, "verifies")) return .verifies;
        if (std.mem.eql(u8, s, "cites")) return .cites;
        if (std.mem.eql(u8, s, "supersedes")) return .supersedes;
        if (std.mem.eql(u8, s, "touches")) return .touches;
        return null;
    }

    pub fn toText(self: LinkRelationship) []const u8 {
        return switch (self) {
            .derives_from => "derives-from",
            .blocks => "blocks",
            .addresses => "addresses",
            .verifies => "verifies",
            .cites => "cites",
            .supersedes => "supersedes",
            .touches => "touches",
        };
    }
};

/// Direction of a link relative to the focus entity.
pub const LinkEdgeDirection = enum {
    /// Focus entity is the source (from_kind/from_id = focus); the link
    /// points outward to another entity.
    outbound,
    /// Focus entity is the target (to_kind/to_id = focus); another entity
    /// points inward to the focus.
    inbound,
};

/// One related-entity row for the Entity-Link Graph view.
///
/// Each row represents one entity_links edge (task 4027). The navigator
/// groups rows by relationship kind + direction.
///
/// All string fields are heap-owned by the EntityLinkGraphData that
/// contains this row; freed by EntityLinkGraphData.deinit.
pub const EntityLinkRow = struct {
    /// The entity_links.id for this edge.
    link_id: i64,
    /// Relationship kind (e.g. .derives_from, .blocks).
    relationship: LinkRelationship,
    /// Direction relative to the focus entity.
    direction: LinkEdgeDirection,
    /// Kind string of the *other* (non-focus) entity, e.g. "task".
    other_kind: []const u8,
    /// ID of the other entity.
    other_id: i64,
    /// Pre-formatted readable label:
    ///   "[direction] relationship  other_kind:other_id — title"
    /// Heap-owned; stays valid for the duration of the EntityLinkGraphData
    /// that owns it (required for render-level test stability).
    display_text: []const u8,

    pub fn deinit(self: EntityLinkRow, allocator: std.mem.Allocator) void {
        allocator.free(self.other_kind);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []EntityLinkRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Focus entity for the Entity-Link Graph view.
///
/// Specifies the entity whose neighborhood is currently displayed.
/// A null focus means "no entity selected" — the view shows an empty state.
pub const EntityFocus = struct {
    /// Entity kind string, e.g. "task", "plan", "decision".
    kind: []const u8,
    /// Entity id.
    id: i64,
    /// Resolved title (may be empty when resolution fails).
    title: []const u8,
    /// Human-readable header: "kind:id — title".
    header: []const u8,

    pub fn deinit(self: EntityFocus, allocator: std.mem.Allocator) void {
        allocator.free(self.kind);
        allocator.free(self.title);
        allocator.free(self.header);
    }
};

/// All data for the Entity-Link Graph view for the current focus entity.
///
/// Caller owns the result; free via `EntityLinkGraphData.deinit`.
pub const EntityLinkGraphData = struct {
    /// Focus entity. Null when nothing is selected.
    focus: ?EntityFocus,
    /// All related-entity rows (both inbound + outbound, all relationship kinds).
    /// Ordered by: relationship text asc, direction (outbound first) asc, other_id asc.
    rows: []EntityLinkRow,
    /// Pre-formatted "Links: N" label — heap-owned so grapheme pointers from
    /// printSegment remain valid after the render function returns. This avoids
    /// the stack-buffer-dangling-pointer hazard in render-level tests.
    links_count_label: []const u8,

    pub fn deinit(self: EntityLinkGraphData, allocator: std.mem.Allocator) void {
        if (self.focus) |f| f.deinit(allocator);
        EntityLinkRow.deinitMany(self.rows, allocator);
        allocator.free(self.links_count_label);
    }
};

/// Query the Entity-Link Graph data for a given focus entity (task 4027).
///
/// Returns all entity_links rows where either:
///   (a) from_kind=kind AND from_id=id (outbound: focus → other)
///   (b) to_kind=kind   AND to_id=id   (inbound: other → focus)
///
/// For each row, resolves the other entity's title via resolveEntityTitle.
/// Builds a human-readable display_text for each row:
///   "[out] derives-from  artifact:5 — FoundingSpec"
///   "[in]  blocks        task:12 — Implement parser"
///
/// All strings in the result are heap-owned. Caller frees via
/// EntityLinkGraphData.deinit.
pub fn queryEntityLinkGraph(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    focus_kind: []const u8,
    focus_id: i64,
) !EntityLinkGraphData {
    var rows: std.ArrayList(EntityLinkRow) = .empty;
    errdefer {
        for (rows.items) |r| r.deinit(allocator);
        rows.deinit(allocator);
    }

    // ---- Outbound edges: focus is the from side ---------------------------
    {
        var stmt = d.prepare(
            \\select id, to_kind, to_id, relationship
            \\from entity_links
            \\where from_kind = ? and from_id = ?
            \\order by relationship asc, to_kind asc, to_id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        var params = [_]db.sqlite.Param{
            .{ .text = focus_kind },
            .{ .int = focus_id },
        };
        stmt.bind(&params) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const link_id = stmt.columnInt(0);
                    const to_kind = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(to_kind);
                    const to_id = stmt.columnInt(2);
                    const rel_str = try stmt.columnTextAlloc(3, allocator);
                    defer allocator.free(rel_str);

                    const rel = LinkRelationship.fromText(rel_str) orelse {
                        // Unknown relationship — skip rather than panic.
                        allocator.free(to_kind);
                        continue;
                    };

                    // Resolve other entity title.
                    const title_opt = resolveEntityTitle(d, allocator, to_kind, to_id);
                    defer if (title_opt) |t| allocator.free(t);

                    // Build display_text.  Memory contract: display_text is heap-owned
                    // and stable; to_kind is also kept as other_kind.
                    const display_text = if (title_opt) |t|
                        try std.fmt.allocPrint(
                            allocator,
                            "[out] {s}  {s}:{d} — {s}",
                            .{ rel.toText(), to_kind, to_id, t },
                        )
                    else
                        try std.fmt.allocPrint(
                            allocator,
                            "[out] {s}  {s}:{d}",
                            .{ rel.toText(), to_kind, to_id },
                        );
                    errdefer allocator.free(display_text);

                    try rows.append(allocator, .{
                        .link_id = link_id,
                        .relationship = rel,
                        .direction = .outbound,
                        .other_kind = to_kind,
                        .other_id = to_id,
                        .display_text = display_text,
                    });
                },
            }
        }
    }

    // ---- Inbound edges: focus is the to side ------------------------------
    {
        var stmt = d.prepare(
            \\select id, from_kind, from_id, relationship
            \\from entity_links
            \\where to_kind = ? and to_id = ?
            \\order by relationship asc, from_kind asc, from_id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        var params = [_]db.sqlite.Param{
            .{ .text = focus_kind },
            .{ .int = focus_id },
        };
        stmt.bind(&params) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const link_id = stmt.columnInt(0);
                    const from_kind = try stmt.columnTextAlloc(1, allocator);
                    errdefer allocator.free(from_kind);
                    const from_id = stmt.columnInt(2);
                    const rel_str = try stmt.columnTextAlloc(3, allocator);
                    defer allocator.free(rel_str);

                    const rel = LinkRelationship.fromText(rel_str) orelse {
                        allocator.free(from_kind);
                        continue;
                    };

                    const title_opt = resolveEntityTitle(d, allocator, from_kind, from_id);
                    defer if (title_opt) |t| allocator.free(t);

                    const display_text = if (title_opt) |t|
                        try std.fmt.allocPrint(
                            allocator,
                            "[in]  {s}  {s}:{d} — {s}",
                            .{ rel.toText(), from_kind, from_id, t },
                        )
                    else
                        try std.fmt.allocPrint(
                            allocator,
                            "[in]  {s}  {s}:{d}",
                            .{ rel.toText(), from_kind, from_id },
                        );
                    errdefer allocator.free(display_text);

                    try rows.append(allocator, .{
                        .link_id = link_id,
                        .relationship = rel,
                        .direction = .inbound,
                        .other_kind = from_kind,
                        .other_id = from_id,
                        .display_text = display_text,
                    });
                },
            }
        }
    }

    // Sort: relationship text asc, direction (outbound < inbound) asc, other_id asc.
    // This groups all outbound edges for a relationship before inbound edges.
    const sorted_rows = try rows.toOwnedSlice(allocator);
    std.mem.sort(EntityLinkRow, sorted_rows, {}, struct {
        fn lt(_: void, a: EntityLinkRow, b: EntityLinkRow) bool {
            const ra = a.relationship.toText();
            const rb = b.relationship.toText();
            const rel_cmp = std.mem.order(u8, ra, rb);
            if (rel_cmp != .eq) return rel_cmp == .lt;
            // Same relationship: outbound < inbound.
            const da: u8 = if (a.direction == .outbound) 0 else 1;
            const db_dir: u8 = if (b.direction == .outbound) 0 else 1;
            if (da != db_dir) return da < db_dir;
            // Same direction: sort by other_id ascending.
            return a.other_id < b.other_id;
        }
    }.lt);

    // Build the focus entity descriptor.
    const kind_owned = try allocator.dupe(u8, focus_kind);
    errdefer allocator.free(kind_owned);

    const title_opt = resolveEntityTitle(d, allocator, focus_kind, focus_id);
    // MEMORY GUARD (brief rule (c)): on OOM in resolveEntityTitle the result is null;
    // we use an owned empty string — never alias focus_kind into the title field.
    const title_owned = title_opt orelse try allocator.dupe(u8, "");
    errdefer allocator.free(title_owned);

    const header = if (title_owned.len > 0)
        try std.fmt.allocPrint(allocator, "{s}:{d} — {s}", .{ focus_kind, focus_id, title_owned })
    else
        try std.fmt.allocPrint(allocator, "{s}:{d}", .{ focus_kind, focus_id });
    errdefer allocator.free(header);

    // Pre-format the "Links: N" label as a heap string so render functions can
    // pass it directly to printSegment without a stack-buffer dangling-pointer hazard.
    const links_count_label = try std.fmt.allocPrint(allocator, "Links: {d}", .{sorted_rows.len});
    errdefer allocator.free(links_count_label);

    return .{
        .focus = .{
            .kind = kind_owned,
            .id = focus_id,
            .title = title_owned,
            .header = header,
        },
        .rows = sorted_rows,
        .links_count_label = links_count_label,
    };
}

/// Query entity-link graph data for an entity chosen from the first available
/// entity in the DB (task, plan, decision — whichever has the most links first).
/// Used as the default-first-entity heuristic for the view's initial load.
///
/// Returns null focus data (empty rows) when the DB has no entities.
pub fn queryEntityLinkGraphDefault(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) !EntityLinkGraphData {
    // Pick the entity that appears most often in entity_links as a starting point.
    // Fallback ordering: check entity_links first (from side), then tasks, then plans.
    var stmt = d.prepare(
        \\select from_kind, from_id from entity_links
        \\group by from_kind, from_id
        \\order by count(*) desc limit 1
    ) catch return makeEmptyEntityLinkGraphData(allocator);
    defer stmt.finalize();
    stmt.bind(&.{}) catch return makeEmptyEntityLinkGraphData(allocator);

    switch (stmt.step() catch return makeEmptyEntityLinkGraphData(allocator)) {
        .done => {
            // No entity_links at all. Try a task as default.
            // NOTE: stmt will be finalized by the defer above; do NOT call
            // stmt.finalize() here or it would double-finalize.
            return queryEntityLinkGraphFirstTask(d, allocator);
        },
        .row => {
            const kind_str = stmt.columnTextAlloc(0, allocator) catch
                return makeEmptyEntityLinkGraphData(allocator);
            defer allocator.free(kind_str);
            const id = stmt.columnInt(1);
            return queryEntityLinkGraph(d, allocator, kind_str, id);
        },
    }
}

fn queryEntityLinkGraphFirstTask(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) !EntityLinkGraphData {
    var stmt = d.prepare("select id from tasks order by id asc limit 1") catch
        return makeEmptyEntityLinkGraphData(allocator);
    defer stmt.finalize();
    stmt.bind(&.{}) catch return makeEmptyEntityLinkGraphData(allocator);
    switch (stmt.step() catch return makeEmptyEntityLinkGraphData(allocator)) {
        .done => return makeEmptyEntityLinkGraphData(allocator),
        .row => {
            const tid = stmt.columnInt(0);
            return queryEntityLinkGraph(d, allocator, "task", tid);
        },
    }
}

/// Build an empty EntityLinkGraphData with heap-allocated (zero-length) rows
/// slice so that deinit can safely call allocator.free(rows).
fn makeEmptyEntityLinkGraphData(allocator: std.mem.Allocator) !EntityLinkGraphData {
    // Allocate a zero-length slice so deinit's allocator.free(rows) is safe.
    const empty_rows = try allocator.alloc(EntityLinkRow, 0);
    const label = try allocator.dupe(u8, "Links: 0");
    return .{
        .focus = null,
        .rows = empty_rows,
        .links_count_label = label,
    };
}
