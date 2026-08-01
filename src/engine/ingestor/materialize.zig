//! Stable, provenance-bearing task facts derived during spec ingest.
//!
//! The caller owns the transaction. Facts are staged as a complete set for
//! every task below an anchor plan, compared with the authoritative set, and
//! replaced only when their semantic contents differ.

const std = @import("std");
const db = @import("db");
const parse = @import("parse.zig");

pub const materializer_version = "spec-ingest-v1";

pub const Error = error{
    QueryFailed,
    InvalidCitation,
} || std.mem.Allocator.Error;

const Value = union(enum) {
    bool: bool,
    integer: i64,
    text: []const u8,
};

pub const RoadmapCitation = struct {
    task_id: i64,
    artifact_id: i64,
    source_locator: []const u8,
    source_text: []const u8,
};

pub fn reconcile(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
    roadmap_citations: []const RoadmapCitation,
) Error!void {
    try createStage(d);
    d.exec("delete from temp.routing_task_facts_stage") catch return Error.QueryFailed;

    try stageTaskFacts(d, allocator, anchor_plan_id);
    try stageRoadmapFacts(d, allocator, roadmap_citations);
    try stageArtifactFacts(d, allocator, anchor_plan_id, roadmap_citations);
    try stageDecisionFacts(d, allocator, anchor_plan_id);
    try stageQuestionFacts(d, allocator, anchor_plan_id);
    try stageScenarioFacts(d, allocator, anchor_plan_id);
    try stageLinkFacts(d, allocator, anchor_plan_id);

    if (try factSetsEqual(d, anchor_plan_id)) return;

    _ = d.execParams(
        \\delete from routing_task_facts
        \\where task_id in (
        \\  select t.id from tasks t
        \\  join plans p on p.id = t.plan_id
        \\  where p.parent_plan_id = ?
        \\)
    , &.{.{ .int = anchor_plan_id }}) catch return Error.QueryFailed;

    d.exec(
        \\insert into routing_task_facts (
        \\  task_id, fact_kind, value_type, value_bool, value_integer,
        \\  value_real, value_text, source_entity_kind, source_entity_id,
        \\  source_locator, source_digest, materializer_version
        \\)
        \\select distinct task_id, fact_kind, value_type, value_bool, value_integer,
        \\       null, value_text, source_entity_kind, source_entity_id,
        \\       source_locator, source_digest, materializer_version
        \\from temp.routing_task_facts_stage
        \\order by task_id, source_entity_kind, source_entity_id, source_locator,
        \\         fact_kind, value_type, coalesce(value_text, ''),
        \\         coalesce(value_integer, value_bool)
    ) catch return Error.QueryFailed;
}

fn stageRoadmapFacts(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    citations: []const RoadmapCitation,
) Error!void {
    for (citations) |citation| {
        try stage(
            d,
            allocator,
            citation.task_id,
            "cited_artifact_section",
            .{ .text = citation.source_text },
            "artifact",
            citation.artifact_id,
            citation.source_locator,
            citation.source_text,
        );
    }
}

fn createStage(d: *db.sqlite.Db) Error!void {
    d.exec(
        \\create temp table if not exists routing_task_facts_stage (
        \\  task_id integer not null,
        \\  fact_kind text not null,
        \\  value_type text not null,
        \\  value_bool integer,
        \\  value_integer integer,
        \\  value_text text,
        \\  source_entity_kind text not null,
        \\  source_entity_id integer not null,
        \\  source_locator text not null,
        \\  source_digest text not null,
        \\  materializer_version text not null
        \\)
    ) catch return Error.QueryFailed;
}

fn factSetsEqual(d: *db.sqlite.Db, anchor_plan_id: i64) Error!bool {
    var stmt = d.prepare(
        \\with current_facts as (
        \\  select task_id, fact_kind, value_type, value_bool, value_integer,
        \\         value_text, source_entity_kind, source_entity_id,
        \\         source_locator, source_digest, materializer_version
        \\  from routing_task_facts
        \\  where task_id in (
        \\    select t.id from tasks t
        \\    join plans p on p.id = t.plan_id
        \\    where p.parent_plan_id = ?
        \\  )
        \\),
        \\delta as (
        \\  select * from (
        \\    select * from current_facts
        \\    except select * from temp.routing_task_facts_stage
        \\  )
        \\  union all
        \\  select * from (
        \\    select * from temp.routing_task_facts_stage
        \\    except select * from current_facts
        \\  )
        \\)
        \\select count(*) from delta
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => false,
        .row => stmt.columnInt(0) == 0,
    };
}

