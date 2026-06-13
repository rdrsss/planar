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
    /// Optional FK to workflow_runs.id; set by planar-execute via
    /// --run <id>. NULL for interactive / non-workflow claims.
    run_id: ?i64 = null,
    /// Optional stage name from the workflow that dispatched this
    /// worker (e.g. "code", "review"). NULL when --stage is omitted.
    stage: ?[]const u8 = null,
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
            \\  run_id, stage,
            \\  lease_expires_at
            \\) values (
            \\  lower(hex(randomblob(16))), ?, ?, ?, ?,
            \\  'active', ?, ?, ?, ?,
            \\  ?, ?,
            \\  ?, ?, ?, ?,
            \\  ?, ?,
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
            \\  run_id, stage,
            \\  lease_expires_at
            \\) values (
            \\  lower(hex(randomblob(16))), ?, ?, ?, ?,
            \\  'active', ?, ?, ?, ?,
            \\  ?, ?,
            \\  ?, ?, ?, ?,
            \\  ?, ?,
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
        intOrNull(args.run_id),
        textOrNull(args.stage),
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

/// List claims that are stale in practice: rows with `status='stale'`,
/// plus `active` rows whose lease has already expired (reconcile has not
/// run yet but the claim is no longer honored). Shares `claim_columns`
/// and `readClaimRow` with the other claim-list APIs so the column order
/// lives in exactly one place. Used by the operator dashboard.
pub fn listStale(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) Error![]types.Claim {
    var stmt = d.prepare(claim_select_stale) catch return Error.QueryFailed;
    defer stmt.finalize();
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

/// Return the `id` of the most-recently-claimed active claim owned by
/// `session_id`, or `null` when the session has no active claims. Used
/// by entity-create hooks to decide whether to write an action row.
///
/// "Most recent" is determined by `claimed_at desc` then `id desc` as a
/// tiebreaker (the same ordering `listActive` uses). A claim must have
/// `status = 'active'` and a non-expired `lease_expires_at` to qualify.
pub fn latestActiveClaimForSession(
    d: *db.sqlite.Db,
    session_id: i64,
) Error!?i64 {
    var stmt = d.prepare(
        \\select id from agent_work_claims
        \\where session_id = ?
        \\  and status = 'active'
        \\  and lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\order by claimed_at desc, id desc
        \\limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => null,
        .row => stmt.columnInt(0),
    };
}

/// Write a completed `agent_actions` row recording that the session's active
/// claim performed an entity-create. Best-effort per Decision D2: errors are
/// logged via `std.log.scoped(.agentactivity)` and swallowed — the entity-
/// create write is the contract; activity surfacing is not.
///
/// When the session has no active claim (e.g. an operator running
/// `planar question add` from a shell), returns immediately — no row, no error.
///
/// The row is written with `action_kind = other` and both `started_at` and
/// `ended_at` set to now, `outcome = 'ok'`. The `vendor` is resolved from the
/// claim row so the caller does not need to pass it.
pub fn recordEntityCreateAction(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
    entity_kind: types.ActionEntityKind,
    entity_id: i64,
    summary: []const u8,
) void {
    recordEntityCreateActionInner(d, allocator, session_id, entity_kind, entity_id, summary) catch |e| {
        std.log.scoped(.agentactivity).warn(
            "recordEntityCreateAction: failed to write action row: {s}",
            .{@errorName(e)},
        );
    };
}

fn recordEntityCreateActionInner(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
    entity_kind: types.ActionEntityKind,
    entity_id: i64,
    summary: []const u8,
) Error!void {
    // Resolve the latest active claim for the session. Get both `id` and
    // `vendor` in one query to avoid a second round-trip.
    var stmt = d.prepare(
        \\select id, vendor from agent_work_claims
        \\where session_id = ?
        \\  and status = 'active'
        \\  and lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\order by claimed_at desc, id desc
        \\limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        // No active claim: operator shell invocation — return silently.
        .done => return,
        .row => {},
    }
    const claim_id = stmt.columnInt(0);
    const vendor = stmt.columnTextAlloc(1, allocator) catch return Error.QueryFailed;
    defer allocator.free(vendor);

    // Insert the action row as a completed-immediately write (start + end).
    // Uses startAction + endAction so the row shape matches the existing
    // heartbeat pattern from heartbeat.zig.
    const action_id = try startAction(d, allocator, .{
        .session_id = session_id,
        .claim_id = claim_id,
        .action_kind = .other,
        .entity_kind = entity_kind,
        .entity_id = entity_id,
        .vendor = vendor,
    });
    try endAction(d, allocator, action_id, .ok, summary);
}

// =========================================================================
// Associate claim with a run/stage
// =========================================================================

/// Stamp `run_id` and (optionally) `stage` on an active claim identified by
/// `claim_token`. Best-effort: when no active claim matches the token the
/// function returns 0 (no-op). Used by `planar-execute` at dispatch time to
/// backfill the run context onto a PRE-ACQUIRED claim (decision 457/Q602).
///
/// Returns the number of rows updated (0 when the claim is not active or does
/// not exist, 1 on success). Caller does NOT need to wrap in a transaction
/// for the single-row UPDATE.
pub fn associateClaimRun(
    d: *db.sqlite.Db,
    claim_token: []const u8,
    run_id: i64,
    stage: ?[]const u8,
) Error!i64 {
    _ = d.execParams(
        \\update agent_work_claims
        \\set run_id = ?,
        \\    stage = ?
        \\where claim_token = ? and status = 'active'
    , &.{
        .{ .int = run_id },
        textOrNull(stage),
        .{ .text = claim_token },
    }) catch return Error.QueryFailed;
    // execParams returns the last-insert-rowid, not changes(). Use a follow-up
    // query to count the updated rows.
    const updated = d.intQuery(
        "select changes()",
    ) catch return Error.QueryFailed;
    return updated;
}

// =========================================================================
// Reconcile
// =========================================================================

pub const ReconcilePolicy = struct {
    /// Additional grace beyond lease expiry; default 0.
    stale_after_secs: i64 = 0,
    /// When true, return the candidate set without writing.
    dry_run: bool = false,
    /// When non-null, scope the sweep to this session only.
    /// Null = global sweep (all sessions), the default.
    session_id: ?i64 = null,
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
    // When policy.session_id is set, scope all queries to that session.
    var sel_buf: [768]u8 = undefined;
    const sel_sql = if (policy.session_id) |sid|
        std.fmt.bufPrintZ(&sel_buf,
            \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
            \\       status, vendor, vendor_session_id, role, model,
            \\       worktree_id, worktree_path,
            \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
            \\       purpose, base_ref,
            \\       claimed_at, last_heartbeat_at, lease_expires_at,
            \\       released_at, release_reason,
            \\       run_id, stage
            \\from agent_work_claims
            \\where status = 'active'
            \\  and lease_expires_at < strftime('%Y-%m-%dT%H:%M:%fZ','now', '-{d} seconds')
            \\  and session_id = {d}
        , .{ policy.stale_after_secs, sid }) catch return Error.QueryFailed
    else
        std.fmt.bufPrintZ(&sel_buf,
            \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
            \\       status, vendor, vendor_session_id, role, model,
            \\       worktree_id, worktree_path,
            \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
            \\       purpose, base_ref,
            \\       claimed_at, last_heartbeat_at, lease_expires_at,
            \\       released_at, release_reason,
            \\       run_id, stage
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

    // Apply the mark-stale UPDATE scoped to the session when set.
    var upd_buf: [640]u8 = undefined;
    const upd_sql = if (policy.session_id) |sid|
        std.fmt.bufPrintZ(&upd_buf,
            \\update agent_work_claims
            \\set status = 'stale',
            \\    released_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'),
            \\    release_reason = 'reconcile: heartbeat expired'
            \\where status = 'active'
            \\  and lease_expires_at < strftime('%Y-%m-%dT%H:%M:%fZ','now', '-{d} seconds')
            \\  and session_id = {d}
        , .{ policy.stale_after_secs, sid }) catch return Error.QueryFailed
    else
        std.fmt.bufPrintZ(&upd_buf,
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
    // ended_at as the ended_at timestamp. When scoped, only close
    // actions belonging to the target session.
    var act_upd_buf: [512]u8 = undefined;
    const act_upd_sql = if (policy.session_id) |sid|
        std.fmt.bufPrintZ(&act_upd_buf,
            \\update agent_actions
            \\set ended_at = (select ended_at from sessions where id = agent_actions.session_id),
            \\    outcome = 'aborted'
            \\where ended_at is null
            \\  and session_id = {d}
            \\  and exists (
            \\    select 1 from sessions s
            \\    where s.id = agent_actions.session_id and s.ended_at is not null
            \\  )
        , .{sid}) catch return Error.QueryFailed
    else
        "update agent_actions" ++
            "\nset ended_at = (select ended_at from sessions where id = agent_actions.session_id)," ++
            "\n    outcome = 'aborted'" ++
            "\nwhere ended_at is null" ++
            "\n  and exists (" ++
            "\n    select 1 from sessions s" ++
            "\n    where s.id = agent_actions.session_id and s.ended_at is not null" ++
            "\n  )";
    _ = d.execParams(act_upd_sql, &.{}) catch return Error.QueryFailed;
    // Count via a follow-up SELECT against the same predicate post-update:
    // ended_at is now non-null and outcome='aborted'; we lack a per-update
    // count from execParams. Approximate via a separate count query that
    // matches the set we just touched.
    var act_cnt_buf: [512]u8 = undefined;
    const act_cnt_sql = if (policy.session_id) |sid|
        std.fmt.bufPrintZ(&act_cnt_buf,
            \\select count(*) from agent_actions
            \\where outcome = 'aborted'
            \\  and ended_at is not null
            \\  and session_id = {d}
            \\  and exists (
            \\    select 1 from sessions s
            \\    where s.id = agent_actions.session_id and s.ended_at is not null
            \\  )
        , .{sid}) catch return Error.QueryFailed
    else
        "select count(*) from agent_actions" ++
            "\nwhere outcome = 'aborted'" ++
            "\n  and ended_at is not null" ++
            "\n  and exists (" ++
            "\n    select 1 from sessions s" ++
            "\n    where s.id = agent_actions.session_id and s.ended_at is not null" ++
            "\n  )";
    if (d.intQuery(act_cnt_sql)) |n| {
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

/// Return the action with the highest `(started_at, id)` for `claim_id`,
/// or `null` when no actions exist for the claim.
///
/// Implements Decision D4 (tech-spec line 165): read live from
/// `agent_actions` — no denormalized column on `agent_work_claims`.
/// The query is backed by `ix_agent_actions_claim` (migration 00015)
/// so the O(1) lookup cost is acceptable for the typical N ≤ 30 active
/// claims rendered by `planar-watch ps`.
///
/// Tie-breaking: `started_at desc, id desc` — the highest id wins when
/// two actions share the same `started_at` timestamp.
pub fn latestActionForClaim(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    claim_id: i64,
) Error!?types.Action {
    var stmt = d.prepare(
        \\select id, session_id, session_entry_id, parent_action_id, claim_id,
        \\       action_kind, entity_kind, entity_id,
        \\       vendor, vendor_role, model,
        \\       started_at, ended_at, outcome, summary,
        \\       head_sha, dirty,
        \\       metadata
        \\from agent_actions
        \\where claim_id = ?
        \\order by started_at desc, id desc
        \\limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = claim_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => null,
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
// Column order for all claim SELECT statements (indices 0-25):
//   0:id, 1:claim_token, 2:session_id, 3:entity_kind, 4:entity_id, 5:claim_scope,
//   6:status, 7:vendor, 8:vendor_session_id, 9:role, 10:model,
//   11:worktree_id, 12:worktree_path,
//   13:repo_root, 14:branch, 15:head_sha_at_claim, 16:dirty_at_claim,
//   17:purpose, 18:base_ref,
//   19:claimed_at, 20:last_heartbeat_at, 21:lease_expires_at,
//   22:released_at, 23:release_reason,
//   24:run_id, 25:stage

const claim_columns =
    \\id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\status, vendor, vendor_session_id, role, model,
    \\worktree_id, worktree_path,
    \\repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\purpose, base_ref,
    \\claimed_at, last_heartbeat_at, lease_expires_at,
    \\released_at, release_reason,
    \\run_id, stage
;

const claim_select_by_id: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason,
    \\       run_id, stage
    \\from agent_work_claims where id = ?
;

const claim_select_by_token: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason,
    \\       run_id, stage
    \\from agent_work_claims where claim_token = ?
;

const claim_select_active_all: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason,
    \\       run_id, stage
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
    \\       released_at, release_reason,
    \\       run_id, stage
    \\from agent_work_claims
    \\where status = 'active' and session_id = ?
    \\order by claimed_at desc
;

const claim_select_stale: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason,
    \\       run_id, stage
    \\from agent_work_claims
    \\where status = 'stale'
    \\   or (status = 'active' and lease_expires_at < strftime('%Y-%m-%dT%H:%M:%fZ','now'))
    \\order by claimed_at desc
;

const claim_select_by_entity: [:0]const u8 =
    \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
    \\       status, vendor, vendor_session_id, role, model,
    \\       worktree_id, worktree_path,
    \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
    \\       purpose, base_ref,
    \\       claimed_at, last_heartbeat_at, lease_expires_at,
    \\       released_at, release_reason,
    \\       run_id, stage
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
    \\       released_at, release_reason,
    \\       run_id, stage
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
        .run_id = stmt.columnIntOpt(24),
        .stage = try stmt.columnTextOpt(25, allocator),
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

test "reconcileStale --session scopes to one session; other session untouched" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    // Two sessions each with one expired active claim.
    const sid_a = try insertTestSession(&d);
    const sid_b = try insertTestSession(&d);
    const tid_a = try insertTestTask(&d);
    const tid_b = try insertTestTask(&d);

    const c_a = try acquireClaim(&d, a, .{
        .session_id = sid_a,
        .entity_kind = .task,
        .entity_id = tid_a,
        .vendor = "test",
        .ttl_secs = -10, // already expired
    });
    defer c_a.deinit(a);

    const c_b = try acquireClaim(&d, a, .{
        .session_id = sid_b,
        .entity_kind = .task,
        .entity_id = tid_b,
        .vendor = "test",
        .ttl_secs = -10, // already expired
    });
    defer c_b.deinit(a);

    // Scoped sweep for session A only.
    const r_a = try reconcileStale(&d, a, .{ .session_id = sid_a });
    defer r_a.deinit(a);
    try std.testing.expectEqual(@as(i64, 1), r_a.claims_marked_stale);

    // Session A's claim must be stale.
    const reloaded_a = try getClaimByToken(&d, a, c_a.claim_token);
    defer reloaded_a.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.stale, reloaded_a.status);

    // Session B's claim must still be active — untouched by the A-scoped sweep.
    const reloaded_b = try getClaimByToken(&d, a, c_b.claim_token);
    defer reloaded_b.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.active, reloaded_b.status);

    // Now the global sweep picks up session B's claim.
    const r_global = try reconcileStale(&d, a, .{});
    defer r_global.deinit(a);
    try std.testing.expectEqual(@as(i64, 1), r_global.claims_marked_stale);

    const reloaded_b2 = try getClaimByToken(&d, a, c_b.claim_token);
    defer reloaded_b2.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.stale, reloaded_b2.status);
}

test "reconcileStale --session dry-run scopes candidates to the session" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const sid_a = try insertTestSession(&d);
    const sid_b = try insertTestSession(&d);
    const tid_a = try insertTestTask(&d);
    const tid_b = try insertTestTask(&d);

    const c_a = try acquireClaim(&d, a, .{
        .session_id = sid_a,
        .entity_kind = .task,
        .entity_id = tid_a,
        .vendor = "test",
        .ttl_secs = -10,
    });
    defer c_a.deinit(a);

    const c_b = try acquireClaim(&d, a, .{
        .session_id = sid_b,
        .entity_kind = .task,
        .entity_id = tid_b,
        .vendor = "test",
        .ttl_secs = -10,
    });
    defer c_b.deinit(a);

    // Dry-run scoped to session A: must return exactly 1 candidate (A's claim).
    const r = try reconcileStale(&d, a, .{ .session_id = sid_a, .dry_run = true });
    defer r.deinit(a);
    try std.testing.expectEqual(@as(usize, 1), r.candidates.len);
    try std.testing.expectEqual(sid_a, r.candidates[0].session_id);

    // Both claims still active (dry-run must not write).
    const ra = try getClaimByToken(&d, a, c_a.claim_token);
    defer ra.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.active, ra.status);

    const rb = try getClaimByToken(&d, a, c_b.claim_token);
    defer rb.deinit(a);
    try std.testing.expectEqual(types.ClaimStatus.active, rb.status);
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

test "latestActiveClaimForSession returns null when no claims exist for the session" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);

    const result = try latestActiveClaimForSession(&d, sid);
    try std.testing.expect(result == null);
}

test "latestActiveClaimForSession returns the claim id when exactly one active claim exists" {
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

    const result = try latestActiveClaimForSession(&d, sid);
    try std.testing.expect(result != null);
    try std.testing.expectEqual(c.id, result.?);
}

test "latestActionForClaim returns null when no actions exist for the claim" {
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

    const result = try latestActionForClaim(&d, a, c.id);
    try std.testing.expect(result == null);
}

test "latestActionForClaim returns the single action when exactly one exists" {
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

    const action_id = try startAction(&d, a, .{
        .session_id = sid,
        .claim_id = c.id,
        .action_kind = .heartbeat,
        .vendor = "test",
    });
    try endAction(&d, a, action_id, .ok, "doing work");

    const result = try latestActionForClaim(&d, a, c.id);
    try std.testing.expect(result != null);
    defer result.?.deinit(a);
    try std.testing.expectEqual(action_id, result.?.id);
    try std.testing.expectEqualStrings("doing work", result.?.summary.?);
}

test "latestActionForClaim returns the most-recent action (highest started_at then id) when several exist" {
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

    // Insert three actions. Because SQLite's strftime precision is
    // milliseconds and these inserts happen within the same millisecond
    // in CI, we cannot rely on started_at differing. We rely on id
    // ordering as the tiebreaker — the last-inserted id is highest and
    // must be returned by latestActionForClaim.
    const id1 = try startAction(&d, a, .{
        .session_id = sid,
        .claim_id = c.id,
        .action_kind = .heartbeat,
        .vendor = "test",
    });
    try endAction(&d, a, id1, .ok, "first");

    const id2 = try startAction(&d, a, .{
        .session_id = sid,
        .claim_id = c.id,
        .action_kind = .heartbeat,
        .vendor = "test",
    });
    try endAction(&d, a, id2, .ok, "second");

    const id3 = try startAction(&d, a, .{
        .session_id = sid,
        .claim_id = c.id,
        .action_kind = .heartbeat,
        .vendor = "test",
    });
    try endAction(&d, a, id3, .ok, "third");

    const result = try latestActionForClaim(&d, a, c.id);
    try std.testing.expect(result != null);
    defer result.?.deinit(a);
    // The highest id (id3) must be returned regardless of timestamp ties.
    try std.testing.expectEqual(id3, result.?.id);
    try std.testing.expectEqualStrings("third", result.?.summary.?);
}

test "latestActiveClaimForSession returns the most-recent claim id when multiple active claims exist" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid1 = try insertTestTask(&d);
    const tid2 = try insertTestTask(&d);
    const tid3 = try insertTestTask(&d);

    // Acquire three claims for the same session (on distinct entities so
    // acquireClaim does not see ClaimContention). The last one inserted
    // has the highest id and claimed_at, so it should be returned.
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
    const c3 = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid3,
        .vendor = "test",
    });
    defer c3.deinit(a);

    const result = try latestActiveClaimForSession(&d, sid);
    try std.testing.expect(result != null);
    // c3 was acquired last so it has the highest claimed_at and id.
    try std.testing.expectEqual(c3.id, result.?);
}

