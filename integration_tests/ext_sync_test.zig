//! integration_tests/ext_sync_test.zig
//!
//! Smoke checks for ext register/list/test flows.

const std = @import("std");
const harness = @import("harness");

const RegisterJSON = struct {
    id: i64,
    slug: []const u8,
    kind: []const u8,
};

const TestJSON = struct {
    slug: []const u8,
    ok: bool,
};

test "ext register github + ext test wiring" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const reg = suite.mustRunJSON(RegisterJSON, arena, &.{
        "ext",       "register",  "github",     "gh-demo",
        "--project", "acme/demo", "--auth-env", "PLANAR_TEST_GH_TOKEN",
        "--json",
    });
    try std.testing.expect(reg.id > 0);
    try std.testing.expectEqualStrings("gh-demo", reg.slug);
    try std.testing.expectEqualStrings("github-issues", reg.kind);

    const list = suite.mustRun(&.{ "ext", "list" });
    defer gpa.free(list);
    try std.testing.expect(std.mem.containsAtLeast(u8, list, 1, "gh-demo"));

    const test_out = suite.mustRunWith(
        &.{ "ext", "test", "gh-demo", "--json" },
        &.{.{ .key = "PLANAR_TEST_GH_TOKEN", .value = "dummy-token" }},
    );
    defer gpa.free(test_out);
    const parsed = std.json.parseFromSlice(TestJSON, arena, test_out, .{}) catch |e| {
        std.debug.print("ext test parse failed: {s}\nraw: {s}\n", .{ @errorName(e), test_out });
        try std.testing.expect(false);
        unreachable;
    };
    try std.testing.expectEqualStrings("gh-demo", parsed.value.slug);
    try std.testing.expect(parsed.value.ok);
}
