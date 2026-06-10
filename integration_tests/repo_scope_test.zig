//! integration_tests/repo_scope_test.zig
//!
//! Black-box coverage for first-class `repo:<slug>` planning scopes and
//! longest-project-root cwd resolution.

const std = @import("std");
const harness = @import("harness");

const InitJSON = struct {
    project_slug: []const u8,
    root_path: []const u8 = "",
};

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    scope_kind: []const u8,
    scope_id: ?i64 = null,
};

const TaskJSON = struct {
    id: i64,
    title: []const u8,
    scope_kind: []const u8,
    scope_id: ?i64 = null,
    next_action: ?[]const u8 = null,
};

const TitledJSON = struct {
    id: i64,
    title: []const u8,
};

const SearchHitJSON = struct {
    kind: []const u8,
    title: []const u8,
};

const WorkspaceInitJSON = struct {
    projects: []const struct { slug: []const u8 },
};

test "child process cwd wins over stale parent PWD for worktree gate and init" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const child_cwd = suite.freshSystemTmpDir();
    const stale_pwd = try std.fs.path.join(arena, &.{
        suite.tmpAbsPath(),
        ".worktrees",
        "stale-pwd-plan",
        "stale-pwd-task",
    });
    try mkdirp(stale_pwd);

    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PLANAR_DISABLE_WORKTREE_GATE", .value = "0" },
        .{ .key = "PWD", .value = stale_pwd },
    };

    const init = mustRunJSONWithEnvInDir(&suite, InitJSON, arena, child_cwd, &.{
        "init", "--allow-no-repo", "--json", "--name", "pwd-regression",
    }, &env);
    const real_child_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, child_cwd, std.testing.allocator);
    defer std.testing.allocator.free(real_child_cwd);
    try std.testing.expectEqualStrings(real_child_cwd, init.root_path);

    const plan = mustRunJSONWithEnvInDir(&suite, PlanJSON, arena, child_cwd, &.{
        "plan", "create", "--json", "--scope", "global", "stale PWD regression plan",
    }, &env);
    try std.testing.expectEqualStrings("global", plan.scope_kind);
}

test "repo scope create list update guard and tree surfaces" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.tmpAbsPath();
    const init_alpha = mustRunJSONInDir(&suite, InitJSON, arena, root, &.{
        "init", "--allow-no-repo", "--json", "--name", "repo-scope-alpha",
    });
    const alpha_scope = try std.fmt.allocPrint(arena, "repo:{s}", .{init_alpha.project_slug});

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--scope", alpha_scope, "Repo scoped plan",
    });
    try std.testing.expectEqualStrings("repo", plan.scope_kind);
    try std.testing.expect(plan.scope_id != null);

    const global_plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Global control plan",
    });
    try std.testing.expectEqualStrings("global", global_plan.scope_kind);

    const listed = suite.mustRunJSON([]PlanJSON, arena, &.{
        "plan", "list", "--json", "--scope", alpha_scope,
    });
    try std.testing.expectEqual(@as(usize, 1), listed.len);
    try std.testing.expectEqual(plan.id, listed[0].id);
    try std.testing.expectEqualStrings("repo", listed[0].scope_kind);

    const plan_id = try std.fmt.allocPrint(arena, "{d}", .{plan.id});
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--scope", alpha_scope, "--plan", plan_id, "Repo scoped task",
    });
    try std.testing.expectEqualStrings("repo", task.scope_kind);
    try std.testing.expectEqual(plan.scope_id.?, task.scope_id.?);

    const task_id = try std.fmt.allocPrint(arena, "{d}", .{task.id});
    const updated = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "update", "--json", "--scope", alpha_scope, "--next-action", "continue repo work", task_id,
    });
    try std.testing.expect(updated.next_action != null);
    try std.testing.expectEqualStrings("continue repo work", updated.next_action.?);

    const mismatch = suite.expectFailure(&.{
        "task", "update", "--scope", "global", "--next-action", "wrong scope", task_id,
    });
    defer gpa.free(mismatch);
    try std.testing.expect(std.mem.containsAtLeast(u8, mismatch, 1, "scope mismatch"));
    try std.testing.expect(std.mem.containsAtLeast(u8, mismatch, 1, alpha_scope));

    const tree = suite.mustRunInDir(root, &.{
        "tree", "--scope", alpha_scope,
    });
    defer gpa.free(tree);
    try std.testing.expect(std.mem.containsAtLeast(u8, tree, 1, alpha_scope));
    try std.testing.expect(std.mem.containsAtLeast(u8, tree, 1, "Repo scoped plan"));

    const unknown = suite.expectFailure(&.{
        "plan", "list", "--scope", "repo:no-such-repo",
    });
    defer gpa.free(unknown);
    try std.testing.expect(std.mem.containsAtLeast(u8, unknown, 1, "SlugNotFound"));
}

