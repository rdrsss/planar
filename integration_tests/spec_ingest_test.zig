//! integration_tests/spec_ingest_test.zig
//!
//! Integration coverage for `spec ingest` apply guards and rollback contracts.

const std = @import("std");
const harness = @import("harness");

test "spec ingest rejects --apply-removals without --apply" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const stderr = suite.expectFailure(&.{ "spec", "ingest", "1", "--apply-removals" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "--apply-removals requires --apply"));
}

test "spec ingest apply unresolved task slug rolls back derived graph" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const tech_body =
        \\# Atomic Unresolved Slug Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
        \\## Decisions
        \\
        \\### Use a Savepoint Boundary
        \\
        \\A failed apply must leave no derived rows behind.
        \\
    ;
    const roadmap_body =
        \\# Atomic Unresolved Slug Roadmap
        \\
        \\## Apply Boundary
        \\
        \\- Create the rollback fixture task [slug: real-fixture-task]
        \\
    ;
    const test_spec_body =
        \\# Atomic Unresolved Slug Test Spec
        \\
        \\## Scenarios
        \\
        \\### Scenario: Missing task slug fails apply
        \\
        \\**Bucket:** error path
        \\**Verifies:** task:missing-fixture-task
        \\
        \\This scenario intentionally cites a task slug not present in the roadmap.
        \\
    ;

    const fixture = createSpecIngestFixture(
        &suite,
        arena,
        "Atomic Unresolved Slug Apply Plan",
        tech_body,
        roadmap_body,
        test_spec_body,
        "workbench-atomic-unresolved",
    );
    try expectNoDerivedGraph(captureDerivedGraph(&suite, arena, fixture.plan_id, fixture.env));

    const stderr = suite.expectFailureWith(&.{ "spec", "ingest", fixture.plan_id, "--apply" }, fixture.env);
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "ResolveSlugFailed"));

    try expectNoDerivedGraph(captureDerivedGraph(&suite, arena, fixture.plan_id, fixture.env));
}

test "spec ingest apply mid-write slug conflict rolls back derived graph" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const tech_body =
        \\# Atomic Mid Write Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    const roadmap_body =
        \\# Atomic Mid Write Roadmap
        \\
        \\## First Milestone
        \\
        \\- First task writes before the conflict [slug: duplicate-atomic-task]
        \\
        \\## Second Milestone
        \\
        \\- Second task trips the duplicate slug [slug: duplicate-atomic-task]
        \\
    ;

    const fixture = createSpecIngestFixture(
        &suite,
        arena,
        "Atomic Mid Write Apply Plan",
        tech_body,
        roadmap_body,
        null,
        "workbench-atomic-mid-write",
    );
    try expectNoDerivedGraph(captureDerivedGraph(&suite, arena, fixture.plan_id, fixture.env));

    const stderr = suite.expectFailureWith(&.{ "spec", "ingest", fixture.plan_id, "--apply" }, fixture.env);
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "SlugConflict"));

    try expectNoDerivedGraph(captureDerivedGraph(&suite, arena, fixture.plan_id, fixture.env));
}

