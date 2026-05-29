//! engine/docs/manifest_v2 — repo-state manifest schema (plan 423 M1).
//!
//! The v2 manifest replaces the v1 .manifest-docs format. v1 keyed each
//! doc's `sources` map by Planar artifact IDs (workstation-local autoincrement
//! integers). v2 keys `sources` by repo paths, making the manifest fully
//! portable across machines and clones.
//!
//! Top-level shape:
//!
//!     {
//!       "version": 2,
//!       "algo":    "xxh64",
//!       "root":    "<xxh64 of canonical-serialized entries + nodoc>",
//!       "generated_at": "<RFC3339>",
//!       "entries": {
//!         "docs/<path>.md": {
//!           "doc_hash":     "<xxh64 of doc body>",
//!           "sources":      { "<repo-path>": "<subtree-merkle hash>" },
//!           "sources_hash": "<xxh64 of canonical sources block>",
//!           "entry_hash":   "<xxh64 of doc_hash || sources_hash>"
//!         }
//!       },
//!       "nodoc": {
//!         "<repo-path>": "<hash-when-decided>"
//!       }
//!     }
//!
//! Co-lives with v1's `manifest.zig` until plan 423 M6 deletes the old code.
//! The file name on disk for v2 is `.planar-manifest` (decided in
//! [[q323-manifest-filename]]).

const std = @import("std");
const merkle = @import("merkle.zig");

pub const algo = "xxh64";
pub const version: i64 = 2;
pub const file_name = ".planar-manifest";

/// One repo-path → hash pair inside a doc's `sources` map (or the top-level
/// `nodoc` map). Hashes are emitted as lowercase 16-hex-char strings.
pub const SourceRow = struct {
    path: []const u8,
    hash: []const u8,
};

/// One entry in `entries`. The map key (`docs/<path>.md`) is stored
/// separately on the parent `EntryRow`.
pub const Entry = struct {
    doc_hash: []const u8,
    sources: []const SourceRow,
    sources_hash: []const u8,
    entry_hash: []const u8,
};

pub const EntryRow = struct {
    path: []const u8,
    entry: Entry,
};

/// One repo-path → hash pair in the top-level `nodoc` map.
pub const NodocRow = struct {
    path: []const u8,
    hash: []const u8,
};

pub const Manifest = struct {
    version: i64 = version,
    algo: []const u8 = algo,
    root: []const u8,
    generated_at: []const u8,
    entries: []const EntryRow,
    nodoc: []const NodocRow,
};

/// Format a u64 hash as a lowercase 16-hex-char string. Caller owns the
/// returned slice.
pub fn formatHash(allocator: std.mem.Allocator, h: u64) ![]const u8 {
    return std.fmt.allocPrint(allocator, "{x:0>16}", .{h});
}

/// Compute the `sources_hash` for an entry by canonical-serializing its
/// `sources` block: each row emitted as `<path>\0<hash>\n`, sorted by path.
/// Returns the xxh64 of that byte stream as a hex string.
pub fn computeSourcesHash(allocator: std.mem.Allocator, sources: []const SourceRow) ![]const u8 {
    // sources are required to be sorted by caller / builder. Defensive sort
    // here keeps the helper safe for direct callers.
    const rows = try allocator.alloc(SourceRow, sources.len);
    defer allocator.free(rows);
    @memcpy(rows, sources);
    std.mem.sort(SourceRow, rows, {}, lessThanSourcePath);

    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);
    for (rows) |row| {
        try buf.appendSlice(allocator, row.path);
        try buf.append(allocator, 0);
        try buf.appendSlice(allocator, row.hash);
        try buf.append(allocator, '\n');
    }
    return formatHash(allocator, merkle.Hasher.hash(merkle.seed, buf.items));
}

/// Compute an entry's `entry_hash` as xxh64(doc_hash || sources_hash). Both
/// inputs are the hex-encoded strings stored on the entry — that way a
/// downstream consumer can recompute the entry_hash without knowing the
/// internal byte order of the u64.
pub fn computeEntryHash(allocator: std.mem.Allocator, doc_hash: []const u8, sources_hash: []const u8) ![]const u8 {
    var concat = try allocator.alloc(u8, doc_hash.len + sources_hash.len);
    defer allocator.free(concat);
    @memcpy(concat[0..doc_hash.len], doc_hash);
    @memcpy(concat[doc_hash.len..], sources_hash);
    return formatHash(allocator, merkle.Hasher.hash(merkle.seed, concat));
}