test "cwd read resolution uses longest project root for root nested and sibling workspace cases" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.freshSystemTmpDir();
    const nested = try std.fs.path.join(gpa, &.{ root, "modules", "nested" });
    defer gpa.free(nested);
    const root_src = try std.fs.path.join(gpa, &.{ root, "src" });
    defer gpa.free(root_src);
    const nested_src = try std.fs.path.join(gpa, &.{ nested, "src" });
    defer gpa.free(nested_src);
    try mkdirp(root_src);
    try mkdirp(nested_src);

    const init_root = suite.mustRunInDir(root, &.{
        "init", "--allow-no-repo", "--name", "root-repo",
    });
    gpa.free(init_root);
    const init_nested = suite.mustRunInDir(nested, &.{
        "init", "--allow-no-repo", "--name", "nested-repo",
    });
    gpa.free(init_nested);
    const assoc_root = suite.mustRun(&.{ "assoc", "create", "root-project", "--kind", "project" });
    gpa.free(assoc_root);
    const assoc_nested = suite.mustRun(&.{ "assoc", "create", "nested-project", "--kind", "project" });
    gpa.free(assoc_nested);
    const add_root = suite.mustRun(&.{ "assoc", "add", "root-project", root });
    gpa.free(add_root);
    const add_nested = suite.mustRun(&.{ "assoc", "add", "nested-project", nested });
    gpa.free(add_nested);

    const root_show = suite.mustRunInDir(root_src, &.{ "scope", "show" });
    defer gpa.free(root_show);
    try std.testing.expect(std.mem.containsAtLeast(u8, root_show, 1, "root-project"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, root_show, 1, "nested-project"));

    const nested_show = suite.mustRunInDir(nested_src, &.{ "scope", "show" });
    defer gpa.free(nested_show);
    try std.testing.expect(std.mem.containsAtLeast(u8, nested_show, 1, "nested-project"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, nested_show, 1, "root-project"));

    const ws = suite.freshSystemTmpDir();
    const home = try std.fs.path.join(gpa, &.{ ws, ".planar-home" });
    defer gpa.free(home);
    const repo_a = try std.fs.path.join(gpa, &.{ ws, "repo-a" });
    defer gpa.free(repo_a);
    const repo_a_src = try std.fs.path.join(gpa, &.{ repo_a, "src" });
    defer gpa.free(repo_a_src);
    const repo_b = try std.fs.path.join(gpa, &.{ ws, "repo-b" });
    defer gpa.free(repo_b);
    const repo_a_git = try std.fs.path.join(gpa, &.{ repo_a, ".git" });
    defer gpa.free(repo_a_git);
    const repo_b_git = try std.fs.path.join(gpa, &.{ repo_b, ".git" });
    defer gpa.free(repo_b_git);
    try mkdirp(repo_a_git);
    try mkdirp(repo_b_git);
    try mkdirp(repo_a_src);
    try mkdirp(home);

    const abs_ws = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ws, gpa);
    defer gpa.free(abs_ws);
    const abs_home = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, home, gpa);
    defer gpa.free(abs_home);
    const abs_repo_a_src = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, repo_a_src, gpa);
    defer gpa.free(abs_repo_a_src);

    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PWD", .value = abs_ws },
    };
    const init_ws = suite.execWithInDir(abs_ws, &.{
        "workspace", "init", "--slug", "sibling-ws",
    }, &env);
    defer init_ws.deinit(gpa);
    try std.testing.expect(init_ws.term == .exited and init_ws.term.exited == 0);

    const workspace_show = mustRunWithEnvInDir(&suite, abs_ws, &.{ "scope", "show" }, &env);
    defer gpa.free(workspace_show);
    try std.testing.expect(std.mem.containsAtLeast(u8, workspace_show, 1, "org:sibling-ws"));
    try std.testing.expect(std.mem.containsAtLeast(u8, workspace_show, 1, "repo:repo-a"));
    try std.testing.expect(std.mem.containsAtLeast(u8, workspace_show, 1, "repo:repo-b"));

    const repo_a_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PWD", .value = abs_repo_a_src },
    };
    const sibling_show = mustRunWithEnvInDir(&suite, abs_repo_a_src, &.{ "scope", "show" }, &repo_a_env);
    defer gpa.free(sibling_show);
    try std.testing.expect(std.mem.containsAtLeast(u8, sibling_show, 1, "repo:repo-a"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, sibling_show, 1, "org:sibling-ws"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, sibling_show, 1, "repo:repo-b"));

    const seed_plan_scopes = [_]struct { scope: []const u8, title: []const u8 }{
        .{ .scope = "assoc:sibling-ws", .title = "sibling workspace plan" },
        .{ .scope = "repo:repo-a", .title = "sibling repo-a plan" },
        .{ .scope = "repo:repo-b", .title = "sibling repo-b plan" },
        .{ .scope = "global", .title = "sibling global plan" },
    };
    for (seed_plan_scopes) |seed| {
        _ = mustRunJSONWithEnvInDir(&suite, PlanJSON, arena, abs_ws, &.{
            "plan", "create", "--json", "--scope", seed.scope, seed.title,
        }, &env);
    }
    const seed_task_scopes = [_]struct { scope: []const u8, title: []const u8 }{
        .{ .scope = "assoc:sibling-ws", .title = "sibling workspace task" },
        .{ .scope = "repo:repo-a", .title = "sibling repo-a task" },
        .{ .scope = "repo:repo-b", .title = "sibling repo-b task" },
        .{ .scope = "global", .title = "sibling global task" },
    };
    for (seed_task_scopes) |seed| {
        _ = mustRunJSONWithEnvInDir(&suite, TaskJSON, arena, abs_ws, &.{
            "task", "add", "--json", "--scope", seed.scope, seed.title,
        }, &env);
    }

    const workspace_plans = mustRunJSONWithEnvInDir(&suite, []PlanJSON, arena, abs_ws, &.{
        "plan", "list", "--json",
    }, &env);
    try expectPlanTitle(workspace_plans, "sibling workspace plan", true);
    try expectPlanTitle(workspace_plans, "sibling repo-a plan", true);
    try expectPlanTitle(workspace_plans, "sibling repo-b plan", true);
    try expectPlanTitle(workspace_plans, "sibling global plan", false);

    const workspace_tasks = mustRunJSONWithEnvInDir(&suite, []TaskJSON, arena, abs_ws, &.{
        "task", "list", "--json",
    }, &env);
    try expectTaskTitle(workspace_tasks, "sibling workspace task", true);
    try expectTaskTitle(workspace_tasks, "sibling repo-a task", true);
    try expectTaskTitle(workspace_tasks, "sibling repo-b task", true);
    try expectTaskTitle(workspace_tasks, "sibling global task", false);

    const repo_a_plans = mustRunJSONWithEnvInDir(&suite, []PlanJSON, arena, abs_repo_a_src, &.{
        "plan", "list", "--json",
    }, &repo_a_env);
    try expectPlanTitle(repo_a_plans, "sibling repo-a plan", true);
    try expectPlanTitle(repo_a_plans, "sibling repo-b plan", false);
    try expectPlanTitle(repo_a_plans, "sibling workspace plan", false);
    try expectPlanTitle(repo_a_plans, "sibling global plan", false);

    const repo_a_tasks = mustRunJSONWithEnvInDir(&suite, []TaskJSON, arena, abs_repo_a_src, &.{
        "task", "list", "--json",
    }, &repo_a_env);
    try expectTaskTitle(repo_a_tasks, "sibling repo-a task", true);
    try expectTaskTitle(repo_a_tasks, "sibling repo-b task", false);
    try expectTaskTitle(repo_a_tasks, "sibling workspace task", false);
    try expectTaskTitle(repo_a_tasks, "sibling global task", false);
}

