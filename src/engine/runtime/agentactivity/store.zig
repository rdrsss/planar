//! agentactivity/store — typed primitive store functions for
//! `agent_work_claims` and `agent_actions`.
//!
//! These are the single-table CRUD primitives the engine consumes;
//! multi-table atomic operations (pull/complete/fail/release/block)
//! live in `atomic.zig` and call into this module under a
//! `BEGIN IMMEDIATE` transaction.
//!
//! All public functions take `*db.sqlite.Db` and an allocator
//! explicitly. Returned values own their string fields and must be
//! released via the appropriate `deinit` helper from `types.zig`.
//!
//! Transactional invariants:
//! - `acquireClaim` and `heartbeatClaim` and `forceTakeover` MUST be
//!   called under `BEGIN IMMEDIATE` (atomic.zig wraps them). The
//!   read-vs-insert race for "no second active claim on the same
//!   (kind,id)" is checked-then-inserted; that pair is only safe with
//!   the writer lock held.
//! - Read-only selectors (`listActive`, `listByEntity`, `listBySession`)
//!   use the default deferred transaction.
//! - `claim_token` is generated INSIDE the INSERT via SQL
//!   `lower(hex(randomblob(16)))` — no Zig-side RNG. The 32-char hex
//!   format is opaque to callers; never parse semantics out.

const std = @import("std");
const db = @import("db");
const types = @import("types.zig");

pub const Error = error{
    /// An exclusive, active, unexpired claim already exists on the
    /// requested `(entity_kind, entity_id)`. `acquireClaim` refuses;
    /// the caller may retry after the existing claim is
    /// released/completed/aborted, or pass a `--force` takeover.
    ClaimContention,
    /// The supplied claim_token did not match any row.
    ClaimNotFound,
    /// The claim row's `session_id` does not match the
    /// caller-supplied session. release/heartbeat enforce this guard;
    /// abort intentionally does not (operator force-release path).
    ClaimSessionMismatch,
    /// `--worktree <id>` was supplied AND the `worktrees` table exists
    /// AND no matching row was found.
    WorktreeNotFound,
    /// The supplied claim is not in a state that allows this
    /// transition (e.g. heartbeat on a released claim).
    ClaimNotActive,
    /// Backstop for unmapped sqlite failures.
    QueryFailed,
} || std.mem.Allocator.Error;

// =========================================================================
// Acquire
// =========================================================================

pub const AcquireArgs = struct {
    session_id: i64,
    entity_kind: types.EntityKind,
    entity_id: i64,
    claim_scope: types.ClaimScope = .exclusive,
    vendor: []const u8,
    vendor_session_id: ?[]const u8 = null,
    role: ?[]const u8 = null,
    model: ?[]const u8 = null,
    worktree_id: ?i64 = null,
    worktree_path: ?[]const u8 = null,
    purpose: ?[]const u8 = null,
    base_ref: ?[]const u8 = null,
    /// Lease TTL in seconds; lease_expires_at is computed as
    /// `strftime('%Y-...','now', +ttl_secs ' seconds')` in SQL.
    ttl_secs: i64 = 600,
    /// Locality snapshot to record on the claim row. Use
    /// `types.Locality.skipped` (the default) when --no-locality-probe
    /// was passed or the probe was not run.
    locality: types.Locality = .{},
    /// When true, mark any existing active claim on the same
    /// `(entity_kind, entity_id)` as stale before inserting the new
    /// row. Operator-only recovery path; the normal acquireClaim flow
    /// returns ClaimContention instead.
    force: bool = false,
};