test "spec ingest apply-removals failure rolls back retirements and replacements" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const tech_body =
        \\# Apply Removals Rollback Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    const roadmap_body =
        \\# Apply Removals Rollback Roadmap
        \\
        \\## Milestone Alpha
        \\
        \\- Old implementation [slug: shared-task]
        \\
    ;
    const test_spec_body =
        \\# Apply Removals Rollback Test Spec
        \\
        \\## Scenarios
        \\
        \\### Scenario: Shared task covered
        \\
        \\**Bucket:** happy path
        \\**Verifies:** task:shared-task
        \\
        \\The initial graph has a valid verifying scenario.
        \\
    ;

    const fixture = createSpecIngestFixture(
        &suite,
        arena,
        "Apply Removals Rollback Plan",
        tech_body,
        roadmap_body,
        test_spec_body,
        "workbench-apply-removals-rollback",
    );

    const push_json = suite.mustRunWith(&.{ "workbench", "push", "--json", fixture.plan_id }, fixture.env);
    defer gpa.free(push_json);
    const roadmap_path = try findArtifactPathFromPush(arena, fixture.wb_root, push_json, "roadmap");
    const test_spec_path = try findArtifactPathFromPush(arena, fixture.wb_root, push_json, "test_spec");

    const apply1 = suite.execWith(&.{ "spec", "ingest", fixture.plan_id, "--apply" }, fixture.env);
    defer apply1.deinit(gpa);
    try std.testing.expect(apply1.term == .exited and apply1.term.exited == 0);

    const PlanRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8 = "",
        slug: []const u8 = "",
    };
    const TaskRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8 = "",
        slug: ?[]const u8 = null,
    };

    const child_plans_json = suite.mustRunWith(&.{ "plan", "list", "--json", "--parent", fixture.plan_id }, fixture.env);
    defer gpa.free(child_plans_json);
    const child_plans = parseJSON([]const PlanRow, arena, child_plans_json);

    var old_plan_id: i64 = 0;
    var old_plan_slug: []const u8 = "";
    for (child_plans) |row| {
        if (std.mem.eql(u8, row.title, "Milestone Alpha")) {
            old_plan_id = row.id;
            old_plan_slug = row.slug;
            break;
        }
    }
    try std.testing.expect(old_plan_id > 0);
    const old_plan_id_s = std.fmt.allocPrint(arena, "{d}", .{old_plan_id}) catch @panic("OOM");

    const old_tasks_json = suite.mustRunWith(&.{ "task", "list", "--json", "--plan", old_plan_id_s }, fixture.env);
    defer gpa.free(old_tasks_json);
    const old_tasks = parseJSON([]const TaskRow, arena, old_tasks_json);
    try std.testing.expect(old_tasks.len == 1);
    const old_task_id_s = std.fmt.allocPrint(arena, "{d}", .{old_tasks[0].id}) catch @panic("OOM");

    try replaceFileBlock(
        arena,
        roadmap_path,
        \\## Milestone Alpha
        \\
        \\- Old implementation [slug: shared-task]
        \\
    ,
        \\## Milestone Alpha!
        \\
        \\- New implementation [slug: shared-task]
        \\
        ,
    );
    try replaceFileBlock(arena, test_spec_path, "task:shared-task", "task:missing-after-removal");

    const stderr = suite.expectFailureWith(&.{ "spec", "ingest", fixture.plan_id, "--apply", "--apply-removals" }, fixture.env);
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "ResolveSlugFailed"));

    const old_plan_json = suite.mustRunWith(&.{ "plan", "show", "--json", old_plan_id_s }, fixture.env);
    defer gpa.free(old_plan_json);
    const old_plan = parseJSON(PlanRow, arena, old_plan_json);
    try std.testing.expectEqualStrings("Milestone Alpha", old_plan.title);
    try std.testing.expect(!std.mem.eql(u8, old_plan.status, "abandoned"));
    try std.testing.expectEqualStrings(old_plan_slug, old_plan.slug);
    try std.testing.expect(!std.mem.startsWith(u8, old_plan.slug, "stale-"));

    const old_task_json = suite.mustRunWith(&.{ "task", "show", "--json", old_task_id_s }, fixture.env);
    defer gpa.free(old_task_json);
    const old_task = parseJSON(TaskRow, arena, old_task_json);
    try std.testing.expectEqualStrings("Old implementation", old_task.title);
    try std.testing.expect(!std.mem.eql(u8, old_task.status, "cancelled"));
    try std.testing.expect(old_task.slug != null);
    try std.testing.expectEqualStrings("shared-task", old_task.slug.?);

    const refreshed_plans_json = suite.mustRunWith(&.{ "plan", "list", "--json", "--parent", fixture.plan_id }, fixture.env);
    defer gpa.free(refreshed_plans_json);
    const refreshed_plans = parseJSON([]const PlanRow, arena, refreshed_plans_json);
    var replacement_plan_count: usize = 0;
    for (refreshed_plans) |row| {
        if (std.mem.eql(u8, row.title, "Milestone Alpha!")) replacement_plan_count += 1;
    }
    try std.testing.expectEqual(@as(usize, 0), replacement_plan_count);
}

test "spec ingest batched apply keeps per-anchor transaction boundary" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const wb = createWorkbenchEnv(&suite, arena, "workbench-multi-plan-apply");
    const valid = createSpecIngestFixtureWithEnv(
        &suite,
        arena,
        "Multi Apply Valid Plan",
        "# Multi Valid Tech Spec\n\n## Status\n\nDraft.\n",
        "# Multi Valid Roadmap\n\n## Valid Milestone\n\n- Valid task [slug: valid-task]\n",
        null,
        wb.env,
        wb.wb_root,
    );
    const invalid = createSpecIngestFixtureWithEnv(
        &suite,
        arena,
        "Multi Apply Invalid Plan",
        "# Multi Invalid Tech Spec\n\n## Status\n\nDraft.\n",
        "# Multi Invalid Roadmap\n\n## Invalid Milestone\n\n- Invalid task [slug: invalid-task]\n",
        "# Multi Invalid Test Spec\n\n## Scenarios\n\n### Scenario: Missing slug\n\n**Bucket:** error path\n**Verifies:** task:missing-invalid-task\n",
        wb.env,
        wb.wb_root,
    );

    const res = suite.execWith(&.{ "spec", "ingest", valid.plan_id, invalid.plan_id, "--apply" }, wb.env);
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited and res.term.exited != 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stderr, 1, "ResolveSlugFailed"));
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stderr, 1, "one or more plans failed to ingest"));

    const valid_graph = captureDerivedGraph(&suite, arena, valid.plan_id, valid.env);
    try std.testing.expectEqualStrings("active", valid_graph.anchor_status);
    try std.testing.expectEqual(@as(usize, 1), valid_graph.child_plan_count);
    try std.testing.expectEqual(@as(usize, 1), valid_graph.task_count);

    try expectNoDerivedGraph(captureDerivedGraph(&suite, arena, invalid.plan_id, invalid.env));
}

