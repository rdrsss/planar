//! engine/runtime/resume — resume packet builder + validate.
//!
//! Mirrors `src/internal/resume/resume.go`. The packet is the 8-section
//! resume payload `planar resume <task-id>` returns:
//!
//!   1. identity      — task id, plan id, title, scope
//!   2. state         — status, next_action, last action
//!   3. plan position — parent plan + (completed, current, remaining)
//!   4. operational   — external_links for the task (M8 — skipped here
//!                      with a refresh note "operational plane not
//!                      available in M7 build")
//!   5. recent activity
//!   6. decisions and questions
//!   7. linked artifacts
//!   8. audit footer  — most recent session for the task
//!
//! `validate` is the lightweight resumability check used by
//! `planar resume validate` and `audit handoff-readiness`.

const std = @import("std");
const db = @import("db");
const session_mod = @import("session.zig");
const snapshot_mod = @import("snapshot.zig");
const extlink = @import("../external/link.zig");
const claim_store = @import("agentactivity/store.zig");
const claim_types = @import("agentactivity/types.zig");

// =========================================================================
// Types — JSON-stable (used in handler emission too)
// =========================================================================

pub const Identity = struct {
    task_id: i64,
    plan_id: ?i64 = null,
    title: []const u8,
    status: []const u8,
    scope_kind: []const u8,
    scope_id: ?i64 = null,
};

pub const State = struct {
    status: []const u8,
    next_action: []const u8,
    last_action_at: []const u8 = "",
    last_action_body: []const u8 = "",
};

pub const PlanStep = struct {
    ordinal: i64,
    body: []const u8,
    status: []const u8,
};

pub const PlanPosition = struct {
    plan_id: ?i64 = null,
    plan_title: []const u8 = "",
    completed: []const PlanStep,
    current: []const PlanStep,
    remaining: []const PlanStep,
};

pub const RecentEntry = struct {
    session_id: i64,
    prefix: []const u8,
    body: []const u8,
    created_at: []const u8,
};

pub const DecisionSummary = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

pub const QuestionSummary = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
    answer_body: []const u8 = "",
};

pub const ArtifactLink = struct {
    artifact_id: i64,
    title: []const u8,
    kind: []const u8,
    relationship: []const u8,
};

pub const ExternalLinkState = struct {
    link_id: i64,
    external_id: []const u8,
    external_url: []const u8 = "",
    remote_status: []const u8 = "",
    remote_assignee: []const u8 = "",
    last_synced_at: []const u8 = "",
    sync_status: []const u8,
    conflict: bool = false,
    refresh_error: []const u8 = "",
};

pub const OperationalPlane = struct {
    links: []const ExternalLinkState,
    refresh_note: []const u8 = "",
};

pub const Audit = struct {
    session_id: i64,
    vendor: []const u8,
    started_at: []const u8,
};

/// Worktree + locality state read from the active `agent_work_claims`
/// row when an exclusive claim is held on the task. The resumer's shell
/// wrapper inspects `worktree_path` to decide whether to prepend a
/// `cd <path>` directive before continuing. All string fields are owned
/// by the packet's allocator; empty string means "claim row had NULL".
pub const ActiveClaim = struct {
    claim_id: i64,
    claim_token: []const u8,
    vendor: []const u8,
    worktree_path: []const u8 = "",
    repo_root: []const u8 = "",
    branch: []const u8 = "",
};

pub const Packet = struct {
    identity: Identity,
    state: State,
    plan: PlanPosition,
    operational_plane: OperationalPlane,
    recent_activity: []const RecentEntry,
    decisions: []const DecisionSummary,
    questions: []const QuestionSummary,
    artifacts: []const ArtifactLink,
    audit: ?Audit = null,
    /// Active exclusive claim on this task, if any. Surfaces the
    /// worktree the prior session was running in so the resumer can
    /// `cd` there before continuing.
    active_claim: ?ActiveClaim = null,
};

