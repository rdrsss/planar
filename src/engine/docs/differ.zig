//! engine/docs/differ — `planar-doc diff` + five-signal classifier
//! (plan 423 M4).
//!
//! Walks the manifest and the working tree and emits one record per
//! detected drift, classifying each into one of five signals (Q329):
//!
//!   - regenerate-candidate — a covered source path's hash drifted.
//!   - hand-edit            — a doc's body hash drifted while sources hold.
//!   - new-authoring        — a path appears in the working tree under the
//!                            coverage policy but is not in any
//!                            entries.<doc>.sources and not in nodoc.
//!   - deletion             — a path in the manifest no longer exists in
//!                            the working tree.
//!   - nodoc-stale          — a path in nodoc has hash drift from when it
//!                            was marked.
//!
//! Signal completeness: every drift in the working tree relative to the
//! stored manifest maps to at least one signal. The classifier is
//! exhaustive over these five buckets.

const std = @import("std");
const manifest_v2 = @import("manifest_v2.zig");
const merkle = @import("merkle.zig");
const walk = @import("walk.zig");

const c = @cImport({
    @cInclude("sys/stat.h");
    @cInclude("fcntl.h");
    @cInclude("unistd.h");
});

pub const Signal = enum {
    @"regenerate-candidate",
    @"hand-edit",
    @"new-authoring",
    deletion,
    @"nodoc-stale",

    pub fn toString(s: Signal) []const u8 {
        return @tagName(s);
    }
};

/// One record emitted by `diff`. Owned by the parent DiffResult; freed via
/// `DiffResult.deinit`.
pub const Record = struct {
    signal: Signal,
    /// The repo-relative path the record concerns.
    path: []const u8,
    /// The doc path this record attributes to (`docs/X.md`). Empty when the
    /// record stands on its own (new-authoring, deletion of a nodoc entry,
    /// nodoc-stale).
    doc: []const u8,
    /// Short prose detail for human output. Owned.
    detail: []const u8,
};

pub const DiffResult = struct {
    records: []Record,
    allocator: std.mem.Allocator,

    pub fn deinit(self: *DiffResult) void {
        for (self.records) |r| {
            self.allocator.free(r.path);
            self.allocator.free(r.doc);
            self.allocator.free(r.detail);
        }
        self.allocator.free(self.records);
    }
};

pub const Error = error{
    ManifestMissing,
    MalformedJson,
    MissingField,
    IncompatibleVersion,
    IncompatibleAlgorithm,
    OpenFailed,
    ReadFailed,
    OutOfMemory,
    PathTooLong,
    ReadlinkFailed,
};

