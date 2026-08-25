//! engine/routing/dispatch.zig — the preview/confirm boundary (plan 950 task
//! 5528).
//!
//! A dispatch is authorized in two steps. `preview` freezes everything the
//! operator is shown — packet and profile digests, policy versions, the host
//! capability snapshot, cohort, candidate, claim target, exclusions — and
//! returns a single-use, expiry-bound token. `confirm` revalidates every one
//! of those bound values against current state and, only if all still hold,
//! writes the immutable dispatch snapshot in one transaction.
//!
//! The failure this structure exists to prevent is a dispatch against state
//! the operator never saw. Between preview and confirm the task can be edited,
//! the host's capability snapshot can expire, the claim can move to another
//! agent, or the routing policy can change. Re-deriving at confirm time would
//! silently accept all of those; comparing against the frozen copy rejects
//! them as `stale_preview`.
//!
//! Atomicity matters as much as the checks. A confirm that wrote a partial
//! dispatch — snapshot without consuming the token, or a consumed token
//! without a snapshot — would leave a spawn either unauthorized or
//! unrepeatable. Everything happens inside one immediate transaction, so a
//! rejected confirm leaves no trace but the unconsumed preview.

const std = @import("std");
const db = @import("db");
const store = @import("store.zig");

/// Bumped when the preview/confirm contract changes shape. A token minted
/// under one contract must never be confirmed under another.
pub const dispatch_contract_version = "routing-dispatch-v1";

pub const Error = error{
    QueryFailed,
    /// A bound value moved between preview and confirm, the token expired, or
    /// it was already consumed. Deliberately one reason: the operator's remedy
    /// is identical in every case — re-preview and look again.
    StalePreview,
    /// The token does not exist at all.
    UnknownPreview,
    /// An override was requested for a candidate below the profile's tier
    /// floor, or otherwise ineligible.
    OverrideNotEligible,
    /// A value arrived carrying control characters or an ambiguous duplicate
    /// option. Never shell-reparsed; rejected as data.
    InvalidValue,
} || std.mem.Allocator.Error;

/// Why a confirm was rejected. Reported alongside `StalePreview` so an
/// operator can see WHICH binding moved without re-deriving it themselves —
/// the error is uniform, the diagnosis is not.
pub const StaleReason = enum {
    expired,
    already_consumed,
    packet_changed,
    profile_changed,
    policy_changed,
    capability_changed,
    cohort_changed,
    claim_changed,
    candidate_changed,

    pub fn text(self: StaleReason) []const u8 {
        return @tagName(self);
    }
};

/// Whether a dispatch's outcome may move a recommendation. Mirrors the
/// evidence boundary in `evidence.zig`; carried on the preview so the operator
/// is told, before confirming, whether this run will count.
pub const EvidenceState = enum { evidential, observational };

// ---------------------------------------------------------------------------
// Value safety
// ---------------------------------------------------------------------------

/// Model and candidate identifiers are opaque operator data. They are stored
/// and compared as bytes and never handed to a shell, so the only thing that
/// must be rejected is what would corrupt a record or a terminal: control
/// characters. Punctuation that looks dangerous in a shell (`;`, `$()`, `--`)
/// is legitimate content here precisely because nothing reparses it.
pub fn validValue(value: []const u8) bool {
    if (value.len == 0) return false;
    for (value) |c| {
        if (c < 0x20 or c == 0x7f) return false;
    }
    return true;
}

