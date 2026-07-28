//! Canonical, fail-closed dispatch packet contracts.
//!
//! This module deliberately compiles facts, not routing choices. Callers read
//! current Planar rows and links into these typed inputs; the compiler orders
//! unordered evidence, checks source freshness and completeness, and hashes
//! only semantic fields. Runtime routing consumes the resulting digest.

const std = @import("std");

pub const policy_version = "routing-packet-v1";

pub const Evidence = struct {
    kind: []const u8,
    id: i64,
    locator: []const u8,
    text: []const u8,
    source_digest: []const u8 = "",
    current_digest: []const u8 = "",
    required: bool = true,
    covered: bool = true,
};

pub const TaskInput = struct {
    task_id: i64,
    title: []const u8,
    body: []const u8,
    next_action: []const u8,
    acceptance_criteria: []const u8,
    owning_plans: []const Evidence,
    anchor_plans: []const Evidence,
    citations: []const Evidence,
    decisions: []const Evidence,
    questions: []const Evidence,
    scenarios: []const Evidence,
    dependencies: []const Evidence,
    touches: []const Evidence,
    claims: []const Evidence,
    validation_gates: []const Evidence,
    facts: []const Evidence,
};

pub const ReadinessReason = enum {
    missing_title,
    missing_body,
    generic_acceptance,
    generic_next_action,
    missing_owning_plan,
    missing_anchor_plan,
    missing_product_spec,
    missing_tech_spec,
    missing_roadmap,
    missing_test_spec,
    missing_locked_decision,
    missing_dependency,
    missing_touch,
    absent_validation_gates,
    uncovered_required_scenario,
    stale_fact,
    contradictory_mandatory_fact,
};

pub const TaskPacket = struct {
    input: TaskInput,
    canonical: []const u8,
    digest: [64]u8,
    reasons: []const ReadinessReason,

    pub fn ready(self: TaskPacket) bool {
        return self.reasons.len == 0;
    }

    pub fn deinit(self: TaskPacket, allocator: std.mem.Allocator) void {
        allocator.free(self.canonical);
        allocator.free(self.reasons);
    }
};

/// Compile a current task packet. No provider, model, work type, or tier is
/// selected here: those are dispatch-time projections over this packet.
pub fn compileTask(
    allocator: std.mem.Allocator,
    input: TaskInput,
) (std.mem.Allocator.Error || std.Io.Writer.Error)!TaskPacket {
    var reasons: std.ArrayList(ReadinessReason) = .empty;
    defer reasons.deinit(allocator);

    if (trim(input.title).len == 0) try appendReason(allocator, &reasons, .missing_title);
    if (trim(input.body).len == 0) try appendReason(allocator, &reasons, .missing_body);
    if (genericAcceptance(input.acceptance_criteria))
        try appendReason(allocator, &reasons, .generic_acceptance);
    if (genericNextAction(input.next_action))
        try appendReason(allocator, &reasons, .generic_next_action);
    if (input.owning_plans.len == 0) try appendReason(allocator, &reasons, .missing_owning_plan);
    if (input.anchor_plans.len == 0) try appendReason(allocator, &reasons, .missing_anchor_plan);
    if (!hasKind(input.citations, "product_spec")) try appendReason(allocator, &reasons, .missing_product_spec);
    if (!hasKind(input.citations, "tech_spec")) try appendReason(allocator, &reasons, .missing_tech_spec);
    if (!hasKind(input.citations, "roadmap")) try appendReason(allocator, &reasons, .missing_roadmap);
    if (!hasKind(input.citations, "test_spec")) try appendReason(allocator, &reasons, .missing_test_spec);
    if (input.decisions.len == 0) try appendReason(allocator, &reasons, .missing_locked_decision);
    if (input.dependencies.len == 0) try appendReason(allocator, &reasons, .missing_dependency);
    if (input.touches.len == 0) try appendReason(allocator, &reasons, .missing_touch);
    if (input.validation_gates.len == 0) try appendReason(allocator, &reasons, .absent_validation_gates);

    for (input.scenarios) |scenario| {
        if (scenario.required and !scenario.covered) {
            try appendReason(allocator, &reasons, .uncovered_required_scenario);
            break;
        }
    }
    for (input.facts) |fact| {
        if (fact.source_digest.len == 0 or fact.current_digest.len == 0 or
            !std.mem.eql(u8, fact.source_digest, fact.current_digest))
        {
            try appendReason(allocator, &reasons, .stale_fact);
            break;
        }
    }
    if (contradictory(input.facts))
        try appendReason(allocator, &reasons, .contradictory_mandatory_fact);

    const canonical = try canonicalTask(allocator, input);
    errdefer allocator.free(canonical);
    return .{
        .input = input,
        .canonical = canonical,
        .digest = digest(canonical),
        .reasons = try reasons.toOwnedSlice(allocator),
    };
}