/// Diff `<repo_root>/.planar-manifest` against the current working tree.
/// Returns a sorted-by-signal-then-path list of records; an empty list
/// means the manifest matches reality.
pub fn diff(allocator: std.mem.Allocator, repo_root: []const u8) anyerror!DiffResult {
    const manifest_path = std.fs.path.join(allocator, &.{ repo_root, manifest_v2.file_name }) catch return error.OutOfMemory;
    defer allocator.free(manifest_path);

    const bytes = try readFileAllocOpt(allocator, manifest_path);
    if (bytes == null) return error.ManifestMissing;
    defer allocator.free(bytes.?);
    const stored = manifest_v2.fromJson(allocator, bytes.?) catch |e| switch (e) {
        error.IncompatibleVersion, error.IncompatibleAlgorithm => return e,
        else => return error.MalformedJson,
    };
    defer manifest_v2.deinitManifest(allocator, stored);

    var records: std.ArrayList(Record) = .empty;
    errdefer {
        for (records.items) |r| {
            allocator.free(r.path);
            allocator.free(r.doc);
            allocator.free(r.detail);
        }
        records.deinit(allocator);
    }

    // Build a set of covered source paths and nodoc paths for quick membership
    // checks during the working-tree walk.
    var covered: std.StringHashMap(void) = .init(allocator);
    defer covered.deinit();
    for (stored.entries) |row| {
        // The entry's own path (the doc itself) is implicitly covered.
        // Without this, every doc body appears as new-authoring on
        // every `diff` run.
        try covered.put(row.path, {});
        for (row.entry.sources) |sr| {
            try covered.put(sr.path, {});
        }
    }
    var nodoc_set: std.StringHashMap(void) = .init(allocator);
    defer nodoc_set.deinit();
    for (stored.nodoc) |row| {
        try nodoc_set.put(row.path, {});
    }

    // Pass 1: per-entry classification.
    for (stored.entries) |row| {
        // hand-edit: doc_hash drifted, sources_hash unchanged.
        const fresh_doc_hash = try computeDocHashStr(allocator, repo_root, row.path);
        defer allocator.free(fresh_doc_hash);

        // Compute fresh sources_hash by recomputing each source's hash
        // against the FS and re-canonicalizing.
        var fresh_sources = try allocator.alloc(manifest_v2.SourceRow, row.entry.sources.len);
        defer {
            for (fresh_sources) |sr| {
                allocator.free(sr.path);
                allocator.free(sr.hash);
            }
            allocator.free(fresh_sources);
        }
        for (row.entry.sources, 0..) |sr, i| {
            fresh_sources[i] = .{
                .path = try allocator.dupe(u8, sr.path),
                .hash = try computePathHashStr(allocator, repo_root, sr.path),
            };
        }
        const fresh_sources_hash = try manifest_v2.computeSourcesHash(allocator, fresh_sources);
        defer allocator.free(fresh_sources_hash);

        const doc_changed = !std.mem.eql(u8, fresh_doc_hash, row.entry.doc_hash);
        const sources_changed = !std.mem.eql(u8, fresh_sources_hash, row.entry.sources_hash);

        if (sources_changed) {
            // Identify each source that drifted and emit regenerate-candidate.
            for (row.entry.sources) |sr| {
                const fresh_path_hash = try computePathHashStr(allocator, repo_root, sr.path);
                defer allocator.free(fresh_path_hash);
                if (!std.mem.eql(u8, fresh_path_hash, sr.hash)) {
                    // Detect deletion: hash of zero indicates the path is gone.
                    if (!pathExists(allocator, repo_root, sr.path)) {
                        try records.append(allocator, .{
                            .signal = .deletion,
                            .path = try allocator.dupe(u8, sr.path),
                            .doc = try allocator.dupe(u8, row.path),
                            .detail = try allocator.dupe(u8, "covered source removed"),
                        });
                    } else {
                        try records.append(allocator, .{
                            .signal = .@"regenerate-candidate",
                            .path = try allocator.dupe(u8, sr.path),
                            .doc = try allocator.dupe(u8, row.path),
                            .detail = try allocator.dupe(u8, "covered source content changed"),
                        });
                    }
                }
            }
        }
        if (doc_changed and !sources_changed) {
            try records.append(allocator, .{
                .signal = .@"hand-edit",
                .path = try allocator.dupe(u8, row.path),
                .doc = try allocator.dupe(u8, row.path),
                .detail = try allocator.dupe(u8, "doc body changed; covered sources unchanged"),
            });
        }
    }

    // Pass 2: nodoc-stale + nodoc deletions.
    for (stored.nodoc) |row| {
        if (!pathExists(allocator, repo_root, row.path)) {
            try records.append(allocator, .{
                .signal = .deletion,
                .path = try allocator.dupe(u8, row.path),
                .doc = try allocator.dupe(u8, ""),
                .detail = try allocator.dupe(u8, "nodoc-marked path removed"),
            });
            continue;
        }
        const fresh = try computePathHashStr(allocator, repo_root, row.path);
        defer allocator.free(fresh);
        if (!std.mem.eql(u8, fresh, row.hash)) {
            try records.append(allocator, .{
                .signal = .@"nodoc-stale",
                .path = try allocator.dupe(u8, row.path),
                .doc = try allocator.dupe(u8, ""),
                .detail = try allocator.dupe(u8, "hash drifted since marked nodoc"),
            });
        }
    }

    // Pass 3: new-authoring. Walk the working tree; for each file (and
    // symlink), test path membership against the union of covered + nodoc
    // plus any *ancestor* directory entry in covered. A file under a
    // directory-typed source is covered transitively.
    var ctx = NewAuthoringCtx{
        .allocator = allocator,
        .covered = &covered,
        .nodoc_set = &nodoc_set,
        .records = &records,
    };
    try walk.walk(allocator, repo_root, &ctx, NewAuthoringCtx.cb);

    // Sort records: by signal name, then by path.
    std.mem.sort(Record, records.items, {}, lessThanRecord);

    return .{
        .records = try records.toOwnedSlice(allocator),
        .allocator = allocator,
    };
}

