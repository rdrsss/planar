//! Representative M9 lifecycle coverage for the agent/skill ergonomics feature.
//!
//! Focused suites pin each verb's complete contract. These scenarios compose
//! the same public commands in the order prescribed by the rendered skills:
//! render/fresh health -> cross-vendor introspection preview -> approved triage,
//! durable knowledge -> read-only observation, approved doc maintenance, and
//! guarded sync conflict resolution.

const std = @import("std");
const harness = @import("harness");
const ext_sync = @import("../ext_sync_test.zig");
const installed_surface = @import("../installed_surface_test.zig");

const Id = struct { id: i64 };

fn envBin(name: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const value: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, value, name) and value.len > name.len and value[name.len] == '=')
            return value[name.len + 1 ..];
    }
    @panic("required integration-test binary environment variable is missing");
}

fn runSibling(suite: *const harness.Suite, env_name: []const u8, args: []const []const u8) harness.Suite.RunResult {
    const a = suite.allocator;
    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(a);
    argv.append(a, envBin(env_name)) catch @panic("OOM");
    for (args) |arg| argv.append(a, arg) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var count: usize = 0;
    while (raw[count] != null) : (count += 1) {}
    const block: std.process.Environ.PosixBlock = .{ .slice = @ptrCast(raw[0..count :null]) };
    var env = (std.process.Environ{ .block = block }).createMap(a) catch @panic("OOM");
    defer env.deinit();
    env.put("PLANAR_DB", suite.db_path) catch @panic("OOM");
    const result = std.process.run(a, std.testing.io, .{ .argv = argv.items, .environ_map = &env }) catch @panic("spawn failed");
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr };
}

fn mustRunSibling(suite: *const harness.Suite, env_name: []const u8, args: []const []const u8) []u8 {
    const result = runSibling(suite, env_name, args);
    suite.allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("{s} failed: {any}\n{s}\n", .{ env_name, result.term, result.stdout });
        @panic("sibling binary failed");
    }
    return result.stdout;
}

fn writeAbsolute(path: []const u8, data: []const u8) !void {
    if (std.fs.path.dirname(path)) |parent| try std.Io.Dir.cwd().createDirPath(std.testing.io, parent);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = data });
}

test "scenario: fresh managed install is healthy and status checks are read-only" {
    try installed_surface.runSelectedVendorInstallerLifecycle();
}

