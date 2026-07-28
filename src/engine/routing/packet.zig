//! Canonical, fail-closed dispatch packet contracts.
//!
//! This module deliberately compiles facts, not routing choices. Callers read
//! current Planar rows and links into these typed inputs; the compiler orders
//! unordered evidence, checks source freshness and completeness, and hashes
//! only semantic fields. Runtime routing consumes the resulting digest.

const std = @import("std");
const db = @import("db");
const materialize = @import("../ingestor/materialize.zig");
const test_spec_status = @import("../planning/test_spec_status.zig");

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
    materializer_version: []const u8 = "",
    current_materializer_version: []const u8 = "",
    freshness: []const u8 = "current",
};

pub const TaskInput = struct {
    task_id: i64,
    status: []const u8,
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
    invalid_task_status,
    generic_acceptance,
    generic_next_action,
    missing_owning_plan,
    missing_anchor_plan,
    invalid_owning_plan,
    invalid_anchor_plan,
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

pub fn renderText(packet: TaskPacket, writer: *std.Io.Writer) !void {
    try writer.print("task packet {d}: {s}\n", .{ packet.input.task_id, if (packet.ready()) "ready" else "not_ready" });
    try writer.print("digest: {s}\n", .{packet.digest});
    if (packet.reasons.len > 0) {
        try writer.writeAll("reasons:\n");
        for (packet.reasons) |reason| try writer.print("- {s}\n", .{@tagName(reason)});
    }
    try writer.writeAll(packet.canonical);
    try writer.writeByte('\n');
}

pub const LiveTaskPacket = struct {
    backing_allocator: std.mem.Allocator,
    arena: *std.heap.ArenaAllocator,
    packet: TaskPacket,

    pub fn deinit(self: *LiveTaskPacket) void {
        self.packet.deinit(self.arena.allocator());
        self.arena.deinit();
        self.backing_allocator.destroy(self.arena);
    }
};

/// Reassembles the packet from the current database snapshot. No caller-owned
/// evidence fragments cross this boundary.
pub fn assembleTask(allocator: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64) !LiveTaskPacket {
    const arena = try allocator.create(std.heap.ArenaAllocator);
    arena.* = std.heap.ArenaAllocator.init(allocator);
    errdefer {
        arena.deinit();
        allocator.destroy(arena);
    }
    const a = arena.allocator();
    var task = try d.prepare("select title,coalesce(body,''),coalesce(next_action,''),plan_id,status from tasks where id=?\x00");
    defer task.finalize();
    try task.bind(&.{.{ .int = task_id }});
    if (try task.step() != .row) return error.TaskNotFound;
    const title = try task.columnTextAlloc(0, a);
    const body = try task.columnTextAlloc(1, a);
    const next_action = try task.columnTextAlloc(2, a);
    const plan_id = task.columnIntOpt(3);
    const task_status = try task.columnTextAlloc(4, a);
    const acceptance = extractSectionFold(body, "## Acceptance criteria") orelse body;

    const owning = if (plan_id) |id| try oneRowEvidence(a, d, "select 'plan',id,'plan:'||id,title,status,'plan:'||id from plans where id=?\x00", id) else &.{};
    const anchors = if (plan_id) |id| try oneRowEvidence(a, d, "select 'plan',p.id,'plan:'||p.id,p.title,p.status,'plan:'||p.id from plans owner join plans p on p.id=coalesce(owner.parent_plan_id,(select el.to_id from entity_links el join plans parent on parent.id=el.to_id where el.from_kind='plan' and el.from_id=owner.id and el.to_kind='plan' and el.relationship='derives-from' and parent.scope_kind=owner.scope_kind and coalesce(parent.scope_id,0)=coalesce(owner.scope_id,0) limit 1),owner.id) where owner.id=?\x00", id) else &.{};
    const citations = try linkedEvidence(a, d, task_id, "artifact", "cites", "select kind,id,coalesce(source_path,'artifact:'||id),coalesce(body,title),status,'artifact:'||id from artifacts where id=?\x00");
    const decisions = try linkedEvidenceFromTask(a, d, task_id, "decision", "cites", "select 'decision',id,'decision:'||id,body,status,'decision:'||id from decisions where id=?\x00");
    const questions = try linkedEvidence(a, d, task_id, "question", "addresses", "select 'question',id,'question:'||id,coalesce(answer_body,body,title),status,'question:'||id from questions where id=?\x00");
    const anchor_plan_id = if (anchors.len == 1) anchors[0].id else if (plan_id) |id| id else 0;
    const scenarios = try scenarioEvidence(a, d, task_id, anchor_plan_id);
    const dependencies = try linkedEvidence(a, d, task_id, "task", "blocks", "select 'dependency',id,'task:'||id,title,case when status='done' then 'satisfied' else status end,'task:'||id from tasks where id=?\x00");
    const touches = try pathEvidence(a, d, task_id);
    const claims = try claimEvidence(a, d, task_id);
    const gates = try gateEvidence(a, task_id, body);
    const facts = try factEvidence(a, d, task_id);
    const input: TaskInput = .{ .task_id = task_id, .status = task_status, .title = title, .body = body, .next_action = next_action, .acceptance_criteria = acceptance, .owning_plans = owning, .anchor_plans = anchors, .citations = citations, .decisions = decisions, .questions = questions, .scenarios = scenarios, .dependencies = dependencies, .touches = touches, .claims = claims, .validation_gates = gates, .facts = facts };
    return .{ .backing_allocator = allocator, .arena = arena, .packet = try compileTask(a, input) };
}

fn extractSection(body: []const u8, heading: []const u8) ?[]const u8 {
    const start = std.mem.indexOf(u8, body, heading) orelse return null;
    const rest = body[start + heading.len ..];
    const end = std.mem.indexOf(u8, rest, "\n## ") orelse rest.len;
    return trim(rest[0..end]);
}

fn extractSectionFold(body: []const u8, heading: []const u8) ?[]const u8 {
    if (heading.len > body.len) return null;
    var start: usize = 0;
    while (start + heading.len <= body.len) : (start += 1) {
        if (!std.ascii.eqlIgnoreCase(body[start .. start + heading.len], heading)) continue;
        const rest = body[start + heading.len ..];
        const end = std.mem.indexOf(u8, rest, "\n## ") orelse rest.len;
        return trim(rest[0..end]);
    }
    return null;
}

fn rowEvidence(a: std.mem.Allocator, stmt: *db.sqlite.Stmt) !Evidence {
    const text = try stmt.columnTextAlloc(3, a);
    const current = digest(text);
    return .{ .kind = try stmt.columnTextAlloc(0, a), .id = stmt.columnInt(1), .locator = try stmt.columnTextAlloc(2, a), .text = text, .source_digest = try a.dupe(u8, &current), .current_digest = try a.dupe(u8, &current), .status = try stmt.columnTextAlloc(4, a), .provenance = try stmt.columnTextAlloc(5, a) };
}

fn oneRowEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, sql: [:0]const u8, id: i64) ![]const Evidence {
    var stmt = try d.prepare(sql);
    defer stmt.finalize();
    switch (std.mem.count(u8, sql, "?")) {
        1 => try stmt.bind(&.{.{ .int = id }}),
        2 => try stmt.bind(&.{ .{ .int = id }, .{ .int = id } }),
        3 => try stmt.bind(&.{ .{ .int = id }, .{ .int = id }, .{ .int = id } }),
        else => return error.InvalidEvidenceQuery,
    }
    if (try stmt.step() != .row) return &.{};
    const out = try a.alloc(Evidence, 1);
    out[0] = try rowEvidence(a, &stmt);
    return out;
}