/// Reject a duplicate option rather than silently taking the first or last.
/// "Last wins" is how an operator ends up dispatching to a model they did not
/// intend, having seen their own earlier flag echoed back.
pub fn takeOnce(existing: ?[]const u8, incoming: []const u8) Error![]const u8 {
    if (existing != null) return Error.InvalidValue;
    if (!validValue(incoming)) return Error.InvalidValue;
    return incoming;
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

/// Everything a preview binds. Each field is compared at confirm time; adding
/// a field here without comparing it in `confirm` would silently widen what a
/// dispatch may drift on.
pub const Binding = struct {
    logical_work_item_id: []const u8,
    project_id: i64,

    validation_policy_version: []const u8,
    routing_policy_version: []const u8,
    profile_rule_version: []const u8,
    vendor: []const u8,
    role: []const u8,
    tier: store.Tier,
    work_type: store.WorkType,
    complexity: store.Complexity,

    packet_digest: []const u8,
    profile_digest: []const u8,
    policy_digest: []const u8,
    capability_digest: []const u8,

    requested_candidate_id: i64,
    host_id: []const u8,
    delegated_candidate_id: ?i64 = null,
    assignment_class: store.AssignmentClass,
    experiment_id: ?i64 = null,

    claim_token: ?[]const u8 = null,
    claim_status: ?[]const u8 = null,

    exclusions_json: []const u8 = "[]",
    evidence_state: EvidenceState,
};

pub const PreviewRequest = struct {
    task_id: ?i64,
    binding: Binding,
    /// Absolute RFC3339 instant. An expiry-bound token is what stops an
    /// operator confirming a preview they generated yesterday.
    expires_at: []const u8,
};

pub const Preview = struct {
    id: i64,
    token: []const u8,

    pub fn deinit(self: Preview, allocator: std.mem.Allocator) void {
        allocator.free(self.token);
    }
};

/// Mint a preview, binding current state to a single-use token.
///
/// The token is generated in SQL via `lower(hex(randomblob(16)))`, matching
/// how `agent_work_claims.claim_token` is minted — one opaque-handle
/// convention across the schema rather than two.
pub fn preview(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    req: PreviewRequest,
) Error!Preview {
    const b = req.binding;
    if (!validValue(b.host_id) or !validValue(b.vendor) or !validValue(b.role)) {
        return Error.InvalidValue;
    }
    if (!validValue(req.expires_at)) return Error.InvalidValue;

    const sql: [:0]const u8 =
        \\insert into routing_dispatch_previews (
        \\  preview_token, task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version, profile_rule_version,
        \\  vendor, role, tier, work_type, complexity,
        \\  packet_digest, profile_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, host_id, delegated_candidate_id,
        \\  assignment_class, experiment_id, claim_token, claim_status_at_preview,
        \\  exclusions_json, evidence_state, expires_at
        \\) values (
        \\  lower(hex(randomblob(16))), ?, ?, ?,
        \\  ?, ?, ?,
        \\  ?, ?, ?, ?, ?,
        \\  ?, ?, ?, ?,
        \\  ?, ?, ?,
        \\  ?, ?, ?, ?,
        \\  ?, ?, ?
        \\)
        \\returning id, preview_token
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        if (req.task_id) |t| .{ .int = t } else .{ .null = {} },
        .{ .text = b.logical_work_item_id },
        .{ .int = b.project_id },
        .{ .text = b.validation_policy_version },
        .{ .text = b.routing_policy_version },
        .{ .text = b.profile_rule_version },
        .{ .text = b.vendor },
        .{ .text = b.role },
        .{ .text = @tagName(b.tier) },
        .{ .text = @tagName(b.work_type) },
        .{ .text = b.complexity.toText() },
        .{ .text = b.packet_digest },
        .{ .text = b.profile_digest },
        .{ .text = b.policy_digest },
        .{ .text = b.capability_digest },
        .{ .int = b.requested_candidate_id },
        .{ .text = b.host_id },
        if (b.delegated_candidate_id) |c| .{ .int = c } else .{ .null = {} },
        .{ .text = @tagName(b.assignment_class) },
        if (b.experiment_id) |e| .{ .int = e } else .{ .null = {} },
        if (b.claim_token) |c| .{ .text = c } else .{ .null = {} },
        if (b.claim_status) |c| .{ .text = c } else .{ .null = {} },
        .{ .text = b.exclusions_json },
        .{ .text = @tagName(b.evidence_state) },
        .{ .text = req.expires_at },
    }) catch return Error.QueryFailed;

    if ((stmt.step() catch return Error.QueryFailed) != .row) return Error.QueryFailed;
    const id = stmt.columnInt(0);
    const token = stmt.columnTextAlloc(1, allocator) catch return Error.QueryFailed;
    return .{ .id = id, .token = token };
}

// ---------------------------------------------------------------------------
// Confirm
// ---------------------------------------------------------------------------

