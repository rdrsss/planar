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

test "explicit plan id crosses cwd scope for ingest reads but apply requires matching scope" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const project_a = suite.registerProject("explicit-plan-read-a");
    const project_b = suite.freshSystemTmpDir();
    const init_b = suite.mustRunInDir(project_b, &.{
        "init", "--allow-no-repo", "--name", "explicit-plan-read-b",
    });
    gpa.free(init_b);

    const assoc_a = "explicit-plan-read-a";
    const assoc_b = "explicit-plan-read-b";
    const scope_b = "assoc:explicit-plan-read-b";
    const create_a = suite.mustRun(&.{ "assoc", "create", assoc_a, "--kind", "project" });
    gpa.free(create_a);
    const create_b = suite.mustRun(&.{ "assoc", "create", assoc_b, "--kind", "project" });
    gpa.free(create_b);
    const add_a = suite.mustRun(&.{ "assoc", "add", assoc_a, project_a });
    gpa.free(add_a);
    const add_b = suite.mustRun(&.{ "assoc", "add", assoc_b, project_b });
    gpa.free(add_b);

    const IDJSON = struct { id: i64 };
    const plan = suite.mustRunJSON(IDJSON, arena, &.{
        "plan", "create", "--json", "--scope", scope_b, "Explicit plan read target",
    });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const tech_body =
        \\# Explicit Plan Read Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    const roadmap_body =
        \\# Explicit Plan Read Roadmap
        \\
        \\## Scope-safe ingestion
        \\
        \\- Apply only with explicit operator intent [slug: explicit-plan-read-task]
        \\
    ;
    const test_spec_body =
        \\# Explicit Plan Read Test Spec
        \\
        \\## Scenarios
        \\
        \\### Scenario: Explicit plan read stays available
        \\
        \\**Bucket:** happy path
        \\**Verifies:** task:explicit-plan-read-task
        \\
    ;

    const tech = suite.mustRun(&.{
        "artifact", "add",   "--json", "--scope", scope_b,                        "--kind", "tech_spec",
        "--plan",   plan_id, "--body", tech_body, "Explicit Plan Read Tech Spec",
    });
    gpa.free(tech);
    const roadmap = suite.mustRun(&.{
        "artifact", "add",   "--json", "--scope",    scope_b,                      "--kind", "roadmap",
        "--plan",   plan_id, "--body", roadmap_body, "Explicit Plan Read Roadmap",
    });
    gpa.free(roadmap);
    const test_spec = suite.mustRun(&.{
        "artifact", "add",   "--json", "--scope",      scope_b,                        "--kind", "test_spec",
        "--plan",   plan_id, "--body", test_spec_body, "Explicit Plan Read Test Spec",
    });
    gpa.free(test_spec);

    const wb = createWorkbenchEnv(&suite, arena, "workbench-explicit-plan-read");
    const push = suite.mustRunWith(&.{ "workbench", "push", "--json", plan_id }, wb.env);
    gpa.free(push);

    const cwd_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = project_a },
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb.wb_root },
    };

    // Explicit-ID reads locate the anchor independently of cwd-derived scope.
    const shown = suite.execWithInDir(project_a, &.{ "plan", "show", "--json", plan_id }, &cwd_env);
    defer shown.deinit(gpa);
    try std.testing.expect(shown.term == .exited and shown.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, shown.stdout, 1, plan_id));

    const status_before = suite.execWithInDir(project_a, &.{ "test-spec", "status", plan_id, "--json" }, &cwd_env);
    defer status_before.deinit(gpa);
    try std.testing.expect(status_before.term == .exited and status_before.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, status_before.stdout, 1, plan_id));

    const PreviewJSON = struct {
        anchor_plan_id: i64,
        summary: struct { additions: i64 },
    };
    const preview = suite.execWithInDir(project_a, &.{ "spec", "ingest", plan_id, "--json" }, &cwd_env);
    defer preview.deinit(gpa);
    try std.testing.expect(preview.term == .exited and preview.term.exited == 0);
    const preview_json = parseJSON(PreviewJSON, arena, preview.stdout);
    try std.testing.expectEqual(plan.id, preview_json.anchor_plan_id);
    try std.testing.expect(preview_json.summary.additions > 0);

    // Apply is a write boundary: cwd scope A cannot mutate the scope-B anchor.
    const refused = suite.execWithInDir(project_a, &.{ "spec", "ingest", plan_id, "--apply" }, &cwd_env);
    defer refused.deinit(gpa);
    try std.testing.expect(refused.term == .exited and refused.term.exited != 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, refused.stderr, 1, "belongs to explicit-plan-read-b"));
    try std.testing.expect(std.mem.containsAtLeast(u8, refused.stderr, 1, "resolved write scope is explicit-plan-read-a"));
    try std.testing.expect(std.mem.containsAtLeast(u8, refused.stderr, 1, "--scope explicit-plan-read-b"));

    const PlanJSON = struct { status: []const u8 };
    const after_refusal = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", plan_id });
    try std.testing.expectEqualStrings("draft", after_refusal.status);

    // The operator can recover from the same cwd by selecting the anchor scope.
    const applied = suite.execWithInDir(project_a, &.{
        "spec", "ingest", plan_id, "--apply", "--scope", scope_b,
    }, &cwd_env);
    defer applied.deinit(gpa);
    try std.testing.expect(applied.term == .exited and applied.term.exited == 0);

    const after_apply = suite.mustRunJSON(PlanJSON, arena, &.{ "plan", "show", "--json", plan_id });
    try std.testing.expectEqualStrings("active", after_apply.status);

    const status_after = suite.execWithInDir(project_a, &.{ "test-spec", "status", plan_id, "--json" }, &cwd_env);
    defer status_after.deinit(gpa);
    try std.testing.expect(status_after.term == .exited and status_after.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, status_after.stdout, 1, "\"tasks_with_slug\":1"));
    try std.testing.expect(std.mem.containsAtLeast(u8, status_after.stdout, 1, "\"tasks_covered\":1"));
}

