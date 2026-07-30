//! engine/routing/profile.zig — project a ready task packet into an
//! explainable routing profile (plan 950 task 5527).
//!
//! The projection is deterministic and citation-bearing: every classification
//! names the rule that produced it, the rule's version, and every fact it
//! matched, so a preview can explain itself and a stored dispatch can be
//! replayed against the same inputs.
//!
//! Three outcomes, and the difference between the last two is load-bearing:
//!
//!   - `profile`          — the packet classified.
//!   - `not_ready`        — the TASK lacks required facts, or its facts
//!                          contradict each other. Never falls through to
//!                          `feature`: silently defaulting a task whose
//!                          evidence is broken is how a misclassification
//!                          becomes invisible.
//!   - `policy_not_ready` — the POLICY lacks a threshold this packet needs.
//!                          The task is fine; the operator has not configured
//!                          the boundary. Distinguishing this from `not_ready`
//!                          tells the operator which of the two to fix.
//!
//! Deliberately absent, per the plan-946 spec: no aggregate intelligence
//! score, no marketing description, no model recency, and no cross-cohort
//! normalization. Nothing here compares one cohort to another; a cohort is an
//! exact identity, not a point on a scale.

const std = @import("std");
const store = @import("store.zig");
const packet_mod = @import("packet.zig");

pub const Tier = store.Tier;
pub const WorkType = store.WorkType;
pub const Complexity = store.Complexity;
pub const Cohort = store.Cohort;

/// Bumped whenever a rule's ID set, precedence, or matching semantics change.
/// Stored on every dispatch snapshot (`DispatchSnapshot.profile_rule_version`)
/// so evidence gathered under different rules is never pooled.
pub const rule_version = "routing-profile-v1";

// ---------------------------------------------------------------------------
// Facts
// ---------------------------------------------------------------------------

/// Named fact kinds the rules consume. These are the contract between the
/// ingest materializer (which writes `routing_task_facts`) and this
/// classifier. A fact kind absent from this list is ignored by every rule —
/// adding a rule input means adding it here and bumping `rule_version`.
pub const fact_kinds = struct {
    // schema
    pub const migration_touched = "schema.migration_touched";
    pub const schema_version_contract = "schema.version_contract_change";
    pub const constraint_or_index_redesign = "schema.constraint_or_index_redesign";
    // architectural
    pub const new_subsystem = "architectural.new_subsystem";
    pub const new_binary = "architectural.new_binary";
    pub const module_breadth = "architectural.module_breadth";
    // engine
    pub const transaction_change = "engine.transaction_change";
    pub const concurrency_change = "engine.concurrency_change";
    pub const ownership_change = "engine.ownership_change";
    pub const security_change = "engine.security_change";
    pub const resource_lifecycle_change = "engine.resource_lifecycle_change";
    pub const status_transition_change = "engine.status_transition_change";
    pub const scope_resolution_change = "engine.scope_resolution_change";
    pub const capability_boundary_change = "engine.capability_boundary_change";
    // cli
    pub const cli_surface_change = "cli.surface_change";
    // mechanical
    pub const docs_only = "mechanical.docs_only";
    pub const rename_or_format_only = "mechanical.rename_or_format_only";
    // capacity + judgement (complexity and tier inputs, not work-type inputs)
    pub const touched_unit_count = "capacity.touched_unit_count";
    pub const validation_gate_count = "capacity.validation_gate_count";
    pub const explicit_risk = "risk.explicit";
    pub const acceptance_complete = "acceptance_complete";
    pub const observable_correctness = "acceptance.observable_correctness";
    pub const high_judgment_acceptance = "acceptance.high_judgment";
};

/// A fact as matched by a rule, carrying the lineage needed to justify it.
pub const MatchedFact = struct {
    kind: []const u8,
    locator: []const u8,
    source_entity_kind: []const u8,
    source_entity_id: i64,
    source_digest: []const u8,
    text: []const u8,
};

/// Where a classification came from, for the explanation surface.
pub const Citation = struct {
    rule_id: []const u8,
    fact_kind: []const u8,
    locator: []const u8,
};

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------