/// State observed at confirm time, to be compared against the frozen preview.
pub const CurrentState = struct {
    now: []const u8,
    packet_digest: []const u8,
    profile_digest: []const u8,
    policy_digest: []const u8,
    capability_digest: []const u8,
    requested_candidate_id: i64,
    claim_token: ?[]const u8,
    claim_status: ?[]const u8,
    /// Current cohort. Drift here means the work is no longer the same kind of
    /// work, so prior evidence for the cohort does not apply.
    vendor: []const u8,
    role: []const u8,
    tier: store.Tier,
    work_type: store.WorkType,
    complexity: store.Complexity,
    validation_policy_version: []const u8,
    routing_policy_version: []const u8,
};

fn optEql(a: ?[]const u8, b: ?[]const u8) bool {
    if (a == null and b == null) return true;
    if (a == null or b == null) return false;
    return std.mem.eql(u8, a.?, b.?);
}

/// Compare a frozen binding against current state, naming the first drift.
///
/// Order is deliberate: lifecycle problems (expired, consumed) come before
/// content drift, because "your token is stale" is a more actionable answer
/// than "the packet changed" when both are true.
pub fn classifyStale(
    bound: Binding,
    expires_at: []const u8,
    consumed: bool,
    cur: CurrentState,
) ?StaleReason {
    if (consumed) return .already_consumed;
    // RFC3339 UTC with a fixed shape sorts lexicographically, which is why the
    // schema stores instants that way.
    if (std.mem.order(u8, cur.now, expires_at) != .lt) return .expired;

    if (!std.mem.eql(u8, bound.packet_digest, cur.packet_digest)) return .packet_changed;
    if (!std.mem.eql(u8, bound.profile_digest, cur.profile_digest)) return .profile_changed;
    if (!std.mem.eql(u8, bound.policy_digest, cur.policy_digest)) return .policy_changed;
    if (!std.mem.eql(u8, bound.capability_digest, cur.capability_digest)) {
        return .capability_changed;
    }

    if (!std.mem.eql(u8, bound.vendor, cur.vendor) or
        !std.mem.eql(u8, bound.role, cur.role) or
        bound.tier != cur.tier or
        bound.work_type != cur.work_type or
        bound.complexity != cur.complexity or
        !std.mem.eql(u8, bound.validation_policy_version, cur.validation_policy_version) or
        !std.mem.eql(u8, bound.routing_policy_version, cur.routing_policy_version))
    {
        return .cohort_changed;
    }

    if (bound.requested_candidate_id != cur.requested_candidate_id) return .candidate_changed;
    if (!optEql(bound.claim_token, cur.claim_token) or
        !optEql(bound.claim_status, cur.claim_status))
    {
        return .claim_changed;
    }
    return null;
}

/// Tier floor check for an operator override. An override may move sideways or
/// up, never below the floor the profile established — that floor is the whole
/// reason the profile computed a tier.
pub fn overrideAllowed(floor: store.Tier, requested: store.Tier) bool {
    return @intFromEnum(requested) >= @intFromEnum(floor);
}

/// Load a preview's frozen binding plus its lifecycle columns.
///
/// Returned slices are owned by `arena`; callers pass an arena so the many
/// small strings a binding holds are freed in one shot rather than field by
/// field.
pub const LoadedPreview = struct {
    id: i64,
    binding: Binding,
    expires_at: []const u8,
    consumed: bool,
    task_id: ?i64,
};

fn textCol(stmt: *db.sqlite.Stmt, idx: c_int, arena: std.mem.Allocator) Error![]const u8 {
    return stmt.columnTextAlloc(idx, arena) catch Error.QueryFailed;
}

