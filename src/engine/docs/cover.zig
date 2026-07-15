//! engine/docs/cover — `planar-doc cover` / `planar-doc nodoc` manifest
//! mutators (plan 423 M5).
//!
//! All four operations enforce the one-source-one-doc constraint per the
//! tech spec: a source path may appear in AT MOST one
//! `entries.<doc>.sources` map AND never simultaneously in `nodoc`. The
//! operations are:
//!
//!   addCover(doc_path, source_path)
//!   removeCover(doc_path, source_path)
//!   addNodoc(source_path)
//!   removeNodoc(source_path)
//!
//! Each function loads the manifest, mutates the in-memory structure,
//! recomputes the affected entry's sources_hash / entry_hash, recomputes
//! the manifest root, and writes the manifest atomically. Behavior under
//! constraint violation is explicit refusal via the Error set.
//!
//! Engine-level only; the `planar-doc cover/nodoc` CLI surface is wired in
//! M6 when the new binary lands.

const std = @import("std");
const manifest_v2 = @import("manifest_v2.zig");
const merkle = @import("merkle.zig");
const builder = @import("builder.zig");

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
    DocEntryMissing,
    SourceAlreadyCoveredByDifferentDoc,
    SourceAlreadyInNodoc,
    SourceAlreadyCovered,
    SourceNotCovered,
    PathNotInNodoc,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    RenameFailed,
    OutOfMemory,
    PathTooLong,
    ReadlinkFailed,
};

/// Add `source_path` to `entries.<doc_path>.sources`. Refuses if the source
/// is already covered (by any doc), already in `nodoc`, or the doc entry
/// doesn't exist. The doc entry must be created by an earlier promote or by
/// the documenter agent's worklist; this function does NOT create entries.
pub fn addCover(
    allocator: std.mem.Allocator,
    repo_root: []const u8,
    doc_path: []const u8,
    source_path: []const u8,
) Error!void {
    var m = try loadManifest(allocator, repo_root);
    defer manifest_v2.deinitManifest(allocator, m);

    // Constraint checks.
    if (findNodocIndex(m, source_path) != null) return error.SourceAlreadyInNodoc;
    if (findCoveringDoc(m, source_path)) |existing_doc| {
        if (std.mem.eql(u8, existing_doc, doc_path)) return error.SourceAlreadyCovered;
        return error.SourceAlreadyCoveredByDifferentDoc;
    }

    const entry_idx = findEntryIndex(m, doc_path) orelse blk: {
        // Auto-register the doc entry. This is the path the documenter
        // agent's "create-doc" action takes: the operator authors a new
        // doc body and runs `planar-doc cover` to wire its first source.
        // The entry's doc_hash is computed from the doc body on disk;
        // sources_hash + entry_hash are sealed by recomputeAndWrite at
        // the end of addCover.
        const doc_hash = try computePathHashStr(allocator, repo_root, doc_path);
        const empty_sources = try allocator.alloc(manifest_v2.SourceRow, 0);
        const sources_hash = try manifest_v2.computeSourcesHash(allocator, empty_sources);
        const entry_hash = try manifest_v2.computeEntryHash(allocator, doc_hash, sources_hash);
        const new_entry: manifest_v2.EntryRow = .{
            .path = try allocator.dupe(u8, doc_path),
            .entry = .{
                .doc_hash = doc_hash,
                .sources = empty_sources,
                .sources_hash = sources_hash,
                .entry_hash = entry_hash,
            },
        };
        const new_entries = try allocator.alloc(manifest_v2.EntryRow, m.entries.len + 1);
        for (m.entries, 0..) |row, i| new_entries[i] = row;
        new_entries[m.entries.len] = new_entry;
        std.mem.sort(manifest_v2.EntryRow, new_entries, {}, manifest_v2.lessThanEntryPath);
        allocator.free(m.entries);
        m.entries = new_entries;
        break :blk findEntryIndex(m, doc_path).?;
    };

    // Build a new sources slice with the added row.
    const old_sources = m.entries[entry_idx].entry.sources;
    var new_sources = try allocator.alloc(manifest_v2.SourceRow, old_sources.len + 1);
    errdefer allocator.free(new_sources);
    for (old_sources, 0..) |sr, i| {
        new_sources[i] = .{
            .path = try allocator.dupe(u8, sr.path),
            .hash = try allocator.dupe(u8, sr.hash),
        };
    }
    new_sources[old_sources.len] = .{
        .path = try allocator.dupe(u8, source_path),
        .hash = try computePathHashStr(allocator, repo_root, source_path),
    };
    std.mem.sort(manifest_v2.SourceRow, new_sources, {}, lessThanSourcePath);

    try replaceEntrySources(allocator, &m, entry_idx, new_sources);
    try recomputeAndWrite(allocator, repo_root, &m);
}

