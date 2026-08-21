//! engine/routing/roles.zig — every runtime role resolves from its
//! authoritative packet class, or says why it could not (plan 950 task 5530).
//!
//! Nine roles run in the delivery loop, and they do not all have the same
//! evidence available at the moment they are dispatched:
//!
//!   * **Pre-task roles** — planner, spec-reviewer, ingestor, orchestrator —
//!     run BEFORE a task exists. Their authority is a planning packet, which
//!     establishes readiness but carries no work type or complexity, because
//!     there is no unit of work yet to classify.
//!   * **Task-bound roles** — coder, test-coder, reviewer, research, janitor —
//!     run against a specific task. Their authority is that task's packet,
//!     compiled into a profile with a derived tier, work type, and complexity.
//!
//! When the authoritative packet is absent or not ready, resolution reports
//! the configured static fallback AND the reason — never a derived tier. That
//! distinction is the point: a tier invented from an unready packet looks
//! identical to one derived from a complete one, and an operator reading the
//! output has no way to tell that the routing decision rested on nothing.

const std = @import("std");
const store = @import("store.zig");
const packet_mod = @import("packet.zig");
const profile_mod = @import("profile.zig");

pub const resolution_version = "routing-roles-v1";

/// Every role the delivery loop dispatches.
pub const Role = enum {
    planner,
    spec_reviewer,
    ingestor,
    orchestrator,
    coder,
    test_coder,
    reviewer,
    research,
    janitor,

    pub fn text(self: Role) []const u8 {
        return @tagName(self);
    }
};

/// Which packet is authoritative for a role. This is a property of WHEN the
/// role runs, not of what it does.
pub const PacketClass = enum { planning, task };

pub fn packetClass(role: Role) PacketClass {
    return switch (role) {
        .planner, .spec_reviewer, .ingestor, .orchestrator => .planning,
        .coder, .test_coder, .reviewer, .research, .janitor => .task,
    };
}

/// Where a resolution's routing values came from. An operator must be able to
/// tell a derived decision from a fallback at a glance.
pub const Source = enum { packet, static_fallback };

/// Why the authoritative packet could not be used.
pub const FallbackReason = enum {
    /// No packet was supplied at all.
    no_packet,
    /// The packet exists but its readiness reasons are non-empty.
    packet_not_ready,
    /// The packet is ready, but policy could not produce a profile (e.g. a
    /// populated metric with no threshold covering it).
    policy_not_ready,

    pub fn text(self: FallbackReason) []const u8 {
        return @tagName(self);
    }
};

/// The configured static fallback for a role. Deliberately tier-only: a
/// fallback is a policy floor an operator chose in advance, not a derived
/// classification, so it carries no work type or complexity to masquerade as
/// packet-derived evidence.
pub const StaticFallback = struct {
    tier: store.Tier,
};

pub const Resolution = struct {
    role: Role,
    class: PacketClass,
    source: Source,

    /// The resolved routing tier. For a packet-backed task role this is the
    /// profile's derived FLOOR — an operator may raise it, never lower it, so
    /// the floor is the routing baseline rather than a fixed choice.
    tier: store.Tier,
    /// Present only when derived from a task packet. Null for every planning
    /// role and every fallback — planning packets classify no work, and a
    /// fallback classifies nothing at all.
    work_type: ?store.WorkType = null,
    complexity: ?store.Complexity = null,

    /// Set exactly when `source == .static_fallback`.
    fallback_reason: ?FallbackReason = null,
    /// The rule version that produced a packet-derived profile.
    rule_version: ?[]const u8 = null,

    pub fn packetBacked(self: Resolution) bool {
        return self.source == .packet;
    }
};

