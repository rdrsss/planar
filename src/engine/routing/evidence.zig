//! engine/routing/evidence.zig — the boundary between precommitted experiment
//! assignments and observational routing telemetry (plan 950 task 5529).
//!
//! The distinction this module exists to hold:
//!
//!   A *declared experiment* sample can move a recommendation. Everything
//!   else — defaults, fallbacks, overrides, review-bypass runs — is
//!   observational. Observational runs keep every scrap of telemetry
//!   (attempts, gates, latency, tokens, cost) because that is how an operator
//!   debugs a dispatch, but they never touch the numerator or denominator of
//!   a recommendation.
//!
//! Collapsing the two is the failure this guards against: a fallback that
//! happened to succeed is not evidence the fallback candidate is good, it is
//! evidence the primary was unavailable. Counting it would let availability
//! masquerade as quality.
//!
//! Folding is idempotent by construction. `routing_dispatch_events.event_id`
//! is unique and `(dispatch_id, sequence)` is unique, so a duplicate delivery
//! is a no-op and an out-of-order delivery sorts by sequence rather than by
//! arrival. Retries append attempts; exactly one terminal sample results.

const std = @import("std");
const db = @import("db");
const store = @import("store.zig");

/// Bumped when fold semantics change. Recorded alongside samples so evidence
/// gathered under different folding rules is never pooled.
pub const evidence_version = "routing-evidence-v1";

pub const Error = error{
    QueryFailed,
    /// An assignment was attempted without a recorded operator confirmation.
    /// Assignment spends experiment quota, so it is gated the same way a
    /// spawn is.
    OperatorConfirmationRequired,
    /// The manifest must exist and be frozen before any outcome references it.
    ExperimentNotDeclared,
    /// A finalized outcome may only be changed by an audited supersession.
    OutcomeAlreadyFinalized,
    /// A supersession must name the event it replaces.
    SupersessionTargetMissing,
} || std.mem.Allocator.Error;

// ---------------------------------------------------------------------------
// Eligibility: which runs may move a recommendation
// ---------------------------------------------------------------------------

/// Why a run is observational rather than evidential. Stored verbatim in
/// `routing_terminal_samples.exclusion_reason`, so the exclusion is auditable
/// rather than implied by absence.
pub const ExclusionReason = enum {
    default_assignment,
    fallback_assignment,
    operator_override,
    review_bypassed,
    candidate_mismatch,
    missing_evidence,

    pub fn text(self: ExclusionReason) []const u8 {
        return switch (self) {
            .default_assignment => "default_assignment",
            .fallback_assignment => "fallback_assignment",
            .operator_override => "operator_override",
            .review_bypassed => "review_bypassed",
            .candidate_mismatch => "candidate_mismatch",
            .missing_evidence => "missing_evidence",
        };
    }
};

/// Whether a run counts toward a recommendation, and if not, exactly why.
///
/// Note `review_bypassed` excludes even a completed run whose gates passed:
/// quality success requires an INDEPENDENT reviewer approval, and a bypass is
/// precisely the absence of one. The run's telemetry is still retained.
pub fn cohortEligible(
    class: store.AssignmentClass,
    reviewer: store.ReviewerDisposition,
    terminal: store.TerminalState,
) ?ExclusionReason {
    switch (terminal) {
        .candidate_mismatch => return .candidate_mismatch,
        .missing_evidence => return .missing_evidence,
        else => {},
    }
    if (reviewer == .bypassed) return .review_bypassed;
    return switch (class) {
        .declared_experiment => null,
        .default => .default_assignment,
        .fallback => .fallback_assignment,
        .override => .operator_override,
    };
}

/// Quality success is a conjunction, not a vibe: the work completed, every
/// required gate passed, and an independent reviewer approved it under the
/// recorded validation policy. Any missing conjunct is a false, never a
/// "probably".
pub fn qualitySuccess(
    terminal: store.TerminalState,
    required_gates_total: usize,
    required_gates_passed: usize,
    reviewer: store.ReviewerDisposition,
) bool {
    if (terminal != .completed) return false;
    if (required_gates_total == 0) return false;
    if (required_gates_passed != required_gates_total) return false;
    return reviewer == .approved;
}

// ---------------------------------------------------------------------------
// Folding
// ---------------------------------------------------------------------------