test "plan and task list derive read scope without leaking unrelated scopes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.freshSystemTmpDir();
    const home = try std.fs.path.join(gpa, &.{ root, ".planar-home" });
    defer gpa.free(home);
    const meta = try std.fs.path.join(gpa, &.{ root, "meta-read" });
    defer gpa.free(meta);
    const meta_src = try std.fs.path.join(gpa, &.{ meta, "src" });
    defer gpa.free(meta_src);
    const nested = try std.fs.path.join(gpa, &.{ meta, "modules", "nested" });
    defer gpa.free(nested);
    const nested_src = try std.fs.path.join(gpa, &.{ nested, "src" });
    defer gpa.free(nested_src);
    const other = try std.fs.path.join(gpa, &.{ root, "other-read" });
    defer gpa.free(other);
    const outside = try std.fs.path.join(gpa, &.{ root, "outside" });
    defer gpa.free(outside);
    try mkdirp(home);
    try mkdirp(try std.fs.path.join(arena, &.{ meta, ".git" }));
    try mkdirp(meta_src);
    try mkdirp(try std.fs.path.join(arena, &.{ nested, ".git" }));
    try mkdirp(nested_src);
    try mkdirp(other);
    try mkdirp(outside);

    const abs_home = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, home, gpa);
    defer gpa.free(abs_home);
    const abs_meta = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, meta, gpa);
    defer gpa.free(abs_meta);
    const abs_meta_src = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, meta_src, gpa);
    defer gpa.free(abs_meta_src);
    const abs_nested_src = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, nested_src, gpa);
    defer gpa.free(abs_nested_src);
    const abs_other = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, other, gpa);
    defer gpa.free(abs_other);
    const abs_outside = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, outside, gpa);
    defer gpa.free(abs_outside);

    const meta_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = abs_meta },
    };
    const ws = mustRunJSONWithEnvInDir(&suite, WorkspaceInitJSON, arena, abs_meta, &.{
        "workspace", "init", "--meta-repo", "--scan", "2", "--no-scan", "--json", "--slug", "read-ws",
    }, &meta_env);
    try std.testing.expectEqual(@as(usize, 2), ws.projects.len);

    const other_init = mustRunJSONWithEnvInDir(&suite, InitJSON, arena, abs_other, &.{
        "init", "--allow-no-repo", "--json", "--name", "other-read",
    }, &.{
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = abs_other },
    });
    const other_scope = try std.fmt.allocPrint(arena, "repo:{s}", .{other_init.project_slug});

    const unrelated_out = mustRunWithEnvInDir(&suite, abs_meta, &.{ "assoc", "create", "unrelated-read", "--kind", "project" }, &meta_env);
    defer gpa.free(unrelated_out);

    const seed_plan_scopes = [_]struct { scope: []const u8, title: []const u8 }{
        .{ .scope = "assoc:read-ws", .title = "org visible plan" },
        .{ .scope = "repo:meta-read", .title = "root visible plan" },
        .{ .scope = "repo:nested", .title = "nested visible plan" },
        .{ .scope = "global", .title = "global leak plan" },
        .{ .scope = other_scope, .title = "other leak plan" },
        .{ .scope = "assoc:unrelated-read", .title = "unrelated leak plan" },
    };
    for (seed_plan_scopes) |seed| {
        _ = mustRunJSONWithEnvInDir(&suite, PlanJSON, arena, abs_meta, &.{
            "plan", "create", "--json", "--scope", seed.scope, seed.title,
        }, &meta_env);
    }

    const seed_task_scopes = [_]struct { scope: []const u8, title: []const u8 }{
        .{ .scope = "assoc:read-ws", .title = "org visible task" },
        .{ .scope = "repo:meta-read", .title = "root visible task" },
        .{ .scope = "repo:nested", .title = "nested visible task" },
        .{ .scope = "global", .title = "global leak task" },
        .{ .scope = other_scope, .title = "other leak task" },
        .{ .scope = "assoc:unrelated-read", .title = "unrelated leak task" },
    };
    for (seed_task_scopes) |seed| {
        _ = mustRunJSONWithEnvInDir(&suite, TaskJSON, arena, abs_meta, &.{
            "task", "add", "--json", "--scope", seed.scope, seed.title,
        }, &meta_env);
    }
    const seed_query_scopes = [_]struct { scope: []const u8, label: []const u8 }{
        .{ .scope = "assoc:read-ws", .label = "org visible" },
        .{ .scope = "repo:meta-read", .label = "root visible" },
        .{ .scope = "repo:nested", .label = "nested visible" },
        .{ .scope = "global", .label = "global leak" },
        .{ .scope = other_scope, .label = "other leak" },
        .{ .scope = "assoc:unrelated-read", .label = "unrelated leak" },
    };
    for (seed_query_scopes) |seed| {
        _ = mustRunJSONWithEnvInDir(&suite, TitledJSON, arena, abs_meta, &.{
            "question", "add", "--json", "--scope", seed.scope, try std.fmt.allocPrint(arena, "{s} question", .{seed.label}),
        }, &meta_env);
        _ = mustRunJSONWithEnvInDir(&suite, TitledJSON, arena, abs_meta, &.{
            "scenario", "add", "--json", "--scope", seed.scope, try std.fmt.allocPrint(arena, "{s} scenario", .{seed.label}),
        }, &meta_env);
        _ = mustRunJSONWithEnvInDir(&suite, TitledJSON, arena, abs_meta, &.{
            "decision", "add", "--json", "--scope", seed.scope, "--body", "decision body", try std.fmt.allocPrint(arena, "{s} decision", .{seed.label}),
        }, &meta_env);
        _ = mustRunJSONWithEnvInDir(&suite, TitledJSON, arena, abs_meta, &.{
            "artifact", "add", "--json", "--scope", seed.scope, "--kind", "tech_spec", try std.fmt.allocPrint(arena, "{s} artifact", .{seed.label}),
        }, &meta_env);
    }

    const meta_plans = mustRunJSONWithEnvInDir(&suite, []PlanJSON, arena, abs_meta, &.{
        "plan", "list", "--json",
    }, &meta_env);
    try expectPlanTitle(meta_plans, "org visible plan", true);
    try expectPlanTitle(meta_plans, "root visible plan", true);
    try expectPlanTitle(meta_plans, "nested visible plan", true);
    try expectPlanTitle(meta_plans, "global leak plan", false);
    try expectPlanTitle(meta_plans, "other leak plan", false);
    try expectPlanTitle(meta_plans, "unrelated leak plan", false);

    const meta_tasks = mustRunJSONWithEnvInDir(&suite, []TaskJSON, arena, abs_meta, &.{
        "task", "list", "--json",
    }, &meta_env);
    try expectTaskTitle(meta_tasks, "org visible task", true);
    try expectTaskTitle(meta_tasks, "root visible task", true);
    try expectTaskTitle(meta_tasks, "nested visible task", true);
    try expectTaskTitle(meta_tasks, "global leak task", false);
    try expectTaskTitle(meta_tasks, "other leak task", false);
    try expectTaskTitle(meta_tasks, "unrelated leak task", false);

    const meta_questions = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_meta, &.{
        "question", "list", "--json",
    }, &meta_env);
    try expectTitle(meta_questions, "org visible question", true);
    try expectTitle(meta_questions, "root visible question", true);
    try expectTitle(meta_questions, "nested visible question", true);
    try expectTitle(meta_questions, "global leak question", false);
    try expectTitle(meta_questions, "other leak question", false);
    try expectTitle(meta_questions, "unrelated leak question", false);

    const meta_scenarios = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_meta, &.{
        "scenario", "list", "--json",
    }, &meta_env);
    try expectTitle(meta_scenarios, "org visible scenario", true);
    try expectTitle(meta_scenarios, "root visible scenario", true);
    try expectTitle(meta_scenarios, "nested visible scenario", true);
    try expectTitle(meta_scenarios, "global leak scenario", false);
    try expectTitle(meta_scenarios, "other leak scenario", false);
    try expectTitle(meta_scenarios, "unrelated leak scenario", false);

    const meta_decisions = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_meta, &.{
        "decision", "list", "--json",
    }, &meta_env);
    try expectTitle(meta_decisions, "org visible decision", true);
    try expectTitle(meta_decisions, "root visible decision", true);
    try expectTitle(meta_decisions, "nested visible decision", true);
    try expectTitle(meta_decisions, "global leak decision", false);
    try expectTitle(meta_decisions, "other leak decision", false);
    try expectTitle(meta_decisions, "unrelated leak decision", false);

    const meta_artifacts = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_meta, &.{
        "artifact", "list", "--json",
    }, &meta_env);
    try expectTitle(meta_artifacts, "org visible artifact", true);
    try expectTitle(meta_artifacts, "root visible artifact", true);
    try expectTitle(meta_artifacts, "nested visible artifact", true);
    try expectTitle(meta_artifacts, "global leak artifact", false);
    try expectTitle(meta_artifacts, "other leak artifact", false);
    try expectTitle(meta_artifacts, "unrelated leak artifact", false);

    const meta_search = mustRunJSONWithEnvInDir(&suite, []SearchHitJSON, arena, abs_meta, &.{
        "search", "--json", "visible",
    }, &meta_env);
    try expectSearchTitle(meta_search, "org visible plan", true);
    try expectSearchTitle(meta_search, "root visible plan", true);
    try expectSearchTitle(meta_search, "nested visible plan", true);
    try expectSearchTitle(meta_search, "global leak plan", false);
    try expectSearchTitle(meta_search, "other leak plan", false);
    try expectSearchTitle(meta_search, "unrelated leak plan", false);

    const meta_tree = mustRunWithEnvInDir(&suite, abs_meta, &.{"tree"}, &meta_env);
    defer gpa.free(meta_tree);
    try std.testing.expect(std.mem.containsAtLeast(u8, meta_tree, 1, "org visible plan"));
    try std.testing.expect(std.mem.containsAtLeast(u8, meta_tree, 1, "root visible plan"));
    try std.testing.expect(std.mem.containsAtLeast(u8, meta_tree, 1, "nested visible plan"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, meta_tree, 1, "global leak plan"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, meta_tree, 1, "other leak plan"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, meta_tree, 1, "unrelated leak plan"));

    const meta_src_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = abs_meta_src },
    };
    const root_plans = mustRunJSONWithEnvInDir(&suite, []PlanJSON, arena, abs_meta_src, &.{
        "plan", "list", "--json",
    }, &meta_src_env);
    try expectPlanTitle(root_plans, "root visible plan", true);
    try expectPlanTitle(root_plans, "org visible plan", false);
    try expectPlanTitle(root_plans, "nested visible plan", false);

    const root_tasks = mustRunJSONWithEnvInDir(&suite, []TaskJSON, arena, abs_meta_src, &.{
        "task", "list", "--json",
    }, &meta_src_env);
    try expectTaskTitle(root_tasks, "root visible task", true);
    try expectTaskTitle(root_tasks, "org visible task", false);
    try expectTaskTitle(root_tasks, "nested visible task", false);

    const root_questions = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_meta_src, &.{
        "question", "list", "--json",
    }, &meta_src_env);
    try expectTitle(root_questions, "root visible question", true);
    try expectTitle(root_questions, "org visible question", false);
    try expectTitle(root_questions, "nested visible question", false);

    const root_scenarios = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_meta_src, &.{
        "scenario", "list", "--json",
    }, &meta_src_env);
    try expectTitle(root_scenarios, "root visible scenario", true);
    try expectTitle(root_scenarios, "org visible scenario", false);
    try expectTitle(root_scenarios, "nested visible scenario", false);

    const root_decisions = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_meta_src, &.{
        "decision", "list", "--json",
    }, &meta_src_env);
    try expectTitle(root_decisions, "root visible decision", true);
    try expectTitle(root_decisions, "org visible decision", false);
    try expectTitle(root_decisions, "nested visible decision", false);

    const root_artifacts = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_meta_src, &.{
        "artifact", "list", "--json",
    }, &meta_src_env);
    try expectTitle(root_artifacts, "root visible artifact", true);
    try expectTitle(root_artifacts, "org visible artifact", false);
    try expectTitle(root_artifacts, "nested visible artifact", false);

    const root_search = mustRunJSONWithEnvInDir(&suite, []SearchHitJSON, arena, abs_meta_src, &.{
        "search", "--json", "visible",
    }, &meta_src_env);
    try expectSearchTitle(root_search, "root visible plan", true);
    try expectSearchTitle(root_search, "org visible plan", false);
    try expectSearchTitle(root_search, "nested visible plan", false);

    const root_tree = mustRunWithEnvInDir(&suite, abs_meta_src, &.{"tree"}, &meta_src_env);
    defer gpa.free(root_tree);
    try std.testing.expect(std.mem.containsAtLeast(u8, root_tree, 1, "root visible plan"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, root_tree, 1, "org visible plan"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, root_tree, 1, "nested visible plan"));

    const nested_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = abs_nested_src },
    };
    const nested_plans = mustRunJSONWithEnvInDir(&suite, []PlanJSON, arena, abs_nested_src, &.{
        "plan", "list", "--json",
    }, &nested_env);
    try expectPlanTitle(nested_plans, "nested visible plan", true);
    try expectPlanTitle(nested_plans, "root visible plan", false);
    try expectPlanTitle(nested_plans, "org visible plan", false);

    const nested_tasks = mustRunJSONWithEnvInDir(&suite, []TaskJSON, arena, abs_nested_src, &.{
        "task", "list", "--json",
    }, &nested_env);
    try expectTaskTitle(nested_tasks, "nested visible task", true);
    try expectTaskTitle(nested_tasks, "root visible task", false);
    try expectTaskTitle(nested_tasks, "org visible task", false);

    const nested_questions = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_nested_src, &.{
        "question", "list", "--json",
    }, &nested_env);
    try expectTitle(nested_questions, "nested visible question", true);
    try expectTitle(nested_questions, "root visible question", false);
    try expectTitle(nested_questions, "org visible question", false);

    const nested_scenarios = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_nested_src, &.{
        "scenario", "list", "--json",
    }, &nested_env);
    try expectTitle(nested_scenarios, "nested visible scenario", true);
    try expectTitle(nested_scenarios, "root visible scenario", false);
    try expectTitle(nested_scenarios, "org visible scenario", false);

    const nested_decisions = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_nested_src, &.{
        "decision", "list", "--json",
    }, &nested_env);
    try expectTitle(nested_decisions, "nested visible decision", true);
    try expectTitle(nested_decisions, "root visible decision", false);
    try expectTitle(nested_decisions, "org visible decision", false);

    const nested_artifacts = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_nested_src, &.{
        "artifact", "list", "--json",
    }, &nested_env);
    try expectTitle(nested_artifacts, "nested visible artifact", true);
    try expectTitle(nested_artifacts, "root visible artifact", false);
    try expectTitle(nested_artifacts, "org visible artifact", false);

    const nested_search = mustRunJSONWithEnvInDir(&suite, []SearchHitJSON, arena, abs_nested_src, &.{
        "search", "--json", "visible",
    }, &nested_env);
    try expectSearchTitle(nested_search, "nested visible plan", true);
    try expectSearchTitle(nested_search, "root visible plan", false);
    try expectSearchTitle(nested_search, "org visible plan", false);

    const nested_tree = mustRunWithEnvInDir(&suite, abs_nested_src, &.{"tree"}, &nested_env);
    defer gpa.free(nested_tree);
    try std.testing.expect(std.mem.containsAtLeast(u8, nested_tree, 1, "nested visible plan"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, nested_tree, 1, "root visible plan"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, nested_tree, 1, "org visible plan"));

    const explicit_root_plans = mustRunJSONWithEnvInDir(&suite, []PlanJSON, arena, abs_nested_src, &.{
        "plan", "list", "--json", "--scope", "repo:meta-read",
    }, &nested_env);
    try expectPlanTitle(explicit_root_plans, "root visible plan", true);
    try expectPlanTitle(explicit_root_plans, "nested visible plan", false);

    const explicit_root_tasks = mustRunJSONWithEnvInDir(&suite, []TaskJSON, arena, abs_nested_src, &.{
        "task", "list", "--json", "--scope", "repo:meta-read",
    }, &nested_env);
    try expectTaskTitle(explicit_root_tasks, "root visible task", true);
    try expectTaskTitle(explicit_root_tasks, "nested visible task", false);

    const explicit_root_questions = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_nested_src, &.{
        "question", "list", "--json", "--scope", "repo:meta-read",
    }, &nested_env);
    try expectTitle(explicit_root_questions, "root visible question", true);
    try expectTitle(explicit_root_questions, "nested visible question", false);

    const explicit_root_scenarios = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_nested_src, &.{
        "scenario", "list", "--json", "--scope", "repo:meta-read",
    }, &nested_env);
    try expectTitle(explicit_root_scenarios, "root visible scenario", true);
    try expectTitle(explicit_root_scenarios, "nested visible scenario", false);

    const explicit_root_decisions = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_nested_src, &.{
        "decision", "list", "--json", "--scope", "repo:meta-read",
    }, &nested_env);
    try expectTitle(explicit_root_decisions, "root visible decision", true);
    try expectTitle(explicit_root_decisions, "nested visible decision", false);

    const explicit_root_artifacts = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_nested_src, &.{
        "artifact", "list", "--json", "--scope", "repo:meta-read",
    }, &nested_env);
    try expectTitle(explicit_root_artifacts, "root visible artifact", true);
    try expectTitle(explicit_root_artifacts, "nested visible artifact", false);

    const outside_env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_HOME", .value = abs_home },
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = abs_outside },
    };
    const outside_plan_fail = suite.execWithInDir(abs_outside, &.{ "plan", "list", "--json" }, &outside_env);
    defer outside_plan_fail.deinit(gpa);
    try std.testing.expect(outside_plan_fail.term == .exited and outside_plan_fail.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, outside_plan_fail.stderr, "--scope global") != null);

    const outside_task_fail = suite.execWithInDir(abs_outside, &.{ "task", "list", "--json" }, &outside_env);
    defer outside_task_fail.deinit(gpa);
    try std.testing.expect(outside_task_fail.term == .exited and outside_task_fail.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, outside_task_fail.stderr, "--scope global") != null);

    const outside_question_fail = suite.execWithInDir(abs_outside, &.{ "question", "list", "--json" }, &outside_env);
    defer outside_question_fail.deinit(gpa);
    try std.testing.expect(outside_question_fail.term == .exited and outside_question_fail.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, outside_question_fail.stderr, "--scope global") != null);

    const outside_scenario_fail = suite.execWithInDir(abs_outside, &.{ "scenario", "list", "--json" }, &outside_env);
    defer outside_scenario_fail.deinit(gpa);
    try std.testing.expect(outside_scenario_fail.term == .exited and outside_scenario_fail.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, outside_scenario_fail.stderr, "--scope global") != null);

    const outside_decision_fail = suite.execWithInDir(abs_outside, &.{ "decision", "list", "--json" }, &outside_env);
    defer outside_decision_fail.deinit(gpa);
    try std.testing.expect(outside_decision_fail.term == .exited and outside_decision_fail.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, outside_decision_fail.stderr, "--scope global") != null);

    const outside_artifact_fail = suite.execWithInDir(abs_outside, &.{ "artifact", "list", "--json" }, &outside_env);
    defer outside_artifact_fail.deinit(gpa);
    try std.testing.expect(outside_artifact_fail.term == .exited and outside_artifact_fail.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, outside_artifact_fail.stderr, "--scope global") != null);

    const global_plans = mustRunJSONWithEnvInDir(&suite, []PlanJSON, arena, abs_outside, &.{
        "plan", "list", "--json", "--scope", "global",
    }, &outside_env);
    try expectPlanTitle(global_plans, "global leak plan", true);
    try expectPlanTitle(global_plans, "root visible plan", false);

    const global_tasks = mustRunJSONWithEnvInDir(&suite, []TaskJSON, arena, abs_outside, &.{
        "task", "list", "--json", "--scope", "global",
    }, &outside_env);
    try expectTaskTitle(global_tasks, "global leak task", true);
    try expectTaskTitle(global_tasks, "root visible task", false);

    const global_questions = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_outside, &.{
        "question", "list", "--json", "--scope", "global",
    }, &outside_env);
    try expectTitle(global_questions, "global leak question", true);
    try expectTitle(global_questions, "root visible question", false);

    const global_scenarios = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_outside, &.{
        "scenario", "list", "--json", "--scope", "global",
    }, &outside_env);
    try expectTitle(global_scenarios, "global leak scenario", true);
    try expectTitle(global_scenarios, "root visible scenario", false);

    const global_decisions = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_outside, &.{
        "decision", "list", "--json", "--scope", "global",
    }, &outside_env);
    try expectTitle(global_decisions, "global leak decision", true);
    try expectTitle(global_decisions, "root visible decision", false);

    const global_artifacts = mustRunJSONWithEnvInDir(&suite, []TitledJSON, arena, abs_outside, &.{
        "artifact", "list", "--json", "--scope", "global",
    }, &outside_env);
    try expectTitle(global_artifacts, "global leak artifact", true);
    try expectTitle(global_artifacts, "root visible artifact", false);
}