/// Resolve a task-bound role from its compiled profile outcome.
///
/// Asserts the role actually belongs to the task class: routing a planning
/// role through a task profile would attach a work type to a role that runs
/// before any work is classified.
pub fn resolveTask(
    role: Role,
    outcome: ?profile_mod.Outcome,
    fallback: StaticFallback,
) Resolution {
    std.debug.assert(packetClass(role) == .task);

    const out = outcome orelse return fallbackFor(role, .task, fallback, .no_packet);
    return switch (out) {
        .profile => |p| .{
            .role = role,
            .class = .task,
            .source = .packet,
            .tier = p.tier_floor,
            .work_type = p.work_type,
            .complexity = p.complexity,
            .rule_version = profile_mod.rule_version,
        },
        .not_ready => fallbackFor(role, .task, fallback, .packet_not_ready),
        .policy_not_ready => fallbackFor(role, .task, fallback, .policy_not_ready),
    };
}

/// Resolve a task-bound role starting from packet readiness.
///
/// `profile.compile` asserts a ready packet, so a caller holding an UNREADY
/// one cannot produce an `Outcome` at all. Without this the caller would have
/// to invent a profile-level reason (`missing_required_fact`) to describe a
/// packet-level problem, which would misattribute the failure — the packet is
/// incomplete, the profile rules are fine.
pub fn resolveTaskPacket(
    role: Role,
    packet_ready: bool,
    outcome: ?profile_mod.Outcome,
    fallback: StaticFallback,
) Resolution {
    std.debug.assert(packetClass(role) == .task);
    if (!packet_ready) return fallbackFor(role, .task, fallback, .packet_not_ready);
    return resolveTask(role, outcome, fallback);
}

/// Resolve a pre-task role from its planning packet.
///
/// A ready planning packet still yields no work type or complexity — there is
/// no task to classify — so a planning role's routing is its configured tier
/// either way. What changes is whether the decision is packet-backed: a ready
/// packet means the role was dispatched against established readiness, and an
/// absent or unready one means it was not, which is exactly what an operator
/// needs to see before approving the spawn.
pub fn resolvePlanning(
    role: Role,
    pkt: ?packet_mod.PlanningPacket,
    fallback: StaticFallback,
) Resolution {
    std.debug.assert(packetClass(role) == .planning);

    const p = pkt orelse return fallbackFor(role, .planning, fallback, .no_packet);
    if (!p.ready()) return fallbackFor(role, .planning, fallback, .packet_not_ready);
    return .{
        .role = role,
        .class = .planning,
        .source = .packet,
        .tier = fallback.tier,
        .rule_version = packet_mod.policy_version,
    };
}

