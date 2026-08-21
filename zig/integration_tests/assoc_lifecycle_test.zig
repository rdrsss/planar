//! integration_tests/assoc_lifecycle_test.zig
//!
//! Covers the association read/detect/remove verbs that had no black-box
//! test: `assoc list`, `assoc members`, `assoc detect`, and `assoc remove`
//! (the membership-removal inverse of `assoc add`).
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

const Assoc = struct { slug: []const u8, kind: []const u8 };

test "scenario: assoc list / members / detect / remove" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("assoc-lifecycle");
    suite.addAssoc("acme", "org"); // assoc create acme --kind org + assoc add acme <root>

    // ---- assoc list: the new association is present.
    const list_raw = suite.mustRun(&.{ "assoc", "list", "--json" });
    defer gpa.free(list_raw);
    const list = std.json.parseFromSlice([]Assoc, arena, list_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    var saw_acme = false;
    for (list.value) |a| if (std.mem.eql(u8, a.slug, "acme")) {
        saw_acme = true;
    };
    try std.testing.expect(saw_acme);

    // ---- assoc members: the registered repo root is a member.
    const members = suite.mustRun(&.{ "assoc", "members", "acme" });
    defer gpa.free(members);
    try std.testing.expect(std.mem.containsAtLeast(u8, members, 1, root));

    // ---- assoc detect: runs from the project dir, exits cleanly.
    const detect = suite.mustRunInDir(root, &.{ "assoc", "detect", "--json" });
    gpa.free(detect);

    // ---- assoc remove: drop the membership, then it is gone from members.
    const removed = suite.mustRun(&.{ "assoc", "remove", "acme", root });
    gpa.free(removed);
    const members_after = suite.mustRun(&.{ "assoc", "members", "acme" });
    defer gpa.free(members_after);
    try std.testing.expect(!std.mem.containsAtLeast(u8, members_after, 1, root));
}
