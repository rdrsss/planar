//! integration_tests/skills_render_test.zig — `planar skills render` parity tests.

const std = @import("std");
const harness = @import("harness");

test "skills render writes all vendor outputs" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-happy");
    defer gpa.free(root);

    try writeFixtureSource(gpa, root, "pl-alpha.md", "pl-alpha", "Alpha body");
    try writeFixtureSource(gpa, root, "pl-beta.md", "pl-beta", "Beta body");

    const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
    defer gpa.free(stdout);
    try std.testing.expectEqual(@as(usize, 8), countNonEmptyLines(stdout));

    const vendors = [_][]const u8{ "commands/claude", "skills/codex", "skills/copilot", "skills/gemini" };
    for (vendors) |v| {
        for ([_][]const u8{ "pl-alpha.md", "pl-beta.md" }) |name| {
            const p = try std.fs.path.join(gpa, &.{ root, v, name });
            defer gpa.free(p);
            try std.Io.Dir.cwd().access(std.testing.io, p, .{});
        }
    }
}

test "skills render makes orchestrator host and Codex references explicit" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-orchestrator-host");
    defer gpa.free(root);

    try writeFixtureSource(gpa, root, "pl-orchestrator.md", "pl-orchestrator", "See [agent](../../agents/orchestrator.md).\n\n{{.VendorNotes}}");
    const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
    defer gpa.free(stdout);

    const codex = try readPath(gpa, root, "skills/codex/pl-orchestrator.md");
    defer gpa.free(codex);
    try std.testing.expect(std.mem.indexOf(u8, codex, "Active host vendor: `codex`") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex, "references/agents/orchestrator.md") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex, "../../agents/orchestrator.md") == null);

    const claude = try readPath(gpa, root, "commands/claude/pl-orchestrator.md");
    defer gpa.free(claude);
    try std.testing.expect(std.mem.indexOf(u8, claude, "Active host vendor: `claude`") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude, "CLAUDE_CODE_SUBAGENT_MODEL") != null);
}

test "skills render check on missing src dir exits zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-check-missing-src");
    defer gpa.free(root);

    const stdout = mustRunInDir(&suite, root, &.{ "skills", "render", "--check" });
    defer gpa.free(stdout);
    try std.testing.expectEqual(@as(usize, 0), countNonEmptyLines(stdout));
}

test "skills render check on empty src dir exits zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-check-empty-src");
    defer gpa.free(root);
    const src_dir = try std.fs.path.join(gpa, &.{ root, "skills", "src" });
    defer gpa.free(src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);

    const stdout = mustRunInDir(&suite, root, &.{ "skills", "render", "--check" });
    defer gpa.free(stdout);
    try std.testing.expectEqual(@as(usize, 0), countNonEmptyLines(stdout));
}

test "skills render check reports missing output" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-check-missing");
    defer gpa.free(root);
    try writeFixtureSource(gpa, root, "pl-alpha.md", "pl-alpha", "Alpha body");

    const stderr = expectFailureInDir(&suite, root, &.{ "skills", "render", "--check" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "[missing]") != null);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "commands/claude/pl-alpha.md") != null);
}

test "skills render check reports orphan output" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-check-orphan");
    defer gpa.free(root);
    try writeFixtureSource(gpa, root, "pl-alpha.md", "pl-alpha", "Alpha body");
    {
        const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
        defer gpa.free(stdout);
    }
    const orphan = try std.fs.path.join(gpa, &.{ root, "commands", "claude", "pl-orphan.md" });
    defer gpa.free(orphan);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = orphan, .data = "orphan\n" });

    const stderr = expectFailureInDir(&suite, root, &.{ "skills", "render", "--check" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "[orphan]") != null);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "pl-orphan.md") != null);
}

