//! integration_tests/agents_render_test.zig — `planar skills render` agent subagent output tests.
//!
//! Exercises the per-vendor agent render path: given real agent role specs under
//! <out>/agents/, the renderer emits agents/claude/<name>.md,
//! agents/codex/<name>.toml, and agents/copilot/<name>.agent.md.  The tests here
//! focus on the load-bearing security property (orchestrator has no Edit/Write in
//! its tools: list; coder does), idempotency, and --check parity.

const std = @import("std");
const harness = @import("harness");

// Render the real orchestrator + coder agent specs from the repo and assert the
// per-vendor output files exist under the staging dir.
test "agents render emits per-vendor files for canonical specialists" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "agents-render-outputs");
    defer gpa.free(root);

    const src_abs = try repoAgentsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);

    // Copy the real agent specs into <root>/agents/ so the renderer picks them up.
    try copyAgentSpecs(gpa, src_abs, root);

    const skills_src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(skills_src_abs);

    // Render with real sources.
    const stdout = suite.mustRunInDir(root, &.{
        "skills",
        "render",
        "--src",
        skills_src_abs,
        "--out",
        root,
    });
    defer gpa.free(stdout);

    // Claude: <role>.md
    try assertFileExists(gpa, root, "agents/claude/orchestrator.md");
    try assertFileExists(gpa, root, "agents/claude/coder.md");
    try assertFileExists(gpa, root, "agents/claude/doc-author.md");
    try assertFileExists(gpa, root, "agents/claude/sync-reconciler.md");
    // Codex: <role>.toml
    try assertFileExists(gpa, root, "agents/codex/orchestrator.toml");
    try assertFileExists(gpa, root, "agents/codex/coder.toml");
    try assertFileExists(gpa, root, "agents/codex/doc-author.toml");
    try assertFileExists(gpa, root, "agents/codex/sync-reconciler.toml");
    // Copilot: <role>.agent.md
    try assertFileExists(gpa, root, "agents/copilot/orchestrator.agent.md");
    try assertFileExists(gpa, root, "agents/copilot/coder.agent.md");
    try assertFileExists(gpa, root, "agents/copilot/doc-author.agent.md");
    try assertFileExists(gpa, root, "agents/copilot/sync-reconciler.agent.md");
}

test "spec-review hazard lenses render across agent and skill vendors" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "agents-render-spec-review-hazards");
    defer gpa.free(root);

    const agents_src = try repoAgentsSrcFromBin(gpa, suite.bin);
    defer gpa.free(agents_src);
    try copyAgentSpecs(gpa, agents_src, root);

    const skills_src = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(skills_src);
    const stdout = suite.mustRunInDir(root, &.{
        "skills",
        "render",
        "--src",
        skills_src,
        "--out",
        root,
    });
    defer gpa.free(stdout);

    for ([_][]const u8{
        "agents/claude/spec-reviewer.md",
        "agents/codex/spec-reviewer.toml",
        "agents/copilot/spec-reviewer.agent.md",
        "commands/claude/pl-spec-review.md",
        "skills/codex/pl-spec-review.md",
        "skills/copilot/pl-spec-review.md",
    }) |projection| {
        const rendered = try readPath(gpa, root, projection);
        defer gpa.free(rendered);
        for ([_][]const u8{
            "Hazard lens audit",
            "Resource lifecycle and cleanup",
            "Deterministic ordering and replay",
            "Concurrency, ownership, cancellation, and races",
            "Shell, build, and template escaping across interpretation boundaries",
            "artifact and missing section explicitly",
            "not applicable -- no gap",
            "build-tool-specific remedies",
            "target project's",
        }) |needle| {
            try std.testing.expect(std.mem.indexOf(u8, rendered, needle) != null);
        }
    }
}