test "spec ingest failed apply leaves workbench status and push clean" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const fixture = createSpecIngestFixture(
        &suite,
        arena,
        "Failed Apply Workbench Cleanliness Plan",
        "# Workbench Clean Tech Spec\n\n## Status\n\nDraft.\n\n## Decisions\n\n### Keep rollback invisible\n\nFailed apply rows must not render files.\n",
        "# Workbench Clean Roadmap\n\n## Clean Milestone\n\n- Clean task [slug: clean-task]\n",
        "# Workbench Clean Test Spec\n\n## Scenarios\n\n### Scenario: Missing clean slug\n\n**Bucket:** error path\n**Verifies:** task:missing-clean-task\n",
        "workbench-failed-apply-clean",
    );

    const before_files = try listWorkbenchFiles(arena, fixture.wb_root);
    try expectNoGeneratedWorkbenchFiles(before_files);

    const stderr = suite.expectFailureWith(&.{ "spec", "ingest", fixture.plan_id, "--apply" }, fixture.env);
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "ResolveSlugFailed"));
    try expectNoDerivedGraph(captureDerivedGraph(&suite, arena, fixture.plan_id, fixture.env));

    const status = suite.execWith(&.{ "workbench", "status", fixture.plan_id, "--json" }, fixture.env);
    defer status.deinit(gpa);
    try std.testing.expect(status.term == .exited and status.term.exited == 0);
    try expectNoGeneratedWorkbenchStatusEntries(arena, status.stdout);

    const push = suite.execWith(&.{ "workbench", "push", fixture.plan_id }, fixture.env);
    defer push.deinit(gpa);
    try std.testing.expect(push.term == .exited and push.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, push.stdout, "pre-existing terminal") == null);

    const after_files = try listWorkbenchFiles(arena, fixture.wb_root);
    try std.testing.expectEqual(before_files.len, after_files.len);
    try expectNoGeneratedWorkbenchFiles(after_files);
}

fn parseJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) T {
    const trimmed = std.mem.trim(u8, buf, " \n");
    const parsed = std.json.parseFromSlice(T, arena, trimmed, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    return parsed.value;
}

fn parseNDJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) []const T {
    var rows: std.ArrayList(T) = .empty;
    var it = std.mem.splitScalar(u8, buf, '\n');
    while (it.next()) |line_raw| {
        const line = std.mem.trim(u8, line_raw, " \t\r\n");
        if (line.len == 0) continue;
        const parsed = std.json.parseFromSlice(T, arena, line, .{
            .allocate = .alloc_always,
            .ignore_unknown_fields = true,
        }) catch unreachable;
        rows.append(arena, parsed.value) catch @panic("OOM");
    }
    return rows.toOwnedSlice(arena) catch @panic("OOM");
}

const SpecIngestFixture = struct {
    plan_id: []const u8,
    env: []const harness.Suite.ExtraEnvEntry,
    wb_root: []const u8,
};

const WorkbenchEnv = struct {
    env: []const harness.Suite.ExtraEnvEntry,
    wb_root: []const u8,
};

fn createWorkbenchEnv(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    workbench_leaf: []const u8,
) WorkbenchEnv {
    const tmp_abs = suite.tmpAbsPath();
    const wb_root = std.fs.path.join(arena, &.{ tmp_abs, workbench_leaf }) catch @panic("OOM");
    const env = arena.alloc(harness.Suite.ExtraEnvEntry, 1) catch @panic("OOM");
    env[0] = .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root };
    return .{ .env = env, .wb_root = wb_root };
}

fn createSpecIngestFixture(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    title: []const u8,
    tech_body: []const u8,
    roadmap_body: []const u8,
    test_spec_body: ?[]const u8,
    workbench_leaf: []const u8,
) SpecIngestFixture {
    const wb = createWorkbenchEnv(suite, arena, workbench_leaf);
    return createSpecIngestFixtureWithEnv(suite, arena, title, tech_body, roadmap_body, test_spec_body, wb.env, wb.wb_root);
}

