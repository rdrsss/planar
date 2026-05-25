//! integration_tests/editor_add_test.zig
//!
//! Non-TTY editor-add parity coverage across planning entities.

const std = @import("std");
const harness = @import("harness");

const IDJSON = struct { id: i64 };
const TaskShowJSON = struct {
    id: i64,
    body: ?[]const u8 = null,
};

fn parseJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) T {
    const trimmed = std.mem.trim(u8, buf, " \n");
    const parsed = std.json.parseFromSlice(T, arena, trimmed, .{
        .ignore_unknown_fields = true,
    }) catch unreachable;
    return parsed.value;
}

test "editor add covers task/artifact/question/scenario in non-tty harness" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Editor Add Entity Coverage Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");
    const task = suite.mustRunJSON(IDJSON, arena, &.{
        "task", "add", "--json", "--plan", plan_id, "--editor", "Editor add task",
    });
    try std.testing.expect(task.id > 0);

    const artifact = suite.mustRunJSON(IDJSON, arena, &.{
        "artifact", "add", "--json", "--kind", "tech_spec", "--plan", plan_id, "--editor", "Editor add artifact",
    });
    try std.testing.expect(artifact.id > 0);

    const q_res = suite.exec(&.{
        "question", "add", "--json", "--editor", "Editor add question",
    });
    defer q_res.deinit(gpa);
    try std.testing.expect(q_res.term == .exited and q_res.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, q_res.stderr, 1, "--editor not yet implemented"));
    const q = parseJSON(IDJSON, arena, q_res.stdout);
    try std.testing.expect(q.id > 0);

    const s_res = suite.exec(&.{
        "scenario", "add", "--json", "--plan", plan_id, "--editor", "Editor add scenario",
    });
    defer s_res.deinit(gpa);
    try std.testing.expect(s_res.term == .exited and s_res.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, s_res.stderr, 1, "--editor not yet implemented"));
    const s = parseJSON(IDJSON, arena, s_res.stdout);
    try std.testing.expect(s.id > 0);
}

test "decision add with --editor and no --body aborts in non-tty harness" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = suite.exec(&.{
        "decision", "add", "--json", "--editor", "Editor add decision",
    });
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited and res.term.exited != 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stderr, 1, "--body is required"));
}

test "task add with explicit --body is not blocked by --editor flag" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Editor Body Precedence Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");
    const task = suite.mustRunJSON(IDJSON, arena, &.{
        "task",   "add",            "--json",
        "--plan", plan_id,          "--editor",
        "--body", "body from flag", "Editor body precedence task",
    });
    const task_id = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");
    const shown = suite.mustRunJSON(TaskShowJSON, arena, &.{ "task", "show", "--json", task_id });
    try std.testing.expect(shown.body != null);
    try std.testing.expect(std.mem.containsAtLeast(u8, shown.body.?, 1, "body from flag"));
}