test "guidance identity closeout renders across orchestrator and documenter agents" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "agents-render-guidance-closeout");
    defer gpa.free(root);

    const agents_src = try repoAgentsSrcFromBin(gpa, suite.bin);
    defer gpa.free(agents_src);
    try copyAgentSpecs(gpa, agents_src, root);
    const skills_src = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(skills_src);
    const stdout = suite.mustRunInDir(root, &.{ "skills", "render", "--src", skills_src, "--out", root });
    defer gpa.free(stdout);

    for ([_][]const u8{
        "agents/claude/orchestrator.md",
        "agents/codex/orchestrator.toml",
        "agents/copilot/orchestrator.agent.md",
        "agents/claude/documenter.md",
        "agents/codex/documenter.toml",
        "agents/copilot/documenter.agent.md",
    }) |projection| {
        const rendered = try readPath(gpa, root, projection);
        defer gpa.free(rendered);
        for ([_][]const u8{
            "authoritative_identity",
            "migration_tail",
            "schema_version",
            "generated_surface_boundary",
            "guidance_equivalence",
            "guidance-identity-drift",
            "operator-gated",
            "clean closeout",
            "planar-execute",
        }) |needle| try std.testing.expect(std.mem.indexOf(u8, rendered, needle) != null);
    }
}

