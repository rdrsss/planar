//! Typed persistence contracts for adaptive routing evidence.
//!
//! Candidate identifiers are deliberately opaque. These types describe rows
//! stored by migration 00030; classification and recommendation policy live in
//! later routing modules.

const std = @import("std");
const db = @import("db");

/// Stable schema contract introduced by migration 00030.
pub const schema_version: u32 = 31;

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

pub const RegistryCandidate = struct {
    registration: CandidateRegistration,
    bindings: []CandidateBinding,
    latest_observation: ?HostObservation,

    pub fn deinit(self: RegistryCandidate, allocator: std.mem.Allocator) void {
        allocator.free(self.registration.vendor);
        allocator.free(self.registration.candidate_id);
        allocator.free(self.registration.compatibility_source);
        for (self.bindings) |binding| allocator.free(binding.role);
        allocator.free(self.bindings);
        if (self.latest_observation) |observation| {
            allocator.free(observation.host_id);
            allocator.free(observation.evidence_ref);
            allocator.free(observation.captured_at);
            allocator.free(observation.expires_at);
        }
    }
};

pub const RegistryError = error{
    InvalidValue,
    NotFound,
    Conflict,
    QueryFailed,
    OutOfMemory,
};

pub const CreateCandidate = struct {
    vendor: []const u8,
    candidate_id: []const u8,
    enabled: bool = true,
    fallback_order: i64,
    compatibility_source: []const u8 = "native",
};

pub fn createCandidate(d: *db.sqlite.Db, args: CreateCandidate) RegistryError!i64 {
    if (!validOpaqueValue(args.vendor) or !validOpaqueValue(args.candidate_id) or
        args.fallback_order < 0 or
        !(std.mem.eql(u8, args.compatibility_source, "native") or
            std.mem.eql(u8, args.compatibility_source, "legacy_config")))
        return error.InvalidValue;
    return d.execParams(
        \\insert into routing_candidates
        \\  (vendor, candidate_id, enabled, fallback_order, compatibility_source)
        \\values (?, ?, ?, ?, ?)
    , &.{
        .{ .text = args.vendor },
        .{ .text = args.candidate_id },
        .{ .int = @intFromBool(args.enabled) },
        .{ .int = args.fallback_order },
        .{ .text = args.compatibility_source },
    }) catch return error.Conflict;
}

pub fn findCandidateId(d: *db.sqlite.Db, vendor: []const u8, candidate_id: []const u8) RegistryError!?i64 {
    var stmt = d.prepare(
        "select id from routing_candidates where vendor = ? and candidate_id = ?",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .text = vendor }, .{ .text = candidate_id } }) catch return error.QueryFailed;
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => null,
        .row => stmt.columnInt(0),
    };
}

pub fn updateCandidate(
    d: *db.sqlite.Db,
    id: i64,
    enabled: bool,
    fallback_order: i64,
) RegistryError!void {
    if (fallback_order < 0) return error.InvalidValue;
    _ = d.execParams(
        \\update routing_candidates
        \\set enabled = ?, fallback_order = ?,
        \\    registration_version = registration_version + 1,
        \\    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
        \\where id = ?
    , &.{ .{ .int = @intFromBool(enabled) }, .{ .int = fallback_order }, .{ .int = id } }) catch
        return error.QueryFailed;
    if (d.changes() != 1) return error.NotFound;
}

pub fn deleteCandidate(d: *db.sqlite.Db, id: i64) RegistryError!void {
    _ = d.execParams("delete from routing_candidates where id = ?", &.{.{ .int = id }}) catch
        return error.QueryFailed;
    if (d.changes() != 1) return error.NotFound;
}

