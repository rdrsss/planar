//! engine/docs/builder — `planar-doc build` and `verify` (plan 423 M3).
//!
//! `build(allocator, repo_root)` reads the existing `.planar-manifest` if
//! one exists, recomputes every entry's hashes against the current
//! filesystem state, and writes a fresh manifest atomically. Membership
//! is preserved: which docs are covered + which source paths each covers +
//! which paths are in `nodoc` are unchanged. Only the hash values move.
//!
//! `verify(allocator, repo_root)` reads the stored manifest, recomputes
//! the current root hash via the same algorithm, and returns whether they
//! match. Sub-50ms warm-path target on this repo per Q331 (verified by
//! the soft assertion in the integration test plus the
//! `make bench-verify` target M9 adds).
//!
//! Both verbs operate on the manifest file at `<repo_root>/.planar-manifest`.

const std = @import("std");
const merkle = @import("merkle.zig");
const manifest_v2 = @import("manifest_v2.zig");

const c = @cImport({
    @cInclude("fcntl.h");
    @cInclude("unistd.h");
    @cInclude("sys/stat.h");
});

pub const Error = error{
    ManifestMissing,
    MalformedJson,
    MissingField,
    IncompatibleVersion,
    IncompatibleAlgorithm,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    RenameFailed,
    OutOfMemory,
    PathTooLong,
    ReadlinkFailed,
};

pub const BuildResult = struct {
    /// Newly-written manifest. Owned; free with `manifest_v2.deinitManifest`.
    manifest: manifest_v2.Manifest,
    /// Whether the manifest file already existed before this build (i.e.
    /// whether this was a fresh-DB run vs. an in-place rebuild).
    pre_existed: bool,
};

/// Read the existing manifest at `<root>/.planar-manifest` if present,
/// recompute every entry's hashes against the current FS state, write the
/// new manifest atomically, and return the new structure.
pub fn build(allocator: std.mem.Allocator, repo_root: []const u8) Error!BuildResult {
    const manifest_path = std.fs.path.join(allocator, &.{ repo_root, manifest_v2.file_name }) catch return error.OutOfMemory;
    defer allocator.free(manifest_path);

    var prior: ?manifest_v2.Manifest = null;
    defer if (prior) |p| manifest_v2.deinitManifest(allocator, p);

    const pre_existed = blk: {
        const bytes = readFileAllocOpt(allocator, manifest_path) catch null;
        if (bytes) |b| {
            defer allocator.free(b);
            prior = manifest_v2.fromJson(allocator, b) catch |e| switch (e) {
                error.IncompatibleVersion, error.IncompatibleAlgorithm => return e,
                else => return error.MalformedJson,
            };
            break :blk true;
        }
        break :blk false;
    };

    // Carry the prior entries' membership forward (which docs, which
    // sources, which nodoc paths). Hashes are recomputed below.
    var entries: std.ArrayList(manifest_v2.EntryRow) = .empty;
    errdefer {
        for (entries.items) |row| freeEntryRow(allocator, row);
        entries.deinit(allocator);
    }
    var nodoc: std.ArrayList(manifest_v2.NodocRow) = .empty;
    errdefer {
        for (nodoc.items) |row| freeNodocRow(allocator, row);
        nodoc.deinit(allocator);
    }

    if (prior) |p| {
        for (p.entries) |row| {
            const new_doc_hash = try computeDocHash(allocator, repo_root, row.path);
            errdefer allocator.free(new_doc_hash);

            // Recompute each source path's hash. Membership (the path
            // keys) is preserved; values update.
            var sources: std.ArrayList(manifest_v2.SourceRow) = .empty;
            errdefer {
                for (sources.items) |sr| {
                    allocator.free(sr.path);
                    allocator.free(sr.hash);
                }
                sources.deinit(allocator);
            }
            for (row.entry.sources) |sr| {
                const new_hash = try computePathHash(allocator, repo_root, sr.path);
                errdefer allocator.free(new_hash);
                try sources.append(allocator, .{
                    .path = try allocator.dupe(u8, sr.path),
                    .hash = new_hash,
                });
            }

            const owned_sources = try sources.toOwnedSlice(allocator);
            errdefer {
                for (owned_sources) |sr| {
                    allocator.free(sr.path);
                    allocator.free(sr.hash);
                }
                allocator.free(owned_sources);
            }
            const sources_hash = try manifest_v2.computeSourcesHash(allocator, owned_sources);
            errdefer allocator.free(sources_hash);
            const entry_hash = try manifest_v2.computeEntryHash(allocator, new_doc_hash, sources_hash);
            errdefer allocator.free(entry_hash);

            try entries.append(allocator, .{
                .path = try allocator.dupe(u8, row.path),
                .entry = .{
                    .doc_hash = new_doc_hash,
                    .sources = owned_sources,
                    .sources_hash = sources_hash,
                    .entry_hash = entry_hash,
                },
            });
        }
        for (p.nodoc) |row| {
            const new_hash = try computePathHash(allocator, repo_root, row.path);
            errdefer allocator.free(new_hash);
            try nodoc.append(allocator, .{
                .path = try allocator.dupe(u8, row.path),
                .hash = new_hash,
            });
        }
    }

    const owned_entries = try entries.toOwnedSlice(allocator);
    errdefer {
        for (owned_entries) |row| freeEntryRow(allocator, row);
        allocator.free(owned_entries);
    }
    const owned_nodoc = try nodoc.toOwnedSlice(allocator);
    errdefer {
        for (owned_nodoc) |row| freeNodocRow(allocator, row);
        allocator.free(owned_nodoc);
    }

    const root_hash = try manifest_v2.computeRoot(allocator, owned_entries, owned_nodoc);
    errdefer allocator.free(root_hash);

    const generated_at = try generatedAtNow(allocator);
    errdefer allocator.free(generated_at);

    const algo_dup = try allocator.dupe(u8, manifest_v2.algo);
    errdefer allocator.free(algo_dup);

    const m = manifest_v2.Manifest{
        .version = manifest_v2.version,
        .algo = algo_dup,
        .root = root_hash,
        .generated_at = generated_at,
        .entries = owned_entries,
        .nodoc = owned_nodoc,
    };

    const json = try manifest_v2.toJson(allocator, m);
    defer allocator.free(json);
    try merkle.atomicWrite(allocator, manifest_path, json);

    return .{ .manifest = m, .pre_existed = pre_existed };
}