fn createSpecIngestFixtureWithEnv(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    title: []const u8,
    tech_body: []const u8,
    roadmap_body: []const u8,
    test_spec_body: ?[]const u8,
    env: []const harness.Suite.ExtraEnvEntry,
    wb_root: []const u8,
) SpecIngestFixture {
    const IDJSON = struct { id: i64 };
    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", title });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const tech_out = suite.mustRun(&.{
        "artifact",  "add",
        "--json",    "--kind",
        "tech_spec", "--plan",
        plan_id,     "--body",
        tech_body,   "Atomic Apply Tech Spec",
    });
    defer suite.allocator.free(tech_out);

    const roadmap_out = suite.mustRun(&.{
        "artifact",   "add",
        "--json",     "--kind",
        "roadmap",    "--plan",
        plan_id,      "--body",
        roadmap_body, "Atomic Apply Roadmap",
    });
    defer suite.allocator.free(roadmap_out);

    if (test_spec_body) |body| {
        const test_out = suite.mustRun(&.{
            "artifact",  "add",
            "--json",    "--kind",
            "test_spec", "--plan",
            plan_id,     "--body",
            body,        "Atomic Apply Test Spec",
        });
        defer suite.allocator.free(test_out);
    }

    const push_out = suite.mustRunWith(&.{ "workbench", "push", "--json", plan_id }, env);
    defer suite.allocator.free(push_out);

    return .{ .plan_id = plan_id, .env = env, .wb_root = wb_root };
}

const DerivedGraphSnapshot = struct {
    anchor_status: []const u8,
    child_plan_count: usize,
    task_count: usize,
    decision_count: usize,
    scenario_count: usize,
    anchor_non_artifact_derived_link_count: usize,
    verifies_link_count: usize,
};

fn captureDerivedGraph(
    suite: *const harness.Suite,
    arena: std.mem.Allocator,
    plan_id: []const u8,
    env: []const harness.Suite.ExtraEnvEntry,
) DerivedGraphSnapshot {
    const PlanShowJSON = struct {
        id: i64,
        status: []const u8,
    };
    const PlanRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8 = "",
    };
    const TaskRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8 = "",
    };
    const DecisionRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8 = "",
    };
    const ScenarioRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8 = "",
    };
    const LinkRow = struct {
        id: i64,
        from_kind: []const u8,
        from_id: i64,
        to_kind: []const u8,
        to_id: i64,
        relationship: []const u8,
    };

    const anchor_json = suite.mustRunWith(&.{ "plan", "show", "--json", plan_id }, env);
    defer suite.allocator.free(anchor_json);
    const anchor = parseJSON(PlanShowJSON, arena, anchor_json);

    const child_plans_json = suite.mustRunWith(&.{ "plan", "list", "--json", "--parent", plan_id }, env);
    defer suite.allocator.free(child_plans_json);
    const child_plans = parseJSON([]const PlanRow, arena, child_plans_json);

    var task_count: usize = 0;
    for (child_plans) |child| {
        const child_id = std.fmt.allocPrint(arena, "{d}", .{child.id}) catch @panic("OOM");
        const tasks_json = suite.mustRunWith(&.{ "task", "list", "--json", "--plan", child_id }, env);
        defer suite.allocator.free(tasks_json);
        const tasks = parseJSON([]const TaskRow, arena, tasks_json);
        task_count += tasks.len;
    }

    const decisions_json = suite.mustRunWith(&.{ "decision", "list", "--json", "--plan", plan_id }, env);
    defer suite.allocator.free(decisions_json);
    const decisions = parseJSON([]const DecisionRow, arena, decisions_json);

    const scenarios_json = suite.mustRunWith(&.{ "scenario", "list", "--json" }, env);
    defer suite.allocator.free(scenarios_json);
    const scenarios = parseJSON([]const ScenarioRow, arena, scenarios_json);

    const plan_ref = std.fmt.allocPrint(arena, "plan:{s}", .{plan_id}) catch @panic("OOM");
    const anchor_links_json = suite.mustRunWith(&.{ "links", "list", "--json", plan_ref }, env);
    defer suite.allocator.free(anchor_links_json);
    const anchor_links = parseNDJSON(LinkRow, arena, anchor_links_json);
    var anchor_non_artifact_derived_link_count: usize = 0;
    for (anchor_links) |link| {
        if (!std.mem.eql(u8, link.relationship, "derives-from")) continue;
        if (std.mem.eql(u8, link.from_kind, "artifact") or std.mem.eql(u8, link.to_kind, "artifact")) continue;
        anchor_non_artifact_derived_link_count += 1;
    }

    var verifies_link_count: usize = 0;
    for (scenarios) |scenario| {
        const scenario_ref = std.fmt.allocPrint(arena, "test_scenario:{d}", .{scenario.id}) catch @panic("OOM");
        const links_json = suite.mustRunWith(&.{ "links", "list", "--json", scenario_ref }, env);
        defer suite.allocator.free(links_json);
        const links = parseNDJSON(LinkRow, arena, links_json);
        for (links) |link| {
            if (std.mem.eql(u8, link.relationship, "verifies")) verifies_link_count += 1;
        }
    }

    return .{
        .anchor_status = anchor.status,
        .child_plan_count = child_plans.len,
        .task_count = task_count,
        .decision_count = decisions.len,
        .scenario_count = scenarios.len,
        .anchor_non_artifact_derived_link_count = anchor_non_artifact_derived_link_count,
        .verifies_link_count = verifies_link_count,
    };
}