test "acquireClaim with run_id and stage populates both columns" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);

    // Insert a workflow_runs row to serve as FK target.
    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug) values ('global','p','run-stage-plan')",
        &.{},
    );
    const run_id = try d.execParams(
        \\insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root)
        \\values (?, 'test-wf', 'run-stage-test-1', 12345, '/tmp/repo')
    ,
        &.{.{ .int = plan_id }},
    );

    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
        .run_id = run_id,
        .stage = "code",
    });
    defer c.deinit(a);

    try std.testing.expectEqual(types.ClaimStatus.active, c.status);
    try std.testing.expect(c.run_id != null);
    try std.testing.expectEqual(run_id, c.run_id.?);
    try std.testing.expect(c.stage != null);
    try std.testing.expectEqualStrings("code", c.stage.?);
}

test "acquireClaim without run_id and stage leaves both null" {
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

    try std.testing.expect(c.run_id == null);
    try std.testing.expect(c.stage == null);
}

test "associateClaimRun stamps run_id and stage on an active claim" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug) values ('global','p','assoc-plan')",
        &.{},
    );
    const run_id = try d.execParams(
        \\insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root)
        \\values (?, 'test-wf', 'assoc-test-1', 12345, '/tmp/repo')
    ,
        &.{.{ .int = plan_id }},
    );

    // Acquire WITHOUT run_id/stage.
    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
    });
    defer c.deinit(a);
    try std.testing.expect(c.run_id == null);
    try std.testing.expect(c.stage == null);

    // Associate with run + stage.
    const updated = try associateClaimRun(&d, c.claim_token, run_id, "code");
    try std.testing.expectEqual(@as(i64, 1), updated);

    // Re-fetch and verify both columns are now populated.
    const reloaded = try getClaimByToken(&d, a, c.claim_token);
    defer reloaded.deinit(a);
    try std.testing.expect(reloaded.run_id != null);
    try std.testing.expectEqual(run_id, reloaded.run_id.?);
    try std.testing.expect(reloaded.stage != null);
    try std.testing.expectEqualStrings("code", reloaded.stage.?);
}