fn mkdirp(path: []const u8) !void {
    try std.Io.Dir.cwd().createDirPath(std.testing.io, path);
}

fn mustRunWithEnvInDir(
    suite: *harness.Suite,
    cwd: []const u8,
    args: []const []const u8,
    extra_env: []const harness.Suite.ExtraEnvEntry,
) []u8 {
    const res = suite.execWithInDir(cwd, args, extra_env);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "\nmustRunWithEnvInDir: non-zero exit in cwd '{s}'\nstdout: {s}\nstderr: {s}\n",
            .{ cwd, res.stdout, res.stderr },
        );
        res.deinit(suite.allocator);
        @panic("mustRunWithEnvInDir: non-zero exit");
    }
    suite.allocator.free(res.stderr);
    return res.stdout;
}

fn mustRunJSONWithEnvInDir(
    suite: *harness.Suite,
    comptime T: type,
    arena: std.mem.Allocator,
    cwd: []const u8,
    args: []const []const u8,
    extra_env: []const harness.Suite.ExtraEnvEntry,
) T {
    const out = mustRunWithEnvInDir(suite, cwd, args, extra_env);
    defer suite.allocator.free(out);
    const parsed = std.json.parseFromSlice(T, arena, out, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print(
            "\nmustRunJSONWithEnvInDir: JSON decode failed: {s}\nraw: {s}\n",
            .{ @errorName(e), out },
        );
        @panic("mustRunJSONWithEnvInDir: JSON decode failed");
    };
    return parsed.value;
}