test "cross-scope write cue and normalized mappings render across mutation surfaces and vendors" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "agents-render-cross-scope-cue");
    defer gpa.free(root);

    const src_abs = try repoAgentsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    try copyAgentSpecs(gpa, src_abs, root);

    const skills_src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(skills_src_abs);
    const stdout = suite.mustRunInDir(root, &.{
        "skills",
        "render",
        "--src",
        skills_src_abs,
        "--out",
        root,
    });
    defer gpa.free(stdout);

    const agent_paths = [_][]const u8{
        "agents/claude/coder.md",
        "agents/codex/coder.toml",
        "agents/copilot/coder.agent.md",
        "agents/claude/ext-sync.md",
        "agents/codex/ext-sync.toml",
        "agents/copilot/ext-sync.agent.md",
        "agents/claude/feedback-triager.md",
        "agents/codex/feedback-triager.toml",
        "agents/copilot/feedback-triager.agent.md",
        "agents/claude/importer.md",
        "agents/codex/importer.toml",
        "agents/copilot/importer.agent.md",
        "agents/claude/ingestor.md",
        "agents/codex/ingestor.toml",
        "agents/copilot/ingestor.agent.md",
        "agents/claude/introspector.md",
        "agents/codex/introspector.toml",
        "agents/copilot/introspector.agent.md",
        "agents/claude/janitor.md",
        "agents/codex/janitor.toml",
        "agents/copilot/janitor.agent.md",
        "agents/claude/orchestrator.md",
        "agents/codex/orchestrator.toml",
        "agents/copilot/orchestrator.agent.md",
        "agents/claude/planner.md",
        "agents/codex/planner.toml",
        "agents/copilot/planner.agent.md",
        "agents/claude/spec-reviewer.md",
        "agents/codex/spec-reviewer.toml",
        "agents/copilot/spec-reviewer.agent.md",
        "agents/claude/sync-reconciler.md",
        "agents/codex/sync-reconciler.toml",
        "agents/copilot/sync-reconciler.agent.md",
        "agents/claude/synthesizer.md",
        "agents/codex/synthesizer.toml",
        "agents/copilot/synthesizer.agent.md",
    };
    for (agent_paths) |path| {
        const rendered = try readPath(gpa, root, path);
        defer gpa.free(rendered);
        try assertCrossScopeContract(rendered);
    }

    const mutation_skills = [_][]const u8{
        "pl-doctor",
        "pl-coder",
        "pl-ext-create",
        "pl-ext-propagate",
        "pl-feedback-triage",
        "pl-import",
        "pl-introspect",
        "pl-knowledge",
        "pl-orchestrator",
        "pl-plan",
        "pl-promote",
        "pl-question",
        "pl-report-issue",
        "pl-reviewer",
        "pl-scenario",
        "pl-spec-draft",
        "pl-spec-ingest",
        "pl-spec-review",
        "pl-sync",
        "pl-synthesize",
        "pl-task",
        "pl-workbench",
        "pl-workbench-archive",
        "pl-workbench-sync",
        "pl-workspace-scan",
    };
    const vendor_dirs = [_][]const u8{
        "commands/claude",
        "skills/codex",
        "skills/copilot",
    };
    for (mutation_skills) |slug| {
        for (vendor_dirs) |vendor_dir| {
            const path = try std.fmt.allocPrint(gpa, "{s}/{s}.md", .{ vendor_dir, slug });
            defer gpa.free(path);
            const rendered = try readPath(gpa, root, path);
            defer gpa.free(rendered);
            try assertCrossScopeContract(rendered);
        }
    }

    const WorkbenchWorkflow = struct {
        slug: []const u8,
        mutations: []const []const u8,
    };
    const workbench_workflows = [_]WorkbenchWorkflow{
        .{
            .slug = "pl-workbench",
            .mutations = &.{
                "planar workbench pull <plan>",
                "planar workbench push <plan>",
                "planar workbench sync <plan>",
            },
        },
        .{
            .slug = "pl-workbench-archive",
            .mutations = &.{
                "planar workbench archive <plan>",
                "planar workbench restore <plan>",
            },
        },
        .{
            .slug = "pl-workbench-sync",
            .mutations = &.{"planar workbench sync <plan>"},
        },
    };

    for (vendor_dirs) |vendor_dir| {
        for (workbench_workflows) |workflow| {
            const path = try std.fmt.allocPrint(gpa, "{s}/{s}.md", .{ vendor_dir, workflow.slug });
            defer gpa.free(path);
            const rendered = try readPath(gpa, root, path);
            defer gpa.free(rendered);
            try assertWorkbenchTargetRule(
                rendered,
                workflow.mutations,
            );
        }

        const workspace_path = try std.fmt.allocPrint(gpa, "{s}/pl-workspace-scan.md", .{vendor_dir});
        defer gpa.free(workspace_path);
        const workspace = try readPath(gpa, root, workspace_path);
        defer gpa.free(workspace);
        try assertCommandTargetRule(
            workspace,
            "- Single workspace target:",
            "[cross-scope write: association:org:work]",
            "planar workspace routing build org:work",
            "Do not add `--scope`.",
        );
        try assertCommandTargetRule(
            workspace,
            "- All-workspaces doctor:",
            "[cross-scope write: association:org:work]",
            "planar workspace doctor --json",
            "doctor takes no target or",
        );

        const promote_path = try std.fmt.allocPrint(gpa, "{s}/pl-promote.md", .{vendor_dir});
        defer gpa.free(promote_path);
        const promote = try readPath(gpa, root, promote_path);
        defer gpa.free(promote);
        try assertCommandTargetRule(
            promote,
            "- `planar promote`/`demote`:",
            "[cross-scope write: association:org:acme]",
            "`--to` or `--from`/global-demotion form",
            "do not add `--scope`.",
        );
    }

    const read_only_paths = [_][]const u8{
        "agents/claude/reviewer.md",
        "agents/codex/reviewer.toml",
        "agents/copilot/reviewer.agent.md",
        "agents/claude/documenter.md",
        "agents/codex/documenter.toml",
        "agents/copilot/documenter.agent.md",
        "commands/claude/pl-status.md",
        "skills/codex/pl-status.md",
        "skills/copilot/pl-status.md",
    };
    for (read_only_paths) |path| {
        const rendered = try readPath(gpa, root, path);
        defer gpa.free(rendered);
        try std.testing.expect(std.mem.indexOf(u8, rendered, "## Cross-scope write cue") == null);
    }
}

fn assertCrossScopeContract(rendered: []const u8) !void {
    try std.testing.expect(std.mem.indexOf(u8, rendered, "<normalized-target-label>") != null);
    try assertCueMapping(rendered, "- Repo/project row", "[cross-scope write: project:planar]", "--scope repo:planar");
    try assertCueMapping(rendered, "- Ordinary association", "[cross-scope write: association:org:acme]", "--scope assoc:org:acme");
    try assertCueMapping(rendered, "- Legacy project association", "[cross-scope write: project:planar]", "--scope assoc:project:planar");
    try assertCueMapping(rendered, "- Global target", "[cross-scope write: global]", "--scope global");
    try std.testing.expect(std.mem.indexOf(u8, rendered, "Never emit `association:project:planar`") != null);
    try std.testing.expect(std.mem.indexOf(u8, rendered, "[cross-scope write: association:project:planar]") == null);
    try std.testing.expect(std.mem.indexOf(u8, rendered, "Same-scope writes MUST NOT emit any cross-scope cue.") != null);
}

