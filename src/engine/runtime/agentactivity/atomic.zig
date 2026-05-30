//! agentactivity/atomic — multi-table atomic operation wrappers.
//!
//! These wrap one `BEGIN IMMEDIATE` transaction around (claim lifecycle
//! + action lifecycle + tasks.status update) so the agent's "pull →
//! work → complete/fail/release/block" ritual stays atomic. Each
//! wrapper calls the single-table primitives from `store.zig` rather
//! than duplicating the SQL.
//!
//! The status-transition guard (`policy.status.check`) MUST be
//! consulted before each `UPDATE tasks SET status`. On
//! `IllegalTransition` the entire transaction rolls back and the claim
//! keeps its previous state — this prevents `complete` on an already-
//! done task from creating a half-released claim.
//!
//! Why `BEGIN IMMEDIATE`: the "check no active claim, then insert new
//! claim" pair (in store.acquireClaim) is only safe with the writer
//! lock held from the start of the transaction. `BEGIN IMMEDIATE`
//! takes the writer lock immediately; `BEGIN` (deferred) waits until
//! the first write, which leaves a window for a concurrent process to
//! sneak its own claim in. The cross-process concurrency integration
//! test exercises this exact race.

const std = @import("std");
const db = @import("db");
const types = @import("types.zig");
const store = @import("store.zig");
const policy = @import("../../policy.zig");

pub const Error = store.Error || policy.status.Error || error{
    NoEligibleTask,
    TaskNotFound,
    /// `complete`/`fail`/etc. called against a claim that isn't on a
    /// task (it's on a plan or plan_step). The caller wired the wrong
    /// verb.
    ClaimNotOnTask,
};

/// Result of `pullNext`. `no_work=true` ⇒ all other fields zeroed.
pub const PullResult = struct {
    no_work: bool = false,
    claim: ?types.Claim = null,
    task_id: i64 = 0,
    action_id: i64 = 0,

    pub fn deinit(self: PullResult, allocator: std.mem.Allocator) void {
        if (self.claim) |c| c.deinit(allocator);
    }
};

pub const PullArgs = struct {
    plan_id: i64,
    session_id: i64,
    vendor: []const u8,
    vendor_session_id: ?[]const u8 = null,
    role: ?[]const u8 = null,
    model: ?[]const u8 = null,
    worktree_id: ?i64 = null,
    worktree_path: ?[]const u8 = null,
    purpose: ?[]const u8 = null,
    base_ref: ?[]const u8 = null,
    ttl_secs: i64 = 600,
    locality: types.Locality = .{},
    /// Role the action_kind defaults to. Coder is the typical case;
    /// the CLI surfaces a --role flag mapping to ActionKind.
    action_kind: types.ActionKind = .coder,
    /// Optional opaque text persisted on the dispatch action row
    /// (`agent_actions.metadata`). The orchestrator strategy gate
    /// writes the chosen strategy + axes blob here so the next cycle
    /// can read it back.
    metadata: ?[]const u8 = null,
    /// Optional parent action id. When the orchestrator dispatches a
    /// coder sub-agent and wants the dispatch to appear as a child of
    /// its own action in `planar-watch tree`, it passes the
    /// orchestrator's own action id here. The resulting action row's
    /// `parent_action_id` is set to this value, establishing the
    /// cross-session hierarchy edge that `walkTree` relies on.
    /// When null (default), the new action is a root (no parent).
    parent_action_id: ?i64 = null,
};

