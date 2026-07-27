//! Typed persistence contracts for adaptive routing evidence.
//!
//! Candidate identifiers are deliberately opaque. These types describe rows
//! stored by migration 00030; classification and recommendation policy live in
//! later routing modules.

const std = @import("std");
const db = @import("db");

/// Stable schema contract introduced by migration 00030.
pub const schema_version: u32 = 30;

/// A value persisted in a provenance-bearing task fact.
pub const FactValue = union(enum) {
    bool: bool,
    integer: i64,
    real: f64,
    text: []const u8,
    json: []const u8,
};

/// Candidate capability tier. This is policy capacity, not a model identity.
pub const Tier = enum {
    small,
    medium,
    large,
};

/// Versioned work classification used as an exact cohort dimension.
pub const WorkType = enum {
    schema,
    engine,
    architectural,
    cli,
    feature,
    mechanical,
};

/// Versioned complexity classification used as an exact cohort dimension.
pub const Complexity = enum {
    bounded,
    standard,
    high_risk,
};

/// Exact host observation of candidate availability.
pub const Availability = enum {
    available,
    unavailable,
    unknown,
};

/// Exact spawn-verification result reported by a host integration.
pub const SpawnVerification = enum {
    verified,
    unverified,
    failed,
    mismatch,
};

/// How a candidate became the requested dispatch identity.
pub const AssignmentClass = enum {
    fallback,
    default,
    override,
    declared_experiment,
};

/// Reviewer state recorded with dispatch evidence.
pub const ReviewerDisposition = enum {
    required,
    approved,
    request_changes,
    bypassed,
    not_reached,
};

/// Terminal states are explicit; none aliases evidence to another candidate.
pub const TerminalState = enum {
    pending,
    completed,
    quality_failed,
    spawn_failed,
    cancelled,
    aborted,
    candidate_mismatch,
    missing_evidence,
};

/// Operator action that authorized the immutable dispatch snapshot.
pub const OperatorDecision = enum {
    confirmed,
    overridden,
};

/// Append-only lifecycle event kind.
pub const EventKind = enum {
    attempt_started,
    attempt_finished,
    outcome,
    supersession,
};

/// One opaque operator registration. `candidate_id` has no parsed semantics.
pub const CandidateRegistration = struct {
    id: i64,
    vendor: []const u8,
    candidate_id: []const u8,
    enabled: bool,
    fallback_order: i64,
    registration_version: i64,
    compatibility_source: []const u8,
};

/// Explicit role/tier eligibility for an opaque candidate.
pub const CandidateBinding = struct {
    candidate_id: i64,
    role: []const u8,
    tier: Tier,
};

/// One immutable, versioned host capability observation.
pub const HostObservation = struct {
    id: i64,
    candidate_id: i64,
    host_id: []const u8,
    observation_version: i64,
    availability: Availability,
    spawn_verification: SpawnVerification,
    evidence_ref: []const u8,
    captured_at: []const u8,
    expires_at: []const u8,
};

/// One materialized task fact with complete source lineage.
pub const TaskFact = struct {
    id: i64,
    task_id: i64,
    fact_kind: []const u8,
    value: FactValue,
    source_entity_kind: []const u8,
    source_entity_id: i64,
    source_locator: []const u8,
    source_digest: []const u8,
    materializer_version: []const u8,
};

/// Frozen cohort dimensions shared by experiments, dispatches, and samples.
pub const Cohort = struct {
    project_id: i64,
    validation_policy_version: []const u8,
    vendor: []const u8,
    role: []const u8,
    tier: Tier,
    work_type: WorkType,
    complexity: Complexity,
};

/// Frozen declared-experiment manifest. Only `status` may advance.
pub const Experiment = struct {
    id: i64,
    experiment_key: []const u8,
    cohort: Cohort,
    routing_policy_version: []const u8,
    eligible_population_json: []const u8,
    candidate_set_json: []const u8,
    allocation_method: []const u8,
    stopping_rule_json: []const u8,
    analysis_policy_json: []const u8,
    manifest_digest: []const u8,
    operator_approved_at: []const u8,
    status: []const u8,
};

