//! integration_tests/workbench_test.zig
//!
//! Smoke checks for workbench verbs.

const std = @import("std");
const harness = @import("harness");

const IDJSON = struct { id: i64 };

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