fn stageTaskFacts(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) Error!void {
    var stmt = d.prepare(
        \\select t.id, coalesce(t.body, ''), coalesce(t.next_action, '')
        \\from tasks t join plans p on p.id = t.plan_id
        \\where p.parent_plan_id = ?
        \\order by t.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return Error.QueryFailed;
    while (true) switch (stmt.step() catch return Error.QueryFailed) {
        .done => break,
        .row => {
            const task_id = stmt.columnInt(0);
            const body = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(body);
            const next_action = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(next_action);

            const acceptance = section(body, "## Acceptance Criteria");
            const acceptance_ready = acceptance.len > 0 and
                !containsFold(acceptance, "is implemented and tested.");
            try stage(d, allocator, task_id, "acceptance_complete", .{ .bool = acceptance_ready }, "task", task_id, "body#acceptance-criteria", acceptance);
            if (acceptance.len > 0)
                try stage(d, allocator, task_id, "acceptance_text", .{ .text = acceptance }, "task", task_id, "body#acceptance-criteria", acceptance);

            const next_ready = next_action.len > 0 and
                !containsFold(next_action, "implement per acceptance criteria");
            try stage(d, allocator, task_id, "next_action_exact", .{ .bool = next_ready }, "task", task_id, "next_action", next_action);
            if (next_action.len > 0)
                try stage(d, allocator, task_id, "next_action", .{ .text = next_action }, "task", task_id, "next_action", next_action);

            try stageEvidenceFlags(d, allocator, task_id, "task", task_id, "body", body);
            if (body.len > 0)
                try stage(d, allocator, task_id, "context_bytes", .{ .integer = @intCast(body.len) }, "task", task_id, "body", body);
        },
    };
}

fn stageArtifactFacts(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
    roadmap_citations: []const RoadmapCitation,
) Error!void {
    var stmt = d.prepare(
        \\select t.id, coalesce(t.body, ''), a.id, a.kind, coalesce(a.body, '')
        \\from tasks t
        \\join plans p on p.id = t.plan_id
        \\join entity_links el on el.from_kind = 'task' and el.from_id = t.id
        \\  and el.to_kind = 'artifact' and el.relationship = 'cites'
        \\join artifacts a on a.id = el.to_id
        \\where p.parent_plan_id = ?
        \\order by t.id, a.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return Error.QueryFailed;
    while (true) switch (stmt.step() catch return Error.QueryFailed) {
        .done => break,
        .row => {
            const task_id = stmt.columnInt(0);
            const task_body = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(task_body);
            const artifact_id = stmt.columnInt(2);
            const kind = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(kind);
            const body = try stmt.columnTextAlloc(4, allocator);
            defer allocator.free(body);

            if (std.mem.eql(u8, kind, "roadmap")) {
                if (hasParsedRoadmapCitation(roadmap_citations, task_id, artifact_id)) continue;
                if (!try hasExplicitArtifactReference(allocator, task_body, artifact_id)) continue;
            }
            const locator = try explicitArtifactLocator(allocator, task_body, artifact_id);
            defer allocator.free(locator);
            const cited_source = artifactSection(body, locator) orelse return Error.InvalidCitation;
            try stage(d, allocator, task_id, "cited_artifact_section", .{ .text = cited_source }, "artifact", artifact_id, locator, cited_source);
        },
    };
}

fn hasParsedRoadmapCitation(
    citations: []const RoadmapCitation,
    task_id: i64,
    artifact_id: i64,
) bool {
    for (citations) |citation| {
        if (citation.task_id == task_id and citation.artifact_id == artifact_id) return true;
    }
    return false;
}

fn hasExplicitArtifactReference(
    allocator: std.mem.Allocator,
    task_body: []const u8,
    artifact_id: i64,
) std.mem.Allocator.Error!bool {
    const marker = try std.fmt.allocPrint(allocator, "artifact:{d}#", .{artifact_id});
    defer allocator.free(marker);
    return std.mem.indexOf(u8, task_body, marker) != null;
}

fn stageDecisionFacts(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) Error!void {
    var stmt = d.prepare(
        \\select t.id, de.id, de.body
        \\from tasks t
        \\join plans p on p.id = t.plan_id
        \\join entity_links el on el.from_kind = 'decision'
        \\  and el.to_kind = 'plan' and el.to_id = ?
        \\  and el.relationship = 'derives-from'
        \\join decisions de on de.id = el.from_id and de.status = 'accepted'
        \\where p.parent_plan_id = ?
        \\order by t.id, de.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = anchor_plan_id } }) catch return Error.QueryFailed;
    while (true) switch (stmt.step() catch return Error.QueryFailed) {
        .done => break,
        .row => {
            const task_id = stmt.columnInt(0);
            const decision_id = stmt.columnInt(1);
            const body = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(body);
            try stage(d, allocator, task_id, "locked_decision", .{ .text = body }, "decision", decision_id, "body", body);
            try stageEvidenceFlags(d, allocator, task_id, "decision", decision_id, "body", body);
        },
    };
}

fn stageQuestionFacts(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) Error!void {
    var stmt = d.prepare(
        \\select t.id, q.id, coalesce(q.body, '')
        \\from tasks t
        \\join plans p on p.id = t.plan_id
        \\join entity_links el on el.from_kind = 'question'
        \\  and el.to_kind = 'plan' and el.to_id = ?
        \\  and el.relationship = 'derives-from'
        \\join questions q on q.id = el.from_id and q.status = 'open'
        \\where p.parent_plan_id = ?
        \\order by t.id, q.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = anchor_plan_id } }) catch return Error.QueryFailed;
    while (true) switch (stmt.step() catch return Error.QueryFailed) {
        .done => break,
        .row => {
            const task_id = stmt.columnInt(0);
            const question_id = stmt.columnInt(1);
            const body = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(body);
            try stage(d, allocator, task_id, "unresolved_question", .{ .bool = true }, "question", question_id, "status", body);
        },
    };
}

