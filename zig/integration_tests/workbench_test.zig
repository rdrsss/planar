//! integration_tests/workbench_test.zig
//!
//! Smoke checks for workbench verbs.

const std = @import("std");
const harness = @import("harness");

const IDJSON = struct { id: i64 };

const StatusEntry = struct { file_path: []const u8 };
const StatusJSON = struct { entries: []const StatusEntry };
const LintIssue = struct {
    path: []const u8,
    line: i64,
    severity: []const u8,
    code: []const u8,
    message: []const u8,
    hint: []const u8,
};

fn writeLintFixture(path: []const u8, content: []const u8) !void {
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = content });
}

fn expectLintIssue(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    env: []const harness.Suite.ExtraEnvEntry,
    path: []const u8,
    code: []const u8,
    severity: []const u8,
    line: i64,
    message_fragment: []const u8,
    hint_fragment: []const u8,
) !void {
    const result = suite.execWith(&.{ "workbench", "lint", "--path", path, "--json" }, env);
    defer result.deinit(std.testing.allocator);
    try std.testing.expect(result.term == .exited and result.term.exited == 1);
    const issue = try std.json.parseFromSlice(LintIssue, arena, result.stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = false,
    });
    try std.testing.expectEqualStrings(path, issue.value.path);
    try std.testing.expectEqualStrings(code, issue.value.code);
    try std.testing.expectEqualStrings(severity, issue.value.severity);
    try std.testing.expectEqual(line, issue.value.line);
    try std.testing.expect(std.mem.indexOf(u8, issue.value.message, message_fragment) != null);
    try std.testing.expect(std.mem.indexOf(u8, issue.value.hint, hint_fragment) != null);
}

test "workbench push/status/pull/list smoke" {
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

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Workbench Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");
    _ = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "Workbench task" });

    const push = suite.execWith(&.{ "workbench", "push", plan_id }, env);
    defer push.deinit(gpa);
    try std.testing.expect(push.term == .exited and push.term.exited == 0);

    const status = suite.execWith(&.{ "workbench", "status", plan_id }, env);
    defer status.deinit(gpa);
    try std.testing.expect(status.term == .exited and status.term.exited == 0);

    const pull = suite.execWith(&.{ "workbench", "pull", plan_id }, env);
    defer pull.deinit(gpa);
    try std.testing.expect(pull.term == .exited and pull.term.exited == 0);

    const list = suite.execWith(&.{ "workbench", "list" }, env);
    defer list.deinit(gpa);
    try std.testing.expect(list.term == .exited and list.term.exited == 0);
}

test "workbench lint supports plan all and path targets with stable diagnostics" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const wb_root = std.fmt.allocPrint(arena, "{s}/workbench", .{suite.tmpAbsPath()}) catch @panic("OOM");
    const env = &[_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Lint Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");
    gpa.free(suite.mustRunWith(&.{ "workbench", "push", plan_id, "--json" }, env));

    const clean = suite.execWith(&.{ "workbench", "lint", plan_id }, env);
    defer clean.deinit(gpa);
    try std.testing.expect(clean.term == .exited and clean.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, clean.stdout, "0 errors, 0 warnings") != null);

    const status_raw = suite.mustRunWith(&.{ "workbench", "status", plan_id, "--json" }, env);
    const status = std.json.parseFromSlice(StatusJSON, arena, status_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(status_raw);
    var readme: ?[]const u8 = null;
    for (status.value.entries) |entry| {
        if (std.mem.endsWith(u8, entry.file_path, "README.md")) {
            readme = entry.file_path;
            break;
        }
    }
    try std.testing.expect(readme != null);
    const readme_abs = std.fmt.allocPrint(arena, "{s}/{s}", .{ wb_root, readme.? }) catch @panic("OOM");
    std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = readme_abs, .data = "missing frontmatter\n" }) catch unreachable;

    const path_result = suite.execWith(&.{ "workbench", "lint", "--path", readme_abs, "--json" }, env);
    defer path_result.deinit(gpa);
    try std.testing.expect(path_result.term == .exited and path_result.term.exited == 1);
    const issue = std.json.parseFromSlice(LintIssue, arena, path_result.stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expectEqualStrings(readme_abs, issue.value.path);
    try std.testing.expectEqualStrings("error", issue.value.severity);
    try std.testing.expectEqualStrings("malformed_frontmatter", issue.value.code);

    const all_result = suite.execWith(&.{ "workbench", "lint", "--all" }, env);
    defer all_result.deinit(gpa);
    try std.testing.expect(all_result.term == .exited and all_result.term.exited == 1);
    try std.testing.expect(std.mem.indexOf(u8, all_result.stdout, "1 errors, 0 warnings") != null);

    const missing_target = suite.execWith(&.{ "workbench", "lint" }, env);
    defer missing_target.deinit(gpa);
    try std.testing.expect(missing_target.term == .exited and missing_target.term.exited == 2);
}