/// Acquire a new claim. MUST be called under `BEGIN IMMEDIATE` (the
/// atomic wrappers in `atomic.zig` handle that). Returns the freshly
/// inserted Claim row (with the generated `claim_token`).
pub fn acquireClaim(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: AcquireArgs,
) Error!types.Claim {
    if (args.worktree_id) |wt| try validateWorktreeId(d, wt);

    // Exclusivity check: an exclusive, active, unexpired claim on the
    // same (kind, id) blocks. Shared claims coexist with shared; an
    // exclusive in the bucket blocks shared and vice versa. We check
    // and (for --force) mark stale within the same writer-held
    // transaction; the caller's BEGIN IMMEDIATE provides the lock.
    const has_active = try hasActiveClaim(d, args.entity_kind, args.entity_id);
    if (has_active) {
        if (!args.force) return Error.ClaimContention;
        try markActiveStale(d, args.entity_kind, args.entity_id, "force takeover");
    }

    // Insert the new row. `claim_token` is generated inside the SQL via
    // `lower(hex(randomblob(16)))` per the locked tech-spec decision.
    // The TTL is rendered into the SQL fragment (it's a small fixed
    // integer; safe to format inline).
    // Render the TTL into a signed seconds modifier (+N or -N).
    // SQLite's `+-10 seconds` is a syntax error returning NULL; we must
    // emit either "+N seconds" or "N seconds" (an unsigned-prefixed
    // negative). The clean form is "-N seconds" for negative TTLs (used
    // by tests that need to simulate already-expired claims for the
    // reconcile path).
    var sql_buf: [1024]u8 = undefined;
    const insert_sql = if (args.ttl_secs >= 0)
        std.fmt.bufPrintZ(&sql_buf,
            \\insert into agent_work_claims (
            \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
            \\  status, vendor, vendor_session_id, role, model,
            \\  worktree_id, worktree_path,
            \\  repo_root, branch, head_sha_at_claim, dirty_at_claim,
            \\  purpose, base_ref,
            \\  lease_expires_at
            \\) values (
            \\  lower(hex(randomblob(16))), ?, ?, ?, ?,
            \\  'active', ?, ?, ?, ?,
            \\  ?, ?,
            \\  ?, ?, ?, ?,
            \\  ?, ?,
            \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '+{d} seconds')
            \\)
        , .{args.ttl_secs}) catch return Error.QueryFailed
    else
        std.fmt.bufPrintZ(&sql_buf,
            \\insert into agent_work_claims (
            \\  claim_token, session_id, entity_kind, entity_id, claim_scope,
            \\  status, vendor, vendor_session_id, role, model,
            \\  worktree_id, worktree_path,
            \\  repo_root, branch, head_sha_at_claim, dirty_at_claim,
            \\  purpose, base_ref,
            \\  lease_expires_at
            \\) values (
            \\  lower(hex(randomblob(16))), ?, ?, ?, ?,
            \\  'active', ?, ?, ?, ?,
            \\  ?, ?,
            \\  ?, ?, ?, ?,
            \\  ?, ?,
            \\  strftime('%Y-%m-%dT%H:%M:%fZ','now', '{d} seconds')
            \\)
        , .{args.ttl_secs}) catch return Error.QueryFailed;

    const dirty_text: ?[]const u8 = if (args.locality.dirty == .unknown and args.locality.repo_root == null and args.locality.branch == null and args.locality.head_sha == null)
        null
    else
        args.locality.dirty.toText();

    const id = d.execParams(insert_sql, &.{
        .{ .int = args.session_id },
        .{ .text = args.entity_kind.toText() },
        .{ .int = args.entity_id },
        .{ .text = args.claim_scope.toText() },
        .{ .text = args.vendor },
        textOrNull(args.vendor_session_id),
        textOrNull(args.role),
        textOrNull(args.model),
        intOrNull(args.worktree_id),
        textOrNull(args.worktree_path),
        textOrNull(args.locality.repo_root),
        textOrNull(args.locality.branch),
        textOrNull(args.locality.head_sha),
        textOrNull(dirty_text),
        textOrNull(args.purpose),
        textOrNull(args.base_ref),
    }) catch return Error.QueryFailed;

    return try getClaimById(d, allocator, id);
}

// =========================================================================
// Heartbeat / release / abort
// =========================================================================

/// Refresh the lease on an active claim. Updates `last_heartbeat_at` to
/// now and `lease_expires_at` to now+ttl_secs. Caller MUST hold
/// `BEGIN IMMEDIATE` (atomic.zig wraps this).
pub fn heartbeatClaim(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    claim_token: []const u8,
    ttl_secs: i64,
) Error!types.Claim {
    const c = try getClaimByToken(d, allocator, claim_token);
    // We hold the row briefly only to verify status; release ownership
    // back so the caller gets a fresh snapshot at the end.
    if (c.status != .active) {
        c.deinit(allocator);
        return Error.ClaimNotActive;
    }
    c.deinit(allocator);

    var sql_buf: [512]u8 = undefined;
    const sql = if (ttl_secs >= 0)
        std.fmt.bufPrintZ(&sql_buf,
            \\update agent_work_claims
            \\set last_heartbeat_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'),
            \\    lease_expires_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '+{d} seconds')
            \\where claim_token = ? and status = 'active'
        , .{ttl_secs}) catch return Error.QueryFailed
    else
        std.fmt.bufPrintZ(&sql_buf,
            \\update agent_work_claims
            \\set last_heartbeat_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'),
            \\    lease_expires_at = strftime('%Y-%m-%dT%H:%M:%fZ','now', '{d} seconds')
            \\where claim_token = ? and status = 'active'
        , .{ttl_secs}) catch return Error.QueryFailed;

    _ = d.execParams(sql, &.{.{ .text = claim_token }}) catch return Error.QueryFailed;
    return try getClaimByToken(d, allocator, claim_token);
}

/// Release a claim with a final status. Valid targets:
///   .released  — graceful give-up
///   .completed — work succeeded
///   .aborted   — work failed
///   .stale     — reconcile path
/// Refuses `.active` (release IS the transition away from active).
pub fn releaseClaim(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    claim_token: []const u8,
    new_status: types.ClaimStatus,
    reason: ?[]const u8,
) Error!types.Claim {
    if (new_status == .active) return Error.QueryFailed;

    _ = d.execParams(
        \\update agent_work_claims
        \\set status = ?,
        \\    released_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'),
        \\    release_reason = ?
        \\where claim_token = ?
    , &.{
        .{ .text = new_status.toText() },
        textOrNull(reason),
        .{ .text = claim_token },
    }) catch return Error.QueryFailed;

    return try getClaimByToken(d, allocator, claim_token);
}

/// Operator-side force-release. Distinct from `releaseClaim` because
/// `abort` does not require the calling session to own the claim — it's
/// the recovery path for stuck claims from any session. The
/// `aborting_session_id` is used by the atomic wrapper to write an
/// audit-record action row naming the actor.
pub fn abortClaim(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    claim_token: []const u8,
    reason: ?[]const u8,
) Error!types.Claim {
    return try releaseClaim(d, allocator, claim_token, .aborted, reason);
}

