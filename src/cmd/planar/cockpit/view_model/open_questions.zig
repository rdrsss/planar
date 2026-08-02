//! Open-question, linked-entity, and detail cockpit queries.

const std = @import("std");
const db = @import("db");
const StatusBadge = @import("common.zig").StatusBadge;
const questionStatusBadge = @import("scope_explorer.zig").questionStatusBadge;
const resolveEntityTitle = @import("entity_title.zig").resolve;

// Open Questions view-model  (tasks 4023, 4024)
// =========================================================================

/// Status filter for the Open Questions view (task 4024).
///
/// Status values confirmed from migration 00003_work_items.up.sql:
///   check(status in ('open','answered','wontfix'))
pub const QuestionStatusFilter = enum {
    /// Show only open questions (default).
    open,
    /// Show only answered questions.
    answered,
    /// Show only wontfix questions.
    wontfix,
    /// Show all questions regardless of status.
    all,

    /// The next filter value in the cycle order: open → answered → wontfix → all → open.
    pub fn next(self: QuestionStatusFilter) QuestionStatusFilter {
        return switch (self) {
            .open => .answered,
            .answered => .wontfix,
            .wontfix => .all,
            .all => .open,
        };
    }

    /// SQL WHERE clause fragment for this filter. Returns null when filter=all.
    pub fn sqlWhere(self: QuestionStatusFilter) ?[]const u8 {
        return switch (self) {
            .open => "status = 'open'",
            .answered => "status = 'answered'",
            .wontfix => "status = 'wontfix'",
            .all => null,
        };
    }

    /// Short display label for the status bar.
    pub fn label(self: QuestionStatusFilter) []const u8 {
        return switch (self) {
            .open => "open",
            .answered => "answered",
            .wontfix => "wontfix",
            .all => "all",
        };
    }
};

