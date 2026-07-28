//! Canonical, fail-closed dispatch packet contracts.
//!
//! This module deliberately compiles facts, not routing choices. Callers read
//! current Planar rows and links into these typed inputs; the compiler orders
//! unordered evidence, checks source freshness and completeness, and hashes
//! only semantic fields. Runtime routing consumes the resulting digest.

const std = @import("std");
const db = @import("db");

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
    status: []const u8 = "ready",
    provenance: []const u8 = "planar",
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
    unresolved_citation,
    invalid_locked_decision,
    unresolved_question,
    invalid_dependency,
    invalid_touch,
    inactive_claim,
    invalid_validation_gate,
    missing_provenance,
    stale_mandatory_evidence,
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

pub const LiveTaskPacket = struct {
    arena: std.heap.ArenaAllocator,
    packet: TaskPacket,

    pub fn deinit(self: *LiveTaskPacket) void {
        self.packet.deinit(self.arena.allocator());
        self.arena.deinit();
    }
};

/// Reassembles the packet from the current database snapshot. No caller-owned
/// evidence fragments cross this boundary.
pub fn assembleTask(allocator: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64) !LiveTaskPacket {
    var arena = std.heap.ArenaAllocator.init(allocator);
    errdefer arena.deinit();
    const a = arena.allocator();
    var task = try d.prepare("select title,coalesce(body,''),coalesce(next_action,''),plan_id from tasks where id=?\x00");
    defer task.finalize();
    try task.bind(&.{.{ .int = task_id }});
    if (try task.step() != .row) return error.TaskNotFound;
    const title = try task.columnTextAlloc(0, a);
    const body = try task.columnTextAlloc(1, a);
    const next_action = try task.columnTextAlloc(2, a);
    const plan_id = task.columnIntOpt(3);
    const acceptance = extractSection(body, "## Acceptance criteria") orelse body;

    const owning = if (plan_id) |id| try oneRowEvidence(a, d, "select 'plan',id,'plan:'||id,title,'active','plan:'||id from plans where id=?\x00", id) else &.{};
    const anchors = if (plan_id) |id| try oneRowEvidence(a, d, "select 'plan',p.id,'plan:'||p.id,p.title,'active','plan:'||p.id from plans p where p.id=coalesce((select to_id from entity_links where from_kind='plan' and from_id=? and to_kind='plan' and relationship='derives-from' limit 1),?)\x00", id) else &.{};
    const citations = try linkedEvidence(a, d, task_id, "artifact", "cites", "select kind,id,coalesce(source_path,'artifact:'||id),coalesce(body,title),case when status in ('active','draft') then 'resolved' else status end,'artifact:'||id from artifacts where id=?\x00");
    const decisions = try linkedEvidence(a, d, task_id, "decision", "addresses", "select 'decision',id,'decision:'||id,body,case when status='accepted' then 'locked' else status end,'decision:'||id from decisions where id=?\x00");
    const questions = try linkedEvidence(a, d, task_id, "question", "addresses", "select 'question',id,'question:'||id,coalesce(answer_body,body,title),status,'question:'||id from questions where id=?\x00");
    const scenarios = try linkedEvidence(a, d, task_id, "test_scenario", "verifies", "select 'scenario',id,'scenario:'||id,coalesce(body,title),status,'scenario:'||id from test_scenarios where id=?\x00");
    const dependencies = try linkedEvidence(a, d, task_id, "task", "blocks", "select 'dependency',id,'task:'||id,title,case when status='done' then 'satisfied' else status end,'task:'||id from tasks where id=?\x00");
    const touches = try pathEvidence(a, d, task_id);
    const claims = try claimEvidence(a, d, task_id);
    const gates = try gateEvidence(a, task_id, body);
    const facts = try factEvidence(a, d, task_id);
    const input: TaskInput = .{ .task_id = task_id, .title = title, .body = body, .next_action = next_action, .acceptance_criteria = acceptance, .owning_plans = owning, .anchor_plans = anchors, .citations = citations, .decisions = decisions, .questions = questions, .scenarios = scenarios, .dependencies = dependencies, .touches = touches, .claims = claims, .validation_gates = gates, .facts = facts };
    return .{ .arena = arena, .packet = try compileTask(a, input) };
}

fn extractSection(body: []const u8, heading: []const u8) ?[]const u8 {
    const start = std.mem.indexOf(u8, body, heading) orelse return null;
    const rest = body[start + heading.len ..];
    const end = std.mem.indexOf(u8, rest, "\n## ") orelse rest.len;
    return trim(rest[0..end]);
}

