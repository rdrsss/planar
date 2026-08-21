//! integration_tests/schema_test.zig
//!
//! Pins the `planar schema` JSON-catalog contract. `schema` was added in
//! PR #13 across all four binaries but shipped without an integration
//! test for its output shape; this closes that gap.
//!
//! Also a regression guard for the worktree-gate fix: `schema` is a pure
//! `.rodata` catalog with no DB access, so it is classified
//! `execution_or_read` and must run from any cwd (the harness exercises
//! the binary directly).
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

/// The top-level shape `planar schema` emits. `ignore_unknown_fields`
/// lets us pin only the load-bearing keys without enumerating the full
/// per-command node.
const Catalog = struct {
    schemaVersion: i64,
    layout: []const u8,
    root: []const u8,
    commands: []struct {
        name: []const u8,
    },
};

test "schema: emits a non-empty versioned JSON catalog rooted at planar" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const cat = suite.mustRunJSON(Catalog, arena, &.{"schema"});

    try std.testing.expect(cat.schemaVersion >= 1);
    try std.testing.expectEqualStrings("planar", cat.root);
    try std.testing.expect(cat.commands.len > 0);

    // The catalog must contain the root node plus known top-level verbs.
    // Spot-check a couple so a truncated/garbled catalog fails loudly.
    var saw_root = false;
    var saw_plan = false;
    for (cat.commands) |c| {
        if (std.mem.eql(u8, c.name, "planar")) saw_root = true;
        if (std.mem.eql(u8, c.name, "plan")) saw_plan = true;
    }
    try std.testing.expect(saw_root);
    try std.testing.expect(saw_plan);
}