pub fn deinitPacket(p: Packet, allocator: std.mem.Allocator) void {
    allocator.free(p.identity.title);
    allocator.free(p.identity.status);
    allocator.free(p.identity.scope_kind);
    allocator.free(p.state.status);
    allocator.free(p.state.next_action);
    if (p.state.last_action_at.len > 0) allocator.free(p.state.last_action_at);
    if (p.state.last_action_body.len > 0) allocator.free(p.state.last_action_body);
    if (p.plan.plan_title.len > 0) allocator.free(p.plan.plan_title);
    for ([_][]const PlanStep{ p.plan.completed, p.plan.current, p.plan.remaining }) |slice| {
        for (slice) |s| {
            allocator.free(s.body);
            allocator.free(s.status);
        }
        allocator.free(slice);
    }
    for (p.operational_plane.links) |l| {
        allocator.free(l.external_id);
        allocator.free(l.external_url);
        allocator.free(l.remote_status);
        allocator.free(l.remote_assignee);
        allocator.free(l.last_synced_at);
        allocator.free(l.sync_status);
        allocator.free(l.refresh_error);
    }
    allocator.free(p.operational_plane.links);
    allocator.free(p.operational_plane.refresh_note);
    for (p.recent_activity) |e| {
        allocator.free(e.prefix);
        allocator.free(e.body);
        allocator.free(e.created_at);
    }
    allocator.free(p.recent_activity);
    for (p.decisions) |d| {
        allocator.free(d.title);
        allocator.free(d.status);
    }
    allocator.free(p.decisions);
    for (p.questions) |q| {
        allocator.free(q.title);
        allocator.free(q.status);
        if (q.answer_body.len > 0) allocator.free(q.answer_body);
    }
    allocator.free(p.questions);
    for (p.artifacts) |a| {
        allocator.free(a.title);
        allocator.free(a.kind);
        allocator.free(a.relationship);
    }
    allocator.free(p.artifacts);
    if (p.audit) |au| {
        allocator.free(au.vendor);
        allocator.free(au.started_at);
    }
    if (p.active_claim) |ac| {
        allocator.free(ac.claim_token);
        allocator.free(ac.vendor);
        if (ac.worktree_path.len > 0) allocator.free(ac.worktree_path);
        if (ac.repo_root.len > 0) allocator.free(ac.repo_root);
        if (ac.branch.len > 0) allocator.free(ac.branch);
    }
}

pub const ValidationFailure = struct {
    check: []const u8,
    message: []const u8,
    remediation: []const u8,
};

pub const ValidationResult = struct {
    task_id: i64,
    resumable: bool,
    failures: []const ValidationFailure,
};

pub fn deinitResult(r: ValidationResult, allocator: std.mem.Allocator) void {
    for (r.failures) |f| {
        allocator.free(f.message);
        allocator.free(f.remediation);
    }
    allocator.free(r.failures);
}

pub const Error = error{
    NotFound,
    QueryFailed,
} || std.mem.Allocator.Error || session_mod.Error || snapshot_mod.Error;

// =========================================================================
// Validate
// =========================================================================

/// Validate task resumability:
///   - next_action is non-empty
///   - at least one context_snapshot row exists
pub fn validate(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error!ValidationResult {
    // Fetch task fields directly to keep this module independent of
    // engine.planning.task's deep struct + allocator policy.
    var task_status_buf: [64]u8 = undefined;
    var next_action: []const u8 = "";
    var na_owned: ?[]const u8 = null;
    defer if (na_owned) |buf| allocator.free(buf);

    {
        const sql: [:0]const u8 = "select coalesce(status,''), coalesce(next_action,'') from tasks where id = ?";
        var stmt = d.prepare(sql) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return Error.NotFound,
            .row => {
                const st = try stmt.columnTextAlloc(0, allocator);
                defer allocator.free(st);
                @memcpy(task_status_buf[0..@min(st.len, task_status_buf.len)], st[0..@min(st.len, task_status_buf.len)]);
                const na = try stmt.columnTextAlloc(1, allocator);
                na_owned = na;
                next_action = na;
            },
        }
    }

    var failures: std.ArrayList(ValidationFailure) = .empty;
    errdefer {
        for (failures.items) |f| {
            allocator.free(f.message);
            allocator.free(f.remediation);
        }
        failures.deinit(allocator);
    }

    if (next_action.len == 0) {
        const msg = try allocator.dupe(u8, "next_action is null");
        const rem = try std.fmt.allocPrint(
            allocator,
            "planar task update {d} --next-action \"<text>\"",
            .{task_id},
        );
        try failures.append(allocator, .{
            .check = "next_action",
            .message = msg,
            .remediation = rem,
        });
    }

    const latest = try snapshot_mod.getLatestForTask(d, allocator, task_id);
    if (latest) |s| {
        snapshot_mod.deinit(s, allocator);
    } else {
        const msg = try allocator.dupe(u8, "no context snapshot found");
        const rem = try std.fmt.allocPrint(
            allocator,
            "planar capture snapshot --task {d}",
            .{task_id},
        );
        try failures.append(allocator, .{
            .check = "snapshot",
            .message = msg,
            .remediation = rem,
        });
    }

    const failure_slice = try failures.toOwnedSlice(allocator);
    return .{
        .task_id = task_id,
        .resumable = failure_slice.len == 0,
        .failures = failure_slice,
    };
}