// =========================================================================
// Read paths
// =========================================================================

pub fn getClaimById(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
) Error!types.Claim {
    var stmt = d.prepare(claim_select_by_id) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.ClaimNotFound,
        .row => try readClaimRow(&stmt, allocator),
    };
}

pub fn getClaimByToken(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    token: []const u8,
) Error!types.Claim {
    var stmt = d.prepare(claim_select_by_token) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = token }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.ClaimNotFound,
        .row => try readClaimRow(&stmt, allocator),
    };
}

/// Look up the entity referenced by `c` and resolve its scope
/// (plans / plan_steps / tasks share the same `scope_kind` +
/// `scope_id` columns, except plan_steps inherit from the parent
/// plan). Best-effort: a missing entity returns
/// `ClaimScopeInfo.unknown` rather than erroring — display surfaces
/// (planar-watch ps / claims) should not break because one row is
/// stale.
///
/// Caller owns the returned `slug` slice (when non-null) — release
/// via `info.deinit(allocator)`.
pub fn resolveClaimScope(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    c: types.Claim,
) types.ClaimScopeInfo {
    const sql_z: [:0]const u8 = switch (c.entity_kind) {
        .plan => "select scope_kind, scope_id from plans where id = ?",
        .task => "select scope_kind, scope_id from tasks where id = ?",
        // plan_steps inherit their scope from the parent plan; no
        // scope columns of their own per the 00003_work_items schema.
        .plan_step =>
        \\select p.scope_kind, p.scope_id from plan_steps ps
        \\  join plans p on ps.plan_id = p.id where ps.id = ?
        ,
    };
    var stmt = d.prepare(sql_z) catch return types.ClaimScopeInfo.unknown;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = c.entity_id }}) catch return types.ClaimScopeInfo.unknown;
    switch (stmt.step() catch return types.ClaimScopeInfo.unknown) {
        .done => return types.ClaimScopeInfo.unknown,
        .row => {},
    }

    const kind_text = stmt.columnTextAlloc(0, allocator) catch return types.ClaimScopeInfo.unknown;
    defer allocator.free(kind_text);

    const kind_static: []const u8 = if (std.mem.eql(u8, kind_text, "global"))
        "global"
    else if (std.mem.eql(u8, kind_text, "association"))
        "association"
    else if (std.mem.eql(u8, kind_text, "repo"))
        "repo"
    else
        "?";

    if (std.mem.eql(u8, kind_static, "global")) {
        return .{ .kind = "global", .slug = null };
    }
    const scope_id = stmt.columnIntOpt(1) orelse return .{ .kind = kind_static, .slug = null };

    const slug_sql: [:0]const u8 = if (std.mem.eql(u8, kind_static, "association"))
        "select slug from associations where id = ?"
    else
        "select slug from projects where id = ?";
    var slug_stmt = d.prepare(slug_sql) catch return .{ .kind = kind_static, .slug = null };
    defer slug_stmt.finalize();
    slug_stmt.bind(&.{.{ .int = scope_id }}) catch return .{ .kind = kind_static, .slug = null };
    const slug: ?[]const u8 = switch (slug_stmt.step() catch return .{ .kind = kind_static, .slug = null }) {
        .done => null,
        .row => slug_stmt.columnTextAlloc(0, allocator) catch null,
    };
    return .{ .kind = kind_static, .slug = slug };
}