fn assertCueMapping(rendered: []const u8, bullet_start: []const u8, cue: []const u8, cli_scope: []const u8) !void {
    const start = std.mem.indexOf(u8, rendered, bullet_start) orelse return error.TestExpectedEqual;
    const tail = rendered[start..];
    const end = std.mem.indexOfPos(u8, tail, bullet_start.len, "\n-") orelse tail.len;
    const bullet = tail[0..end];
    try std.testing.expect(std.mem.indexOf(u8, bullet, cue) != null);
    try std.testing.expect(std.mem.indexOf(u8, bullet, cli_scope) != null);
}

fn assertCommandTargetRule(
    rendered: []const u8,
    bullet_start: []const u8,
    cue_rule: []const u8,
    target_rule: []const u8,
    scope_rule: []const u8,
) !void {
    const start = std.mem.indexOf(u8, rendered, bullet_start) orelse return error.TestExpectedEqual;
    const tail = rendered[start..];
    const end = std.mem.indexOfPos(u8, tail, bullet_start.len, "\n-") orelse tail.len;
    const bullet = tail[0..end];
    try std.testing.expect(std.mem.indexOf(u8, bullet, cue_rule) != null);
    try std.testing.expect(std.mem.indexOf(u8, bullet, target_rule) != null);
    try std.testing.expect(std.mem.indexOf(u8, bullet, scope_rule) != null);
}

fn assertWorkbenchTargetRule(rendered: []const u8, mutations: []const []const u8) !void {
    try assertCommandTargetRule(
        rendered,
        "- Workbench plan target:",
        "[cross-scope write: project:planar]",
        "preserve the positional target",
        "Do not add `--scope`.",
    );
    for (mutations) |mutation| {
        try std.testing.expect(std.mem.indexOf(u8, rendered, mutation) != null);
    }
}

// Load-bearing security property: orchestrator (capability=coordinate) must NOT
// have Edit or Write in its tools list, while coder (capability=write) must have
// both, and at the sonnet model id.
test "agents render orchestrator has no Edit-Write tools coder has both at sonnet" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "agents-render-security");
    defer gpa.free(root);

    const src_abs = try repoAgentsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    try copyAgentSpecs(gpa, src_abs, root);

    const skills_src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(skills_src_abs);

    {
        const stdout = suite.mustRunInDir(root, &.{
            "skills",
            "render",
            "--src",
            skills_src_abs,
            "--out",
            root,
        });
        defer gpa.free(stdout);
    }

    // Check orchestrator (coordinate): the `tools:` frontmatter line must NOT
    // contain Edit or Write. Extract just the tools: line from the frontmatter
    // block (between the opening --- and the closing ---) to avoid false
    // positives from prose in the body that mentions "Edit/Write".
    const orch_claude = try readPath(gpa, root, "agents/claude/orchestrator.md");
    defer gpa.free(orch_claude);
    const orch_tools_line = extractToolsLine(orch_claude);
    if (orch_tools_line) |line| {
        if (std.mem.indexOf(u8, line, "Edit") != null) {
            std.debug.print(
                "\norchestrator tools: line must not contain 'Edit' but does:\n{s}\n",
                .{line},
            );
            try std.testing.expect(false);
        }
        if (std.mem.indexOf(u8, line, "Write") != null) {
            std.debug.print(
                "\norchestrator tools: line must not contain 'Write' but does:\n{s}\n",
                .{line},
            );
            try std.testing.expect(false);
        }
        // Orchestrator tools must include Agent (coordination-only).
        try std.testing.expect(std.mem.indexOf(u8, line, "Agent") != null);
    } else {
        std.debug.print("\norchestrator.md has no tools: line in frontmatter\n{s}\n", .{orch_claude});
        try std.testing.expect(false);
    }

    // Check coder (write): tools: frontmatter line must contain both Edit and Write.
    const coder_claude = try readPath(gpa, root, "agents/claude/coder.md");
    defer gpa.free(coder_claude);
    const coder_tools_line = extractToolsLine(coder_claude);
    if (coder_tools_line) |line| {
        if (std.mem.indexOf(u8, line, "Edit") == null) {
            std.debug.print("\ncoder tools: line must contain 'Edit' but does not:\n{s}\n", .{line});
            try std.testing.expect(false);
        }
        if (std.mem.indexOf(u8, line, "Write") == null) {
            std.debug.print("\ncoder tools: line must contain 'Write' but does not:\n{s}\n", .{line});
            try std.testing.expect(false);
        }
    } else {
        std.debug.print("\ncoder.md has no tools: line in frontmatter\n{s}\n", .{coder_claude});
        try std.testing.expect(false);
    }

    // Coder renders at the medium tier → claude-sonnet-5.
    // Check in the full file (model: line is in the frontmatter, unambiguous).
    try std.testing.expect(std.mem.indexOf(u8, coder_claude, "claude-sonnet-5") != null);
}