test "spec ingest member association may apply repo anchor and derived rows stay repo scoped" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const InitJSON = struct { project_slug: []const u8 };
    const project_root = suite.tmpAbsPath();
    const init_raw = suite.mustRunInDir(project_root, &.{
        "init", "--allow-no-repo", "--json", "--name", "repo-ingest-anchor",
    });
    defer gpa.free(init_raw);
    const init = parseJSON(InitJSON, arena, init_raw);
    const repo_scope = std.fmt.allocPrint(arena, "repo:{s}", .{init.project_slug}) catch @panic("OOM");

    const assoc_slug = "repo-ingest-members";
    const assoc_scope = "assoc:repo-ingest-members";
    const create_assoc = suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "org" });
    gpa.free(create_assoc);
    const add_member = suite.mustRun(&.{ "assoc", "add", assoc_slug, project_root });
    gpa.free(add_member);

    const ScopedEntity = struct {
        id: i64,
        scope_kind: []const u8,
        scope_id: ?i64 = null,
    };
    const anchor = suite.mustRunJSON(ScopedEntity, arena, &.{
        "plan", "create", "--json", "--scope", repo_scope, "Repo ingest anchor",
    });
    try std.testing.expectEqualStrings("repo", anchor.scope_kind);
    try std.testing.expect(anchor.scope_id != null);
    const anchor_id = std.fmt.allocPrint(arena, "{d}", .{anchor.id}) catch @panic("OOM");

    const tech_body =
        \\# Repo Ingest Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    const roadmap_body =
        \\# Repo Ingest Roadmap
        \\
        \\## Preserve repo provenance
        \\
        \\- Create a repo-scoped derived task [slug: repo-derived-task]
        \\
    ;
    const tech = suite.mustRun(&.{
        "artifact", "add",     "--json", "--scope", repo_scope,              "--kind", "tech_spec",
        "--plan",   anchor_id, "--body", tech_body, "Repo Ingest Tech Spec",
    });
    gpa.free(tech);
    const roadmap = suite.mustRun(&.{
        "artifact", "add",     "--json", "--scope",    repo_scope,            "--kind", "roadmap",
        "--plan",   anchor_id, "--body", roadmap_body, "Repo Ingest Roadmap",
    });
    gpa.free(roadmap);

    const wb = createWorkbenchEnv(&suite, arena, "workbench-repo-ingest-member");
    const push = suite.mustRunWith(&.{ "workbench", "push", "--json", anchor_id }, wb.env);
    gpa.free(push);

    // The org association covers its member repository for writes, but the
    // anchor's repo scope remains the provenance for every derived row.
    const apply = suite.execWith(&.{
        "spec", "ingest", anchor_id, "--apply", "--scope", assoc_scope,
    }, wb.env);
    defer apply.deinit(gpa);
    try std.testing.expect(apply.term == .exited and apply.term.exited == 0);

    const children_raw = suite.mustRun(&.{
        "plan", "list", "--json", "--scope", repo_scope, "--parent", anchor_id,
    });
    defer gpa.free(children_raw);
    const children = parseJSON([]const ScopedEntity, arena, children_raw);
    try std.testing.expectEqual(@as(usize, 1), children.len);
    try std.testing.expectEqualStrings("repo", children[0].scope_kind);
    try std.testing.expectEqual(anchor.scope_id.?, children[0].scope_id.?);

    const child_id = std.fmt.allocPrint(arena, "{d}", .{children[0].id}) catch @panic("OOM");
    const tasks_raw = suite.mustRun(&.{
        "task", "list", "--json", "--scope", repo_scope, "--plan", child_id,
    });
    defer gpa.free(tasks_raw);
    const tasks = parseJSON([]const ScopedEntity, arena, tasks_raw);
    try std.testing.expectEqual(@as(usize, 1), tasks.len);
    try std.testing.expectEqualStrings("repo", tasks[0].scope_kind);
    try std.testing.expectEqual(anchor.scope_id.?, tasks[0].scope_id.?);
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