/// One row in the Open Questions navigator list.
///
/// Schema reference: migrations/00003_work_items.up.sql
///   questions(id, scope_kind, scope_id, title, body, status,
///             answer_body, answered_at, created_at, updated_at)
///   status check: ('open','answered','wontfix')
pub const OpenQuestionsRow = struct {
    id: i64,
    /// Question title.
    title: []const u8,
    /// Status badge derived from the questions.status column.
    badge: StatusBadge,
    /// Status string for display ("open" / "answered" / "wontfix").
    status: []const u8,
    /// Pre-formatted display string: "[badge] status  title" — heap-allocated
    /// so grapheme pointers from printSegment remain valid after render returns.
    display_text: []const u8,

    pub fn deinit(self: OpenQuestionsRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.status);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []OpenQuestionsRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One linked entity for a question (from entity_links).
///
/// Questions can be linked to plans, artifacts, tasks, decisions, etc. via
/// entity_links (both from_kind='question' and to_kind='question' directions).
/// This struct surfaces the target entity so the operator can jump to it.
///
/// The 'addresses' relationship is the canonical question→plan/artifact link
/// (a question "addresses" a plan's concern). Both directions are surfaced.
pub const QuestionLinkedEntity = struct {
    /// Target entity kind string, e.g. "plan", "artifact".
    target_kind: []const u8,
    /// Target entity id.
    target_id: i64,
    /// Relationship kind, e.g. "addresses", "derives-from".
    relationship: []const u8,
    /// Human-readable label: "<kind>:<id> — <title>" or "<kind>:<id>".
    label: []const u8,

    pub fn deinit(self: QuestionLinkedEntity, allocator: std.mem.Allocator) void {
        allocator.free(self.target_kind);
        allocator.free(self.relationship);
        allocator.free(self.label);
    }

    pub fn deinitMany(rows: []QuestionLinkedEntity, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Detail pane data for a selected question in the Open Questions view.
///
/// Combines the question's body (markdown) with the list of linked entities
/// from entity_links and the answer_body when status='answered'.
///
/// All fields are caller-owned; free via `OpenQuestionsDetail.deinit`.
pub const OpenQuestionsDetail = struct {
    /// Question id.
    id: i64,
    /// Question title.
    title: []const u8,
    /// Rendered markdown body: "**Status:** {s}\n\n{body}\n\n**Answer:** {answer_body}".
    body: []const u8,
    /// Linked entities from entity_links (both directions). May be empty.
    linked: []QuestionLinkedEntity,

    pub fn deinit(self: OpenQuestionsDetail, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.body);
        QuestionLinkedEntity.deinitMany(self.linked, allocator);
    }
};

/// Query questions filtered by status, ordered newest-first (updated_at desc, id desc).
///
/// Status values confirmed from migration 00003_work_items.up.sql:
///   check(status in ('open','answered','wontfix'))
///
/// Caller owns the result; free via `OpenQuestionsRow.deinitMany`.
pub fn queryOpenQuestions(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter: QuestionStatusFilter,
) ![]OpenQuestionsRow {
    // Build the query dynamically based on the filter. Using two compile-time
    // constants avoids string interpolation and keeps the query typed/safe.
    const sql_all =
        \\select id, title, status
        \\from questions
        \\order by updated_at desc, id desc
    ;
    const sql_open =
        \\select id, title, status
        \\from questions
        \\where status = 'open'
        \\order by updated_at desc, id desc
    ;
    const sql_answered =
        \\select id, title, status
        \\from questions
        \\where status = 'answered'
        \\order by updated_at desc, id desc
    ;
    const sql_wontfix =
        \\select id, title, status
        \\from questions
        \\where status = 'wontfix'
        \\order by updated_at desc, id desc
    ;

    const sql = switch (filter) {
        .all => sql_all,
        .open => sql_open,
        .answered => sql_answered,
        .wontfix => sql_wontfix,
    };

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return error.QueryFailed;

    var out: std.ArrayList(OpenQuestionsRow) = .empty;
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
                errdefer allocator.free(status_text);

                const badge = questionStatusBadge(status_text);

                // Pre-format the navigator display string so that grapheme
                // pointers from printSegment remain valid after render returns.
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s}  {s}",
                    .{ badge.glyph(), status_text, title },
                );
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .title = title,
                    .badge = badge,
                    .status = status_text,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query linked entities for a question from entity_links.
///
/// Surfaces both directions:
///   (1) Outgoing: question→X (from_kind='question', from_id=question_id)
///   (2) Incoming: X→question (to_kind='question', to_id=question_id)
///
/// Per migration 00004_entity_links.up.sql:
///   from_kind/to_kind include 'question' (confirmed in the CHECK constraint)
///   valid relationships: ('derives-from','depends-on','addresses','verifies','cites','supersedes','touches')
///
/// For each linked entity, the title is resolved via resolveEntityTitle.
/// Caller owns the result; free via `QuestionLinkedEntity.deinitMany`.
pub fn queryQuestionLinkedEntities(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    question_id: i64,
) ![]QuestionLinkedEntity {
    var out: std.ArrayList(QuestionLinkedEntity) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    // (1) Outgoing: question → X (question as from_kind).
    {
        var stmt = d.prepare(
            \\select to_kind, to_id, relationship
            \\from entity_links
            \\where from_kind = 'question' and from_id = ?
            \\order by relationship asc, to_kind asc, to_id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = question_id }}) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const to_kind = try stmt.columnTextAlloc(0, allocator);
                    errdefer allocator.free(to_kind);
                    const to_id = stmt.columnInt(1);
                    const rel = try stmt.columnTextAlloc(2, allocator);
                    errdefer allocator.free(rel);

                    const title_opt = resolveEntityTitle(d, allocator, to_kind, to_id);
                    // On OOM, propagate the error rather than aliasing to_kind
                    // into label — aliasing causes a double-free in deinit.
                    const label = if (title_opt) |t|
                        std.fmt.allocPrint(allocator, "{s}:{d} — {s}", .{ to_kind, to_id, t }) catch blk: {
                            allocator.free(t);
                            break :blk std.fmt.allocPrint(allocator, "{s}:{d}", .{ to_kind, to_id }) catch return error.OutOfMemory;
                        }
                    else
                        std.fmt.allocPrint(allocator, "{s}:{d}", .{ to_kind, to_id }) catch return error.OutOfMemory;
                    errdefer allocator.free(label);

                    if (title_opt) |t| allocator.free(t);

                    try out.append(allocator, .{
                        .target_kind = to_kind,
                        .target_id = to_id,
                        .relationship = rel,
                        .label = label,
                    });
                },
            }
        }
    }

    // (2) Incoming: X → question (question as to_kind).
    {
        var stmt = d.prepare(
            \\select from_kind, from_id, relationship
            \\from entity_links
            \\where to_kind = 'question' and to_id = ?
            \\order by relationship asc, from_kind asc, from_id asc
        ) catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = question_id }}) catch return error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return error.QueryFailed) {
                .done => break,
                .row => {
                    const from_kind = try stmt.columnTextAlloc(0, allocator);
                    errdefer allocator.free(from_kind);
                    const from_id = stmt.columnInt(1);
                    const rel = try stmt.columnTextAlloc(2, allocator);
                    errdefer allocator.free(rel);

                    const title_opt = resolveEntityTitle(d, allocator, from_kind, from_id);
                    // On OOM, propagate the error rather than aliasing from_kind
                    // into label — aliasing causes a double-free in deinit.
                    const label = if (title_opt) |t|
                        std.fmt.allocPrint(allocator, "{s}:{d} — {s}", .{ from_kind, from_id, t }) catch blk: {
                            allocator.free(t);
                            break :blk std.fmt.allocPrint(allocator, "{s}:{d}", .{ from_kind, from_id }) catch return error.OutOfMemory;
                        }
                    else
                        std.fmt.allocPrint(allocator, "{s}:{d}", .{ from_kind, from_id }) catch return error.OutOfMemory;
                    errdefer allocator.free(label);

                    if (title_opt) |t| allocator.free(t);

                    try out.append(allocator, .{
                        .target_kind = from_kind,
                        .target_id = from_id,
                        .relationship = rel,
                        .label = label,
                    });
                },
            }
        }
    }

    return try out.toOwnedSlice(allocator);
}