pub const PlanningRole = enum { planner, spec_reviewer, ingestor };

pub const PlanningInput = struct {
    role: PlanningRole,
    goal: []const u8,
    scope_facts: []const Evidence = &.{},
    artifacts: []const Evidence = &.{},
    questions: []const Evidence = &.{},
    constraints: []const Evidence = &.{},
    required_outputs: []const Evidence = &.{},
    strict_preview: []const Evidence = &.{},
    coverage: []const Evidence = &.{},
    decisions: []const Evidence = &.{},
    review_rubric_version: []const u8 = "",
    apply_boundary: []const u8 = "",
};

pub const PlanningReason = enum {
    missing_goal,
    missing_scope_facts,
    missing_source_artifacts,
    missing_required_outputs,
    missing_artifact_digests,
    missing_strict_preview,
    missing_review_rubric,
    missing_coverage,
    missing_locked_decisions,
    missing_apply_boundary,
};

pub const PlanningPacket = struct {
    input: PlanningInput,
    canonical: []const u8,
    digest: [64]u8,
    reasons: []const PlanningReason,

    pub fn ready(self: PlanningPacket) bool {
        return self.reasons.len == 0;
    }

    pub fn deinit(self: PlanningPacket, allocator: std.mem.Allocator) void {
        allocator.free(self.canonical);
        allocator.free(self.reasons);
    }
};

/// Compile a role-specific pre-task packet. Task identity and task facts are
/// intentionally absent from this API, preventing callers from fabricating
/// implementation context before ingest creates a task.
pub fn compilePlanning(
    allocator: std.mem.Allocator,
    input: PlanningInput,
) (std.mem.Allocator.Error || std.Io.Writer.Error)!PlanningPacket {
    var reasons: std.ArrayList(PlanningReason) = .empty;
    defer reasons.deinit(allocator);
    if (trim(input.goal).len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_goal);
    switch (input.role) {
        .planner => {
            if (input.scope_facts.len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_scope_facts);
            if (input.artifacts.len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_source_artifacts);
            if (input.required_outputs.len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_required_outputs);
        },
        .spec_reviewer => {
            if (!hasFourCurrentArtifacts(input.artifacts)) try appendUnique(PlanningReason, allocator, &reasons, .missing_artifact_digests);
            if (input.strict_preview.len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_strict_preview);
            if (trim(input.review_rubric_version).len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_review_rubric);
        },
        .ingestor => {
            if (!hasFourCurrentArtifacts(input.artifacts)) try appendUnique(PlanningReason, allocator, &reasons, .missing_artifact_digests);
            if (input.strict_preview.len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_strict_preview);
            if (input.coverage.len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_coverage);
            if (input.decisions.len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_locked_decisions);
            if (trim(input.apply_boundary).len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_apply_boundary);
        },
    }
    const canonical = try canonicalPlanning(allocator, input);
    errdefer allocator.free(canonical);
    return .{
        .input = input,
        .canonical = canonical,
        .digest = digest(canonical),
        .reasons = try reasons.toOwnedSlice(allocator),
    };
}

pub const InvocationResolution = union(enum) {
    packet: struct { digest: [64]u8 },
    static_fallback: struct { candidate: []const u8, reason: []const u8 },
    unavailable: struct { reason: []const u8 },
};