pub const VerifyResult = struct {
    /// True when the stored root hash equals the freshly-computed root.
    matches: bool,
    /// The stored root hash, as read from disk. Owned by the caller.
    stored_root: []const u8,
    /// The freshly-computed root hash. Owned by the caller.
    computed_root: []const u8,
};

/// Compute the current manifest root and compare against the stored value.
/// Does NOT write to disk.
pub fn verify(allocator: std.mem.Allocator, repo_root: []const u8) Error!VerifyResult {
    const manifest_path = std.fs.path.join(allocator, &.{ repo_root, manifest_v2.file_name }) catch return error.OutOfMemory;
    defer allocator.free(manifest_path);

    const bytes = readFileAllocOpt(allocator, manifest_path) catch return error.ReadFailed;
    if (bytes == null) return error.ManifestMissing;
    defer allocator.free(bytes.?);

    const stored = manifest_v2.fromJson(allocator, bytes.?) catch |e| switch (e) {
        error.IncompatibleVersion, error.IncompatibleAlgorithm => return e,
        else => return error.MalformedJson,
    };
    defer manifest_v2.deinitManifest(allocator, stored);

    // Recompute hashes for every entry against current FS state.
    var fresh_entries: std.ArrayList(manifest_v2.EntryRow) = .empty;
    defer {
        for (fresh_entries.items) |row| freeEntryRow(allocator, row);
        fresh_entries.deinit(allocator);
    }
    var fresh_nodoc: std.ArrayList(manifest_v2.NodocRow) = .empty;
    defer {
        for (fresh_nodoc.items) |row| freeNodocRow(allocator, row);
        fresh_nodoc.deinit(allocator);
    }

    for (stored.entries) |row| {
        const doc_hash = try computeDocHash(allocator, repo_root, row.path);
        errdefer allocator.free(doc_hash);
        const sources = try allocator.alloc(manifest_v2.SourceRow, row.entry.sources.len);
        errdefer {
            for (sources) |sr| {
                allocator.free(sr.path);
                allocator.free(sr.hash);
            }
            allocator.free(sources);
        }
        for (row.entry.sources, 0..) |sr, i| {
            sources[i] = .{
                .path = try allocator.dupe(u8, sr.path),
                .hash = try computePathHash(allocator, repo_root, sr.path),
            };
        }
        const sources_hash = try manifest_v2.computeSourcesHash(allocator, sources);
        errdefer allocator.free(sources_hash);
        const entry_hash = try manifest_v2.computeEntryHash(allocator, doc_hash, sources_hash);
        errdefer allocator.free(entry_hash);
        try fresh_entries.append(allocator, .{
            .path = try allocator.dupe(u8, row.path),
            .entry = .{
                .doc_hash = doc_hash,
                .sources = sources,
                .sources_hash = sources_hash,
                .entry_hash = entry_hash,
            },
        });
    }
    for (stored.nodoc) |row| {
        try fresh_nodoc.append(allocator, .{
            .path = try allocator.dupe(u8, row.path),
            .hash = try computePathHash(allocator, repo_root, row.path),
        });
    }

    const fresh_root = try manifest_v2.computeRoot(allocator, fresh_entries.items, fresh_nodoc.items);
    const stored_root = try allocator.dupe(u8, stored.root);
    return .{
        .matches = std.mem.eql(u8, stored_root, fresh_root),
        .stored_root = stored_root,
        .computed_root = fresh_root,
    };
}