test "skills render check content drift and diff output" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-check-content");
    defer gpa.free(root);
    try writeFixtureSource(gpa, root, "pl-alpha.md", "pl-alpha", "Alpha body");
    {
        const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
        defer gpa.free(stdout);
    }
    const tampered = try std.fs.path.join(gpa, &.{ root, "commands", "claude", "pl-alpha.md" });
    defer gpa.free(tampered);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = tampered, .data = "tampered\n" });

    const stderr = expectFailureInDir(&suite, root, &.{ "skills", "render", "--check", "--diff" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "[content] commands/claude/pl-alpha.md") != null);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "--- a/commands/claude/pl-alpha.md") != null);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "+++ b/commands/claude/pl-alpha.md") != null);
}

test "skills render slug-filtered check suppresses orphan scan" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-filtered-orphan");
    defer gpa.free(root);
    try writeFixtureSource(gpa, root, "pl-a.md", "pl-a", "A");
    try writeFixtureSource(gpa, root, "pl-b.md", "pl-b", "B");
    {
        const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
        defer gpa.free(stdout);
    }
    const orphan = try std.fs.path.join(gpa, &.{ root, "commands", "claude", "pl-extra.md" });
    defer gpa.free(orphan);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = orphan, .data = "orphan\n" });

    const stdout = mustRunInDir(&suite, root, &.{ "skills", "render", "--check", "pl-a" });
    defer gpa.free(stdout);
    try std.testing.expectEqual(@as(usize, 0), countNonEmptyLines(stdout));
}

test "skills render is idempotent" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-idempotent");
    defer gpa.free(root);
    try writeFixtureSource(gpa, root, "pl-alpha.md", "pl-alpha", "Alpha body");
    {
        const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
        defer gpa.free(stdout);
    }
    const first = try snapshotVendorTree(gpa, root);
    defer freeSnapshot(gpa, first);
    {
        const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
        defer gpa.free(stdout);
    }
    const second = try snapshotVendorTree(gpa, root);
    defer freeSnapshot(gpa, second);
    try expectSnapshotsEqual(first, second);
}

test "skills render digests are stable and change with authored input" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-digests");
    defer gpa.free(root);
    try writeFixtureSource(gpa, root, "pl-alpha.md", "pl-alpha", "Alpha body");

    {
        const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
        defer gpa.free(stdout);
    }
    const paths = [_][]const u8{
        "commands/claude/pl-alpha.md",
        "skills/codex/pl-alpha.md",
        "skills/copilot/pl-alpha.md",
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

    // A second unchanged render is byte-identical, including both digest rows.
    {
        const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
        defer gpa.free(stdout);
    }
    for (paths, 0..) |path, i| {
        const unchanged = try readPath(gpa, root, path);
        defer gpa.free(unchanged);
        try std.testing.expectEqualStrings(before[i], unchanged);
    }

    try writeFixtureSource(gpa, root, "pl-alpha.md", "pl-alpha", "Changed alpha body");
    {
        const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
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

test "skills render rewrites and checks agents models table parity" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-agents-models");
    defer gpa.free(root);
    try writeFixtureSource(gpa, root, "pl-alpha.md", "pl-alpha", "Alpha body");

    const agents = try std.fs.path.join(gpa, &.{ root, "agents" });
    defer gpa.free(agents);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, agents);
    const models = try std.fs.path.join(gpa, &.{ agents, "models.md" });
    defer gpa.free(models);
    const stale =
        \\# Models
        \\
        \\## Tier Table
        \\
        \\| Tier | Claude | Codex | Copilot |
        \\|------|--------|-------|---------|
        \\| medium | stale | stale | stale |
        \\| large | stale | stale | stale |
        \\
        \\## Agent Assignments
    ;
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = models, .data = stale });

    const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
    defer gpa.free(stdout);
    try std.testing.expect(std.mem.indexOf(u8, stdout, "agents/models.md") != null);

    const rewritten = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, models, gpa, std.Io.Limit.limited(1024 * 1024));
    defer gpa.free(rewritten);
    try std.testing.expect(std.mem.indexOf(u8, rewritten, "| ------ | ------ | ----- | ------- |") != null);
    const medium_idx = std.mem.indexOf(u8, rewritten, "| medium |");
    const large_idx = std.mem.indexOf(u8, rewritten, "| large |");
    try std.testing.expect(medium_idx != null and large_idx != null and medium_idx.? < large_idx.?);

    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = models, .data = stale });
    const stderr = expectFailureInDir(&suite, root, &.{ "skills", "render", "--check", "--diff" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "[content] agents/models.md") != null);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "--- a/agents/models.md") != null);
}