fn expectNoDerivedGraph(snapshot: DerivedGraphSnapshot) !void {
    try std.testing.expectEqualStrings("draft", snapshot.anchor_status);
    try std.testing.expectEqual(@as(usize, 0), snapshot.child_plan_count);
    try std.testing.expectEqual(@as(usize, 0), snapshot.task_count);
    try std.testing.expectEqual(@as(usize, 0), snapshot.decision_count);
    try std.testing.expectEqual(@as(usize, 0), snapshot.scenario_count);
    try std.testing.expectEqual(@as(usize, 0), snapshot.anchor_non_artifact_derived_link_count);
    try std.testing.expectEqual(@as(usize, 0), snapshot.verifies_link_count);
}

fn findRoadmapPathFromPush(
    allocator: std.mem.Allocator,
    wb_root: []const u8,
    push_stdout: []const u8,
) ![]u8 {
    return findArtifactPathFromPush(allocator, wb_root, push_stdout, "roadmap");
}

fn findArtifactPathFromPush(
    allocator: std.mem.Allocator,
    wb_root: []const u8,
    push_stdout: []const u8,
    artifact_kind: []const u8,
) ![]u8 {
    const PushJSON = struct {
        entries: []const struct {
            file_path: []const u8,
            entity_kind: []const u8,
        },
    };
    const parsed = parseJSON(PushJSON, allocator, push_stdout);
    const marker = try std.fmt.allocPrint(allocator, "artifact_kind: {s}", .{artifact_kind});
    defer allocator.free(marker);

    for (parsed.entries) |entry| {
        if (!std.mem.eql(u8, entry.entity_kind, "artifact")) continue;
        const abs = if (std.fs.path.isAbsolute(entry.file_path))
            try allocator.dupe(u8, entry.file_path)
        else
            try std.fs.path.join(allocator, &.{ wb_root, entry.file_path });
        errdefer allocator.free(abs);

        const body = std.Io.Dir.cwd().readFileAlloc(
            std.testing.io,
            abs,
            allocator,
            .limited(1024 * 1024),
        ) catch {
            allocator.free(abs);
            continue;
        };
        defer allocator.free(body);
        if (std.mem.indexOf(u8, body, marker) != null) {
            return abs;
        }
        allocator.free(abs);
    }
    return error.FileNotFound;
}

fn replaceFileBlock(
    arena: std.mem.Allocator,
    path: []const u8,
    needle: []const u8,
    replacement: []const u8,
) !void {
    const before = try std.Io.Dir.cwd().readFileAlloc(
        std.testing.io,
        path,
        arena,
        .limited(2 * 1024 * 1024),
    );
    const idx = std.mem.indexOf(u8, before, needle) orelse return error.FileNotFound;
    const after = try std.mem.concat(arena, u8, &.{
        before[0..idx],
        replacement,
        before[idx + needle.len ..],
    });
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = path,
        .data = after,
    });
}

fn listWorkbenchFiles(arena: std.mem.Allocator, wb_root: []const u8) ![]const []const u8 {
    var rows: std.ArrayList([]const u8) = .empty;
    var root = try std.Io.Dir.cwd().openDir(std.testing.io, wb_root, .{ .iterate = true });
    defer root.close(std.testing.io);
    try collectWorkbenchFiles(arena, &root, "", &rows);
    return rows.toOwnedSlice(arena);
}

fn collectWorkbenchFiles(
    arena: std.mem.Allocator,
    dir: *std.Io.Dir,
    prefix: []const u8,
    rows: *std.ArrayList([]const u8),
) !void {
    var it = dir.iterate();
    while (try it.next(std.testing.io)) |entry| {
        const rel = if (prefix.len == 0)
            try arena.dupe(u8, entry.name)
        else
            try std.fs.path.join(arena, &.{ prefix, entry.name });
        switch (entry.kind) {
            .file => try rows.append(arena, rel),
            .directory => {
                var child = try dir.openDir(std.testing.io, entry.name, .{ .iterate = true });
                defer child.close(std.testing.io);
                try collectWorkbenchFiles(arena, &child, rel, rows);
            },
            else => {},
        }
    }
}