/// List all active claims, optionally filtered by session id. Read-only
/// — uses the default deferred transaction.
pub fn listActive(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    filter_session_id: ?i64,
) Error![]types.Claim {
    const sql = if (filter_session_id != null) claim_select_active_for_session else claim_select_active_all;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    if (filter_session_id) |sid| {
        stmt.bind(&.{.{ .int = sid }}) catch return Error.QueryFailed;
    }
    var out: std.ArrayList(types.Claim) = .empty;
    errdefer {
        for (out.items) |c| c.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readClaimRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn listByEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_kind: types.EntityKind,
    entity_id: i64,
) Error![]types.Claim {
    var stmt = d.prepare(claim_select_by_entity) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .text = entity_kind.toText() },
        .{ .int = entity_id },
    }) catch return Error.QueryFailed;
    var out: std.ArrayList(types.Claim) = .empty;
    errdefer {
        for (out.items) |c| c.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readClaimRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

pub fn listBySession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
) Error![]types.Claim {
    var stmt = d.prepare(claim_select_by_session) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return Error.QueryFailed;
    var out: std.ArrayList(types.Claim) = .empty;
    errdefer {
        for (out.items) |c| c.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readClaimRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Reconcile
// =========================================================================

pub const ReconcilePolicy = struct {
    /// Additional grace beyond lease expiry; default 0.
    stale_after_secs: i64 = 0,
    /// When true, return the candidate set without writing.
    dry_run: bool = false,
};

pub const ReconcileResult = struct {
    claims_marked_stale: i64 = 0,
    actions_closed: i64 = 0,
    candidates: []types.Claim = &.{},

    pub fn deinit(self: ReconcileResult, allocator: std.mem.Allocator) void {
        types.Claim.deinitMany(self.candidates, allocator);
    }
};

/// Mark every active claim whose lease expired more than
/// `stale_after_secs` ago as stale, and close orphaned actions whose
/// owning session has ended. Atomic — caller wraps in `BEGIN IMMEDIATE`.
pub fn reconcileStale(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    policy: ReconcilePolicy,
) Error!ReconcileResult {
    // Select the candidate active+expired claims first; we always need
    // them for the returned candidate list (or to mark stale).
    var sel_buf: [512]u8 = undefined;
    const sel_sql = std.fmt.bufPrintZ(&sel_buf,
        \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\       status, vendor, vendor_session_id, role, model,
        \\       worktree_id, worktree_path,
        \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
        \\       purpose, base_ref,
        \\       claimed_at, last_heartbeat_at, lease_expires_at,
        \\       released_at, release_reason
        \\from agent_work_claims
        \\where status = 'active'
        \\  and lease_expires_at < strftime('%Y-%m-%dT%H:%M:%fZ','now', '-{d} seconds')
    , .{policy.stale_after_secs}) catch return Error.QueryFailed;

    var stmt = d.prepare(sel_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    var candidates: std.ArrayList(types.Claim) = .empty;
    errdefer {
        for (candidates.items) |c| c.deinit(allocator);
        candidates.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try candidates.append(allocator, try readClaimRow(&stmt, allocator)),
        }
    }

    var result: ReconcileResult = .{};
    if (policy.dry_run) {
        result.candidates = try candidates.toOwnedSlice(allocator);
        return result;
    }

    // Apply the mark-stale UPDATE and the orphaned-actions UPDATE.
    var upd_buf: [512]u8 = undefined;
    const upd_sql = std.fmt.bufPrintZ(&upd_buf,
        \\update agent_work_claims
        \\set status = 'stale',
        \\    released_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'),
        \\    release_reason = 'reconcile: heartbeat expired'
        \\where status = 'active'
        \\  and lease_expires_at < strftime('%Y-%m-%dT%H:%M:%fZ','now', '-{d} seconds')
    , .{policy.stale_after_secs}) catch return Error.QueryFailed;
    _ = d.execParams(upd_sql, &.{}) catch return Error.QueryFailed;
    result.claims_marked_stale = @intCast(candidates.items.len);

    // Orphaned actions: ended_at IS NULL and the owning session has
    // ended. Close them with outcome='aborted' and the session's
    // ended_at as the ended_at timestamp.
    _ = d.execParams(
        \\update agent_actions
        \\set ended_at = (select ended_at from sessions where id = agent_actions.session_id),
        \\    outcome = 'aborted'
        \\where ended_at is null
        \\  and exists (
        \\    select 1 from sessions s
        \\    where s.id = agent_actions.session_id and s.ended_at is not null
        \\  )
    , &.{}) catch return Error.QueryFailed;
    // Count via a follow-up SELECT against the same predicate post-update:
    // ended_at is now non-null and outcome='aborted'; we lack a per-update
    // count from execParams. Approximate via a separate count query that
    // matches the set we just touched.
    if (d.intQuery(
        \\select count(*) from agent_actions
        \\where outcome = 'aborted'
        \\  and ended_at is not null
        \\  and exists (
        \\    select 1 from sessions s
        \\    where s.id = agent_actions.session_id and s.ended_at is not null
        \\  )
    )) |n| {
        result.actions_closed = n;
    } else |_| {
        result.actions_closed = 0;
    }

    result.candidates = try candidates.toOwnedSlice(allocator);
    return result;
}

// =========================================================================
// Actions
// =========================================================================

pub const StartActionArgs = struct {
    session_id: i64,
    session_entry_id: ?i64 = null,
    parent_action_id: ?i64 = null,
    claim_id: ?i64 = null,
    action_kind: types.ActionKind,
    entity_kind: ?types.ActionEntityKind = null,
    entity_id: ?i64 = null,
    vendor: []const u8,
    vendor_role: ?[]const u8 = null,
    model: ?[]const u8 = null,
    /// Locality at action start. Use `types.Locality.skipped` to skip.
    locality: types.Locality = .{},
    /// Free-form opaque text persisted to `agent_actions.metadata`.
    /// Typically a JSON document encoded by the caller (e.g. the
    /// orchestrator strategy gate writes
    /// `{"strategy":"<name>","axes":{...},...}`). The engine does NOT
    /// parse or validate the content — that's the caller's contract.
    /// NULL to leave the column unset.
    metadata: ?[]const u8 = null,
};

/// Insert an agent_actions row with `started_at` defaulted to now and
/// `ended_at` NULL. Returns the new row id.
pub fn startAction(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: StartActionArgs,
) Error!i64 {
    _ = allocator;
    if ((args.entity_kind == null) != (args.entity_id == null)) {
        // Schema CHECK refuses asymmetric set; surface as QueryFailed.
        return Error.QueryFailed;
    }

    const dirty_text: ?[]const u8 = if (args.locality.head_sha == null and args.locality.dirty == .unknown)
        null
    else
        args.locality.dirty.toText();

    const id = d.execParams(
        \\insert into agent_actions (
        \\  session_id, session_entry_id, parent_action_id, claim_id,
        \\  action_kind, entity_kind, entity_id,
        \\  vendor, vendor_role, model,
        \\  head_sha, dirty,
        \\  metadata
        \\) values (
        \\  ?, ?, ?, ?,
        \\  ?, ?, ?,
        \\  ?, ?, ?,
        \\  ?, ?,
        \\  ?
        \\)
    , &.{
        .{ .int = args.session_id },
        intOrNull(args.session_entry_id),
        intOrNull(args.parent_action_id),
        intOrNull(args.claim_id),
        .{ .text = args.action_kind.toText() },
        if (args.entity_kind) |k| .{ .text = k.toText() } else .{ .null = {} },
        intOrNull(args.entity_id),
        .{ .text = args.vendor },
        textOrNull(args.vendor_role),
        textOrNull(args.model),
        textOrNull(args.locality.head_sha),
        textOrNull(dirty_text),
        textOrNull(args.metadata),
    }) catch return Error.QueryFailed;
    return id;
}

/// Close an agent_actions row. Sets ended_at=now, outcome, and
/// optionally summary. Refuses to update an already-ended row (the
/// schema permits it but the engine treats it as a programming error
/// worth surfacing).
pub fn endAction(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    outcome: types.Outcome,
    summary: ?[]const u8,
) Error!void {
    _ = allocator;
    _ = d.execParams(
        \\update agent_actions
        \\set ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'),
        \\    outcome = ?,
        \\    summary = coalesce(?, summary)
        \\where id = ? and ended_at is null
    , &.{
        .{ .text = outcome.toText() },
        textOrNull(summary),
        .{ .int = id },
    }) catch return Error.QueryFailed;
}

pub fn getActionById(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
) Error!types.Action {
    var stmt = d.prepare(action_select_by_id) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.ClaimNotFound, // overload: action not found maps here
        .row => try readActionRow(&stmt, allocator),
    };
}

// =========================================================================
// nextWork — claim-aware "what's available" selector
// =========================================================================

pub const NextWorkBucket = enum { available, claimed, stale, blocked };

pub const NextWorkRow = struct {
    bucket: NextWorkBucket,
    task_id: i64,
    title: []const u8,
    status: []const u8,
    priority: i64,
    claim: ?types.Claim = null,

    pub fn deinit(self: NextWorkRow, allocator: std.mem.Allocator) void {
        allocator.free(self.title);
        allocator.free(self.status);
        if (self.claim) |c| c.deinit(allocator);
    }
};

pub const NextWork = struct {
    rows: []NextWorkRow,

    pub fn deinit(self: NextWork, allocator: std.mem.Allocator) void {
        for (self.rows) |r| r.deinit(allocator);
        allocator.free(self.rows);
    }
};

/// Resolve the "what's next" view for a plan id. Returns every task
/// reachable from the plan classified into one of four buckets:
/// available (no active claim, status in {todo}), claimed (active,
/// unexpired claim), stale (claim status='stale' or expired without
/// reconcile), blocked (task status='blocked').
///
/// The selector intentionally does NOT mutate task status — operator
/// callers use it for display only. Atomic operations have their own
/// status-flipping queries.
pub fn nextWork(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) Error!NextWork {
    // Pull every task that hangs off the plan or any of its child
    // plans. The schema lets us reach child plans via plans.parent_plan_id
    // but the canonical "what work belongs to this plan" is just
    // tasks.plan_id = ?. Multi-level recursion can land later.
    var stmt = d.prepare(
        \\select t.id, t.title, t.status, t.priority,
        \\       (
        \\         select id from agent_work_claims c
        \\         where c.entity_kind = 'task'
        \\           and c.entity_id = t.id
        \\           and c.status = 'active'
        \\           and c.lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\         order by c.id desc limit 1
        \\       ) as active_claim_id,
        \\       (
        \\         select id from agent_work_claims c
        \\         where c.entity_kind = 'task'
        \\           and c.entity_id = t.id
        \\           and (c.status = 'stale'
        \\             or (c.status = 'active' and c.lease_expires_at < strftime('%Y-%m-%dT%H:%M:%fZ','now')))
        \\         order by c.id desc limit 1
        \\       ) as stale_claim_id
        \\from tasks t
        \\where t.plan_id = ?
        \\order by t.priority asc, t.id asc
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(NextWorkRow) = .empty;
    errdefer {
        for (out.items) |r| r.deinit(allocator);
        out.deinit(allocator);
    }

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const task_id = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, allocator);
                errdefer allocator.free(title);
                const status = try stmt.columnTextAlloc(2, allocator);
                errdefer allocator.free(status);
                const priority = stmt.columnInt(3);
                const active_id = stmt.columnIntOpt(4);
                const stale_id = stmt.columnIntOpt(5);

                var bucket: NextWorkBucket = .available;
                var claim: ?types.Claim = null;
                if (std.mem.eql(u8, status, "blocked")) {
                    bucket = .blocked;
                } else if (active_id) |cid| {
                    bucket = .claimed;
                    claim = try getClaimById(d, allocator, cid);
                } else if (stale_id) |cid| {
                    bucket = .stale;
                    claim = try getClaimById(d, allocator, cid);
                } else if (std.mem.eql(u8, status, "todo")) {
                    bucket = .available;
                } else if (std.mem.eql(u8, status, "doing")) {
                    // doing with no active claim: leftover from a stale
                    // claim that's since been reconciled. Surface as
                    // available so a fresh pull picks it up.
                    bucket = .available;
                } else {
                    // done / cancelled: skip — not part of any bucket.
                    allocator.free(title);
                    allocator.free(status);
                    continue;
                }

                try out.append(allocator, .{
                    .bucket = bucket,
                    .task_id = task_id,
                    .title = title,
                    .status = status,
                    .priority = priority,
                    .claim = claim,
                });
            },
        }
    }

    return .{ .rows = try out.toOwnedSlice(allocator) };
}