// =========================================================================
// buildPacket
// =========================================================================

/// Assemble the full resume packet for task `task_id`. Section 4 loads
/// the external_links rows whose entity is this task and surfaces them
/// as ExternalLinkState entries on the operational plane.
pub fn buildPacket(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error!Packet {
    // ---- Section 1+2 — Identity + State -------------------------------
    var identity: Identity = undefined;
    var state: State = undefined;

    {
        const sql: [:0]const u8 =
            \\select id, plan_id, coalesce(title, ''), coalesce(status, ''),
            \\       coalesce(scope_kind, 'global'), scope_id, coalesce(next_action, '')
            \\from tasks where id = ?
        ;
        var stmt = d.prepare(sql) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return Error.NotFound,
            .row => {
                const tid = stmt.columnInt(0);
                const plan_id = stmt.columnIntOpt(1);
                const title = try stmt.columnTextAlloc(2, allocator);
                const status_str = try stmt.columnTextAlloc(3, allocator);
                const scope_kind = try stmt.columnTextAlloc(4, allocator);
                const scope_id = stmt.columnIntOpt(5);
                const next_action = try stmt.columnTextAlloc(6, allocator);

                identity = .{
                    .task_id = tid,
                    .plan_id = plan_id,
                    .title = title,
                    .status = try allocator.dupe(u8, status_str),
                    .scope_kind = scope_kind,
                    .scope_id = scope_id,
                };
                state = .{
                    .status = status_str,
                    .next_action = next_action,
                };
            },
        }
    }

    // ---- Section 3 — Plan position ------------------------------------
    const plan_pos = try buildPlanPosition(d, allocator, identity.plan_id);

    // ---- Section 4 — Operational plane --------------------------------
    const op_plane = try buildOperationalPlane(d, allocator, task_id);

    // ---- Section 5 — Recent activity ----------------------------------
    const entries = try session_mod.recentEntriesForTask(d, allocator, task_id, 50);
    defer session_mod.deinitEntries(entries, allocator);

    var recent: std.ArrayList(RecentEntry) = .empty;
    errdefer {
        for (recent.items) |e| {
            allocator.free(e.prefix);
            allocator.free(e.body);
            allocator.free(e.created_at);
        }
        recent.deinit(allocator);
    }
    for (entries) |e| {
        try recent.append(allocator, .{
            .session_id = e.session_id,
            .prefix = try allocator.dupe(u8, e.prefix),
            .body = try allocator.dupe(u8, e.body),
            .created_at = try allocator.dupe(u8, e.created_at),
        });
    }
    if (entries.len > 0) {
        const last = entries[0];
        state.last_action_at = try allocator.dupe(u8, last.created_at);
        state.last_action_body = try allocator.dupe(u8, last.body);
    }

    // ---- Section 6 — Decisions + questions ----------------------------
    const decisions = try buildDecisions(d, allocator, task_id);
    errdefer {
        for (decisions) |de| {
            allocator.free(de.title);
            allocator.free(de.status);
        }
        allocator.free(decisions);
    }
    const questions = try buildQuestions(d, allocator, task_id);

    // ---- Section 7 — Artifacts ---------------------------------------
    const artifacts = try buildArtifacts(d, allocator, task_id);

    // ---- Section 8 — Audit footer ------------------------------------
    var audit_opt: ?Audit = null;
    const sessions = try session_mod.recentSessionsForTask(d, allocator, task_id, 1);
    defer session_mod.deinitMany(sessions, allocator);
    if (sessions.len > 0) {
        audit_opt = .{
            .session_id = sessions[0].id,
            .vendor = try allocator.dupe(u8, sessions[0].vendor),
            .started_at = try allocator.dupe(u8, sessions[0].started_at),
        };
    }

    // ---- Active claim — surfaces worktree state for cd-prefix ---------
    const active_claim_opt = try buildActiveClaim(d, allocator, task_id);

    // Take ownership of the recent slice.
    const recent_slice = try recent.toOwnedSlice(allocator);

    return .{
        .identity = identity,
        .state = state,
        .plan = plan_pos,
        .operational_plane = op_plane,
        .recent_activity = recent_slice,
        .decisions = decisions,
        .questions = questions,
        .artifacts = artifacts,
        .audit = audit_opt,
        .active_claim = active_claim_opt,
    };
}