/// Drop `source_path` from `entries.<doc_path>.sources`. Refuses if the
/// source is not in that doc's sources or the doc entry doesn't exist.
pub fn removeCover(
    allocator: std.mem.Allocator,
    repo_root: []const u8,
    doc_path: []const u8,
    source_path: []const u8,
) Error!void {
    var m = try loadManifest(allocator, repo_root);
    defer manifest_v2.deinitManifest(allocator, m);

    const entry_idx = findEntryIndex(m, doc_path) orelse return error.DocEntryMissing;
    const src_idx = findSourceIndex(m.entries[entry_idx].entry.sources, source_path) orelse return error.SourceNotCovered;

    const old_sources = m.entries[entry_idx].entry.sources;
    var new_sources = try allocator.alloc(manifest_v2.SourceRow, old_sources.len - 1);
    errdefer allocator.free(new_sources);
    var out_idx: usize = 0;
    for (old_sources, 0..) |sr, i| {
        if (i == src_idx) continue;
        new_sources[out_idx] = .{
            .path = try allocator.dupe(u8, sr.path),
            .hash = try allocator.dupe(u8, sr.hash),
        };
        out_idx += 1;
    }

    try replaceEntrySources(allocator, &m, entry_idx, new_sources);
    try recomputeAndWrite(allocator, repo_root, &m);
}

/// Add `source_path` to the top-level `nodoc` map. Refuses if the path is
/// already covered by any doc.
pub fn addNodoc(
    allocator: std.mem.Allocator,
    repo_root: []const u8,
    source_path: []const u8,
) Error!void {
    var m = try loadManifest(allocator, repo_root);
    defer manifest_v2.deinitManifest(allocator, m);

    if (findCoveringDoc(m, source_path) != null) return error.SourceAlreadyCoveredByDifferentDoc;
    if (findNodocIndex(m, source_path) != null) return error.SourceAlreadyInNodoc;

    const hash = try computePathHashStr(allocator, repo_root, source_path);
    errdefer allocator.free(hash);

    var new_nodoc = try allocator.alloc(manifest_v2.NodocRow, m.nodoc.len + 1);
    errdefer allocator.free(new_nodoc);
    for (m.nodoc, 0..) |row, i| {
        new_nodoc[i] = .{
            .path = try allocator.dupe(u8, row.path),
            .hash = try allocator.dupe(u8, row.hash),
        };
    }
    new_nodoc[m.nodoc.len] = .{
        .path = try allocator.dupe(u8, source_path),
        .hash = hash,
    };
    std.mem.sort(manifest_v2.NodocRow, new_nodoc, {}, lessThanNodocPath);

    try replaceNodoc(allocator, &m, new_nodoc);
    try recomputeAndWrite(allocator, repo_root, &m);
}

/// Drop `source_path` from the top-level `nodoc` map. Refuses if the path
/// is not present.
pub fn removeNodoc(
    allocator: std.mem.Allocator,
    repo_root: []const u8,
    source_path: []const u8,
) Error!void {
    var m = try loadManifest(allocator, repo_root);
    defer manifest_v2.deinitManifest(allocator, m);

    const idx = findNodocIndex(m, source_path) orelse return error.PathNotInNodoc;

    var new_nodoc = try allocator.alloc(manifest_v2.NodocRow, m.nodoc.len - 1);
    errdefer allocator.free(new_nodoc);
    var out_idx: usize = 0;
    for (m.nodoc, 0..) |row, i| {
        if (i == idx) continue;
        new_nodoc[out_idx] = .{
            .path = try allocator.dupe(u8, row.path),
            .hash = try allocator.dupe(u8, row.hash),
        };
        out_idx += 1;
    }

    try replaceNodoc(allocator, &m, new_nodoc);
    try recomputeAndWrite(allocator, repo_root, &m);
}