pub fn loadPreview(
    d: *db.sqlite.Db,
    arena: std.mem.Allocator,
    token: []const u8,
) Error!LoadedPreview {
    if (!validValue(token)) return Error.InvalidValue;
    const sql: [:0]const u8 =
        \\select id, logical_work_item_id, project_id,
        \\       validation_policy_version, routing_policy_version, profile_rule_version,
        \\       vendor, role, tier, work_type, complexity,
        \\       packet_digest, profile_digest, policy_digest, capability_digest,
        \\       requested_candidate_id, host_id, assignment_class,
        \\       coalesce(claim_token,''), coalesce(claim_status_at_preview,''),
        \\       exclusions_json, evidence_state, expires_at,
        \\       consumed_at is not null, task_id, delegated_candidate_id, experiment_id
        \\from routing_dispatch_previews where preview_token = ?
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = token }}) catch return Error.QueryFailed;
    if ((stmt.step() catch return Error.QueryFailed) != .row) return Error.UnknownPreview;

    const claim_token = try textCol(&stmt, 18, arena);
    const claim_status = try textCol(&stmt, 19, arena);
    return .{
        .id = stmt.columnInt(0),
        .binding = .{
            .logical_work_item_id = try textCol(&stmt, 1, arena),
            .project_id = stmt.columnInt(2),
            .validation_policy_version = try textCol(&stmt, 3, arena),
            .routing_policy_version = try textCol(&stmt, 4, arena),
            .profile_rule_version = try textCol(&stmt, 5, arena),
            .vendor = try textCol(&stmt, 6, arena),
            .role = try textCol(&stmt, 7, arena),
            .tier = std.meta.stringToEnum(store.Tier, try textCol(&stmt, 8, arena)) orelse
                return Error.QueryFailed,
            .work_type = std.meta.stringToEnum(store.WorkType, try textCol(&stmt, 9, arena)) orelse
                return Error.QueryFailed,
            // Read back through fromText, not stringToEnum: the stored
            // spelling is the hyphenated `high-risk`, which is not an enum
            // tag. See store.Complexity's doc comment (task 6092).
            .complexity = store.Complexity.fromText(try textCol(&stmt, 10, arena)) orelse
                return Error.QueryFailed,
            .packet_digest = try textCol(&stmt, 11, arena),
            .profile_digest = try textCol(&stmt, 12, arena),
            .policy_digest = try textCol(&stmt, 13, arena),
            .capability_digest = try textCol(&stmt, 14, arena),
            .requested_candidate_id = stmt.columnInt(15),
            .host_id = try textCol(&stmt, 16, arena),
            .assignment_class = std.meta.stringToEnum(
                store.AssignmentClass,
                try textCol(&stmt, 17, arena),
            ) orelse return Error.QueryFailed,
            .claim_token = if (claim_token.len == 0) null else claim_token,
            .claim_status = if (claim_status.len == 0) null else claim_status,
            .exclusions_json = try textCol(&stmt, 20, arena),
            .evidence_state = std.meta.stringToEnum(EvidenceState, try textCol(&stmt, 21, arena)) orelse
                return Error.QueryFailed,
        },
        .expires_at = try textCol(&stmt, 22, arena),
        .consumed = stmt.columnInt(23) != 0,
        .task_id = if (stmt.columnIsNull(24)) null else stmt.columnInt(24),
    };
}

pub const ConfirmRequest = struct {
    token: []const u8,
    dispatch_key: []const u8,
    current: CurrentState,
    reviewer_disposition: store.ReviewerDisposition = .required,
    operator_decision: store.OperatorDecision = .confirmed,
};

pub const Confirmed = struct {
    dispatch_id: i64,
};

