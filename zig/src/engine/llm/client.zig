//! engine/llm/client — cache/pending-file client for no-network LLM handoff.

const std = @import("std");

pub const CacheKind = enum {
    import_interpretation,
    bootstrap_synthesis,
};

pub fn subdir(kind: CacheKind) []const u8 {
    return switch (kind) {
        .import_interpretation => "import-interpretation",
        .bootstrap_synthesis => "bootstrap-synthesis",
    };
}

pub fn cacheDir(
    allocator: std.mem.Allocator,
    planar_home: []const u8,
    kind: CacheKind,
) ![]const u8 {
    if (planar_home.len == 0) return error.InvalidInput;
    return try std.fs.path.join(allocator, &.{ planar_home, "cache", subdir(kind) });
}

pub fn cachePath(
    allocator: std.mem.Allocator,
    planar_home: []const u8,
    kind: CacheKind,
    repo_slug: []const u8,
    fingerprint: []const u8,
) ![]const u8 {
    try validateSlug(repo_slug);
    if (!looksHex(fingerprint)) return error.InvalidInput;
    const dir = try cacheDir(allocator, planar_home, kind);
    defer allocator.free(dir);
    const file = try std.fmt.allocPrint(allocator, "{s}.json", .{fingerprint});
    defer allocator.free(file);
    return try std.fs.path.join(allocator, &.{ dir, repo_slug, file });
}

pub fn pendingPath(
    allocator: std.mem.Allocator,
    planar_home: []const u8,
    kind: CacheKind,
    repo_slug: []const u8,
) ![]const u8 {
    try validateSlug(repo_slug);
    const dir = try cacheDir(allocator, planar_home, kind);
    defer allocator.free(dir);
    return try std.fs.path.join(allocator, &.{ dir, repo_slug, "_pending.json" });
}

pub fn readIfExists(path: []const u8, allocator: std.mem.Allocator, max_bytes: usize) !?[]u8 {
    const data = std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(max_bytes)) catch |e| switch (e) {
        error.FileNotFound => return null,
        else => return e,
    };
    return data;
}

pub fn writeJsonAtomic(path: []const u8, json: []const u8, allocator: std.mem.Allocator) !void {
    if (path.len == 0) return error.InvalidInput;
    if (std.fs.path.dirname(path)) |parent| {
        try std.Io.Dir.cwd().createDirPath(fsIo(), parent);
    }
    const tmp = try std.fmt.allocPrint(allocator, "{s}.tmp", .{path});
    defer allocator.free(tmp);
    try std.Io.Dir.cwd().writeFile(fsIo(), .{ .sub_path = tmp, .data = json });
    try std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), path, fsIo());
}

pub fn sha256HexAlloc(allocator: std.mem.Allocator, bytes: []const u8) ![]const u8 {
    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &digest, .{});
    const out = try allocator.alloc(u8, digest.len * 2);
    for (digest, 0..) |b, i| {
        out[i * 2] = "0123456789abcdef"[b >> 4];
        out[i * 2 + 1] = "0123456789abcdef"[b & 0x0f];
    }
    return out;
}

pub fn looksHex(s: []const u8) bool {
    if (s.len == 0) return false;
    for (s) |c| switch (c) {
        '0'...'9', 'a'...'f', 'A'...'F' => {},
        else => return false,
    };
    return true;
}

pub fn validateSlug(slug: []const u8) !void {
    if (slug.len == 0) return error.InvalidInput;
    if (std.mem.indexOfAny(u8, slug, "/\\") != null) return error.InvalidInput;
    if (std.mem.indexOf(u8, slug, "..") != null) return error.InvalidInput;
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

test "cache paths stay inside planar home" {
    const gpa = std.testing.allocator;
    const cache = try cachePath(gpa, "/tmp/planar-home", .import_interpretation, "repo", "abc123");
    defer gpa.free(cache);
    try std.testing.expectEqualStrings("/tmp/planar-home/cache/import-interpretation/repo/abc123.json", cache);
    const pending = try pendingPath(gpa, "/tmp/planar-home", .bootstrap_synthesis, "repo");
    defer gpa.free(pending);
    try std.testing.expectEqualStrings("/tmp/planar-home/cache/bootstrap-synthesis/repo/_pending.json", pending);
}

test "validateSlug rejects traversal/path separators" {
    try std.testing.expectError(error.InvalidInput, validateSlug("../repo"));
    try std.testing.expectError(error.InvalidInput, validateSlug("a/b"));
    try std.testing.expectError(error.InvalidInput, validateSlug(""));
}