/// Atomic pull: pick the next eligible task (highest-priority todo
/// with no active claim) belonging to plan_id, claim it exclusively,
/// flip the task to `doing`, and insert a fresh action row.
pub fn pullNext(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: PullArgs,
) Error!PullResult {
    try beginImmediate(d);
    var committed = false;
    errdefer {
        if (!committed) rollback(d);
    }

    // Pick the next eligible task: plan matches, status in {todo,
    // doing}, no active unexpired claim. The `doing` branch is the
    // plan 493 F1 liveness path — a task whose previous worker
    // crashed is re-pullable. The exclusivity check also lives
    // inside store.acquireClaim, so this query is the candidate
    // filter, not a contract.
    const task_id = pickNextEligible(d, args.plan_id) catch |e| switch (e) {
        store.Error.QueryFailed => {
            rollback(d);
            committed = true; // already rolled back, suppress errdefer
            return e;
        },
        else => return e,
    } orelse {
        try commit(d);
        committed = true;
        return .{ .no_work = true };
    };

    // Status guard. Read the actual current status so the policy
    // check matches reality — when the predicate selected a `doing`
    // task (post-crash liveness path), this is a `doing → doing`
    // identity transition; otherwise it's the normal `todo → doing`.
    const current_pull_status = currentTaskStatus(d, allocator, task_id) catch |e| return e;
    defer allocator.free(current_pull_status);
    policy.status.check(.task, current_pull_status, "doing") catch |e| {
        rollback(d);
        committed = true;
        return e;
    };

    var claim = store.acquireClaim(d, allocator, .{
        .session_id = args.session_id,
        .entity_kind = .task,
        .entity_id = task_id,
        .vendor = args.vendor,
        .vendor_session_id = args.vendor_session_id,
        .role = args.role,
        .model = args.model,
        .worktree_id = args.worktree_id,
        .worktree_path = args.worktree_path,
        .purpose = args.purpose,
        .base_ref = args.base_ref,
        .ttl_secs = args.ttl_secs,
        .locality = args.locality,
    }) catch |e| return e;
    errdefer claim.deinit(allocator);

    // Flip task status. We already ran the guard above.
    _ = d.execParams(
        "update tasks set status = 'doing', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?",
        &.{.{ .int = task_id }},
    ) catch {
        return store.Error.QueryFailed;
    };

    const action_id = store.startAction(d, allocator, .{
        .session_id = args.session_id,
        .parent_action_id = args.parent_action_id,
        .claim_id = claim.id,
        .action_kind = args.action_kind,
        .entity_kind = .task,
        .entity_id = task_id,
        .vendor = args.vendor,
        .vendor_role = args.role,
        .model = args.model,
        .locality = args.locality,
        .metadata = args.metadata,
    }) catch |e| return e;

    try commit(d);
    committed = true;

    return .{
        .no_work = false,
        .claim = claim,
        .task_id = task_id,
        .action_id = action_id,
    };
}

pub const PeekResult = struct {
    no_work: bool = false,
    task_id: i64 = 0,
};

/// Read-only "what's next" — same query as pullNext step 1, no writes.
pub fn peekNext(
    d: *db.sqlite.Db,
    plan_id: i64,
) Error!PeekResult {
    const task_id = (try pickNextEligible(d, plan_id)) orelse return .{ .no_work = true };
    return .{ .no_work = false, .task_id = task_id };
}

pub const CompleteResult = struct {
    claim: types.Claim,
    task_id: i64,
    action_id: ?i64 = null,

    pub fn deinit(self: CompleteResult, allocator: std.mem.Allocator) void {
        self.claim.deinit(allocator);
    }
};

pub fn completeWork(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    claim_token: []const u8,
    summary: ?[]const u8,
) Error!CompleteResult {
    return try terminalTransition(d, allocator, .{
        .claim_token = claim_token,
        .summary = summary,
        .task_to = "done",
        .claim_to = .completed,
        .outcome = .ok,
        .reason = null,
    });
}

pub fn failWork(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    claim_token: []const u8,
    reason: []const u8,
) Error!CompleteResult {
    return try terminalTransition(d, allocator, .{
        .claim_token = claim_token,
        .summary = null,
        .task_to = "todo",
        .claim_to = .aborted,
        .outcome = .@"error",
        .reason = reason,
    });
}

pub fn releaseWork(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    claim_token: []const u8,
    reason: ?[]const u8,
) Error!CompleteResult {
    return try terminalTransition(d, allocator, .{
        .claim_token = claim_token,
        .summary = null,
        .task_to = "todo",
        .claim_to = .released,
        .outcome = .aborted,
        .reason = reason,
    });
}

pub const BlockResult = CompleteResult;

