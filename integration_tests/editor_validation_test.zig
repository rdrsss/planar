//! integration_tests/editor_validation_test.zig
//!
//! Frontmatter validation coverage for editor-driven edit.

const std = @import("std");
const harness = @import("harness");

const IDJSON = struct { id: i64 };
const TaskShowJSON = struct {
    id: i64,
    status: []const u8,
};

fn writeFile(abs_path: []const u8, body: []const u8) !void {
    if (std.mem.lastIndexOfScalar(u8, abs_path, '/')) |slash| {
        const dir = abs_path[0..slash];
        std.Io.Dir.cwd().createDirPath(std.testing.io, dir) catch |e| switch (e) {
            error.PathAlreadyExists => {},
            else => return e,
        };
    }
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = abs_path,
        .data = body,
    });
}

fn writeEditorScript(abs_path: []const u8, body: []const u8) !void {
    try writeFile(abs_path, body);
    const chmod_res = try std.process.run(std.testing.allocator, std.testing.io, .{
        .argv = &.{ "/bin/chmod", "755", abs_path },
    });
    defer std.testing.allocator.free(chmod_res.stdout);
    defer std.testing.allocator.free(chmod_res.stderr);
    if (chmod_res.term != .exited or chmod_res.term.exited != 0) return error.AccessDenied;
}

test "editor validation rejects invalid status mutation on task edit" {
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
    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Editor Validation Reject Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");
    const task = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "Reject Invalid Status Task" });
    const task_id = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    const rewrite = std.fmt.allocPrint(arena,
        \\---
        \\entity_kind: task
        \\entity_id: {d}
        \\anchor_plan_id: {d}
        \\title: Reject Invalid Status Task
        \\status: frobnicated
        \\priority: 100
        \\---
        \\
        \\body edit
        \\
    , .{ task.id, plan.id }) catch @panic("OOM");
    const rewrite_path = std.fs.path.join(arena, &.{ tmp_abs, "task-rewrite.md" }) catch @panic("OOM");
    try writeFile(rewrite_path, rewrite);
    const script = std.fs.path.join(arena, &.{ tmp_abs, "editor-invalid-status.sh" }) catch @panic("OOM");
    const script_body = std.fmt.allocPrint(arena, "#!/bin/sh\ncp {s} \"$1\"\n", .{rewrite_path}) catch @panic("OOM");
    try writeEditorScript(script, script_body);

    const env = &[_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
        .{ .key = "PLANAR_EDITOR", .value = script },
    };
    const res = suite.execWith(&.{ "task", "edit", task_id }, env);
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited and res.term.exited != 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stderr, 1, "QueryFailed"));

    const shown = suite.mustRunJSON(TaskShowJSON, arena, &.{ "task", "show", "--json", task_id });
    try std.testing.expectEqualStrings("todo", shown.status);
}

test "editor validation warns and ignores artifact_kind mutation under M4 limits" {
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

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Editor Validation Warning Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");
    const artifact = suite.mustRunJSON(IDJSON, arena, &.{
        "artifact", "add", "--json", "--kind", "tech_spec", "--plan", plan_id, "Validation Artifact",
    });
    const artifact_id = std.fmt.allocPrint(arena, "{d}", .{artifact.id}) catch @panic("OOM");

    const rewrite = std.fmt.allocPrint(arena,
        \\---
        \\entity_kind: artifact
        \\entity_id: {d}
        \\anchor_plan_id: {d}
        \\title: Validation Artifact
        \\status: active
        \\artifact_kind: adr
        \\---
        \\
        \\rewritten body
        \\
    , .{ artifact.id, plan.id }) catch @panic("OOM");
    const rewrite_path = std.fs.path.join(arena, &.{ tmp_abs, "artifact-rewrite.md" }) catch @panic("OOM");
    try writeFile(rewrite_path, rewrite);
    const script = std.fs.path.join(arena, &.{ tmp_abs, "editor-artifact-kind.sh" }) catch @panic("OOM");
    const script_body = std.fmt.allocPrint(arena, "#!/bin/sh\ncp {s} \"$1\"\n", .{rewrite_path}) catch @panic("OOM");
    try writeEditorScript(script, script_body);

    const env = &[_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
        .{ .key = "PLANAR_EDITOR", .value = script },
    };
    const res = suite.execWith(&.{ "artifact", "edit", artifact_id }, env);
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited and res.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stderr, 1, "M4 limitation"));

    const shown = suite.mustRun(&.{ "artifact", "show", "--json", artifact_id });
    defer gpa.free(shown);
    try std.testing.expect(std.mem.containsAtLeast(u8, shown, 1, "\"kind\":\"tech_spec\""));
}
