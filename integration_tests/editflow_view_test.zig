//! integration_tests/editflow_view_test.zig
//!
//! Smoke tests for the `<entity> view` flow (D-m4-view-flow): plan view smoke.
//!
//! Verifies that `planar plan view <id>` with PAGER=cat writes the entity's
//! rendered markdown to its workbench file and then cats it to stdout, and
//! that the output contains the expected frontmatter fields.
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

/// PlanJSON is the shape returned by `plan create --json`.
const PlanJSON = struct {
    id: i64,
    title: []const u8,
};

test "plan view: output contains title and entity_kind in frontmatter" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Build a writable workbench root under the suite's temp directory so we
    // don't pollute ~/.planar/workbench. We need an absolute path since
    // createDirAbsolute asserts it. Use the open tmp dir handle to resolve
    // its real path.
    var tmp_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp_len = suite.tmp_dir.dir.realPath(std.testing.io, &tmp_buf) catch @panic("cannot resolve tmp dir");
    const tmp_abs = tmp_buf[0..tmp_len];

    const wb_root = std.fs.path.join(arena, &.{
        tmp_abs,
        "workbench",
    }) catch @panic("OOM");

    // 1. Create a plan and capture the JSON output.
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "Smoke View Plan",
    });

    // 2. Run `plan view <id>` with PAGER=/bin/cat so the rendered markdown
    //    is written to stdout (captured by the harness), and with a controlled
    //    workbench root so we don't write to ~/.planar/workbench.
    var id_buf: [32]u8 = undefined;
    const id_str = std.fmt.bufPrint(&id_buf, "{d}", .{plan.id}) catch unreachable;

    const res = suite.execWith(
        &.{ "plan", "view", id_str },
        &.{
            .{ .key = "PAGER", .value = "/bin/cat" },
            .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
        },
    );
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // 3. Assert the command succeeded.
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "\nplan view failed\nstdout: {s}\nstderr: {s}\n",
            .{ res.stdout, res.stderr },
        );
        try std.testing.expect(false);
    }

    // 4. The output (from cat on the workbench file) must contain the plan
    //    title in the frontmatter and the entity_kind marker.
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "Smoke View Plan"));
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "entity_kind: plan"));
}