pub fn blockWork(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    claim_token: []const u8,
    blocker_task_id: i64,
    reason: ?[]const u8,
) Error!BlockResult {
    try beginImmediate(d);
    var committed = false;
    errdefer {
        if (!committed) rollback(d);
    }

    var claim = store.getClaimByToken(d, allocator, claim_token) catch |e| return e;
    errdefer claim.deinit(allocator);

    if (claim.entity_kind != .task) return Error.ClaimNotOnTask;

    // Status guard: must allow * → blocked. Read current then check.
    const current_status = currentTaskStatus(d, allocator, claim.entity_id) catch |e| return e;
    defer allocator.free(current_status);
    policy.status.check(.task, current_status, "blocked") catch |e| {
        rollback(d);
        committed = true;
        return e;
    };

    // entity_links blocker edge.
    _ = d.execParams(
        "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', ?, 'task', ?, 'blocks')",
        &.{ .{ .int = blocker_task_id }, .{ .int = claim.entity_id } },
    ) catch return store.Error.QueryFailed;

    _ = d.execParams(
        "update tasks set status = 'blocked', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?",
        &.{.{ .int = claim.entity_id }},
    ) catch return store.Error.QueryFailed;

    // Close active action(s) for this claim.
    closeOpenActionForClaim(d, claim.id, .aborted, reason) catch |e| return e;

    // Release the claim.
    var released = store.releaseClaim(d, allocator, claim_token, .released, reason) catch |e| return e;
    errdefer released.deinit(allocator);

    try commit(d);
    committed = true;

    const task_id = claim.entity_id;
    claim.deinit(allocator);
    return .{ .claim = released, .task_id = task_id };
}

// =========================================================================
// Internals
// =========================================================================

const TerminalArgs = struct {
    claim_token: []const u8,
    summary: ?[]const u8,
    /// New `tasks.status` value: e.g. "done", "todo".
    task_to: []const u8,
    claim_to: types.ClaimStatus,
    outcome: types.Outcome,
    reason: ?[]const u8,
};

fn terminalTransition(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    targs: TerminalArgs,
) Error!CompleteResult {
    try beginImmediate(d);
    var committed = false;
    errdefer {
        if (!committed) rollback(d);
    }

    var claim = store.getClaimByToken(d, allocator, targs.claim_token) catch |e| return e;
    errdefer claim.deinit(allocator);

    if (claim.status != .active) {
        rollback(d);
        committed = true;
        return store.Error.ClaimNotActive;
    }
    if (claim.entity_kind != .task) return Error.ClaimNotOnTask;

    // Status guard on the task transition. We read CURRENT first so the
    // guard matches the actual database state.
    const current_status = currentTaskStatus(d, allocator, claim.entity_id) catch |e| return e;
    defer allocator.free(current_status);
    policy.status.check(.task, current_status, targs.task_to) catch |e| {
        rollback(d);
        committed = true;
        return e;
    };

    _ = d.execParams(
        "update tasks set status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?",
        &.{ .{ .text = targs.task_to }, .{ .int = claim.entity_id } },
    ) catch return store.Error.QueryFailed;

    closeOpenActionForClaim(d, claim.id, targs.outcome, targs.summary) catch |e| return e;

    var released = store.releaseClaim(d, allocator, targs.claim_token, targs.claim_to, targs.reason) catch |e| return e;
    errdefer released.deinit(allocator);

    try commit(d);
    committed = true;

    const task_id = claim.entity_id;
    claim.deinit(allocator);
    return .{ .claim = released, .task_id = task_id };
}