const NewAuthoringCtx = struct {
    allocator: std.mem.Allocator,
    covered: *std.StringHashMap(void),
    nodoc_set: *std.StringHashMap(void),
    records: *std.ArrayList(Record),

    fn cb(ctx: *anyopaque, entry: walk.Entry) anyerror!void {
        const self: *NewAuthoringCtx = @ptrCast(@alignCast(ctx));
        if (try self.isCoveredOrNodoc(entry.rel_path)) return;
        // Also test ancestor directory paths (e.g. `src/engine/`) to honor
        // transitive coverage.
        var path = entry.rel_path;
        while (std.fs.path.dirname(path)) |parent| {
            // The covered set stores directory keys with trailing `/`.
            const dir_key = try std.fmt.allocPrint(self.allocator, "{s}/", .{parent});
            defer self.allocator.free(dir_key);
            if (self.covered.contains(dir_key) or self.nodoc_set.contains(dir_key)) return;
            if (parent.len == 0) break;
            path = parent;
        }
        try self.records.append(self.allocator, .{
            .signal = .@"new-authoring",
            .path = try self.allocator.dupe(u8, entry.rel_path),
            .doc = try self.allocator.dupe(u8, ""),
            .detail = try self.allocator.dupe(u8, "path not covered by any doc and not in nodoc"),
        });
    }

    fn isCoveredOrNodoc(self: *NewAuthoringCtx, rel: []const u8) !bool {
        if (self.covered.contains(rel)) return true;
        if (self.nodoc_set.contains(rel)) return true;
        return false;
    }
};

// ----------------------------------------------------------------------
// Internal helpers

fn computeDocHashStr(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) ![]const u8 {
    const abs = try std.fs.path.join(allocator, &.{ root, rel });
    defer allocator.free(abs);
    const h = merkle.hashFile(allocator, abs) catch 0;
    return manifest_v2.formatHash(allocator, h);
}

fn computePathHashStr(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) ![]const u8 {
    const trim = trimTrailingSlash(rel);
    const abs = try std.fs.path.join(allocator, &.{ root, trim });
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

fn pathExists(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) bool {
    const trim = trimTrailingSlash(rel);
    const abs = std.fs.path.join(allocator, &.{ root, trim }) catch return false;
    defer allocator.free(abs);
    const abs_z = allocator.dupeZ(u8, abs) catch return false;
    defer allocator.free(abs_z);
    var st: c.struct_stat = undefined;
    return c.stat(abs_z.ptr, &st) == 0;
}

fn lessThanRecord(_: void, a: Record, b: Record) bool {
    const sig_cmp = std.mem.order(u8, a.signal.toString(), b.signal.toString());
    if (sig_cmp != .eq) return sig_cmp == .lt;
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
const builder = @import("builder.zig");

test "diff returns ManifestMissing when no manifest exists" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];
    try testing.expectError(error.ManifestMissing, diff(testing.allocator, root));
}

test "diff on a clean tree returns no records" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];

    const built = try builder.build(testing.allocator, root);
    manifest_v2.deinitManifest(testing.allocator, built.manifest);

    var result = try diff(testing.allocator, root);
    defer result.deinit();
    // An empty fresh manifest still surfaces the .gitignore and any seeded
    // files as new-authoring; for this tmp dir nothing was seeded so the
    // walk emits 0 records.
    try testing.expectEqual(@as(usize, 0), result.records.len);
}

test "Signal.toString uses the tag name" {
    try testing.expectEqualStrings("regenerate-candidate", Signal.toString(.@"regenerate-candidate"));
    try testing.expectEqualStrings("hand-edit", Signal.toString(.@"hand-edit"));
    try testing.expectEqualStrings("new-authoring", Signal.toString(.@"new-authoring"));
    try testing.expectEqualStrings("deletion", Signal.toString(.deletion));
    try testing.expectEqualStrings("nodoc-stale", Signal.toString(.@"nodoc-stale"));
}