test "agents render doc-author with write capability at large tier" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "agents-render-doc-author");
    defer gpa.free(root);

    const src_abs = try repoAgentsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    try copyAgentSpecs(gpa, src_abs, root);

    const skills_src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(skills_src_abs);
    const stdout = suite.mustRunInDir(root, &.{
        "skills",
        "render",
        "--src",
        skills_src_abs,
        "--out",
        root,
    });
    defer gpa.free(stdout);

    const claude = try readPath(gpa, root, "agents/claude/doc-author.md");
    defer gpa.free(claude);
    const tools_line = extractToolsLine(claude) orelse return error.TestUnexpectedResult;
    try std.testing.expect(std.mem.indexOf(u8, tools_line, "Edit") != null);
    try std.testing.expect(std.mem.indexOf(u8, tools_line, "Write") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "model: claude-opus-4-8") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "approved_rows") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "before the first file mutation") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "`.planar-manifest`") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "Does not originate, infer, or widen operator approval") != null);

    const codex = try readPath(gpa, root, "agents/codex/doc-author.toml");
    defer gpa.free(codex);
    try std.testing.expect(std.mem.indexOf(u8, codex, "sandbox_mode = \"workspace-write\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex, "model = \"gpt-5.6-sol\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex, "model_reasoning_effort = \"high\"") != null);
}

test "agents render sync-reconciler coordinate capability at large tier" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "agents-render-sync-reconciler");
    defer gpa.free(root);

    const src_abs = try repoAgentsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    try copyAgentSpecs(gpa, src_abs, root);

    const skills_src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(skills_src_abs);
    const stdout = suite.mustRunInDir(root, &.{
        "skills",
        "render",
        "--src",
        skills_src_abs,
        "--out",
        root,
    });
    defer gpa.free(stdout);

    const claude = try readPath(gpa, root, "agents/claude/sync-reconciler.md");
    defer gpa.free(claude);
    const tools_line = extractToolsLine(claude) orelse return error.TestUnexpectedResult;
    try std.testing.expect(std.mem.indexOf(u8, tools_line, "Edit") == null);
    try std.testing.expect(std.mem.indexOf(u8, tools_line, "Write") == null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "model: claude-opus-4-8") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "`keep-local`") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "`keep-remote`") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "`manual-merge`") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "`defer`") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "only valid disposition is `defer`") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "second explicit") != null);
    try expectSyncStatusAuditBoundary(claude);
    try expectSyncReconciliationContract(claude);

    const codex = try readPath(gpa, root, "agents/codex/sync-reconciler.toml");
    defer gpa.free(codex);
    try std.testing.expect(std.mem.indexOf(u8, codex, "sandbox_mode = \"workspace-write\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex, "must NOT edit source files") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex, "model = \"gpt-5.6-sol\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex, "model_reasoning_effort = \"high\"") != null);
    try expectSyncStatusAuditBoundary(codex);
    try expectSyncReconciliationContract(codex);

    const copilot = try readPath(gpa, root, "agents/copilot/sync-reconciler.agent.md");
    defer gpa.free(copilot);
    try expectSyncStatusAuditBoundary(copilot);
    try expectSyncReconciliationContract(copilot);

    for ([_][]const u8{
        "commands/claude/pl-sync.md",
        "skills/codex/pl-sync.md",
        "skills/copilot/pl-sync.md",
    }) |rel| {
        const projection = try readPath(gpa, root, rel);
        defer gpa.free(projection);
        try expectSyncReconciliationContract(projection);
    }
}