fn fallbackFor(
    role: Role,
    class: PacketClass,
    fallback: StaticFallback,
    reason: FallbackReason,
) Resolution {
    return .{
        .role = role,
        .class = class,
        .source = .static_fallback,
        .tier = fallback.tier,
        .work_type = null,
        .complexity = null,
        .fallback_reason = reason,
    };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const testing = std.testing;

test "every role has exactly one authoritative packet class" {
    // Exhaustive: a role added without a class would fail to compile in the
    // switch, but this also pins WHICH side each role landed on, since that is
    // a semantic decision rather than a mechanical one.
    const planning = [_]Role{ .planner, .spec_reviewer, .ingestor, .orchestrator };
    const task = [_]Role{ .coder, .test_coder, .reviewer, .research, .janitor };
    for (planning) |r| try testing.expectEqual(PacketClass.planning, packetClass(r));
    for (task) |r| try testing.expectEqual(PacketClass.task, packetClass(r));
    try testing.expectEqual(
        @as(usize, planning.len + task.len),
        @typeInfo(Role).@"enum".fields.len,
    );
}

test "a task role derives tier, work type, and complexity from its profile" {
    const outcome: profile_mod.Outcome = .{ .profile = .{
        .work_type = .architectural,
        .work_type_rule_id = "work-type.architectural.v1",
        .rule_version = profile_mod.rule_version,
        .complexity = .high_risk,
        .complexity_threshold_id = "",
        .tier_floor = .large,
        .tier_floor_rule_id = "tier.large.v1",
        .matched_facts = &.{},
        .citations = &.{},
    } };
    const r = resolveTask(.coder, outcome, .{ .tier = .small });
    try testing.expect(r.packetBacked());
    try testing.expectEqual(store.Tier.large, r.tier);
    try testing.expectEqual(store.WorkType.architectural, r.work_type.?);
    try testing.expectEqual(store.Complexity.high_risk, r.complexity.?);
    try testing.expect(r.fallback_reason == null);
    // The configured fallback must NOT leak into a packet-backed resolution.
    try testing.expect(r.tier != .small);
}

test "an absent or unready task packet yields the static fallback and a reason" {
    // Absent packet.
    const none = resolveTask(.reviewer, null, .{ .tier = .medium });
    try testing.expect(!none.packetBacked());
    try testing.expectEqual(store.Tier.medium, none.tier);
    try testing.expectEqual(FallbackReason.no_packet, none.fallback_reason.?);

    // A fallback must never present a derived classification: showing a work
    // type here would be indistinguishable from a real derivation.
    try testing.expect(none.work_type == null);
    try testing.expect(none.complexity == null);

    const unready = resolveTask(
        .coder,
        .{ .not_ready = .missing_required_fact },
        .{ .tier = .medium },
    );
    try testing.expectEqual(FallbackReason.packet_not_ready, unready.fallback_reason.?);
    try testing.expect(unready.work_type == null);

    const gap = resolveTask(
        .janitor,
        .{ .policy_not_ready = .{ .metric = "churn", .threshold_id = null } },
        .{ .tier = .small },
    );
    try testing.expectEqual(FallbackReason.policy_not_ready, gap.fallback_reason.?);
    try testing.expectEqual(store.Tier.small, gap.tier);
}

test "a planning role is packet-backed when ready but still classifies no work" {
    const ready: packet_mod.PlanningPacket = .{
        .input = .{ .role = .planner, .goal = "g" },
        .canonical = "",
        .digest = [_]u8{'0'} ** 64,
        .reasons = &.{},
    };
    const r = resolvePlanning(.planner, ready, .{ .tier = .large });
    try testing.expect(r.packetBacked());
    try testing.expectEqual(PacketClass.planning, r.class);
    // No task exists yet, so there is nothing to classify — and inventing a
    // work type here would give a pre-task role fake task evidence.
    try testing.expect(r.work_type == null);
    try testing.expect(r.complexity == null);
}

test "an unready planning packet falls back with its reason" {
    const unready: packet_mod.PlanningPacket = .{
        .input = .{ .role = .ingestor, .goal = "" },
        .canonical = "",
        .digest = [_]u8{'0'} ** 64,
        .reasons = &.{.missing_goal},
    };
    const r = resolvePlanning(.ingestor, unready, .{ .tier = .medium });
    try testing.expect(!r.packetBacked());
    try testing.expectEqual(FallbackReason.packet_not_ready, r.fallback_reason.?);

    const absent = resolvePlanning(.spec_reviewer, null, .{ .tier = .medium });
    try testing.expectEqual(FallbackReason.no_packet, absent.fallback_reason.?);
}

test "an unready packet is attributed to the packet, not to the profile rules" {
    // The distinction matters for diagnosis: an incomplete packet is the
    // operator's to fix, a policy gap is the rule table's.
    const r = resolveTaskPacket(.coder, false, null, .{ .tier = .medium });
    try testing.expect(!r.packetBacked());
    try testing.expectEqual(FallbackReason.packet_not_ready, r.fallback_reason.?);
    try testing.expect(r.work_type == null);

    // A ready packet with no outcome is a genuinely absent profile.
    const absent = resolveTaskPacket(.coder, true, null, .{ .tier = .medium });
    try testing.expectEqual(FallbackReason.no_packet, absent.fallback_reason.?);
}

test "all nine roles resolve without a packet, each naming the fallback" {
    // The guarantee is that no role is silently unroutable: every one of them
    // produces an explainable answer even with no evidence at all.
    for (std.enums.values(Role)) |role| {
        const r = switch (packetClass(role)) {
            .task => resolveTask(role, null, .{ .tier = .medium }),
            .planning => resolvePlanning(role, null, .{ .tier = .medium }),
        };
        try testing.expectEqual(role, r.role);
        try testing.expect(!r.packetBacked());
        try testing.expectEqual(FallbackReason.no_packet, r.fallback_reason.?);
        try testing.expectEqual(store.Tier.medium, r.tier);
    }
}