// =========================================================================
// Regression: render must be idempotent INCLUDING agents/models.md.
// Pre-fix the renderer's splitLines() returned a trailing empty string
// item for any input ending in '\n', then the line-by-line append loop
// added one '\n' per item — so agents/models.md grew by one blank line
// per render pass. The original "is idempotent" test missed this
// because its fixture had no agents/models.md so the renderer skipped
// the renderModelsDoc path. This test seeds that file explicitly.
// =========================================================================

test "skills render is idempotent for agents/models.md (regression)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-models-idempotent");
    defer gpa.free(root);
    try writeFixtureSource(gpa, root, "pl-alpha.md", "pl-alpha", "Alpha body");

    // Seed agents/models.md with the canonical shape the operator
    // would have on disk after a clean render — including a final
    // trailing newline, which is the input shape that triggered the
    // phantom-newline bug.
    const agents = try std.fs.path.join(gpa, &.{ root, "agents" });
    defer gpa.free(agents);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, agents);
    const models = try std.fs.path.join(gpa, &.{ agents, "models.md" });
    defer gpa.free(models);
    const seed =
        \\---
        \\name: models
        \\description: tier-to-model mapping.
        \\---
        \\
        \\# Models
        \\
        \\## Tier Table
        \\
        \\| Tier | Claude | Codex | Copilot |
        \\|------|--------|-------|---------|
        \\| medium | seed | seed | seed |
        \\| large | seed | seed | seed |
        \\
        \\## Agent Assignments
        \\
        \\(populated by the renderer)
        \\
    ;
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = models, .data = seed });

    // First render: brings the Tier Table to the canonical form.
    {
        const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
        defer gpa.free(stdout);
    }
    const first = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, models, gpa, std.Io.Limit.limited(1024 * 1024));
    defer gpa.free(first);

    // Second render: must produce byte-identical output. If the
    // renderer is non-idempotent (e.g. appends a phantom trailing
    // newline per pass) the snapshot diverges.
    {
        const stdout = mustRunInDir(&suite, root, &.{ "skills", "render" });
        defer gpa.free(stdout);
    }
    const second = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, models, gpa, std.Io.Limit.limited(1024 * 1024));
    defer gpa.free(second);

    if (!std.mem.eql(u8, first, second)) {
        std.debug.print(
            "\nagents/models.md is not idempotent across render passes:\n--- first ({d} bytes) ---\n{s}\n--- second ({d} bytes) ---\n{s}\n",
            .{ first.len, first, second.len, second },
        );
        try std.testing.expect(false);
    }

    // Third pass via --check must also report no drift. This locks
    // the bidirectional contract: re-render produces identical
    // output AND --check agrees the existing file matches.
    const stdout = mustRunInDir(&suite, root, &.{ "skills", "render", "--check" });
    defer gpa.free(stdout);
}