/// Immutable evidence describing one confirmed dispatch.
pub const DispatchSnapshot = struct {
    id: i64,
    dispatch_key: []const u8,
    task_id: ?i64,
    logical_work_item_id: []const u8,
    cohort: Cohort,
    routing_policy_version: []const u8,
    profile_rule_version: []const u8,
    packet_digest: []const u8,
    policy_digest: []const u8,
    capability_digest: []const u8,
    requested_candidate_id: i64,
    actual_vendor: ?[]const u8,
    actual_candidate_id: ?[]const u8,
    assignment_class: AssignmentClass,
    experiment_id: ?i64,
    operator_decision: OperatorDecision,
    reviewer_disposition: ReviewerDisposition,
    terminal_state: TerminalState,
    confirmed_at: []const u8,
};

/// Append-only event input. `(event_id, dispatch_id + sequence)` is the
/// deterministic replay and ordering identity.
pub const DispatchEvent = struct {
    event_id: []const u8,
    dispatch_id: i64,
    sequence: i64,
    event_kind: EventKind,
    attempt_number: ?i64,
    terminal_state: ?TerminalState,
    supersedes_event_id: ?[]const u8,
    payload_json: []const u8,
    occurred_at: []const u8,
};

/// One derived terminal experiment sample after all retries are folded.
pub const TerminalSample = struct {
    id: i64,
    experiment_id: i64,
    logical_work_item_id: []const u8,
    role: []const u8,
    initial_packet_digest: []const u8,
    candidate_id: i64,
    cohort: Cohort,
    routing_policy_version: []const u8,
    terminal_event_id: []const u8,
    terminal_state: TerminalState,
    quality_success: bool,
    cohort_eligible: bool,
    exclusion_reason: ?[]const u8,
};

test "migration 00030 enforces opaque candidate and host observation identity" {
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, std.testing.allocator);

    try conn.exec(
        \\insert into routing_candidates
        \\  (vendor, candidate_id, fallback_order)
        \\values ('vendor-x', 'opaque -- value; $()', 0)
    );
    try std.testing.expectEqual(
        @as(i64, 1),
        try conn.intQuery(
            "select count(*) from routing_candidates where candidate_id = 'opaque -- value; $()'",
        ),
    );
    try expectConstraint(&conn,
        \\insert into routing_candidates
        \\  (vendor, candidate_id, fallback_order)
        \\values ('vendor-x', 'opaque -- value; $()', 1)
    );
    try expectConstraint(&conn,
        \\insert into routing_host_observations (
        \\  candidate_id, host_id, observation_version, availability,
        \\  spawn_verification, evidence_ref, captured_at, expires_at
        \\) values (
        \\  1, 'host-a', 1, 'available', 'verified', 'evidence:1',
        \\  '2026-01-02T00:00:00Z', '2026-01-01T00:00:00Z'
        \\)
    );
    try conn.exec(
        \\insert into routing_host_observations (
        \\  candidate_id, host_id, observation_version, availability,
        \\  spawn_verification, evidence_ref, captured_at, expires_at
        \\) values (
        \\  1, 'host-a', 1, 'available', 'verified', 'evidence:1',
        \\  '2026-01-01T00:00:00Z', '2026-01-02T00:00:00Z'
        \\)
    );
    try expectConstraint(
        &conn,
        "update routing_host_observations set availability = 'unavailable' where id = 1",
    );
    try expectConstraint(&conn, "delete from routing_host_observations where id = 1");
    try expectConstraint(&conn, "delete from routing_candidates where id = 1");
}

test "migration 00030 rejects ambiguous fact values and duplicate replay events" {
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, std.testing.allocator);
    try conn.exec(
        "insert into projects (slug, name) values ('routing-test', 'Routing test')",
    );
    try conn.exec(
        \\insert into tasks (scope_kind, scope_id, title)
        \\values ('repo', 1, 'routing task')
    );
    try expectConstraint(&conn,
        \\insert into routing_task_facts (
        \\  task_id, fact_kind, value_type, value_bool, value_integer,
        \\  source_entity_kind, source_entity_id, source_locator,
        \\  source_digest, materializer_version
        \\) values (
        \\  1, 'acceptance_complete', 'bool', 1, 1,
        \\  'task', 1, 'acceptance', 'digest', 'materializer-v1'
        \\)
    );

    try seedDispatch(&conn);
    try conn.exec(
        \\insert into routing_dispatch_events (
        \\  dispatch_id, event_id, sequence, event_kind, attempt_number,
        \\  payload_json, occurred_at
        \\) values (1, 'event-1', 1, 'attempt_started', 1, '{}', '2026-01-01T00:00:00Z')
    );
    try expectConstraint(&conn,
        \\insert into routing_dispatch_events (
        \\  dispatch_id, event_id, sequence, event_kind, attempt_number,
        \\  payload_json, occurred_at
        \\) values (1, 'event-1', 2, 'attempt_started', 2, '{}', '2026-01-01T00:01:00Z')
    );
    try expectConstraint(&conn,
        \\insert into routing_dispatch_events (
        \\  dispatch_id, event_id, sequence, event_kind, attempt_number,
        \\  payload_json, occurred_at
        \\) values (1, 'event-2', 1, 'attempt_started', 2, '{}', '2026-01-01T00:01:00Z')
    );
}