/// Find the next eligible task on the requested plan. A task is
/// eligible when (a) it has no active unexpired claim AND (b) its
/// status is either `todo` (the normal case) or `doing` (the
/// claim-less liveness case — its previous worker crashed and the
/// lease lapsed).
///
/// The `doing`-no-active-claim branch is plan 493 F1 (tech-spec
/// artifact 265, methodology.md L100: "a stale claim no longer
/// blocks pull"). Without it, a task whose worker crashed stays
/// `doing` forever (reconcile only mutates claim rows, never
/// `tasks.status`) and is unreachable from `pull`/`peek` until an
/// operator manually flips it back to `todo`.
///
/// The "no active unexpired claim" predicate matches the existing
/// `nextWork` predicate (status = 'active' AND lease_expires_at >=
/// now) so the selector and the operator viewer agree by
/// construction — every task `nextWork` surfaces in its
/// `available` or `stale` bucket is selectable by `pull`. Returns
/// null when no candidate exists.
fn pickNextEligible(d: *db.sqlite.Db, plan_id: i64) store.Error!?i64 {
    var stmt = d.prepare(
        \\select t.id from tasks t
        \\where t.plan_id = ?
        \\  and t.status in ('todo', 'doing')
        \\  and not exists (
        \\    select 1 from agent_work_claims c
        \\    where c.entity_kind = 'task'
        \\      and c.entity_id = t.id
        \\      and c.status = 'active'
        \\      and c.lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\  )
        \\order by t.priority asc, t.id asc
        \\limit 1
    ) catch return store.Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return store.Error.QueryFailed;
    return switch (stmt.step() catch return store.Error.QueryFailed) {
        .done => null,
        .row => stmt.columnInt(0),
    };
}

fn currentTaskStatus(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
) Error![]u8 {
    var stmt = d.prepare("select status from tasks where id = ?") catch return store.Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return store.Error.QueryFailed;
    switch (stmt.step() catch return store.Error.QueryFailed) {
        .done => return Error.TaskNotFound,
        .row => {
            const text = try stmt.columnTextAlloc(0, allocator);
            // columnTextAlloc returns []const u8; we need a mutable
            // owner for the defer free path. Duplicate.
            const owned = try allocator.dupe(u8, text);
            allocator.free(text);
            return owned;
        },
    }
}

/// Close every still-open action attached to this claim with the given
/// outcome + optional summary. Most claims have exactly one open
/// action (started by pull) so this is a tight loop in practice.
fn closeOpenActionForClaim(
    d: *db.sqlite.Db,
    claim_id: i64,
    outcome: types.Outcome,
    summary: ?[]const u8,
) Error!void {
    _ = d.execParams(
        \\update agent_actions
        \\set ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'),
        \\    outcome = ?,
        \\    summary = coalesce(?, summary)
        \\where claim_id = ? and ended_at is null
    , &.{
        .{ .text = outcome.toText() },
        if (summary) |s| .{ .text = s } else .{ .null = {} },
        .{ .int = claim_id },
    }) catch return store.Error.QueryFailed;
}

fn beginImmediate(d: *db.sqlite.Db) store.Error!void {
    d.exec("BEGIN IMMEDIATE") catch return store.Error.QueryFailed;
}

fn commit(d: *db.sqlite.Db) store.Error!void {
    d.exec("COMMIT") catch return store.Error.QueryFailed;
}

fn rollback(d: *db.sqlite.Db) void {
    // Best-effort — failure to rollback while already in error path is
    // unrecoverable; log via std.log but don't shadow the original error.
    d.exec("ROLLBACK") catch |e| {
        std.log.warn("agentactivity.atomic: rollback failed: {s}", .{@errorName(e)});
    };
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

fn newSessionAndPlan(d: *db.sqlite.Db) !struct { sid: i64, pid: i64 } {
    const sid = try d.execParams("insert into sessions (vendor) values ('test')", &.{});
    const pid = try d.execParams(
        "insert into plans (scope_kind, title, slug) values ('global','p','test-plan')",
        &.{},
    );
    return .{ .sid = sid, .pid = pid };
}

test "pullNext happy path claims task, flips status, inserts action" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    const tid = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 't', 'todo', 100)",
        &.{.{ .int = sp.pid }},
    );

    const res = try pullNext(&d, a, .{
        .plan_id = sp.pid,
        .session_id = sp.sid,
        .vendor = "test",
    });
    defer res.deinit(a);

    try std.testing.expect(!res.no_work);
    try std.testing.expectEqual(tid, res.task_id);
    try std.testing.expect(res.claim != null);
    try std.testing.expect(res.action_id != 0);

    const status = try d.intQuery("select count(*) from tasks where id = (select max(id) from tasks) and status = 'doing'");
    try std.testing.expectEqual(@as(i64, 1), status);
}

