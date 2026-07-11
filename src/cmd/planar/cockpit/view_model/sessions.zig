//! Sessions, handoffs, snapshots, and commit-history cockpit queries.

const std = @import("std");
const db = @import("db");
const testing = std.testing;

// =========================================================================
// Sessions & Handoff view-model  (tasks 4032, 4033, 4034)
// =========================================================================
//
// Schema sources (confirmed from migrations):
//   sessions (00005_sessions.up.sql, 00021_session_commits.up.sql):
//     id, task_id, project_id, agent_id, vendor, vendor_session_id, model,
//     started_at, ended_at, summary, repo_root, head_sha_at_start
//   session_entries (00005_sessions.up.sql):
//     id, session_id, ordinal, prefix, body, created_at
//     prefix check: ('action','observation','decision','question','file',
//                    'command','note','error','read')
//   context_snapshots (00005_sessions.up.sql):
//     id, session_id, task_id, vendor, vendor_session_id, body, next_action,
//     created_at
//   handoffs (00005_sessions.up.sql, 00017_handoffs_worktree.up.sql):
//     id, from_snapshot_id, to_session_id, from_vendor, to_vendor,
//     status, validated_at, consumed_at, created_at,
//     worktree_path, repo_root, branch
//     status check: ('pending','validated','consumed','abandoned')
//   session_commits (00021_session_commits.up.sql):
//     id, session_id, claim_id, sha, repo_root, branch, subject, author,
//     committed_at, recorded_at
//
// Resume-readiness representation: A handoff is resume-ready when its
// status is 'validated' (the handoff has been validated and its snapshot
// body is populated) or 'consumed' (the handoff was already consumed by a
// successor session — historically ready). A status of 'pending' means the
// snapshot has not yet been validated; 'abandoned' means the handoff was
// intentionally cancelled. This is checked by looking at handoffs.status
// and the associated context_snapshots row. A context_snapshot row with a
// non-null next_action is the minimum readiness marker.