test "migration 00030 enforces foreign keys, bindings, and immutable audit rows" {
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, std.testing.allocator);
    try conn.exec(
        "insert into projects (slug, name) values ('routing-test', 'Routing test')",
    );
    try conn.exec(
        \\insert into tasks (scope_kind, scope_id, title)
        \\values ('repo', 1, 'routing task')
    );

    try expectConstraint(&conn,
        \\insert into routing_candidate_bindings (candidate_id, role, tier)
        \\values (999, 'coder', 'medium')
    );
    try conn.exec(
        \\insert into routing_candidates
        \\  (vendor, candidate_id, fallback_order)
        \\values ('vendor-x', 'candidate-a', 0)
    );
    try conn.exec(
        \\insert into routing_candidate_bindings (candidate_id, role, tier)
        \\values (1, 'coder', 'medium')
    );
    try expectConstraint(&conn,
        \\insert into routing_candidate_bindings (candidate_id, role, tier)
        \\values (1, 'coder', 'unknown')
    );
    try conn.exec(
        \\insert into routing_candidates
        \\  (vendor, candidate_id, fallback_order)
        \\values ('vendor-y', 'candidate-cross-vendor', 0)
    );

    try seedDispatchRow(&conn);
    try expectConstraint(
        &conn,
        "update routing_dispatch_snapshots set terminal_state = 'completed' where id = 1",
    );
    try expectConstraint(&conn, "delete from routing_dispatch_snapshots where id = 1");

    try seedExperiment(&conn);
    try expectConstraint(&conn, "delete from routing_experiments where id = 1");
    try expectConstraint(
        &conn,
        "update routing_experiments set candidate_set_json = '[]' where id = 1",
    );
    try conn.exec("update routing_experiments set status = 'running' where id = 1");
    try std.testing.expectEqual(
        @as(i64, 1),
        try conn.intQuery(
            "select count(*) from routing_experiments where id = 1 and status = 'running'",
        ),
    );

    try seedExperimentalDispatch(&conn);
    try expectConstraint(&conn,
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\)
        \\select
        \\  'cross-vendor-candidate', task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  2, 'vendor-y', 'candidate-cross-vendor',
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\from routing_dispatch_snapshots where id = 2
    );
    try expectConstraint(&conn,
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\)
        \\select
        \\  'bad-cohort', task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, 'small', work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\from routing_dispatch_snapshots where id = 2
    );
    try expectConstraint(&conn,
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\)
        \\select
        \\  'bad-population', task_id, 'task:missing', project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\from routing_dispatch_snapshots where id = 2
    );
    try conn.exec(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\)
        \\select
        \\  'actual-mismatch', task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, 'candidate-b',
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\from routing_dispatch_snapshots where id = 2
    );
    try conn.exec(
        \\insert into routing_dispatch_events (
        \\  dispatch_id, event_id, sequence, event_kind, terminal_state,
        \\  payload_json, occurred_at
        \\) values (
        \\  3, 'mismatch-completed-event', 1, 'outcome', 'completed', '{}',
        \\  '2026-01-01T00:09:00Z'
        \\)
    );
    try expectConstraint(&conn,
        \\insert into routing_terminal_samples (
        \\  experiment_id, logical_work_item_id, role, initial_packet_digest,
        \\  candidate_id, project_id, validation_policy_version,
        \\  routing_policy_version, vendor, tier, work_type, complexity,
        \\  terminal_event_id, terminal_state, quality_success,
        \\  cohort_eligible, finalized_at
        \\) values (
        \\  1, 'task:1', 'coder', 'packet-experiment', 1, 1,
        \\  'validation-v1', 'routing-v1', 'vendor-x', 'medium', 'schema',
        \\  'standard', 'mismatch-completed-event', 'completed', 1, 1,
        \\  '2026-01-01T00:10:00Z'
        \\)
    );
    try conn.exec(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\)
        \\select
        \\  'actual-mismatch-named', task_id, 'task:2', project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\from routing_dispatch_snapshots where id = 3
    );
    try conn.exec(
        \\insert into routing_dispatch_events (
        \\  dispatch_id, event_id, sequence, event_kind, terminal_state,
        \\  payload_json, occurred_at
        \\) values (
        \\  4, 'mismatch-named-event', 1, 'outcome', 'candidate_mismatch', '{}',
        \\  '2026-01-01T00:09:30Z'
        \\)
    );
    try conn.exec(
        \\insert into routing_terminal_samples (
        \\  experiment_id, logical_work_item_id, role, initial_packet_digest,
        \\  candidate_id, project_id, validation_policy_version,
        \\  routing_policy_version, vendor, tier, work_type, complexity,
        \\  terminal_event_id, terminal_state, quality_success,
        \\  cohort_eligible, exclusion_reason, finalized_at
        \\) values (
        \\  1, 'task:2', 'coder', 'packet-experiment', 1, 1,
        \\  'validation-v1', 'routing-v1', 'vendor-x', 'medium', 'schema',
        \\  'standard', 'mismatch-named-event', 'candidate_mismatch', 0, 0,
        \\  'actual_candidate_mismatch', '2026-01-01T00:10:00Z'
        \\)
    );
    try conn.exec(
        \\insert into routing_dispatch_events (
        \\  dispatch_id, event_id, sequence, event_kind, terminal_state,
        \\  payload_json, occurred_at
        \\) values (
        \\  2, 'terminal-event', 1, 'outcome', 'completed', '{}',
        \\  '2026-01-01T00:10:00Z'
        \\)
    );
    try conn.exec(
        \\insert into routing_candidates
        \\  (vendor, candidate_id, fallback_order)
        \\values ('vendor-x', 'candidate-b', 1)
    );
    try expectConstraint(&conn,
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\)
        \\select
        \\  'bad-candidate-set', task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  3, actual_vendor, 'candidate-b',
        \\  assignment_class, experiment_id, operator_decision,
        \\  reviewer_disposition, terminal_state, confirmed_at
        \\from routing_dispatch_snapshots where id = 2
    );
    try expectConstraint(&conn,
        \\insert into routing_terminal_samples (
        \\  experiment_id, logical_work_item_id, role, initial_packet_digest,
        \\  candidate_id, project_id, validation_policy_version,
        \\  routing_policy_version, vendor, tier, work_type, complexity,
        \\  terminal_event_id, terminal_state, quality_success,
        \\  cohort_eligible, finalized_at
        \\) values (
        \\  1, 'task:1', 'coder', 'packet-experiment', 3, 1,
        \\  'validation-v1', 'routing-v1', 'vendor-x', 'medium', 'schema',
        \\  'standard', 'terminal-event', 'completed', 1, 1,
        \\  '2026-01-01T00:11:00Z'
        \\)
    );
    try conn.exec(
        \\insert into routing_terminal_samples (
        \\  experiment_id, logical_work_item_id, role, initial_packet_digest,
        \\  candidate_id, project_id, validation_policy_version,
        \\  routing_policy_version, vendor, tier, work_type, complexity,
        \\  terminal_event_id, terminal_state, quality_success,
        \\  cohort_eligible, finalized_at
        \\) values (
        \\  1, 'task:1', 'coder', 'packet-experiment', 1, 1,
        \\  'validation-v1', 'routing-v1', 'vendor-x', 'medium', 'schema',
        \\  'standard', 'terminal-event', 'completed', 1, 1,
        \\  '2026-01-01T00:11:00Z'
        \\)
    );
    try expectConstraint(&conn, "delete from routing_terminal_samples where id = 1");
}