test "skills render real sources keep model tiers notes and invocation blocks" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-real-sources");
    defer gpa.free(root);

    const src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);

    {
        const stdout = mustRunInDir(&suite, root, &.{
            "skills",
            "render",
            "--src",
            src_abs,
            "--out",
            root,
            "pl-coder",
            "pl-orchestrator",
            "pl-spec-draft",
            "pl-introspect",
            "pl-synthesize",
            "pl-workspace-scan",
        });
        defer gpa.free(stdout);
    }

    const claude_coder = try readPath(gpa, root, "commands/claude/pl-coder.md");
    defer gpa.free(claude_coder);
    const codex_coder = try readPath(gpa, root, "skills/codex/pl-coder.md");
    defer gpa.free(codex_coder);
    const claude_orch = try readPath(gpa, root, "commands/claude/pl-orchestrator.md");
    defer gpa.free(claude_orch);
    const codex_orch = try readPath(gpa, root, "skills/codex/pl-orchestrator.md");
    defer gpa.free(codex_orch);
    const claude_spec_draft = try readPath(gpa, root, "commands/claude/pl-spec-draft.md");
    defer gpa.free(claude_spec_draft);
    const codex_introspect = try readPath(gpa, root, "skills/codex/pl-introspect.md");
    defer gpa.free(codex_introspect);
    // Canonical docs intentionally link these install-time projections. The
    // semantic source lint exempts only these exact links, so renderer-side
    // coverage must prove that both targets resolve out of tree.
    for ([_][]const u8{
        "commands/claude/pl-synthesize.md",
        "commands/claude/pl-workspace-scan.md",
    }) |projection| {
        const target = try std.fs.path.join(gpa, &.{ root, projection });
        defer gpa.free(target);
        try std.Io.Dir.cwd().access(std.testing.io, target, .{});
    }

    try std.testing.expect(std.mem.indexOf(u8, claude_coder, "model: claude-sonnet-5") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex_coder, "model: gpt-5.6-terra") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_coder, "## Invocation") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex_coder, "## Invocation") == null);
    try std.testing.expect(std.mem.indexOf(u8, claude_orch, "model: claude-opus-4-8") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex_orch, "model: gpt-5.6-sol") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_orch, "## Vendor Notes") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_spec_draft, "argument-hint: \"<goal>\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, claude_spec_draft, "\\\"<goal>\\\"") == null);
    try std.testing.expect(std.mem.indexOf(u8, codex_introspect, "description: 'Preview redacted usage-introspection findings, report signal coverage, and apply approved findings") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex_introspect, "association''s feedback plan.'") != null);
}

test "skills render preserves the durable boundary contract for every vendor" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-durable-boundary");
    defer gpa.free(root);

    const src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    const stdout = mustRunInDir(&suite, root, &.{
        "skills",
        "render",
        "--src",
        src_abs,
        "--out",
        root,
        "pl-orchestrator",
    });
    defer gpa.free(stdout);

    for ([_][]const u8{
        "commands/claude/pl-orchestrator.md",
        "skills/codex/pl-orchestrator.md",
        "skills/copilot/pl-orchestrator.md",
    }) |projection| {
        const rendered = try readPath(gpa, root, projection);
        defer gpa.free(rendered);
        for ([_][]const u8{
            "## Durable boundary checklist",
            "post-operation status is `todo`",
            "`doing`, or `blocked`",
            "Exclude post-operation `done` and `cancelled` tasks",
            "orchestration_checkpoint: v1",
            "iteration_scope: <coder-review|test-coder|none>",
            "planar resume validate <task-id> --json",
            "exactly `planar resume <task-id> --json`",
            "return boundary `outcome=partial`",
        }) |needle| {
            try std.testing.expect(std.mem.indexOf(u8, rendered, needle) != null);
        }
    }
}

test "skills render wires durable checkpoints into every execution strategy" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-durable-strategies");
    defer gpa.free(root);

    const src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    const stdout = mustRunInDir(&suite, root, &.{
        "skills",
        "render",
        "--src",
        src_abs,
        "--out",
        root,
        "pl-orchestrator",
    });
    defer gpa.free(stdout);

    for ([_][]const u8{
        "commands/claude/pl-orchestrator.md",
        "skills/codex/pl-orchestrator.md",
        "skills/copilot/pl-orchestrator.md",
    }) |projection| {
        const rendered = try readPath(gpa, root, projection);
        defer gpa.free(rendered);
        for ([_][]const u8{
            "### `classic` boundary and restart",
            "result: request-changes",
            "### `barrel-deferred` boundary and restart",
            "result: slice-verified",
            "### `parallel-fanout` wave boundary and restart",
            "result: wave-partial",
            "run `planar resume <task-id> --json` before",
            "does not change the terminal ritual",
        }) |needle| {
            try std.testing.expect(std.mem.indexOf(u8, rendered, needle) != null);
        }
    }
}

