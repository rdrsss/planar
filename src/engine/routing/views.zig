//! engine/routing/views.zig — read-only inspection of the routing evidence
//! plane (plan 950 task 5531).
//!
//! Everything here answers "what does Planar believe, and on what basis?".
//! Nothing writes.
//!
//! The organizing rule is that an exclusion must always be *named*. A sample
//! that does not count toward a recommendation is far more informative than
//! one that does — it tells an operator that a run happened, was recorded,
//! and was deliberately set aside, and why. Omitting excluded rows would make
//! the evidence look thinner than it is and give no way to audit the
//! boundary; showing them without a reason would look like a bug.

const std = @import("std");
const db = @import("db");
const store = @import("store.zig");

pub const views_version = "routing-views-v1";

pub const Error = error{QueryFailed} || std.mem.Allocator.Error;

// ---------------------------------------------------------------------------
// Experiments
// ---------------------------------------------------------------------------

/// One declared experiment, with the manifest identity that was frozen when it
/// was approved. The manifest body itself is not expanded here: what an
/// operator needs at a glance is which cohort it governs, whether it is still
/// running, and how much evidence it has produced.
pub const ExperimentRow = struct {
    id: i64,
    experiment_key: []const u8,
    status: []const u8,
    vendor: []const u8,
    role: []const u8,
    tier: []const u8,
    work_type: []const u8,
    complexity: []const u8,
    validation_policy_version: []const u8,
    routing_policy_version: []const u8,
    manifest_digest: []const u8,
    operator_approved_at: []const u8,
    /// Work items the frozen manifest admits.
    population_size: i64,
    /// Candidates the frozen manifest admits.
    candidate_count: i64,
    /// Terminal samples recorded so far, and how many of those count.
    samples: i64,
    eligible_samples: i64,

    pub fn deinit(self: ExperimentRow, a: std.mem.Allocator) void {
        a.free(self.experiment_key);
        a.free(self.status);
        a.free(self.vendor);
        a.free(self.role);
        a.free(self.tier);
        a.free(self.work_type);
        a.free(self.complexity);
        a.free(self.validation_policy_version);
        a.free(self.routing_policy_version);
        a.free(self.manifest_digest);
        a.free(self.operator_approved_at);
    }
};

pub fn listExperiments(
    d: *db.sqlite.Db,
    a: std.mem.Allocator,
) Error![]ExperimentRow {
    const sql: [:0]const u8 =
        \\select e.id, e.experiment_key, e.status, e.vendor, e.role, e.tier,
        \\       e.work_type, e.complexity, e.validation_policy_version,
        \\       e.routing_policy_version, e.manifest_digest, e.operator_approved_at,
        \\       (select count(*) from json_each(e.eligible_population_json)),
        \\       (select count(*) from json_each(e.candidate_set_json)),
        \\       (select count(*) from routing_terminal_samples s where s.experiment_id = e.id),
        \\       (select count(*) from routing_terminal_samples s
        \\          where s.experiment_id = e.id and s.cohort_eligible = 1)
        \\from routing_experiments as e
        \\order by e.id asc
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();

    var rows: std.ArrayList(ExperimentRow) = .empty;
    errdefer {
        for (rows.items) |r| r.deinit(a);
        rows.deinit(a);
    }

    while ((stmt.step() catch return Error.QueryFailed) == .row) {
        const row: ExperimentRow = .{
            .id = stmt.columnInt(0),
            .experiment_key = stmt.columnTextAlloc(1, a) catch return Error.QueryFailed,
            .status = stmt.columnTextAlloc(2, a) catch return Error.QueryFailed,
            .vendor = stmt.columnTextAlloc(3, a) catch return Error.QueryFailed,
            .role = stmt.columnTextAlloc(4, a) catch return Error.QueryFailed,
            .tier = stmt.columnTextAlloc(5, a) catch return Error.QueryFailed,
            .work_type = stmt.columnTextAlloc(6, a) catch return Error.QueryFailed,
            .complexity = stmt.columnTextAlloc(7, a) catch return Error.QueryFailed,
            .validation_policy_version = stmt.columnTextAlloc(8, a) catch return Error.QueryFailed,
            .routing_policy_version = stmt.columnTextAlloc(9, a) catch return Error.QueryFailed,
            .manifest_digest = stmt.columnTextAlloc(10, a) catch return Error.QueryFailed,
            .operator_approved_at = stmt.columnTextAlloc(11, a) catch return Error.QueryFailed,
            .population_size = stmt.columnInt(12),
            .candidate_count = stmt.columnInt(13),
            .samples = stmt.columnInt(14),
            .eligible_samples = stmt.columnInt(15),
        };
        try rows.append(a, row);
    }
    return rows.toOwnedSlice(a);
}