fn rowEvidence(a: std.mem.Allocator, stmt: *db.sqlite.Stmt) !Evidence {
    const text = try stmt.columnTextAlloc(3, a);
    const current = digest(text);
    return .{ .kind = try stmt.columnTextAlloc(0, a), .id = stmt.columnInt(1), .locator = try stmt.columnTextAlloc(2, a), .text = text, .source_digest = try a.dupe(u8, &current), .current_digest = try a.dupe(u8, &current), .status = try stmt.columnTextAlloc(4, a), .provenance = try stmt.columnTextAlloc(5, a) };
}

fn oneRowEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, sql: [:0]const u8, id: i64) ![]const Evidence {
    var stmt = try d.prepare(sql);
    defer stmt.finalize();
    try stmt.bind(if (std.mem.count(u8, sql, "?") == 2) &.{ .{ .int = id }, .{ .int = id } } else &.{.{ .int = id }});
    if (try stmt.step() != .row) return &.{};
    const out = try a.alloc(Evidence, 1);
    out[0] = try rowEvidence(a, &stmt);
    return out;
}

fn linkedEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64, kind: []const u8, relationship: []const u8, entity_sql: [:0]const u8) ![]const Evidence {
    var ids = try d.prepare("select to_id from entity_links where from_kind='task' and from_id=? and to_kind=? and relationship=? order by to_id\x00");
    defer ids.finalize();
    try ids.bind(&.{ .{ .int = task_id }, .{ .text = kind }, .{ .text = relationship } });
    var out: std.ArrayList(Evidence) = .empty;
    while (try ids.step() == .row) {
        const rows = try oneRowEvidence(a, d, entity_sql, ids.columnInt(0));
        if (rows.len == 1) try out.append(a, rows[0]);
    }
    return out.toOwnedSlice(a);
}

fn pathEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64) ![]const Evidence {
    var stmt = try d.prepare("select 'touch',id,path,path,'resolved','task_touch_path:'||id from task_touch_paths where task_id=? order by id\x00");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    var out: std.ArrayList(Evidence) = .empty;
    while (try stmt.step() == .row) try out.append(a, try rowEvidence(a, &stmt));
    return out.toOwnedSlice(a);
}
fn claimEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64) ![]const Evidence {
    var stmt = try d.prepare("select 'claim',id,'claim:'||id,claim_token,case when status='active' and lease_expires_at>strftime('%Y-%m-%dT%H:%M:%fZ','now') then 'active' else status end,'agent_work_claim:'||id from agent_work_claims where entity_kind='task' and entity_id=? and status='active' order by id\x00");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    var out: std.ArrayList(Evidence) = .empty;
    while (try stmt.step() == .row) try out.append(a, try rowEvidence(a, &stmt));
    return out.toOwnedSlice(a);
}
fn gateEvidence(a: std.mem.Allocator, task_id: i64, body: []const u8) ![]const Evidence {
    const section = extractSection(body, "## Required validation") orelse return &.{};
    const current = digest(section);
    const out = try a.alloc(Evidence, 1);
    out[0] = .{ .kind = "validation_gate", .id = task_id, .locator = "task:required-validation", .text = section, .source_digest = try a.dupe(u8, &current), .current_digest = try a.dupe(u8, &current), .status = "required", .provenance = "task.body" };
    return out;
}
fn factEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64) ![]const Evidence {
    var stmt = try d.prepare("select fact_kind,id,source_locator,coalesce(value_text,cast(value_bool as text),cast(value_integer as text),cast(value_real as text),''),source_digest,source_entity_kind,source_entity_id from routing_task_facts where task_id=? order by id\x00");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    var out: std.ArrayList(Evidence) = .empty;
    while (try stmt.step() == .row) {
        const source_kind = try stmt.columnTextAlloc(5, a);
        const source_id = stmt.columnInt(6);
        const current_text = try sourceEntityText(a, d, source_kind, source_id);
        const current = digest(current_text);
        try out.append(a, .{ .kind = try stmt.columnTextAlloc(0, a), .id = stmt.columnInt(1), .locator = try stmt.columnTextAlloc(2, a), .text = try stmt.columnTextAlloc(3, a), .source_digest = try stmt.columnTextAlloc(4, a), .current_digest = try a.dupe(u8, &current), .status = "materialized", .provenance = try std.fmt.allocPrint(a, "{s}:{d}", .{ source_kind, source_id }) });
    }
    return out.toOwnedSlice(a);
}