/// The folded view of one dispatch's event stream.
pub const Fold = struct {
    /// Highest attempt number observed. Retries append attempts; they do not
    /// create additional samples.
    attempts: i64,
    /// Terminal state from the surviving outcome event, if one exists.
    terminal: ?store.TerminalState,
    /// The event id the terminal state came from — the sample's audit anchor.
    terminal_event_id: ?[]const u8,
    /// True when a supersession replaced an earlier outcome. The replacement
    /// is auditable: both events remain in the stream.
    superseded: bool,

    pub fn deinit(self: Fold, allocator: std.mem.Allocator) void {
        if (self.terminal_event_id) |id| allocator.free(id);
    }
};

fn parseTerminal(text: []const u8) ?store.TerminalState {
    inline for (@typeInfo(store.TerminalState).@"enum".fields) |f| {
        if (std.mem.eql(u8, f.name, text)) return @field(store.TerminalState, f.name);
    }
    return null;
}

/// Fold a dispatch's append-only event stream into its current terminal view.
///
/// Ordering is by `sequence`, never by arrival or by `recorded_at`: an event
/// delivered late must land in its logical position, otherwise a retry that
/// arrives out of order could overwrite a later outcome.
///
/// A supersession replaces the outcome it names. Because both events remain
/// in the stream, "what did we believe, and when did that change" stays
/// answerable — which is the whole point of requiring supersession rather
/// than allowing an in-place rewrite.
pub fn foldDispatch(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    dispatch_id: i64,
) Error!Fold {
    const sql: [:0]const u8 =
        \\select event_kind, coalesce(attempt_number,0), coalesce(terminal_state,''),
        \\       event_id, coalesce(supersedes_event_id,'')
        \\from routing_dispatch_events
        \\where dispatch_id = ?
        \\order by sequence asc, id asc
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = dispatch_id }}) catch return Error.QueryFailed;

    var attempts: i64 = 0;
    var terminal: ?store.TerminalState = null;
    var terminal_id: ?[]const u8 = null;
    var superseded = false;
    errdefer if (terminal_id) |id| allocator.free(id);

    // Events superseded by a later supersession, by event id.
    var dropped: std.StringHashMapUnmanaged(void) = .empty;
    defer {
        var keys = dropped.keyIterator();
        while (keys.next()) |k| allocator.free(k.*);
        dropped.deinit(allocator);
    }

    while ((stmt.step() catch return Error.QueryFailed) == .row) {
        const kind = stmt.columnTextAlloc(0, allocator) catch return Error.QueryFailed;
        defer allocator.free(kind);
        const attempt = stmt.columnInt(1);
        const state_text = stmt.columnTextAlloc(2, allocator) catch return Error.QueryFailed;
        defer allocator.free(state_text);
        const event_id = stmt.columnTextAlloc(3, allocator) catch return Error.QueryFailed;
        const target = stmt.columnTextAlloc(4, allocator) catch return Error.QueryFailed;
        defer allocator.free(target);

        var keep_event_id = false;
        defer if (!keep_event_id) allocator.free(event_id);

        if (attempt > attempts) attempts = attempt;

        if (std.mem.eql(u8, kind, "supersession")) {
            if (target.len == 0) return Error.SupersessionTargetMissing;
            try dropped.put(allocator, try allocator.dupe(u8, target), {});
            superseded = true;
            // A supersession invalidates the outcome it names; if that was the
            // one we were carrying, drop it and let a later outcome stand.
            if (terminal_id) |cur| {
                if (std.mem.eql(u8, cur, target)) {
                    allocator.free(cur);
                    terminal_id = null;
                    terminal = null;
                }
            }
            continue;
        }

        if (std.mem.eql(u8, kind, "outcome")) {
            if (dropped.contains(event_id)) continue;
            if (terminal_id) |cur| allocator.free(cur);
            terminal = parseTerminal(state_text);
            terminal_id = event_id;
            keep_event_id = true;
        }
    }

    return .{
        .attempts = attempts,
        .terminal = terminal,
        .terminal_event_id = terminal_id,
        .superseded = superseded,
    };
}

// ---------------------------------------------------------------------------
// Assignment gating
// ---------------------------------------------------------------------------

/// Inputs to the assignment gate. Assignment spends experiment quota, so it
/// carries the same operator gate as a spawn — an unconfirmed assignment
/// would let a preview quietly consume a slot.
pub const AssignmentRequest = struct {
    experiment_id: ?i64,
    class: store.AssignmentClass,
    operator_confirmed: bool,
};