// Idempotency: rendering twice must produce byte-identical agent output files.
test "agents render is idempotent" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "agents-render-idempotent");
    defer gpa.free(root);

    const src_abs = try repoAgentsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    try copyAgentSpecs(gpa, src_abs, root);

    const skills_src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(skills_src_abs);

    const render_args = &[_][]const u8{
        "skills", "render", "--src", skills_src_abs, "--out", root,
    };

    {
        const stdout = suite.mustRunInDir(root, render_args);
        defer gpa.free(stdout);
    }
    const snap1 = try snapshotAgentTree(gpa, root);
    defer freeSnapshot(gpa, snap1);
    {
        const stdout = suite.mustRunInDir(root, render_args);
        defer gpa.free(stdout);
    }
    const snap2 = try snapshotAgentTree(gpa, root);
    defer freeSnapshot(gpa, snap2);

    try expectSnapshotsEqual(snap1, snap2);
}

test "agents render digests are stable and change with authored input" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "agents-render-digests");
    defer gpa.free(root);

    const src_abs = try repoAgentsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    try copyAgentSpecs(gpa, src_abs, root);
    const skills_src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(skills_src_abs);
    const render_args = &[_][]const u8{
        "skills", "render", "--src", skills_src_abs, "--out", root,
    };
    {
        const stdout = suite.mustRunInDir(root, render_args);
        defer gpa.free(stdout);
    }

    const paths = [_][]const u8{
        "agents/claude/coder.md",
        "agents/codex/coder.toml",
        "agents/copilot/coder.agent.md",
    };
    var before: [paths.len][]u8 = undefined;
    defer {
        for (before) |bytes| gpa.free(bytes);
    }
    for (paths, 0..) |path, i| {
        before[i] = try readPath(gpa, root, path);
        try std.testing.expectEqual(@as(usize, 64), digestValue(before[i], "x-planar-source-digest").?.len);
        try std.testing.expectEqual(@as(usize, 64), digestValue(before[i], "x-planar-projection-digest").?.len);
        if (i > 0) try std.testing.expectEqualStrings(
            digestValue(before[0], "x-planar-source-digest").?,
            digestValue(before[i], "x-planar-source-digest").?,
        );
    }

    // Repeatability includes metadata and the vendor-specific semantic body.
    {
        const stdout = suite.mustRunInDir(root, render_args);
        defer gpa.free(stdout);
    }
    for (paths, 0..) |path, i| {
        const unchanged = try readPath(gpa, root, path);
        defer gpa.free(unchanged);
        try std.testing.expectEqualStrings(before[i], unchanged);
    }

    const coder_path = try std.fs.path.join(gpa, &.{ root, "agents", "coder.md" });
    defer gpa.free(coder_path);
    const coder_source = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, coder_path, gpa, std.Io.Limit.limited(4 * 1024 * 1024));
    defer gpa.free(coder_source);
    const changed_source = try std.fmt.allocPrint(gpa, "{s}\nChanged authored input.\n", .{coder_source});
    defer gpa.free(changed_source);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = coder_path, .data = changed_source });
    {
        const stdout = suite.mustRunInDir(root, render_args);
        defer gpa.free(stdout);
    }
    for (paths, 0..) |path, i| {
        const changed = try readPath(gpa, root, path);
        defer gpa.free(changed);
        try std.testing.expect(!std.mem.eql(
            u8,
            digestValue(before[i], "x-planar-source-digest").?,
            digestValue(changed, "x-planar-source-digest").?,
        ));
        try std.testing.expect(!std.mem.eql(
            u8,
            digestValue(before[i], "x-planar-projection-digest").?,
            digestValue(changed, "x-planar-projection-digest").?,
        ));
    }
}