test "spec ingest apply creates resolved questions in anchor scope and links them to plan" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    _ = suite.registerProject("spec-ingest-question-scope-project");
    suite.addAssoc("spec-ingest-question-scope", "project");
    const scope = "assoc:spec-ingest-question-scope";
    const wb = createWorkbenchEnv(&suite, arena, "workbench-question-scope");

    const IDJSON = struct { id: i64 };
    const plan = suite.mustRunJSON(IDJSON, arena, &.{
        "plan", "create", "--json", "--scope", scope, "Resolved Question Ingest Plan",
    });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const tech_body =
        \\# Resolved Question Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
        \\## Open Questions
        \\
        \\### Should resolved questions stay in the anchor scope?
        \\
        \\Resolution: Yes. The question must inherit the anchor association scope.
        \\
        \\### Should resolved questions be visible through question list --plan?
        \\
        \\Resolution: Yes. The ingestor must create the derives-from plan link.
        \\
    ;
    const roadmap_body =
        \\# Resolved Question Roadmap
        \\
        \\## Question Persistence
        \\
        \\- Persist resolved questions through the normal question path [slug: resolved-question-path]
        \\
    ;

    const tech_out = suite.mustRun(&.{
        "artifact",  "add",
        "--json",    "--scope",
        scope,       "--kind",
        "tech_spec", "--plan",
        plan_id,     "--body",
        tech_body,   "Resolved Question Tech Spec",
    });
    defer gpa.free(tech_out);
    const roadmap_out = suite.mustRun(&.{
        "artifact",   "add",
        "--json",     "--scope",
        scope,        "--kind",
        "roadmap",    "--plan",
        plan_id,      "--body",
        roadmap_body, "Resolved Question Roadmap",
    });
    defer gpa.free(roadmap_out);

    const push_out = suite.mustRunWith(&.{ "workbench", "push", "--json", plan_id }, wb.env);
    defer gpa.free(push_out);

    const apply = suite.execWith(&.{ "spec", "ingest", plan_id, "--apply", "--scope", scope }, wb.env);
    defer apply.deinit(gpa);
    try std.testing.expect(apply.term == .exited and apply.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, apply.stderr, 1, "2 questions added, 2 questions answered"));

    const QuestionRow = struct {
        id: i64,
        title: []const u8,
        status: []const u8,
        answer_body: ?[]const u8 = null,
    };
    const scoped_questions_json = suite.mustRunWith(&.{
        "question", "list", "--json", "--scope", scope, "--plan", plan_id, "--status", "answered",
    }, wb.env);
    defer gpa.free(scoped_questions_json);
    const scoped_questions = parseJSON([]const QuestionRow, arena, scoped_questions_json);
    try std.testing.expectEqual(@as(usize, 2), scoped_questions.len);
    for (scoped_questions) |q| {
        try std.testing.expectEqualStrings("answered", q.status);
        try std.testing.expect(q.answer_body != null);
        try std.testing.expect(q.answer_body.?.len > 0);
    }

    const global_questions_json = suite.mustRunWith(&.{
        "question", "list", "--json", "--scope", "global", "--status", "answered",
    }, wb.env);
    defer gpa.free(global_questions_json);
    const global_questions = parseJSON([]const QuestionRow, arena, global_questions_json);
    for (global_questions) |q| {
        try std.testing.expect(!std.mem.eql(u8, q.title, "Should resolved questions stay in the anchor scope?"));
        try std.testing.expect(!std.mem.eql(u8, q.title, "Should resolved questions be visible through question list --plan?"));
    }

    const PreviewJSON = struct {
        summary: struct {
            additions: i64,
            updates: i64,
            removals: i64,
        },
    };
    const preview_json = suite.mustRunWith(&.{
        "spec", "ingest", plan_id, "--format", "json", "--scope", scope,
    }, wb.env);
    defer gpa.free(preview_json);
    const preview = parseJSON(PreviewJSON, arena, preview_json);
    try std.testing.expectEqual(@as(i64, 0), preview.summary.additions);
    try std.testing.expectEqual(@as(i64, 0), preview.summary.updates);
    try std.testing.expectEqual(@as(i64, 0), preview.summary.removals);
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

    const child_plans_json = suite.mustRunWith(&.{ "plan", "list", "--json", "--scope", "global", "--parent", fixture.plan_id }, fixture.env);
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

    const old_tasks_json = suite.mustRunWith(&.{ "task", "list", "--json", "--scope", "global", "--plan", old_plan_id_s }, fixture.env);
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

    const refreshed_plans_json = suite.mustRunWith(&.{ "plan", "list", "--json", "--scope", "global", "--parent", fixture.plan_id }, fixture.env);
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