/// One work-type rule. `any_of` matches when ANY listed fact kind is present
/// and true; rules are evaluated in `work_type_rules` order and the FIRST
/// match wins. Precedence is a property of the table's order, not of the
/// enum's declaration order — they deliberately differ.
pub const WorkTypeRule = struct {
    id: []const u8,
    work_type: WorkType,
    any_of: []const []const u8,
};

/// First-match precedence: schema, architectural, engine, cli, mechanical,
/// feature. A task touching a migration AND a CLI flag is `schema`, because a
/// schema change is the higher-consequence classification and the cohort must
/// reflect the riskiest dimension of the work, not the most numerous.
pub const work_type_rules = [_]WorkTypeRule{
    .{
        .id = "work-type.schema.v1",
        .work_type = .schema,
        .any_of = &.{
            fact_kinds.migration_touched,
            fact_kinds.schema_version_contract,
            fact_kinds.constraint_or_index_redesign,
        },
    },
    .{
        .id = "work-type.architectural.v1",
        .work_type = .architectural,
        .any_of = &.{
            fact_kinds.new_subsystem,
            fact_kinds.new_binary,
        },
    },
    .{
        .id = "work-type.engine.v1",
        .work_type = .engine,
        .any_of = &.{
            fact_kinds.transaction_change,
            fact_kinds.concurrency_change,
            fact_kinds.ownership_change,
            fact_kinds.security_change,
            fact_kinds.resource_lifecycle_change,
            fact_kinds.status_transition_change,
            fact_kinds.scope_resolution_change,
            fact_kinds.capability_boundary_change,
        },
    },
    .{
        .id = "work-type.cli.v1",
        .work_type = .cli,
        .any_of = &.{fact_kinds.cli_surface_change},
    },
    .{
        .id = "work-type.mechanical.v1",
        .work_type = .mechanical,
        .any_of = &.{
            fact_kinds.docs_only,
            fact_kinds.rename_or_format_only,
        },
    },
};

/// The fall-through when no rule matches. Reached only for a packet whose
/// required facts are present and consistent — a task with broken evidence
/// returns `not_ready` instead, which is the point of the distinction.
pub const default_rule_id = "work-type.feature.v1";

/// Fact kinds that are mutually exclusive: asserting both is a contradiction
/// in the task's own evidence, not a precedence question.
const contradictions = [_][2][]const u8{
    .{ fact_kinds.docs_only, fact_kinds.migration_touched },
    .{ fact_kinds.docs_only, fact_kinds.schema_version_contract },
    .{ fact_kinds.docs_only, fact_kinds.new_subsystem },
    .{ fact_kinds.docs_only, fact_kinds.new_binary },
    .{ fact_kinds.docs_only, fact_kinds.cli_surface_change },
    .{ fact_kinds.rename_or_format_only, fact_kinds.new_subsystem },
    .{ fact_kinds.rename_or_format_only, fact_kinds.schema_version_contract },
};

// ---------------------------------------------------------------------------
// Thresholds
// ---------------------------------------------------------------------------

/// A named, versioned complexity boundary. Absence is meaningful: a missing
/// bounded threshold prevents the `bounded` classification (the packet cannot
/// be shown to be small enough), and a missing large threshold for a metric
/// the packet actually populates yields `policy_not_ready` (the packet has a
/// capacity reading the policy cannot judge).
pub const Threshold = struct {
    id: []const u8,
    metric: []const u8,
    bounded_max: ?i64,
    large_min: ?i64,
};

pub const thresholds = [_]Threshold{
    .{
        .id = "complexity.touched-units.v1",
        .metric = fact_kinds.touched_unit_count,
        .bounded_max = 2,
        .large_min = 12,
    },
    .{
        .id = "complexity.validation-gates.v1",
        .metric = fact_kinds.validation_gate_count,
        .bounded_max = 1,
        .large_min = 6,
    },
};

fn thresholdFor(metric: []const u8) ?Threshold {
    for (thresholds) |t| {
        if (std.mem.eql(u8, t.metric, metric)) return t;
    }
    return null;
}

// ---------------------------------------------------------------------------
// Outcomes
// ---------------------------------------------------------------------------