// --check must pass on a freshly rendered staging dir with the real agent specs.
test "agents render check passes after render" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "agents-render-check");
    defer gpa.free(root);

    const src_abs = try repoAgentsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    try copyAgentSpecs(gpa, src_abs, root);

    const skills_src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(skills_src_abs);

    {
        const stdout = suite.mustRunInDir(root, &.{
            "skills",
            "render",
            "--src",
            skills_src_abs,
            "--out",
            root,
        });
        defer gpa.free(stdout);
    }

    // --check must exit zero (no drift) immediately after a render.
    const check_out = suite.mustRunInDir(root, &.{
        "skills",
        "render",
        "--check",
        "--src",
        skills_src_abs,
        "--out",
        root,
    });
    defer gpa.free(check_out);
}

// =========================================================================
// Helpers
// =========================================================================

fn fixtureRoot(allocator: std.mem.Allocator, tmp: *std.testing.TmpDir, name: []const u8) ![]u8 {
    const rel = try std.fs.path.join(allocator, &.{ ".zig-cache/tmp", &tmp.sub_path, name });
    defer allocator.free(rel);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, rel);
    const resolved_z = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, rel, allocator);
    defer allocator.free(resolved_z);
    return allocator.dupe(u8, resolved_z[0..resolved_z.len]);
}

// Resolve the repo's agents/ directory from the compiled binary path.
// Binary is at <repo>/zig-out/bin/planar; agents/ is at <repo>/agents/.
fn repoAgentsSrcFromBin(allocator: std.mem.Allocator, bin_path: []const u8) ![]u8 {
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    return std.fs.path.join(allocator, &.{ d3, "agents" });
}

// Resolve the repo's skills/src/ directory from the compiled binary path.
fn repoSkillsSrcFromBin(allocator: std.mem.Allocator, bin_path: []const u8) ![]u8 {
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    return std.fs.path.join(allocator, &.{ d3, "skills", "src" });
}

// Copy the real agent role specs (*.md files, excluding non-role docs that lack
// role:/capability: frontmatter) from src_agents into <root>/agents/.
// We copy only the role specs needed for these tests; non-role docs (methodology,
// models, doctrine) are left out so the renderer skips them cleanly.
fn copyAgentSpecs(allocator: std.mem.Allocator, src_agents: []const u8, root: []const u8) !void {
    const dst_agents = try std.fs.path.join(allocator, &.{ root, "agents" });
    defer allocator.free(dst_agents);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, dst_agents);

    // Non-role docs that the renderer explicitly skips; we still copy them so
    // the agents/ dir has a realistic shape but the renderer won't produce
    // vendor outputs for them (they lack role:/capability: frontmatter).
    const copy_all_md = true;
    _ = copy_all_md;

    var src_dir = try std.Io.Dir.cwd().openDir(std.testing.io, src_agents, .{ .iterate = true });
    defer src_dir.close(std.testing.io);
    var it = src_dir.iterate();
    while (try it.next(std.testing.io)) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".md")) continue;
        const src_path = try std.fs.path.join(allocator, &.{ src_agents, entry.name });
        defer allocator.free(src_path);
        const dst_path = try std.fs.path.join(allocator, &.{ dst_agents, entry.name });
        defer allocator.free(dst_path);
        const data = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, src_path, allocator, std.Io.Limit.limited(4 * 1024 * 1024));
        defer allocator.free(data);
        try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = dst_path, .data = data });
    }
}

// extractToolsLine scans the YAML frontmatter block (between the opening ---
// and the closing --- on a line by itself) and returns the slice of the content
// that starts with "tools:" (without the trailing newline). Returns null when
// no such line is found or when the frontmatter is missing.
fn extractToolsLine(content: []const u8) ?[]const u8 {
    if (!std.mem.startsWith(u8, content, "---\n")) return null;
    const rest = content[4..];
    const close = std.mem.indexOf(u8, rest, "\n---\n") orelse return null;
    const fm = rest[0..close];
    var it = std.mem.splitScalar(u8, fm, '\n');
    while (it.next()) |line| {
        if (std.mem.startsWith(u8, line, "tools:")) return line;
    }
    return null;
}