fn linkedEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64, kind: []const u8, relationship: []const u8, entity_sql: [:0]const u8) ![]const Evidence {
    return linkedEvidenceFromTask(a, d, task_id, kind, relationship, entity_sql);
}

fn linkedEvidenceFromTask(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64, kind: []const u8, relationship: []const u8, entity_sql: [:0]const u8) ![]const Evidence {
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

fn linkedEvidenceToTask(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64, kind: []const u8, relationship: []const u8, entity_sql: [:0]const u8) ![]const Evidence {
    var ids = try d.prepare("select from_id from entity_links where to_kind='task' and to_id=? and from_kind=? and relationship=? order by from_id\x00");
    defer ids.finalize();
    try ids.bind(&.{ .{ .int = task_id }, .{ .text = kind }, .{ .text = relationship } });
    var out: std.ArrayList(Evidence) = .empty;
    while (try ids.step() == .row) {
        const rows = try oneRowEvidence(a, d, entity_sql, ids.columnInt(0));
        if (rows.len == 1) try out.append(a, rows[0]);
    }
    return out.toOwnedSlice(a);
}

/// Scenario coverage uses the same authoritative relationship directions as
/// `test-spec status`: the scenario must belong to the anchor and verify the
/// task. Lifecycle status is preserved independently from coverage.
fn scenarioEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64, anchor_plan_id: i64) ![]const Evidence {
    var stmt = try d.prepare(
        \\select 'scenario',s.id,'scenario:'||s.id,coalesce(s.body,s.title),
        \\       s.status,'scenario:'||s.id
        \\from test_scenarios s
        \\join entity_links owner on owner.from_kind='test_scenario'
        \\ and owner.from_id=s.id and owner.to_kind='plan' and owner.to_id=?
        \\ and owner.relationship='derives-from'
        \\join entity_links verifies on verifies.from_kind='test_scenario'
        \\ and verifies.from_id=s.id and verifies.to_kind='task'
        \\ and verifies.to_id=? and verifies.relationship='verifies'
        \\order by s.id
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = task_id } });
    var out: std.ArrayList(Evidence) = .empty;
    while (try stmt.step() == .row) {
        var evidence = try rowEvidence(a, &stmt);
        evidence.covered = true;
        try out.append(a, evidence);
    }
    return out.toOwnedSlice(a);
}

