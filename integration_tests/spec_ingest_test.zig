//! integration_tests/spec_ingest_test.zig
//!
//! Smoke coverage for `spec ingest` apply-removals guard.

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

fn parseJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) T {
    const trimmed = std.mem.trim(u8, buf, " \n");
    const parsed = std.json.parseFromSlice(T, arena, trimmed, .{
        .ignore_unknown_fields = true,
    }) catch unreachable;
    return parsed.value;
}

fn findRoadmapPathFromPush(
    allocator: std.mem.Allocator,
    wb_root: []const u8,
    push_stdout: []const u8,
) ![]u8 {
    const PushJSON = struct {
        entries: []const struct {
            file_path: []const u8,
            entity_kind: []const u8,
        },
    };
    const parsed = parseJSON(PushJSON, allocator, push_stdout);

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
        if (std.mem.indexOf(u8, body, "artifact_kind: roadmap") != null) {
            return abs;
        }
        allocator.free(abs);
    }
    return error.FileNotFound;
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
        "plan", "list", "--json", "--parent", plan_id,
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
        "task", "list", "--json", "--plan", removed_plan_id_s,
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