pub fn bindCandidate(
    d: *db.sqlite.Db,
    candidate_id: i64,
    role: []const u8,
    tier: Tier,
) RegistryError!void {
    if (!validOpaqueValue(role)) return error.InvalidValue;
    _ = d.execParams(
        \\insert into routing_candidate_bindings (candidate_id, role, tier)
        \\values (?, ?, ?)
        \\on conflict(candidate_id, role, tier) do nothing
    , &.{ .{ .int = candidate_id }, .{ .text = role }, .{ .text = @tagName(tier) } }) catch
        return error.QueryFailed;
}

pub fn unbindCandidate(
    d: *db.sqlite.Db,
    candidate_id: i64,
    role: []const u8,
    tier: Tier,
) RegistryError!void {
    _ = d.execParams(
        "delete from routing_candidate_bindings where candidate_id = ? and role = ? and tier = ?",
        &.{ .{ .int = candidate_id }, .{ .text = role }, .{ .text = @tagName(tier) } },
    ) catch return error.QueryFailed;
}

pub const ObserveCandidate = struct {
    candidate_id: i64,
    host_id: []const u8,
    observation_version: i64,
    availability: Availability,
    spawn_verification: SpawnVerification,
    evidence_ref: []const u8,
    captured_at: []const u8,
    expires_at: []const u8,
};

pub fn observeCandidate(d: *db.sqlite.Db, args: ObserveCandidate) RegistryError!i64 {
    if (!validOpaqueValue(args.host_id) or !validOpaqueValue(args.evidence_ref) or
        args.observation_version <= 0 or
        std.mem.order(u8, args.expires_at, args.captured_at) != .gt)
        return error.InvalidValue;
    return d.execParams(
        \\insert into routing_host_observations
        \\  (candidate_id, host_id, observation_version, availability,
        \\   spawn_verification, evidence_ref, captured_at, expires_at)
        \\values (?, ?, ?, ?, ?, ?, ?, ?)
    , &.{
        .{ .int = args.candidate_id },
        .{ .text = args.host_id },
        .{ .int = args.observation_version },
        .{ .text = @tagName(args.availability) },
        .{ .text = @tagName(args.spawn_verification) },
        .{ .text = args.evidence_ref },
        .{ .text = args.captured_at },
        .{ .text = args.expires_at },
    }) catch return error.Conflict;
}

