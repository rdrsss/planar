//! workflow/dirs.zig — resolve shipped and sandbox workflow directories.
//!
//! Shipped workflows: `$PLANAR_WORKFLOWS_DIR` (test override) or
//! `$PLANAR_HOME/workflows/` (default `~/.planar/workflows/`).
//!
//! Sandbox workflows: `$PLANAR_HOME/local/workflows/` (default
//! `~/.planar/local/workflows/`).
//!
//! Neither directory is required to exist — an absent directory is
//! treated as an empty list.

const std = @import("std");
const runtime = @import("runtime");

pub const Dirs = struct {
    /// Absolute path to the shipped workflows directory (allocator-owned).
    shipped: []const u8,
    /// Absolute path to the sandbox workflows directory (allocator-owned).
    sandbox: []const u8,

    pub fn deinit(self: Dirs, allocator: std.mem.Allocator) void {
        allocator.free(self.shipped);
        allocator.free(self.sandbox);
    }
};

/// resolve derives both workflow directories from the process environment.
/// Returns allocator-owned strings; caller frees via `dirs.deinit(allocator)`.
pub fn resolve(ctx: *const runtime.Ctx) !Dirs {
    const allocator = ctx.allocator;
    const environ = ctx.environ;

    const planar_home = try planarHomeDir(environ, allocator);
    defer allocator.free(planar_home);

    // Shipped directory: PLANAR_WORKFLOWS_DIR (test override) or
    // $PLANAR_HOME/workflows/.
    const shipped = if (environ.getPosix("PLANAR_WORKFLOWS_DIR")) |v|
        try allocator.dupe(u8, v)
    else
        try std.fs.path.join(allocator, &.{ planar_home, "workflows" });
    errdefer allocator.free(shipped);

    // Sandbox directory: $PLANAR_HOME/local/workflows/.
    const sandbox = try std.fs.path.join(allocator, &.{ planar_home, "local", "workflows" });

    return .{ .shipped = shipped, .sandbox = sandbox };
}

/// planarHomeDir returns `$PLANAR_HOME` or `$HOME/.planar`, allocated on `allocator`.
pub fn planarHomeDir(environ: std.process.Environ, allocator: std.mem.Allocator) ![]const u8 {
    if (environ.getPosix("PLANAR_HOME")) |v| {
        return allocator.dupe(u8, v);
    }
    const home = environ.getPosix("HOME") orelse "/tmp";
    return std.fs.path.join(allocator, &.{ home, ".planar" });
}
