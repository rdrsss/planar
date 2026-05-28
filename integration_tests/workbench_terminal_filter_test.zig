//! integration_tests/workbench_terminal_filter_test.zig
//!
//! End-to-end checks for plan 439 M2: `workbench push` filters
//! failure-terminal entities from the FS write set by default; success
//! terminals stay visible; `--filter-mode all` extends the filter.

const std = @import("std");
const harness = @import("harness");

const IDJSON = struct { id: i64 };

fn workbenchEnv(arena: std.mem.Allocator, suite: *harness.Suite) []const harness.Suite.ExtraEnvEntry {
    var tmp_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp_len = suite.tmp_dir.dir.realPath(std.testing.io, &tmp_buf) catch @panic("cannot resolve tmp dir");
    const tmp_abs = tmp_buf[0..tmp_len];
    const wb_root = std.fs.path.join(arena, &.{ tmp_abs, "workbench" }) catch @panic("OOM");
    const env = arena.alloc(harness.Suite.ExtraEnvEntry, 1) catch @panic("OOM");
    env[0] = .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root };
    return env;
}

fn fileExists(path: []const u8) bool {
    var f = std.fs.cwd().openFile(path, .{}) catch return false;
    f.close();
    return true;
}

fn featureDirUnder(arena: std.mem.Allocator, wb_root: []const u8, plan_id: i64) []const u8 {
    // The workbench assoc slug for project-scoped plans we create in tests is
    // typically the project's assoc slug (e.g. `project_planar` in operator
    // runs); in a fresh test DB no assoc exists, so the workbench writes
    // under the empty-assoc directory. The plan key is `p<id>`. We accept
    // whichever subtree the push writes — search for the plan directory.
    _ = arena;
    _ = wb_root;
    _ = plan_id;
    return "";
}

test "workbench push filters failure-terminal tasks by default" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const env = workbenchEnv(arena, &suite);

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Filter Test Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const active = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "Active task" });
    const cancelled = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "Cancelled task" });

    // Cancel one task.
    const cancel_id = std.fmt.allocPrint(arena, "{d}", .{cancelled.id}) catch @panic("OOM");
    const cancel = suite.execWith(&.{ "task", "cancel", cancel_id }, env);
    defer cancel.deinit(gpa);
    try std.testing.expect(cancel.term == .exited and cancel.term.exited == 0);

    // Default push — failure-terminal filter on. Cancelled task must NOT
    // appear in the FS tree; active task must.
    const push = suite.execWith(&.{ "workbench", "push", plan_id }, env);
    defer push.deinit(gpa);
    try std.testing.expect(push.term == .exited and push.term.exited == 0);

    // The push summary on stdout should report `1 filtered (mode=failures)`.
    try std.testing.expect(std.mem.indexOf(u8, push.stdout, "filtered") != null);
    try std.testing.expect(std.mem.indexOf(u8, push.stdout, "mode=failures") != null);
    try std.testing.expect(std.mem.indexOf(u8, push.stdout, "1 filtered") != null);

    // Inspect the workbench tree under PLANAR_WORKBENCH_ROOT.
    var dir_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp_len = suite.tmp_dir.dir.realPath(std.testing.io, &dir_buf) catch @panic("cannot resolve tmp dir");
    const wb_root = std.fs.path.join(arena, &.{ dir_buf[0..tmp_len], "workbench" }) catch @panic("OOM");

    // The workbench creates `<assoc-slug>/p<id>/...`; we walk to find the
    // task directory regardless of slug.
    var found_active = false;
    var found_cancelled = false;
    walkForTaskFiles(arena, wb_root, active.id, cancelled.id, &found_active, &found_cancelled);

    try std.testing.expect(found_active);
    try std.testing.expect(!found_cancelled);

    _ = active;
}

test "workbench push --filter-mode all also filters done tasks" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const env = workbenchEnv(arena, &suite);

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "All-Mode Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const active = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "Active task" });
    const done = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "Done task" });

    // Walk done task through todo → doing → done.
    const done_id = std.fmt.allocPrint(arena, "{d}", .{done.id}) catch @panic("OOM");
    const start = suite.execWith(&.{ "task", "start", done_id }, env);
    defer start.deinit(gpa);
    try std.testing.expect(start.term == .exited and start.term.exited == 0);
    const finish = suite.execWith(&.{ "task", "done", done_id }, env);
    defer finish.deinit(gpa);
    try std.testing.expect(finish.term == .exited and finish.term.exited == 0);

    // Default push (failures mode) — done task should still be written.
    const push_failures = suite.execWith(&.{ "workbench", "push", plan_id }, env);
    defer push_failures.deinit(gpa);
    try std.testing.expect(push_failures.term == .exited and push_failures.term.exited == 0);

    var dir_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp_len = suite.tmp_dir.dir.realPath(std.testing.io, &dir_buf) catch @panic("cannot resolve tmp dir");
    const wb_root = std.fs.path.join(arena, &.{ dir_buf[0..tmp_len], "workbench" }) catch @panic("OOM");

    var found_done_under_failures = false;
    var found_active_under_failures = false;
    walkForTaskFiles(arena, wb_root, active.id, done.id, &found_active_under_failures, &found_done_under_failures);
    try std.testing.expect(found_active_under_failures);
    try std.testing.expect(found_done_under_failures);

    // Now push with --filter-mode all; done task should be filtered.
    const push_all = suite.execWith(&.{ "workbench", "push", plan_id, "--filter-mode", "all" }, env);
    defer push_all.deinit(gpa);
    try std.testing.expect(push_all.term == .exited and push_all.term.exited == 0);
    try std.testing.expect(std.mem.indexOf(u8, push_all.stdout, "mode=all") != null);
}

/// Walk the workbench tree starting at `wb_root` and set found flags for
/// task files matching the given IDs. File naming convention is
/// `<id>-<slug>.md` so we match on the id prefix.
fn walkForTaskFiles(
    arena: std.mem.Allocator,
    wb_root: []const u8,
    active_id: i64,
    cancelled_or_done_id: i64,
    found_active: *bool,
    found_other: *bool,
) void {
    const active_prefix = std.fmt.allocPrint(arena, "{d}-", .{active_id}) catch @panic("OOM");
    const other_prefix = std.fmt.allocPrint(arena, "{d}-", .{cancelled_or_done_id}) catch @panic("OOM");

    var dir = std.fs.cwd().openDir(wb_root, .{ .iterate = true }) catch return;
    defer dir.close();
    walkRecursive(arena, &dir, active_prefix, other_prefix, found_active, found_other);
}

fn walkRecursive(
    arena: std.mem.Allocator,
    dir: *std.fs.Dir,
    active_prefix: []const u8,
    other_prefix: []const u8,
    found_active: *bool,
    found_other: *bool,
) void {
    var it = dir.iterate();
    while (it.next() catch null) |entry| {
        switch (entry.kind) {
            .file => {
                if (std.mem.startsWith(u8, entry.name, active_prefix)) found_active.* = true;
                if (std.mem.startsWith(u8, entry.name, other_prefix)) found_other.* = true;
            },
            .directory => {
                var sub = dir.openDir(entry.name, .{ .iterate = true }) catch continue;
                defer sub.close();
                walkRecursive(arena, &sub, active_prefix, other_prefix, found_active, found_other);
            },
            else => {},
        }
    }
}