/// Authorize an assignment, or say precisely why not.
///
/// Only `declared_experiment` requires a live experiment manifest; the other
/// classes are observational and need no manifest, because they can never
/// spend experiment quota in the first place.
pub fn authorizeAssignment(req: AssignmentRequest) Error!void {
    if (!req.operator_confirmed) return Error.OperatorConfirmationRequired;
    if (req.class == .declared_experiment and req.experiment_id == null) {
        return Error.ExperimentNotDeclared;
    }
}

/// Guard an in-place outcome rewrite. A finalized outcome may only change
/// through an audited supersession event, never by mutating the original.
pub fn authorizeLateOutcome(already_finalized: bool, is_supersession: bool) Error!void {
    if (already_finalized and !is_supersession) return Error.OutcomeAlreadyFinalized;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const testing = std.testing;

test "eligibility: only declared experiments can move a recommendation" {
    try testing.expect(cohortEligible(.declared_experiment, .approved, .completed) == null);
    try testing.expectEqual(ExclusionReason.default_assignment, cohortEligible(.default, .approved, .completed).?);
    try testing.expectEqual(ExclusionReason.fallback_assignment, cohortEligible(.fallback, .approved, .completed).?);
    try testing.expectEqual(ExclusionReason.operator_override, cohortEligible(.override, .approved, .completed).?);
}

test "eligibility: a bypassed review excludes even a completed declared experiment" {
    // The run succeeded and its gates passed, but nobody independent looked at
    // it. Retained as telemetry, excluded from evidence.
    try testing.expectEqual(
        ExclusionReason.review_bypassed,
        cohortEligible(.declared_experiment, .bypassed, .completed).?,
    );
}

test "eligibility: mismatch and missing evidence exclude regardless of class" {
    try testing.expectEqual(
        ExclusionReason.candidate_mismatch,
        cohortEligible(.declared_experiment, .approved, .candidate_mismatch).?,
    );
    try testing.expectEqual(
        ExclusionReason.missing_evidence,
        cohortEligible(.declared_experiment, .approved, .missing_evidence).?,
    );
}

test "quality success is a conjunction: completion, all gates, independent approval" {
    try testing.expect(qualitySuccess(.completed, 3, 3, .approved));
    // completion alone is not success
    try testing.expect(!qualitySuccess(.completed, 3, 2, .approved));
    try testing.expect(!qualitySuccess(.completed, 3, 3, .request_changes));
    try testing.expect(!qualitySuccess(.completed, 3, 3, .bypassed));
    try testing.expect(!qualitySuccess(.quality_failed, 3, 3, .approved));
    // no required gates at all cannot be "all gates passed"
    try testing.expect(!qualitySuccess(.completed, 0, 0, .approved));
}

test "assignment: no experiment assignment without explicit operator confirmation" {
    try testing.expectError(
        Error.OperatorConfirmationRequired,
        authorizeAssignment(.{ .experiment_id = 1, .class = .declared_experiment, .operator_confirmed = false }),
    );
    // observational classes are gated too — an unconfirmed spawn is a spawn
    try testing.expectError(
        Error.OperatorConfirmationRequired,
        authorizeAssignment(.{ .experiment_id = null, .class = .default, .operator_confirmed = false }),
    );
}

test "assignment: a declared experiment requires a declared manifest" {
    try testing.expectError(
        Error.ExperimentNotDeclared,
        authorizeAssignment(.{ .experiment_id = null, .class = .declared_experiment, .operator_confirmed = true }),
    );
    try authorizeAssignment(.{ .experiment_id = 7, .class = .declared_experiment, .operator_confirmed = true });
    // observational classes need no manifest
    try authorizeAssignment(.{ .experiment_id = null, .class = .fallback, .operator_confirmed = true });
}

test "late outcome: a finalized outcome cannot be rewritten except by supersession" {
    try testing.expectError(
        Error.OutcomeAlreadyFinalized,
        authorizeLateOutcome(true, false),
    );
    try authorizeLateOutcome(true, true);
    try authorizeLateOutcome(false, false);
}

// --- fold tests (DB-backed: the fold is the heart of this module) ----------

/// Seed the minimum rows a dispatch event needs: a project, a candidate, and
/// a dispatch snapshot to hang events off.
fn seedDispatch(conn: *db.sqlite.Db, allocator: std.mem.Allocator) !i64 {
    _ = try conn.execParams("insert into projects (slug, name, root_path) values ('p','p','/p')", &.{});
    const cand = try store.createCandidate(conn, .{
        .vendor = "vendor-x",
        .candidate_id = "cand-1",
        .fallback_order = 0,
    });
    _ = allocator;
    const exp = try conn.execParams(
        \\insert into routing_experiments (
        \\  experiment_key, project_id, validation_policy_version, vendor, role, tier,
        \\  work_type, complexity, routing_policy_version, eligible_population_json,
        \\  candidate_set_json, allocation_method, stopping_rule_json,
        \\  analysis_policy_json, manifest_digest, operator_approved_at
        \\) values (
        \\  'exp-1',1,'val-v1','vendor-x','coder','medium','feature','standard','route-v1',
        \\  '["lwi-1"]',json_array(?),'balanced','{}','{}','digest','2026-01-01T00:00:00Z'
        \\)
    , &.{.{ .int = cand }});
    return conn.execParams(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, logical_work_item_id, project_id, validation_policy_version,
        \\  vendor, role, tier, work_type, complexity, routing_policy_version,
        \\  profile_rule_version, packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\) values (
        \\  'dk-1','lwi-1',1,'val-v1','vendor-x','coder','medium','feature','standard',
        \\  'route-v1','profile-v1','pkt','pol','cap',?,'declared_experiment',?,'confirmed',
        \\  'required','pending','2026-01-01T00:00:00Z'
        \\)
    , &.{ .{ .int = cand }, .{ .int = exp } });
}