/// Return the active exclusive claim on this task (most recent), or
/// null when no active claim exists. Surfaces the worktree path the
/// prior session was operating in.
fn buildActiveClaim(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error!?ActiveClaim {
    const rows = claim_store.listByEntity(d, allocator, .task, task_id) catch |e| switch (e) {
        error.OutOfMemory => return error.OutOfMemory,
        else => return Error.QueryFailed,
    };
    defer claim_types.Claim.deinitMany(rows, allocator);

    // listByEntity orders by claimed_at desc; pick the first active row.
    for (rows) |c| {
        if (c.status != .active) continue;
        return .{
            .claim_id = c.id,
            .claim_token = try allocator.dupe(u8, c.claim_token),
            .vendor = try allocator.dupe(u8, c.vendor),
            .worktree_path = if (c.worktree_path) |p|
                try allocator.dupe(u8, p)
            else
                "",
            .repo_root = if (c.repo_root) |p|
                try allocator.dupe(u8, p)
            else
                "",
            .branch = if (c.branch) |b|
                try allocator.dupe(u8, b)
            else
                "",
        };
    }
    return null;
}

fn buildOperationalPlane(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error!OperationalPlane {
    const links = extlink.linksForEntity(d, allocator, .task, task_id) catch |e| switch (e) {
        error.OutOfMemory => return error.OutOfMemory,
        else => return Error.QueryFailed,
    };
    defer extlink.deinitMany(links, allocator);

    var out: std.ArrayList(ExternalLinkState) = .empty;
    errdefer {
        for (out.items) |item| {
            allocator.free(item.external_id);
            allocator.free(item.external_url);
            allocator.free(item.remote_status);
            allocator.free(item.remote_assignee);
            allocator.free(item.last_synced_at);
            allocator.free(item.sync_status);
            allocator.free(item.refresh_error);
        }
        out.deinit(allocator);
    }

    for (links) |l| {
        try out.append(allocator, .{
            .link_id = l.id,
            .external_id = try allocator.dupe(u8, l.external_id),
            .external_url = try allocator.dupe(u8, l.external_url orelse ""),
            .remote_status = try allocator.dupe(u8, ""),
            .remote_assignee = try allocator.dupe(u8, ""),
            .last_synced_at = try allocator.dupe(u8, l.last_synced_at orelse ""),
            .sync_status = try allocator.dupe(u8, l.last_sync_status.toText()),
            .conflict = l.last_sync_status == .conflict,
            .refresh_error = try allocator.dupe(u8, ""),
        });
    }

    return .{
        .links = try out.toOwnedSlice(allocator),
        .refresh_note = try allocator.dupe(u8, ""),
    };
}

fn buildPlanPosition(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id_opt: ?i64,
) Error!PlanPosition {
    if (plan_id_opt == null) {
        return .{
            .plan_id = null,
            .plan_title = "",
            .completed = try allocator.alloc(PlanStep, 0),
            .current = try allocator.alloc(PlanStep, 0),
            .remaining = try allocator.alloc(PlanStep, 0),
        };
    }
    const plan_id = plan_id_opt.?;

    var plan_title: []const u8 = "";
    {
        const sql: [:0]const u8 = "select coalesce(title,'') from plans where id = ?";
        var stmt = d.prepare(sql) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => {
                return .{
                    .plan_id = plan_id,
                    .plan_title = "",
                    .completed = try allocator.alloc(PlanStep, 0),
                    .current = try allocator.alloc(PlanStep, 0),
                    .remaining = try allocator.alloc(PlanStep, 0),
                };
            },
            .row => plan_title = try stmt.columnTextAlloc(0, allocator),
        }
    }
    errdefer if (plan_title.len > 0) allocator.free(plan_title);

    var completed: std.ArrayList(PlanStep) = .empty;
    var current: std.ArrayList(PlanStep) = .empty;
    var remaining: std.ArrayList(PlanStep) = .empty;
    errdefer {
        for ([_]std.ArrayList(PlanStep){ completed, current, remaining }) |lst| {
            for (lst.items) |s| {
                allocator.free(s.body);
                allocator.free(s.status);
            }
        }
        completed.deinit(allocator);
        current.deinit(allocator);
        remaining.deinit(allocator);
    }

    const sql: [:0]const u8 =
        "select ordinal, coalesce(body, ''), coalesce(status, 'pending') " ++
        "from plan_steps where plan_id = ? order by ordinal";
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const ord = stmt.columnInt(0);
                const body = try stmt.columnTextAlloc(1, allocator);
                const st = try stmt.columnTextAlloc(2, allocator);
                const step: PlanStep = .{ .ordinal = ord, .body = body, .status = st };
                if (std.mem.eql(u8, st, "done") or std.mem.eql(u8, st, "skipped")) {
                    try completed.append(allocator, step);
                } else if (std.mem.eql(u8, st, "in-progress")) {
                    try current.append(allocator, step);
                } else {
                    try remaining.append(allocator, step);
                }
            },
        }
    }

    return .{
        .plan_id = plan_id,
        .plan_title = plan_title,
        .completed = try completed.toOwnedSlice(allocator),
        .current = try current.toOwnedSlice(allocator),
        .remaining = try remaining.toOwnedSlice(allocator),
    };
}