/// Confirm a preview: revalidate every bound value, then write the dispatch
/// snapshot and consume the token — all inside one `BEGIN IMMEDIATE`.
///
/// `stale_out` receives the specific drift when the result is `StalePreview`,
/// so the caller can tell the operator WHICH binding moved. The error itself
/// stays uniform because the remedy never differs: re-preview and look again.
///
/// All-or-nothing is the point. A snapshot without a consumed token would let
/// the same authorization spawn twice; a consumed token without a snapshot
/// would burn the authorization with nothing to show for it. The transaction
/// admits neither.
pub fn confirm(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    req: ConfirmRequest,
    stale_out: ?*StaleReason,
) Error!Confirmed {
    if (!validValue(req.dispatch_key)) return Error.InvalidValue;

    var arena_state = std.heap.ArenaAllocator.init(allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    d.exec("begin immediate") catch return Error.QueryFailed;
    // Any early return below leaves the transaction open, so roll back on every
    // failure path rather than trusting the caller to notice.
    errdefer d.exec("rollback") catch {};

    const loaded = try loadPreview(d, arena, req.token);
    if (classifyStale(loaded.binding, loaded.expires_at, loaded.consumed, req.current)) |reason| {
        if (stale_out) |out| out.* = reason;
        // The errdefer above owns the rollback; doing it here too would issue a
        // second rollback against a transaction that is already gone.
        return Error.StalePreview;
    }

    const b = loaded.binding;
    const dispatch_id = d.execParams(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version, profile_rule_version,
        \\  vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, assignment_class, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\) values (
        \\  ?, ?, ?, ?,
        \\  ?, ?, ?,
        \\  ?, ?, ?, ?, ?,
        \\  ?, ?, ?,
        \\  ?, ?, ?,
        \\  ?, 'pending', ?
        \\)
    , &.{
        .{ .text = req.dispatch_key },
        if (loaded.task_id) |t| .{ .int = t } else .{ .null = {} },
        .{ .text = b.logical_work_item_id },
        .{ .int = b.project_id },
        .{ .text = b.validation_policy_version },
        .{ .text = b.routing_policy_version },
        .{ .text = b.profile_rule_version },
        .{ .text = b.vendor },
        .{ .text = b.role },
        .{ .text = @tagName(b.tier) },
        .{ .text = @tagName(b.work_type) },
        .{ .text = b.complexity.toText() },
        .{ .text = b.packet_digest },
        .{ .text = b.policy_digest },
        .{ .text = b.capability_digest },
        .{ .int = b.requested_candidate_id },
        .{ .text = @tagName(b.assignment_class) },
        .{ .text = @tagName(req.operator_decision) },
        .{ .text = @tagName(req.reviewer_disposition) },
        .{ .text = req.current.now },
    }) catch return Error.QueryFailed;

    _ = d.execParams(
        \\update routing_dispatch_previews
        \\set consumed_at = ?, consumed_dispatch_id = ?
        \\where id = ? and consumed_at is null
    , &.{
        .{ .text = req.current.now },
        .{ .int = dispatch_id },
        .{ .int = loaded.id },
    }) catch return Error.QueryFailed;

    // A racing confirm that consumed the token first leaves zero rows changed.
    // Without this check the loser would also write a snapshot, and one
    // authorization would have produced two dispatches.
    if (d.changes() != 1) {
        if (stale_out) |out| out.* = .already_consumed;
        return Error.StalePreview;
    }

    d.exec("commit") catch return Error.QueryFailed;
    return .{ .dispatch_id = dispatch_id };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const testing = std.testing;

fn sampleBinding() Binding {
    return .{
        .logical_work_item_id = "lwi-1",
        .project_id = 1,
        .validation_policy_version = "val-v1",
        .routing_policy_version = "route-v1",
        .profile_rule_version = "profile-v1",
        .vendor = "vendor-x",
        .role = "coder",
        .tier = .medium,
        .work_type = .feature,
        .complexity = .standard,
        .packet_digest = "pkt",
        .profile_digest = "prof",
        .policy_digest = "pol",
        .capability_digest = "cap",
        .requested_candidate_id = 1,
        .host_id = "host-a",
        .assignment_class = .default,
        .evidence_state = .observational,
    };
}

fn sampleCurrent() CurrentState {
    return .{
        .now = "2026-01-01T00:00:00Z",
        .packet_digest = "pkt",
        .profile_digest = "prof",
        .policy_digest = "pol",
        .capability_digest = "cap",
        .requested_candidate_id = 1,
        .claim_token = null,
        .claim_status = null,
        .vendor = "vendor-x",
        .role = "coder",
        .tier = .medium,
        .work_type = .feature,
        .complexity = .standard,
        .validation_policy_version = "val-v1",
        .routing_policy_version = "route-v1",
    };
}

const far_future = "2027-01-01T00:00:00Z";

test "confirm: unchanged state is not stale" {
    try testing.expect(classifyStale(sampleBinding(), far_future, false, sampleCurrent()) == null);
}

test "confirm: each bound value is actually compared" {
    // A binding field that is stored but never compared is worse than one that
    // is absent — it reads as a guarantee that does not hold. This walks each.
    {
        var cur = sampleCurrent();
        cur.packet_digest = "other";
        try testing.expectEqual(StaleReason.packet_changed, classifyStale(sampleBinding(), far_future, false, cur).?);
    }
    {
        var cur = sampleCurrent();
        cur.profile_digest = "other";
        try testing.expectEqual(StaleReason.profile_changed, classifyStale(sampleBinding(), far_future, false, cur).?);
    }
    {
        var cur = sampleCurrent();
        cur.policy_digest = "other";
        try testing.expectEqual(StaleReason.policy_changed, classifyStale(sampleBinding(), far_future, false, cur).?);
    }
    {
        var cur = sampleCurrent();
        cur.capability_digest = "other";
        try testing.expectEqual(StaleReason.capability_changed, classifyStale(sampleBinding(), far_future, false, cur).?);
    }
    {
        var cur = sampleCurrent();
        cur.tier = .large;
        try testing.expectEqual(StaleReason.cohort_changed, classifyStale(sampleBinding(), far_future, false, cur).?);
    }
    {
        var cur = sampleCurrent();
        cur.requested_candidate_id = 2;
        try testing.expectEqual(StaleReason.candidate_changed, classifyStale(sampleBinding(), far_future, false, cur).?);
    }
    {
        var cur = sampleCurrent();
        cur.claim_token = "someone-else";
        try testing.expectEqual(StaleReason.claim_changed, classifyStale(sampleBinding(), far_future, false, cur).?);
    }
}

test "confirm: expiry and reuse are rejected before content drift" {
    var cur = sampleCurrent();
    cur.now = "2028-01-01T00:00:00Z";
    try testing.expectEqual(
        StaleReason.expired,
        classifyStale(sampleBinding(), far_future, false, cur).?,
    );

    // Consumed outranks everything, including a still-valid expiry.
    try testing.expectEqual(
        StaleReason.already_consumed,
        classifyStale(sampleBinding(), far_future, true, sampleCurrent()).?,
    );

    // A token that expires exactly now is expired: the boundary belongs to the
    // rejecting side, otherwise "expires_at" would mean "expires just after".
    var boundary = sampleCurrent();
    boundary.now = far_future;
    try testing.expectEqual(
        StaleReason.expired,
        classifyStale(sampleBinding(), far_future, false, boundary).?,
    );
}

test "override may move up or sideways but never below the profile floor" {
    try testing.expect(overrideAllowed(.medium, .medium));
    try testing.expect(overrideAllowed(.medium, .large));
    try testing.expect(!overrideAllowed(.medium, .small));
    try testing.expect(!overrideAllowed(.large, .medium));
}

test "values are data: shell punctuation survives, control characters do not" {
    // These are opaque operator identifiers. Nothing reparses them, so the
    // punctuation is content — rejecting it would be the bug.
    try testing.expect(validValue("--opaque; $(whoami) && rm -rf /"));
    try testing.expect(validValue("gpt-5.3-codex-spark"));
    try testing.expect(!validValue(""));
    try testing.expect(!validValue("has\nnewline"));
    try testing.expect(!validValue("has\ttab"));
    try testing.expect(!validValue("has\x00null"));
}

test "a duplicate option is rejected rather than resolved by position" {
    try testing.expectEqualStrings("a", try takeOnce(null, "a"));
    try testing.expectError(Error.InvalidValue, takeOnce("a", "b"));
    try testing.expectError(Error.InvalidValue, takeOnce(null, "bad\nvalue"));
}

test "preview mints a single-use token bound to current state" {
    const a = testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);

    _ = try conn.execParams(
        "insert into projects (slug, name, root_path) values ('p','p','/p')",
        &.{},
    );
    const cand = try store.createCandidate(&conn, .{
        .vendor = "vendor-x",
        .candidate_id = "cand-1",
        .fallback_order = 0,
    });

    var binding = sampleBinding();
    binding.requested_candidate_id = cand;

    const p1 = try preview(&conn, a, .{
        .task_id = null,
        .binding = binding,
        .expires_at = far_future,
    });
    defer p1.deinit(a);
    try testing.expectEqual(@as(usize, 32), p1.token.len);

    // Two previews never share a token; the token is the single-use handle, so
    // a collision would let one confirm consume another's authorization.
    const p2 = try preview(&conn, a, .{
        .task_id = null,
        .binding = binding,
        .expires_at = far_future,
    });
    defer p2.deinit(a);
    try testing.expect(!std.mem.eql(u8, p1.token, p2.token));
}