test "workbench lint path enforces every rendered entity schema and diagnostic class" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    _ = suite.registerProject("wb-lint-schema");
    const wb_root = try std.fmt.allocPrint(arena, "{s}/workbench", .{suite.tmpAbsPath()});
    const env = &[_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };
    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Lint schema anchor" });

    const EntityCase = struct {
        kind: []const u8,
        status: []const u8,
        artifact_line: []const u8 = "",
    };
    const cases = [_]EntityCase{
        .{ .kind = "plan", .status = "draft" },
        .{ .kind = "task", .status = "todo" },
        .{ .kind = "artifact", .status = "active", .artifact_line = "artifact_kind: tech_spec\n" },
        .{ .kind = "scenario", .status = "ready" },
        .{ .kind = "decision", .status = "proposed" },
        .{ .kind = "question", .status = "open" },
    };

    for (cases, 0..) |case, index| {
        const path = try std.fmt.allocPrint(arena, "{s}/lint-{s}.md", .{ suite.tmpAbsPath(), case.kind });
        const valid = try std.fmt.allocPrint(
            arena,
            "---\nentity_kind: {s}\nentity_id: {d}\nanchor_plan_id: {d}\ntitle: Valid {s}\nstatus: {s}\n{s}---\n",
            .{ case.kind, index + 1, plan.id, case.kind, case.status, case.artifact_line },
        );
        try writeLintFixture(path, valid);
        const clean = suite.execWith(&.{ "workbench", "lint", "--path", path, "--json" }, env);
        defer clean.deinit(gpa);
        try std.testing.expect(clean.term == .exited and clean.term.exited == 0);
        try std.testing.expectEqual(@as(usize, 0), clean.stdout.len);

        const missing_title = try std.fmt.allocPrint(
            arena,
            "---\nentity_kind: {s}\nentity_id: {d}\nanchor_plan_id: {d}\nstatus: {s}\n{s}---\n",
            .{ case.kind, index + 1, plan.id, case.status, case.artifact_line },
        );
        try writeLintFixture(path, missing_title);
        try expectLintIssue(&suite, arena, env, path, "missing_required_field", "error", 5, "title", "title");

        const missing_status = try std.fmt.allocPrint(
            arena,
            "---\nentity_kind: {s}\nentity_id: {d}\nanchor_plan_id: {d}\ntitle: Missing status\n{s}---\n",
            .{ case.kind, index + 1, plan.id, case.artifact_line },
        );
        try writeLintFixture(path, missing_status);
        try expectLintIssue(&suite, arena, env, path, "missing_required_field", "error", 6, "status", "status");

        const invalid_status = try std.fmt.allocPrint(
            arena,
            "---\nentity_kind: {s}\nentity_id: {d}\nanchor_plan_id: {d}\ntitle: Invalid status\nstatus: not-a-status\n{s}---\n",
            .{ case.kind, index + 1, plan.id, case.artifact_line },
        );
        try writeLintFixture(path, invalid_status);
        try expectLintIssue(&suite, arena, env, path, "invalid_field_value", "error", 6, "status", case.status);
    }

    const artifact_path = try std.fmt.allocPrint(arena, "{s}/lint-artifact.md", .{suite.tmpAbsPath()});
    const missing_artifact_kind = try std.fmt.allocPrint(
        arena,
        "---\nentity_kind: artifact\nentity_id: 90\nanchor_plan_id: {d}\ntitle: Missing kind\nstatus: active\n---\n",
        .{plan.id},
    );
    try writeLintFixture(artifact_path, missing_artifact_kind);
    try expectLintIssue(&suite, arena, env, artifact_path, "missing_required_field", "error", 7, "artifact_kind", "artifact_kind");

    const invalid_artifact_kind = try std.fmt.allocPrint(
        arena,
        "---\nentity_kind: artifact\nentity_id: 91\nanchor_plan_id: {d}\ntitle: Invalid kind\nstatus: active\nartifact_kind: imaginary\n---\n",
        .{plan.id},
    );
    try writeLintFixture(artifact_path, invalid_artifact_kind);
    try expectLintIssue(&suite, arena, env, artifact_path, "invalid_field_value", "error", 7, "artifact_kind", "tech_spec");

    const identity_path = try std.fmt.allocPrint(arena, "{s}/lint-identity.md", .{suite.tmpAbsPath()});
    const missing_identity = try std.fmt.allocPrint(
        arena,
        "---\nentity_kind: task\nanchor_plan_id: {d}\ntitle: Missing ID\nstatus: todo\n---\n",
        .{plan.id},
    );
    try writeLintFixture(identity_path, missing_identity);
    try expectLintIssue(&suite, arena, env, identity_path, "missing_required_field", "error", 3, "entity_id", "entity_id");

    const invalid_kind = try std.fmt.allocPrint(
        arena,
        "---\nentity_kind: widget\nentity_id: 1\nanchor_plan_id: {d}\ntitle: Widget\nstatus: active\n---\n",
        .{plan.id},
    );
    try writeLintFixture(identity_path, invalid_kind);
    try expectLintIssue(&suite, arena, env, identity_path, "invalid_entity_kind", "error", 2, "entity_kind", "question");

    const anchor_path = try std.fmt.allocPrint(arena, "{s}/lint-anchor.md", .{suite.tmpAbsPath()});
    const anchors = [_]i64{ 0, 99999999 };
    for (anchors) |anchor| {
        const invalid_anchor = try std.fmt.allocPrint(
            arena,
            "---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: {d}\ntitle: Invalid anchor\nstatus: todo\n---\n",
            .{anchor},
        );
        try writeLintFixture(anchor_path, invalid_anchor);
        try expectLintIssue(&suite, arena, env, anchor_path, "anchor_plan_not_found", "warning", 4, "anchor_plan_id", "owning top-level plan");
    }
}