fn stageScenarioFacts(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) Error!void {
    var stmt = d.prepare(
        \\select t.id, s.id, coalesce(s.body, ''), s.status
        \\from tasks t
        \\join plans p on p.id = t.plan_id
        \\join entity_links el on el.to_kind = 'task' and el.to_id = t.id
        \\  and el.from_kind = 'test_scenario' and el.relationship = 'verifies'
        \\join test_scenarios s on s.id = el.from_id
        \\where p.parent_plan_id = ?
        \\order by t.id, s.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return Error.QueryFailed;
    while (true) switch (stmt.step() catch return Error.QueryFailed) {
        .done => break,
        .row => {
            const task_id = stmt.columnInt(0);
            const scenario_id = stmt.columnInt(1);
            const body = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(body);
            const status = try stmt.columnTextAlloc(3, allocator);
            defer allocator.free(status);
            try stage(d, allocator, task_id, "scenario", .{ .text = body }, "test_scenario", scenario_id, "body", body);
            try stage(d, allocator, task_id, "scenario_coverage", .{ .text = status }, "test_scenario", scenario_id, "status", status);
            if (field(body, "**Acceptance:**")) |gate|
                try stage(d, allocator, task_id, "validation_gate", .{ .text = gate }, "test_scenario", scenario_id, "body#acceptance", gate);
            try stageEvidenceFlags(d, allocator, task_id, "test_scenario", scenario_id, "body", body);
        },
    };
}

fn stageLinkFacts(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_plan_id: i64) Error!void {
    var stmt = d.prepare(
        \\select t.id, el.relationship, el.from_kind, el.from_id,
        \\       el.to_kind, el.to_id
        \\from tasks t
        \\join plans p on p.id = t.plan_id
        \\join entity_links el on (
        \\  (el.from_kind = 'task' and el.from_id = t.id)
        \\  or (el.to_kind = 'task' and el.to_id = t.id)
        \\)
        \\where p.parent_plan_id = ?
        \\  and el.relationship in ('touches', 'blocks')
        \\order by t.id, el.from_kind, el.from_id, el.to_kind, el.to_id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return Error.QueryFailed;
    while (true) switch (stmt.step() catch return Error.QueryFailed) {
        .done => break,
        .row => {
            const task_id = stmt.columnInt(0);
            const relationship = try stmt.columnTextAlloc(1, allocator);
            defer allocator.free(relationship);
            const from_kind = try stmt.columnTextAlloc(2, allocator);
            defer allocator.free(from_kind);
            const from_id = stmt.columnInt(3);
            const to_kind = try stmt.columnTextAlloc(4, allocator);
            defer allocator.free(to_kind);
            const to_id = stmt.columnInt(5);
            const other_kind = if (std.mem.eql(u8, from_kind, "task") and from_id == task_id) to_kind else from_kind;
            const other_id = if (std.mem.eql(u8, from_kind, "task") and from_id == task_id) to_id else from_id;
            const locator = try std.fmt.allocPrint(allocator, "{s}:{d}", .{ other_kind, other_id });
            defer allocator.free(locator);
            const fact_kind = if (std.mem.eql(u8, relationship, "touches"))
                "touch"
            else if (std.mem.eql(u8, from_kind, "task") and from_id == task_id)
                "blocks"
            else
                "blocked_by";
            try stage(d, allocator, task_id, fact_kind, .{ .integer = other_id }, other_kind, other_id, locator, relationship);
        },
    };

    try stageCount(d, allocator, anchor_plan_id, "touch", "breadth", "links#touches");
    try stageCount(d, allocator, anchor_plan_id, "scenario", "validation_burden", "links#scenarios");
    try stageCount(d, allocator, anchor_plan_id, "blocks", "dependency_fanout", "links#blocks-outgoing");
}

fn stageCount(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    anchor_plan_id: i64,
    counted_kind: []const u8,
    fact_kind: []const u8,
    locator: []const u8,
) Error!void {
    var stmt = d.prepare(
        \\select t.id, count(s.fact_kind)
        \\from tasks t join plans p on p.id = t.plan_id
        \\left join temp.routing_task_facts_stage s
        \\  on s.task_id = t.id and s.fact_kind = ?
        \\where p.parent_plan_id = ?
        \\group by t.id order by t.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .text = counted_kind }, .{ .int = anchor_plan_id } }) catch return Error.QueryFailed;
    while (true) switch (stmt.step() catch return Error.QueryFailed) {
        .done => break,
        .row => {
            const task_id = stmt.columnInt(0);
            const count = stmt.columnInt(1);
            if (count > 0) {
                const semantic_count = try std.fmt.allocPrint(allocator, "{s}:{d}", .{ counted_kind, count });
                defer allocator.free(semantic_count);
                try stage(d, allocator, task_id, fact_kind, .{ .integer = count }, "task", task_id, locator, semantic_count);
            }
        },
    };
}