fn pathEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64) ![]const Evidence {
    var stmt = try d.prepare("select 'touch',id,path,path,'resolved','task_touch_path:'||id from task_touch_paths where task_id=? order by id\x00");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    var out: std.ArrayList(Evidence) = .empty;
    while (try stmt.step() == .row) try out.append(a, try rowEvidence(a, &stmt));
    var repos = try d.prepare(
        "select 'touch',p.id,'repo:'||p.id,p.slug,'resolved','project:'||p.id from entity_links el join projects p on p.id=el.to_id where el.from_kind='task' and el.from_id=? and el.to_kind='repo' and el.relationship='touches' order by p.id\x00",
    );
    defer repos.finalize();
    try repos.bind(&.{.{ .int = task_id }});
    while (try repos.step() == .row) try out.append(a, try rowEvidence(a, &repos));
    return out.toOwnedSlice(a);
}
fn claimEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64) ![]const Evidence {
    var stmt = try d.prepare("select 'claim',id,'claim:'||id,claim_token,case when status='active' and lease_expires_at>strftime('%Y-%m-%dT%H:%M:%fZ','now') then 'active' when status='active' then 'expired' else status end,'agent_work_claim:'||id from agent_work_claims where entity_kind='task' and entity_id=? and status='active' order by id\x00");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    var out: std.ArrayList(Evidence) = .empty;
    while (try stmt.step() == .row) try out.append(a, try rowEvidence(a, &stmt));
    return out.toOwnedSlice(a);
}
fn gateEvidence(a: std.mem.Allocator, task_id: i64, body: []const u8) ![]const Evidence {
    const section = extractSectionFold(body, "## Required validation") orelse return &.{};
    const current = digest(section);
    const out = try a.alloc(Evidence, 1);
    out[0] = .{ .kind = "validation_gate", .id = task_id, .locator = "task:required-validation", .text = section, .source_digest = try a.dupe(u8, &current), .current_digest = try a.dupe(u8, &current), .status = "required", .provenance = "task.body" };
    return out;
}
fn factEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, task_id: i64) ![]const Evidence {
    var stmt = try d.prepare("select fact_kind,id,source_locator,coalesce(value_text,cast(value_bool as text),cast(value_integer as text),cast(value_real as text),''),source_digest,source_entity_kind,source_entity_id,materializer_version from routing_task_facts where task_id=? order by id\x00");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = task_id }});
    var out: std.ArrayList(Evidence) = .empty;
    while (try stmt.step() == .row) {
        const source_kind = try stmt.columnTextAlloc(5, a);
        const source_id = stmt.columnInt(6);
        const fact_kind = try stmt.columnTextAlloc(0, a);
        const locator = try stmt.columnTextAlloc(2, a);
        const semantic_source = try factSemanticSource(a, d, task_id, fact_kind, source_kind, source_id, locator);
        const current = if (semantic_source) |semantic|
            try materialize.sourceDigestAlloc(a, source_kind, source_id, locator, semantic)
        else
            "";
        const source = try stmt.columnTextAlloc(4, a);
        const stored_version = try stmt.columnTextAlloc(7, a);
        const fresh = current.len > 0 and std.mem.eql(u8, source, current) and
            std.mem.eql(u8, stored_version, materialize.materializer_version);
        try out.append(a, .{
            .kind = fact_kind,
            .id = stmt.columnInt(1),
            .locator = locator,
            .text = try stmt.columnTextAlloc(3, a),
            .source_digest = source,
            .current_digest = current,
            .status = "materialized",
            .provenance = try std.fmt.allocPrint(a, "{s}:{d}", .{ source_kind, source_id }),
            .materializer_version = stored_version,
            .current_materializer_version = materialize.materializer_version,
            .freshness = if (fresh) "current" else "stale",
        });
    }
    return out.toOwnedSlice(a);
}

fn scalarText(a: std.mem.Allocator, d: *db.sqlite.Db, sql: [:0]const u8, id: i64) !?[]const u8 {
    var row = try d.prepare(sql);
    defer row.finalize();
    try row.bind(&.{.{ .int = id }});
    if (try row.step() != .row) return null;
    return try row.columnTextAlloc(0, a);
}