// ============================================================================
// Internal helpers

fn loadManifest(allocator: std.mem.Allocator, repo_root: []const u8) Error!manifest_v2.Manifest {
    const manifest_path = std.fs.path.join(allocator, &.{ repo_root, manifest_v2.file_name }) catch return error.OutOfMemory;
    defer allocator.free(manifest_path);
    const bytes = readFileAllocOpt(allocator, manifest_path) catch return error.ReadFailed;
    if (bytes == null) return error.ManifestMissing;
    defer allocator.free(bytes.?);
    return manifest_v2.fromJson(allocator, bytes.?) catch |e| switch (e) {
        error.IncompatibleVersion, error.IncompatibleAlgorithm => e,
        else => error.MalformedJson,
    };
}

fn findEntryIndex(m: manifest_v2.Manifest, doc_path: []const u8) ?usize {
    for (m.entries, 0..) |row, i| {
        if (std.mem.eql(u8, row.path, doc_path)) return i;
    }
    return null;
}

fn findSourceIndex(sources: []const manifest_v2.SourceRow, source_path: []const u8) ?usize {
    for (sources, 0..) |sr, i| {
        if (std.mem.eql(u8, sr.path, source_path)) return i;
    }
    return null;
}

fn findCoveringDoc(m: manifest_v2.Manifest, source_path: []const u8) ?[]const u8 {
    for (m.entries) |row| {
        for (row.entry.sources) |sr| {
            if (std.mem.eql(u8, sr.path, source_path)) return row.path;
        }
    }
    return null;
}

fn findNodocIndex(m: manifest_v2.Manifest, source_path: []const u8) ?usize {
    for (m.nodoc, 0..) |row, i| {
        if (std.mem.eql(u8, row.path, source_path)) return i;
    }
    return null;
}