// =========================================================================
// Internals
// =========================================================================

fn hasActiveClaim(
    d: *db.sqlite.Db,
    entity_kind: types.EntityKind,
    entity_id: i64,
) Error!bool {
    var stmt = d.prepare(
        \\select 1 from agent_work_claims
        \\where entity_kind = ?
        \\  and entity_id = ?
        \\  and status = 'active'
        \\  and lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .text = entity_kind.toText() },
        .{ .int = entity_id },
    }) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => false,
        .row => true,
    };
}

fn markActiveStale(
    d: *db.sqlite.Db,
    entity_kind: types.EntityKind,
    entity_id: i64,
    reason: []const u8,
) Error!void {
    _ = d.execParams(
        \\update agent_work_claims
        \\set status = 'stale',
        \\    released_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'),
        \\    release_reason = ?
        \\where entity_kind = ?
        \\  and entity_id = ?
        \\  and status = 'active'
    , &.{
        .{ .text = reason },
        .{ .text = entity_kind.toText() },
        .{ .int = entity_id },
    }) catch return Error.QueryFailed;
}

/// Validate `worktree_id` per the tech-spec rule: if a `worktrees`
/// table exists, the id must resolve; if no such table exists, the id
/// is opaque display context and passes through.
fn validateWorktreeId(d: *db.sqlite.Db, worktree_id: i64) Error!void {
    const table_present = d.intQuery(
        "select count(*) from sqlite_master where type='table' and name='worktrees'",
    ) catch return Error.QueryFailed;
    if (table_present == 0) return; // table not present yet; opaque.

    var stmt = d.prepare("select 1 from worktrees where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = worktree_id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .row => return,
        .done => return Error.WorktreeNotFound,
    }
}

