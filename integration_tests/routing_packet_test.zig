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
        \\(9002,'association',1,9001,'Live packet task','## Acceptance criteria
        \\Exact current rows reach the brief.
        \\## Required validation
        \\make test-integration','doing','Compile live rows and verify the production brief.'),
        \\(9003,'association',1,9001,'Dependency','','done','Already complete.');
        \\insert into artifacts(id,scope_kind,scope_id,kind,title,body,source_path,status) values
        \\(9010,'association',1,'product_spec','P','p','p.md','active'),(9011,'association',1,'tech_spec','T','t','t.md','active'),
        \\(9012,'association',1,'roadmap','R','r','r.md','active'),(9013,'association',1,'test_spec','S','s','s.md','active');
        \\insert into decisions(id,scope_kind,scope_id,title,body,status) values(9020,'association',1,'Locked','locked text','accepted');
        \\insert into test_scenarios(id,scope_kind,scope_id,title,body,status) values(2142,'association',1,'Coder brief equals authoritative packet','SCENARIO_2142_EXACT_CONTEXT','ready');
        \\insert into entity_links(from_kind,from_id,to_kind,to_id,relationship) values
        \\('plan',9001,'plan',9000,'derives-from'),('task',9002,'artifact',9010,'cites'),('task',9002,'artifact',9011,'cites'),
        \\('task',9002,'artifact',9012,'cites'),('task',9002,'artifact',9013,'cites'),('task',9002,'decision',9020,'cites'),
        \\('test_scenario',2142,'task',9002,'verifies'),('task',9002,'task',9003,'blocks'),
        \\('artifact',9010,'plan',9000,'derives-from'),('artifact',9011,'plan',9000,'derives-from'),
        \\('artifact',9012,'plan',9000,'derives-from'),('artifact',9013,'plan',9000,'derives-from'),
        \\('decision',9020,'plan',9000,'derives-from'),('test_scenario',2142,'plan',9000,'derives-from');
        \\insert into task_touch_paths(task_id,repo_id,path) select 9002,id,'src/live.zig' from projects order by id limit 1;
        \\insert into sessions(id,vendor) values(9040,'test');
        \\insert into agent_work_claims(claim_token,session_id,entity_kind,entity_id,status,vendor,lease_expires_at) values('liveclaim',9040,'task',9002,'active','test','2999-01-01T00:00:00.000Z');
    );
    const fact_digest = try sourceDigestAlloc(
        allocator,
        "task",
        9002,
        "next_action",
        "Compile live rows and verify the production brief.",
    );
    defer allocator.free(fact_digest);
    _ = try live_db.execParams(
        \\insert into routing_task_facts(
        \\ task_id,fact_kind,value_type,value_text,source_entity_kind,
        \\ source_entity_id,source_locator,source_digest,materializer_version
        \\) values(9002,'next_action','text','Compile live rows and verify the production brief.',
        \\ 'task',9002,'next_action',?,'spec-ingest-v1')
    , &.{.{ .text = fact_digest }});
    var live = try packet.assembleTask(allocator, &live_db, 9002);
    defer live.deinit();
    try std.testing.expect(live.packet.ready());
    try std.testing.expectEqualStrings("Anchor", live.packet.input.anchor_plans[0].text);
    try std.testing.expectEqual(@as(usize, 4), live.packet.input.citations.len);
    try std.testing.expectEqual(@as(i64, 9020), live.packet.input.decisions[0].id);
    try std.testing.expectEqual(@as(i64, 2142), live.packet.input.scenarios[0].id);
    try std.testing.expectEqualStrings("liveclaim", live.packet.input.claims[0].text);
    try std.testing.expectEqualStrings("current", live.packet.input.facts[0].freshness);
    try std.testing.expectEqualStrings(fact_digest, live.packet.input.facts[0].current_digest);

    var reviewer_packet = try packet.assemblePlanning(allocator, &live_db, .spec_reviewer, 9000);
    defer reviewer_packet.deinit();
    try std.testing.expect(reviewer_packet.packet.ready());
    var ingestor_packet = try packet.assemblePlanning(allocator, &live_db, .ingestor, 9000);
    defer ingestor_packet.deinit();
    try std.testing.expect(ingestor_packet.packet.ready());
    var invocation = try packet.assembleInvocation(allocator, &live_db, 9000, "configured-static");
    defer invocation.deinit();
    switch (invocation.resolution) {
        .packet => {},
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
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, fact_digest) != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "caller text must not win") == null);
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
    };
    const input: packet.TaskInput = .{
        .task_id = 5526,
        .title = "Compile authoritative packets",
        .body = "Preserve exact packet evidence.",
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
