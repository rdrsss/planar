//! integration_tests/plan_update_test.zig — plan update parity checks.

const std = @import("std");
const harness = @import("harness");

test "plan update changes slug without changing status" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const CreateJSON = struct {
        id: i64,
        slug: []const u8,
        status: []const u8,
    };
    const ShowJSON = struct {
        slug: []const u8,
        status: []const u8,
    };

    const created = suite.mustRunJSON(CreateJSON, arena, &.{
        "plan", "create", "--json", "--slug", "old-plan-slug", "Slug Update Plan",
    });
    try std.testing.expectEqualStrings("old-plan-slug", created.slug);

    const id_s = try std.fmt.allocPrint(arena, "{d}", .{created.id});
    const update_out = suite.mustRun(&.{ "plan", "update", id_s, "--slug", "new-plan-slug" });
    defer gpa.free(update_out);

    const shown = suite.mustRunJSON(ShowJSON, arena, &.{ "plan", "show", "--json", id_s });
    try std.testing.expectEqualStrings("new-plan-slug", shown.slug);
    try std.testing.expectEqualStrings("draft", shown.status);
}