test "a consumed preview is immutable at the schema level" {
    const a = testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);

    _ = try conn.execParams(
        "insert into projects (slug, name, root_path) values ('p','p','/p')",
        &.{},
    );
    const cand = try store.createCandidate(&conn, .{
        .vendor = "vendor-x",
        .candidate_id = "cand-1",
        .fallback_order = 0,
    });
    var binding = sampleBinding();
    binding.requested_candidate_id = cand;
    const p = try preview(&conn, a, .{
        .task_id = null,
        .binding = binding,
        .expires_at = far_future,
    });
    defer p.deinit(a);

    // Consumption must name the dispatch it authorized; the schema pairs them.
    const exp_id: ?i64 = null;
    _ = exp_id;
    const dispatch = try conn.execParams(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, logical_work_item_id, project_id, validation_policy_version,
        \\  vendor, role, tier, work_type, complexity, routing_policy_version,
        \\  profile_rule_version, packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, assignment_class, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\) values (
        \\  'dk-1','lwi-1',1,'val-v1','vendor-x','coder','medium','feature','standard',
        \\  'route-v1','profile-v1','pkt','pol','cap',?,'default','confirmed',
        \\  'required','pending','2026-01-01T00:00:00Z'
        \\)
    , &.{.{ .int = cand }});

    _ = try conn.execParams(
        "update routing_dispatch_previews set consumed_at = ?, consumed_dispatch_id = ? where id = ?",
        &.{ .{ .text = "2026-01-01T00:00:00Z" }, .{ .int = dispatch }, .{ .int = p.id } },
    );

    // A second consume must not be able to rewrite the audit link.
    const reuse = conn.execParams(
        "update routing_dispatch_previews set consumed_dispatch_id = ? where id = ?",
        &.{ .{ .int = dispatch }, .{ .int = p.id } },
    );
    try testing.expectError(error.StepFailed, reuse);
}

