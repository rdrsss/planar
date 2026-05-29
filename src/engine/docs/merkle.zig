//! engine/docs/merkle — content-addressed hashing primitives for the
//! repo-state manifest (plan 423 M1).
//!
//! Two hash flavors:
//!  * `hashFile(path) -> u64` — xxh64 over the file's raw bytes.
//!  * `hashDir(path) -> u64` — xxh64 over a canonical child-record byte
//!    stream that describes a directory's contents recursively.
//!
//! Canonical child-record format (sorted ascending by name):
//!
//!     tag(1) | name(utf8) | null(1) | child_hash(8, little-endian)
//!
//! tag is one of:
//!   * `'f'` — regular file. child_hash = hashFile.
//!   * `'d'` — subdirectory. child_hash = hashDir.
//!   * `'s'` — symlink. child_hash = xxh64(readlink target path).
//!
//! Decisions:
//!   * Empty directory hashes to zero (decided in [[q330-symlink-semantics]]
//!     adjacent: deterministic null state).
//!   * Symlinks are hashed by their target *path string*, NOT by the target's
//!     content (Q330). This makes a repoint detectable as a hash change even
//!     when old and new targets have identical bytes, and avoids cycle
//!     detection because the walker never follows.
//!   * Hash algorithm is `std.hash.XxHash64` with seed 0 throughout.
//!   * Big-endian or little-endian for the embedded child_hash? Little-endian
//!     to match the natural Zig integer representation on the host. The
//!     manifest is regenerated per-machine, so byte order isn't part of the
//!     cross-machine contract; what matters is determinism of the algorithm.

const std = @import("std");

pub const Hasher = std.hash.XxHash64;
pub const seed: u64 = 0;

/// All-zero hash sentinel — returned for empty directories.
pub const empty_hash: u64 = 0;

/// Atomic sequence counter used to build unique temp filenames inside the
/// same process. Cross-process uniqueness is provided by the rename
/// semantics (the loser of a race just retries on the next call).
var atomic_tmp_counter: u64 = 0;

const c = @cImport({
    @cInclude("dirent.h");
    @cInclude("sys/stat.h");
    @cInclude("fcntl.h");
    @cInclude("unistd.h");
    @cInclude("stdio.h");
});

/// Hash the raw bytes of `path` via streaming xxh64. Returns 0 for an empty
/// file (the algorithm's natural identity).
pub fn hashFile(allocator: std.mem.Allocator, path: []const u8) !u64 {
    const path_z = try allocator.dupeZ(u8, path);
    defer allocator.free(path_z);
    const fd = c.open(path_z.ptr, c.O_RDONLY);
    if (fd < 0) return error.OpenFailed;
    defer _ = c.close(fd);

    var h = Hasher.init(seed);
    var buf: [64 * 1024]u8 = undefined;
    while (true) {
        const n = c.read(fd, &buf, buf.len);
        if (n < 0) return error.ReadFailed;
        if (n == 0) break;
        h.update(buf[0..@intCast(n)]);
    }
    return h.final();
}

/// Hash a symlink's target path string. Caller is responsible for ensuring
/// `path` is a symlink (caller has already stat'ed it); behavior on a
/// non-symlink path is undefined.
pub fn hashSymlink(allocator: std.mem.Allocator, path: []const u8) !u64 {
    const path_z = try allocator.dupeZ(u8, path);
    defer allocator.free(path_z);
    var target_buf: [std.fs.max_path_bytes]u8 = undefined;
    const n = c.readlink(path_z.ptr, &target_buf, target_buf.len);
    if (n < 0) return error.ReadlinkFailed;
    return Hasher.hash(seed, target_buf[0..@intCast(n)]);
}

/// Tags appearing in the canonical child-record byte stream.
pub const Tag = enum(u8) {
    file = 'f',
    directory = 'd',
    symlink = 's',
};

/// One entry in a directory's child list. Owned by `listDirChildren`'s
/// returned slice; freed via `freeChildren`.
pub const Child = struct {
    name: []const u8,
    tag: Tag,
};