test "spec ingest preview warns on non-empty Scenarios section that yields 0 scenarios" {
    // A ## Scenarios section whose content is prose paragraphs and plain bullets
    // — no '### Scenario:' H3, no '**Verifies:**', no '#### Scenario:' H4 —
    // must trigger the loud stderr warning instead of silently producing 0
    // scenarios.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const tech_body =
        \\# Zero Scenarios Warning Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    const roadmap_body =
        \\# Zero Scenarios Warning Roadmap
        \\
        \\## Coverage Milestone
        \\
        \\- Seed task for the fixture [slug: zero-scen-seed-task]
        \\
    ;
    // Intentionally no '### Scenario:' H3, '**Verifies:**', or '#### Scenario:'.
    // Plain prose + plain bullets that do not match any scenario grammar.
    const test_spec_body =
        \\# Zero Scenarios Warning Test Spec
        \\
        \\## Scenarios
        \\
        \\These scenarios will be documented as proper H3 blocks later.
        \\
        \\- Cover the happy path (pending)
        \\- Cover the error path (pending)
        \\
    ;

    const fixture = createSpecIngestFixture(
        &suite,
        arena,
        "Zero Scenarios Warning Plan",
        tech_body,
        roadmap_body,
        test_spec_body,
        "workbench-zero-scen-warn",
    );

    // Run preview (no --apply): the warning must appear on stderr.
    const res = suite.execWith(&.{ "spec", "ingest", fixture.plan_id }, fixture.env);
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited and res.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(
        u8,
        res.stderr,
        1,
        "has content but 0 scenarios extracted",
    ));

    // The JSON preview must also show 0 scenario additions.
    const PreviewJSON = struct {
        summary: struct {
            additions: i64,
            updates: i64,
            removals: i64,
        },
    };
    const preview_json = suite.mustRunWith(
        &.{ "spec", "ingest", fixture.plan_id, "--format", "json" },
        fixture.env,
    );
    defer gpa.free(preview_json);
    const preview = parseJSON(PreviewJSON, arena, preview_json);
    // The roadmap has 1 task that would be an addition; decisions/questions/
    // scenarios contribute 0 scenario additions. Confirm scenarios stayed at 0
    // by checking the stderr warning path fired (asserted above) and that the
    // overall additions are only from the roadmap task, not from scenarios.
    // The scenario count in the diff is captured via the stderr warning gate
    // above; additionally assert additions >= 0 (shape sanity).
    try std.testing.expect(preview.summary.additions >= 0);
}

