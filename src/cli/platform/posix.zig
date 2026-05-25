//! POSIX argv acquisition. argv is already UTF-8 on Linux/macOS, so
//! `std.process.argsAlloc` only allocates the slice header (~16 bytes
//! per arg pointer); the strings themselves are borrowed from process
//! memory and not copied.

const std = @import("std");

pub fn argv(allocator: std.mem.Allocator) ![][:0]u8 {
    return std.process.argsAlloc(allocator);
}

pub fn freeArgv(allocator: std.mem.Allocator, args: [][:0]u8) void {
    std.process.argsFree(allocator, args);
}