pub fn listCandidates(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) RegistryError![]RegistryCandidate {
    var out: std.ArrayList(RegistryCandidate) = .empty;
    errdefer {
        for (out.items) |candidate| candidate.deinit(allocator);
        out.deinit(allocator);
    }
    var stmt = d.prepare(
        \\select id, vendor, candidate_id, enabled, fallback_order,
        \\       registration_version, compatibility_source
        \\from routing_candidates
        \\order by vendor, fallback_order, candidate_id
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    while (true) switch (stmt.step() catch return error.QueryFailed) {
        .done => break,
        .row => {
            const id = stmt.columnInt(0);
            try out.append(allocator, .{
                .registration = .{
                    .id = id,
                    .vendor = stmt.columnTextAlloc(1, allocator) catch return error.OutOfMemory,
                    .candidate_id = stmt.columnTextAlloc(2, allocator) catch return error.OutOfMemory,
                    .enabled = stmt.columnInt(3) != 0,
                    .fallback_order = stmt.columnInt(4),
                    .registration_version = stmt.columnInt(5),
                    .compatibility_source = stmt.columnTextAlloc(6, allocator) catch return error.OutOfMemory,
                },
                .bindings = try readBindings(d, allocator, id),
                .latest_observation = try readLatestObservation(d, allocator, id),
            });
        },
    };
    return out.toOwnedSlice(allocator);
}

/// Read one candidate with the latest observation made by the requested host.
/// Host identity is mandatory at eligibility time; observations from other
/// hosts are never substituted even when they carry a larger version number.
pub fn getCandidateForHost(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    host_id: []const u8,
) RegistryError!RegistryCandidate {
    if (!validOpaqueValue(host_id)) return error.InvalidValue;
    var stmt = d.prepare(
        \\select id, vendor, candidate_id, enabled, fallback_order,
        \\       registration_version, compatibility_source
        \\from routing_candidates where id = ?
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    if ((stmt.step() catch return error.QueryFailed) == .done) return error.NotFound;
    const candidate: RegistryCandidate = .{
        .registration = .{
            .id = stmt.columnInt(0),
            .vendor = stmt.columnTextAlloc(1, allocator) catch return error.OutOfMemory,
            .candidate_id = stmt.columnTextAlloc(2, allocator) catch return error.OutOfMemory,
            .enabled = stmt.columnInt(3) != 0,
            .fallback_order = stmt.columnInt(4),
            .registration_version = stmt.columnInt(5),
            .compatibility_source = stmt.columnTextAlloc(6, allocator) catch return error.OutOfMemory,
        },
        .bindings = try readBindings(d, allocator, id),
        .latest_observation = try readLatestObservationForHost(d, allocator, id, host_id),
    };
    return candidate;
}

fn readBindings(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) RegistryError![]CandidateBinding {
    var out: std.ArrayList(CandidateBinding) = .empty;
    errdefer {
        for (out.items) |binding| allocator.free(binding.role);
        out.deinit(allocator);
    }
    var stmt = d.prepare(
        "select role, tier from routing_candidate_bindings where candidate_id = ? order by role, tier",
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    while (true) switch (stmt.step() catch return error.QueryFailed) {
        .done => break,
        .row => {
            const tier_text = stmt.columnTextAlloc(1, allocator) catch return error.OutOfMemory;
            defer allocator.free(tier_text);
            try out.append(allocator, .{
                .candidate_id = id,
                .role = stmt.columnTextAlloc(0, allocator) catch return error.OutOfMemory,
                .tier = parseTier(tier_text) orelse return error.QueryFailed,
            });
        },
    };
    return out.toOwnedSlice(allocator);
}

fn readLatestObservation(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
) RegistryError!?HostObservation {
    var stmt = d.prepare(
        \\select id, host_id, observation_version, availability,
        \\       spawn_verification, evidence_ref, captured_at, expires_at
        \\from routing_host_observations
        \\where candidate_id = ?
        \\order by observation_version desc, id desc limit 1
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => null,
        .row => blk: {
            const availability = stmt.columnTextAlloc(3, allocator) catch return error.OutOfMemory;
            defer allocator.free(availability);
            const verification = stmt.columnTextAlloc(4, allocator) catch return error.OutOfMemory;
            defer allocator.free(verification);
            break :blk .{
                .id = stmt.columnInt(0),
                .candidate_id = id,
                .host_id = stmt.columnTextAlloc(1, allocator) catch return error.OutOfMemory,
                .observation_version = stmt.columnInt(2),
                .availability = parseAvailability(availability) orelse return error.QueryFailed,
                .spawn_verification = parseSpawnVerification(verification) orelse return error.QueryFailed,
                .evidence_ref = stmt.columnTextAlloc(5, allocator) catch return error.OutOfMemory,
                .captured_at = stmt.columnTextAlloc(6, allocator) catch return error.OutOfMemory,
                .expires_at = stmt.columnTextAlloc(7, allocator) catch return error.OutOfMemory,
            };
        },
    };
}

fn readLatestObservationForHost(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    id: i64,
    host_id: []const u8,
) RegistryError!?HostObservation {
    var stmt = d.prepare(
        \\select id, host_id, observation_version, availability,
        \\       spawn_verification, evidence_ref, captured_at, expires_at
        \\from routing_host_observations
        \\where candidate_id = ? and host_id = ?
        \\order by observation_version desc, id desc limit 1
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = id }, .{ .text = host_id } }) catch return error.QueryFailed;
    return readObservationRow(&stmt, allocator, id);
}

fn readObservationRow(
    stmt: anytype,
    allocator: std.mem.Allocator,
    id: i64,
) RegistryError!?HostObservation {
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => null,
        .row => blk: {
            const availability = stmt.columnTextAlloc(3, allocator) catch return error.OutOfMemory;
            defer allocator.free(availability);
            const verification = stmt.columnTextAlloc(4, allocator) catch return error.OutOfMemory;
            defer allocator.free(verification);
            break :blk .{
                .id = stmt.columnInt(0),
                .candidate_id = id,
                .host_id = stmt.columnTextAlloc(1, allocator) catch return error.OutOfMemory,
                .observation_version = stmt.columnInt(2),
                .availability = parseAvailability(availability) orelse return error.QueryFailed,
                .spawn_verification = parseSpawnVerification(verification) orelse return error.QueryFailed,
                .evidence_ref = stmt.columnTextAlloc(5, allocator) catch return error.OutOfMemory,
                .captured_at = stmt.columnTextAlloc(6, allocator) catch return error.OutOfMemory,
                .expires_at = stmt.columnTextAlloc(7, allocator) catch return error.OutOfMemory,
            };
        },
    };
}