test "pullNext returns no_work when nothing eligible" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    const res = try pullNext(&d, a, .{
        .plan_id = sp.pid,
        .session_id = sp.sid,
        .vendor = "test",
    });
    defer res.deinit(a);
    try std.testing.expect(res.no_work);
    try std.testing.expect(res.claim == null);
}

test "pullNext respects existing claim — second call gets no_work" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 't', 'todo')",
        &.{.{ .int = sp.pid }},
    );

    const r1 = try pullNext(&d, a, .{
        .plan_id = sp.pid,
        .session_id = sp.sid,
        .vendor = "test",
    });
    defer r1.deinit(a);
    try std.testing.expect(!r1.no_work);

    const r2 = try pullNext(&d, a, .{
        .plan_id = sp.pid,
        .session_id = sp.sid,
        .vendor = "test",
    });
    defer r2.deinit(a);
    try std.testing.expect(r2.no_work);
}

test "completeWork transitions claim to completed and task to done" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 't', 'todo')",
        &.{.{ .int = sp.pid }},
    );

    const pulled = try pullNext(&d, a, .{
        .plan_id = sp.pid,
        .session_id = sp.sid,
        .vendor = "test",
    });
    defer pulled.deinit(a);

    const done = try completeWork(&d, a, pulled.claim.?.claim_token, "shipped");
    defer done.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.completed, done.claim.status);

    const ndone = try d.intQuery("select count(*) from tasks where status = 'done'");
    try std.testing.expectEqual(@as(i64, 1), ndone);
}

test "failWork sends task back to todo and aborts claim" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 't', 'todo')",
        &.{.{ .int = sp.pid }},
    );

    const pulled = try pullNext(&d, a, .{
        .plan_id = sp.pid,
        .session_id = sp.sid,
        .vendor = "test",
    });
    defer pulled.deinit(a);

    const failed = try failWork(&d, a, pulled.claim.?.claim_token, "broke");
    defer failed.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.aborted, failed.claim.status);

    const todo_count = try d.intQuery("select count(*) from tasks where status = 'todo'");
    try std.testing.expectEqual(@as(i64, 1), todo_count);
}

test "releaseWork sends task back to todo and releases claim" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 't', 'todo')",
        &.{.{ .int = sp.pid }},
    );

    const pulled = try pullNext(&d, a, .{
        .plan_id = sp.pid,
        .session_id = sp.sid,
        .vendor = "test",
    });
    defer pulled.deinit(a);

    const released = try releaseWork(&d, a, pulled.claim.?.claim_token, "stepped back");
    defer released.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.released, released.claim.status);
}

test "blockWork creates entity_links edge, flips task to blocked, releases claim" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 't', 'todo')",
        &.{.{ .int = sp.pid }},
    );
    const blocker = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','blocker','todo')",
        &.{},
    );

    const pulled = try pullNext(&d, a, .{
        .plan_id = sp.pid,
        .session_id = sp.sid,
        .vendor = "test",
    });
    defer pulled.deinit(a);

    const blocked = try blockWork(&d, a, pulled.claim.?.claim_token, blocker, "waiting");
    defer blocked.deinit(a);

    const blk_count = try d.intQuery("select count(*) from tasks where status = 'blocked'");
    try std.testing.expectEqual(@as(i64, 1), blk_count);
    const link_count = try d.intQuery(
        "select count(*) from entity_links where relationship = 'blocks'",
    );
    try std.testing.expectEqual(@as(i64, 1), link_count);
}

test "completeWork on a non-active claim refuses with ClaimNotActive" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    _ = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 't', 'todo')",
        &.{.{ .int = sp.pid }},
    );
    const pulled = try pullNext(&d, a, .{
        .plan_id = sp.pid,
        .session_id = sp.sid,
        .vendor = "test",
    });
    defer pulled.deinit(a);
    // Complete once — second complete should error.
    const r1 = try completeWork(&d, a, pulled.claim.?.claim_token, null);
    r1.deinit(a);
    try std.testing.expectError(
        store.Error.ClaimNotActive,
        completeWork(&d, a, pulled.claim.?.claim_token, null),
    );
}