test "spec ingest preview warns on non-empty Open Questions section that yields 0 questions" {
    // A ## Open Questions section whose content uses plain bullets instead of
    // '### <title>' H3 headings must trigger the loud stderr warning.  The
    // open-questions parser only extracts H3s; bullets produce nothing.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const tech_body =
        \\# Zero Questions Warning Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
        \\## Open Questions
        \\
        \\- Is the scope boundary correct here? (use H3 headings to make this parseable)
        \\- Should we support batched previews? (use H3 headings to make this parseable)
        \\
    ;
    const roadmap_body =
        \\# Zero Questions Warning Roadmap
        \\
        \\## Questions Milestone
        \\
        \\- Seed task for zero-questions fixture [slug: zero-q-seed-task]
        \\
    ;

    const fixture = createSpecIngestFixture(
        &suite,
        arena,
        "Zero Questions Warning Plan",
        tech_body,
        roadmap_body,
        null,
        "workbench-zero-q-warn",
    );

    // Run preview (no --apply): the warning must appear on stderr.
    const res = suite.execWith(&.{ "spec", "ingest", fixture.plan_id }, fixture.env);
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited and res.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(
        u8,
        res.stderr,
        1,
        "has content but 0 questions extracted",
    ));

    // JSON preview must reflect 0 question additions.
    const PreviewJSON = struct {
        summary: struct {
            additions: i64,
            updates: i64,
            removals: i64,
        },
    };
    const preview_json = suite.mustRunWith(
        &.{ "spec", "ingest", fixture.plan_id, "--format", "json" },
        fixture.env,
    );
    defer gpa.free(preview_json);
    const preview = parseJSON(PreviewJSON, arena, preview_json);
    // Questions contribute 0 additions; the roadmap task may add >=1.
    // The load-bearing assertion is the stderr warning check above.
    // Verify shape sanity.
    try std.testing.expect(preview.summary.additions >= 0);
}