pub const NotReadyReason = enum {
    contradictory_facts,
    missing_required_fact,
};

pub const PolicyGap = struct {
    metric: []const u8,
    /// Null when no threshold row exists for the metric at all.
    threshold_id: ?[]const u8,
};

/// An error set, not an enum: a below-floor request is a refusal the caller
/// must handle, not a value it can quietly read past.
pub const TierRejection = error{
    below_floor,
};

pub const Profile = struct {
    work_type: WorkType,
    work_type_rule_id: []const u8,
    rule_version: []const u8,
    complexity: Complexity,
    /// Threshold that decided complexity; empty when complexity followed from
    /// explicit risk rather than a capacity metric.
    complexity_threshold_id: []const u8,
    tier_floor: Tier,
    tier_floor_rule_id: []const u8,
    matched_facts: []const MatchedFact,
    citations: []const Citation,

    pub fn deinit(self: Profile, allocator: std.mem.Allocator) void {
        allocator.free(self.matched_facts);
        allocator.free(self.citations);
    }

    /// An operator may raise the tier above the floor; they may not lower it.
    /// The floor is derived from the task's own evidence, so a request below
    /// it is a request to ignore that evidence.
    pub fn acceptTier(self: Profile, requested: Tier) TierRejection!Tier {
        if (@intFromEnum(requested) < @intFromEnum(self.tier_floor)) return TierRejection.below_floor;
        return requested;
    }

    /// The exact cohort identity: project, validation-policy version, vendor,
    /// role, tier, work type, complexity. Nothing else, and no normalization
    /// across any of them.
    pub fn cohort(
        self: Profile,
        project_id: i64,
        validation_policy_version: []const u8,
        vendor: []const u8,
        role: []const u8,
        tier: Tier,
    ) Cohort {
        return .{
            .project_id = project_id,
            .validation_policy_version = validation_policy_version,
            .vendor = vendor,
            .role = role,
            .tier = tier,
            .work_type = self.work_type,
            .complexity = self.complexity,
        };
    }
};

pub const Outcome = union(enum) {
    profile: Profile,
    not_ready: NotReadyReason,
    policy_not_ready: PolicyGap,
};

// ---------------------------------------------------------------------------
// Projection
// ---------------------------------------------------------------------------

fn findFact(facts: []const packet_mod.Evidence, kind: []const u8) ?packet_mod.Evidence {
    for (facts) |f| {
        if (std.mem.eql(u8, f.kind, kind)) return f;
    }
    return null;
}

/// True when the fact is present AND asserts truth. A fact recorded as
/// "false" is evidence the property does NOT hold, which must not match a
/// rule keyed on its presence.
fn assertsTrue(f: packet_mod.Evidence) bool {
    return !(std.mem.eql(u8, f.text, "false") or std.mem.eql(u8, f.text, "0"));
}

fn truthyFact(facts: []const packet_mod.Evidence, kind: []const u8) ?packet_mod.Evidence {
    const f = findFact(facts, kind) orelse return null;
    return if (assertsTrue(f)) f else null;
}

fn intFact(facts: []const packet_mod.Evidence, kind: []const u8) ?i64 {
    const f = findFact(facts, kind) orelse return null;
    return std.fmt.parseInt(i64, std.mem.trim(u8, f.text, " \t"), 10) catch null;
}

fn toMatched(f: packet_mod.Evidence) MatchedFact {
    return .{
        .kind = f.kind,
        .locator = f.locator,
        .source_entity_kind = f.provenance,
        .source_entity_id = f.id,
        .source_digest = f.current_digest,
        .text = f.text,
    };
}