fn isGeneratedWorkbenchEntityPath(path: []const u8) bool {
    return std.mem.indexOf(u8, path, "/tasks/") != null or
        std.mem.startsWith(u8, path, "tasks/") or
        std.mem.indexOf(u8, path, "/plans/") != null or
        std.mem.startsWith(u8, path, "plans/") or
        std.mem.indexOf(u8, path, "/decisions/") != null or
        std.mem.startsWith(u8, path, "decisions/") or
        std.mem.indexOf(u8, path, "/scenarios/") != null or
        std.mem.startsWith(u8, path, "scenarios/");
}

fn expectNoGeneratedWorkbenchFiles(paths: []const []const u8) !void {
    for (paths) |path| {
        try std.testing.expect(!isGeneratedWorkbenchEntityPath(path));
    }
}

fn expectNoGeneratedWorkbenchStatusEntries(arena: std.mem.Allocator, stdout: []const u8) !void {
    const StatusJSON = struct {
        entries: []const struct {
            file_path: []const u8,
        },
    };
    const status = parseJSON(StatusJSON, arena, stdout);
    for (status.entries) |entry| {
        try std.testing.expect(!isGeneratedWorkbenchEntityPath(entry.file_path));
    }
}

test "spec ingest orphan removals stay pending without flag and cancel/abandon with flag" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp_len = suite.tmp_dir.dir.realPath(std.testing.io, &tmp_buf) catch @panic("cannot resolve tmp dir");
    const tmp_abs = tmp_buf[0..tmp_len];
    const wb_root = std.fs.path.join(arena, &.{ tmp_abs, "workbench" }) catch @panic("OOM");
    const env = &[_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    const IDJSON = struct { id: i64 };
    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Orphan Apply-Removals Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const tech_body =
        \\# Orphan Coverage Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    const roadmap_body =
        \\# Orphan Coverage Roadmap
        \\
        \\## M1 Keep
        \\
        \\- Keep Task
        \\
        \\## M2 Remove Later
        \\
        \\- Remove Task
        \\
    ;

    const tech_out = suite.mustRun(&.{
        "artifact",  "add",
        "--json",    "--kind",
        "tech_spec", "--plan",
        plan_id,     "--body",
        tech_body,   "Orphan Tech Spec",
    });
    defer gpa.free(tech_out);
    const roadmap_out = suite.mustRun(&.{
        "artifact",   "add",
        "--json",     "--kind",
        "roadmap",    "--plan",
        plan_id,      "--body",
        roadmap_body, "Orphan Roadmap",
    });
    defer gpa.free(roadmap_out);

    const push_res = suite.execWith(&.{ "workbench", "push", "--json", plan_id }, env);
    defer push_res.deinit(gpa);
    try std.testing.expect(push_res.term == .exited and push_res.term.exited == 0);
    const roadmap_path = try findRoadmapPathFromPush(arena, wb_root, push_res.stdout);

    const apply1 = suite.execWith(&.{ "spec", "ingest", plan_id, "--apply" }, env);
    defer apply1.deinit(gpa);
    try std.testing.expect(apply1.term == .exited and apply1.term.exited == 0);

    const PlanRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8 = "",
    };
    const child_plans_json = suite.mustRunWith(&.{
        "plan", "list", "--json", "--scope", "global", "--parent", plan_id,
    }, env);
    defer gpa.free(child_plans_json);
    const child_plans = parseJSON([]const PlanRow, arena, child_plans_json);
    try std.testing.expect(child_plans.len >= 2);

    var removed_plan_id: i64 = 0;
    for (child_plans) |row| {
        if (std.mem.indexOf(u8, row.title, "M2 Remove Later") != null) {
            removed_plan_id = row.id;
            break;
        }
    }
    try std.testing.expect(removed_plan_id > 0);
    const removed_plan_id_s = std.fmt.allocPrint(arena, "{d}", .{removed_plan_id}) catch @panic("OOM");

    const TaskRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8 = "",
    };
    const removed_tasks_json = suite.mustRunWith(&.{
        "task", "list", "--json", "--scope", "global", "--plan", removed_plan_id_s,
    }, env);
    defer gpa.free(removed_tasks_json);
    const removed_tasks = parseJSON([]const TaskRow, arena, removed_tasks_json);
    try std.testing.expect(removed_tasks.len >= 1);

    var removed_task_id: i64 = 0;
    for (removed_tasks) |row| {
        if (std.mem.indexOf(u8, row.title, "Remove Task") != null) {
            removed_task_id = row.id;
            break;
        }
    }
    try std.testing.expect(removed_task_id > 0);
    const removed_task_id_s = std.fmt.allocPrint(arena, "{d}", .{removed_task_id}) catch @panic("OOM");

    const roadmap_before = try std.Io.Dir.cwd().readFileAlloc(
        std.testing.io,
        roadmap_path,
        arena,
        .limited(2 * 1024 * 1024),
    );
    const remove_block =
        \\## M2 Remove Later
        \\
        \\- Remove Task
        \\
    ;
    const idx = std.mem.indexOf(u8, roadmap_before, remove_block) orelse return error.FileNotFound;
    const roadmap_after = try std.mem.concat(arena, u8, &.{
        roadmap_before[0..idx],
        roadmap_before[idx + remove_block.len ..],
    });
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = roadmap_path,
        .data = roadmap_after,
    });

    const apply2 = suite.execWith(&.{ "spec", "ingest", plan_id, "--apply" }, env);
    defer apply2.deinit(gpa);
    try std.testing.expect(apply2.term == .exited and apply2.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, apply2.stdout, 1, "proposed removals"));

    const PlanShowJSON = struct { id: i64, status: []const u8 };
    const TaskShowJSON = struct { id: i64, status: []const u8 };

    const removed_plan_before_json = suite.mustRunWith(&.{ "plan", "show", "--json", removed_plan_id_s }, env);
    defer gpa.free(removed_plan_before_json);
    const removed_plan_before = parseJSON(PlanShowJSON, arena, removed_plan_before_json);
    try std.testing.expect(!std.mem.eql(u8, removed_plan_before.status, "abandoned"));

    const removed_task_before_json = suite.mustRunWith(&.{ "task", "show", "--json", removed_task_id_s }, env);
    defer gpa.free(removed_task_before_json);
    const removed_task_before = parseJSON(TaskShowJSON, arena, removed_task_before_json);
    try std.testing.expect(!std.mem.eql(u8, removed_task_before.status, "cancelled"));

    const apply3 = suite.execWith(&.{ "spec", "ingest", plan_id, "--apply", "--apply-removals" }, env);
    defer apply3.deinit(gpa);
    try std.testing.expect(apply3.term == .exited and apply3.term.exited == 0);

    const removed_plan_after_json = suite.mustRunWith(&.{ "plan", "show", "--json", removed_plan_id_s }, env);
    defer gpa.free(removed_plan_after_json);
    const removed_plan_after = parseJSON(PlanShowJSON, arena, removed_plan_after_json);
    try std.testing.expectEqualStrings("abandoned", removed_plan_after.status);

    const removed_task_after_json = suite.mustRunWith(&.{ "task", "show", "--json", removed_task_id_s }, env);
    defer gpa.free(removed_task_after_json);
    const removed_task_after = parseJSON(TaskShowJSON, arena, removed_task_after_json);
    try std.testing.expectEqualStrings("cancelled", removed_task_after.status);
}