/// Resolve the orchestration host before launch. A fallback is always visibly
/// labelled and never represented as evidence-backed packet output.
pub fn resolveInvocation(
    packet_digest: ?[64]u8,
    packet_ready: bool,
    fallback_candidate: ?[]const u8,
) InvocationResolution {
    if (packet_digest != null and packet_ready)
        return .{ .packet = .{ .digest = packet_digest.? } };
    const reason = if (packet_digest == null) "invocation_packet_absent" else "invocation_packet_not_ready";
    if (fallback_candidate) |candidate|
        return .{ .static_fallback = .{ .candidate = candidate, .reason = reason } };
    return .{ .unavailable = .{ .reason = reason } };
}

pub const CoderBrief = struct {
    task_id: i64,
    title: []const u8,
    body: []const u8,
    next_action: []const u8,
    acceptance_criteria: []const u8,
    citations: []const Evidence,
    decisions: []const Evidence,
    scenarios: []const Evidence,
    dependencies: []const Evidence,
    validation_gates: []const Evidence,
    packet_digest: [64]u8,
};

pub const BriefError = error{PacketNotReady};

/// A lossless adapter over the authoritative packet. It borrows packet fields
/// and cannot accept independently reconstructed brief fragments.
pub fn coderBrief(packet: TaskPacket) BriefError!CoderBrief {
    if (!packet.ready()) return error.PacketNotReady;
    return .{
        .task_id = packet.input.task_id,
        .title = packet.input.title,
        .body = packet.input.body,
        .next_action = packet.input.next_action,
        .acceptance_criteria = packet.input.acceptance_criteria,
        .citations = packet.input.citations,
        .decisions = packet.input.decisions,
        .scenarios = packet.input.scenarios,
        .dependencies = packet.input.dependencies,
        .validation_gates = packet.input.validation_gates,
        .packet_digest = packet.digest,
    };
}

fn canonicalTask(allocator: std.mem.Allocator, input: TaskInput) ![]const u8 {
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();
    try out.writer.writeAll("{\"policy\":");
    try json(policy_version, &out.writer);
    try out.writer.print(",\"task_id\":{d},\"title\":", .{input.task_id});
    try json(input.title, &out.writer);
    try out.writer.writeAll(",\"body\":");
    try json(input.body, &out.writer);
    try out.writer.writeAll(",\"next_action\":");
    try json(input.next_action, &out.writer);
    try out.writer.writeAll(",\"acceptance_criteria\":");
    try json(input.acceptance_criteria, &out.writer);
    try canonicalEvidenceField(allocator, &out.writer, "owning_plans", input.owning_plans);
    try canonicalEvidenceField(allocator, &out.writer, "anchor_plans", input.anchor_plans);
    try canonicalEvidenceField(allocator, &out.writer, "citations", input.citations);
    try canonicalEvidenceField(allocator, &out.writer, "decisions", input.decisions);
    try canonicalEvidenceField(allocator, &out.writer, "questions", input.questions);
    try canonicalEvidenceField(allocator, &out.writer, "scenarios", input.scenarios);
    try canonicalEvidenceField(allocator, &out.writer, "dependencies", input.dependencies);
    try canonicalEvidenceField(allocator, &out.writer, "touches", input.touches);
    try canonicalEvidenceField(allocator, &out.writer, "claims", input.claims);
    try canonicalEvidenceField(allocator, &out.writer, "validation_gates", input.validation_gates);
    try canonicalEvidenceField(allocator, &out.writer, "facts", input.facts);
    try out.writer.writeByte('}');
    return out.toOwnedSlice();
}