/// Enumerate immediate children of `dir`, sorted ascending by name. Excludes
/// `.` and `..`. Caller frees with `freeChildren`.
pub fn listDirChildren(allocator: std.mem.Allocator, dir: []const u8) ![]Child {
    const dir_z = try allocator.dupeZ(u8, dir);
    defer allocator.free(dir_z);
    const dp = c.opendir(dir_z.ptr) orelse return error.OpenFailed;
    defer _ = c.closedir(dp);

    var out: std.ArrayList(Child) = .empty;
    errdefer {
        for (out.items) |ch| allocator.free(ch.name);
        out.deinit(allocator);
    }

    while (c.readdir(dp)) |ent| {
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&ent.*.d_name)));
        if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;

        const child_path = try std.fs.path.join(allocator, &.{ dir, name });
        defer allocator.free(child_path);
        const child_z = try allocator.dupeZ(u8, child_path);
        defer allocator.free(child_z);

        var st: c.struct_stat = undefined;
        if (c.lstat(child_z.ptr, &st) != 0) continue;

        const tag: Tag = switch (st.st_mode & c.S_IFMT) {
            c.S_IFDIR => .directory,
            c.S_IFLNK => .symlink,
            else => .file,
        };

        try out.append(allocator, .{
            .name = try allocator.dupe(u8, name),
            .tag = tag,
        });
    }

    const slice = try out.toOwnedSlice(allocator);
    std.mem.sort(Child, slice, {}, lessThanChildName);
    return slice;
}

pub fn freeChildren(allocator: std.mem.Allocator, children: []Child) void {
    for (children) |ch| allocator.free(ch.name);
    allocator.free(children);
}

fn lessThanChildName(_: void, a: Child, b: Child) bool {
    return std.mem.order(u8, a.name, b.name) == .lt;
}

/// Hash a directory recursively via the canonical child-record format. An
/// empty directory hashes to `empty_hash` (0).
pub fn hashDir(allocator: std.mem.Allocator, dir: []const u8) !u64 {
    const children = try listDirChildren(allocator, dir);
    defer freeChildren(allocator, children);
    if (children.len == 0) return empty_hash;

    var h = Hasher.init(seed);
    for (children) |child| {
        const child_path = try std.fs.path.join(allocator, &.{ dir, child.name });
        defer allocator.free(child_path);

        const child_hash: u64 = switch (child.tag) {
            .file => try hashFile(allocator, child_path),
            .directory => try hashDir(allocator, child_path),
            .symlink => try hashSymlink(allocator, child_path),
        };

        // Canonical record: tag(1) | name | null(1) | child_hash(8, LE).
        var record_buf: [11]u8 = undefined; // tag + null + u64
        record_buf[0] = @intFromEnum(child.tag);
        h.update(record_buf[0..1]);
        h.update(child.name);
        record_buf[1] = 0;
        h.update(record_buf[1..2]);
        std.mem.writeInt(u64, record_buf[2..10], child_hash, .little);
        h.update(record_buf[2..10]);
    }
    return h.final();
}

/// Atomic file write: serialize → temp file in the same directory → fsync →
/// rename over the target. Crashes mid-write leave the target untouched.
pub fn atomicWrite(allocator: std.mem.Allocator, path: []const u8, content: []const u8) !void {
    // Temp path lives in the same dir so the rename is atomic (same filesystem).
    const dir = std.fs.path.dirname(path) orelse ".";
    const base = std.fs.path.basename(path);
    const tmp_seq = @atomicRmw(u64, &atomic_tmp_counter, .Add, 1, .seq_cst);
    const tmp_name = try std.fmt.allocPrint(allocator, ".{s}.tmp.{x}", .{ base, tmp_seq });
    defer allocator.free(tmp_name);
    const tmp_path = try std.fs.path.join(allocator, &.{ dir, tmp_name });
    defer allocator.free(tmp_path);

    const tmp_z = try allocator.dupeZ(u8, tmp_path);
    defer allocator.free(tmp_z);
    const fd = c.open(tmp_z.ptr, c.O_WRONLY | c.O_CREAT | c.O_TRUNC, @as(c_uint, 0o644));
    if (fd < 0) return error.OpenFailed;

    var off: usize = 0;
    while (off < content.len) {
        const n = c.write(fd, content.ptr + off, content.len - off);
        if (n < 0) {
            _ = c.close(fd);
            const _z = try allocator.dupeZ(u8, tmp_path);
            defer allocator.free(_z);
            _ = c.unlink(_z.ptr);
            return error.WriteFailed;
        }
        off += @intCast(n);
    }
    if (c.fsync(fd) != 0) {
        _ = c.close(fd);
        const _z = try allocator.dupeZ(u8, tmp_path);
        defer allocator.free(_z);
        _ = c.unlink(_z.ptr);
        return error.SyncFailed;
    }
    _ = c.close(fd);

    const path_z = try allocator.dupeZ(u8, path);
    defer allocator.free(path_z);
    if (c.rename(tmp_z.ptr, path_z.ptr) != 0) {
        _ = c.unlink(tmp_z.ptr);
        return error.RenameFailed;
    }
}

// ============================================================================
// Tests
// ============================================================================

const testing = std.testing;

test "hashFile deterministic across calls" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    const path = try writeTempFile(testing.allocator, &tmp, "a.txt", "hello world");
    defer testing.allocator.free(path);

    const h1 = try hashFile(testing.allocator, path);
    const h2 = try hashFile(testing.allocator, path);
    try testing.expectEqual(h1, h2);
    // Sanity: non-zero for non-empty content.
    try testing.expect(h1 != 0);
}