test "migration 00030 down preserves schema 29 state and legacy configuration" {
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();

    try db.migrate.applyAll(&conn, std.testing.allocator);
    try conn.exec(
        "insert into config (key, value) values ('models.legacy', 'opaque-old-id')",
    );
    const down_sql = try std.Io.Dir.cwd().readFileAlloc(
        std.testing.io,
        "migrations/00030_adaptive_routing_evidence.down.sql",
        std.testing.allocator,
        .limited(64 * 1024),
    );
    defer std.testing.allocator.free(down_sql);
    try conn.execSlice(std.testing.allocator, down_sql);
    try std.testing.expectEqual(
        @as(i64, 29),
        try conn.intQuery("select max(version) from schema_migrations"),
    );

    try db.migrate.applyAll(&conn, std.testing.allocator);
    try std.testing.expectEqual(
        @as(i64, 1),
        try conn.intQuery(
            \\select count(*) from routing_candidate_compatibility_v1
            \\where source = 'legacy_config'
            \\  and legacy_config_key = 'models.legacy'
            \\  and legacy_config_value = 'opaque-old-id'
        ),
    );
    try conn.execSlice(std.testing.allocator, down_sql);
    try std.testing.expectEqual(
        @as(i64, 29),
        try conn.intQuery("select max(version) from schema_migrations"),
    );
    try std.testing.expectEqual(
        @as(i64, 1),
        try conn.intQuery(
            "select count(*) from config where key = 'models.legacy' and value = 'opaque-old-id'",
        ),
    );
    try std.testing.expectError(
        error.PrepareFailed,
        conn.intQuery("select count(*) from routing_candidates"),
    );
}