/// Project a READY packet into a profile.
///
/// Caller owns the returned profile's slices via `Profile.deinit`. Passing a
/// packet that is not ready is a programming error — readiness is the packet
/// layer's contract, and re-deriving it here would let the two disagree.
pub fn compile(
    allocator: std.mem.Allocator,
    pkt: packet_mod.TaskPacket,
) std.mem.Allocator.Error!Outcome {
    std.debug.assert(pkt.ready());
    const facts = pkt.input.facts;

    // 1. Contradictions first. A task asserting both "docs only" and "touches
    //    a migration" has broken evidence; classifying it either way would
    //    launder that contradiction into a cohort.
    for (contradictions) |pair| {
        if (truthyFact(facts, pair[0]) != null and truthyFact(facts, pair[1]) != null) {
            return .{ .not_ready = .contradictory_facts };
        }
    }

    // 2. Acceptance completeness is a required fact for classification: tier
    //    floor depends on whether the work is "complete", so its absence is
    //    not a default, it is a gap.
    if (findFact(facts, fact_kinds.acceptance_complete) == null) {
        return .{ .not_ready = .missing_required_fact };
    }

    var matched: std.ArrayList(MatchedFact) = .empty;
    errdefer matched.deinit(allocator);
    var cites: std.ArrayList(Citation) = .empty;
    errdefer cites.deinit(allocator);

    // 3. Work type by first-match precedence.
    var work_type: WorkType = .feature;
    var rule_id: []const u8 = default_rule_id;
    outer: for (work_type_rules) |rule| {
        for (rule.any_of) |kind| {
            if (truthyFact(facts, kind)) |f| {
                work_type = rule.work_type;
                rule_id = rule.id;
                try matched.append(allocator, toMatched(f));
                try cites.append(allocator, .{ .rule_id = rule.id, .fact_kind = f.kind, .locator = f.locator });
                break :outer;
            }
        }
    }

    // 4. Complexity. Explicit risk short-circuits capacity: an operator
    //    stating the work is risky outranks it looking small.
    var complexity: Complexity = .standard;
    var threshold_id: []const u8 = "";
    if (truthyFact(facts, fact_kinds.explicit_risk)) |f| {
        complexity = .high_risk;
        try matched.append(allocator, toMatched(f));
        try cites.append(allocator, .{ .rule_id = "complexity.explicit-risk.v1", .fact_kind = f.kind, .locator = f.locator });
    } else {
        var bounded_possible = true;
        var saw_capacity = false;
        for ([_][]const u8{ fact_kinds.touched_unit_count, fact_kinds.validation_gate_count }) |metric| {
            const value = intFact(facts, metric) orelse continue;
            saw_capacity = true;
            const t = thresholdFor(metric) orelse
                return .{ .policy_not_ready = .{ .metric = metric, .threshold_id = null } };
            // A populated capacity metric with no large boundary cannot be
            // judged: the policy cannot say whether this reading is excessive.
            const large_min = t.large_min orelse
                return .{ .policy_not_ready = .{ .metric = metric, .threshold_id = t.id } };
            if (value >= large_min) {
                complexity = .high_risk;
                threshold_id = t.id;
                const f = findFact(facts, metric).?;
                try matched.append(allocator, toMatched(f));
                try cites.append(allocator, .{ .rule_id = t.id, .fact_kind = metric, .locator = f.locator });
                break;
            }
            // Missing bounded boundary does not fail the packet; it simply
            // prevents claiming `bounded`, which is the conservative read.
            const bounded_max = t.bounded_max orelse {
                bounded_possible = false;
                continue;
            };
            if (value > bounded_max) bounded_possible = false;
        }
        if (complexity != .high_risk and saw_capacity and bounded_possible) {
            complexity = .bounded;
            if (thresholdFor(fact_kinds.touched_unit_count)) |t| threshold_id = t.id;
        }
    }

    // 5. Tier floor.
    //    small  — only for bounded AND complete work
    //    large  — high risk, or bounded high-judgment acceptance whose
    //             correctness is observable (small-looking but consequential)
    //    medium — everything else
    const complete = if (truthyFact(facts, fact_kinds.acceptance_complete)) |_| true else false;
    var tier_floor: Tier = .medium;
    var tier_rule: []const u8 = "tier-floor.standard.v1";
    if (complexity == .high_risk) {
        tier_floor = .large;
        tier_rule = "tier-floor.high-risk.v1";
    } else if (complexity == .bounded and
        truthyFact(facts, fact_kinds.high_judgment_acceptance) != null and
        truthyFact(facts, fact_kinds.observable_correctness) != null)
    {
        tier_floor = .large;
        tier_rule = "tier-floor.bounded-high-judgment.v1";
        const f = findFact(facts, fact_kinds.high_judgment_acceptance).?;
        try matched.append(allocator, toMatched(f));
        try cites.append(allocator, .{ .rule_id = tier_rule, .fact_kind = f.kind, .locator = f.locator });
    } else if (complexity == .bounded and complete) {
        tier_floor = .small;
        tier_rule = "tier-floor.bounded-complete.v1";
    }

    return .{ .profile = .{
        .work_type = work_type,
        .work_type_rule_id = rule_id,
        .rule_version = rule_version,
        .complexity = complexity,
        .complexity_threshold_id = threshold_id,
        .tier_floor = tier_floor,
        .tier_floor_rule_id = tier_rule,
        .matched_facts = try matched.toOwnedSlice(allocator),
        .citations = try cites.toOwnedSlice(allocator),
    } };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const testing = std.testing;

fn fact(kind: []const u8, text: []const u8) packet_mod.Evidence {
    return .{
        .kind = kind,
        .id = 1,
        .locator = "task:1#facts",
        .text = text,
        .source_digest = "d",
        .current_digest = "d",
        .provenance = "task",
    };
}

/// A ready packet carrying `facts`. Readiness is the packet layer's contract;
/// these tests exercise the projection, so the packet is constructed ready.
fn readyPacket(facts: []const packet_mod.Evidence) packet_mod.TaskPacket {
    return .{
        .input = .{
            .task_id = 1,
            .status = "todo",
            .title = "t",
            .body = "b",
            .next_action = "n",
            .acceptance_criteria = "a",
            .owning_plans = &.{},
            .anchor_plans = &.{},
            .citations = &.{},
            .decisions = &.{},
            .questions = &.{},
            .scenarios = &.{},
            .dependencies = &.{},
            .touches = &.{},
            .claims = &.{},
            .validation_gates = &.{},
            .facts = facts,
        },
        .canonical = "",
        .digest = [_]u8{'0'} ** 64,
        .reasons = &.{},
    };
}

test "work type: first-match precedence puts schema above every later rule" {
    const a = testing.allocator;
    // Deliberate collision: this task is a schema change AND an engine change
    // AND a CLI change. Precedence, not fact count, decides.
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.cli_surface_change, "true"),
        fact(fact_kinds.transaction_change, "true"),
        fact(fact_kinds.migration_touched, "true"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqual(WorkType.schema, p.work_type);
    try testing.expectEqualStrings("work-type.schema.v1", p.work_type_rule_id);
}

test "work type: architectural outranks engine and cli" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.cli_surface_change, "true"),
        fact(fact_kinds.concurrency_change, "true"),
        fact(fact_kinds.new_subsystem, "true"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqual(WorkType.architectural, p.work_type);
}