fn factSemanticSource(
    a: std.mem.Allocator,
    d: *db.sqlite.Db,
    task_id: i64,
    fact_kind: []const u8,
    source_kind: []const u8,
    source_id: i64,
    locator: []const u8,
) !?[]const u8 {
    if (std.mem.eql(u8, source_kind, "task") and source_id == task_id) {
        if (std.mem.eql(u8, locator, "body#acceptance-criteria")) {
            const body = (try scalarText(a, d, "select coalesce(body,'') from tasks where id=?\x00", task_id)) orelse return null;
            return materialize.section(body, "## Acceptance Criteria");
        }
        if (std.mem.eql(u8, locator, "next_action"))
            return scalarText(a, d, "select coalesce(next_action,'') from tasks where id=?\x00", task_id);
        if (std.mem.eql(u8, locator, "body"))
            return scalarText(a, d, "select coalesce(body,'') from tasks where id=?\x00", task_id);
        const count_kind: ?[]const u8 = if (std.mem.eql(u8, locator, "links#touches"))
            "touch"
        else if (std.mem.eql(u8, locator, "links#scenarios"))
            "scenario"
        else if (std.mem.eql(u8, locator, "links#blocks-outgoing"))
            "blocks"
        else
            null;
        if (count_kind) |kind| {
            const count = if (std.mem.eql(u8, kind, "touch"))
                try countQuery(d, "select count(*) from entity_links where from_kind='task' and from_id=? and relationship='touches'\x00", task_id)
            else if (std.mem.eql(u8, kind, "scenario"))
                try countQuery(d, "select count(*) from entity_links where to_kind='task' and to_id=? and from_kind='test_scenario' and relationship='verifies'\x00", task_id)
            else
                try countQuery(d, "select count(*) from entity_links where from_kind='task' and from_id=? and to_kind='task' and relationship='blocks'\x00", task_id);
            return try std.fmt.allocPrint(a, "{s}:{d}", .{ kind, count });
        }
    }
    if (std.mem.eql(u8, source_kind, "artifact")) {
        const body = (try scalarText(a, d, "select coalesce(body,'') from artifacts where id=?\x00", source_id)) orelse return null;
        return materialize.artifactSection(body, locator);
    }
    if (std.mem.eql(u8, source_kind, "decision"))
        return scalarText(a, d, "select body from decisions where id=?\x00", source_id);
    if (std.mem.eql(u8, source_kind, "question"))
        return scalarText(a, d, "select coalesce(body,'') from questions where id=?\x00", source_id);
    if (std.mem.eql(u8, source_kind, "test_scenario")) {
        if (std.mem.eql(u8, locator, "status"))
            return scalarText(a, d, "select status from test_scenarios where id=?\x00", source_id);
        const body = (try scalarText(a, d, "select coalesce(body,'') from test_scenarios where id=?\x00", source_id)) orelse return null;
        if (std.mem.eql(u8, locator, "body#acceptance"))
            return materialize.field(body, "**Acceptance:**") orelse "";
        return body;
    }
    if (std.mem.eql(u8, fact_kind, "touch"))
        return if (try liveRelationship(d, task_id, source_kind, source_id, "touches", .outgoing)) "touches" else null;
    if (std.mem.eql(u8, fact_kind, "blocks"))
        return if (try liveRelationship(d, task_id, source_kind, source_id, "blocks", .outgoing)) "blocks" else null;
    if (std.mem.eql(u8, fact_kind, "blocked_by"))
        return if (try liveRelationship(d, task_id, source_kind, source_id, "blocks", .incoming)) "blocks" else null;
    return null;
}

fn countQuery(d: *db.sqlite.Db, sql: [:0]const u8, id: i64) !i64 {
    var stmt = try d.prepare(sql);
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = id }});
    if (try stmt.step() != .row) return 0;
    return stmt.columnInt(0);
}

const RelationshipDirection = enum { outgoing, incoming };