fn parseTier(value: []const u8) ?Tier {
    inline for (std.meta.tags(Tier)) |tag| if (std.mem.eql(u8, value, @tagName(tag))) return tag;
    return null;
}

fn parseAvailability(value: []const u8) ?Availability {
    inline for (std.meta.tags(Availability)) |tag| if (std.mem.eql(u8, value, @tagName(tag))) return tag;
    return null;
}

fn parseSpawnVerification(value: []const u8) ?SpawnVerification {
    inline for (std.meta.tags(SpawnVerification)) |tag| if (std.mem.eql(u8, value, @tagName(tag))) return tag;
    return null;
}

/// Independent dispatch gates for one opaque registration.  These fields are
/// intentionally not collapsed into a score: callers must retain the named
/// reason for every failed gate.
pub const Eligibility = struct {
    cli_available: bool,
    exact_spawn_verified: bool,
    role_tier_bound: bool,
    role_surface_override_supported: bool,
    host_policy_permits: bool,
    observation_fresh: bool,

    pub fn eligible(self: Eligibility) bool {
        return self.cli_available and self.exact_spawn_verified and
            self.role_tier_bound and self.role_surface_override_supported and
            self.host_policy_permits and self.observation_fresh;
    }

    pub fn reasonCount(self: Eligibility) usize {
        return @as(usize, @intFromBool(!self.cli_available)) +
            @as(usize, @intFromBool(!self.exact_spawn_verified)) +
            @as(usize, @intFromBool(!self.role_tier_bound)) +
            @as(usize, @intFromBool(!self.role_surface_override_supported)) +
            @as(usize, @intFromBool(!self.host_policy_permits)) +
            @as(usize, @intFromBool(!self.observation_fresh));
    }

    pub fn reasons(self: Eligibility, buffer: *[6]EligibilityReason) []const EligibilityReason {
        var len: usize = 0;
        if (!self.cli_available) {
            buffer[len] = .provider_cli_unavailable;
            len += 1;
        }
        if (!self.exact_spawn_verified) {
            buffer[len] = .exact_spawn_unverified;
            len += 1;
        }
        if (!self.role_tier_bound) {
            buffer[len] = .role_tier_not_bound;
            len += 1;
        }
        if (!self.role_surface_override_supported) {
            buffer[len] = .role_surface_override_unsupported;
            len += 1;
        }
        if (!self.host_policy_permits) {
            buffer[len] = .host_policy_denied;
            len += 1;
        }
        if (!self.observation_fresh) {
            buffer[len] = .host_observation_expired;
            len += 1;
        }
        return buffer[0..len];
    }
};

pub const EligibilityReason = enum {
    provider_cli_unavailable,
    exact_spawn_unverified,
    role_tier_not_bound,
    role_surface_override_unsupported,
    host_policy_denied,
    host_observation_expired,
};

/// Inputs already established by the host/configuration boundary.  Planar
/// never derives any field from a candidate ID or display metadata.
pub const EligibilityInput = struct {
    enabled: bool,
    binding_present: bool,
    role_surface_override_supported: bool,
    host_policy_permits: bool,
    observation: ?HostObservation,
    now: []const u8,
};