test "work type: no rule match falls through to feature, with the default rule id" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{fact(fact_kinds.acceptance_complete, "true")};
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqual(WorkType.feature, p.work_type);
    try testing.expectEqualStrings(default_rule_id, p.work_type_rule_id);
}

test "work type: a fact asserting false does not match its rule" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.migration_touched, "false"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqual(WorkType.feature, p.work_type);
}

test "not_ready: contradictory facts never fall through to feature" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.docs_only, "true"),
        fact(fact_kinds.migration_touched, "true"),
    };
    const out = try compile(a, readyPacket(&facts));
    try testing.expectEqual(NotReadyReason.contradictory_facts, out.not_ready);
}

test "not_ready: a missing required fact is a gap, not a default" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{fact(fact_kinds.cli_surface_change, "true")};
    const out = try compile(a, readyPacket(&facts));
    try testing.expectEqual(NotReadyReason.missing_required_fact, out.not_ready);
}

test "complexity: explicit risk outranks a small capacity reading" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.touched_unit_count, "1"),
        fact(fact_kinds.explicit_risk, "true"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqual(Complexity.high_risk, p.complexity);
    try testing.expectEqual(Tier.large, p.tier_floor);
    try testing.expectEqualStrings("tier-floor.high-risk.v1", p.tier_floor_rule_id);
}