const EvidenceFlag = struct { needle: []const u8, fact_kind: []const u8 };
const evidence_flags = [_]EvidenceFlag{
    .{ .needle = "schema", .fact_kind = "risk_schema" },
    .{ .needle = "migration", .fact_kind = "risk_schema" },
    .{ .needle = "transaction", .fact_kind = "risk_transaction" },
    .{ .needle = "atomic", .fact_kind = "risk_transaction" },
    .{ .needle = "concurren", .fact_kind = "risk_concurrency" },
    .{ .needle = "ownership", .fact_kind = "risk_ownership" },
    .{ .needle = "security", .fact_kind = "risk_security" },
    .{ .needle = "state transition", .fact_kind = "risk_state_transition" },
    .{ .needle = "resource lifecycle", .fact_kind = "risk_resource_lifecycle" },
    .{ .needle = "architecture", .fact_kind = "risk_architecture" },
    .{ .needle = "mechanical", .fact_kind = "mechanicality_evidence" },
    .{ .needle = "enumerated", .fact_kind = "mechanicality_evidence" },
};

fn stageEvidenceFlags(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
    source_kind: []const u8,
    source_id: i64,
    locator: []const u8,
    text: []const u8,
) Error!void {
    for (evidence_flags) |flag| {
        if (containsFold(text, flag.needle))
            try stage(d, allocator, task_id, flag.fact_kind, .{ .bool = true }, source_kind, source_id, locator, text);
    }
}

fn stage(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_id: i64,
    fact_kind: []const u8,
    value: Value,
    source_kind: []const u8,
    source_id: i64,
    locator: []const u8,
    semantic_source: []const u8,
) Error!void {
    const digest = try sourceDigestAlloc(allocator, source_kind, source_id, locator, semantic_source);
    defer allocator.free(digest);
    const value_type = switch (value) {
        .bool => "bool",
        .integer => "integer",
        .text => "text",
    };
    const value_bool: db.sqlite.Param = switch (value) {
        .bool => |v| .{ .int = @intFromBool(v) },
        else => .null,
    };
    const value_integer: db.sqlite.Param = switch (value) {
        .integer => |v| .{ .int = v },
        else => .null,
    };
    const value_text: db.sqlite.Param = switch (value) {
        .text => |v| .{ .text = v },
        else => .null,
    };
    _ = d.execParams(
        \\insert into temp.routing_task_facts_stage (
        \\  task_id, fact_kind, value_type, value_bool, value_integer,
        \\  value_text, source_entity_kind, source_entity_id, source_locator,
        \\  source_digest, materializer_version
        \\) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    , &.{
        .{ .int = task_id },
        .{ .text = fact_kind },
        .{ .text = value_type },
        value_bool,
        value_integer,
        value_text,
        .{ .text = source_kind },
        .{ .int = source_id },
        .{ .text = locator },
        .{ .text = digest },
        .{ .text = materializer_version },
    }) catch return Error.QueryFailed;
}

pub fn sourceDigestAlloc(
    allocator: std.mem.Allocator,
    source_kind: []const u8,
    source_id: i64,
    locator: []const u8,
    semantic_source: []const u8,
) std.mem.Allocator.Error![]const u8 {
    const canonical = try std.fmt.allocPrint(
        allocator,
        "{s}\x00{d}\x00{s}\x00{s}",
        .{ source_kind, source_id, locator, semantic_source },
    );
    defer allocator.free(canonical);
    var digest: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(canonical, &digest, .{});
    return try std.fmt.allocPrint(allocator, "{x}", .{digest});
}

pub fn roadmapBulletForSlug(body: []const u8, slug: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, body, '\n');
    while (lines.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " \t\r");
        if (!std.mem.startsWith(u8, trimmed, "- ") and !std.mem.startsWith(u8, trimmed, "* ")) continue;
        const marker_start = std.mem.indexOf(u8, trimmed, "[slug:") orelse continue;
        const after_marker = trimmed[marker_start + "[slug:".len ..];
        const close = std.mem.indexOfScalar(u8, after_marker, ']') orelse continue;
        if (std.mem.eql(u8, std.mem.trim(u8, after_marker[0..close], " \t"), slug))
            return trimmed;
    }
    return null;
}

fn explicitArtifactLocator(
    allocator: std.mem.Allocator,
    task_body: []const u8,
    artifact_id: i64,
) Error![]const u8 {
    const marker = try std.fmt.allocPrint(allocator, "artifact:{d}#", .{artifact_id});
    defer allocator.free(marker);
    const start = std.mem.indexOf(u8, task_body, marker) orelse return Error.InvalidCitation;
    const tail = task_body[start..];
    var end: usize = 0;
    while (end < tail.len and tail[end] != '\n' and tail[end] != '\r' and
        tail[end] != ',' and tail[end] != ')' and tail[end] != ']') : (end += 1)
    {}
    const locator = std.mem.trim(u8, tail[0..end], " \t");
    if (locator.len <= marker.len) return Error.InvalidCitation;
    return try allocator.dupe(u8, locator);
}