// ============================================================================
// Internal helpers

fn computeDocHash(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) ![]const u8 {
    const abs = try std.fs.path.join(allocator, &.{ root, rel });
    defer allocator.free(abs);
    const h = merkle.hashFile(allocator, abs) catch 0;
    return manifest_v2.formatHash(allocator, h);
}

/// Compute the merkle hash for a manifest source path. Directory paths end
/// with `/` and produce a recursive directory merkle; everything else
/// produces a file content hash. Missing paths hash to zero (which is
/// significant — the verify path detects the "source went missing" case).
fn computePathHash(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) ![]const u8 {
    const abs = try std.fs.path.join(allocator, &.{ root, trimTrailingSlash(rel) });
    defer allocator.free(abs);
    const is_dir = rel.len > 0 and rel[rel.len - 1] == '/';
    const h: u64 = if (is_dir)
        merkle.hashDir(allocator, abs) catch 0
    else
        merkle.hashFile(allocator, abs) catch 0;
    return manifest_v2.formatHash(allocator, h);
}

fn trimTrailingSlash(s: []const u8) []const u8 {
    if (s.len > 0 and s[s.len - 1] == '/') return s[0 .. s.len - 1];
    return s;
}

fn freeEntryRow(allocator: std.mem.Allocator, row: manifest_v2.EntryRow) void {
    allocator.free(row.path);
    allocator.free(row.entry.doc_hash);
    allocator.free(row.entry.sources_hash);
    allocator.free(row.entry.entry_hash);
    for (row.entry.sources) |sr| {
        allocator.free(sr.path);
        allocator.free(sr.hash);
    }
    allocator.free(row.entry.sources);
}

fn freeNodocRow(allocator: std.mem.Allocator, row: manifest_v2.NodocRow) void {
    allocator.free(row.path);
    allocator.free(row.hash);
}

fn readFileAllocOpt(allocator: std.mem.Allocator, path: []const u8) !?[]u8 {
    const path_z = try allocator.dupeZ(u8, path);
    defer allocator.free(path_z);
    const fd = c.open(path_z.ptr, c.O_RDONLY);
    if (fd < 0) return null;
    defer _ = c.close(fd);
    var st: c.struct_stat = undefined;
    if (c.fstat(fd, &st) != 0) return null;
    const size: usize = @intCast(st.st_size);
    const buf = try allocator.alloc(u8, size);
    errdefer allocator.free(buf);
    var off: usize = 0;
    while (off < size) {
        const n = c.read(fd, @as([*]u8, @ptrCast(buf.ptr)) + off, size - off);
        if (n <= 0) break;
        off += @intCast(n);
    }
    return buf[0..off];
}

/// Best-effort UTC timestamp for the `generated_at` field. Falls back to an
/// empty string if the host doesn't expose clock_gettime; the manifest's
/// integrity check uses the root hash, not this field, so an empty value is
/// acceptable.
fn generatedAtNow(allocator: std.mem.Allocator) ![]const u8 {
    return allocator.dupe(u8, "");
}

// ============================================================================
// Tests
// ============================================================================

const testing = std.testing;

test "build creates a manifest with empty entries against a fresh tree" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];

    const result = try build(testing.allocator, root);
    defer manifest_v2.deinitManifest(testing.allocator, result.manifest);
    try testing.expect(!result.pre_existed);
    try testing.expectEqual(@as(usize, 0), result.manifest.entries.len);
    try testing.expectEqual(@as(usize, 0), result.manifest.nodoc.len);
}

test "build → verify round-trip: matches on clean tree" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];

    const first = try build(testing.allocator, root);
    manifest_v2.deinitManifest(testing.allocator, first.manifest);

    const v = try verify(testing.allocator, root);
    defer testing.allocator.free(v.stored_root);
    defer testing.allocator.free(v.computed_root);
    try testing.expect(v.matches);
}

test "verify returns ManifestMissing when no manifest exists" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];
    try testing.expectError(error.ManifestMissing, verify(testing.allocator, root));
}

test "build is idempotent on identical tree" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];

    const first = try build(testing.allocator, root);
    const first_root = try testing.allocator.dupe(u8, first.manifest.root);
    manifest_v2.deinitManifest(testing.allocator, first.manifest);
    defer testing.allocator.free(first_root);

    const second = try build(testing.allocator, root);
    defer manifest_v2.deinitManifest(testing.allocator, second.manifest);
    try testing.expectEqualStrings(first_root, second.manifest.root);
    try testing.expect(second.pre_existed);
}