/// Compute the manifest root hash by canonical-serializing the entries
/// block AND the nodoc block, then xxh64-ing the concatenation. Per
/// tech-spec § Manifest hashing details.
pub fn computeRoot(allocator: std.mem.Allocator, entries: []const EntryRow, nodoc: []const NodocRow) ![]const u8 {
    // Sort defensively. Entries by path. Nodoc by path.
    const entry_rows = try allocator.alloc(EntryRow, entries.len);
    defer allocator.free(entry_rows);
    @memcpy(entry_rows, entries);
    std.mem.sort(EntryRow, entry_rows, {}, lessThanEntryPath);

    const nodoc_rows = try allocator.alloc(NodocRow, nodoc.len);
    defer allocator.free(nodoc_rows);
    @memcpy(nodoc_rows, nodoc);
    std.mem.sort(NodocRow, nodoc_rows, {}, lessThanNodocPath);

    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);
    for (entry_rows) |row| {
        try buf.appendSlice(allocator, row.path);
        try buf.append(allocator, 0);
        try buf.appendSlice(allocator, row.entry.entry_hash);
        try buf.append(allocator, '\n');
    }
    // Separator byte between blocks ensures an empty entries block + nodoc
    // entry can't collide with the inverse.
    try buf.append(allocator, 0);
    for (nodoc_rows) |row| {
        try buf.appendSlice(allocator, row.path);
        try buf.append(allocator, 0);
        try buf.appendSlice(allocator, row.hash);
        try buf.append(allocator, '\n');
    }
    return formatHash(allocator, merkle.Hasher.hash(merkle.seed, buf.items));
}

// ============================================================================
// JSON serializer / deserializer
// ============================================================================

/// Serialize a manifest to JSON. Caller owns the returned bytes. The
/// emitted form is stable: sorted entries, sorted sources, sorted nodoc;
/// pretty-printed with two-space indent for human and git-diff friendliness.
pub fn toJson(allocator: std.mem.Allocator, m: Manifest) ![]const u8 {
    const entry_rows = try allocator.alloc(EntryRow, m.entries.len);
    defer allocator.free(entry_rows);
    @memcpy(entry_rows, m.entries);
    std.mem.sort(EntryRow, entry_rows, {}, lessThanEntryPath);

    const nodoc_rows = try allocator.alloc(NodocRow, m.nodoc.len);
    defer allocator.free(nodoc_rows);
    @memcpy(nodoc_rows, m.nodoc);
    std.mem.sort(NodocRow, nodoc_rows, {}, lessThanNodocPath);

    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);

    try buf.appendSlice(allocator, "{\n");
    try appendPrint(&buf, allocator, "  \"version\": {d},\n", .{m.version});
    try buf.appendSlice(allocator, "  \"algo\": ");
    try appendJsonString(&buf, allocator, m.algo);
    try buf.appendSlice(allocator, ",\n  \"root\": ");
    try appendJsonString(&buf, allocator, m.root);
    try buf.appendSlice(allocator, ",\n  \"generated_at\": ");
    try appendJsonString(&buf, allocator, m.generated_at);
    try buf.appendSlice(allocator, ",\n  \"entries\": {");

    for (entry_rows, 0..) |row, i| {
        if (i > 0) try buf.append(allocator, ',');
        try buf.appendSlice(allocator, "\n    ");
        try appendJsonString(&buf, allocator, row.path);
        try buf.appendSlice(allocator, ": {\n      \"doc_hash\": ");
        try appendJsonString(&buf, allocator, row.entry.doc_hash);
        try buf.appendSlice(allocator, ",\n      \"sources\": {");

        const source_rows = try allocator.alloc(SourceRow, row.entry.sources.len);
        defer allocator.free(source_rows);
        @memcpy(source_rows, row.entry.sources);
        std.mem.sort(SourceRow, source_rows, {}, lessThanSourcePath);

        for (source_rows, 0..) |sr, j| {
            if (j > 0) try buf.append(allocator, ',');
            try buf.appendSlice(allocator, "\n        ");
            try appendJsonString(&buf, allocator, sr.path);
            try buf.appendSlice(allocator, ": ");
            try appendJsonString(&buf, allocator, sr.hash);
        }
        if (source_rows.len > 0) try buf.appendSlice(allocator, "\n      ");
        try buf.appendSlice(allocator, "},\n      \"sources_hash\": ");
        try appendJsonString(&buf, allocator, row.entry.sources_hash);
        try buf.appendSlice(allocator, ",\n      \"entry_hash\": ");
        try appendJsonString(&buf, allocator, row.entry.entry_hash);
        try buf.appendSlice(allocator, "\n    }");
    }
    if (entry_rows.len > 0) try buf.appendSlice(allocator, "\n  ");
    try buf.appendSlice(allocator, "},\n  \"nodoc\": {");

    for (nodoc_rows, 0..) |row, i| {
        if (i > 0) try buf.append(allocator, ',');
        try buf.appendSlice(allocator, "\n    ");
        try appendJsonString(&buf, allocator, row.path);
        try buf.appendSlice(allocator, ": ");
        try appendJsonString(&buf, allocator, row.hash);
    }
    if (nodoc_rows.len > 0) try buf.appendSlice(allocator, "\n  ");
    try buf.appendSlice(allocator, "}\n}\n");

    return buf.toOwnedSlice(allocator);
}