fn buildDecisions(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error![]DecisionSummary {
    const sql: [:0]const u8 =
        \\select d.id, coalesce(d.title, ''), coalesce(d.status, '')
        \\from decisions d
        \\join sessions s on s.id = d.session_id
        \\where s.task_id = ?
        \\order by d.id
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(DecisionSummary) = .empty;
    errdefer {
        for (out.items) |x| {
            allocator.free(x.title);
            allocator.free(x.status);
        }
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .title = try stmt.columnTextAlloc(1, allocator),
                .status = try stmt.columnTextAlloc(2, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn buildQuestions(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error![]QuestionSummary {
    const sql: [:0]const u8 =
        \\select distinct q.id, coalesce(q.title, ''), coalesce(q.status, ''),
        \\       coalesce(q.answer_body, '')
        \\from questions q
        \\join entity_links el
        \\  on (el.from_kind = 'task' and el.from_id = ? and el.to_kind = 'question' and el.to_id = q.id)
        \\  or (el.to_kind = 'task'   and el.to_id   = ? and el.from_kind = 'question' and el.from_id = q.id)
        \\order by q.id
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = task_id }, .{ .int = task_id } }) catch return Error.QueryFailed;

    var out: std.ArrayList(QuestionSummary) = .empty;
    errdefer {
        for (out.items) |x| {
            allocator.free(x.title);
            allocator.free(x.status);
            if (x.answer_body.len > 0) allocator.free(x.answer_body);
        }
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const ans = try stmt.columnTextAlloc(3, allocator);
                try out.append(allocator, .{
                    .id = stmt.columnInt(0),
                    .title = try stmt.columnTextAlloc(1, allocator),
                    .status = try stmt.columnTextAlloc(2, allocator),
                    .answer_body = ans,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn buildArtifacts(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error![]ArtifactLink {
    const sql: [:0]const u8 =
        \\select a.id, coalesce(a.title, ''), coalesce(a.kind, ''),
        \\       coalesce(el.relationship, '')
        \\from artifacts a
        \\join entity_links el on el.to_kind = 'artifact' and el.to_id = a.id
        \\where el.from_kind = 'task' and el.from_id = ?
        \\order by a.id
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(ArtifactLink) = .empty;
    errdefer {
        for (out.items) |x| {
            allocator.free(x.title);
            allocator.free(x.kind);
            allocator.free(x.relationship);
        }
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .artifact_id = stmt.columnInt(0),
                .title = try stmt.columnTextAlloc(1, allocator),
                .kind = try stmt.columnTextAlloc(2, allocator),
                .relationship = try stmt.columnTextAlloc(3, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
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

test "validate fails when next_action and snapshot are missing" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global', 't', 'todo')",
        &.{},
    );
    const r = try validate(&d, a, tid);
    defer deinitResult(r, a);
    try std.testing.expect(!r.resumable);
    try std.testing.expectEqual(@as(usize, 2), r.failures.len);
}

test "validate succeeds when next_action set + snapshot exists" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status, next_action) values ('global', 't', 'todo', 'do it')",
        &.{},
    );
    const sess = try session_mod.startSession(&d, a, .{ .vendor = "v", .task_id = tid });
    defer session_mod.deinit(sess, a);
    const snap = try snapshot_mod.create(&d, a, .{
        .session_id = sess.id,
        .task_id = tid,
        .vendor = "v",
        .next_action = "do it",
    });
    defer snapshot_mod.deinit(snap, a);

    const r = try validate(&d, a, tid);
    defer deinitResult(r, a);
    try std.testing.expect(r.resumable);
    try std.testing.expectEqual(@as(usize, 0), r.failures.len);
}

test "validate returns NotFound on unknown task" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.NotFound, validate(&d, a, 9999));
}

test "buildPacket assembles identity + recent activity + audit footer" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status, next_action) values ('global', 'the title', 'doing', 'next')",
        &.{},
    );
    const sess = try session_mod.startSession(&d, a, .{ .vendor = "claude", .task_id = tid });
    defer session_mod.deinit(sess, a);
    try session_mod.appendEntry(&d, sess.id, "note", "hello");

    const p = try buildPacket(&d, a, tid);
    defer deinitPacket(p, a);
    try std.testing.expectEqualStrings("the title", p.identity.title);
    try std.testing.expectEqualStrings("doing", p.state.status);
    try std.testing.expectEqualStrings("next", p.state.next_action);
    try std.testing.expectEqual(@as(usize, 1), p.recent_activity.len);
    try std.testing.expect(p.audit != null);
    try std.testing.expectEqualStrings("claude", p.audit.?.vendor);
}

test "buildPacket includes plan position when plan exists" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try d.execParams(
        "insert into plans (scope_kind, slug, title, status) values ('global', 'plan-x', 'plan x', 'active')",
        &.{},
    );
    _ = try d.execParams(
        "insert into plan_steps (plan_id, ordinal, body, status) values (?, 1, 'step1', 'done')",
        &.{.{ .int = pid }},
    );
    _ = try d.execParams(
        "insert into plan_steps (plan_id, ordinal, body, status) values (?, 2, 'step2', 'pending')",
        &.{.{ .int = pid }},
    );
    const tid = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 't', 'todo')",
        &.{.{ .int = pid }},
    );
    const p = try buildPacket(&d, a, tid);
    defer deinitPacket(p, a);
    try std.testing.expectEqual(pid, p.plan.plan_id.?);
    try std.testing.expectEqualStrings("plan x", p.plan.plan_title);
    try std.testing.expectEqual(@as(usize, 1), p.plan.completed.len);
    try std.testing.expectEqual(@as(usize, 1), p.plan.remaining.len);
}