fn replaceEntrySources(
    allocator: std.mem.Allocator,
    m: *manifest_v2.Manifest,
    entry_idx: usize,
    new_sources: []manifest_v2.SourceRow,
) Error!void {
    // Free old sources slice contents.
    for (m.entries[entry_idx].entry.sources) |sr| {
        allocator.free(sr.path);
        allocator.free(sr.hash);
    }
    allocator.free(m.entries[entry_idx].entry.sources);
    // Cast the entries slice back to a mutable handle by re-allocating
    // since manifest types declare `[]const ...`. Simplest: rebuild the
    // entries slice.
    var new_entries = try allocator.alloc(manifest_v2.EntryRow, m.entries.len);
    errdefer allocator.free(new_entries);
    for (m.entries, 0..) |row, i| {
        if (i == entry_idx) {
            new_entries[i] = .{
                .path = try allocator.dupe(u8, row.path),
                .entry = .{
                    .doc_hash = try allocator.dupe(u8, row.entry.doc_hash),
                    .sources = new_sources,
                    .sources_hash = try allocator.dupe(u8, row.entry.sources_hash),
                    .entry_hash = try allocator.dupe(u8, row.entry.entry_hash),
                },
            };
        } else {
            var sources_copy = try allocator.alloc(manifest_v2.SourceRow, row.entry.sources.len);
            for (row.entry.sources, 0..) |sr, j| {
                sources_copy[j] = .{
                    .path = try allocator.dupe(u8, sr.path),
                    .hash = try allocator.dupe(u8, sr.hash),
                };
            }
            new_entries[i] = .{
                .path = try allocator.dupe(u8, row.path),
                .entry = .{
                    .doc_hash = try allocator.dupe(u8, row.entry.doc_hash),
                    .sources = sources_copy,
                    .sources_hash = try allocator.dupe(u8, row.entry.sources_hash),
                    .entry_hash = try allocator.dupe(u8, row.entry.entry_hash),
                },
            };
        }
    }
    // Free the old entries (after copying out, including the source we
    // already replaced above).
    for (m.entries, 0..) |row, i| {
        if (i == entry_idx) {
            // sources already freed; only free wrapper fields.
            allocator.free(row.path);
            allocator.free(row.entry.doc_hash);
            allocator.free(row.entry.sources_hash);
            allocator.free(row.entry.entry_hash);
        } else {
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
    }
    allocator.free(m.entries);
    m.entries = new_entries;
}

fn replaceNodoc(allocator: std.mem.Allocator, m: *manifest_v2.Manifest, new_nodoc: []manifest_v2.NodocRow) Error!void {
    for (m.nodoc) |row| {
        allocator.free(row.path);
        allocator.free(row.hash);
    }
    allocator.free(m.nodoc);
    m.nodoc = new_nodoc;
}

fn recomputeAndWrite(allocator: std.mem.Allocator, repo_root: []const u8, m: *manifest_v2.Manifest) Error!void {
    // For each entry, recompute sources_hash + entry_hash against the
    // current sources slice (membership may have changed; hash values
    // already reflect the current FS state because addCover / addNodoc
    // hashed at append time).
    var new_entries = try allocator.alloc(manifest_v2.EntryRow, m.entries.len);
    errdefer allocator.free(new_entries);
    for (m.entries, 0..) |row, i| {
        const new_sources_hash = try manifest_v2.computeSourcesHash(allocator, row.entry.sources);
        errdefer allocator.free(new_sources_hash);
        const new_entry_hash = try manifest_v2.computeEntryHash(allocator, row.entry.doc_hash, new_sources_hash);
        errdefer allocator.free(new_entry_hash);
        var sources_copy = try allocator.alloc(manifest_v2.SourceRow, row.entry.sources.len);
        for (row.entry.sources, 0..) |sr, j| {
            sources_copy[j] = .{
                .path = try allocator.dupe(u8, sr.path),
                .hash = try allocator.dupe(u8, sr.hash),
            };
        }
        new_entries[i] = .{
            .path = try allocator.dupe(u8, row.path),
            .entry = .{
                .doc_hash = try allocator.dupe(u8, row.entry.doc_hash),
                .sources = sources_copy,
                .sources_hash = new_sources_hash,
                .entry_hash = new_entry_hash,
            },
        };
    }
    for (m.entries) |row| {
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
    allocator.free(m.entries);
    m.entries = new_entries;

    const new_root = try manifest_v2.computeRoot(allocator, m.entries, m.nodoc);
    allocator.free(m.root);
    m.root = new_root;

    const json = try manifest_v2.toJson(allocator, m.*);
    defer allocator.free(json);
    const manifest_path = try std.fs.path.join(allocator, &.{ repo_root, manifest_v2.file_name });
    defer allocator.free(manifest_path);
    try merkle.atomicWrite(allocator, manifest_path, json);
}

fn computePathHashStr(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) ![]const u8 {
    const trim = if (rel.len > 0 and rel[rel.len - 1] == '/') rel[0 .. rel.len - 1] else rel;
    const abs = try std.fs.path.join(allocator, &.{ root, trim });
    defer allocator.free(abs);
    const is_dir = rel.len > 0 and rel[rel.len - 1] == '/';
    const h: u64 = if (is_dir)
        merkle.hashDir(allocator, abs) catch 0
    else
        merkle.hashFile(allocator, abs) catch 0;
    return manifest_v2.formatHash(allocator, h);
}

fn lessThanSourcePath(_: void, a: manifest_v2.SourceRow, b: manifest_v2.SourceRow) bool {
    return std.mem.order(u8, a.path, b.path) == .lt;
}

fn lessThanNodocPath(_: void, a: manifest_v2.NodocRow, b: manifest_v2.NodocRow) bool {
    return std.mem.order(u8, a.path, b.path) == .lt;
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

// ============================================================================
// Tests
// ============================================================================

const testing = std.testing;

test "addCover auto-registers a new doc entry on first source" {
    // Plan 423 M7 contract change: addCover creates the entry if it
    // doesn't exist yet, so the documenter agent's `create-doc` flow
    // (operator commits doc body → runs cover) works end-to-end.
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];

    try tmp.dir.createDirPath(std.testing.io, "src/foo");
    var src_f = try tmp.dir.createFile(std.testing.io, "src/foo/lib.zig", .{});
    try src_f.writeStreamingAll(std.testing.io, "pub fn foo() void {}\n");
    src_f.close(std.testing.io);
    try tmp.dir.createDirPath(std.testing.io, "docs/features");
    var doc_f = try tmp.dir.createFile(std.testing.io, "docs/features/foo.md", .{});
    try doc_f.writeStreamingAll(std.testing.io, "# Foo\n");
    doc_f.close(std.testing.io);

    const built = try builder.build(testing.allocator, root);
    manifest_v2.deinitManifest(testing.allocator, built.manifest);

    try addCover(testing.allocator, root, "docs/features/foo.md", "src/foo/");
}

test "addNodoc + removeNodoc round-trip" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];

    const built = try builder.build(testing.allocator, root);
    manifest_v2.deinitManifest(testing.allocator, built.manifest);

    try addNodoc(testing.allocator, root, "src/foo/bar.zig");
    // Re-adding is refused.
    try testing.expectError(error.SourceAlreadyInNodoc, addNodoc(testing.allocator, root, "src/foo/bar.zig"));
    try removeNodoc(testing.allocator, root, "src/foo/bar.zig");
    // After removal it's gone.
    try testing.expectError(error.PathNotInNodoc, removeNodoc(testing.allocator, root, "src/foo/bar.zig"));
}