test "skills render preserves quota-aware provider breaker policy for every vendor" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-provider-breaker");
    defer gpa.free(root);

    const src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    const stdout = mustRunInDir(&suite, root, &.{
        "skills",
        "render",
        "--src",
        src_abs,
        "--out",
        root,
        "pl-orchestrator",
    });
    defer gpa.free(stdout);

    for ([_][]const u8{
        "commands/claude/pl-orchestrator.md",
        "skills/codex/pl-orchestrator.md",
        "skills/copilot/pl-orchestrator.md",
    }) |projection| {
        const rendered = try readPath(gpa, root, projection);
        defer gpa.free(rendered);
        for ([_][]const u8{
            "## Quota-aware bounded waves and provider circuit breakers",
            "maximum wave size",
            "`usage_limit`, `context_limit`, or `output_limit`",
            "opens only that provider's breaker",
            "continue eligible lanes on unaffected providers",
            "does **not** cancel, abort, reconcile, reclaim",
            "planar-agent reconcile --dry-run",
            "chooses that provider",
            "confirms a new maximum wave size",
            "never triggers automatic claim reconciliation",
        }) |needle| {
            try std.testing.expect(std.mem.indexOf(u8, rendered, needle) != null);
        }
    }
}

test "skills render preserves guidance identity closeout for orchestrator and documenter" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-guidance-closeout");
    defer gpa.free(root);

    const src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);
    const stdout = mustRunInDir(&suite, root, &.{
        "skills",          "render",        "--src", src_abs, "--out", root,
        "pl-orchestrator", "pl-documenter",
    });
    defer gpa.free(stdout);

    for ([_][]const u8{
        "commands/claude/pl-orchestrator.md",
        "skills/codex/pl-orchestrator.md",
        "skills/copilot/pl-orchestrator.md",
        "commands/claude/pl-documenter.md",
        "skills/codex/pl-documenter.md",
        "skills/copilot/pl-documenter.md",
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
            "clean closeout",
            "no automatic prose",
        }) |needle| try std.testing.expect(std.mem.indexOf(u8, rendered, needle) != null);
    }
}

test "skills render pl-spec-review has one authoritative cross-scope rule for every vendor" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-spec-review-cross-scope");
    defer gpa.free(root);

    const src_abs = try repoSkillsSrcFromBin(gpa, suite.bin);
    defer gpa.free(src_abs);

    {
        const stdout = mustRunInDir(&suite, root, &.{
            "skills",
            "render",
            "--src",
            src_abs,
            "--out",
            root,
            "pl-spec-review",
        });
        defer gpa.free(stdout);
    }

    for ([_][]const u8{
        "commands/claude/pl-spec-review.md",
        "skills/codex/pl-spec-review.md",
        "skills/copilot/pl-spec-review.md",
    }) |projection| {
        const rendered = try readPath(gpa, root, projection);
        defer gpa.free(rendered);
        try std.testing.expectEqual(
            @as(usize, 1),
            std.mem.count(u8, rendered, "## Cross-scope write cue"),
        );
        try std.testing.expectEqual(
            @as(usize, 1),
            std.mem.count(u8, rendered, "Before invoking a mutation, compare its target with the cwd-derived scope"),
        );
        try std.testing.expect(std.mem.indexOf(u8, rendered, "<scope-kind>:<scope-slug>") == null);
    }
}

test "skills render supports interspersed slugs and flags ordering" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-interspersed");
    defer gpa.free(root);

    try writeFixtureSource(gpa, root, "pl-a.md", "pl-a", "A");
    try writeFixtureSource(gpa, root, "pl-b.md", "pl-b", "B");
    const src_dir = try std.fs.path.join(gpa, &.{ root, "skills", "src" });
    defer gpa.free(src_dir);

    const stdout = mustRunInDir(&suite, root, &.{
        "skills",
        "render",
        "pl-a",
        "--out",
        root,
        "pl-b",
        "--src",
        src_dir,
    });
    defer gpa.free(stdout);
    try std.testing.expectEqual(@as(usize, 8), countNonEmptyLines(stdout));
}