test "peekNext returns the same id pullNext would acquire" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    const tid = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 't', 'todo', 100)",
        &.{.{ .int = sp.pid }},
    );
    const peeked = try peekNext(&d, sp.pid);
    try std.testing.expect(!peeked.no_work);
    try std.testing.expectEqual(tid, peeked.task_id);
    // peek must not write — the same row is still available.
    const pulled = try pullNext(&d, a, .{ .plan_id = sp.pid, .session_id = sp.sid, .vendor = "t" });
    defer pulled.deinit(a);
    try std.testing.expectEqual(tid, pulled.task_id);
}

// =========================================================================
// Plan 493 F1 — claim-less `doing` tasks are re-pullable.
//
// Contract (per tech-spec artifact 265, methodology.md L100): a task whose
// worker crashed (lease lapsed; no unexpired active claim) MUST be
// re-selected by pickNextEligible / pullNext / peekNext, with or without a
// prior reconcile. The widened predicate is:
//
//     tasks.status = 'todo'
//     OR (tasks.status = 'doing' AND no active unexpired claim)
//
// where "active unexpired claim" matches the existing nextWork predicate
// (status = 'active' AND lease_expires_at >= now). The unit tests below
// drive `peekNext`, which routes through `pickNextEligible`, so they
// exercise the selector directly.
// =========================================================================

test "pickNextEligible selects a claim-less doing task (F1 — post-reconcile state)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    // Mirror the post-crash post-reconcile state: a task stranded in
    // `doing` with no claim row at all. Pre-F1 `pickNextEligible`
    // requires `status='todo'` and returns no_work; post-F1 it returns
    // this task.
    const tid = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 'crashed', 'doing', 100)",
        &.{.{ .int = sp.pid }},
    );
    const peeked = try peekNext(&d, sp.pid);
    try std.testing.expect(!peeked.no_work);
    try std.testing.expectEqual(tid, peeked.task_id);
}

test "pickNextEligible selects a doing task whose claim is expired-but-still-active (F1 pre-reconcile window)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    const tid = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 'crashed', 'doing', 100)",
        &.{.{ .int = sp.pid }},
    );
    // An `active` claim whose lease_expires_at is far in the past —
    // i.e. the worker crashed but `reconcile` has not yet flipped the
    // claim to `stale`. F1 says the widened predicate must still pick
    // the task because the claim is not an UNEXPIRED active claim.
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id,
        \\  status, vendor,
        \\  claimed_at, last_heartbeat_at, lease_expires_at
        \\) values (
        \\  'expired-token', ?, 'task', ?,
        \\  'active', 'test',
        \\  '2020-01-01T00:00:00.000Z',
        \\  '2020-01-01T00:00:00.000Z',
        \\  '2020-01-01T00:00:00.000Z'
        \\)
    , &.{ .{ .int = sp.sid }, .{ .int = tid } });

    const peeked = try peekNext(&d, sp.pid);
    try std.testing.expect(!peeked.no_work);
    try std.testing.expectEqual(tid, peeked.task_id);
}

test "pickNextEligible still skips a doing task whose claim is active+unexpired (F1 regression fence)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sp = try newSessionAndPlan(&d);
    const tid = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', ?, 'live', 'doing', 100)",
        &.{.{ .int = sp.pid }},
    );
    // A genuinely-live claim: lease expires far in the future. The
    // widened predicate MUST NOT pick this task — it is real work in
    // flight.
    _ = try d.execParams(
        \\insert into agent_work_claims (
        \\  claim_token, session_id, entity_kind, entity_id,
        \\  status, vendor,
        \\  claimed_at, last_heartbeat_at, lease_expires_at
        \\) values (
        \\  'live-token', ?, 'task', ?,
        \\  'active', 'test',
        \\  '2025-01-01T00:00:00.000Z',
        \\  '2025-01-01T00:00:00.000Z',
        \\  '2099-01-01T00:00:00.000Z'
        \\)
    , &.{ .{ .int = sp.sid }, .{ .int = tid } });

    const peeked = try peekNext(&d, sp.pid);
    try std.testing.expect(peeked.no_work);
}