fn expectConstraint(conn: *db.sqlite.Db, sql: [:0]const u8) !void {
    var stmt = try conn.prepare(sql);
    defer stmt.finalize();
    try std.testing.expectError(error.StepFailed, stmt.step());
}

fn seedExperiment(conn: *db.sqlite.Db) !void {
    try conn.exec(
        \\insert into routing_experiments (
        \\  experiment_key, project_id, validation_policy_version, vendor,
        \\  role, tier, work_type, complexity, routing_policy_version,
        \\  eligible_population_json, candidate_set_json, allocation_method,
        \\  stopping_rule_json, analysis_policy_json, manifest_digest,
        \\  operator_approved_at
        \\) values (
        \\  'experiment-1', 1, 'validation-v1', 'vendor-x', 'coder',
        \\  'medium', 'schema', 'standard', 'routing-v1',
        \\  '["task:1","task:2"]', '[1,2]',
        \\  'balanced', '{}', '{}', 'manifest',
        \\  '2026-01-01T00:00:00Z'
        \\)
    );
}

fn seedDispatch(conn: *db.sqlite.Db) !void {
    try conn.exec(
        \\insert into routing_candidates
        \\  (vendor, candidate_id, fallback_order)
        \\values ('vendor-x', 'candidate-a', 0)
    );
    try seedDispatchRow(conn);
}

fn seedDispatchRow(conn: *db.sqlite.Db) !void {
    try conn.exec(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, assignment_class, operator_decision,
        \\  reviewer_disposition, confirmed_at
        \\) values (
        \\  'dispatch-1', 1, 'task:1', 1, 'validation-v1', 'routing-v1',
        \\  'profile-v1', 'vendor-x', 'coder', 'medium', 'schema', 'standard',
        \\  'packet', 'policy', 'capability', 1, 'default', 'confirmed',
        \\  'required', '2026-01-01T00:00:00Z'
        \\)
    );
}

fn seedExperimentalDispatch(conn: *db.sqlite.Db) !void {
    try conn.exec(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, task_id, logical_work_item_id, project_id,
        \\  validation_policy_version, routing_policy_version,
        \\  profile_rule_version, vendor, role, tier, work_type, complexity,
        \\  packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id,
        \\  operator_decision, reviewer_disposition, confirmed_at
        \\) values (
        \\  'dispatch-experiment', 1, 'task:1', 1, 'validation-v1',
        \\  'routing-v1', 'profile-v1', 'vendor-x', 'coder', 'medium',
        \\  'schema', 'standard', 'packet-experiment', 'policy', 'capability',
        \\  1, 'vendor-x', 'candidate-a', 'declared_experiment', 1,
        \\  'confirmed', 'approved',
        \\  '2026-01-01T00:05:00Z'
        \\)
    );
}