fn canonicalPlanning(allocator: std.mem.Allocator, input: PlanningInput) ![]const u8 {
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();
    try out.writer.writeAll("{\"policy\":");
    try json(policy_version, &out.writer);
    try out.writer.writeAll(",\"role\":");
    try json(@tagName(input.role), &out.writer);
    try out.writer.writeAll(",\"goal\":");
    try json(input.goal, &out.writer);
    try canonicalEvidenceField(allocator, &out.writer, "scope_facts", input.scope_facts);
    try canonicalEvidenceField(allocator, &out.writer, "artifacts", input.artifacts);
    try canonicalEvidenceField(allocator, &out.writer, "questions", input.questions);
    try canonicalEvidenceField(allocator, &out.writer, "constraints", input.constraints);
    try canonicalEvidenceField(allocator, &out.writer, "required_outputs", input.required_outputs);
    try canonicalEvidenceField(allocator, &out.writer, "strict_preview", input.strict_preview);
    try canonicalEvidenceField(allocator, &out.writer, "coverage", input.coverage);
    try canonicalEvidenceField(allocator, &out.writer, "decisions", input.decisions);
    try out.writer.writeAll(",\"review_rubric_version\":");
    try json(input.review_rubric_version, &out.writer);
    try out.writer.writeAll(",\"apply_boundary\":");
    try json(input.apply_boundary, &out.writer);
    try out.writer.writeByte('}');
    return out.toOwnedSlice();
}

fn canonicalEvidenceField(
    allocator: std.mem.Allocator,
    writer: *std.Io.Writer,
    name: []const u8,
    values: []const Evidence,
) !void {
    const sorted = try allocator.dupe(Evidence, values);
    defer allocator.free(sorted);
    std.mem.sort(Evidence, sorted, {}, lessEvidence);
    try writer.writeAll(",\"");
    try writer.writeAll(name);
    try writer.writeAll("\":[");
    for (sorted, 0..) |item, index| {
        if (index != 0) try writer.writeByte(',');
        try writer.writeAll("{\"kind\":");
        try json(item.kind, writer);
        try writer.print(",\"id\":{d},\"locator\":", .{item.id});
        try json(item.locator, writer);
        try writer.writeAll(",\"text\":");
        try json(item.text, writer);
        try writer.writeAll(",\"source_digest\":");
        try json(item.source_digest, writer);
        try writer.writeAll(",\"current_digest\":");
        try json(item.current_digest, writer);
        try writer.print(",\"required\":{},\"covered\":{}}}", .{ item.required, item.covered });
    }
    try writer.writeByte(']');
}

fn json(value: []const u8, writer: *std.Io.Writer) !void {
    try std.json.Stringify.encodeJsonString(value, .{}, writer);
}

fn lessEvidence(_: void, a: Evidence, b: Evidence) bool {
    const kind = std.mem.order(u8, a.kind, b.kind);
    if (kind != .eq) return kind == .lt;
    if (a.id != b.id) return a.id < b.id;
    const locator = std.mem.order(u8, a.locator, b.locator);
    if (locator != .eq) return locator == .lt;
    const text = std.mem.order(u8, a.text, b.text);
    if (text != .eq) return text == .lt;
    return std.mem.order(u8, a.source_digest, b.source_digest) == .lt;
}

fn digest(bytes: []const u8) [64]u8 {
    var raw: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &raw, .{});
    return std.fmt.bytesToHex(raw, .lower);
}

fn trim(value: []const u8) []const u8 {
    return std.mem.trim(u8, value, " \t\r\n");
}

fn genericAcceptance(value: []const u8) bool {
    const cleaned = trim(value);
    return cleaned.len == 0 or containsFold(cleaned, "is implemented and tested") or
        containsFold(cleaned, "works as expected") or containsFold(cleaned, "per spec");
}

fn genericNextAction(value: []const u8) bool {
    const cleaned = trim(value);
    return cleaned.len == 0 or containsFold(cleaned, "implement per acceptance criteria") or
        containsFold(cleaned, "implement the task") or std.ascii.eqlIgnoreCase(cleaned, "todo");
}

fn containsFold(haystack: []const u8, needle: []const u8) bool {
    if (needle.len > haystack.len) return false;
    var i: usize = 0;
    while (i + needle.len <= haystack.len) : (i += 1)
        if (std.ascii.eqlIgnoreCase(haystack[i .. i + needle.len], needle)) return true;
    return false;
}

fn hasKind(values: []const Evidence, kind: []const u8) bool {
    for (values) |value|
        if (std.mem.eql(u8, value.kind, kind) and trim(value.locator).len > 0) return true;
    return false;
}