/// Strip the synthetic wrapper `planar artifact show` puts around a stored
/// body, returning the authored document.
///
/// A stored artifact begins with an `## Content` heading followed by a YAML
/// frontmatter block. The workbench source parsed during ingestion has
/// neither, so parsing the stored body directly counts `## Content` as a
/// milestone and shifts every `milestone:N` locator by one — every roadmap
/// citation then resolves to the wrong item, or to nothing.
///
/// Only a wrapper at the very start is removed. A legitimately authored
/// section that happens to be called "Content" further down is left alone, and
/// an authored milestone with no work items keeps its index.
pub fn unwrapStoredArtifactBody(body: []const u8) []const u8 {
    const trimmed = std.mem.trimStart(u8, body, " \t\r\n");
    if (!std.mem.startsWith(u8, trimmed, "## Content")) return body;

    // Past the heading line.
    var rest = trimmed["## Content".len..];
    const after_heading = std.mem.indexOfScalar(u8, rest, '\n') orelse return body;
    rest = rest[after_heading + 1 ..];

    const lead = std.mem.trimStart(u8, rest, " \t\r\n");
    if (!std.mem.startsWith(u8, lead, "---")) {
        // `## Content` with no frontmatter is not the wrapper shape; leave it.
        return body;
    }
    // Skip the opening fence line, then find the closing one.
    var scan = lead["---".len..];
    const after_open = std.mem.indexOfScalar(u8, scan, '\n') orelse return body;
    scan = scan[after_open + 1 ..];

    var offset: usize = 0;
    while (offset < scan.len) {
        const line_end = std.mem.indexOfScalarPos(u8, scan, offset, '\n') orelse scan.len;
        const line = std.mem.trim(u8, scan[offset..line_end], " \t\r");
        if (std.mem.eql(u8, line, "---")) {
            const body_start = if (line_end < scan.len) line_end + 1 else scan.len;
            return scan[body_start..];
        }
        offset = if (line_end < scan.len) line_end + 1 else scan.len;
    }
    // Unterminated frontmatter: not the wrapper shape.
    return body;
}

/// Resolve a `roadmap#milestone:N/item:M` locator to its canonical folded
/// bullet text — the same string ingestion staged as `source_text`.
///
/// Returns null when the milestone or item does not exist, which is what makes
/// a citation to a deleted roadmap item go stale instead of silently staying
/// fresh. Indices are 1-based, matching the locators the differ emits.
pub fn roadmapSectionAlloc(
    allocator: std.mem.Allocator,
    body: []const u8,
    locator: []const u8,
) std.mem.Allocator.Error!?[]const u8 {
    const hash = std.mem.indexOfScalar(u8, locator, '#') orelse return null;
    const spec = locator[hash + 1 ..];
    if (!std.mem.startsWith(u8, spec, "milestone:")) return null;
    const rest = spec["milestone:".len..];
    const slash = std.mem.indexOfScalar(u8, rest, '/') orelse return null;
    const milestone_no = std.fmt.parseInt(usize, rest[0..slash], 10) catch return null;
    const item_part = rest[slash + 1 ..];
    if (!std.mem.startsWith(u8, item_part, "item:")) return null;
    const item_no = std.fmt.parseInt(usize, item_part["item:".len..], 10) catch return null;
    if (milestone_no == 0 or item_no == 0) return null;

    const authored = unwrapStoredArtifactBody(body);
    const milestones = try parse.parseRoadmap(allocator, authored);
    defer {
        for (milestones) |m| parse.deinitMilestone(m, allocator);
        allocator.free(milestones);
    }
    if (milestone_no > milestones.len) return null;
    const ms = milestones[milestone_no - 1];
    if (item_no > ms.work_items.len) return null;
    const wi = ms.work_items[item_no - 1];
    const text = if (wi.source_text.len > 0) wi.source_text else wi.title;
    return try allocator.dupe(u8, text);
}

pub fn artifactSection(body: []const u8, locator: []const u8) ?[]const u8 {
    const hash = std.mem.indexOfScalar(u8, locator, '#') orelse return null;
    const section_name = std.mem.trim(u8, locator[hash + 1 ..], " \t");
    if (section_name.len == 0) return null;

    var selected_level: ?usize = null;
    var section_start: usize = 0;
    var fence_char: ?u8 = null;
    var fence_len: usize = 0;
    var offset: usize = 0;
    while (offset <= body.len) {
        const line_end = std.mem.indexOfScalarPos(u8, body, offset, '\n') orelse body.len;
        const line = body[offset..line_end];
        const next_offset = if (line_end < body.len) line_end + 1 else body.len + 1;

        if (fenceDelimiter(line)) |fence| {
            if (fence_char == null) {
                fence_char = fence.char;
                fence_len = fence.len;
            } else if (fence.char == fence_char.? and fence.len >= fence_len and fence.is_closing) {
                fence_char = null;
                fence_len = 0;
            }
            offset = next_offset;
            continue;
        }

        if (fence_char == null and !isIndentedCode(line)) {
            if (atxHeading(line)) |heading| {
                if (selected_level) |level| {
                    if (heading.level <= level)
                        return std.mem.trim(u8, body[section_start..offset], " \t\r\n");
                } else if (std.ascii.eqlIgnoreCase(heading.title, section_name)) {
                    selected_level = heading.level;
                    section_start = @min(next_offset, body.len);
                }
            }
        }

        if (line_end == body.len) break;
        offset = next_offset;
    }
    return if (selected_level != null)
        std.mem.trim(u8, body[section_start..], " \t\r\n")
    else
        null;
}

const Heading = struct {
    level: usize,
    title: []const u8,
};