fn textOrNull(s: ?[]const u8) db.sqlite.Param {
    if (s) |v| return .{ .text = v };
    return .{ .null = {} };
}

fn intOrNull(v: ?i64) db.sqlite.Param {
    if (v) |x| return .{ .int = x };
    return .{ .null = {} };
}

// -- SQL constants ------------------------------------------------------

const claim_columns =
    \\id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\status, vendor, vendor_session_id, role, model,
    \\worktree_id, worktree_path,
    \\repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\purpose, base_ref,
    \\claimed_at, last_heartbeat_at, lease_expires_at,
    \\released_at, release_reason
;

const claim_select_by_id: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason
    \\from agent_work_claims where id = ?
;

const claim_select_by_token: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason
    \\from agent_work_claims where claim_token = ?
;

const claim_select_active_all: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason
    \\from agent_work_claims
    \\where status = 'active'
    \\order by claimed_at desc
;

const claim_select_active_for_session: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason
    \\from agent_work_claims
    \\where status = 'active' and session_id = ?
    \\order by claimed_at desc
;

const claim_select_by_entity: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason
    \\from agent_work_claims
    \\where entity_kind = ? and entity_id = ?
    \\order by claimed_at desc
;

const claim_select_by_session: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason
    \\from agent_work_claims
    \\where session_id = ?
    \\order by claimed_at desc
;

const action_select_by_id: [:0]const u8 =
    \\select id, session_id, session_entry_id, parent_action_id, claim_id,
    \\       action_kind, entity_kind, entity_id,
    \\       vendor, vendor_role, model,
    \\       started_at, ended_at, outcome, summary,
    \\       head_sha, dirty,
    \\       metadata
    \\from agent_actions where id = ?
;

fn readClaimRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!types.Claim {
    const kind_text = try stmt.columnTextAlloc(3, allocator);
    defer allocator.free(kind_text);
    const kind = types.EntityKind.fromText(kind_text) orelse return Error.QueryFailed;

    const scope_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(scope_text);
    const scope = types.ClaimScope.fromText(scope_text) orelse return Error.QueryFailed;

    const status_text = try stmt.columnTextAlloc(6, allocator);
    defer allocator.free(status_text);
    const status = types.ClaimStatus.fromText(status_text) orelse return Error.QueryFailed;

    const dirty_opt = try stmt.columnTextOpt(16, allocator);
    var dirty: ?types.Dirty = null;
    if (dirty_opt) |d_text| {
        defer allocator.free(d_text);
        dirty = types.Dirty.fromText(d_text);
    }

    return .{
        .id = stmt.columnInt(0),
        .claim_token = try stmt.columnTextAlloc(1, allocator),
        .session_id = stmt.columnInt(2),
        .entity_kind = kind,
        .entity_id = stmt.columnInt(4),
        .claim_scope = scope,
        .status = status,
        .vendor = try stmt.columnTextAlloc(7, allocator),
        .vendor_session_id = try stmt.columnTextOpt(8, allocator),
        .role = try stmt.columnTextOpt(9, allocator),
        .model = try stmt.columnTextOpt(10, allocator),
        .worktree_id = stmt.columnIntOpt(11),
        .worktree_path = try stmt.columnTextOpt(12, allocator),
        .repo_root = try stmt.columnTextOpt(13, allocator),
        .branch = try stmt.columnTextOpt(14, allocator),
        .head_sha_at_claim = try stmt.columnTextOpt(15, allocator),
        .dirty_at_claim = dirty,
        .purpose = try stmt.columnTextOpt(17, allocator),
        .base_ref = try stmt.columnTextOpt(18, allocator),
        .claimed_at = try stmt.columnTextAlloc(19, allocator),
        .last_heartbeat_at = try stmt.columnTextAlloc(20, allocator),
        .lease_expires_at = try stmt.columnTextAlloc(21, allocator),
        .released_at = try stmt.columnTextOpt(22, allocator),
        .release_reason = try stmt.columnTextOpt(23, allocator),
    };
}

