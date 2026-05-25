//! handlers/config/path.zig — `planar config path`
//!
//! Print the resolved config file path to stdout. Exit 0.
//!
//! Resolution order (mirrors Go's config/path.go):
//!   1. $PLANAR_CONFIG_PATH (with ~ expansion)
//!   2. $HOME/.planar/config.toml

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    _ = cli.castArgs(main.root, &.{ "config", "path" }, args_ptr);
    const ctx = runtime.current();

    const path = resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
    defer ctx.allocator.free(path);

    try ctx.stdout.print("{s}\n", .{path});
}

/// Resolve the effective config file path.
/// Mirrors Go's config.ResolvePath():
///   1. $PLANAR_CONFIG_PATH (with ~ expansion)
///   2. $HOME/.planar/config.toml
pub fn resolveConfigPath(allocator: std.mem.Allocator, environ: std.process.Environ) ![]u8 {
    if (environ.getPosix("PLANAR_CONFIG_PATH")) |raw| {
        if (raw.len > 0) {
            return expandTilde(allocator, raw, environ);
        }
    }
    const home = environ.getPosix("HOME") orelse return error.HomeNotSet;
    return std.fs.path.join(allocator, &.{ home, ".planar", "config.toml" });
}

/// Expand a leading ~ to the home directory. Returns an owned copy.
fn expandTilde(allocator: std.mem.Allocator, path: []const u8, environ: std.process.Environ) ![]u8 {
    if (std.mem.eql(u8, path, "~")) {
        const home = environ.getPosix("HOME") orelse return error.HomeNotSet;
        return allocator.dupe(u8, home);
    }
    if (std.mem.startsWith(u8, path, "~/")) {
        const home = environ.getPosix("HOME") orelse return error.HomeNotSet;
        return std.fs.path.join(allocator, &.{ home, path[2..] });
    }
    return allocator.dupe(u8, path);
}
