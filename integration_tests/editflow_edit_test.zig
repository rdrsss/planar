//! integration_tests/editflow_edit_test.zig
//!
//! Focused edit-flow parity checks.

const std = @import("std");
const harness = @import("harness");

const IDJSON = struct { id: i64 };
const ReviewJSON = struct {
    workbench_path: []const u8,
};

test "task edit warns before overwriting a divergent on-disk workbench file" {
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
        .{ .key = "PLANAR_EDITOR", .value = "/usr/bin/true" },
    };

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Edit Warning Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");
    const task = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "Warn overwrite task" });
    const task_id = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    const review_res = suite.execWith(&.{ "task", "review", task_id, "--json" }, env);
    defer review_res.deinit(gpa);
    try std.testing.expect(review_res.term == .exited and review_res.term.exited == 0);
    const review_raw = try gpa.dupe(u8, review_res.stdout);
    defer gpa.free(review_raw);
    const review_parsed = std.json.parseFromSlice(ReviewJSON, arena, review_raw, .{
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("review parse failed: {s}\nraw: {s}\n", .{ @errorName(e), review_raw });
        try std.testing.expect(false);
        unreachable;
    };

    const wb_path = review_parsed.value.workbench_path;
    if (std.mem.lastIndexOfScalar(u8, wb_path, '/')) |slash| {
        const dir = wb_path[0..slash];
        std.Io.Dir.cwd().createDirPath(std.testing.io, dir) catch |e| switch (e) {
            error.PathAlreadyExists => {},
            else => return e,
        };
    }
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = wb_path,
        .data = "---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: 1\ntitle: local-only\nstatus: todo\n---\n\nlocal override\n",
    });

    const res = suite.execWith(&.{ "task", "edit", task_id }, env);
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited and res.term.exited == 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stderr, 1, "warning: overwriting local workbench file"));
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stderr, 1, "aborted: no changes made"));
}