fn appendPrint(buf: *std.ArrayList(u8), allocator: std.mem.Allocator, comptime fmt: []const u8, args: anytype) !void {
    const s = try std.fmt.allocPrint(allocator, fmt, args);
    defer allocator.free(s);
    try buf.appendSlice(allocator, s);
}

fn appendJsonString(buf: *std.ArrayList(u8), allocator: std.mem.Allocator, s: []const u8) !void {
    try buf.append(allocator, '"');
    for (s) |ch| switch (ch) {
        '"' => try buf.appendSlice(allocator, "\\\""),
        '\\' => try buf.appendSlice(allocator, "\\\\"),
        '\n' => try buf.appendSlice(allocator, "\\n"),
        '\r' => try buf.appendSlice(allocator, "\\r"),
        '\t' => try buf.appendSlice(allocator, "\\t"),
        0...8, 11...12, 14...0x1f => try appendPrint(buf, allocator, "\\u{x:0>4}", .{ch}),
        else => try buf.append(allocator, ch),
    };
    try buf.append(allocator, '"');
}

/// Free a manifest's nested slices. Use after `fromJson` returns owned data.
pub fn deinitManifest(allocator: std.mem.Allocator, m: Manifest) void {
    allocator.free(m.algo);
    allocator.free(m.root);
    allocator.free(m.generated_at);
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
    for (m.nodoc) |row| {
        allocator.free(row.path);
        allocator.free(row.hash);
    }
    allocator.free(m.nodoc);
}