test "spec ingest apply-removals frees stale plan and task slugs before replacements" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp_len = suite.tmp_dir.dir.realPath(std.testing.io, &tmp_buf) catch @panic("cannot resolve tmp dir");
    const tmp_abs = tmp_buf[0..tmp_len];
    const wb_root = std.fs.path.join(arena, &.{ tmp_abs, "workbench-slug-reuse" }) catch @panic("OOM");
    const env = &[_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    const IDJSON = struct { id: i64 };
    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Slug Reuse Apply-Removals Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const tech_body =
        \\# Slug Reuse Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    const roadmap_body =
        \\# Slug Reuse Roadmap
        \\
        \\## Milestone Alpha
        \\
        \\- Old implementation [slug: shared-task]
        \\
    ;

    const tech_out = suite.mustRun(&.{
        "artifact",  "add",
        "--json",    "--kind",
        "tech_spec", "--plan",
        plan_id,     "--body",
        tech_body,   "Slug Reuse Tech Spec",
    });
    defer gpa.free(tech_out);
    const roadmap_out = suite.mustRun(&.{
        "artifact",   "add",
        "--json",     "--kind",
        "roadmap",    "--plan",
        plan_id,      "--body",
        roadmap_body, "Slug Reuse Roadmap",
    });
    defer gpa.free(roadmap_out);

    const push_res = suite.execWith(&.{ "workbench", "push", "--json", plan_id }, env);
    defer push_res.deinit(gpa);
    try std.testing.expect(push_res.term == .exited and push_res.term.exited == 0);
    const roadmap_path = try findRoadmapPathFromPush(arena, wb_root, push_res.stdout);

    const apply1 = suite.execWith(&.{ "spec", "ingest", plan_id, "--apply" }, env);
    defer apply1.deinit(gpa);
    try std.testing.expect(apply1.term == .exited and apply1.term.exited == 0);

    const PlanRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8 = "",
        slug: []const u8 = "",
    };
    const TaskRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8 = "",
        slug: ?[]const u8 = null,
    };

    const child_plans_json = suite.mustRunWith(&.{
        "plan", "list", "--json", "--scope", "global", "--parent", plan_id,
    }, env);
    defer gpa.free(child_plans_json);
    const child_plans = parseJSON([]const PlanRow, arena, child_plans_json);

    var old_plan_id: i64 = 0;
    for (child_plans) |row| {
        if (std.mem.eql(u8, row.title, "Milestone Alpha")) {
            old_plan_id = row.id;
            break;
        }
    }
    try std.testing.expect(old_plan_id > 0);
    const old_plan_id_s = std.fmt.allocPrint(arena, "{d}", .{old_plan_id}) catch @panic("OOM");

    const old_tasks_json = suite.mustRunWith(&.{
        "task", "list", "--json", "--scope", "global", "--plan", old_plan_id_s,
    }, env);
    defer gpa.free(old_tasks_json);
    const old_tasks = parseJSON([]const TaskRow, arena, old_tasks_json);
    try std.testing.expect(old_tasks.len == 1);
    const old_task_id = old_tasks[0].id;
    const old_task_id_s = std.fmt.allocPrint(arena, "{d}", .{old_task_id}) catch @panic("OOM");

    const roadmap_before = try std.Io.Dir.cwd().readFileAlloc(
        std.testing.io,
        roadmap_path,
        arena,
        .limited(2 * 1024 * 1024),
    );
    const old_block =
        \\## Milestone Alpha
        \\
        \\- Old implementation [slug: shared-task]
        \\
    ;
    const new_block =
        \\## Milestone Alpha!
        \\
        \\- New implementation [slug: shared-task]
        \\
    ;
    const idx = std.mem.indexOf(u8, roadmap_before, old_block) orelse return error.FileNotFound;
    const roadmap_after = try std.mem.concat(arena, u8, &.{
        roadmap_before[0..idx],
        new_block,
        roadmap_before[idx + old_block.len ..],
    });
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = roadmap_path,
        .data = roadmap_after,
    });

    const apply2 = suite.execWith(&.{ "spec", "ingest", plan_id, "--apply", "--apply-removals" }, env);
    defer apply2.deinit(gpa);
    try std.testing.expect(apply2.term == .exited and apply2.term.exited == 0);

    const old_plan_json = suite.mustRunWith(&.{ "plan", "show", "--json", old_plan_id_s }, env);
    defer gpa.free(old_plan_json);
    const old_plan = parseJSON(PlanRow, arena, old_plan_json);
    try std.testing.expectEqualStrings("abandoned", old_plan.status);
    try std.testing.expect(std.mem.startsWith(u8, old_plan.slug, "stale-"));

    const old_task_json = suite.mustRunWith(&.{ "task", "show", "--json", old_task_id_s }, env);
    defer gpa.free(old_task_json);
    const old_task = parseJSON(TaskRow, arena, old_task_json);
    try std.testing.expectEqualStrings("cancelled", old_task.status);
    try std.testing.expect(old_task.slug == null);

    const refreshed_plans_json = suite.mustRunWith(&.{
        "plan", "list", "--json", "--scope", "global", "--parent", plan_id,
    }, env);
    defer gpa.free(refreshed_plans_json);
    const refreshed_plans = parseJSON([]const PlanRow, arena, refreshed_plans_json);

    var new_plan_id: i64 = 0;
    for (refreshed_plans) |row| {
        if (std.mem.eql(u8, row.title, "Milestone Alpha!")) {
            new_plan_id = row.id;
            break;
        }
    }
    try std.testing.expect(new_plan_id > 0);
    const new_plan_id_s = std.fmt.allocPrint(arena, "{d}", .{new_plan_id}) catch @panic("OOM");

    const new_tasks_json = suite.mustRunWith(&.{
        "task", "list", "--json", "--scope", "global", "--plan", new_plan_id_s,
    }, env);
    defer gpa.free(new_tasks_json);
    const new_tasks = parseJSON([]const TaskRow, arena, new_tasks_json);
    try std.testing.expect(new_tasks.len == 1);
    try std.testing.expectEqualStrings("New implementation", new_tasks[0].title);
    try std.testing.expect(new_tasks[0].slug != null);
    try std.testing.expectEqualStrings("shared-task", new_tasks[0].slug.?);
}