test "scenario: rendered introspection lifecycle previews three vendors before approved triage" {
    const a = std.testing.allocator;
    var suite = harness.Suite.init(a);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(a);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    _ = suite.registerProject("m9-introspect-triage");

    const root = suite.tmpAbsPath();
    const rendered = try std.fs.path.join(arena, &.{ root, "rendered" });
    var src_buf: [std.fs.max_path_bytes]u8 = undefined;
    var src_dir = try std.Io.Dir.cwd().openDir(std.testing.io, "skills/src", .{});
    defer src_dir.close(std.testing.io);
    const src_len = try src_dir.realPath(std.testing.io, &src_buf);
    const src = src_buf[0..src_len];
    a.free(suite.mustRun(&.{ "skills", "render", "--src", src, "--out", rendered, "pl-introspect", "pl-feedback-triage", "pl-knowledge", "pl-observe", "pl-doc-maintain", "pl-sync" }));
    for ([_][]const u8{
        "commands/claude/pl-introspect.md",
        "skills/codex/pl-feedback-triage.md",
        "skills/copilot/pl-sync.md",
    }) |relative| {
        const path = try std.fs.path.join(arena, &.{ rendered, relative });
        try std.Io.Dir.cwd().access(std.testing.io, path, .{});
    }

    const home = try std.fs.path.join(arena, &.{ root, "home" });
    const config = try std.fs.path.join(arena, &.{ root, "config.toml" });
    const claude = try std.fs.path.join(arena, &.{ root, "claude.jsonl" });
    const codex = try std.fs.path.join(arena, &.{ root, "codex.jsonl" });
    const copilot = try std.fs.path.join(arena, &.{ root, "copilot.jsonl" });
    try writeAbsolute(claude, "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-13T12:01:00Z\",\"tool\":{\"name\":\"planar task add\",\"input\":{\"body\":\"PRIVATE\"}},\"exit_code\":1}\n");
    try writeAbsolute(codex, "{\"schema_version\":1,\"event\":\"command_execution\",\"timestamp\":\"2026-07-13T12:02:00Z\",\"command_name\":\"planar plan show\",\"arguments\":[\"PRIVATE\"],\"exit_code\":1,\"retry_of_previous\":true}\n");
    try writeAbsolute(copilot, "{\"version\":\"1\",\"kind\":\"shell_result\",\"time\":\"2026-07-13T12:03:00Z\",\"command\":{\"name\":\"planar sync pull\",\"args\":[\"PRIVATE\"]},\"exit_code\":0,\"status\":\"abandoned\"}\n");
    const cfg = try std.fmt.allocPrint(arena, "[introspection]\ncli_log = false\n[introspection.transcripts]\nclaude_path = \"{s}\"\ncodex_path = \"{s}\"\ncopilot_path = \"{s}\"\n", .{ claude, codex, copilot });
    try writeAbsolute(config, cfg);
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "HOME", .value = home },
        .{ .key = "PLANAR_CONFIG_PATH", .value = config },
        .{ .key = "PLANAR_HOME", .value = try std.fs.path.join(arena, &.{ home, ".planar" }) },
    };

    const health = suite.mustRunWith(&.{ "health", "--json" }, &env);
    defer a.free(health);
    try std.testing.expect(std.mem.indexOf(u8, health, "\"overall\":\"ok\"") != null);
    const plans_before = suite.mustRun(&.{ "plan", "list", "--scope", "global", "--json" });
    defer a.free(plans_before);
    const preview_raw = suite.mustRunWith(&.{ "report", "--days", "30", "--json" }, &env);
    defer a.free(preview_raw);
    try std.testing.expect(std.mem.indexOf(u8, preview_raw, "PRIVATE") == null);
    var preview = try std.json.parseFromSlice(std.json.Value, arena, preview_raw, .{});
    defer preview.deinit();
    const signals = preview.value.object.get("introspection_preview").?.object.get("signals").?.array.items;
    try std.testing.expectEqual(@as(usize, 3), signals.len);
    try std.testing.expectEqualStrings("claude", signals[0].object.get("vendor").?.string);
    try std.testing.expectEqualStrings("codex", signals[1].object.get("vendor").?.string);
    try std.testing.expectEqualStrings("copilot", signals[2].object.get("vendor").?.string);
    const plans_after_preview = suite.mustRun(&.{ "plan", "list", "--scope", "global", "--json" });
    defer a.free(plans_after_preview);
    try std.testing.expectEqualStrings(plans_before, plans_after_preview);

    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "Planar Feedback", "--slug", "planar-feedback", "--scope", "global", "--json" });
    const plan_id = try std.fmt.allocPrint(arena, "{d}", .{plan.id});
    const finding = suite.mustRunJSON(Id, arena, &.{ "task", "add", "retry-pattern: planar plan show", "--plan", plan_id, "--body", "vendor=codex; category=retry; count=1", "--scope", "global", "--json" });
    const finding_ref = try std.fmt.allocPrint(arena, "task:{d}", .{finding.id});
    _ = suite.mustRunJSON(std.json.Value, arena, &.{ "feedback", "triage", "set", finding_ref, "--severity", "high", "--disposition", "accepted", "--reproduction", "reproduced", "--evidence", "redacted cross-vendor aggregate", "--scope", "global", "--json" });
    const shown = suite.mustRunJSON(std.json.Value, arena, &.{ "feedback", "triage", "show", finding_ref, "--json" });
    try std.testing.expectEqualStrings("accepted", shown.object.get("disposition").?.string);
    const post_plan = suite.mustRunJSON(std.json.Value, arena, &.{ "plan", "show", plan_id, "--json" });
    try std.testing.expectEqual(plan.id, post_plan.object.get("id").?.integer);
}

