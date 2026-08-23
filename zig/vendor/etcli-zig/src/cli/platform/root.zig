//! OS-isolated platform layer for the cli library.
//!
//! Currently only houses argv acquisition. Zig 0.16 exposes process
//! arguments through `std.process.Init.Minimal.args`; this wrapper turns
//! that source into a parser-friendly, allocator-owned `[]const []const u8`
//! on every platform.

const std = @import("std");

pub fn argv(
    allocator: std.mem.Allocator,
    args_source: std.process.Args,
) ![]const []const u8 {
    var count_it = try std.process.Args.Iterator.initAllocator(args_source, allocator);
    defer count_it.deinit();

    var count: usize = 0;
    while (count_it.next()) |_| count += 1;

    const out = try allocator.alloc([]const u8, count);
    errdefer allocator.free(out);

    var fill_it = try std.process.Args.Iterator.initAllocator(args_source, allocator);
    defer fill_it.deinit();

    var filled: usize = 0;
    errdefer {
        for (out[0..filled]) |arg| allocator.free(arg);
    }

    while (fill_it.next()) |arg| {
        out[filled] = try allocator.dupe(u8, arg);
        filled += 1;
    }

    return out;
}

/// Deep-copy a borrowed argv slice into allocator-owned storage. Pairs with
/// `freeArgv`. Exposed so the dupe + error-cleanup path is unit-testable
/// without a real process args source (which `argv` requires).
pub fn dupeArgv(allocator: std.mem.Allocator, args: []const []const u8) ![]const []const u8 {
    const out = try allocator.alloc([]const u8, args.len);
    errdefer allocator.free(out);

    var filled: usize = 0;
    errdefer for (out[0..filled]) |arg| allocator.free(arg);

    for (args) |arg| {
        out[filled] = try allocator.dupe(u8, arg);
        filled += 1;
    }
    return out;
}

pub fn freeArgv(allocator: std.mem.Allocator, args: []const []const u8) void {
    for (args) |arg| allocator.free(arg);
    allocator.free(args);
}

// ---- tests ----

test "dupeArgv deep-copies and round-trips through freeArgv" {
    const src: []const []const u8 = &.{ "tool", "task", "add", "--title", "hi" };
    const owned = try dupeArgv(std.testing.allocator, src);
    defer freeArgv(std.testing.allocator, owned);

    try std.testing.expectEqual(src.len, owned.len);
    for (src, owned) |a, b| {
        try std.testing.expectEqualStrings(a, b);
        // Deep copy: distinct backing storage, not an alias.
        try std.testing.expect(a.ptr != b.ptr);
    }
}

test "dupeArgv frees partial work when an allocation fails" {
    const src: []const []const u8 = &.{ "a", "b", "c", "d" };
    // Enough allocations to build `out` plus a few dupes, then force failure
    // on a later dupe so the errdefer cleanup paths run under the leak check.
    var failing = std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = 3 });
    try std.testing.expectError(error.OutOfMemory, dupeArgv(failing.allocator(), src));
}

test "dupeArgv handles an empty argv" {
    const owned = try dupeArgv(std.testing.allocator, &.{});
    defer freeArgv(std.testing.allocator, owned);
    try std.testing.expectEqual(@as(usize, 0), owned.len);
}