fn liveRelationship(d: *db.sqlite.Db, task_id: i64, other_kind: []const u8, other_id: i64, relationship: []const u8, direction: RelationshipDirection) !bool {
    const sql: [:0]const u8 = switch (direction) {
        .outgoing => "select count(*) from entity_links where relationship=? and from_kind='task' and from_id=? and to_kind=? and to_id=?\x00",
        .incoming => "select count(*) from entity_links where relationship=? and to_kind='task' and to_id=? and from_kind=? and from_id=?\x00",
    };
    var stmt = try d.prepare(sql);
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .text = relationship }, .{ .int = task_id }, .{ .text = other_kind }, .{ .int = other_id } });
    return try stmt.step() == .row and stmt.columnInt(0) > 0;
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
    if (!statusIn(input.status, &.{ "todo", "doing" }))
        try appendReason(allocator, &reasons, .invalid_task_status);
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

    if (input.scenarios.len == 0)
        try appendReason(allocator, &reasons, .uncovered_required_scenario);
    for (input.scenarios) |scenario| {
        if (scenario.required and !scenario.covered) {
            try appendReason(allocator, &reasons, .uncovered_required_scenario);
            break;
        }
    }
    for (input.facts) |fact| {
        if (fact.source_digest.len == 0 or fact.current_digest.len == 0 or
            !std.mem.eql(u8, fact.source_digest, fact.current_digest) or
            (fact.materializer_version.len > 0 and
                !std.mem.eql(u8, fact.materializer_version, fact.current_materializer_version)) or
            !std.mem.eql(u8, fact.freshness, "current"))
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

pub const PlanningRole = enum { planner, spec_reviewer, ingestor, orchestrator };

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
    non_current_artifacts,
    missing_strict_preview,
    missing_review_rubric,
    missing_coverage,
    incomplete_coverage,
    missing_locked_decisions,
    invalid_locked_decisions,
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

pub const LivePlanningPacket = struct {
    backing_allocator: std.mem.Allocator,
    arena: *std.heap.ArenaAllocator,
    packet: PlanningPacket,

    pub fn deinit(self: *LivePlanningPacket) void {
        self.packet.deinit(self.arena.allocator());
        self.arena.deinit();
        self.backing_allocator.destroy(self.arena);
    }
};

/// Builds a pre-task packet exclusively from current Planar rows. The caller
/// supplies only the role and anchor plan identity; it cannot inject projected
/// facts or substitute task-shaped context before a task exists.
pub fn assemblePlanning(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    role: PlanningRole,
    anchor_plan_id: i64,
) !LivePlanningPacket {
    const arena = try allocator.create(std.heap.ArenaAllocator);
    arena.* = std.heap.ArenaAllocator.init(allocator);
    errdefer {
        arena.deinit();
        allocator.destroy(arena);
    }
    const a = arena.allocator();

    var plan = try d.prepare(
        "select title,coalesce(summary,''),scope_kind,coalesce(scope_id,0),status from plans where id=?\x00",
    );
    defer plan.finalize();
    try plan.bind(&.{.{ .int = anchor_plan_id }});
    if (try plan.step() != .row) return error.PlanNotFound;
    const title = try plan.columnTextAlloc(0, a);
    const summary = try plan.columnTextAlloc(1, a);
    const goal = if (trim(summary).len > 0) summary else title;
    const scope_kind = try plan.columnTextAlloc(2, a);
    const scope_id = plan.columnInt(3);
    const plan_status = try plan.columnTextAlloc(4, a);

    const scope_facts = try a.alloc(Evidence, 1);
    const scope_text = try std.fmt.allocPrint(a, "{s}:{d}", .{ scope_kind, scope_id });
    const scope_digest = digest(scope_text);
    scope_facts[0] = .{
        .kind = "scope",
        .id = scope_id,
        .locator = "plan:scope",
        .text = scope_text,
        .source_digest = try a.dupe(u8, &scope_digest),
        .current_digest = try a.dupe(u8, &scope_digest),
        .status = "ready",
        .provenance = try std.fmt.allocPrint(a, "plan:{d}", .{anchor_plan_id}),
    };
    const artifacts = try planLinkedEvidence(
        a,
        d,
        anchor_plan_id,
        "artifact",
        "select kind,id,coalesce(source_path,'artifact:'||id),coalesce(body,title),status,'artifact:'||id from artifacts where id=?\x00",
    );
    const questions = try planLinkedEvidence(
        a,
        d,
        anchor_plan_id,
        "question",
        "select 'question',id,'question:'||id,coalesce(answer_body,body,title),status,'question:'||id from questions where id=?\x00",
    );
    const decisions = try planLinkedEvidence(
        a,
        d,
        anchor_plan_id,
        "decision",
        "select 'decision',id,'decision:'||id,body,status,'decision:'||id from decisions where id=?\x00",
    );
    const required_outputs = try fixedOutputs(a);
    const strict_preview = try planningGraphEvidence(a, anchor_plan_id, artifacts, plan_status);
    const coverage = try planningCoverageEvidence(a, d, anchor_plan_id);
    const input: PlanningInput = .{
        .role = role,
        .goal = goal,
        .scope_facts = scope_facts,
        .artifacts = artifacts,
        .questions = questions,
        .constraints = decisions,
        .required_outputs = required_outputs,
        .strict_preview = strict_preview,
        .coverage = coverage,
        .decisions = decisions,
        .review_rubric_version = "spec-review-v1",
        .apply_boundary = if (hasFourCurrentArtifacts(artifacts)) "strict-preview-current" else "",
    };
    return .{ .backing_allocator = allocator, .arena = arena, .packet = try compilePlanning(a, input) };
}

fn planLinkedEvidence(
    a: std.mem.Allocator,
    d: *db.sqlite.Db,
    plan_id: i64,
    kind: []const u8,
    entity_sql: [:0]const u8,
) ![]const Evidence {
    var ids = try d.prepare(
        "select from_id from entity_links where from_kind=? and to_kind='plan' and to_id=? and relationship='derives-from' order by from_id\x00",
    );
    defer ids.finalize();
    try ids.bind(&.{ .{ .text = kind }, .{ .int = plan_id } });
    var out: std.ArrayList(Evidence) = .empty;
    while (try ids.step() == .row) {
        const rows = try oneRowEvidence(a, d, entity_sql, ids.columnInt(0));
        if (rows.len == 1) try out.append(a, rows[0]);
    }
    return out.toOwnedSlice(a);
}

fn fixedOutputs(a: std.mem.Allocator) ![]const Evidence {
    const names = [_][]const u8{ "product_spec", "tech_spec", "roadmap", "test_spec" };
    const out = try a.alloc(Evidence, names.len);
    for (names, 0..) |name, index| {
        const current = digest(name);
        out[index] = .{
            .kind = "required_output",
            .id = @intCast(index + 1),
            .locator = name,
            .text = name,
            .source_digest = try a.dupe(u8, &current),
            .current_digest = try a.dupe(u8, &current),
            .provenance = "routing-packet-policy",
        };
    }
    return out;
}

fn planningGraphEvidence(
    a: std.mem.Allocator,
    plan_id: i64,
    artifacts: []const Evidence,
    plan_status: []const u8,
) ![]const Evidence {
    if (!hasFourCurrentArtifacts(artifacts)) return &.{};
    var canonical: std.Io.Writer.Allocating = .init(a);
    defer canonical.deinit();
    try canonical.writer.print("plan:{d}\x00status:{s}", .{ plan_id, plan_status });
    try canonicalEvidenceField(a, &canonical.writer, "artifacts", artifacts);
    const text = try canonical.toOwnedSlice();
    const current = digest(text);
    const out = try a.alloc(Evidence, 1);
    out[0] = .{
        .kind = "strict_preview_graph",
        .id = plan_id,
        .locator = "plan:current-graph",
        .text = text,
        .source_digest = try a.dupe(u8, &current),
        .current_digest = try a.dupe(u8, &current),
        .provenance = try std.fmt.allocPrint(a, "plan:{d}", .{plan_id}),
    };
    return out;
}

fn planningCoverageEvidence(a: std.mem.Allocator, d: *db.sqlite.Db, plan_id: i64) ![]const Evidence {
    const oracle = try test_spec_status.compute(d, a, plan_id);
    defer test_spec_status.deinit(oracle, a);
    const total_tasks = oracle.summary.total_tasks;
    const tasks_covered = oracle.summary.tasks_covered;
    const total_scenarios = oracle.summary.total_scenarios;
    const complete = total_tasks > 0 and tasks_covered == total_tasks and total_scenarios > 0;
    const text = try std.fmt.allocPrint(a, "tasks:{d};covered:{d};scenarios:{d}", .{ total_tasks, tasks_covered, total_scenarios });
    const current = digest(text);
    const out = try a.alloc(Evidence, 1);
    out[0] = .{
        .kind = "coverage",
        .id = plan_id,
        .locator = "plan:test-scenario-coverage",
        .text = text,
        .source_digest = try a.dupe(u8, &current),
        .current_digest = try a.dupe(u8, &current),
        .covered = complete,
        .status = if (complete) "complete" else "incomplete",
        .provenance = try std.fmt.allocPrint(a, "plan:{d}", .{plan_id}),
    };
    return out;
}

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
    if (input.artifacts.len > 0 and !planningArtifactsCurrent(input.artifacts))
        try appendUnique(PlanningReason, allocator, &reasons, .non_current_artifacts);
    if (input.decisions.len > 0 and !planningDecisionsAccepted(input.decisions))
        try appendUnique(PlanningReason, allocator, &reasons, .invalid_locked_decisions);
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
            if (input.coverage.len > 0 and !planningCoverageComplete(input.coverage))
                try appendUnique(PlanningReason, allocator, &reasons, .incomplete_coverage);
            if (input.decisions.len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_locked_decisions);
            if (trim(input.apply_boundary).len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_apply_boundary);
        },
        .orchestrator => {
            if (input.scope_facts.len == 0) try appendUnique(PlanningReason, allocator, &reasons, .missing_scope_facts);
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

pub const LiveInvocationPacket = struct {
    live: LivePlanningPacket,
    resolution: InvocationResolution,

    pub fn deinit(self: *LiveInvocationPacket) void {
        self.live.deinit();
    }
};

pub fn assembleInvocation(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
    fallback_candidate: ?[]const u8,
) !LiveInvocationPacket {
    var live = try assemblePlanning(allocator, d, .orchestrator, anchor_plan_id);
    errdefer live.deinit();
    return .{
        .resolution = resolveInvocation(live.packet.digest, live.packet.ready(), fallback_candidate),
        .live = live,
    };
}

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
    status: []const u8,
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
    packet_digest: [64]u8,
};