/// Parse a JSON manifest. Returns an owned Manifest; caller frees via
/// `deinitManifest`. Rejects v1 manifests (version != 2) with
/// `error.IncompatibleVersion` and unknown algorithm headers with
/// `error.IncompatibleAlgorithm`.
pub fn fromJson(allocator: std.mem.Allocator, json: []const u8) !Manifest {
    var arena = std.heap.ArenaAllocator.init(allocator);
    defer arena.deinit();
    const aa = arena.allocator();
    const parsed = std.json.parseFromSliceLeaky(std.json.Value, aa, json, .{
        .allocate = .alloc_always,
        .max_value_len = 1 << 20,
    }) catch return error.MalformedJson;
    const root = switch (parsed) {
        .object => |o| o,
        else => return error.MalformedJson,
    };
    const ver = root.get("version") orelse return error.MissingField;
    const ver_i: i64 = switch (ver) {
        .integer => |i| i,
        else => return error.MalformedJson,
    };
    if (ver_i != version) return error.IncompatibleVersion;

    const al = root.get("algo") orelse return error.MissingField;
    const al_s: []const u8 = switch (al) {
        .string => |s| s,
        else => return error.MalformedJson,
    };
    if (!std.mem.eql(u8, al_s, algo)) return error.IncompatibleAlgorithm;

    const root_hash = try takeString(root, "root", allocator);
    errdefer allocator.free(root_hash);
    const gen_at = try takeString(root, "generated_at", allocator);
    errdefer allocator.free(gen_at);
    const algo_dup = try allocator.dupe(u8, algo);
    errdefer allocator.free(algo_dup);

    const entries_v = root.get("entries") orelse return error.MissingField;
    const entries_obj = switch (entries_v) {
        .object => |o| o,
        else => return error.MalformedJson,
    };
    var entries: std.ArrayList(EntryRow) = .empty;
    errdefer {
        for (entries.items) |row| {
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
        entries.deinit(allocator);
    }
    var it = entries_obj.iterator();
    while (it.next()) |kv| {
        const entry_obj = switch (kv.value_ptr.*) {
            .object => |o| o,
            else => return error.MalformedJson,
        };
        const doc_hash = try takeString(entry_obj, "doc_hash", allocator);
        errdefer allocator.free(doc_hash);
        const sources_hash = try takeString(entry_obj, "sources_hash", allocator);
        errdefer allocator.free(sources_hash);
        const entry_hash = try takeString(entry_obj, "entry_hash", allocator);
        errdefer allocator.free(entry_hash);

        var sources: std.ArrayList(SourceRow) = .empty;
        errdefer {
            for (sources.items) |sr| {
                allocator.free(sr.path);
                allocator.free(sr.hash);
            }
            sources.deinit(allocator);
        }
        const sources_v = entry_obj.get("sources") orelse return error.MissingField;
        const sources_obj = switch (sources_v) {
            .object => |o| o,
            else => return error.MalformedJson,
        };
        var sit = sources_obj.iterator();
        while (sit.next()) |skv| {
            const hash_str: []const u8 = switch (skv.value_ptr.*) {
                .string => |s| s,
                else => return error.MalformedJson,
            };
            try sources.append(allocator, .{
                .path = try allocator.dupe(u8, skv.key_ptr.*),
                .hash = try allocator.dupe(u8, hash_str),
            });
        }
        std.mem.sort(SourceRow, sources.items, {}, lessThanSourcePath);

        try entries.append(allocator, .{
            .path = try allocator.dupe(u8, kv.key_ptr.*),
            .entry = .{
                .doc_hash = doc_hash,
                .sources = try sources.toOwnedSlice(allocator),
                .sources_hash = sources_hash,
                .entry_hash = entry_hash,
            },
        });
    }
    std.mem.sort(EntryRow, entries.items, {}, lessThanEntryPath);

    var nodoc_rows: std.ArrayList(NodocRow) = .empty;
    errdefer {
        for (nodoc_rows.items) |row| {
            allocator.free(row.path);
            allocator.free(row.hash);
        }
        nodoc_rows.deinit(allocator);
    }
    if (root.get("nodoc")) |nd_v| {
        const nd_obj = switch (nd_v) {
            .object => |o| o,
            else => return error.MalformedJson,
        };
        var nit = nd_obj.iterator();
        while (nit.next()) |kv| {
            const hash_str: []const u8 = switch (kv.value_ptr.*) {
                .string => |s| s,
                else => return error.MalformedJson,
            };
            try nodoc_rows.append(allocator, .{
                .path = try allocator.dupe(u8, kv.key_ptr.*),
                .hash = try allocator.dupe(u8, hash_str),
            });
        }
    }
    std.mem.sort(NodocRow, nodoc_rows.items, {}, lessThanNodocPath);

    return .{
        .version = version,
        .algo = algo_dup,
        .root = root_hash,
        .generated_at = gen_at,
        .entries = try entries.toOwnedSlice(allocator),
        .nodoc = try nodoc_rows.toOwnedSlice(allocator),
    };
}

fn takeString(obj: std.json.ObjectMap, key: []const u8, allocator: std.mem.Allocator) ![]const u8 {
    const v = obj.get(key) orelse return error.MissingField;
    const s = switch (v) {
        .string => |ss| ss,
        else => return error.MalformedJson,
    };
    return allocator.dupe(u8, s);
}

fn lessThanSourcePath(_: void, a: SourceRow, b: SourceRow) bool {
    return std.mem.order(u8, a.path, b.path) == .lt;
}

fn lessThanEntryPath(_: void, a: EntryRow, b: EntryRow) bool {
    return std.mem.order(u8, a.path, b.path) == .lt;
}

fn lessThanNodocPath(_: void, a: NodocRow, b: NodocRow) bool {
    return std.mem.order(u8, a.path, b.path) == .lt;
}

// ============================================================================
// Tests
// ============================================================================

const testing = std.testing;

test "formatHash zero-pads to 16 hex chars" {
    const s = try formatHash(testing.allocator, 0x1234abcd);
    defer testing.allocator.free(s);
    try testing.expectEqualStrings("000000001234abcd", s);
}

test "computeSourcesHash sorts entries before hashing" {
    const a = [_]SourceRow{
        .{ .path = "b/", .hash = "1111111111111111" },
        .{ .path = "a/", .hash = "0000000000000000" },
    };
    const b = [_]SourceRow{
        .{ .path = "a/", .hash = "0000000000000000" },
        .{ .path = "b/", .hash = "1111111111111111" },
    };
    const h1 = try computeSourcesHash(testing.allocator, &a);
    defer testing.allocator.free(h1);
    const h2 = try computeSourcesHash(testing.allocator, &b);
    defer testing.allocator.free(h2);
    try testing.expectEqualStrings(h1, h2);
}

test "computeRoot stable across entry-order permutation" {
    const sources_a = [_]SourceRow{};
    const sources_b = [_]SourceRow{};

    const e1 = EntryRow{
        .path = "docs/a.md",
        .entry = .{
            .doc_hash = "aaaaaaaaaaaaaaaa",
            .sources = &sources_a,
            .sources_hash = "0000000000000000",
            .entry_hash = "1111111111111111",
        },
    };
    const e2 = EntryRow{
        .path = "docs/b.md",
        .entry = .{
            .doc_hash = "bbbbbbbbbbbbbbbb",
            .sources = &sources_b,
            .sources_hash = "0000000000000000",
            .entry_hash = "2222222222222222",
        },
    };
    const nodoc: []const NodocRow = &.{};
    const order_a = [_]EntryRow{ e1, e2 };
    const order_b = [_]EntryRow{ e2, e1 };

    const r1 = try computeRoot(testing.allocator, &order_a, nodoc);
    defer testing.allocator.free(r1);
    const r2 = try computeRoot(testing.allocator, &order_b, nodoc);
    defer testing.allocator.free(r2);
    try testing.expectEqualStrings(r1, r2);
}

test "computeRoot changes when entries change" {
    const sources: []const SourceRow = &.{};
    const e1 = EntryRow{
        .path = "docs/a.md",
        .entry = .{
            .doc_hash = "aaaaaaaaaaaaaaaa",
            .sources = sources,
            .sources_hash = "0000000000000000",
            .entry_hash = "1111111111111111",
        },
    };
    const e2 = EntryRow{
        .path = "docs/a.md",
        .entry = .{
            .doc_hash = "bbbbbbbbbbbbbbbb",
            .sources = sources,
            .sources_hash = "0000000000000000",
            .entry_hash = "2222222222222222",
        },
    };
    const r1 = try computeRoot(testing.allocator, &.{e1}, &.{});
    defer testing.allocator.free(r1);
    const r2 = try computeRoot(testing.allocator, &.{e2}, &.{});
    defer testing.allocator.free(r2);
    try testing.expect(!std.mem.eql(u8, r1, r2));
}

test "JSON round-trip preserves all fields" {
    const sources = [_]SourceRow{
        .{ .path = "src/engine/", .hash = "1234567890abcdef" },
        .{ .path = "migrations/", .hash = "fedcba0987654321" },
    };
    const entry = Entry{
        .doc_hash = "aaaaaaaaaaaaaaaa",
        .sources = &sources,
        .sources_hash = try computeSourcesHash(testing.allocator, &sources),
        .entry_hash = "9999999999999999",
    };
    defer testing.allocator.free(entry.sources_hash);

    const nodoc = [_]NodocRow{
        .{ .path = "src/engine/llm/llm.zig", .hash = "babababababababa" },
    };
    const entries = [_]EntryRow{
        .{ .path = "docs/architecture.md", .entry = entry },
    };

    const manifest = Manifest{
        .version = version,
        .algo = algo,
        .root = "rootrootrootroot",
        .generated_at = "2026-05-29T00:00:00Z",
        .entries = &entries,
        .nodoc = &nodoc,
    };

    const json = try toJson(testing.allocator, manifest);
    defer testing.allocator.free(json);

    const parsed = try fromJson(testing.allocator, json);
    defer deinitManifest(testing.allocator, parsed);

    try testing.expectEqual(@as(i64, 2), parsed.version);
    try testing.expectEqualStrings("xxh64", parsed.algo);
    try testing.expectEqualStrings("rootrootrootroot", parsed.root);
    try testing.expectEqualStrings("2026-05-29T00:00:00Z", parsed.generated_at);
    try testing.expectEqual(@as(usize, 1), parsed.entries.len);
    try testing.expectEqualStrings("docs/architecture.md", parsed.entries[0].path);
    try testing.expectEqualStrings("aaaaaaaaaaaaaaaa", parsed.entries[0].entry.doc_hash);
    try testing.expectEqual(@as(usize, 2), parsed.entries[0].entry.sources.len);
    // Sources sorted by path.
    try testing.expectEqualStrings("migrations/", parsed.entries[0].entry.sources[0].path);
    try testing.expectEqualStrings("src/engine/", parsed.entries[0].entry.sources[1].path);
    try testing.expectEqual(@as(usize, 1), parsed.nodoc.len);
    try testing.expectEqualStrings("src/engine/llm/llm.zig", parsed.nodoc[0].path);
}

test "fromJson rejects v1 version" {
    const json =
        \\{"version":1,"algo":"xxh64","root":"","generated_at":"","entries":{},"nodoc":{}}
    ;
    try testing.expectError(error.IncompatibleVersion, fromJson(testing.allocator, json));
}

test "fromJson rejects unknown algorithm" {
    const json =
        \\{"version":2,"algo":"blake3","root":"","generated_at":"","entries":{},"nodoc":{}}
    ;
    try testing.expectError(error.IncompatibleAlgorithm, fromJson(testing.allocator, json));
}