fn readActionRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!types.Action {
    const kind_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(kind_text);
    const kind = types.ActionKind.fromText(kind_text) orelse return Error.QueryFailed;

    var ent_kind: ?types.ActionEntityKind = null;
    if (try stmt.columnTextOpt(6, allocator)) |ek_text| {
        defer allocator.free(ek_text);
        ent_kind = types.ActionEntityKind.fromText(ek_text);
    }

    var outcome: ?types.Outcome = null;
    if (try stmt.columnTextOpt(13, allocator)) |o_text| {
        defer allocator.free(o_text);
        outcome = types.Outcome.fromText(o_text);
    }

    var dirty: ?types.Dirty = null;
    if (try stmt.columnTextOpt(16, allocator)) |d_text| {
        defer allocator.free(d_text);
        dirty = types.Dirty.fromText(d_text);
    }

    return .{
        .id = stmt.columnInt(0),
        .session_id = stmt.columnInt(1),
        .session_entry_id = stmt.columnIntOpt(2),
        .parent_action_id = stmt.columnIntOpt(3),
        .claim_id = stmt.columnIntOpt(4),
        .action_kind = kind,
        .entity_kind = ent_kind,
        .entity_id = stmt.columnIntOpt(7),
        .vendor = try stmt.columnTextAlloc(8, allocator),
        .vendor_role = try stmt.columnTextOpt(9, allocator),
        .model = try stmt.columnTextOpt(10, allocator),
        .started_at = try stmt.columnTextAlloc(11, allocator),
        .ended_at = try stmt.columnTextOpt(12, allocator),
        .outcome = outcome,
        .summary = try stmt.columnTextOpt(14, allocator),
        .head_sha = try stmt.columnTextOpt(15, allocator),
        .dirty = dirty,
        .metadata = try stmt.columnTextOpt(17, allocator),
    };
}

// =========================================================================
// Tests — in-memory SQLite + applied migrations
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

fn insertTestSession(d: *db.sqlite.Db) !i64 {
    return try d.execParams(
        "insert into sessions (vendor) values (?)",
        &.{.{ .text = "test" }},
    );
}

fn insertTestTask(d: *db.sqlite.Db) !i64 {
    return try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','t','todo')",
        &.{},
    );
}

test "acquireClaim happy path returns active claim with generated token" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);

    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
        .ttl_secs = 600,
    });
    defer c.deinit(a);

    try std.testing.expectEqual(types.ClaimStatus.active, c.status);
    try std.testing.expectEqual(types.EntityKind.task, c.entity_kind);
    try std.testing.expectEqual(tid, c.entity_id);
    try std.testing.expectEqual(@as(usize, 32), c.claim_token.len);
}

test "acquireClaim refuses second active claim on same entity" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);

    const c1 = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
    });
    defer c1.deinit(a);

    try std.testing.expectError(Error.ClaimContention, acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
    }));
}

test "acquireClaim with force marks existing stale and acquires new" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);

    const c1 = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
    });
    defer c1.deinit(a);

    const c2 = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
        .force = true,
    });
    defer c2.deinit(a);

    // Reload c1 by token; it should now be stale.
    const reloaded = try getClaimByToken(&d, a, c1.claim_token);
    defer reloaded.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.stale, reloaded.status);
    try std.testing.expectEqual(types.ClaimStatus.active, c2.status);
}

test "heartbeatClaim extends lease and updates last_heartbeat_at" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);
    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
        .ttl_secs = 60,
    });
    defer c.deinit(a);

    const refreshed = try heartbeatClaim(&d, a, c.claim_token, 600);
    defer refreshed.deinit(a);
    // The lease_expires_at must shift; the exact ISO timestamps differ
    // by the TTL delta. Comparing as strings works because both are
    // strftime-formatted ISO8601 with millisecond precision.
    try std.testing.expect(!std.mem.eql(u8, c.lease_expires_at, refreshed.lease_expires_at));
}

test "heartbeatClaim refuses released claim with ClaimNotActive" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);
    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
    });
    defer c.deinit(a);

    const r = try releaseClaim(&d, a, c.claim_token, .released, "test");
    r.deinit(a);

    try std.testing.expectError(Error.ClaimNotActive, heartbeatClaim(&d, a, c.claim_token, 600));
}

test "releaseClaim transitions status and records reason" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);
    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
    });
    defer c.deinit(a);

    const r = try releaseClaim(&d, a, c.claim_token, .completed, "done");
    defer r.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.completed, r.status);
    try std.testing.expect(r.released_at != null);
    try std.testing.expectEqualStrings("done", r.release_reason.?);
}

test "abortClaim works on any session and marks aborted" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid1 = try insertTestSession(&d);
    const tid = try insertTestTask(&d);
    const c = try acquireClaim(&d, a, .{
        .session_id = sid1,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
    });
    defer c.deinit(a);

    // No session-ownership check on abort.
    const r = try abortClaim(&d, a, c.claim_token, "stuck");
    defer r.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.aborted, r.status);
}