pub const BriefError = error{PacketNotReady};

/// A lossless adapter over the authoritative packet. It borrows packet fields
/// and cannot accept independently reconstructed brief fragments.
pub fn coderBrief(packet: TaskPacket) BriefError!CoderBrief {
    if (!packet.ready()) return error.PacketNotReady;
    return .{
        .task_id = packet.input.task_id,
        .status = packet.input.status,
        .title = packet.input.title,
        .body = packet.input.body,
        .next_action = packet.input.next_action,
        .acceptance_criteria = packet.input.acceptance_criteria,
        .owning_plans = packet.input.owning_plans,
        .anchor_plans = packet.input.anchor_plans,
        .citations = packet.input.citations,
        .decisions = packet.input.decisions,
        .questions = packet.input.questions,
        .scenarios = packet.input.scenarios,
        .dependencies = packet.input.dependencies,
        .touches = packet.input.touches,
        .claims = packet.input.claims,
        .validation_gates = packet.input.validation_gates,
        .facts = packet.input.facts,
        .packet_digest = packet.digest,
    };
}

fn canonicalTask(allocator: std.mem.Allocator, input: TaskInput) ![]const u8 {
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();
    try out.writer.writeAll("{\"policy\":");
    try json(policy_version, &out.writer);
    try out.writer.print(",\"task_id\":{d},\"status\":", .{input.task_id});
    try json(input.status, &out.writer);
    try out.writer.writeAll(",\"title\":");
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
        try writer.writeAll(",\"materializer_version\":");
        try json(item.materializer_version, writer);
        try writer.writeAll(",\"current_materializer_version\":");
        try json(item.current_materializer_version, writer);
        try writer.writeAll(",\"freshness\":");
        try json(item.freshness, writer);
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
    const provenance = std.mem.order(u8, a.provenance, b.provenance);
    if (provenance != .eq) return provenance == .lt;
    const version = std.mem.order(u8, a.materializer_version, b.materializer_version);
    if (version != .eq) return version == .lt;
    const current_version = std.mem.order(u8, a.current_materializer_version, b.current_materializer_version);
    if (current_version != .eq) return current_version == .lt;
    return std.mem.order(u8, a.freshness, b.freshness) == .lt;
}