pub fn evaluateEligibility(input: EligibilityInput) Eligibility {
    const observation = input.observation;
    return .{
        .cli_available = observation != null and observation.?.availability == .available,
        .exact_spawn_verified = observation != null and observation.?.spawn_verification == .verified,
        .role_tier_bound = input.enabled and input.binding_present,
        .role_surface_override_supported = input.role_surface_override_supported,
        .host_policy_permits = input.host_policy_permits,
        .observation_fresh = observation != null and
            std.mem.order(u8, input.now, observation.?.expires_at) == .lt,
    };
}

/// Reject values that cannot safely cross JSON/argv boundaries.  Punctuation,
/// whitespace, leading dashes, and shell metacharacters remain opaque data.
pub fn validOpaqueValue(value: []const u8) bool {
    if (value.len == 0) return false;
    for (value) |c| if (c < 0x20 or c == 0x7f) return false;
    return true;
}

pub const IdentityVerification = enum {
    matched,
    missing_actual_identity,
    vendor_mismatch,
    candidate_mismatch,
};

pub fn verifyActualIdentity(
    requested_vendor: []const u8,
    requested_candidate: []const u8,
    actual_vendor: ?[]const u8,
    actual_candidate: ?[]const u8,
) IdentityVerification {
    if (actual_vendor == null or actual_candidate == null) return .missing_actual_identity;
    if (!std.mem.eql(u8, requested_vendor, actual_vendor.?)) return .vendor_mismatch;
    if (!std.mem.eql(u8, requested_candidate, actual_candidate.?)) return .candidate_mismatch;
    return .matched;
}

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

test "eligibility names independent provider, verification, binding, policy, and freshness gates" {
    const observation: HostObservation = .{
        .id = 1,
        .candidate_id = 1,
        .host_id = "host-a",
        .observation_version = 3,
        .availability = .unavailable,
        .spawn_verification = .mismatch,
        .evidence_ref = "probe:3",
        .captured_at = "2026-01-01T00:00:00Z",
        .expires_at = "2026-01-02T00:00:00Z",
    };
    const eligibility = evaluateEligibility(.{
        .enabled = false,
        .binding_present = true,
        .role_surface_override_supported = false,
        .host_policy_permits = false,
        .observation = observation,
        .now = "2026-01-03T00:00:00Z",
    });
    try std.testing.expect(!eligibility.eligible());
    try std.testing.expect(!eligibility.cli_available);
    try std.testing.expect(!eligibility.exact_spawn_verified);
    try std.testing.expect(!eligibility.role_tier_bound);
    try std.testing.expect(!eligibility.observation_fresh);
    try std.testing.expectEqual(@as(usize, 6), eligibility.reasonCount());
}

test "opaque identifiers preserve shell punctuation but reject control characters" {
    try std.testing.expect(validOpaqueValue("--opaque value; $() 'quoted'"));
    try std.testing.expect(!validOpaqueValue("unsafe\nvalue"));
    try std.testing.expect(!validOpaqueValue(""));
}