fn addEvent(
    conn: *db.sqlite.Db,
    dispatch_id: i64,
    event_id: []const u8,
    sequence: i64,
    kind: []const u8,
    attempt: ?i64,
    terminal: ?[]const u8,
    supersedes: ?[]const u8,
) !void {
    _ = try conn.execParams(
        \\insert into routing_dispatch_events (
        \\  dispatch_id, event_id, sequence, event_kind, attempt_number,
        \\  terminal_state, supersedes_event_id, payload_json, occurred_at
        \\) values (?,?,?,?,?,?,?,'{}','2026-01-01T00:00:00Z')
    , &.{
        .{ .int = dispatch_id },
        .{ .text = event_id },
        .{ .int = sequence },
        .{ .text = kind },
        if (attempt) |a| .{ .int = a } else .{ .null = {} },
        if (terminal) |t| .{ .text = t } else .{ .null = {} },
        if (supersedes) |s| .{ .text = s } else .{ .null = {} },
    });
}

test "fold: retries append attempts but yield one terminal sample" {
    const a = std.testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);
    const dispatch = try seedDispatch(&conn, a);

    try addEvent(&conn, dispatch, "e1", 0, "attempt_started", 1, null, null);
    try addEvent(&conn, dispatch, "e2", 1, "attempt_finished", 1, null, null);
    try addEvent(&conn, dispatch, "e3", 2, "attempt_started", 2, null, null);
    try addEvent(&conn, dispatch, "e4", 3, "attempt_finished", 2, null, null);
    try addEvent(&conn, dispatch, "e5", 4, "outcome", 2, "completed", null);

    const fold = try foldDispatch(&conn, a, dispatch);
    defer fold.deinit(a);
    try std.testing.expectEqual(@as(i64, 2), fold.attempts);
    try std.testing.expectEqual(store.TerminalState.completed, fold.terminal.?);
    try std.testing.expectEqualStrings("e5", fold.terminal_event_id.?);
    try std.testing.expect(!fold.superseded);
}

test "fold: a duplicate event id is rejected by identity, so folding stays idempotent" {
    const a = std.testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);
    const dispatch = try seedDispatch(&conn, a);

    try addEvent(&conn, dispatch, "e1", 0, "outcome", 1, "completed", null);
    // Re-delivery of the same event: the unique constraint makes the second
    // write a no-op rather than a second terminal state.
    if (addEvent(&conn, dispatch, "e1", 1, "outcome", 1, "quality_failed", null)) |_| {
        return error.TestExpectedDuplicateEventRejection;
    } else |_| {}
    const fold = try foldDispatch(&conn, a, dispatch);
    defer fold.deinit(a);
    try std.testing.expectEqual(store.TerminalState.completed, fold.terminal.?);
}

