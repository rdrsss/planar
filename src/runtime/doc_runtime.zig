//! Minimal process context for planar-doc. Deliberately has no DB import.

const std = @import("std");
const Io = std.Io;

pub const Ctx = struct {
    allocator: std.mem.Allocator,
    io: Io,
    stdout: *Io.Writer,
    stderr: *Io.Writer,
    environ: std.process.Environ,
    argv: []const []const u8,
};

var current_ctx: ?Ctx = null;
var stdout_writer_storage: ?Io.File.Writer = null;
var stderr_writer_storage: ?Io.File.Writer = null;

pub fn init(
    allocator: std.mem.Allocator,
    io: Io,
    stdout_buf: []u8,
    stderr_buf: []u8,
    environ: std.process.Environ,
    argv: []const []const u8,
) void {
    stdout_writer_storage = Io.File.Writer.init(.stdout(), io, stdout_buf);
    stderr_writer_storage = Io.File.Writer.init(.stderr(), io, stderr_buf);
    current_ctx = .{
        .allocator = allocator,
        .io = io,
        .stdout = &stdout_writer_storage.?.interface,
        .stderr = &stderr_writer_storage.?.interface,
        .environ = environ,
        .argv = argv,
    };
}

pub fn current() *const Ctx {
    return &(current_ctx orelse @panic("planar-doc runtime not initialized"));
}

pub fn flush() !void {
    if (stdout_writer_storage) |*w| try w.interface.flush();
    if (stderr_writer_storage) |*w| try w.interface.flush();
}

pub fn shutdown() void {
    flush() catch {};
    current_ctx = null;
    stdout_writer_storage = null;
    stderr_writer_storage = null;
}