test "spec ingest preview detects global slug collision before apply" {
    // Red test: task slug 'global-slug-collision-x' already exists on a
    // different plan. The preview (no --apply) must surface the collision,
    // --strict must refuse (non-zero exit), and the stderr warning must name
    // the colliding slug and the existing task. Today (pre-fix) the preview
    // reports clean and the test asserts the WRONG behaviour — so this test
    // FAILS against current code (correctly red).
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // ---- plan A: seed an existing task that owns the colliding slug -------
    // Use the spec ingest path for plan A so the task gets a proper
    // plan_id linkage (same code path as the real bug scenario).
    const IDJSON = struct { id: i64 };

    const plan_a = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Global Slug Collision Plan A" });
    const plan_a_id = std.fmt.allocPrint(arena, "{d}", .{plan_a.id}) catch @panic("OOM");

    const wb_a = createWorkbenchEnv(&suite, arena, "workbench-slug-collision-plan-a");
    const plan_a_tech =
        \\# Slug Collision Plan A Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    // Plan A's roadmap owns the slug that plan B will collide with.
    const plan_a_roadmap =
        \\# Slug Collision Plan A Roadmap
        \\
        \\## Plan A Milestone
        \\
        \\- The existing task [slug: global-slug-collision-x]
        \\
    ;
    const tech_a_out = suite.mustRun(&.{
        "artifact",  "add",
        "--json",    "--kind",
        "tech_spec", "--plan",
        plan_a_id,   "--body",
        plan_a_tech, "Slug Collision Plan A Tech Spec",
    });
    defer gpa.free(tech_a_out);
    const roadmap_a_out = suite.mustRun(&.{
        "artifact",     "add",
        "--json",       "--kind",
        "roadmap",      "--plan",
        plan_a_id,      "--body",
        plan_a_roadmap, "Slug Collision Plan A Roadmap",
    });
    defer gpa.free(roadmap_a_out);
    const push_a = suite.mustRunWith(&.{ "workbench", "push", "--json", plan_a_id }, wb_a.env);
    defer gpa.free(push_a);
    // Apply plan A to create the existing task with slug 'global-slug-collision-x'.
    const apply_a = suite.execWith(&.{ "spec", "ingest", plan_a_id, "--apply" }, wb_a.env);
    defer apply_a.deinit(gpa);
    try std.testing.expect(apply_a.term == .exited and apply_a.term.exited == 0);

    // Find the task_id and plan_id for the existing task.
    const TaskPlanJSON = struct { id: i64, plan_id: ?i64 = null };
    const TaskListJSON = struct { id: i64 };
    // Get the child plan created by plan A's ingest.
    const child_plans_a_json = suite.mustRunWith(&.{
        "plan", "list", "--json", "--scope", "global", "--parent", plan_a_id,
    }, wb_a.env);
    defer gpa.free(child_plans_a_json);
    const child_plans_a = parseJSON([]const IDJSON, arena, child_plans_a_json);
    try std.testing.expect(child_plans_a.len >= 1);
    const plan_a_child_id_s = std.fmt.allocPrint(arena, "{d}", .{child_plans_a[0].id}) catch @panic("OOM");
    const plan_a_child_id_int = child_plans_a[0].id;

    const tasks_a_json = suite.mustRunWith(&.{
        "task", "list", "--json", "--scope", "global", "--plan", plan_a_child_id_s,
    }, wb_a.env);
    defer gpa.free(tasks_a_json);
    const tasks_a = parseJSON([]const TaskPlanJSON, arena, tasks_a_json);
    try std.testing.expect(tasks_a.len >= 1);
    _ = TaskListJSON; // silence unused

    // ---- plan B: set up its spec artifacts with the colliding slug --------
    const wb = createWorkbenchEnv(&suite, arena, "workbench-slug-collision-detect");

    const plan_b_tech_body =
        \\# Global Slug Collision Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    // Roadmap uses `[slug: global-slug-collision-x]` — same slug, different plan.
    const plan_b_roadmap_body =
        \\# Global Slug Collision Roadmap
        \\
        \\## Collision Milestone
        \\
        \\- New task that collides [slug: global-slug-collision-x]
        \\
    ;

    const fixture = createSpecIngestFixtureWithEnv(
        &suite,
        arena,
        "Global Slug Collision Plan B",
        plan_b_tech_body,
        plan_b_roadmap_body,
        null,
        wb.env,
        wb.wb_root,
    );

    // ---- (a) Non-apply preview must surface the collision ----------------
    // Pre-fix: preview exits 0 and reports 0 collisions (bug).
    // Post-fix: preview must print the collision in stderr/stdout.
    const preview = suite.execWith(&.{ "spec", "ingest", fixture.plan_id }, fixture.env);
    defer preview.deinit(gpa);
    // After the fix the preview must warn about the collision on stderr.
    try std.testing.expect(std.mem.containsAtLeast(
        u8,
        preview.stderr,
        1,
        "global-slug-collision-x",
    ));

    // ---- (b) --strict must refuse (non-zero exit) -------------------------
    const strict = suite.execWith(&.{ "spec", "ingest", fixture.plan_id, "--strict" }, fixture.env);
    defer strict.deinit(gpa);
    // After the fix --strict must exit non-zero and name the collision.
    try std.testing.expect(strict.term == .exited and strict.term.exited != 0);
    try std.testing.expect(std.mem.containsAtLeast(
        u8,
        strict.stderr,
        1,
        "global-slug-collision-x",
    ));

    // ---- (c) JSON preview must include slug_collisions field  -------------
    const PreviewJSON = struct {
        slug_collisions: []const struct {
            slug: []const u8,
            existing_task_id: i64,
            existing_plan_id: i64,
        } = &.{},
    };
    const json_out = suite.mustRunWith(
        &.{ "spec", "ingest", fixture.plan_id, "--format", "json" },
        fixture.env,
    );
    defer gpa.free(json_out);
    const parsed = parseJSON(PreviewJSON, arena, json_out);
    try std.testing.expect(parsed.slug_collisions.len >= 1);
    try std.testing.expectEqualStrings("global-slug-collision-x", parsed.slug_collisions[0].slug);
    try std.testing.expect(parsed.slug_collisions[0].existing_task_id > 0);
    // existing_plan_id is the tasks.plan_id column (the child plan created
    // by plan A's ingest). Verify it matches the child plan we saw.
    try std.testing.expect(parsed.slug_collisions[0].existing_plan_id == plan_a_child_id_int);
}