fn expectPlanTitle(plans: []const PlanJSON, title: []const u8, should_exist: bool) !void {
    for (plans) |plan| {
        if (std.mem.eql(u8, plan.title, title)) {
            try std.testing.expect(should_exist);
            return;
        }
    }
    try std.testing.expect(!should_exist);
}

fn expectTaskTitle(tasks: []const TaskJSON, title: []const u8, should_exist: bool) !void {
    for (tasks) |task| {
        if (std.mem.eql(u8, task.title, title)) {
            try std.testing.expect(should_exist);
            return;
        }
    }
    try std.testing.expect(!should_exist);
}

fn expectSearchTitle(hits: []const SearchHitJSON, title: []const u8, should_exist: bool) !void {
    for (hits) |hit| {
        if (std.mem.eql(u8, hit.title, title)) {
            try std.testing.expect(should_exist);
            return;
        }
    }
    try std.testing.expect(!should_exist);
}

fn expectTitle(items: []const TitledJSON, title: []const u8, should_exist: bool) !void {
    for (items) |item| {
        if (std.mem.eql(u8, item.title, title)) {
            try std.testing.expect(should_exist);
            return;
        }
    }
    try std.testing.expect(!should_exist);
}

fn mustRunJSONInDir(
    suite: *harness.Suite,
    comptime T: type,
    arena: std.mem.Allocator,
    cwd: []const u8,
    args: []const []const u8,
) T {
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = cwd },
    };
    const res = suite.execWithInDir(cwd, args, &env);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "\nmustRunJSONInDir: non-zero exit in cwd '{s}'\nstdout: {s}\nstderr: {s}\n",
            .{ cwd, res.stdout, res.stderr },
        );
        res.deinit(suite.allocator);
        @panic("mustRunJSONInDir: non-zero exit");
    }
    defer res.deinit(suite.allocator);

    const parsed = std.json.parseFromSlice(T, arena, res.stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print(
            "\nmustRunJSONInDir: JSON decode failed: {s}\nraw: {s}\n",
            .{ @errorName(e), res.stdout },
        );
        @panic("mustRunJSONInDir: JSON decode failed");
    };
    return parsed.value;
}