test "scenario: durable knowledge is visible through read-only operational observation" {
    const a = std.testing.allocator;
    var suite = harness.Suite.init(a);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(a);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    _ = suite.registerProject("m9-knowledge-observe");

    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "Decision lifecycle", "--scope", "global", "--json" });
    const plan_id = try std.fmt.allocPrint(arena, "{d}", .{plan.id});
    const artifact = suite.mustRunJSON(Id, arena, &.{ "artifact", "add", "SQLite evidence", "--kind", "tech_spec", "--body", "WAL preserves concurrent readers.", "--scope", "global", "--json" });
    const decision = suite.mustRunJSON(Id, arena, &.{ "decision", "add", "Adopt SQLite WAL", "--body", "Use WAL for operational concurrency.", "--scope", "global", "--json" });
    const annotation = suite.mustRunJSON(Id, arena, &.{ "annotate", "add", "--text", "The busy timeout complements WAL.", "--anchor-path", "src/db/db.zig", "--scope", "global", "--json" });
    const decision_ref = try std.fmt.allocPrint(arena, "decision:{d}", .{decision.id});
    const artifact_ref = try std.fmt.allocPrint(arena, "artifact:{d}", .{artifact.id});
    const annotation_ref = try std.fmt.allocPrint(arena, "annotation:{d}", .{annotation.id});
    const plan_ref = try std.fmt.allocPrint(arena, "plan:{d}", .{plan.id});
    a.free(suite.mustRun(&.{ "links", "add", decision_ref, plan_ref, "--relationship", "addresses", "--json" }));
    a.free(suite.mustRun(&.{ "links", "add", decision_ref, artifact_ref, "--relationship", "cites", "--json" }));
    a.free(suite.mustRun(&.{ "links", "add", annotation_ref, decision_ref, "--relationship", "cites", "--json" }));
    const links = suite.mustRun(&.{ "links", "list", decision_ref, "--json" });
    defer a.free(links);
    try std.testing.expect(std.mem.indexOf(u8, links, "\"relationship\":\"addresses\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, links, "\"relationship\":\"cites\"") != null);
    _ = suite.mustRunJSON(std.json.Value, arena, &.{ "decision", "show", try std.fmt.allocPrint(arena, "{d}", .{decision.id}), "--json" });
    _ = suite.mustRunJSON(std.json.Value, arena, &.{ "artifact", "show", try std.fmt.allocPrint(arena, "{d}", .{artifact.id}), "--json" });
    _ = suite.mustRunJSON(std.json.Value, arena, &.{ "annotate", "show", try std.fmt.allocPrint(arena, "{d}", .{annotation.id}), "--json" });

    const task = suite.mustRunJSON(Id, arena, &.{ "task", "add", "Observe this work", "--plan", plan_id, "--scope", "global", "--json" });
    const task_id = try std.fmt.allocPrint(arena, "{d}", .{task.id});
    const task_ref = try std.fmt.allocPrint(arena, "task:{d}", .{task.id});
    const task_before = suite.mustRun(&.{ "task", "show", task_id, "--json" });
    defer a.free(task_before);
    const claim_raw = mustRunSibling(&suite, "PLANAR_AGENT_BIN", &.{ "claim", "--entity", task_ref, "--role", "documenter", "--no-locality-probe", "--json" });
    defer a.free(claim_raw);
    var claim = try std.json.parseFromSlice(std.json.Value, arena, claim_raw, .{});
    defer claim.deinit();
    const token = claim.value.object.get("claim_token").?.string;
    a.free(mustRunSibling(&suite, "PLANAR_AGENT_BIN", &.{ "heartbeat", "--claim", token, "--status", "scanning docs 1/1", "--json" }));
    const observed = mustRunSibling(&suite, "PLANAR_WATCH_BIN", &.{ "ps", "--json" });
    defer a.free(observed);
    var observed_json = try std.json.parseFromSlice(std.json.Value, arena, observed, .{});
    defer observed_json.deinit();
    var found_task = false;
    for (observed_json.value.object.get("active").?.array.items) |row| {
        const obj = row.object;
        if (std.mem.eql(u8, obj.get("entity_kind").?.string, "task") and obj.get("entity_id").?.integer == task.id)
            found_task = true;
    }
    try std.testing.expect(found_task);
    try std.testing.expect(std.mem.indexOf(u8, observed, "scanning docs 1/1") != null);
    const task_after_observe = suite.mustRun(&.{ "task", "show", task_id, "--json" });
    defer a.free(task_after_observe);
    try std.testing.expectEqualStrings(task_before, task_after_observe);
    a.free(mustRunSibling(&suite, "PLANAR_AGENT_BIN", &.{ "release", "--claim", token, "--reason", "observation complete", "--no-locality-probe", "--json" }));
    const released = suite.mustRunJSON(std.json.Value, arena, &.{ "task", "show", task_id, "--json" });
    try std.testing.expectEqualStrings("todo", released.object.get("status").?.string);
}