fn atxHeading(line: []const u8) ?Heading {
    const without_cr = std.mem.trimEnd(u8, line, "\r");
    var indent: usize = 0;
    while (indent < without_cr.len and without_cr[indent] == ' ' and indent < 4) : (indent += 1) {}
    if (indent > 3 or indent >= without_cr.len or without_cr[indent] != '#') return null;
    var level: usize = 0;
    while (indent + level < without_cr.len and without_cr[indent + level] == '#' and level < 7) : (level += 1) {}
    if (level == 0 or level > 6) return null;
    const after = indent + level;
    if (after < without_cr.len and without_cr[after] != ' ' and without_cr[after] != '\t') return null;
    var title = std.mem.trim(u8, without_cr[after..], " \t");
    while (title.len > 0 and title[title.len - 1] == '#') title = std.mem.trimEnd(u8, title[0 .. title.len - 1], " \t");
    return .{ .level = level, .title = title };
}

const Fence = struct {
    char: u8,
    len: usize,
    is_closing: bool,
};

fn fenceDelimiter(line: []const u8) ?Fence {
    const without_cr = std.mem.trimEnd(u8, line, "\r");
    var indent: usize = 0;
    while (indent < without_cr.len and without_cr[indent] == ' ' and indent < 4) : (indent += 1) {}
    if (indent > 3 or indent >= without_cr.len) return null;
    const char = without_cr[indent];
    if (char != '`' and char != '~') return null;
    var count: usize = 0;
    while (indent + count < without_cr.len and without_cr[indent + count] == char) : (count += 1) {}
    return if (count >= 3) .{
        .char = char,
        .len = count,
        .is_closing = std.mem.trim(u8, without_cr[indent + count ..], " \t").len == 0,
    } else null;
}

fn isIndentedCode(line: []const u8) bool {
    if (line.len == 0) return false;
    if (line[0] == '\t') return true;
    var spaces: usize = 0;
    while (spaces < line.len and line[spaces] == ' ') : (spaces += 1) {}
    return spaces >= 4;
}

fn containsFold(haystack: []const u8, needle: []const u8) bool {
    if (needle.len == 0 or needle.len > haystack.len) return false;
    var i: usize = 0;
    while (i + needle.len <= haystack.len) : (i += 1) {
        if (std.ascii.eqlIgnoreCase(haystack[i .. i + needle.len], needle)) return true;
    }
    return false;
}

pub fn section(body: []const u8, heading: []const u8) []const u8 {
    const start = std.mem.indexOf(u8, body, heading) orelse return "";
    const content_start = start + heading.len;
    const tail = body[content_start..];
    const end = std.mem.indexOf(u8, tail, "\n## ") orelse tail.len;
    return std.mem.trim(u8, tail[0..end], " \t\r\n");
}

pub fn field(body: []const u8, marker: []const u8) ?[]const u8 {
    const start = std.mem.indexOf(u8, body, marker) orelse return null;
    const tail = body[start + marker.len ..];
    const end = std.mem.indexOfScalar(u8, tail, '\n') orelse tail.len;
    const value = std.mem.trim(u8, tail[0..end], " \t\r");
    return if (value.len == 0) null else value;
}

test "generic ingest placeholders are explicitly unready" {
    try std.testing.expect(!containsFold("Concrete acceptance", "is implemented and tested."));
    try std.testing.expect(containsFold("Thing is implemented and tested.", "IS IMPLEMENTED AND TESTED."));
    try std.testing.expect(containsFold("Implement per acceptance criteria.", "implement per acceptance criteria"));
}

test "artifact sections honor nested headings and ignore code-block headings" {
    const body =
        \\```md
        \\## Target
        \\outside fenced decoy
        \\```
        \\    ## Target
        \\    outside indented decoy
        \\## Target
        \\TARGET_SENTINEL
        \\### Nested
        \\NESTED_SENTINEL
        \\```md
        \\## Fenced fake terminator
        \\FENCED_SENTINEL
        \\```
        \\    ## Indented fake terminator
        \\    INDENTED_SENTINEL
        \\TAIL_SENTINEL
        \\## Next
        \\NEXT_SENTINEL
    ;
    const section_body = artifactSection(body, "artifact:7#Target") orelse
        return error.TestUnexpectedResult;
    const expected =
        \\TARGET_SENTINEL
        \\### Nested
        \\NESTED_SENTINEL
        \\```md
        \\## Fenced fake terminator
        \\FENCED_SENTINEL
        \\```
        \\    ## Indented fake terminator
        \\    INDENTED_SENTINEL
        \\TAIL_SENTINEL
    ;
    try std.testing.expectEqualStrings(expected, section_body);
    try std.testing.expect(std.mem.indexOf(u8, section_body, "TARGET_SENTINEL") != null);
    try std.testing.expect(std.mem.indexOf(u8, section_body, "### Nested") != null);
    try std.testing.expect(std.mem.indexOf(u8, section_body, "NESTED_SENTINEL") != null);
    try std.testing.expect(std.mem.indexOf(u8, section_body, "FENCED_SENTINEL") != null);
    try std.testing.expect(std.mem.indexOf(u8, section_body, "INDENTED_SENTINEL") != null);
    try std.testing.expect(std.mem.indexOf(u8, section_body, "TAIL_SENTINEL") != null);
    try std.testing.expect(std.mem.indexOf(u8, section_body, "outside fenced decoy") == null);
    try std.testing.expect(std.mem.indexOf(u8, section_body, "outside indented decoy") == null);
    try std.testing.expect(std.mem.indexOf(u8, section_body, "NEXT_SENTINEL") == null);
}