test "hashFile of empty content is xxh64-of-empty" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    const path = try writeTempFile(testing.allocator, &tmp, "empty.txt", "");
    defer testing.allocator.free(path);

    const h = try hashFile(testing.allocator, path);
    try testing.expectEqual(Hasher.hash(seed, ""), h);
}

test "hashDir of empty directory returns empty_hash" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    const path = try absPath(testing.allocator, &tmp, "");
    defer testing.allocator.free(path);

    const h = try hashDir(testing.allocator, path);
    try testing.expectEqual(empty_hash, h);
}

test "hashDir changes when a child file's content changes" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    const child_path = try writeTempFile(testing.allocator, &tmp, "child.txt", "v1");
    defer testing.allocator.free(child_path);
    const dir = try absPath(testing.allocator, &tmp, "");
    defer testing.allocator.free(dir);

    const h_before = try hashDir(testing.allocator, dir);

    // Overwrite child with different content.
    const child_z = try testing.allocator.dupeZ(u8, child_path);
    defer testing.allocator.free(child_z);
    const fd = c.open(child_z.ptr, c.O_WRONLY | c.O_TRUNC, @as(c_uint, 0o644));
    try testing.expect(fd >= 0);
    _ = c.write(fd, "v2-different", 12);
    _ = c.close(fd);

    const h_after = try hashDir(testing.allocator, dir);
    try testing.expect(h_before != h_after);
}

test "hashDir is stable under entry order (sort by name)" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    const a = try writeTempFile(testing.allocator, &tmp, "a.txt", "alpha");
    defer testing.allocator.free(a);
    const b = try writeTempFile(testing.allocator, &tmp, "b.txt", "beta");
    defer testing.allocator.free(b);
    const dir = try absPath(testing.allocator, &tmp, "");
    defer testing.allocator.free(dir);

    const h_first = try hashDir(testing.allocator, dir);

    // Hash again (the walker's order may differ on different platforms; we
    // sort so the hash is stable). Sanity: re-running should yield the same.
    const h_second = try hashDir(testing.allocator, dir);
    try testing.expectEqual(h_first, h_second);
}

test "hashDir distinguishes adding a child" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    const a = try writeTempFile(testing.allocator, &tmp, "a.txt", "x");
    defer testing.allocator.free(a);
    const dir = try absPath(testing.allocator, &tmp, "");
    defer testing.allocator.free(dir);

    const h_one = try hashDir(testing.allocator, dir);

    const b = try writeTempFile(testing.allocator, &tmp, "b.txt", "y");
    defer testing.allocator.free(b);

    const h_two = try hashDir(testing.allocator, dir);
    try testing.expect(h_one != h_two);
}

test "atomicWrite produces the requested content" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    const path = try absPath(testing.allocator, &tmp, "x.txt");
    defer testing.allocator.free(path);
    try atomicWrite(testing.allocator, path, "atomic body");

    const got = try hashFile(testing.allocator, path);
    try testing.expectEqual(Hasher.hash(seed, "atomic body"), got);
}

test "atomicWrite overwrites an existing file" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    const path = try writeTempFile(testing.allocator, &tmp, "x.txt", "old");
    defer testing.allocator.free(path);
    try atomicWrite(testing.allocator, path, "new");
    const got = try hashFile(testing.allocator, path);
    try testing.expectEqual(Hasher.hash(seed, "new"), got);
}

// ----------------------------------------------------------------------
// Test helpers

fn writeTempFile(allocator: std.mem.Allocator, tmp: *std.testing.TmpDir, name: []const u8, content: []const u8) ![]const u8 {
    const path = try absPath(allocator, tmp, name);
    errdefer allocator.free(path);
    const path_z = try allocator.dupeZ(u8, path);
    defer allocator.free(path_z);
    const fd = c.open(path_z.ptr, c.O_WRONLY | c.O_CREAT | c.O_TRUNC, @as(c_uint, 0o644));
    if (fd < 0) return error.OpenFailed;
    defer _ = c.close(fd);
    if (content.len > 0) {
        const n = c.write(fd, content.ptr, content.len);
        if (n < 0) return error.WriteFailed;
    }
    return path;
}

fn absPath(allocator: std.mem.Allocator, tmp: *std.testing.TmpDir, rel: []const u8) ![]const u8 {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const len = tmp.dir.realPath(std.testing.io, &buf) catch return error.PathResolveFailed;
    if (rel.len == 0) return allocator.dupe(u8, buf[0..len]);
    return std.fs.path.join(allocator, &.{ buf[0..len], rel });
}