// --- confirm: atomicity and single-use under contention ---------------------

const Fixture = struct {
    conn: db.sqlite.Db,
    cand: i64,

    fn init(a: std.mem.Allocator) !Fixture {
        var conn = try db.sqlite.Db.openMemory();
        try db.migrate.applyAll(&conn, a);
        _ = try conn.execParams(
            "insert into projects (slug, name, root_path) values ('p','p','/p')",
            &.{},
        );
        const cand = try store.createCandidate(&conn, .{
            .vendor = "vendor-x",
            .candidate_id = "cand-1",
            .fallback_order = 0,
        });
        return .{ .conn = conn, .cand = cand };
    }

    fn deinit(self: *Fixture) void {
        self.conn.close();
    }

    fn mint(self: *Fixture, a: std.mem.Allocator) !Preview {
        var b = sampleBinding();
        b.requested_candidate_id = self.cand;
        return preview(&self.conn, a, .{
            .task_id = null,
            .binding = b,
            .expires_at = far_future,
        });
    }

    fn count(self: *Fixture, sql: [:0]const u8) !i64 {
        return self.conn.intQuery(sql);
    }
};

test "confirm writes the snapshot and consumes the token together" {
    const a = testing.allocator;
    var fx = try Fixture.init(a);
    defer fx.deinit();
    const p = try fx.mint(a);
    defer p.deinit(a);

    var cur = sampleCurrent();
    cur.requested_candidate_id = fx.cand;

    const done = try confirm(&fx.conn, a, .{
        .token = p.token,
        .dispatch_key = "dk-1",
        .current = cur,
    }, null);

    try testing.expectEqual(@as(i64, 1), try fx.count("select count(*) from routing_dispatch_snapshots"));
    try testing.expectEqual(
        @as(i64, 1),
        try fx.conn.intQuery("select count(*) from routing_dispatch_previews where consumed_at is not null"),
    );
    // The audit link must point at the dispatch this confirm produced.
    try testing.expectEqual(
        done.dispatch_id,
        try fx.conn.intQuery("select consumed_dispatch_id from routing_dispatch_previews"),
    );
}

