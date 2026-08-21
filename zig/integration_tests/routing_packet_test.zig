//! Integration contract from an authoritative packet through coder brief.

const std = @import("std");
const packet = @import("engine").routing.packet;
const db = @import("db");
const harness = @import("harness");

test "packet assembler reads current isolated Planar database state" {
    const allocator = std.testing.allocator;
    var suite = harness.Suite.init(allocator);
    defer suite.deinit();
    _ = suite.registerProject("routing-live");
    suite.addAssoc("routing-live", null);
    const path = try allocator.dupeZ(u8, suite.absDbPath());
    defer allocator.free(path);
    var live_db = try db.sqlite.Db.open(path);
    defer live_db.close();
    try live_db.execSlice(allocator,
        \\insert into plans(id,scope_kind,scope_id,title,slug,status) values(9000,'association',1,'Anchor','anchor','active'),(9001,'association',1,'Owner','owner','active');
        \\insert into tasks(id,scope_kind,scope_id,plan_id,title,body,status,next_action) values
        \\(9002,'association',1,9001,'Live packet task','## Acceptance Criteria
        \\Exact current rows reach the brief.
        \\Citations: artifact:9010#Requirements, artifact:9011#Dispatch-time packet compiler,
        \\artifact:9012#Cross-stack contract, artifact:9013#Coder brief
        \\## Required validation
        \\make test-integration','doing','Compile live rows and verify the production brief.'),
        \\(9003,'association',1,9001,'Dependency','','done','Already complete.');
        \\insert into artifacts(id,scope_kind,scope_id,kind,title,body,source_path,status) values
        \\(9010,'association',1,'product_spec','P','## Requirements
        \\p','p.md','active'),(9011,'association',1,'tech_spec','T','## Dispatch-time packet compiler
        \\t','t.md','active'),(9012,'association',1,'roadmap','R','## Cross-stack contract
        \\r','r.md','active'),(9013,'association',1,'test_spec','S','## Coder brief
        \\s','s.md','active');
        \\insert into decisions(id,scope_kind,scope_id,title,body,status) values(9020,'association',1,'Locked','locked text','accepted');
        \\insert into test_scenarios(id,scope_kind,scope_id,title,body,status) values(2142,'association',1,'Coder brief equals authoritative packet','SCENARIO_2142_EXACT_CONTEXT','draft');
        \\insert into entity_links(from_kind,from_id,to_kind,to_id,relationship) values
        \\('plan',9001,'plan',9000,'derives-from'),('task',9002,'artifact',9010,'cites'),('task',9002,'artifact',9011,'cites'),
        \\('task',9002,'artifact',9012,'cites'),('task',9002,'artifact',9013,'cites'),('task',9002,'decision',9020,'cites'),
        \\('test_scenario',2142,'task',9002,'verifies'),('task',9002,'task',9003,'depends-on'),
        \\('artifact',9010,'plan',9000,'derives-from'),('artifact',9011,'plan',9000,'derives-from'),
        \\('artifact',9012,'plan',9000,'derives-from'),('artifact',9013,'plan',9000,'derives-from'),
        \\('decision',9020,'plan',9000,'derives-from'),('test_scenario',2142,'plan',9000,'derives-from'),
        \\('test_scenario',2142,'task',9003,'verifies');
        \\insert into task_touch_paths(task_id,repo_id,path) select 9002,id,'src/live.zig' from projects order by id limit 1;
        \\insert into sessions(id,vendor) values(9040,'test');
        \\insert into agent_work_claims(claim_token,session_id,entity_kind,entity_id,status,vendor,lease_expires_at) values('liveclaim',9040,'task',9002,'active','test','2999-01-01T00:00:00.000Z');
    );
    const acceptance_digest = try sourceDigestAlloc(
        allocator,
        "task",
        9002,
        "body#acceptance-criteria",
        "Exact current rows reach the brief.\nCitations: artifact:9010#Requirements, artifact:9011#Dispatch-time packet compiler,\nartifact:9012#Cross-stack contract, artifact:9013#Coder brief",
    );
    defer allocator.free(acceptance_digest);
    const next_exact_digest = try sourceDigestAlloc(
        allocator,
        "task",
        9002,
        "next_action",
        "Compile live rows and verify the production brief.",
    );
    defer allocator.free(next_exact_digest);
    const product_digest = try sourceDigestAlloc(allocator, "artifact", 9010, "artifact:9010#Requirements", "p");
    defer allocator.free(product_digest);
    const tech_digest = try sourceDigestAlloc(allocator, "artifact", 9011, "artifact:9011#Dispatch-time packet compiler", "t");
    defer allocator.free(tech_digest);
    const roadmap_digest = try sourceDigestAlloc(allocator, "artifact", 9012, "artifact:9012#Cross-stack contract", "r");
    defer allocator.free(roadmap_digest);
    const test_digest = try sourceDigestAlloc(allocator, "artifact", 9013, "artifact:9013#Coder brief", "s");
    defer allocator.free(test_digest);
    _ = try live_db.execParams(
        \\insert into routing_task_facts(
        \\ task_id,fact_kind,value_type,value_bool,value_text,source_entity_kind,
        \\ source_entity_id,source_locator,source_digest,materializer_version
        \\) values
        \\ (9002,'acceptance_complete','bool',1,null,'task',9002,'body#acceptance-criteria',?,'spec-ingest-v1'),
        \\ (9002,'next_action_exact','bool',1,null,'task',9002,'next_action',?,'spec-ingest-v1'),
        \\ (9002,'cited_artifact_section','text',null,'p','artifact',9010,'artifact:9010#Requirements',?,'spec-ingest-v1'),
        \\ (9002,'cited_artifact_section','text',null,'t','artifact',9011,'artifact:9011#Dispatch-time packet compiler',?,'spec-ingest-v1'),
        \\ (9002,'cited_artifact_section','text',null,'r','artifact',9012,'artifact:9012#Cross-stack contract',?,'spec-ingest-v1'),
        \\ (9002,'cited_artifact_section','text',null,'s','artifact',9013,'artifact:9013#Coder brief',?,'spec-ingest-v1')
    , &.{
        .{ .text = acceptance_digest },
        .{ .text = next_exact_digest },
        .{ .text = product_digest },
        .{ .text = tech_digest },
        .{ .text = roadmap_digest },
        .{ .text = test_digest },
    });
    var live = try packet.assembleTask(allocator, &live_db, 9002);
    defer live.deinit();
    try std.testing.expect(live.packet.ready());
    try std.testing.expectEqualStrings("Anchor", live.packet.input.anchor_plans[0].display_label);
    try std.testing.expectEqual(@as(usize, 4), live.packet.input.citations.len);
    try std.testing.expectEqual(@as(i64, 9020), live.packet.input.decisions[0].id);
    try std.testing.expectEqual(@as(i64, 2142), live.packet.input.scenarios[0].id);
    try std.testing.expect(live.packet.input.scenarios[0].covered);
    try std.testing.expectEqualStrings("draft", live.packet.input.scenarios[0].status);
    try std.testing.expectEqualStrings("doing", live.packet.input.status);
    try std.testing.expectEqualStrings("active", live.packet.input.owning_plans[0].status);
    try std.testing.expectEqualStrings("active", live.packet.input.anchor_plans[0].status);
    try std.testing.expectEqualStrings("liveclaim", live.packet.input.claims[0].text);
    try std.testing.expectEqualStrings("current", live.packet.input.facts[0].freshness);
    try std.testing.expectEqualStrings(acceptance_digest, live.packet.input.facts[0].current_digest);

    // Display-only plan and artifact labels remain renderable but do not
    // participate in the semantic digest.
    try live_db.exec("update plans set title='Renamed owner label' where id=9001; update artifacts set title='Renamed product label' where id=9010");
    var label_renamed = try packet.assembleTask(allocator, &live_db, 9002);
    defer label_renamed.deinit();
    try std.testing.expectEqualStrings("Renamed owner label", label_renamed.packet.input.owning_plans[0].display_label);
    try std.testing.expectEqualStrings("Renamed product label", label_renamed.packet.input.citations[0].display_label);
    try std.testing.expectEqualStrings(&live.packet.digest, &label_renamed.packet.digest);

    // Black-box readiness regressions: the packet assembler must not recover
    // mandatory context from the whole body, a missing fact, or a whole
    // artifact placeholder.
    try live_db.exec("update tasks set body=replace(body,'## Acceptance Criteria','## Acceptance') where id=9002");
    var no_acceptance_heading = try packet.assembleTask(allocator, &live_db, 9002);
    defer no_acceptance_heading.deinit();
    try std.testing.expect(std.mem.indexOfScalar(packet.ReadinessReason, no_acceptance_heading.packet.reasons, .missing_acceptance_section) != null);
    try live_db.exec("update tasks set body=replace(body,'## Acceptance','## Acceptance Criteria') where id=9002");

    try live_db.exec("update routing_task_facts set fact_kind='acceptance_fact_hidden' where task_id=9002 and fact_kind='acceptance_complete'");
    var no_acceptance_fact = try packet.assembleTask(allocator, &live_db, 9002);
    defer no_acceptance_fact.deinit();
    try std.testing.expect(std.mem.indexOfScalar(packet.ReadinessReason, no_acceptance_fact.packet.reasons, .missing_acceptance_fact) != null);
    try live_db.exec("update routing_task_facts set fact_kind='acceptance_complete' where task_id=9002 and fact_kind='acceptance_fact_hidden'");

    try live_db.exec("update routing_task_facts set source_locator='artifact:9010' where task_id=9002 and source_entity_id=9010 and fact_kind='cited_artifact_section'");
    var whole_artifact_placeholder = try packet.assembleTask(allocator, &live_db, 9002);
    defer whole_artifact_placeholder.deinit();
    try std.testing.expect(std.mem.indexOfScalar(packet.ReadinessReason, whole_artifact_placeholder.packet.reasons, .missing_product_spec) != null);
    try std.testing.expect(std.mem.indexOfScalar(packet.ReadinessReason, whole_artifact_placeholder.packet.reasons, .unresolved_citation) != null);
    try live_db.exec("update routing_task_facts set source_locator='artifact:9010#Requirements' where task_id=9002 and source_entity_id=9010 and fact_kind='cited_artifact_section'");

    // Six directly linked cross-plan scenarios must survive assembly even
    // though the owning-plan coverage oracle marks each one uncovered.
    try live_db.execSlice(allocator,
        \\insert into plans(id,scope_kind,scope_id,title,slug,status) values(9060,'association',1,'Other anchor','other-anchor','active');
        \\insert into test_scenarios(id,scope_kind,scope_id,title,body,status) values
        \\(2143,'association',1,'Cross 1','cross-1','draft'),
        \\(2144,'association',1,'Cross 2','cross-2','draft'),
        \\(2145,'association',1,'Cross 3','cross-3','draft'),
        \\(2146,'association',1,'Cross 4','cross-4','draft'),
        \\(2147,'association',1,'Cross 5','cross-5','draft'),
        \\(2148,'association',1,'Cross 6','cross-6','draft');
        \\insert into entity_links(from_kind,from_id,to_kind,to_id,relationship)
        \\select 'test_scenario',id,'task',9002,'verifies' from test_scenarios where id between 2143 and 2148;
        \\insert into entity_links(from_kind,from_id,to_kind,to_id,relationship)
        \\select 'test_scenario',id,'plan',9060,'derives-from' from test_scenarios where id between 2143 and 2148;
    );
    var cross_plan = try packet.assembleTask(allocator, &live_db, 9002);
    defer cross_plan.deinit();
    try std.testing.expectEqual(@as(usize, 7), cross_plan.packet.input.scenarios.len);
    var cross_plan_count: usize = 0;
    for (cross_plan.packet.input.scenarios) |scenario| {
        if (scenario.id >= 2143 and scenario.id <= 2148) {
            cross_plan_count += 1;
            try std.testing.expect(!scenario.covered);
        }
    }
    try std.testing.expectEqual(@as(usize, 6), cross_plan_count);
    try std.testing.expect(std.mem.indexOfScalar(packet.ReadinessReason, cross_plan.packet.reasons, .uncovered_required_scenario) != null);
    try live_db.exec("delete from entity_links where from_kind='test_scenario' and from_id between 2143 and 2148; delete from test_scenarios where id between 2143 and 2148");

    var reviewer_packet = try packet.assemblePlanning(allocator, &live_db, .spec_reviewer, 9000);
    defer reviewer_packet.deinit();
    try std.testing.expect(!reviewer_packet.packet.ready());
    try std.testing.expect(std.mem.indexOfScalar(packet.PlanningReason, reviewer_packet.packet.reasons, .missing_strict_preview) != null);
    var ingestor_packet = try packet.assemblePlanning(allocator, &live_db, .ingestor, 9000);
    defer ingestor_packet.deinit();
    try std.testing.expect(!ingestor_packet.packet.ready());
    try std.testing.expect(std.mem.indexOfScalar(packet.PlanningReason, ingestor_packet.packet.reasons, .missing_strict_preview) != null);
    try std.testing.expect(std.mem.indexOfScalar(packet.PlanningReason, ingestor_packet.packet.reasons, .missing_apply_boundary) != null);
    try std.testing.expectEqualStrings("tasks:2;covered:2;scenarios:1", ingestor_packet.packet.input.coverage[0].text);
    try live_db.exec(
        \\insert into plans(id,scope_kind,scope_id,title,slug,status) values(9050,'association',1,'Empty planning plan','empty-planning','active');
        \\insert into entity_links(from_kind,from_id,to_kind,to_id,relationship) values
        \\('artifact',9010,'plan',9050,'derives-from'),('artifact',9011,'plan',9050,'derives-from'),
        \\('artifact',9012,'plan',9050,'derives-from'),('artifact',9013,'plan',9050,'derives-from'),
        \\('decision',9020,'plan',9050,'derives-from');
    );
    var empty_ingestor = try packet.assemblePlanning(allocator, &live_db, .ingestor, 9050);
    defer empty_ingestor.deinit();
    try std.testing.expect(!empty_ingestor.packet.ready());
    try std.testing.expectEqualStrings("tasks:0;covered:0;scenarios:0", empty_ingestor.packet.input.coverage[0].text);
    try std.testing.expect(std.mem.indexOfScalar(packet.PlanningReason, empty_ingestor.packet.reasons, .incomplete_coverage) != null);
    var invocation = try packet.assembleInvocation(allocator, &live_db, 9000, "configured-static");
    defer invocation.deinit();
    switch (invocation.resolution) {
        .static_fallback => |fallback| try std.testing.expectEqualStrings("invocation_packet_not_ready", fallback.reason),
        else => return error.TestUnexpectedResult,
    }

    const workflow_path = try std.fs.path.join(allocator, &.{ suite.tmpAbsPath(), "authoritative-brief.lua" });
    defer allocator.free(workflow_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = workflow_path, .data =
        \\function run()
        \\  local value = ctx.brief({plan_id=9001, task_id=9002, claim_token="liveclaim", problem_statement="caller text must not win", gates={"caller gate must not win"}})
        \\  flow.result({brief=value})
        \\end
    });
    const result = try runExecute(allocator, suite.tmpAbsPath(), suite.absDbPath(), workflow_path);
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);
    if (result.term.exited != 0)
        std.debug.print("planar-execute stdout:\n{s}\nstderr:\n{s}\n", .{ result.stdout, result.stderr });
    try std.testing.expectEqual(@as(u32, 0), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "Authoritative packet digest") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "Exact current rows reach the brief") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "SCENARIO_2142_EXACT_CONTEXT") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "scenario:2142") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, acceptance_digest) != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "caller text must not win") == null);

    const plan_mismatch_path = try std.fs.path.join(allocator, &.{ suite.tmpAbsPath(), "brief-plan-mismatch.lua" });
    defer allocator.free(plan_mismatch_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = plan_mismatch_path, .data =
        \\function run()
        \\  ctx.brief({plan_id=9000, task_id=9002, claim_token="liveclaim", problem_statement="ignored"})
        \\end
    });
    const plan_mismatch = try runExecute(allocator, suite.tmpAbsPath(), suite.absDbPath(), plan_mismatch_path);
    defer allocator.free(plan_mismatch.stdout);
    defer allocator.free(plan_mismatch.stderr);
    try std.testing.expect(plan_mismatch.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, plan_mismatch.stderr, "does not match authoritative packet") != null);

    const claim_mismatch_path = try std.fs.path.join(allocator, &.{ suite.tmpAbsPath(), "brief-claim-mismatch.lua" });
    defer allocator.free(claim_mismatch_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = claim_mismatch_path, .data =
        \\function run()
        \\  ctx.brief({plan_id=9001, task_id=9002, claim_token="caller-claim", problem_statement="ignored"})
        \\end
    });
    const claim_mismatch = try runExecute(allocator, suite.tmpAbsPath(), suite.absDbPath(), claim_mismatch_path);
    defer allocator.free(claim_mismatch.stdout);
    defer allocator.free(claim_mismatch.stderr);
    try std.testing.expect(claim_mismatch.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, claim_mismatch.stderr, "does not match authoritative packet") != null);

    try live_db.exec("update agent_work_claims set lease_expires_at='2000-01-01T00:00:00.000Z' where claim_token='liveclaim'");
    var expired = try packet.assembleTask(allocator, &live_db, 9002);
    defer expired.deinit();
    try std.testing.expectEqualStrings("expired", expired.packet.input.claims[0].status);
    try std.testing.expect(std.mem.indexOfScalar(packet.ReadinessReason, expired.packet.reasons, .inactive_claim) != null);
    try std.testing.expect(!std.mem.eql(u8, &live.packet.digest, &expired.packet.digest));

    try live_db.exec("update agent_work_claims set lease_expires_at='2999-01-01T00:00:00.000Z' where claim_token='liveclaim'");
    try live_db.exec("update tasks set status='blocked' where id=9002");
    var transitioned = try packet.assembleTask(allocator, &live_db, 9002);
    defer transitioned.deinit();
    try std.testing.expectEqualStrings("blocked", transitioned.packet.input.status);
    try std.testing.expect(std.mem.indexOfScalar(packet.ReadinessReason, transitioned.packet.reasons, .invalid_task_status) != null);
    try std.testing.expect(!std.mem.eql(u8, &live.packet.digest, &transitioned.packet.digest));

    try live_db.exec("update tasks set status='doing' where id=9002");
    try live_db.exec("update plans set status='paused' where id=9001");
    var paused_owner = try packet.assembleTask(allocator, &live_db, 9002);
    defer paused_owner.deinit();
    try std.testing.expect(paused_owner.packet.ready());
    try std.testing.expectEqualStrings("paused", paused_owner.packet.input.owning_plans[0].status);
    try std.testing.expect(!std.mem.eql(u8, &live.packet.digest, &paused_owner.packet.digest));

    try live_db.exec("delete from entity_links where from_kind='test_scenario' and from_id=2142 and to_kind='task' and to_id=9002 and relationship='verifies'");
    var uncovered = try packet.assembleTask(allocator, &live_db, 9002);
    defer uncovered.deinit();
    try std.testing.expectEqual(@as(usize, 0), uncovered.packet.input.scenarios.len);
    try std.testing.expect(std.mem.indexOfScalar(packet.ReadinessReason, uncovered.packet.reasons, .uncovered_required_scenario) != null);

    try live_db.exec("update decisions set status='withdrawn' where id=9020");
    var rejected_decision = try packet.assemblePlanning(allocator, &live_db, .ingestor, 9000);
    defer rejected_decision.deinit();
    try std.testing.expect(std.mem.indexOfScalar(packet.PlanningReason, rejected_decision.packet.reasons, .invalid_locked_decisions) != null);
    try live_db.exec("update decisions set status='accepted' where id=9020");

    try live_db.exec("update artifacts set status='retired' where id=9010");
    var retired_artifact = try packet.assemblePlanning(allocator, &live_db, .spec_reviewer, 9000);
    defer retired_artifact.deinit();
    try std.testing.expect(std.mem.indexOfScalar(packet.PlanningReason, retired_artifact.packet.reasons, .non_current_artifacts) != null);
}