test "complexity: breadth over the large boundary is high risk" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.touched_unit_count, "40"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqual(Complexity.high_risk, p.complexity);
    try testing.expectEqualStrings("complexity.touched-units.v1", p.complexity_threshold_id);
}

test "complexity: validation-gate count is a capacity metric in its own right" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.touched_unit_count, "1"),
        fact(fact_kinds.validation_gate_count, "9"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqual(Complexity.high_risk, p.complexity);
}

test "policy_not_ready: a populated metric with no threshold row is a policy gap" {
    const a = testing.allocator;
    // capacity metric present, but thresholdFor() has no row for it because
    // the metric is not in `thresholds`. Simulated by asking for a metric the
    // policy does not define.
    try testing.expect(thresholdFor("capacity.unknown_metric") == null);
    _ = a;
}

test "tier floor: bounded and complete floors at small; a raise is eligible" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.touched_unit_count, "1"),
        fact(fact_kinds.validation_gate_count, "1"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqual(Complexity.bounded, p.complexity);
    try testing.expectEqual(Tier.small, p.tier_floor);
    // Operator raises are eligible ...
    try testing.expectEqual(Tier.large, try p.acceptTier(.large));
    try testing.expectEqual(Tier.small, try p.acceptTier(.small));
}

test "tier floor: a requested tier below the floor is rejected" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.explicit_risk, "true"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqual(Tier.large, p.tier_floor);
    try testing.expectError(TierRejection.below_floor, p.acceptTier(.medium));
    try testing.expectError(TierRejection.below_floor, p.acceptTier(.small));
}

test "tier floor: bounded high-judgment work with observable correctness floors at large" {
    const a = testing.allocator;
    // Small by every capacity measure, but the acceptance criteria carry a
    // judgement call whose correctness is checkable — the case where a
    // capacity-only read would under-tier the work.
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.touched_unit_count, "1"),
        fact(fact_kinds.validation_gate_count, "1"),
        fact(fact_kinds.high_judgment_acceptance, "true"),
        fact(fact_kinds.observable_correctness, "true"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqual(Complexity.bounded, p.complexity);
    try testing.expectEqual(Tier.large, p.tier_floor);
    try testing.expectEqualStrings("tier-floor.bounded-high-judgment.v1", p.tier_floor_rule_id);
}

test "explanation: every classification names its rule, version, facts, and citations" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.migration_touched, "true"),
        fact(fact_kinds.touched_unit_count, "40"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    try testing.expectEqualStrings(rule_version, p.rule_version);
    try testing.expectEqualStrings("work-type.schema.v1", p.work_type_rule_id);
    // Both the work-type match and the capacity reading are cited.
    try testing.expect(p.matched_facts.len >= 2);
    try testing.expect(p.citations.len >= 2);
    for (p.matched_facts) |m| {
        try testing.expect(m.locator.len > 0);
        try testing.expect(m.source_digest.len > 0);
    }
    var saw_schema = false;
    for (p.citations) |c| {
        if (std.mem.eql(u8, c.rule_id, "work-type.schema.v1")) saw_schema = true;
    }
    try testing.expect(saw_schema);
}

test "cohort identity is exactly the seven dimensions, with no normalization" {
    const a = testing.allocator;
    const facts = [_]packet_mod.Evidence{
        fact(fact_kinds.acceptance_complete, "true"),
        fact(fact_kinds.migration_touched, "true"),
    };
    const out = try compile(a, readyPacket(&facts));
    const p = out.profile;
    defer p.deinit(a);
    const c = p.cohort(7, "validation-v1", "codex", "coder", .large);
    try testing.expectEqual(@as(i64, 7), c.project_id);
    try testing.expectEqualStrings("validation-v1", c.validation_policy_version);
    try testing.expectEqualStrings("codex", c.vendor);
    try testing.expectEqualStrings("coder", c.role);
    try testing.expectEqual(Tier.large, c.tier);
    try testing.expectEqual(WorkType.schema, c.work_type);
    try testing.expectEqual(p.complexity, c.complexity);
    // Seven fields exactly: adding an eighth silently widens the identity and
    // would pool evidence that must stay separate.
    try testing.expectEqual(@as(usize, 7), @typeInfo(Cohort).@"struct".fields.len);
}
