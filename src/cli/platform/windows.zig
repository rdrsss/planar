//! Windows argv acquisition. Windows hands us argv as UTF-16 via
//! `GetCommandLineW`; the stdlib's `std.process.argsAlloc` does the
//! UTF-16 → UTF-8 conversion (allocator-backed, one allocation per
//! arg's UTF-8 buffer) so callers get a uniform `[][:0]u8` shape.

const std = @import("std");

pub fn argv(allocator: std.mem.Allocator) ![][:0]u8 {
    return std.process.argsAlloc(allocator);
}

pub fn freeArgv(allocator: std.mem.Allocator, args: [][:0]u8) void {
    std.process.argsFree(allocator, args);
}
