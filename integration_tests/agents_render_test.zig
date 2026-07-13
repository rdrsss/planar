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
test "agents render emits per-vendor files for orchestrator coder and doc-author" {
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
    // Codex: <role>.toml
    try assertFileExists(gpa, root, "agents/codex/orchestrator.toml");
    try assertFileExists(gpa, root, "agents/codex/coder.toml");
    try assertFileExists(gpa, root, "agents/codex/doc-author.toml");
    // Copilot: <role>.agent.md
    try assertFileExists(gpa, root, "agents/copilot/orchestrator.agent.md");
    try assertFileExists(gpa, root, "agents/copilot/coder.agent.md");
    try assertFileExists(gpa, root, "agents/copilot/doc-author.agent.md");
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

    // Coder renders at the medium tier → claude-sonnet-4-6.
    // Check in the full file (model: line is in the frontmatter, unambiguous).
    try std.testing.expect(std.mem.indexOf(u8, coder_claude, "claude-sonnet-4-6") != null);
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
    try std.testing.expect(std.mem.indexOf(u8, codex, "model = \"gpt-5.5\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex, "model_reasoning_effort = \"high\"") != null);
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