const DocResult = struct { term: std.process.Child.Term, stdout: []u8, stderr: []u8 };

const DocumenterProposalRow = struct {
    row_id: []const u8,
    action: []const u8,
    source_path: []const u8,
    target_path: []const u8,
};

const DocumenterProposal = struct { rows: []const DocumenterProposalRow };

fn applyApprovedProse(
    a: std.mem.Allocator,
    root: []const u8,
    row: DocumenterProposalRow,
    approved_row_ids: []const []const u8,
) !void {
    var approved = false;
    for (approved_row_ids) |row_id| {
        if (std.mem.eql(u8, row_id, row.row_id)) approved = true;
    }
    if (!approved) return error.UnapprovedProposalRow;
    if (!std.mem.eql(u8, row.action, "create-doc") or
        !std.mem.startsWith(u8, row.target_path, "docs/")) return error.InvalidProposalRow;
    const target = try std.fs.path.join(a, &.{ root, row.target_path });
    defer a.free(target);
    try writeAbsolute(target, "# Widget\n\nApproved public reference.\n");
}

fn runDoc(a: std.mem.Allocator, cwd: []const u8, args: []const []const u8) !DocResult {
    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(a);
    try argv.append(a, envBin("PLANAR_DOC_BIN"));
    for (args) |arg| try argv.append(a, arg);
    const result = try std.process.run(a, std.testing.io, .{ .argv = argv.items, .cwd = .{ .path = cwd } });
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr };
}