fn sourceEntityText(a: std.mem.Allocator, d: *db.sqlite.Db, kind: []const u8, id: i64) ![]const u8 {
    const sql: [:0]const u8 = if (std.mem.eql(u8, kind, "task"))
        "select title||'\n'||coalesce(body,'')||'\n'||coalesce(next_action,'') from tasks where id=?\x00"
    else if (std.mem.eql(u8, kind, "artifact"))
        "select title||'\n'||coalesce(body,'')||'\n'||status from artifacts where id=?\x00"
    else if (std.mem.eql(u8, kind, "decision"))
        "select title||'\n'||body||'\n'||status from decisions where id=?\x00"
    else if (std.mem.eql(u8, kind, "question"))
        "select title||'\n'||coalesce(body,'')||'\n'||status||'\n'||coalesce(answer_body,'') from questions where id=?\x00"
    else if (std.mem.eql(u8, kind, "test_scenario"))
        "select title||'\n'||coalesce(body,'')||'\n'||status from test_scenarios where id=?\x00"
    else
        return "";
    var row = try d.prepare(sql);
    defer row.finalize();
    try row.bind(&.{.{ .int = id }});
    if (try row.step() != .row) return "";
    return row.columnTextAlloc(0, a);
}

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
    try validateEvidenceClasses(allocator, &reasons, input);

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
        try writer.print(",\"required\":{},\"covered\":{}", .{ item.required, item.covered });
        try writer.writeAll(",\"status\":");
        try json(item.status, writer);
        try writer.writeAll(",\"provenance\":");
        try json(item.provenance, writer);
        try writer.writeByte('}');
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
    const source_digest = std.mem.order(u8, a.source_digest, b.source_digest);
    if (source_digest != .eq) return source_digest == .lt;
    const current_digest = std.mem.order(u8, a.current_digest, b.current_digest);
    if (current_digest != .eq) return current_digest == .lt;
    if (a.required != b.required) return !a.required;
    if (a.covered != b.covered) return !a.covered;
    const status = std.mem.order(u8, a.status, b.status);
    if (status != .eq) return status == .lt;
    return std.mem.order(u8, a.provenance, b.provenance) == .lt;
}

fn validateEvidenceClasses(allocator: std.mem.Allocator, reasons: *std.ArrayList(ReadinessReason), input: TaskInput) !void {
    const classes = [_]struct { values: []const Evidence, reason: ReadinessReason, accepted: []const []const u8 }{
        .{ .values = input.citations, .reason = .unresolved_citation, .accepted = &.{ "resolved", "ready" } },
        .{ .values = input.decisions, .reason = .invalid_locked_decision, .accepted = &.{ "locked", "accepted", "ready" } },
        .{ .values = input.questions, .reason = .unresolved_question, .accepted = &.{ "answered", "non_blocking", "ready" } },
        .{ .values = input.dependencies, .reason = .invalid_dependency, .accepted = &.{ "satisfied", "ready", "done" } },
        .{ .values = input.touches, .reason = .invalid_touch, .accepted = &.{ "resolved", "ready" } },
        .{ .values = input.claims, .reason = .inactive_claim, .accepted = &.{"active"} },
        .{ .values = input.scenarios, .reason = .uncovered_required_scenario, .accepted = &.{ "ready", "verified" } },
        .{ .values = input.validation_gates, .reason = .invalid_validation_gate, .accepted = &.{ "required", "ready" } },
    };
    for (classes) |class| for (class.values) |item| {
        if (!item.required) continue;
        if (trim(item.provenance).len == 0) try appendReason(allocator, reasons, .missing_provenance);
        if (item.source_digest.len == 0 or item.current_digest.len == 0 or !std.mem.eql(u8, item.source_digest, item.current_digest))
            try appendReason(allocator, reasons, .stale_mandatory_evidence);
        var accepted = false;
        for (class.accepted) |status| if (std.mem.eql(u8, item.status, status)) {
            accepted = true;
            break;
        };
        if (!accepted) try appendReason(allocator, reasons, class.reason);
    };
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

test "canonical ordering is total for duplicate identity keys" {
    const allocator = std.testing.allocator;
    const low = Evidence{ .kind = "product_spec", .id = 1, .locator = "same", .text = "same", .source_digest = "x", .current_digest = "a", .required = false, .covered = false, .status = "resolved", .provenance = "artifact:1" };
    const high = Evidence{ .kind = "product_spec", .id = 1, .locator = "same", .text = "same", .source_digest = "x", .current_digest = "b", .required = true, .covered = true, .status = "ready", .provenance = "artifact:2" };
    const first = [_]Evidence{ low, high };
    const second = [_]Evidence{ high, low };
    var out1: std.Io.Writer.Allocating = .init(allocator);
    defer out1.deinit();
    var out2: std.Io.Writer.Allocating = .init(allocator);
    defer out2.deinit();
    try canonicalEvidenceField(allocator, &out1.writer, "e", &first);
    try canonicalEvidenceField(allocator, &out2.writer, "e", &second);
    try std.testing.expectEqualStrings(out1.written(), out2.written());
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