test "workbench lint path rejects malformed YAML scalars with exact locations and hints" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    _ = suite.registerProject("wb-lint-yaml");
    const wb_root = try std.fmt.allocPrint(arena, "{s}/workbench", .{suite.tmpAbsPath()});
    const env = &[_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };
    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Lint YAML anchor" });
    const path = try std.fmt.allocPrint(arena, "{s}/lint-yaml.md", .{suite.tmpAbsPath()});

    const Case = struct {
        content: []const u8,
        line: i64,
        hint: []const u8,
    };
    const colon = try std.fmt.allocPrint(
        arena,
        "---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: {d}\ntitle: Broken: unquoted\nstatus: todo\n---\n",
        .{plan.id},
    );
    const leading_dash = try std.fmt.allocPrint(
        arena,
        "---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: {d}\ntitle: - unquoted\nstatus: todo\n---\n",
        .{plan.id},
    );
    const tab_indentation = try std.fmt.allocPrint(
        arena,
        "---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: {d}\n\ttitle: tabbed\nstatus: todo\n---\n",
        .{plan.id},
    );
    const malformed_cases = [_]Case{
        .{ .content = "front matter delimiter missing\n", .line = 1, .hint = "start the file" },
        .{ .content = colon, .line = 5, .hint = "quote" },
        .{ .content = leading_dash, .line = 5, .hint = "quote" },
        .{ .content = tab_indentation, .line = 5, .hint = "spaces" },
    };
    for (malformed_cases) |case| {
        try writeLintFixture(path, case.content);
        try expectLintIssue(&suite, arena, env, path, "malformed_frontmatter", "error", case.line, "malformed", case.hint);
    }
}