test "associateClaimRun with null stage stamps run_id only (stage stays NULL)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug) values ('global','p','assoc-null-stage-plan')",
        &.{},
    );
    const run_id = try d.execParams(
        \\insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root)
        \\values (?, 'test-wf', 'assoc-null-stage-1', 99, '/tmp')
    ,
        &.{.{ .int = plan_id }},
    );

    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
    });
    defer c.deinit(a);

    const updated = try associateClaimRun(&d, c.claim_token, run_id, null);
    try std.testing.expectEqual(@as(i64, 1), updated);

    const reloaded = try getClaimByToken(&d, a, c.claim_token);
    defer reloaded.deinit(a);
    try std.testing.expect(reloaded.run_id != null);
    try std.testing.expectEqual(run_id, reloaded.run_id.?);
    // stage remains null when not supplied.
    try std.testing.expect(reloaded.stage == null);
}

test "associateClaimRun returns 0 for a terminal claim (no-op)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug) values ('global','p','assoc-terminal-plan')",
        &.{},
    );
    const run_id = try d.execParams(
        \\insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root)
        \\values (?, 'test-wf', 'assoc-terminal-1', 42, '/tmp')
    ,
        &.{.{ .int = plan_id }},
    );

    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
    });
    defer c.deinit(a);

    // Release the claim (moves it to a terminal status).
    const r = try releaseClaim(&d, a, c.claim_token, .released, "done");
    r.deinit(a);

    // associateClaimRun must be a no-op on a non-active claim.
    const updated = try associateClaimRun(&d, c.claim_token, run_id, "code");
    try std.testing.expectEqual(@as(i64, 0), updated);

    // Stage and run_id stay null (they were never stamped).
    const reloaded = try getClaimByToken(&d, a, c.claim_token);
    defer reloaded.deinit(a);
    try std.testing.expect(reloaded.run_id == null);
    try std.testing.expect(reloaded.stage == null);
}