test "fact freshness rejects reversed relationship directions" {
    const allocator = std.testing.allocator;
    var suite = harness.Suite.init(allocator);
    defer suite.deinit();
    _ = suite.registerProject("routing-directions");
    const path = try allocator.dupeZ(u8, suite.absDbPath());
    defer allocator.free(path);
    var live_db = try db.sqlite.Db.open(path);
    defer live_db.close();
    try live_db.execSlice(allocator,
        \\insert into projects(id,slug,name) values(9204,'direction-target','Direction target');
        \\insert into plans(id,scope_kind,scope_id,title,slug,status) values(9200,'global',null,'Direction plan','direction-plan','active');
        \\insert into tasks(id,scope_kind,scope_id,plan_id,title,body,status,next_action) values
        \\(9201,'global',null,9200,'Direction task','Direction-sensitive facts.','doing','Verify exact relationship directions.'),
        \\(9202,'global',null,9200,'Outgoing target','','done','Done.'),
        \\(9203,'global',null,9200,'Incoming source','','done','Done.');
        \\insert into entity_links(from_kind,from_id,to_kind,to_id,relationship) values
        \\('task',9201,'task',9202,'depends-on'),
        \\('task',9203,'task',9201,'depends-on'),
        \\('task',9201,'repo',9204,'touches');
    );
    const blocks_digest = try sourceDigestAlloc(allocator, "task", 9202, "task:9202", "depends-on");
    defer allocator.free(blocks_digest);
    const blocked_by_digest = try sourceDigestAlloc(allocator, "task", 9203, "task:9203", "depends-on");
    defer allocator.free(blocked_by_digest);
    const touch_digest = try sourceDigestAlloc(allocator, "repo", 9204, "repo:9204", "touches");
    defer allocator.free(touch_digest);
    _ = try live_db.execParams(
        \\insert into routing_task_facts(task_id,fact_kind,value_type,value_integer,source_entity_kind,source_entity_id,source_locator,source_digest,materializer_version)
        \\values(9201,'blocks','integer',9202,'task',9202,'task:9202',?,'spec-ingest-v1'),
        \\      (9201,'blocked_by','integer',9203,'task',9203,'task:9203',?,'spec-ingest-v1'),
        \\      (9201,'touch','integer',9204,'repo',9204,'repo:9204',?,'spec-ingest-v1')
    , &.{ .{ .text = blocks_digest }, .{ .text = blocked_by_digest }, .{ .text = touch_digest } });

    var current = try packet.assembleTask(allocator, &live_db, 9201);
    defer current.deinit();
    for (current.packet.input.facts) |fact|
        try std.testing.expectEqualStrings("current", fact.freshness);

    try live_db.exec(
        \\delete from entity_links where relationship in ('depends-on','touches');
        \\insert into entity_links(from_kind,from_id,to_kind,to_id,relationship) values
        \\('task',9202,'task',9201,'depends-on'),
        \\('task',9201,'task',9203,'depends-on'),
        \\('repo',9204,'task',9201,'touches');
    );
    var reversed = try packet.assembleTask(allocator, &live_db, 9201);
    defer reversed.deinit();
    for (reversed.packet.input.facts) |fact|
        try std.testing.expectEqualStrings("stale", fact.freshness);
    try std.testing.expect(std.mem.indexOfScalar(packet.ReadinessReason, reversed.packet.reasons, .stale_fact) != null);
}