/// One row in the Sessions navigator list.
///
/// Columns: sessions.id, vendor, model, started_at, ended_at, summary,
///          task_id (for display), entry count.
pub const SessionRow = struct {
    id: i64,
    /// Vendor string (e.g. "claude", "codex").
    vendor: []const u8,
    /// Model string or null.
    model: ?[]const u8,
    /// ISO-8601 started_at.
    started_at: []const u8,
    /// ISO-8601 ended_at or null (session still active).
    ended_at: ?[]const u8,
    /// Optional summary.
    summary: ?[]const u8,
    /// Referenced task id (nullable).
    task_id: ?i64,
    /// Count of session_entries rows for this session.
    entry_count: i64,
    /// Pre-formatted display text: "vendor [model?] started_at (N entries)".
    /// Heap-allocated; freed by deinit.
    display_text: []const u8,

    pub fn deinit(self: SessionRow, allocator: std.mem.Allocator) void {
        allocator.free(self.vendor);
        if (self.model) |s| allocator.free(s);
        allocator.free(self.started_at);
        if (self.ended_at) |s| allocator.free(s);
        if (self.summary) |s| allocator.free(s);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []SessionRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One entry in the session lineage detail pane.
///
/// Columns: session_entries.id, ordinal, prefix, body, created_at.
pub const SessionEntryRow = struct {
    id: i64,
    ordinal: i64,
    /// Entry kind prefix: 'action', 'observation', 'decision', etc.
    prefix: []const u8,
    /// Entry body text.
    body: []const u8,
    /// ISO-8601 created_at.
    created_at: []const u8,
    /// Pre-formatted display text: "[prefix] body_prefix".
    display_text: []const u8,

    pub fn deinit(self: SessionEntryRow, allocator: std.mem.Allocator) void {
        allocator.free(self.prefix);
        allocator.free(self.body);
        allocator.free(self.created_at);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []SessionEntryRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One commit associated with a session (from session_commits).
///
/// Columns: session_commits.sha, subject, author, branch, committed_at.
pub const SessionCommitRow = struct {
    id: i64,
    /// Full commit SHA.
    sha: []const u8,
    /// Commit subject line (first line of message), or null.
    subject: ?[]const u8,
    /// Author string, or null.
    author: ?[]const u8,
    /// Branch name, or null.
    branch: ?[]const u8,
    /// ISO-8601 committed_at, or null.
    committed_at: ?[]const u8,
    /// Pre-formatted display text: "sha_short subject_prefix".
    display_text: []const u8,

    pub fn deinit(self: SessionCommitRow, allocator: std.mem.Allocator) void {
        allocator.free(self.sha);
        if (self.subject) |s| allocator.free(s);
        if (self.author) |s| allocator.free(s);
        if (self.branch) |s| allocator.free(s);
        if (self.committed_at) |s| allocator.free(s);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []SessionCommitRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// One handoff row with resume-readiness information.
///
/// Columns: handoffs.id, from_vendor, to_vendor, status, created_at,
///          validated_at, consumed_at, context_snapshots.next_action,
///          context_snapshots.body (non-null presence).
///
/// Resume-readiness: a handoff is ready when status in ('validated','consumed')
/// AND the associated context_snapshot has a non-null next_action.
/// A 'pending' handoff is not yet ready (snapshot not validated).
/// An 'abandoned' handoff is not resume-ready.
pub const HandoffRow = struct {
    id: i64,
    /// Source vendor.
    from_vendor: []const u8,
    /// Destination vendor, or null.
    to_vendor: ?[]const u8,
    /// Handoff lifecycle status: 'pending', 'validated', 'consumed', 'abandoned'.
    /// Surfaced as lifecycle info only — it does NOT gate resume-readiness.
    status: []const u8,
    /// ISO-8601 created_at.
    created_at: []const u8,
    /// ISO-8601 validated_at, or null.
    validated_at: ?[]const u8,
    /// Whether the associated task has a non-empty next_action (tasks.next_action).
    /// Mirrors the first condition in resume.validate (src/engine/runtime/resume.zig:300).
    task_has_next_action: bool,
    /// Whether a context_snapshots row scoped to the associated task exists.
    /// Mirrors the second condition in resume.validate (src/engine/runtime/resume.zig:314).
    task_has_snapshot: bool,
    /// Pre-formatted display text: "from→to [status] (ready/not-ready)".
    display_text: []const u8,

    /// True when the handoff is resume-ready, mirroring the engine's authoritative
    /// oracle at src/engine/runtime/resume.zig (validate, lines 300-336):
    ///   tasks.next_action is non-empty AND a task-scoped context_snapshots row exists.
    /// handoffs.status is NOT a readiness gate — it is lifecycle metadata only.
    pub fn isResumeReady(self: *const HandoffRow) bool {
        return self.task_has_next_action and self.task_has_snapshot;
    }

    pub fn deinit(self: HandoffRow, allocator: std.mem.Allocator) void {
        allocator.free(self.from_vendor);
        if (self.to_vendor) |s| allocator.free(s);
        allocator.free(self.status);
        allocator.free(self.created_at);
        if (self.validated_at) |s| allocator.free(s);
        allocator.free(self.display_text);
    }

    pub fn deinitMany(rows: []HandoffRow, allocator: std.mem.Allocator) void {
        for (rows) |r| r.deinit(allocator);
        allocator.free(rows);
    }
};

/// Full snapshot for the Sessions & Handoff view.
pub const SessionsHandoffSnapshot = struct {
    /// Sessions list, newest-first.
    sessions: []SessionRow,
    /// Handoffs list, newest-first.
    handoffs: []HandoffRow,

    pub fn deinit(self: SessionsHandoffSnapshot, allocator: std.mem.Allocator) void {
        SessionRow.deinitMany(self.sessions, allocator);
        HandoffRow.deinitMany(self.handoffs, allocator);
    }
};

/// Query sessions ordered newest-first. Includes entry count per session.
/// Caller owns the result; free via `SessionRow.deinitMany`.
pub fn querySessions(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]SessionRow {
    var stmt = d.prepare(
        \\select s.id, s.vendor, s.model, s.started_at, s.ended_at, s.summary,
        \\       s.task_id,
        \\       (select count(*) from session_entries se where se.session_id = s.id)
        \\         as entry_count
        \\from sessions s
        \\order by s.started_at desc, s.id desc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(SessionRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const vendor = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(vendor);
                const model = try stmt.columnTextOpt(2, allocator);
                errdefer if (model) |s| allocator.free(s);
                const started_at = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(started_at);
                const ended_at = try stmt.columnTextOpt(4, allocator);
                errdefer if (ended_at) |s| allocator.free(s);
                const summary = try stmt.columnTextOpt(5, allocator);
                errdefer if (summary) |s| allocator.free(s);
                const task_id = stmt.columnIntOpt(6);
                const entry_count = stmt.columnInt(7);

                // Build display_text: "vendor [model] started_prefix (N entries)"
                // Use at most the date part of started_at (first 10 chars).
                const date_prefix: []const u8 = if (started_at.len >= 10)
                    started_at[0..10]
                else
                    started_at;

                const display_text = if (model) |m|
                    try std.fmt.allocPrint(
                        allocator,
                        "{s} [{s}] {s} ({d} entries)",
                        .{ vendor, m, date_prefix, entry_count },
                    )
                else
                    try std.fmt.allocPrint(
                        allocator,
                        "{s} {s} ({d} entries)",
                        .{ vendor, date_prefix, entry_count },
                    );
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .vendor = vendor,
                    .model = model,
                    .started_at = started_at,
                    .ended_at = ended_at,
                    .summary = summary,
                    .task_id = task_id,
                    .entry_count = entry_count,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query session_entries for a specific session, ordered by ordinal.
/// Each entry is the activity lineage of the session.
/// Caller owns the result; free via `SessionEntryRow.deinitMany`.
pub fn querySessionEntries(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
) ![]SessionEntryRow {
    var stmt = d.prepare(
        \\select se.id, se.ordinal, se.prefix, se.body, se.created_at
        \\from session_entries se
        \\where se.session_id = ?
        \\order by se.ordinal asc, se.id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(SessionEntryRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const ordinal = stmt.columnInt(1);
                const prefix = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(prefix);
                const body = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(body);
                const created_at = try stmt.columnTextAlloc(4, allocator);
                errdefer allocator.free(created_at);

                // display_text: "[prefix] body_prefix (first 60 chars of body)"
                const body_preview: []const u8 = if (body.len > 60) body[0..60] else body;
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "[{s}] {s}",
                    .{ prefix, body_preview },
                );
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .ordinal = ordinal,
                    .prefix = prefix,
                    .body = body,
                    .created_at = created_at,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query session_commits for a specific session, ordered by committed_at then id.
/// Caller owns the result; free via `SessionCommitRow.deinitMany`.
pub fn querySessionCommits(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
) ![]SessionCommitRow {
    var stmt = d.prepare(
        \\select sc.id, sc.sha, sc.subject, sc.author, sc.branch, sc.committed_at
        \\from session_commits sc
        \\where sc.session_id = ?
        \\order by coalesce(sc.committed_at, sc.recorded_at) asc, sc.id asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return error.QueryFailed;

    var out: std.ArrayList(SessionCommitRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const sha = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(sha);
                const subject = try stmt.columnTextOpt(2, allocator);
                errdefer if (subject) |s| allocator.free(s);
                const author = try stmt.columnTextOpt(3, allocator);
                errdefer if (author) |s| allocator.free(s);
                const branch = try stmt.columnTextOpt(4, allocator);
                errdefer if (branch) |s| allocator.free(s);
                const committed_at = try stmt.columnTextOpt(5, allocator);
                errdefer if (committed_at) |s| allocator.free(s);

                // display_text: "sha_short [subject_preview]"
                const sha_short: []const u8 = if (sha.len >= 8) sha[0..8] else sha;
                const display_text = if (subject) |subj|
                    try std.fmt.allocPrint(
                        allocator,
                        "{s} {s}",
                        .{ sha_short, subj },
                    )
                else
                    try allocator.dupe(u8, sha_short);
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .sha = sha,
                    .subject = subject,
                    .author = author,
                    .branch = branch,
                    .committed_at = committed_at,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query handoffs ordered newest-first.
///
/// Joins through context_snapshots → tasks to compute the task-centric
/// resume-readiness signal that mirrors the engine's authoritative oracle
/// at src/engine/runtime/resume.zig (validate, lines 300-336):
///
///   task_has_next_action: tasks.next_action is non-empty for the task
///     associated with this handoff's source snapshot.
///   task_has_snapshot: at least one context_snapshots row is scoped
///     to that task (context_snapshots.task_id = tasks.id).
///
/// handoffs.status is surfaced as lifecycle metadata but does NOT gate
/// readiness — a 'pending' handoff whose task satisfies the engine rule
/// is READY; a 'consumed' handoff whose task next_action was cleared is
/// NOT READY. isResumeReady() encodes only the task-centric rule.
///
/// Caller owns the result; free via `HandoffRow.deinitMany`.
pub fn queryHandoffs(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) ![]HandoffRow {
    var stmt = d.prepare(
        \\select h.id, h.from_vendor, h.to_vendor, h.status, h.created_at,
        \\       h.validated_at,
        \\       (t.next_action is not null and t.next_action != '') as task_has_next_action,
        \\       (exists (
        \\           select 1 from context_snapshots cs2
        \\           where cs2.task_id = cs.task_id and cs2.task_id is not null
        \\       )) as task_has_snapshot
        \\from handoffs h
        \\left join context_snapshots cs on cs.id = h.from_snapshot_id
        \\left join tasks t on t.id = cs.task_id
        \\order by h.created_at desc, h.id desc
    ) catch return error.QueryFailed;
    defer stmt.finalize();

    var out: std.ArrayList(HandoffRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const from_vendor = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(from_vendor);
                const to_vendor = try stmt.columnTextOpt(2, allocator);
                errdefer if (to_vendor) |s| allocator.free(s);
                const status = try stmt.columnTextAlloc(3, allocator);
                errdefer allocator.free(status);
                const created_at = try stmt.columnTextAlloc(4, allocator);
                errdefer allocator.free(created_at);
                const validated_at = try stmt.columnTextOpt(5, allocator);
                errdefer if (validated_at) |s| allocator.free(s);
                const task_has_next_action = stmt.columnInt(6) != 0;
                const task_has_snapshot = stmt.columnInt(7) != 0;

                // Readiness is task-centric: both task conditions must hold.
                const is_ready = task_has_next_action and task_has_snapshot;

                // Determine readiness label.
                const ready_label: []const u8 = if (is_ready)
                    "ready"
                else
                    "not-ready";

                // display_text: "from→to  [status]  (ready/not-ready)"
                const to_str: []const u8 = to_vendor orelse "?";
                const display_text = try std.fmt.allocPrint(
                    allocator,
                    "{s}→{s}  [{s}]  ({s})",
                    .{ from_vendor, to_str, status, ready_label },
                );
                errdefer allocator.free(display_text);

                try out.append(allocator, .{
                    .id = id,
                    .from_vendor = from_vendor,
                    .to_vendor = to_vendor,
                    .status = status,
                    .created_at = created_at,
                    .validated_at = validated_at,
                    .task_has_next_action = task_has_next_action,
                    .task_has_snapshot = task_has_snapshot,
                    .display_text = display_text,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Query the full SessionsHandoffSnapshot.
pub fn querySessionsHandoffSnapshot(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) !SessionsHandoffSnapshot {
    const sessions = try querySessions(d, allocator);
    errdefer SessionRow.deinitMany(sessions, allocator);

    const handoffs = try queryHandoffs(d, allocator);
    errdefer HandoffRow.deinitMany(handoffs, allocator);

    return .{
        .sessions = sessions,
        .handoffs = handoffs,
    };
}

// =========================================================================
// Sessions & Handoff view-model tests
// =========================================================================

fn setupTestDbSessions(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "view_model: querySessions on empty DB returns empty slice (task 4032)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const rows = try querySessions(&d, a);
    defer SessionRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 0), rows.len);
}

test "view_model: querySessions returns session row with entry count (task 4032)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','Session Task','todo')",
        &.{},
    );
    const sid = try d.execParams(
        "insert into sessions (task_id, vendor, model, started_at) values (?, 'claude', 'claude-3', '2026-06-10T10:00:00.000Z')",
        &.{.{ .int = tid }},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'action', 'Implemented the feature')",
        &.{.{ .int = sid }},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 2, 'observation', 'Tests pass')",
        &.{.{ .int = sid }},
    );

    const rows = try querySessions(&d, a);
    defer SessionRow.deinitMany(rows, a);

    try testing.expectEqual(@as(usize, 1), rows.len);
    try testing.expectEqualStrings("claude", rows[0].vendor);
    try testing.expect(rows[0].model != null);
    try testing.expectEqualStrings("claude-3", rows[0].model.?);
    try testing.expectEqual(@as(i64, 2), rows[0].entry_count);
    try testing.expectEqual(tid, rows[0].task_id.?);
    // display_text must contain vendor and entry count.
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "claude") != null);
    try testing.expect(std.mem.indexOf(u8, rows[0].display_text, "2") != null);
}

test "view_model: querySessionEntries returns entries in ordinal order (task 4032)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor, started_at) values ('codex', '2026-06-11T09:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 2, 'observation', 'Second entry')",
        &.{.{ .int = sid }},
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'action', 'First entry')",
        &.{.{ .int = sid }},
    );

    const entries = try querySessionEntries(&d, a, sid);
    defer SessionEntryRow.deinitMany(entries, a);

    try testing.expectEqual(@as(usize, 2), entries.len);
    // Ordered by ordinal asc.
    try testing.expectEqual(@as(i64, 1), entries[0].ordinal);
    try testing.expectEqualStrings("action", entries[0].prefix);
    try testing.expectEqualStrings("First entry", entries[0].body);
    try testing.expectEqual(@as(i64, 2), entries[1].ordinal);
    try testing.expectEqualStrings("observation", entries[1].prefix);
    // display_text includes prefix.
    try testing.expect(std.mem.indexOf(u8, entries[0].display_text, "action") != null);
    try testing.expect(std.mem.indexOf(u8, entries[0].display_text, "First entry") != null);
}

test "view_model: querySessionCommits returns commits for session (task 4034)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor, started_at) values ('claude', '2026-06-12T10:00:00.000Z')",
        &.{},
    );
    _ = try d.execParams(
        \\insert into session_commits (session_id, sha, subject, author, branch, committed_at)
        \\values (?, 'abc1234567890def', 'feat: add sessions view', 'Dev <dev@example.com>', 'feature/m11', '2026-06-12T11:00:00.000Z')
    , &.{.{ .int = sid }});
    _ = try d.execParams(
        \\insert into session_commits (session_id, sha, subject, committed_at)
        \\values (?, 'deadbeef11223344', 'fix: typo in render', '2026-06-12T12:00:00.000Z')
    , &.{.{ .int = sid }});

    const commits = try querySessionCommits(&d, a, sid);
    defer SessionCommitRow.deinitMany(commits, a);

    try testing.expectEqual(@as(usize, 2), commits.len);
    // Ordered by committed_at asc.
    try testing.expectEqualStrings("abc1234567890def", commits[0].sha);
    try testing.expect(commits[0].subject != null);
    try testing.expectEqualStrings("feat: add sessions view", commits[0].subject.?);
    try testing.expect(commits[0].author != null);
    try testing.expectEqualStrings("Dev <dev@example.com>", commits[0].author.?);
    try testing.expect(commits[0].branch != null);
    try testing.expectEqualStrings("feature/m11", commits[0].branch.?);
    // display_text: "sha_short subject"
    try testing.expect(std.mem.indexOf(u8, commits[0].display_text, "abc12345") != null);
    try testing.expect(std.mem.indexOf(u8, commits[0].display_text, "feat: add sessions view") != null);
    // Second commit.
    try testing.expectEqualStrings("deadbeef11223344", commits[1].sha);
    try testing.expect(std.mem.indexOf(u8, commits[1].display_text, "deadbeef") != null);
}

test "view_model: querySessionCommits returns empty for session with no commits (task 4034)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const sid = try d.execParams(
        "insert into sessions (vendor, started_at) values ('claude', '2026-06-13T10:00:00.000Z')",
        &.{},
    );

    const commits = try querySessionCommits(&d, a, sid);
    defer SessionCommitRow.deinitMany(commits, a);

    try testing.expectEqual(@as(usize, 0), commits.len);
}

test "view_model: queryHandoffs pending handoff with task next_action + snapshot IS resume-ready (task 4033)" {
    // Engine rule (resume.zig:300-336): READY iff tasks.next_action non-empty
    // AND a task-scoped context_snapshots row exists.  handoffs.status is NOT a
    // gate — a 'pending' handoff whose task satisfies the rule must be READY.
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    // Insert a task with next_action set.
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, next_action) values ('global', 'fix bug', 'Continue implementing the fix')",
        &.{},
    );
    const s1 = try d.execParams(
        "insert into sessions (vendor, started_at, task_id) values ('claude', '2026-06-10T08:00:00.000Z', ?)",
        &.{.{ .int = task_id }},
    );
    // Snapshot is task-scoped (task_id set).
    const snap_id = try d.execParams(
        "insert into context_snapshots (session_id, task_id, vendor, body) values (?, ?, 'claude', 'Summary body')",
        &.{ .{ .int = s1 }, .{ .int = task_id } },
    );
    // Status is 'pending' — the old rule would say NOT READY, the engine says READY.
    _ = try d.execParams(
        "insert into handoffs (from_snapshot_id, from_vendor, to_vendor, status) values (?, 'claude', 'codex', 'pending')",
        &.{.{ .int = snap_id }},
    );

    const handoffs = try queryHandoffs(&d, a);
    defer HandoffRow.deinitMany(handoffs, a);

    try testing.expectEqual(@as(usize, 1), handoffs.len);
    try testing.expectEqualStrings("claude", handoffs[0].from_vendor);
    try testing.expect(handoffs[0].to_vendor != null);
    try testing.expectEqualStrings("codex", handoffs[0].to_vendor.?);
    // Status is 'pending' — lifecycle info, not a readiness gate.
    try testing.expectEqualStrings("pending", handoffs[0].status);
    // Task-centric conditions must both be true.
    try testing.expect(handoffs[0].task_has_next_action);
    try testing.expect(handoffs[0].task_has_snapshot);
    // isResumeReady() must be true despite pending status.
    try testing.expect(handoffs[0].isResumeReady());
    // display_text must say "ready".
    try testing.expect(std.mem.indexOf(u8, handoffs[0].display_text, "ready") != null);
}

test "view_model: queryHandoffs consumed handoff with cleared task next_action is NOT resume-ready (task 4033)" {
    // Engine rule: tasks.next_action empty → NOT READY regardless of handoffs.status.
    // A 'consumed' handoff whose task's next_action was cleared is NOT resumable.
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    // Task with no next_action (cleared after consumption).
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title) values ('global', 'finished task')",
        &.{},
    );
    const s1 = try d.execParams(
        "insert into sessions (vendor, started_at, task_id) values ('claude', '2026-06-11T08:00:00.000Z', ?)",
        &.{.{ .int = task_id }},
    );
    const snap_id = try d.execParams(
        "insert into context_snapshots (session_id, task_id, vendor, body) values (?, ?, 'claude', 'Completed summary')",
        &.{ .{ .int = s1 }, .{ .int = task_id } },
    );
    // Status is 'consumed' — the old rule would say READY, the engine says NOT READY.
    _ = try d.execParams(
        "insert into handoffs (from_snapshot_id, from_vendor, status, consumed_at) values (?, 'claude', 'consumed', '2026-06-11T10:00:00.000Z')",
        &.{.{ .int = snap_id }},
    );

    const handoffs = try queryHandoffs(&d, a);
    defer HandoffRow.deinitMany(handoffs, a);

    try testing.expectEqual(@as(usize, 1), handoffs.len);
    try testing.expectEqualStrings("consumed", handoffs[0].status);
    // task_has_next_action must be false (task.next_action is null/empty).
    try testing.expect(!handoffs[0].task_has_next_action);
    // task_has_snapshot is true (snapshot exists), but that alone is not enough.
    try testing.expect(handoffs[0].task_has_snapshot);
    // isResumeReady() must be false despite consumed status.
    try testing.expect(!handoffs[0].isResumeReady());
    // display_text must say "not-ready".
    try testing.expect(std.mem.indexOf(u8, handoffs[0].display_text, "not-ready") != null);
}