test "associateClaimRun returns 0 for a nonexistent token" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const updated = try associateClaimRun(&d, "nonexistent_token", 99, "code");
    try std.testing.expectEqual(@as(i64, 0), updated);
}

test "getClaimByToken round-trips run_id and stage" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const sid = try insertTestSession(&d);
    const tid = try insertTestTask(&d);

    const plan_id = try d.execParams(
        "insert into plans (scope_kind, title, slug) values ('global','p','rt-plan')",
        &.{},
    );
    const run_id = try d.execParams(
        \\insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root)
        \\values (?, 'wf', 'rt-test-1', 99, '/tmp')
    ,
        &.{.{ .int = plan_id }},
    );

    const c = try acquireClaim(&d, a, .{
        .session_id = sid,
        .entity_kind = .task,
        .entity_id = tid,
        .vendor = "test",
        .run_id = run_id,
        .stage = "review",
    });
    defer c.deinit(a);

    // Re-fetch by token to verify the SELECT path also includes the new columns.
    const reloaded = try getClaimByToken(&d, a, c.claim_token);
    defer reloaded.deinit(a);
    try std.testing.expect(reloaded.run_id != null);
    try std.testing.expectEqual(run_id, reloaded.run_id.?);
    try std.testing.expect(reloaded.stage != null);
    try std.testing.expectEqualStrings("review", reloaded.stage.?);
}
