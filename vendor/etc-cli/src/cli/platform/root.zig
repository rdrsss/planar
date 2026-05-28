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

pub fn freeArgv(allocator: std.mem.Allocator, args: []const []const u8) void {
    for (args) |arg| allocator.free(arg);
    allocator.free(args);
}