fn assertFileExists(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) !void {
    const full = try std.fs.path.join(allocator, &.{ root, rel });
    defer allocator.free(full);
    std.Io.Dir.cwd().access(std.testing.io, full, .{}) catch |e| {
        std.debug.print("\nassertFileExists: {s} not found: {s}\n", .{ rel, @errorName(e) });
        return error.FileNotFound;
    };
}

fn readPath(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) ![]u8 {
    const full = try std.fs.path.join(allocator, &.{ root, rel });
    defer allocator.free(full);
    return std.Io.Dir.cwd().readFileAlloc(std.testing.io, full, allocator, std.Io.Limit.limited(1024 * 1024 * 2));
}

fn expectSyncReconciliationContract(content: []const u8) !void {
    try std.testing.expect(std.mem.indexOf(u8, content, "planar sync status --entity <kind:id> --json") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "planar audit trail --link <link-id> --json") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "planar <kind> show <id> --json") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "`sync status` exposes current link state, not recorded sync") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "new_event_id") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "direction") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "outcome") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "detail") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "provider GET→write race") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "never retry blindly") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "entity post-state") != null);
}

fn expectSyncStatusAuditBoundary(content: []const u8) !void {
    try std.testing.expect(std.mem.indexOf(u8, content, "status contains no conflicted links or rows") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "`sync status` exposes current link state, not recorded sync events") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "`audit trail --link ... --json` as the sync-event evidence surface") != null);
    try std.testing.expect(std.mem.indexOf(u8, content, "status contains no conflict events") == null);
}

const Snap = struct { path: []const u8, body: []const u8 };

fn snapshotAgentTree(allocator: std.mem.Allocator, root: []const u8) ![]Snap {
    const dirs = [_][]const u8{ "agents/claude", "agents/codex", "agents/copilot" };
    var out = std.ArrayList(Snap).empty;
    errdefer {
        for (out.items) |s| {
            allocator.free(s.path);
            allocator.free(s.body);
        }
        out.deinit(allocator);
    }
    for (dirs) |d| {
        const abs = try std.fs.path.join(allocator, &.{ root, d });
        defer allocator.free(abs);
        var dir = std.Io.Dir.cwd().openDir(std.testing.io, abs, .{ .iterate = true }) catch |e| switch (e) {
            error.FileNotFound => continue,
            else => return e,
        };
        defer dir.close(std.testing.io);
        var it = dir.iterate();
        while (try it.next(std.testing.io)) |entry| {
            if (entry.kind != .file) continue;
            const rel = try std.fs.path.join(allocator, &.{ d, entry.name });
            const full = try std.fs.path.join(allocator, &.{ root, rel });
            defer allocator.free(full);
            const body = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, full, allocator, std.Io.Limit.limited(1024 * 1024));
            try out.append(allocator, .{ .path = rel, .body = body });
        }
    }
    std.mem.sort(Snap, out.items, {}, lessSnap);
    return out.toOwnedSlice(allocator);
}

fn lessSnap(_: void, a: Snap, b: Snap) bool {
    return std.mem.order(u8, a.path, b.path) == .lt;
}

fn freeSnapshot(allocator: std.mem.Allocator, snap: []Snap) void {
    for (snap) |s| {
        allocator.free(s.path);
        allocator.free(s.body);
    }
    allocator.free(snap);
}

fn expectSnapshotsEqual(a: []const Snap, b: []const Snap) !void {
    try std.testing.expectEqual(a.len, b.len);
    for (a, b) |left, right| {
        try std.testing.expectEqualStrings(left.path, right.path);
        try std.testing.expectEqualStrings(left.body, right.body);
    }
}

fn digestValue(bytes: []const u8, key: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |raw_line| {
        var line = std.mem.trim(u8, raw_line, " \t\r");
        if (std.mem.startsWith(u8, line, "# ")) line = std.mem.trim(u8, line[2..], " \t\r");
        if (!std.mem.startsWith(u8, line, key)) continue;
        if (line.len <= key.len or line[key.len] != ':') continue;
        return std.mem.trim(u8, line[key.len + 1 ..], " \t\r");
    }
    return null;
}
