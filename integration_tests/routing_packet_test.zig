//! Integration contract from an authoritative packet through coder brief.

const std = @import("std");
const packet = @import("routing_packet");
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
        \\(9003,'association',1,9001,'Dependency','done','Already complete.');
        \\insert into artifacts(id,scope_kind,scope_id,kind,title,body,source_path,status) values
        \\(9010,'association',1,'product_spec','P','p','p.md','active'),(9011,'association',1,'tech_spec','T','t','t.md','active'),
        \\(9012,'association',1,'roadmap','R','r','r.md','active'),(9013,'association',1,'test_spec','S','s','s.md','active');
        \\insert into decisions(id,scope_kind,scope_id,title,body,status) values(9020,'association',1,'Locked','locked text','accepted');
        \\insert into test_scenarios(id,scope_kind,scope_id,title,body,status) values(9030,'association',1,'Scenario','covered','ready');
        \\insert into entity_links(from_kind,from_id,to_kind,to_id,relationship) values
        \\('plan',9001,'plan',9000,'derives-from'),('task',9002,'artifact',9010,'cites'),('task',9002,'artifact',9011,'cites'),
        \\('task',9002,'artifact',9012,'cites'),('task',9002,'artifact',9013,'cites'),('task',9002,'decision',9020,'addresses'),
        \\('task',9002,'test_scenario',9030,'verifies'),('task',9002,'task',9003,'blocks');
        \\insert into task_touch_paths(task_id,repo_id,path) select 9002,id,'src/live.zig' from projects where slug='routing-live';
        \\insert into sessions(id,scope_kind,scope_id,title,status) values(9040,'association',1,'claim session','active');
        \\insert into agent_work_claims(claim_token,session_id,entity_kind,entity_id,status,vendor,lease_expires_at) values('liveclaim',9040,'task',9002,'active','test','2999-01-01T00:00:00.000Z');
    );
    var live = try packet.assembleTask(allocator, &live_db, 9002);
    defer live.deinit();
    try std.testing.expect(live.packet.ready());
    try std.testing.expectEqualStrings("Anchor", live.packet.input.anchor_plans[0].text);
    try std.testing.expectEqual(@as(usize, 4), live.packet.input.citations.len);
    try std.testing.expectEqualStrings("liveclaim", live.packet.input.claims[0].text);

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
    try std.testing.expectEqual(@as(u32, 0), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "Authoritative packet digest") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "Exact current rows reach the brief") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "caller text must not win") == null);
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
    return std.process.run(allocator, std.testing.io, .{ .argv = &.{ envValue("PLANAR_EXECUTE_BIN"), "run", workflow, "--phase", "run" }, .cwd = .{ .path = cwd }, .environ_map = &env });
}

test "current packet is the sole coder brief source and stale lineage fails closed" {
    const allocator = std.testing.allocator;
    const citations = [_]packet.Evidence{
        .{ .kind = "product_spec", .id = 539, .locator = "product-spec.md#requirements", .text = "product" },
        .{ .kind = "tech_spec", .id = 540, .locator = "tech-spec.md#dispatch-time-packet-compiler", .text = "tech" },
        .{ .kind = "roadmap", .id = 541, .locator = "roadmap.md#cross-stack-contract", .text = "roadmap" },
        .{ .kind = "test_spec", .id = 542, .locator = "test-spec.md#coder-brief", .text = "tests" },
    };
    const linked = [_]packet.Evidence{
        .{ .kind = "plan", .id = 950, .locator = "plan:950", .text = "owning plan" },
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
        .claims = &linked,
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