fn validateEvidenceClasses(allocator: std.mem.Allocator, reasons: *std.ArrayList(ReadinessReason), input: TaskInput) !void {
    const classes = [_]struct { values: []const Evidence, reason: ReadinessReason, accepted: []const []const u8 }{
        .{ .values = input.owning_plans, .reason = .invalid_owning_plan, .accepted = &.{ "active", "paused", "ready" } },
        .{ .values = input.anchor_plans, .reason = .invalid_anchor_plan, .accepted = &.{ "active", "paused", "ready" } },
        .{ .values = input.citations, .reason = .unresolved_citation, .accepted = &.{ "active", "draft", "ready" } },
        .{ .values = input.decisions, .reason = .invalid_locked_decision, .accepted = &.{ "accepted", "ready" } },
        .{ .values = input.questions, .reason = .unresolved_question, .accepted = &.{ "answered", "non_blocking", "ready" } },
        .{ .values = input.dependencies, .reason = .invalid_dependency, .accepted = &.{ "satisfied", "ready", "done" } },
        .{ .values = input.touches, .reason = .invalid_touch, .accepted = &.{ "resolved", "ready" } },
        .{ .values = input.claims, .reason = .inactive_claim, .accepted = &.{"active"} },
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
    // Scenario lifecycle is independent of coverage. A linked draft scenario
    // covers its task in the authoritative oracle; only the verifies/ownership
    // relationship (represented by `covered`) determines coverage readiness.
    for (input.scenarios) |item| {
        if (!item.required) continue;
        if (trim(item.provenance).len == 0) try appendReason(allocator, reasons, .missing_provenance);
        if (!evidenceCurrent(item)) try appendReason(allocator, reasons, .stale_mandatory_evidence);
        if (!item.covered) try appendReason(allocator, reasons, .uncovered_required_scenario);
    }
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
            if (std.mem.eql(u8, value.kind, kind) and evidenceCurrent(value) and
                statusIn(value.status, &.{ "draft", "active" }))
            {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

fn evidenceCurrent(value: Evidence) bool {
    return value.source_digest.len > 0 and value.current_digest.len > 0 and
        std.mem.eql(u8, value.source_digest, value.current_digest) and
        std.mem.eql(u8, value.freshness, "current") and trim(value.provenance).len > 0;
}

fn statusIn(status: []const u8, accepted: []const []const u8) bool {
    for (accepted) |value| if (std.mem.eql(u8, status, value)) return true;
    return false;
}

fn planningArtifactsCurrent(values: []const Evidence) bool {
    for (values) |value| if (value.required and
        (!evidenceCurrent(value) or !statusIn(value.status, &.{ "draft", "active" }))) return false;
    return true;
}

fn planningDecisionsAccepted(values: []const Evidence) bool {
    for (values) |value| if (value.required and
        (!evidenceCurrent(value) or !std.mem.eql(u8, value.status, "accepted"))) return false;
    return true;
}

fn planningCoverageComplete(values: []const Evidence) bool {
    for (values) |value| if (value.required and
        (!evidenceCurrent(value) or !value.covered or !std.mem.eql(u8, value.status, "complete"))) return false;
    return values.len > 0;
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
        .status = "doing",
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
        .{ .kind = "product_spec", .id = 1, .locator = "a", .text = "one", .source_digest = "1", .current_digest = "1" },
        .{ .kind = "tech_spec", .id = 2, .locator = "b", .text = "two", .source_digest = "2", .current_digest = "2" },
        .{ .kind = "roadmap", .id = 3, .locator = "c", .text = "three", .source_digest = "3", .current_digest = "3" },
        .{ .kind = "test_spec", .id = 4, .locator = "d", .text = "four", .source_digest = "4", .current_digest = "4" },
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
        .{ .kind = "product_spec", .id = 1, .locator = "p", .text = "", .source_digest = "1", .current_digest = "1", .status = "draft" },
        .{ .kind = "tech_spec", .id = 2, .locator = "t", .text = "", .source_digest = "2", .current_digest = "2", .status = "draft" },
        .{ .kind = "roadmap", .id = 3, .locator = "r", .text = "", .source_digest = "3", .current_digest = "3", .status = "draft" },
        .{ .kind = "test_spec", .id = 4, .locator = "s", .text = "", .source_digest = "4", .current_digest = "4", .status = "draft" },
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

test "planning packet readiness evaluates artifact decision and coverage lifecycle" {
    const allocator = std.testing.allocator;
    const artifacts = [_]Evidence{
        .{ .kind = "product_spec", .id = 1, .locator = "p", .text = "", .source_digest = "1", .current_digest = "1", .status = "draft" },
        .{ .kind = "tech_spec", .id = 2, .locator = "t", .text = "", .source_digest = "2", .current_digest = "2", .status = "active" },
        .{ .kind = "roadmap", .id = 3, .locator = "r", .text = "", .source_digest = "3", .current_digest = "3", .status = "draft" },
        .{ .kind = "test_spec", .id = 4, .locator = "s", .text = "", .source_digest = "4", .current_digest = "4", .status = "active" },
    };
    const preview = [_]Evidence{.{ .kind = "preview", .id = 1, .locator = "strict", .text = "ok" }};
    const coverage = [_]Evidence{.{ .kind = "coverage", .id = 1, .locator = "oracle", .text = "tasks:1;covered:1;scenarios:1", .source_digest = "c", .current_digest = "c", .covered = true, .status = "complete" }};
    const decisions = [_]Evidence{.{ .kind = "decision", .id = 1, .locator = "decision:1", .text = "locked", .source_digest = "d", .current_digest = "d", .status = "accepted" }};
    const input: PlanningInput = .{
        .role = .ingestor,
        .goal = "Apply reviewed artifacts.",
        .artifacts = &artifacts,
        .strict_preview = &preview,
        .coverage = &coverage,
        .decisions = &decisions,
        .apply_boundary = "strict-preview-current",
    };
    var ready = try compilePlanning(allocator, input);
    defer ready.deinit(allocator);
    try std.testing.expect(ready.ready());

    var no_scenarios = coverage;
    no_scenarios[0].text = "tasks:0;covered:0;scenarios:0";
    no_scenarios[0].covered = false;
    no_scenarios[0].status = "incomplete";
    var incomplete_input = input;
    incomplete_input.coverage = &no_scenarios;
    var incomplete = try compilePlanning(allocator, incomplete_input);
    defer incomplete.deinit(allocator);
    try std.testing.expect(std.mem.indexOfScalar(PlanningReason, incomplete.reasons, .incomplete_coverage) != null);

    var rejected = decisions;
    rejected[0].status = "withdrawn";
    var rejected_input = input;
    rejected_input.decisions = &rejected;
    var rejected_packet = try compilePlanning(allocator, rejected_input);
    defer rejected_packet.deinit(allocator);
    try std.testing.expect(std.mem.indexOfScalar(PlanningReason, rejected_packet.reasons, .invalid_locked_decisions) != null);

    var retired = artifacts;
    retired[0].status = "retired";
    var retired_input = input;
    retired_input.artifacts = &retired;
    var retired_packet = try compilePlanning(allocator, retired_input);
    defer retired_packet.deinit(allocator);
    try std.testing.expect(std.mem.indexOfScalar(PlanningReason, retired_packet.reasons, .non_current_artifacts) != null);
}

test "coder brief is lossless and fails closed" {
    const allocator = std.testing.allocator;
    const items = [_]Evidence{
        .{ .kind = "product_spec", .id = 1, .locator = "p", .text = "product", .source_digest = "1", .current_digest = "1" },
        .{ .kind = "tech_spec", .id = 2, .locator = "t", .text = "tech", .source_digest = "2", .current_digest = "2" },
        .{ .kind = "roadmap", .id = 3, .locator = "r", .text = "roadmap", .source_digest = "3", .current_digest = "3" },
        .{ .kind = "test_spec", .id = 4, .locator = "s", .text = "test", .source_digest = "4", .current_digest = "4" },
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