test "skills render diff without check is usage error" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try fixtureRoot(gpa, &tmp, "skills-render-diff-usage");
    defer gpa.free(root);

    const stderr = expectFailureInDir(&suite, root, &.{ "skills", "render", "--diff" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "--diff requires --check") != null);
}

fn fixtureRoot(allocator: std.mem.Allocator, tmp: *std.testing.TmpDir, name: []const u8) ![]u8 {
    const rel = try std.fs.path.join(allocator, &.{ ".zig-cache/tmp", &tmp.sub_path, name });
    defer allocator.free(rel);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, rel);
    const resolved_z = try std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, rel, allocator);
    defer allocator.free(resolved_z);
    return allocator.dupe(u8, resolved_z[0..resolved_z.len]);
}

fn mustRunInDir(suite: *const harness.Suite, cwd: []const u8, args: []const []const u8) []u8 {
    const res = suite.execWithInDir(cwd, args, &.{});
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("\nmustRunInDir failed\nstdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
        suite.allocator.free(res.stderr);
        suite.allocator.free(res.stdout);
        @panic("mustRunInDir: non-zero exit");
    }
    suite.allocator.free(res.stderr);
    return res.stdout;
}

fn expectFailureInDir(suite: *const harness.Suite, cwd: []const u8, args: []const []const u8) []u8 {
    const res = suite.execWithInDir(cwd, args, &.{});
    if (res.term == .exited and res.term.exited == 0) {
        std.debug.print("\nexpectFailureInDir unexpectedly succeeded\nstdout: {s}\n", .{res.stdout});
        suite.allocator.free(res.stdout);
        suite.allocator.free(res.stderr);
        @panic("expectFailureInDir: command exited zero");
    }
    suite.allocator.free(res.stdout);
    return res.stderr;
}

fn writeFixtureSource(
    allocator: std.mem.Allocator,
    root: []const u8,
    name: []const u8,
    slug: []const u8,
    body: []const u8,
) !void {
    const src_dir = try std.fs.path.join(allocator, &.{ root, "skills", "src" });
    defer allocator.free(src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);
    const full = try std.fs.path.join(allocator, &.{ src_dir, name });
    defer allocator.free(full);
    const data = try std.fmt.allocPrint(
        allocator,
        "---\nslug: {s}\ndescription: fixture\nsource: docs/fixture\n---\n{s}\n",
        .{ slug, body },
    );
    defer allocator.free(data);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = full, .data = data });
}

const Snap = struct { path: []const u8, body: []const u8 };

fn snapshotVendorTree(allocator: std.mem.Allocator, root: []const u8) ![]Snap {
    const dirs = [_][]const u8{ "commands/claude", "skills/codex", "skills/copilot", "agents" };
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
            if (!std.mem.endsWith(u8, entry.name, ".md")) continue;
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

fn readPath(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) ![]u8 {
    const full = try std.fs.path.join(allocator, &.{ root, rel });
    defer allocator.free(full);
    return std.Io.Dir.cwd().readFileAlloc(std.testing.io, full, allocator, std.Io.Limit.limited(1024 * 1024 * 2));
}

fn repoSkillsSrcFromBin(allocator: std.mem.Allocator, bin_path: []const u8) ![]u8 {
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    // (flat layout) zig-out lives at the repo root; d3 already is the repo root.
    return std.fs.path.join(allocator, &.{ d3, "skills", "src" });
}

fn countNonEmptyLines(text: []const u8) usize {
    var n: usize = 0;
    var it = std.mem.splitScalar(u8, text, '\n');
    while (it.next()) |line| {
        if (std.mem.trim(u8, line, " \t\r").len > 0) n += 1;
    }
    return n;
}

fn digestValue(bytes: []const u8, key: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |line| {
        if (!std.mem.startsWith(u8, line, key)) continue;
        if (line.len <= key.len or line[key.len] != ':') continue;
        return std.mem.trim(u8, line[key.len + 1 ..], " \t\r");
    }
    return null;
}