test "fold: out-of-order arrival sorts by sequence, not by insertion" {
    const a = std.testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);
    const dispatch = try seedDispatch(&conn, a);

    // The later-sequence outcome is INSERTED FIRST. A fold that trusted
    // arrival order would report quality_failed.
    try addEvent(&conn, dispatch, "e9", 9, "outcome", 1, "completed", null);
    try addEvent(&conn, dispatch, "e2", 2, "outcome", 1, "quality_failed", null);

    const fold = try foldDispatch(&conn, a, dispatch);
    defer fold.deinit(a);
    try std.testing.expectEqual(store.TerminalState.completed, fold.terminal.?);
    try std.testing.expectEqualStrings("e9", fold.terminal_event_id.?);
}

test "fold: a late outcome requires supersession, and both events survive for audit" {
    const a = std.testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);
    const dispatch = try seedDispatch(&conn, a);

    try addEvent(&conn, dispatch, "e1", 0, "outcome", 1, "completed", null);
    try addEvent(&conn, dispatch, "e2", 1, "supersession", null, null, "e1");
    try addEvent(&conn, dispatch, "e3", 2, "outcome", 1, "quality_failed", null);

    const fold = try foldDispatch(&conn, a, dispatch);
    defer fold.deinit(a);
    try std.testing.expectEqual(store.TerminalState.quality_failed, fold.terminal.?);
    try std.testing.expectEqualStrings("e3", fold.terminal_event_id.?);
    try std.testing.expect(fold.superseded);

    // The superseded event is still on the record — supersession replaces the
    // conclusion, not the history.
    var stmt = try conn.prepare("select count(*) from routing_dispatch_events where dispatch_id=?\x00");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = dispatch }});
    _ = try stmt.step();
    try std.testing.expectEqual(@as(i64, 3), stmt.columnInt(0));
}

test "manifest freeze: a dispatch outside the frozen experiment is rejected at write time" {
    const a = std.testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);
    _ = try seedDispatch(&conn, a);

    // Same cohort and candidate, but a work item the frozen eligible
    // population never named. Enrolling it after the fact would be exactly the
    // post-hoc population edit the manifest exists to prevent.
    const out_of_population = conn.execParams(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, logical_work_item_id, project_id, validation_policy_version,
        \\  vendor, role, tier, work_type, complexity, routing_policy_version,
        \\  profile_rule_version, packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\) values (
        \\  'dk-2','lwi-UNDECLARED',1,'val-v1','vendor-x','coder','medium','feature','standard',
        \\  'route-v1','profile-v1','pkt','pol','cap',1,'declared_experiment',1,'confirmed',
        \\  'required','pending','2026-01-01T00:00:00Z'
        \\)
    , &.{});
    try std.testing.expectError(error.StepFailed, out_of_population);

    // Likewise a cohort that drifts from the frozen one: the manifest pins the
    // exact cohort, so a 'large' tier cannot borrow this experiment's evidence.
    const cohort_drift = conn.execParams(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, logical_work_item_id, project_id, validation_policy_version,
        \\  vendor, role, tier, work_type, complexity, routing_policy_version,
        \\  profile_rule_version, packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\) values (
        \\  'dk-3','lwi-1',1,'val-v1','vendor-x','coder','large','feature','standard',
        \\  'route-v1','profile-v1','pkt','pol','cap',1,'declared_experiment',1,'confirmed',
        \\  'required','pending','2026-01-01T00:00:00Z'
        \\)
    , &.{});
    try std.testing.expectError(error.StepFailed, cohort_drift);
}

test "every named terminal state is representable and round-trips" {
    // Criterion: gate failure, spawn failure, cancellation, abort, actual-model
    // mismatch, and missing evidence each need a distinct name — collapsing any
    // pair would make a failure mode unattributable.
    const named = [_][]const u8{
        "completed",
        "quality_failed",
        "spawn_failed",
        "cancelled",
        "aborted",
        "candidate_mismatch",
        "missing_evidence",
    };
    for (named) |name| {
        const parsed = parseTerminal(name) orelse return error.TerminalStateNotRepresentable;
        try std.testing.expectEqualStrings(name, @tagName(parsed));
    }
    try std.testing.expect(parseTerminal("not_a_state") == null);
}