fn sourceDigestAlloc(
    allocator: std.mem.Allocator,
    source_kind: []const u8,
    source_id: i64,
    locator: []const u8,
    semantic_source: []const u8,
) ![]const u8 {
    const canonical = try std.fmt.allocPrint(
        allocator,
        "{s}\x00{d}\x00{s}\x00{s}",
        .{ source_kind, source_id, locator, semantic_source },
    );
    defer allocator.free(canonical);
    var digest: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(canonical, &digest, .{});
    return std.fmt.allocPrint(allocator, "{x}", .{digest});
}

fn envValue(comptime key: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const value = std.mem.span(entry);
        if (std.mem.startsWith(u8, value, key ++ "=")) return value[(key ++ "=").len..];
    }
    @panic(key ++ " missing");
}

fn runExecute(allocator: std.mem.Allocator, cwd: []const u8, db_path: []const u8, workflow: []const u8) !std.process.RunResult {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var count: usize = 0;
    while (raw[count] != null) : (count += 1) {}
    const slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..count :null]);
    var env = try (std.process.Environ{ .block = .{ .slice = slice } }).createMap(allocator);
    defer env.deinit();
    try env.put("PLANAR_DB", db_path);
    try env.put("PLANAR_CONFIG_PATH", "/nonexistent-planar-config.toml");
    try env.put("PLANAR_DISABLE_WORKTREE_GATE", "1");
    const planar_dir = std.fs.path.dirname(envValue("PLANAR_BIN")) orelse ".";
    const old_path = env.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(allocator, "{s}:{s}", .{ planar_dir, old_path });
    defer allocator.free(new_path);
    try env.put("PATH", new_path);
    return std.process.run(allocator, std.testing.io, .{ .argv = &.{ envValue("PLANAR_EXECUTE_BIN"), "run", workflow, "--phase", "run" }, .cwd = .{ .path = cwd }, .environ_map = &env });
}