test "materialization preserves lineage, replay identity, rollback, and model neutrality" {
    const allocator = std.testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, allocator);
    try conn.exec("insert into projects (slug, name) values ('facts', 'Facts')");
    try conn.exec(
        \\insert into plans (scope_kind, scope_id, title, slug, status)
        \\values ('repo', 1, 'Anchor', 'anchor', 'active');
        \\insert into plans (
        \\  scope_kind, scope_id, title, slug, parent_plan_id, status
        \\) values ('repo', 1, 'Milestone', 'milestone', 1, 'active');
        \\insert into tasks (
        \\  scope_kind, scope_id, plan_id, title, body, next_action, slug
        \\) values (
        \\  'repo', 1, 2, 'Sentinel task',
        \\  '## Acceptance Criteria
        \\
        \\- LINEAGE_SENTINEL is implemented and tested.',
        \\  'Implement per acceptance criteria.', 'sentinel-task'
        \\);
        \\insert into artifacts (scope_kind, scope_id, kind, title, body)
        \\values (
        \\  'repo', 1, 'roadmap', 'Display label',
        \\  '## Milestone
        \\
        \\- ARTIFACT_SENTINEL [slug: sentinel-task]'
        \\);
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('artifact', 1, 'plan', 1, 'derives-from');
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 1, 'artifact', 1, 'cites');
        \\insert into decisions (scope_kind, scope_id, title, body)
        \\values ('repo', 1, 'Display decision', 'DECISION_SENTINEL atomic transaction');
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('decision', 1, 'plan', 1, 'derives-from');
        \\insert into questions (scope_kind, scope_id, title, body)
        \\values ('repo', 1, 'Display question', 'QUESTION_SENTINEL');
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('question', 1, 'plan', 1, 'derives-from');
        \\insert into test_scenarios (scope_kind, scope_id, title, body)
        \\values (
        \\  'repo', 1, 'Display scenario',
        \\  '**Acceptance:** SCENARIO_SENTINEL exits zero'
        \\);
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('test_scenario', 1, 'task', 1, 'verifies');
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 1, 'task', 99, 'blocks');
        \\insert into entity_links (
        \\  from_kind, from_id, to_kind, to_id, relationship
        \\) values ('task', 98, 'task', 1, 'blocks')
    );

    const roadmap_citations = [_]RoadmapCitation{.{
        .task_id = 1,
        .artifact_id = 1,
        .source_locator = "roadmap#milestone:1/item:1",
        .source_text = "- ARTIFACT_SENTINEL [slug: sentinel-task]",
    }};
    try conn.savepoint(allocator, "facts_apply");
    try reconcile(&conn, allocator, 1, &roadmap_citations);
    try conn.releaseSavepoint(allocator, "facts_apply");

    try std.testing.expectEqual(@as(i64, 1), try conn.intQuery(
        \\select count(*) from routing_task_facts
        \\where fact_kind = 'acceptance_complete' and value_bool = 0
    ));
    try std.testing.expectEqual(@as(i64, 1), try conn.intQuery(
        \\select count(*) from routing_task_facts
        \\where fact_kind = 'next_action_exact' and value_bool = 0
    ));
    try std.testing.expectEqual(@as(i64, 1), try conn.intQuery(
        \\select count(*) from routing_task_facts
        \\where fact_kind = 'validation_gate'
        \\  and value_text = 'SCENARIO_SENTINEL exits zero'
        \\  and source_entity_kind = 'test_scenario'
        \\  and source_entity_id = 1
        \\  and length(source_digest) = 64
        \\  and materializer_version = 'spec-ingest-v1'
    ));
    try std.testing.expectEqual(@as(i64, 0), try conn.intQuery(
        "select count(*) from routing_task_facts where fact_kind = 'locked_decision'",
    ));
    try std.testing.expectEqual(@as(i64, 1), try conn.intQuery(
        "select count(*) from routing_task_facts where fact_kind = 'blocks'",
    ));
    try std.testing.expectEqual(@as(i64, 1), try conn.intQuery(
        "select count(*) from routing_task_facts where fact_kind = 'blocked_by'",
    ));
    try std.testing.expectEqual(@as(i64, 1), try conn.intQuery(
        \\select value_integer from routing_task_facts
        \\where fact_kind = 'dependency_fanout'
    ));
    try std.testing.expectEqual(@as(i64, 0), try conn.intQuery(
        \\select count(*) from routing_task_facts
        \\where fact_kind like '%model%' or fact_kind like '%tier%'
        \\   or fact_kind like '%provider%' or fact_kind like '%recommend%'
    ));

    const max_id_before = try conn.intQuery("select max(id) from routing_task_facts");
    const count_before = try conn.intQuery("select count(*) from routing_task_facts");
    try conn.savepoint(allocator, "facts_replay");
    try reconcile(&conn, allocator, 1, &roadmap_citations);
    try conn.releaseSavepoint(allocator, "facts_replay");
    try std.testing.expectEqual(max_id_before, try conn.intQuery("select max(id) from routing_task_facts"));
    try std.testing.expectEqual(count_before, try conn.intQuery("select count(*) from routing_task_facts"));

    try conn.exec("update decisions set status = 'accepted' where id = 1");
    try reconcile(&conn, allocator, 1, &roadmap_citations);
    try std.testing.expectEqual(@as(i64, 1), try conn.intQuery(
        \\select count(*) from routing_task_facts
        \\where fact_kind = 'locked_decision' and value_text = 'DECISION_SENTINEL atomic transaction'
    ));
}

