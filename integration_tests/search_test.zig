//! integration_tests/search_test.zig
//!
//! Black-box integration tests for `planar search`.
//!
//! Verifies:
//!   - Happy path: create plan + task with a distinctive token; both appear
//!     in text output.
//!   - JSON happy path: same token; both appear in JSON array.
//!   - Empty / no-match: --json output is exactly "[]"; text output is
//!     exactly "(no results)".
//!   - Exit code 0 for all of the above (an empty result is not an error).
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
    title: []const u8,
};

const TaskJSON = struct {
    id: i64,
    title: []const u8,
};

const SearchHitJSON = struct {
    kind: []const u8,
    id: i64,
    title: []const u8,
    slug: []const u8 = "",
    status: []const u8 = "",
    snippet: []const u8 = "",
    rank: f64 = 0,
};

test "search happy path: plan and task with distinctive token appear in text output" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const token = "SEARCHTOKEN_INTEG_7789";

    // Create a plan with the token in its title.
    _ = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", token ++ " Integration Plan",
    });

    // Create a task with the token in its title.
    // The Zig binary uses `task add` (not `task create`).
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", token ++ " Integration Task",
    });

    // Run `planar search <token>` (text mode).
    const stdout = suite.mustRun(&.{ "search", token });
    defer gpa.free(stdout);

    // Both titles must appear in the text output.
    try std.testing.expect(std.mem.containsAtLeast(u8, stdout, 1, token ++ " Integration Plan"));
    try std.testing.expect(std.mem.containsAtLeast(u8, stdout, 1, token ++ " Integration Task"));
}

test "search JSON happy path: results array contains both kinds" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const token = "SEARCHTOKEN_JSON_4421";

    _ = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", token ++ " Plan",
    });
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", token ++ " Task",
    });

    // --json emits a JSON array.
    const stdout = suite.mustRun(&.{ "search", "--json", token });
    defer gpa.free(stdout);

    // Parse as a JSON array of hits.
    const hits = std.json.parseFromSlice([]SearchHitJSON, arena, std.mem.trim(u8, stdout, " \n"), .{
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nFailed to parse JSON array: {s}\nraw: {s}\n", .{ @errorName(e), stdout });
        try std.testing.expect(false);
        unreachable;
    };

    try std.testing.expect(hits.value.len >= 2);

    var found_plan = false;
    var found_task = false;
    for (hits.value) |h| {
        if (std.mem.eql(u8, h.kind, "plan")) found_plan = true;
        if (std.mem.eql(u8, h.kind, "task")) found_task = true;
    }
    try std.testing.expect(found_plan);
    try std.testing.expect(found_task);
}

test "search no-match JSON: exits 0 and stdout is exactly []" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = suite.exec(&.{ "search", "--json", "DEFINITELYNOTPRESENT_XYZZY_99999" });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // Must exit 0.
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "\nExpected exit 0 for no-match search, got: {any}\nstdout: {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        try std.testing.expect(false);
    }

    // stdout must be exactly "[]\n".
    const trimmed = std.mem.trim(u8, res.stdout, " \n");
    try std.testing.expectEqualStrings("[]", trimmed);
}

test "search no-match text: exits 0 and stdout is exactly (no results)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = suite.exec(&.{ "search", "DEFINITELYNOTPRESENT_XYZZY_88888" });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // Must exit 0.
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "\nExpected exit 0 for no-match search, got: {any}\nstdout: {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        try std.testing.expect(false);
    }

    // stdout must be exactly "(no results)\n".
    const trimmed = std.mem.trim(u8, res.stdout, " \n");
    try std.testing.expectEqualStrings("(no results)", trimmed);
}