test "current packet is the sole coder brief source and stale lineage fails closed" {
    const allocator = std.testing.allocator;
    const citations = [_]packet.Evidence{
        .{ .kind = "product_spec", .id = 539, .locator = "product-spec.md#requirements", .text = "product", .source_digest = "p", .current_digest = "p" },
        .{ .kind = "tech_spec", .id = 540, .locator = "tech-spec.md#dispatch-time-packet-compiler", .text = "tech", .source_digest = "t", .current_digest = "t" },
        .{ .kind = "roadmap", .id = 541, .locator = "roadmap.md#cross-stack-contract", .text = "roadmap", .source_digest = "r", .current_digest = "r" },
        .{ .kind = "test_spec", .id = 542, .locator = "test-spec.md#coder-brief", .text = "tests", .source_digest = "s", .current_digest = "s" },
    };
    const linked = [_]packet.Evidence{
        .{ .kind = "plan", .id = 950, .locator = "plan:950", .text = "owning plan", .source_digest = "plan", .current_digest = "plan" },
    };
    const claims = [_]packet.Evidence{
        .{ .kind = "claim", .id = 951, .locator = "claim:951", .text = "active-claim", .source_digest = "claim", .current_digest = "claim", .status = "active" },
    };
    const fresh = [_]packet.Evidence{
        .{ .kind = "acceptance_complete", .id = 5526, .locator = "task:5526#acceptance", .text = "true", .source_digest = "current", .current_digest = "current" },
        .{ .kind = "next_action_exact", .id = 5527, .locator = "task:5526#next-action", .text = "true", .source_digest = "current", .current_digest = "current" },
    };
    const input: packet.TaskInput = .{
        .task_id = 5526,
        .status = "doing",
        .title = "Compile authoritative packets",
        .body =
        \\Preserve exact packet evidence.
        \\## Acceptance Criteria
        \\The coder brief preserves exact mandatory fields and digest.
        ,
        .next_action = "Compile live linked context, reject incomplete context, and run four named gates.",
        .acceptance_criteria = "The coder brief preserves exact mandatory fields and digest.",
        .owning_plans = &linked,
        .anchor_plans = &linked,
        .citations = &citations,
        .decisions = &linked,
        .questions = &.{},
        .scenarios = &linked,
        .dependencies = &linked,
        .touches = &linked,
        .claims = &claims,
        .validation_gates = &linked,
        .facts = &fresh,
    };

    var compiled = try packet.compileTask(allocator, input);
    defer compiled.deinit(allocator);
    const brief = try packet.coderBrief(compiled);
    try std.testing.expectEqualStrings(input.title, brief.title);
    try std.testing.expectEqualStrings(input.acceptance_criteria, brief.acceptance_criteria);
    try std.testing.expectEqualStrings(&compiled.digest, &brief.packet_digest);

    const stale = [_]packet.Evidence{
        .{ .kind = "acceptance_complete", .id = 5526, .locator = "task:5526#acceptance", .text = "true", .source_digest = "prior", .current_digest = "changed" },
    };
    var stale_input = input;
    stale_input.facts = &stale;
    var rejected = try packet.compileTask(allocator, stale_input);
    defer rejected.deinit(allocator);
    try std.testing.expectError(error.PacketNotReady, packet.coderBrief(rejected));
}