// --- roadmap locator resolution ---------------------------------------------

const wrapped_roadmap =
    \\## Content
    \\
    \\---
    \\entity_kind: artifact
    \\entity_id: 517
    \\artifact_kind: roadmap
    \\---
    \\
    \\# Some Roadmap
    \\
    \\## M1 — First
    \\
    \\- Do the first thing [slug:first]
    \\- Do the second thing
    \\
    \\## M2 — Deliberately empty
    \\
    \\## M3 — Third
    \\
    \\- A folded item that continues
    \\  onto a second line [touches:repo-a]
    \\
;

test "roadmap locator: the synthetic Content wrapper does not shift indices" {
    const a = std.testing.allocator;
    // Parsed naively, `## Content` counts as milestone 1 and every locator is
    // off by one — which is why every live roadmap citation was stale.
    const first = (try roadmapSectionAlloc(a, wrapped_roadmap, "roadmap#milestone:1/item:1")).?;
    defer a.free(first);
    try std.testing.expect(std.mem.indexOf(u8, first, "first thing") != null);
}

test "roadmap locator: an authored empty milestone keeps its index" {
    const a = std.testing.allocator;
    // M2 has no work items, so item:1 must not exist — and M3 must still be 3.
    try std.testing.expect((try roadmapSectionAlloc(a, wrapped_roadmap, "roadmap#milestone:2/item:1")) == null);
    const third = (try roadmapSectionAlloc(a, wrapped_roadmap, "roadmap#milestone:3/item:1")).?;
    defer a.free(third);
    try std.testing.expect(std.mem.indexOf(u8, third, "folded item") != null);
}

test "roadmap locator: folded continuation lines are part of the item" {
    const a = std.testing.allocator;
    const folded = (try roadmapSectionAlloc(a, wrapped_roadmap, "roadmap#milestone:3/item:1")).?;
    defer a.free(folded);
    // The continuation must survive, otherwise the digest would differ from
    // the one ingestion staged and the citation would be stale immediately.
    try std.testing.expect(std.mem.indexOf(u8, folded, "second line") != null);
}

test "roadmap locator: a missing item resolves to null, not to a neighbour" {
    const a = std.testing.allocator;
    // Silently returning an adjacent item would keep a citation to a DELETED
    // roadmap entry looking fresh.
    try std.testing.expect((try roadmapSectionAlloc(a, wrapped_roadmap, "roadmap#milestone:1/item:9")) == null);
    try std.testing.expect((try roadmapSectionAlloc(a, wrapped_roadmap, "roadmap#milestone:9/item:1")) == null);
    try std.testing.expect((try roadmapSectionAlloc(a, wrapped_roadmap, "roadmap#milestone:0/item:1")) == null);
}

test "roadmap locator: a changed item changes the resolved text" {
    const a = std.testing.allocator;
    const before = (try roadmapSectionAlloc(a, wrapped_roadmap, "roadmap#milestone:1/item:1")).?;
    defer a.free(before);
    const edited = try std.mem.replaceOwned(u8, a, wrapped_roadmap, "Do the first thing", "Do something else");
    defer a.free(edited);
    const after = (try roadmapSectionAlloc(a, edited, "roadmap#milestone:1/item:1")).?;
    defer a.free(after);
    // Different text -> different digest -> the old fact goes stale, which is
    // the whole point of tracking freshness.
    try std.testing.expect(!std.mem.eql(u8, before, after));
}

test "roadmap locator: an unwrapped body resolves identically" {
    const a = std.testing.allocator;
    const unwrapped =
        \\# Some Roadmap
        \\
        \\## M1 — First
        \\
        \\- Do the first thing [slug:first]
        \\
    ;
    const got = (try roadmapSectionAlloc(a, unwrapped, "roadmap#milestone:1/item:1")).?;
    defer a.free(got);
    try std.testing.expect(std.mem.indexOf(u8, got, "first thing") != null);
}

test "unwrap: a section legitimately named Content further down is untouched" {
    const a = std.testing.allocator;
    const authored =
        \\# Roadmap
        \\
        \\## M1 — First
        \\
        \\- item one
        \\
        \\## Content
        \\
        \\- not frontmatter
        \\
    ;
    // Only a wrapper at the very start is synthetic. Stripping this would
    // delete authored material.
    try std.testing.expectEqualStrings(authored, unwrapStoredArtifactBody(authored));
    const got = (try roadmapSectionAlloc(a, authored, "roadmap#milestone:1/item:1")).?;
    defer a.free(got);
    try std.testing.expect(std.mem.indexOf(u8, got, "item one") != null);
}

test "unwrap: `## Content` without frontmatter is not the wrapper shape" {
    const authored = "## Content\n\n- just a list\n";
    try std.testing.expectEqualStrings(authored, unwrapStoredArtifactBody(authored));
}

test "roadmap locator: a non-roadmap locator is refused" {
    const a = std.testing.allocator;
    try std.testing.expect((try roadmapSectionAlloc(a, wrapped_roadmap, "artifact:517#Heading")) == null);
    try std.testing.expect((try roadmapSectionAlloc(a, wrapped_roadmap, "roadmap#Heading")) == null);
}