// ---------------------------------------------------------------------------
// Terminal outcomes
// ---------------------------------------------------------------------------

/// One recorded terminal sample. `counts_toward_recommendation` is stated
/// directly rather than left for the reader to infer from `cohort_eligible`,
/// and `exclusion_reason` is always present when it is false.
pub const OutcomeRow = struct {
    id: i64,
    experiment_id: i64,
    logical_work_item_id: []const u8,
    role: []const u8,
    vendor: []const u8,
    candidate: []const u8,
    tier: []const u8,
    work_type: []const u8,
    complexity: []const u8,
    terminal_state: []const u8,
    quality_success: bool,
    counts_toward_recommendation: bool,
    exclusion_reason: ?[]const u8,
    finalized_at: []const u8,

    pub fn deinit(self: OutcomeRow, a: std.mem.Allocator) void {
        a.free(self.logical_work_item_id);
        a.free(self.role);
        a.free(self.vendor);
        a.free(self.candidate);
        a.free(self.tier);
        a.free(self.work_type);
        a.free(self.complexity);
        a.free(self.terminal_state);
        if (self.exclusion_reason) |r| a.free(r);
        a.free(self.finalized_at);
    }
};

/// List terminal outcomes, newest first.
///
/// Excluded samples are included deliberately. They are the audit trail of the
/// evidence boundary: hiding them would make the evidence look thinner than it
/// is and leave no way to check that the boundary was applied correctly.
pub fn listOutcomes(
    d: *db.sqlite.Db,
    a: std.mem.Allocator,
    limit: i64,
) Error![]OutcomeRow {
    const sql: [:0]const u8 =
        \\select s.id, s.experiment_id, s.logical_work_item_id, s.role, s.vendor,
        \\       c.candidate_id, s.tier, s.work_type, s.complexity, s.terminal_state,
        \\       s.quality_success, s.cohort_eligible, coalesce(s.exclusion_reason,''),
        \\       s.finalized_at
        \\from routing_terminal_samples as s
        \\join routing_candidates as c on c.id = s.candidate_id
        \\order by s.id desc
        \\limit ?
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = limit }}) catch return Error.QueryFailed;

    var rows: std.ArrayList(OutcomeRow) = .empty;
    errdefer {
        for (rows.items) |r| r.deinit(a);
        rows.deinit(a);
    }

    while ((stmt.step() catch return Error.QueryFailed) == .row) {
        const reason_text = stmt.columnTextAlloc(12, a) catch return Error.QueryFailed;
        var reason: ?[]const u8 = reason_text;
        if (reason_text.len == 0) {
            a.free(reason_text);
            reason = null;
        }
        const row: OutcomeRow = .{
            .id = stmt.columnInt(0),
            .experiment_id = stmt.columnInt(1),
            .logical_work_item_id = stmt.columnTextAlloc(2, a) catch return Error.QueryFailed,
            .role = stmt.columnTextAlloc(3, a) catch return Error.QueryFailed,
            .vendor = stmt.columnTextAlloc(4, a) catch return Error.QueryFailed,
            .candidate = stmt.columnTextAlloc(5, a) catch return Error.QueryFailed,
            .tier = stmt.columnTextAlloc(6, a) catch return Error.QueryFailed,
            .work_type = stmt.columnTextAlloc(7, a) catch return Error.QueryFailed,
            .complexity = stmt.columnTextAlloc(8, a) catch return Error.QueryFailed,
            .terminal_state = stmt.columnTextAlloc(9, a) catch return Error.QueryFailed,
            .quality_success = stmt.columnInt(10) != 0,
            .counts_toward_recommendation = stmt.columnInt(11) != 0,
            .exclusion_reason = reason,
            .finalized_at = stmt.columnTextAlloc(13, a) catch return Error.QueryFailed,
        };
        try rows.append(a, row);
    }
    return rows.toOwnedSlice(a);
}

pub fn freeExperiments(a: std.mem.Allocator, rows: []ExperimentRow) void {
    for (rows) |r| r.deinit(a);
    a.free(rows);
}

