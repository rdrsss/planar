//! Operator-facing path resolution that preserves valid PWD spelling.
//!
//! `realPath` is useful for identity checks, but it must not choose the path
//! spelling stored in Planar. Shells preserve symlink components in PWD; using
//! that spelling keeps project roots, scope lookups, and staged requests on one
//! stable key. A stale or invalid PWD is rejected by comparing real paths.

const std = @import("std");

pub fn cwd(
    allocator: std.mem.Allocator,
    io: std.Io,
    environ: std.process.Environ,
) ![]u8 {
    return cwdFromPwd(allocator, io, environ.getPosix("PWD"));
}

pub fn cwdCurrent(allocator: std.mem.Allocator, io: std.Io) ![]u8 {
    return cwd(allocator, io, processEnviron());
}

/// Resolve `path` to an absolute lexical path without evaluating symlinks.
/// Relative paths are anchored to the validated PWD-first cwd.
pub fn absolute(
    allocator: std.mem.Allocator,
    io: std.Io,
    environ: std.process.Environ,
    path: []const u8,
) ![]u8 {
    if (path.len == 0) return error.InvalidPath;
    if (std.fs.path.isAbsolute(path)) return std.fs.path.resolve(allocator, &.{path});
    const base = try cwd(allocator, io, environ);
    defer allocator.free(base);
    return std.fs.path.resolve(allocator, &.{ base, path });
}

pub fn absoluteCurrent(allocator: std.mem.Allocator, io: std.Io, path: []const u8) ![]u8 {
    return absolute(allocator, io, processEnviron(), path);
}

fn cwdFromPwd(allocator: std.mem.Allocator, io: std.Io, pwd: ?[]const u8) ![]u8 {
    const real_cwd_z = try std.Io.Dir.realPathFileAlloc(.cwd(), io, ".", allocator);
    defer allocator.free(real_cwd_z);
    const real_cwd = try allocator.dupe(u8, real_cwd_z);
    errdefer allocator.free(real_cwd);
    if (pwd) |candidate| {
        if (candidate.len > 0 and std.fs.path.isAbsolute(candidate)) {
            const real_pwd = std.Io.Dir.realPathFileAlloc(.cwd(), io, candidate, allocator) catch null;
            defer if (real_pwd) |p| allocator.free(p);
            if (real_pwd) |p| {
                if (std.mem.eql(u8, p, real_cwd)) {
                    allocator.free(real_cwd);
                    return allocator.dupe(u8, candidate);
                }
            }
        }
    }
    return real_cwd;
}

fn processEnviron() std.process.Environ {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var count: usize = 0;
    while (raw[count] != null) : (count += 1) {}
    const slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..count :null]);
    return .{ .block = .{ .slice = slice } };
}

test "cwd preserves a symlink-spelled PWD only when it resolves to the process cwd" {
    const allocator = std.testing.allocator;
    const io = std.testing.io;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const current = try std.Io.Dir.realPathFileAlloc(.cwd(), io, ".", allocator);
    defer allocator.free(current);
    var tmp_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp_len = try tmp.dir.realPath(io, &tmp_buf);
    const link = try std.fs.path.join(allocator, &.{ tmp_buf[0..tmp_len], "cwd-link" });
    defer allocator.free(link);
    try std.Io.Dir.cwd().symLink(io, current, link, .{ .is_directory = true });

    const preserved = try cwdFromPwd(allocator, io, link);
    defer allocator.free(preserved);
    try std.testing.expectEqualStrings(link, preserved);

    const stale = try cwdFromPwd(allocator, io, tmp_buf[0..tmp_len]);
    defer allocator.free(stale);
    try std.testing.expectEqualStrings(current, stale);
}