/// Query full Open Questions detail for a selected question.
///
/// Combines the question's body (markdown) with the linked entities from
/// entity_links and the answer_body when status='answered'.
///
/// Returns null when the question does not exist.
/// Caller owns the result; free via `OpenQuestionsDetail.deinit`.
pub fn queryOpenQuestionsDetail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    question_id: i64,
) !?OpenQuestionsDetail {
    var stmt = d.prepare(
        "select title, body, status, answer_body from questions where id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = question_id }}) catch return error.QueryFailed;

    switch (stmt.step() catch return error.QueryFailed) {
        .done => return null,
        .row => {
            const title = try stmt.columnTextAlloc(0, allocator);
            errdefer allocator.free(title);
            const body_opt = try stmt.columnTextOpt(1, allocator);
            defer if (body_opt) |s| allocator.free(s);
            const status_text = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(status_text);
            const answer_opt = try stmt.columnTextOpt(3, allocator);
            defer if (answer_opt) |s| allocator.free(s);

            // Build the markdown body including status, body text, and answer.
            var body_buf: std.ArrayList(u8) = .empty;
            errdefer body_buf.deinit(allocator);

            const status_line = try std.fmt.allocPrint(
                allocator,
                "**Status:** {s}",
                .{status_text},
            );
            defer allocator.free(status_line);
            try body_buf.appendSlice(allocator, status_line);

            if (body_opt) |b| {
                const part = try std.fmt.allocPrint(allocator, "\n\n{s}", .{b});
                defer allocator.free(part);
                try body_buf.appendSlice(allocator, part);
            }

            if (answer_opt) |ans| {
                const part = try std.fmt.allocPrint(allocator, "\n\n**Answer:** {s}", .{ans});
                defer allocator.free(part);
                try body_buf.appendSlice(allocator, part);
            }

            const body = try body_buf.toOwnedSlice(allocator);
            errdefer allocator.free(body);

            const linked = try queryQuestionLinkedEntities(d, allocator, question_id);
            errdefer QuestionLinkedEntity.deinitMany(linked, allocator);

            return .{
                .id = question_id,
                .title = title,
                .body = body,
                .linked = linked,
            };
        },
    }
}