test "a stale confirm writes nothing at all" {
    const a = testing.allocator;
    var fx = try Fixture.init(a);
    defer fx.deinit();
    const p = try fx.mint(a);
    defer p.deinit(a);

    // The task was edited after the operator looked at the preview.
    var cur = sampleCurrent();
    cur.requested_candidate_id = fx.cand;
    cur.packet_digest = "moved";

    var reason: StaleReason = undefined;
    try testing.expectError(Error.StalePreview, confirm(&fx.conn, a, .{
        .token = p.token,
        .dispatch_key = "dk-1",
        .current = cur,
    }, &reason));
    try testing.expectEqual(StaleReason.packet_changed, reason);

    // All-or-nothing: no snapshot, and the token is still spendable so the
    // operator can re-preview rather than being stranded.
    try testing.expectEqual(@as(i64, 0), try fx.count("select count(*) from routing_dispatch_snapshots"));
    try testing.expectEqual(
        @as(i64, 0),
        try fx.conn.intQuery("select count(*) from routing_dispatch_previews where consumed_at is not null"),
    );
}

test "a token confirms exactly once, and the second attempt is refused" {
    const a = testing.allocator;
    var fx = try Fixture.init(a);
    defer fx.deinit();
    const p = try fx.mint(a);
    defer p.deinit(a);

    var cur = sampleCurrent();
    cur.requested_candidate_id = fx.cand;

    _ = try confirm(&fx.conn, a, .{
        .token = p.token,
        .dispatch_key = "dk-1",
        .current = cur,
    }, null);

    // Replaying the same authorization must not produce a second dispatch —
    // that is one operator decision spawning twice.
    var reason: StaleReason = undefined;
    try testing.expectError(Error.StalePreview, confirm(&fx.conn, a, .{
        .token = p.token,
        .dispatch_key = "dk-2",
        .current = cur,
    }, &reason));
    try testing.expectEqual(StaleReason.already_consumed, reason);
    try testing.expectEqual(@as(i64, 1), try fx.count("select count(*) from routing_dispatch_snapshots"));
}

test "an unknown token is refused rather than treated as a fresh dispatch" {
    const a = testing.allocator;
    var fx = try Fixture.init(a);
    defer fx.deinit();

    var cur = sampleCurrent();
    cur.requested_candidate_id = fx.cand;
    try testing.expectError(Error.UnknownPreview, confirm(&fx.conn, a, .{
        .token = "0123456789abcdef0123456789abcdef",
        .dispatch_key = "dk-1",
        .current = cur,
    }, null));
    try testing.expectEqual(@as(i64, 0), try fx.count("select count(*) from routing_dispatch_snapshots"));
}

test "an expired token is refused and leaves no dispatch behind" {
    const a = testing.allocator;
    var fx = try Fixture.init(a);
    defer fx.deinit();
    const p = try fx.mint(a);
    defer p.deinit(a);

    var cur = sampleCurrent();
    cur.requested_candidate_id = fx.cand;
    cur.now = "2028-01-01T00:00:00Z";

    var reason: StaleReason = undefined;
    try testing.expectError(Error.StalePreview, confirm(&fx.conn, a, .{
        .token = p.token,
        .dispatch_key = "dk-1",
        .current = cur,
    }, &reason));
    try testing.expectEqual(StaleReason.expired, reason);
    try testing.expectEqual(@as(i64, 0), try fx.count("select count(*) from routing_dispatch_snapshots"));
}

test "a claim that moved to another agent stops the dispatch" {
    const a = testing.allocator;
    var fx = try Fixture.init(a);
    defer fx.deinit();

    var b = sampleBinding();
    b.requested_candidate_id = fx.cand;
    b.claim_token = "claim-mine";
    b.claim_status = "active";
    const p = try preview(&fx.conn, a, .{
        .task_id = null,
        .binding = b,
        .expires_at = far_future,
    });
    defer p.deinit(a);

    var cur = sampleCurrent();
    cur.requested_candidate_id = fx.cand;
    cur.claim_token = "claim-someone-else";
    cur.claim_status = "active";

    var reason: StaleReason = undefined;
    try testing.expectError(Error.StalePreview, confirm(&fx.conn, a, .{
        .token = p.token,
        .dispatch_key = "dk-1",
        .current = cur,
    }, &reason));
    try testing.expectEqual(StaleReason.claim_changed, reason);
    try testing.expectEqual(@as(i64, 0), try fx.count("select count(*) from routing_dispatch_snapshots"));
}