pub fn freeOutcomes(a: std.mem.Allocator, rows: []OutcomeRow) void {
    for (rows) |r| r.deinit(a);
    a.free(rows);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const testing = std.testing;

test "views: an empty evidence plane lists nothing rather than failing" {
    const a = testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);

    const experiments = try listExperiments(&conn, a);
    defer freeExperiments(a, experiments);
    try testing.expectEqual(@as(usize, 0), experiments.len);

    const outcomes = try listOutcomes(&conn, a, 50);
    defer freeOutcomes(a, outcomes);
    try testing.expectEqual(@as(usize, 0), outcomes.len);
}

test "views: an excluded outcome is listed WITH its reason, not hidden" {
    const a = testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);

    // One eligible sample plus one the host answered with a different model.
    const ranking = @import("ranking.zig");
    const ok: ranking.Sample = .{ .state = "completed", .success = true, .attempts = 1 };
    try ranking.seedCohortWith(
        &conn,
        a,
        &.{.{ .name = "cand-A", .order = 0, .samples = &.{ok} }},
        &.{"lwi-mismatch"},
    );

    const exp = try conn.intQuery("select id from routing_experiments limit 1");
    const cand = try conn.intQuery("select id from routing_candidates limit 1");
    const dispatch = try conn.execParams(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, logical_work_item_id, project_id, validation_policy_version,
        \\  vendor, role, tier, work_type, complexity, routing_policy_version,
        \\  profile_rule_version, packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision, reviewer_disposition,
        \\  terminal_state, confirmed_at
        \\) values (
        \\  'dk-mm','lwi-mismatch',1,'val-v1','vendor-x','coder','medium','feature',
        \\  'standard','route-v1','pr','pk','po','ca',?,'vendor-x','something-else',
        \\  'declared_experiment',?,'confirmed','approved','candidate_mismatch',
        \\  '2026-01-01T00:00:00Z'
        \\)
    , &.{ .{ .int = cand }, .{ .int = exp } });
    _ = try conn.execParams(
        \\insert into routing_dispatch_events (
        \\  dispatch_id, event_id, sequence, event_kind, attempt_number,
        \\  terminal_state, payload_json, occurred_at
        \\) values (?, 'ev-mm', 900, 'outcome', 1, 'candidate_mismatch', '{}', '2026-01-01T00:00:00Z')
    , &.{.{ .int = dispatch }});
    _ = try conn.execParams(
        \\insert into routing_terminal_samples (
        \\  experiment_id, logical_work_item_id, role, initial_packet_digest,
        \\  candidate_id, project_id, validation_policy_version, routing_policy_version,
        \\  vendor, tier, work_type, complexity, terminal_event_id, terminal_state,
        \\  quality_success, cohort_eligible, exclusion_reason, finalized_at
        \\) values (
        \\  ?, 'lwi-mismatch', 'coder', 'pk', ?, 1, 'val-v1', 'route-v1', 'vendor-x',
        \\  'medium', 'feature', 'standard', 'ev-mm', 'candidate_mismatch',
        \\  0, 0, 'candidate_mismatch', '2026-01-01T00:00:00Z'
        \\)
    , &.{ .{ .int = exp }, .{ .int = cand } });

    const experiments = try listExperiments(&conn, a);
    defer freeExperiments(a, experiments);
    try testing.expectEqual(@as(usize, 1), experiments.len);
    // Two samples recorded, one of which counts. Reporting only the eligible
    // count would understate what actually ran.
    try testing.expectEqual(@as(i64, 2), experiments[0].samples);
    try testing.expectEqual(@as(i64, 1), experiments[0].eligible_samples);

    const outcomes = try listOutcomes(&conn, a, 50);
    defer freeOutcomes(a, outcomes);
    try testing.expectEqual(@as(usize, 2), outcomes.len);

    var saw_excluded = false;
    for (outcomes) |o| {
        if (o.counts_toward_recommendation) {
            // An included row must never carry a reason: a reason on a
            // counted sample would be self-contradictory.
            try testing.expect(o.exclusion_reason == null);
        } else {
            saw_excluded = true;
            try testing.expectEqualStrings("candidate_mismatch", o.exclusion_reason.?);
            try testing.expect(!o.quality_success);
        }
    }
    try testing.expect(saw_excluded);
}