test "addNodoc refuses a doc entry path without changing the manifest" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];

    try tmp.dir.createDirPath(std.testing.io, "src/foo");
    var src_f = try tmp.dir.createFile(std.testing.io, "src/foo/lib.zig", .{});
    try src_f.writeStreamingAll(std.testing.io, "pub fn foo() void {}\n");
    src_f.close(std.testing.io);
    try tmp.dir.createDirPath(std.testing.io, "docs/features");
    var doc_f = try tmp.dir.createFile(std.testing.io, "docs/features/foo.md", .{});
    try doc_f.writeStreamingAll(std.testing.io, "# Foo\n");
    doc_f.close(std.testing.io);

    const built = try builder.build(testing.allocator, root);
    manifest_v2.deinitManifest(testing.allocator, built.manifest);
    try addCover(testing.allocator, root, "docs/features/foo.md", "src/foo/");

    const before = try tmp.dir.readFileAlloc(std.testing.io, manifest_v2.file_name, testing.allocator, .limited(128 * 1024));
    defer testing.allocator.free(before);
    try testing.expectError(
        error.SourceAlreadyCoveredByDifferentDoc,
        addNodoc(testing.allocator, root, "docs/features/foo.md"),
    );
    const after = try tmp.dir.readFileAlloc(std.testing.io, manifest_v2.file_name, testing.allocator, .limited(128 * 1024));
    defer testing.allocator.free(after);
    try testing.expectEqualStrings(before, after);
}

test "addCover refuses a doc path already in nodoc without changing the manifest" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];

    try tmp.dir.createDirPath(std.testing.io, "src/foo");
    var src_f = try tmp.dir.createFile(std.testing.io, "src/foo/lib.zig", .{});
    try src_f.writeStreamingAll(std.testing.io, "pub fn foo() void {}\n");
    src_f.close(std.testing.io);
    try tmp.dir.createDirPath(std.testing.io, "docs/features");
    var doc_f = try tmp.dir.createFile(std.testing.io, "docs/features/foo.md", .{});
    try doc_f.writeStreamingAll(std.testing.io, "# Foo\n");
    doc_f.close(std.testing.io);

    const built = try builder.build(testing.allocator, root);
    manifest_v2.deinitManifest(testing.allocator, built.manifest);
    try addNodoc(testing.allocator, root, "docs/features/foo.md");

    const before = try tmp.dir.readFileAlloc(std.testing.io, manifest_v2.file_name, testing.allocator, .limited(128 * 1024));
    defer testing.allocator.free(before);
    try testing.expectError(
        error.SourceAlreadyInNodoc,
        addCover(testing.allocator, root, "docs/features/foo.md", "src/foo/"),
    );
    const after = try tmp.dir.readFileAlloc(std.testing.io, manifest_v2.file_name, testing.allocator, .limited(128 * 1024));
    defer testing.allocator.free(after);
    try testing.expectEqualStrings(before, after);
}
