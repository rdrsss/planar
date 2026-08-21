//! No-argument `planar resume` cwd-scope resolution regressions.

const std = @import("std");
const harness = @import("harness");

const TaskJSON = struct { id: i64 };
const ResumeJSON = struct {
    identity: struct { task_id: i64 },
};

test "resume without id selects the most recent active task in cwd scope" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("resume-scope-hit");
    suite.addAssoc("resume-scope-hit", null);

    const local_raw = suite.mustRunInDir(root, &.{
        "task", "add", "--json", "--next-action", "continue local work", "Local task",
    });
    defer gpa.free(local_raw);
    const local = parseJSON(TaskJSON, arena, local_raw);
    const local_id = std.fmt.allocPrint(arena, "{d}", .{local.id}) catch unreachable;

    const global_raw = suite.mustRunInDir(root, &.{
        "task", "add", "--json", "--scope", "global", "--next-action", "continue global work", "Global task",
    });
    defer gpa.free(global_raw);
    const global = parseJSON(TaskJSON, arena, global_raw);
    const global_id = std.fmt.allocPrint(arena, "{d}", .{global.id}) catch unreachable;

    gpa.free(suite.mustRunInDir(root, &.{
        "capture", "session", "--task", local_id, "--vendor-session-id", "local-session",
    }));
    gpa.free(suite.mustRunInDir(root, &.{
        "capture", "session", "--task", global_id, "--vendor-session-id", "newer-global-session",
    }));

    const resume_raw = suite.mustRunInDir(root, &.{ "resume", "--json" });
    defer gpa.free(resume_raw);
    const packet = parseJSON(ResumeJSON, arena, resume_raw);
    try std.testing.expectEqual(local.id, packet.identity.task_id);
}

test "resume without id refuses when cwd scope has no active task" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("resume-scope-empty");
    suite.addAssoc("resume-scope-empty", null);

    const global_raw = suite.mustRunInDir(root, &.{
        "task", "add", "--json", "--scope", "global", "--next-action", "continue global work", "Foreign global task",
    });
    defer gpa.free(global_raw);
    const global = parseJSON(TaskJSON, arena, global_raw);
    const global_id = std.fmt.allocPrint(arena, "{d}", .{global.id}) catch unreachable;
    gpa.free(suite.mustRunInDir(root, &.{
        "capture", "session", "--task", global_id, "--vendor-session-id", "global-only-session",
    }));

    const stderr = suite.expectFailureInDir(root, &.{ "resume", "--json" });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "no active task in cwd-derived scope") != null);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "pass <task-id> explicitly") != null);
}

fn parseJSON(comptime T: type, allocator: std.mem.Allocator, raw: []const u8) T {
    const parsed = std.json.parseFromSlice(T, allocator, raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("JSON decode failed: {s}\nraw: {s}\n", .{ @errorName(e), raw });
        @panic("JSON decode failed");
    };
    return parsed.value;
}