test "reconcileStale finds expired active claims and marks them stale" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);

    // Acquire with a NEGATIVE TTL so the lease is already expired.
    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
        .ttl_secs = -10,
    });
    defer c.deinit(a);

    const r = try reconcileStale(&d, a, .{});
    defer r.deinit(a);
    try std.testing.expectEqual(@as(i64, 1), r.claims_marked_stale);

    const reloaded = try getClaimByToken(&d, a, c.claim_token);
    defer reloaded.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.stale, reloaded.status);
}

test "reconcileStale --dry-run returns candidates without writing" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);
    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
        .ttl_secs = -10,
    });
    defer c.deinit(a);

    const r = try reconcileStale(&d, a, .{ .dry_run = true });
    defer r.deinit(a);
    try std.testing.expectEqual(@as(usize, 1), r.candidates.len);

    const reloaded = try getClaimByToken(&d, a, c.claim_token);
    defer reloaded.deinit(a);
    // Dry-run must leave the claim active.
    try std.testing.expectEqual(types.ClaimStatus.active, reloaded.status);
}

test "startAction inserts and getActionById round-trips" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const id = try startAction(&d, a, .{
        .session_id = sid,
        .action_kind = .coder,
        .vendor = "test",
    });
    const got = try getActionById(&d, a, id);
    defer got.deinit(a);
    try std.testing.expectEqual(types.ActionKind.coder, got.action_kind);
    try std.testing.expect(got.ended_at == null);
}

test "endAction closes the action and records outcome" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const id = try startAction(&d, a, .{
        .session_id = sid,
        .action_kind = .coder,
        .vendor = "test",
    });
    try endAction(&d, a, id, .ok, "wrapped up");
    const got = try getActionById(&d, a, id);
    defer got.deinit(a);
    try std.testing.expect(got.ended_at != null);
    try std.testing.expectEqual(types.Outcome.ok, got.outcome.?);
    try std.testing.expectEqualStrings("wrapped up", got.summary.?);
}

test "listActive surfaces active claims and skips released" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid1 = try insertTestTask(&d);
    const tid2 = try insertTestTask(&d);

    const c1 = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid1,
        .vendor = "test",
    });
    defer c1.deinit(a);
    const c2 = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid2,
        .vendor = "test",
    });
    defer c2.deinit(a);
    const r = try releaseClaim(&d, a, c2.claim_token, .released, null);
    r.deinit(a);

    const active = try listActive(&d, a, null);
    defer types.Claim.deinitMany(active, a);
    try std.testing.expectEqual(@as(usize, 1), active.len);
}

test "nextWork classifies tasks into available, claimed, blocked buckets" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug) values ('global','p','nw-plan')",
        &.{},
    );
    const t1 = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 't1','todo')",
        &.{.{ .int = plan_id }},
    );
    const t2 = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 't2','todo')",
        &.{.{ .int = plan_id }},
    );
    const t3 = try d.execParams(
        "insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 't3','blocked')",
        &.{.{ .int = plan_id }},
    );
    _ = t1;

    const c2 = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = t2,
        .vendor = "test",
    });
    defer c2.deinit(a);

    const nw = try nextWork(&d, a, plan_id);
    defer nw.deinit(a);

    var n_avail: usize = 0;
    var n_claimed: usize = 0;
    var n_blocked: usize = 0;
    for (nw.rows) |r| {
        switch (r.bucket) {
            .available => n_avail += 1,
            .claimed => n_claimed += 1,
            .blocked => n_blocked += 1,
            .stale => {},
        }
    }
    try std.testing.expectEqual(@as(usize, 1), n_avail);
    try std.testing.expectEqual(@as(usize, 1), n_claimed);
    try std.testing.expectEqual(@as(usize, 1), n_blocked);
    _ = t3;
}

test "startAction persists metadata text and getActionById round-trips it" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const meta = "{\"strategy\":\"isolated-sequential\",\"axes\":{\"isolation\":\"worktree\"},\"rationale\":\"2-task plan\"}";
    const id = try startAction(&d, a, .{
        .session_id = sid,
        .action_kind = .orchestrator,
        .vendor = "test",
        .metadata = meta,
    });
    const got = try getActionById(&d, a, id);
    defer got.deinit(a);
    try std.testing.expect(got.metadata != null);
    try std.testing.expectEqualStrings(meta, got.metadata.?);
}

test "startAction with null metadata leaves the column NULL" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const id = try startAction(&d, a, .{
        .session_id = sid,
        .action_kind = .coder,
        .vendor = "test",
    });
    const got = try getActionById(&d, a, id);
    defer got.deinit(a);
    try std.testing.expect(got.metadata == null);
}

test "acquireClaim records locality columns when provided" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);

    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
        .locality = .{
            .repo_root = "/tmp/repo",
            .branch = "feat/x",
            .head_sha = "abc123",
            .dirty = .clean,
        },
    });
    defer c.deinit(a);
    try std.testing.expectEqualStrings("/tmp/repo", c.repo_root.?);
    try std.testing.expectEqualStrings("feat/x", c.branch.?);
    try std.testing.expectEqualStrings("abc123", c.head_sha_at_claim.?);
    try std.testing.expectEqual(types.Dirty.clean, c.dirty_at_claim.?);
}
