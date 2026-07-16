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
    severity: []const u8,
    code: []const u8,
};

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