test "registry CRUD exposes bindings and latest versioned host observation" {
    const allocator = std.testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, allocator);

    const id = try createCandidate(&conn, .{
        .vendor = "vendor-x",
        .candidate_id = "--opaque value; $()",
        .fallback_order = 7,
    });
    try bindCandidate(&conn, id, "coder", .medium);
    _ = try observeCandidate(&conn, .{
        .candidate_id = id,
        .host_id = "host-a",
        .observation_version = 1,
        .availability = .available,
        .spawn_verification = .unverified,
        .evidence_ref = "probe:1",
        .captured_at = "2026-01-01T00:00:00Z",
        .expires_at = "2026-01-02T00:00:00Z",
    });
    _ = try observeCandidate(&conn, .{
        .candidate_id = id,
        .host_id = "host-a",
        .observation_version = 100,
        .availability = .unavailable,
        .spawn_verification = .failed,
        .evidence_ref = "probe:2",
        .captured_at = "2026-01-02T00:00:00Z",
        .expires_at = "2026-01-03T00:00:00Z",
    });
    _ = try observeCandidate(&conn, .{
        .candidate_id = id,
        .host_id = "host-b",
        .observation_version = 1,
        .availability = .available,
        .spawn_verification = .verified,
        .evidence_ref = "host-b:1",
        .captured_at = "2026-01-02T00:00:00Z",
        .expires_at = "2027-01-03T00:00:00Z",
    });
    try updateCandidate(&conn, id, false, 3);

    const rows = try listCandidates(&conn, allocator);
    defer {
        for (rows) |row| row.deinit(allocator);
        allocator.free(rows);
    }
    try std.testing.expectEqual(@as(usize, 1), rows.len);
    try std.testing.expectEqualStrings("--opaque value; $()", rows[0].registration.candidate_id);
    try std.testing.expect(!rows[0].registration.enabled);
    try std.testing.expectEqual(@as(i64, 3), rows[0].registration.fallback_order);
    try std.testing.expectEqual(@as(usize, 1), rows[0].bindings.len);
    try std.testing.expectEqual(@as(i64, 100), rows[0].latest_observation.?.observation_version);
    try std.testing.expectEqual(Availability.unavailable, rows[0].latest_observation.?.availability);

    const host_b = try getCandidateForHost(&conn, allocator, id, "host-b");
    defer host_b.deinit(allocator);
    try std.testing.expectEqual(@as(i64, 1), host_b.latest_observation.?.observation_version);
    try std.testing.expectEqual(Availability.available, host_b.latest_observation.?.availability);
    try std.testing.expectEqual(SpawnVerification.verified, host_b.latest_observation.?.spawn_verification);

    try unbindCandidate(&conn, id, "coder", .medium);
    try std.testing.expectError(error.QueryFailed, deleteCandidate(&conn, id));
}

test "requested and actual identities never alias" {
    try std.testing.expectEqual(
        IdentityVerification.matched,
        verifyActualIdentity("vendor-x", "candidate-a", "vendor-x", "candidate-a"),
    );
    try std.testing.expectEqual(
        IdentityVerification.vendor_mismatch,
        verifyActualIdentity("vendor-x", "candidate-a", "vendor-y", "candidate-a"),
    );
    try std.testing.expectEqual(
        IdentityVerification.candidate_mismatch,
        verifyActualIdentity("vendor-x", "candidate-a", "vendor-x", "candidate-b"),
    );
    try std.testing.expectEqual(
        IdentityVerification.missing_actual_identity,
        verifyActualIdentity("vendor-x", "candidate-a", null, null),
    );
}

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

/// Roll the routing migrations back to schema 29.
///
/// Downs unwind in reverse order: 00031's table references 00030's, so rolling
/// 30 back first would leave a dangling reference and a stale 31 row in
/// schema_migrations.
fn unwindToSchema29(conn: *db.sqlite.Db) !void {
    for ([_][]const u8{
        "migrations/00031_dispatch_confirmation_tokens.down.sql",
        "migrations/00030_adaptive_routing_evidence.down.sql",
    }) |path| {
        const down_sql = try std.Io.Dir.cwd().readFileAlloc(
            std.testing.io,
            path,
            std.testing.allocator,
            .limited(64 * 1024),
        );
        defer std.testing.allocator.free(down_sql);
        try conn.execSlice(std.testing.allocator, down_sql);
    }
}

test "migration 00030 down preserves schema 29 state and legacy configuration" {
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();

    try db.migrate.applyAll(&conn, std.testing.allocator);
    try conn.exec(
        "insert into config (key, value) values ('models.legacy', 'opaque-old-id')",
    );
    try unwindToSchema29(&conn);
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
    try unwindToSchema29(&conn);
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