test "view_model: queryHandoffs handoff with no task-scoped snapshot is NOT resume-ready (task 4033)" {
    // Engine rule: snapshot must exist scoped to the task — snapshot alone
    // without task_id set is not sufficient.
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    // Task with next_action — but snapshot not linked to the task.
    const task_id = try d.execParams(
        "insert into tasks (scope_kind, title, next_action) values ('global', 'needs snapshot', 'Do the thing')",
        &.{},
    );
    const s1 = try d.execParams(
        "insert into sessions (vendor, started_at, task_id) values ('claude', '2026-06-12T08:00:00.000Z', ?)",
        &.{.{ .int = task_id }},
    );
    // Snapshot has NO task_id — not task-scoped.
    const snap_id = try d.execParams(
        "insert into context_snapshots (session_id, vendor, body) values (?, 'claude', 'Partial body')",
        &.{.{ .int = s1 }},
    );
    _ = try d.execParams(
        "insert into handoffs (from_snapshot_id, from_vendor, status) values (?, 'claude', 'validated')",
        &.{.{ .int = snap_id }},
    );

    const handoffs = try queryHandoffs(&d, a);
    defer HandoffRow.deinitMany(handoffs, a);

    try testing.expectEqual(@as(usize, 1), handoffs.len);
    // The snapshot is not task-scoped, so the task join returns no task row;
    // task_has_next_action and task_has_snapshot are both false.
    try testing.expect(!handoffs[0].task_has_next_action);
    try testing.expect(!handoffs[0].task_has_snapshot);
    try testing.expect(!handoffs[0].isResumeReady());
    try testing.expect(std.mem.indexOf(u8, handoffs[0].display_text, "not-ready") != null);
}

test "view_model: queryHandoffs empty DB returns empty slice (task 4033)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    const handoffs = try queryHandoffs(&d, a);
    defer HandoffRow.deinitMany(handoffs, a);

    try testing.expectEqual(@as(usize, 0), handoffs.len);
}

test "view_model: querySessionsHandoffSnapshot combines both (task 4032/4033)" {
    const a = testing.allocator;
    var d = try setupTestDbSessions(a);
    defer d.close();

    _ = try d.execParams(
        "insert into sessions (vendor, started_at) values ('claude', '2026-06-12T08:00:00.000Z')",
        &.{},
    );

    var snap = try querySessionsHandoffSnapshot(&d, a);
    defer snap.deinit(a);

    try testing.expectEqual(@as(usize, 1), snap.sessions.len);
    try testing.expectEqual(@as(usize, 0), snap.handoffs.len);
}