test "scenario: approved documentation proposal is authored covered and verified" {
    const a = std.testing.allocator;
    var suite = harness.Suite.init(a);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(a);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var path_buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &path_buf);
    const root = path_buf[0..root_len];

    // Agent roles are prompt-driven and are not executed by this deterministic
    // suite. Render their public surfaces and pin the authority boundary that
    // the fixture below models: documenter proposes, the operator approves
    // exact rows, doc-author-equivalent prose writes only those rows, and the
    // caller owns planar-doc mutations.
    const rendered = try std.fs.path.join(arena, &.{ suite.tmpAbsPath(), "doc-lifecycle-rendered" });
    var src_buf: [std.fs.max_path_bytes]u8 = undefined;
    var src_dir = try std.Io.Dir.cwd().openDir(std.testing.io, "skills/src", .{});
    defer src_dir.close(std.testing.io);
    const src_len = try src_dir.realPath(std.testing.io, &src_buf);
    a.free(suite.mustRun(&.{ "skills", "render", "--src", src_buf[0..src_len], "--out", rendered }));
    const maintain_path = try std.fs.path.join(arena, &.{ rendered, "skills/codex/pl-doc-maintain.md" });
    const maintain = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, maintain_path, a, .limited(512 * 1024));
    defer a.free(maintain);
    try std.testing.expect(std.mem.indexOf(u8, maintain, "approved_rows") != null);
    try std.testing.expect(std.mem.indexOf(u8, maintain, "documenter") != null);
    try std.testing.expect(std.mem.indexOf(u8, maintain, "read-only") != null);
    try std.testing.expect(std.mem.indexOf(u8, maintain, "caller") != null);
    try std.testing.expect(std.mem.indexOf(u8, maintain, "unapproved rows must never appear") != null);
    try std.testing.expect(std.mem.indexOf(u8, maintain, "caller—not documenter or") != null);

    const baseline = try runDoc(a, root, &.{ "build", "--json" });
    defer a.free(baseline.stdout);
    defer a.free(baseline.stderr);
    try std.testing.expect(baseline.term == .exited and baseline.term.exited == 0);
    try tmp.dir.createDirPath(std.testing.io, "src/widget");
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "src/widget/lib.zig", .data = "pub fn widget() void {}\n" });

    const preview = try runDoc(a, root, &.{ "diff", "--json" });
    defer a.free(preview.stdout);
    defer a.free(preview.stderr);
    try std.testing.expect(preview.term == .exited and preview.term.exited == 1);
    try std.testing.expect(std.mem.indexOf(u8, preview.stdout, "\"signal\":\"new-authoring\"") != null);
    try std.testing.expectError(error.FileNotFound, tmp.dir.access(std.testing.io, "docs/features/widget.md", .{}));

    const proposal_json =
        \\{"rows":[
        \\  {"row_id":"doc-1","action":"create-doc","source_path":"src/widget/","target_path":"docs/features/widget.md"},
        \\  {"row_id":"doc-2","action":"create-doc","source_path":"src/private/","target_path":"docs/features/unapproved.md"}
        \\]}
    ;
    var proposal = try std.json.parseFromSlice(DocumenterProposal, arena, proposal_json, .{});
    defer proposal.deinit();
    const manifest_before_rejection = try tmp.dir.readFileAlloc(std.testing.io, ".planar-manifest", a, .limited(512 * 1024));
    defer a.free(manifest_before_rejection);
    try std.testing.expectError(
        error.UnapprovedProposalRow,
        applyApprovedProse(a, root, proposal.value.rows[1], &.{"doc-1"}),
    );
    try std.testing.expectError(error.FileNotFound, tmp.dir.access(std.testing.io, "docs/features/unapproved.md", .{}));
    const manifest_after_rejection = try tmp.dir.readFileAlloc(std.testing.io, ".planar-manifest", a, .limited(512 * 1024));
    defer a.free(manifest_after_rejection);
    try std.testing.expectEqualStrings(manifest_before_rejection, manifest_after_rejection);

    try applyApprovedProse(a, root, proposal.value.rows[0], &.{"doc-1"});
    try tmp.dir.access(std.testing.io, "docs/features/widget.md", .{});
    var final_root: ?[]u8 = null;
    defer if (final_root) |value| a.free(value);
    for ([_][]const []const u8{
        &.{ "cover", "docs/features/widget.md", "src/widget/" },
        &.{ "lint", "--json" },
        &.{ "build", "--json" },
        &.{ "verify", "--json" },
        &.{ "diff", "--json" },
    }) |args| {
        const result = try runDoc(a, root, args);
        defer a.free(result.stdout);
        defer a.free(result.stderr);
        if (result.term != .exited or result.term.exited != 0) {
            std.debug.print("planar-doc {s} failed: {s}\n", .{ args[0], result.stderr });
            return error.TestUnexpectedResult;
        }
        if (std.mem.eql(u8, args[0], "verify"))
            try std.testing.expect(std.mem.indexOf(u8, result.stdout, "\"drift\":false") != null);
        if (std.mem.eql(u8, args[0], "build")) {
            var built = try std.json.parseFromSlice(std.json.Value, arena, result.stdout, .{});
            defer built.deinit();
            final_root = try a.dupe(u8, built.value.object.get("root").?.string);
            try std.testing.expect(final_root.?.len > 0);
        }
        if (std.mem.eql(u8, args[0], "diff"))
            try std.testing.expectEqual(@as(usize, 0), std.mem.trim(u8, result.stdout, " \r\n\t").len);
    }
    try std.testing.expect(final_root != null);
}

test "scenario: approved sync conflict resolution uses the rendered public workflow" {
    try ext_sync.runSyncConflictEvidenceAndResolutionLifecycle();
}