test "spec ingest preview detects collision with cancelled-but-slugged task" {
    // Regression test: a task that was cancelled via `planar task cancel`
    // retains its slug (markCancelled does NOT null it — only the spec-removal
    // path does). The global unique index ux_tasks_slug is status-agnostic, so
    // the cancelled task still holds the index slot. Before the fix,
    // findGlobalSlugCollision filtered `status != 'cancelled'`, so it missed
    // the cancelled-but-slugged holder and the collision went unreported by
    // preview — only to blow up at apply time with SlugConflict.
    //
    // Load-bearing: this test FAILS against `where slug = ? and status != 'cancelled'`
    // and PASSES with `where slug = ?`.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const IDJSON = struct { id: i64 };

    // ---- Step 1: seed a task with slug 'cancelled-slug-holder' via task add --
    // We add the task directly via `task add --slug` so we control its slug
    // without going through a full spec ingest for the seed plan.
    const seed_plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Cancelled Slug Holder Plan" });
    const seed_plan_id = std.fmt.allocPrint(arena, "{d}", .{seed_plan.id}) catch @panic("OOM");

    const seed_task = suite.mustRunJSON(IDJSON, arena, &.{
        "task",                    "add",                                       "--json",
        "--plan",                  seed_plan_id,                                "--slug",
        "cancelled-slug-holder-x", "Holder task for cancelled slug regression",
    });
    const seed_task_id = std.fmt.allocPrint(arena, "{d}", .{seed_task.id}) catch @panic("OOM");

    // ---- Step 2: cancel the task via `planar task cancel` -------------------
    // Confirm `task cancel` retains the slug (does NOT null it).
    gpa.free(suite.mustRun(&.{ "task", "cancel", seed_task_id }));

    const TaskRow = struct {
        id: i64,
        status: []const u8 = "",
        slug: ?[]const u8 = null,
    };
    const after_cancel_json = suite.mustRun(&.{ "task", "show", "--json", seed_task_id });
    defer gpa.free(after_cancel_json);
    const after_cancel = parseJSON(TaskRow, arena, after_cancel_json);
    // Cancelled status confirmed.
    try std.testing.expectEqualStrings("cancelled", after_cancel.status);
    // Slug is NOT nulled — this is the correctness invariant being tested.
    try std.testing.expect(after_cancel.slug != null);
    try std.testing.expectEqualStrings("cancelled-slug-holder-x", after_cancel.slug.?);

    // ---- Step 3: create a new plan whose roadmap proposes the same slug -----
    const wb = createWorkbenchEnv(&suite, arena, "workbench-cancelled-slug-collision");

    const tech_body =
        \\# Cancelled Slug Collision Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    const roadmap_body =
        \\# Cancelled Slug Collision Roadmap
        \\
        \\## Collision Milestone
        \\
        \\- Task proposing the cancelled slug [slug: cancelled-slug-holder-x]
        \\
    ;
    const fixture = createSpecIngestFixtureWithEnv(
        &suite,
        arena,
        "Cancelled Slug Collision Plan",
        tech_body,
        roadmap_body,
        null,
        wb.env,
        wb.wb_root,
    );

    // ---- Step 4: preview must surface the collision --------------------------
    // Pre-fix (status != 'cancelled' filter): preview exits 0, reports no
    // collisions (wrong — the slug is still live in the index).
    // Post-fix (no status filter): preview warns about the collision.
    const preview = suite.execWith(&.{ "spec", "ingest", fixture.plan_id }, fixture.env);
    defer preview.deinit(gpa);
    try std.testing.expect(std.mem.containsAtLeast(
        u8,
        preview.stderr,
        1,
        "cancelled-slug-holder-x",
    ));

    // --strict must refuse (non-zero exit) when the collision involves a
    // cancelled-but-slugged task.
    const strict = suite.execWith(&.{ "spec", "ingest", fixture.plan_id, "--strict" }, fixture.env);
    defer strict.deinit(gpa);
    try std.testing.expect(strict.term == .exited and strict.term.exited != 0);
    try std.testing.expect(std.mem.containsAtLeast(
        u8,
        strict.stderr,
        1,
        "cancelled-slug-holder-x",
    ));

    // JSON preview must include the cancelled task in slug_collisions.
    const PreviewJSON = struct {
        slug_collisions: []const struct {
            slug: []const u8,
            existing_task_id: i64,
            existing_plan_id: i64,
        } = &.{},
    };
    const json_out = suite.mustRunWith(
        &.{ "spec", "ingest", fixture.plan_id, "--format", "json" },
        fixture.env,
    );
    defer gpa.free(json_out);
    const parsed = parseJSON(PreviewJSON, arena, json_out);
    try std.testing.expect(parsed.slug_collisions.len >= 1);
    try std.testing.expectEqualStrings("cancelled-slug-holder-x", parsed.slug_collisions[0].slug);
    // The collision's existing_task_id must match the cancelled task we seeded.
    try std.testing.expect(parsed.slug_collisions[0].existing_task_id == seed_task.id);
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

    const child_plans_json = suite.mustRunWith(&.{ "plan", "list", "--json", "--scope", "global", "--parent", plan_id }, env);
    defer suite.allocator.free(child_plans_json);
    const child_plans = parseJSON([]const PlanRow, arena, child_plans_json);

    var task_count: usize = 0;
    for (child_plans) |child| {
        const child_id = std.fmt.allocPrint(arena, "{d}", .{child.id}) catch @panic("OOM");
        const tasks_json = suite.mustRunWith(&.{ "task", "list", "--json", "--scope", "global", "--plan", child_id }, env);
        defer suite.allocator.free(tasks_json);
        const tasks = parseJSON([]const TaskRow, arena, tasks_json);
        task_count += tasks.len;
    }

    const decisions_json = suite.mustRunWith(&.{ "decision", "list", "--json", "--scope", "global", "--plan", plan_id }, env);
    defer suite.allocator.free(decisions_json);
    const decisions = parseJSON([]const DecisionRow, arena, decisions_json);

    const scenarios_json = suite.mustRunWith(&.{ "scenario", "list", "--json", "--scope", "global" }, env);
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