fn hasFourCurrentArtifacts(values: []const Evidence) bool {
    const kinds = [_][]const u8{ "product_spec", "tech_spec", "roadmap", "test_spec" };
    for (kinds) |kind| {
        var found = false;
        for (values) |value| {
            if (std.mem.eql(u8, value.kind, kind) and value.source_digest.len > 0 and
                std.mem.eql(u8, value.source_digest, value.current_digest))
            {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

fn contradictory(values: []const Evidence) bool {
    for (values, 0..) |a, i| {
        if (!a.required) continue;
        for (values[i + 1 ..]) |b| {
            if (b.required and std.mem.eql(u8, a.kind, b.kind) and
                a.id == b.id and std.mem.eql(u8, a.locator, b.locator) and
                !std.mem.eql(u8, a.text, b.text)) return true;
        }
    }
    return false;
}

fn appendReason(
    allocator: std.mem.Allocator,
    values: *std.ArrayList(ReadinessReason),
    value: ReadinessReason,
) !void {
    try appendUnique(ReadinessReason, allocator, values, value);
}

fn appendUnique(comptime T: type, allocator: std.mem.Allocator, values: *std.ArrayList(T), value: T) !void {
    for (values.items) |existing| if (existing == value) return;
    try values.append(allocator, value);
}

fn fixtureInput(items: []const Evidence, facts: []const Evidence) TaskInput {
    return .{
        .task_id = 42,
        .title = "Exact \"title\"; $(still data)",
        .body = "Implement canonical packet behavior.",
        .next_action = "Compile current linked entities and verify all named gates.",
        .acceptance_criteria = "Equivalent evidence order hashes identically and stale evidence is rejected.",
        .owning_plans = items[0..1],
        .anchor_plans = items[0..1],
        .citations = items,
        .decisions = items[0..1],
        .questions = &.{},
        .scenarios = items[0..1],
        .dependencies = items[0..1],
        .touches = items[0..1],
        .claims = &.{},
        .validation_gates = items[0..1],
        .facts = facts,
    };
}

test "canonical task packet ignores input order and safely encodes adversarial text" {
    const allocator = std.testing.allocator;
    const a = Evidence{ .kind = "product_spec", .id = 2, .locator = "§A", .text = "\"; $(touch nope)", .source_digest = "a", .current_digest = "a" };
    const b = Evidence{ .kind = "tech_spec", .id = 3, .locator = "§B", .text = "tech", .source_digest = "b", .current_digest = "b" };
    const c = Evidence{ .kind = "roadmap", .id = 4, .locator = "§C", .text = "roadmap", .source_digest = "c", .current_digest = "c" };
    const d = Evidence{ .kind = "test_spec", .id = 5, .locator = "§D", .text = "tests", .source_digest = "d", .current_digest = "d" };
    const first = [_]Evidence{ a, b, c, d };
    const second = [_]Evidence{ d, c, b, a };
    const fact = Evidence{ .kind = "acceptance_complete", .id = 42, .locator = "body#ac", .text = "true", .source_digest = "fresh", .current_digest = "fresh" };
    var one = try compileTask(allocator, fixtureInput(&first, &.{fact}));
    defer one.deinit(allocator);
    var reordered = fixtureInput(&second, &.{fact});
    reordered.owning_plans = first[0..1];
    reordered.anchor_plans = first[0..1];
    reordered.decisions = first[0..1];
    reordered.scenarios = first[0..1];
    reordered.dependencies = first[0..1];
    reordered.touches = first[0..1];
    reordered.validation_gates = first[0..1];
    var two = try compileTask(allocator, reordered);
    defer two.deinit(allocator);
    try std.testing.expectEqualStrings(&one.digest, &two.digest);
    try std.testing.expect(std.mem.indexOf(u8, one.canonical, "\\\"; $(touch nope)") != null);
}

test "semantic changes alter digest and stale facts fail readiness" {
    const allocator = std.testing.allocator;
    const items = [_]Evidence{
        .{ .kind = "product_spec", .id = 1, .locator = "a", .text = "one" },
        .{ .kind = "tech_spec", .id = 2, .locator = "b", .text = "two" },
        .{ .kind = "roadmap", .id = 3, .locator = "c", .text = "three" },
        .{ .kind = "test_spec", .id = 4, .locator = "d", .text = "four" },
    };
    const stale = Evidence{ .kind = "acceptance_complete", .id = 42, .locator = "body", .text = "true", .source_digest = "old", .current_digest = "new" };
    var packet = try compileTask(allocator, fixtureInput(&items, &.{stale}));
    defer packet.deinit(allocator);
    try std.testing.expect(!packet.ready());
    try std.testing.expect(std.mem.indexOfScalar(ReadinessReason, packet.reasons, .stale_fact) != null);

    var changed_input = fixtureInput(&items, &.{stale});
    changed_input.title = "semantic change";
    var changed = try compileTask(allocator, changed_input);
    defer changed.deinit(allocator);
    try std.testing.expect(!std.mem.eql(u8, &packet.digest, &changed.digest));
}

test "planning packets enforce role-specific inputs without task facts" {
    const allocator = std.testing.allocator;
    const artifacts = [_]Evidence{
        .{ .kind = "product_spec", .id = 1, .locator = "p", .text = "", .source_digest = "1", .current_digest = "1" },
        .{ .kind = "tech_spec", .id = 2, .locator = "t", .text = "", .source_digest = "2", .current_digest = "2" },
        .{ .kind = "roadmap", .id = 3, .locator = "r", .text = "", .source_digest = "3", .current_digest = "3" },
        .{ .kind = "test_spec", .id = 4, .locator = "s", .text = "", .source_digest = "4", .current_digest = "4" },
    };
    const evidence = [_]Evidence{.{ .kind = "preview", .id = 1, .locator = "strict", .text = "ok" }};
    var reviewer = try compilePlanning(allocator, .{
        .role = .spec_reviewer,
        .goal = "Review the four planning artifacts.",
        .artifacts = &artifacts,
        .strict_preview = &evidence,
        .review_rubric_version = "review-v1",
    });
    defer reviewer.deinit(allocator);
    try std.testing.expect(reviewer.ready());

    var ingestor = try compilePlanning(allocator, .{
        .role = .ingestor,
        .goal = "Ingest reviewed artifacts.",
        .artifacts = &artifacts,
        .strict_preview = &evidence,
    });
    defer ingestor.deinit(allocator);
    try std.testing.expect(!ingestor.ready());
}

test "coder brief is lossless and fails closed" {
    const allocator = std.testing.allocator;
    const items = [_]Evidence{
        .{ .kind = "product_spec", .id = 1, .locator = "p", .text = "product" },
        .{ .kind = "tech_spec", .id = 2, .locator = "t", .text = "tech" },
        .{ .kind = "roadmap", .id = 3, .locator = "r", .text = "roadmap" },
        .{ .kind = "test_spec", .id = 4, .locator = "s", .text = "test" },
    };
    const fresh = Evidence{ .kind = "acceptance_complete", .id = 42, .locator = "body", .text = "true", .source_digest = "same", .current_digest = "same" };
    var packet = try compileTask(allocator, fixtureInput(&items, &.{fresh}));
    defer packet.deinit(allocator);
    const brief = try coderBrief(packet);
    try std.testing.expectEqualStrings(packet.input.title, brief.title);
    try std.testing.expectEqualStrings(packet.input.acceptance_criteria, brief.acceptance_criteria);
    try std.testing.expectEqualStrings(&packet.digest, &brief.packet_digest);

    var incomplete_input = fixtureInput(&items, &.{fresh});
    incomplete_input.validation_gates = &.{};
    var incomplete = try compileTask(allocator, incomplete_input);
    defer incomplete.deinit(allocator);
    try std.testing.expectError(error.PacketNotReady, coderBrief(incomplete));
}

test "orchestrator invocation visibly resolves fallback reason" {
    const resolution = resolveInvocation(null, false, "configured-static");
    switch (resolution) {
        .static_fallback => |fallback| {
            try std.testing.expectEqualStrings("configured-static", fallback.candidate);
            try std.testing.expectEqualStrings("invocation_packet_absent", fallback.reason);
        },
        else => return error.TestUnexpectedResult,
    }
}
