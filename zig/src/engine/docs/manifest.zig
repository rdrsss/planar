//! engine/docs/manifest - `.manifest-docs` Merkle-style drift manifest.

const std = @import("std");

pub const algo = "xxh64";
pub const version: i64 = 1;
pub const file_name = ".manifest-docs";
pub const default_docs_root = "docs";

pub const SourceRow = struct {
    ref: []const u8,
    hash: []const u8,
};

pub const SourceOverlay = struct {
    path: []const u8,
    sources: []const SourceRow,
};

pub const Entry = struct {
    doc_hash: []const u8,
    sources: []const SourceRow,
    sources_hash: []const u8,
    entry_hash: []const u8,
};

pub const Manifest = struct {
    version: i64,
    algo: []const u8,
    root: []const u8,
    generated_at: []const u8,
    entries: []const EntryRow,
};

pub const EntryRow = struct {
    path: []const u8,
    entry: Entry,
};

pub const Signal = enum {
    @"regenerate-candidate",
    @"hand-edit",
    @"new-authoring",
    deletion,

    pub fn toText(self: Signal) []const u8 {
        return @tagName(self);
    }
};

pub const Change = struct {
    path: []const u8,
    signal: Signal,
    detail: []const u8,
};

pub fn deinitManifest(m: Manifest, allocator: std.mem.Allocator) void {
    allocator.free(m.algo);
    allocator.free(m.root);
    allocator.free(m.generated_at);
    for (m.entries) |row| deinitEntryRow(row, allocator);
    allocator.free(m.entries);
}

pub fn deinitChanges(changes: []const Change, allocator: std.mem.Allocator) void {
    for (changes) |change| {
        allocator.free(change.path);
        allocator.free(change.detail);
    }
    allocator.free(changes);
}

pub fn normalize(allocator: std.mem.Allocator, content: []const u8) ![]const u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var normalized = try allocator.alloc(u8, content.len);
    defer allocator.free(normalized);
    var n: usize = 0;
    var i: usize = 0;
    while (i < content.len) : (i += 1) {
        if (content[i] == '\r') {
            if (i + 1 < content.len and content[i + 1] == '\n') i += 1;
            normalized[n] = '\n';
        } else {
            normalized[n] = content[i];
        }
        n += 1;
    }

    const body = try writeFrontMatterNormalized(allocator, &out, normalized[0..n]);
    var lines = std.mem.splitScalar(u8, body, '\n');
    while (lines.next()) |line| {
        try out.appendSlice(allocator, trimRight(line, " \t"));
        try out.append(allocator, '\n');
    }
    while (out.items.len > 1 and out.items[out.items.len - 1] == '\n' and out.items[out.items.len - 2] == '\n') {
        _ = out.pop();
    }
    if (out.items.len == 0 or out.items[out.items.len - 1] != '\n') try out.append(allocator, '\n');
    return try out.toOwnedSlice(allocator);
}

pub fn build(root: []const u8, allocator: std.mem.Allocator) !Manifest {
    return try buildWithSources(root, allocator, &.{});
}

pub fn buildWithSources(root: []const u8, allocator: std.mem.Allocator, overlays: []const SourceOverlay) !Manifest {
    var rows: std.ArrayList(EntryRow) = .empty;
    errdefer {
        for (rows.items) |row| deinitEntryRow(row, allocator);
        rows.deinit(allocator);
    }
    try walkMarkdown(root, root, allocator, overlays, &rows);
    std.mem.sort(EntryRow, rows.items, {}, struct {
        fn lessThan(_: void, a: EntryRow, b: EntryRow) bool {
            return std.mem.lessThan(u8, a.path, b.path);
        }
    }.lessThan);
    const root_hash = try computeRoot(rows.items, allocator);
    return .{
        .version = version,
        .algo = try allocator.dupe(u8, algo),
        .root = root_hash,
        .generated_at = try allocator.dupe(u8, "now"),
        .entries = try rows.toOwnedSlice(allocator),
    };
}

pub fn write(path: []const u8, m: Manifest, allocator: std.mem.Allocator) !void {
    var body: std.Io.Writer.Allocating = .init(allocator);
    defer body.deinit();
    const w = &body.writer;
    try w.print("{{\"version\":{d},\"algo\":", .{m.version});
    try writeJSONString(w, m.algo);
    try w.print(",\"root\":", .{});
    try writeJSONString(w, m.root);
    try w.print(",\"generated_at\":", .{});
    try writeJSONString(w, m.generated_at);
    try w.print(",\"entries\":{{", .{});
    for (m.entries, 0..) |row, i| {
        if (i > 0) try w.print(",", .{});
        try writeJSONString(w, row.path);
        try w.print(":{{\"doc_hash\":", .{});
        try writeJSONString(w, row.entry.doc_hash);
        try w.print(",\"sources\":{{", .{});
        for (row.entry.sources, 0..) |source, j| {
            if (j > 0) try w.print(",", .{});
            try writeJSONString(w, source.ref);
            try w.print(":", .{});
            try writeJSONString(w, source.hash);
        }
        try w.print("}},\"sources_hash\":", .{});
        try writeJSONString(w, row.entry.sources_hash);
        try w.print(",\"entry_hash\":", .{});
        try writeJSONString(w, row.entry.entry_hash);
        try w.print("}}", .{});
    }
    try w.print("}}\n}}\n", .{});
    try w.flush();
    try writeFileAtomic(allocator, path, body.written());
}

pub fn load(path: []const u8, allocator: std.mem.Allocator) !Manifest {
    const raw = try std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(16 * 1024 * 1024));
    defer allocator.free(raw);
    var parsed = try std.json.parseFromSlice(std.json.Value, allocator, raw, .{});
    defer parsed.deinit();
    if (parsed.value != .object) return error.InvalidManifest;
    const obj = parsed.value.object;
    const entries_v = obj.get("entries") orelse return error.InvalidManifest;
    if (entries_v != .object) return error.InvalidManifest;

    var rows: std.ArrayList(EntryRow) = .empty;
    errdefer {
        for (rows.items) |row| deinitEntryRow(row, allocator);
        rows.deinit(allocator);
    }
    var it = entries_v.object.iterator();
    while (it.next()) |kv| {
        const entry_v = kv.value_ptr.*;
        if (entry_v != .object) return error.InvalidManifest;
        const entry_obj = entry_v.object;
        var sources: std.ArrayList(SourceRow) = .empty;
        errdefer {
            for (sources.items) |s| {
                allocator.free(s.ref);
                allocator.free(s.hash);
            }
            sources.deinit(allocator);
        }
        if (entry_obj.get("sources")) |sources_v| {
            if (sources_v != .object) return error.InvalidManifest;
            var sit = sources_v.object.iterator();
            while (sit.next()) |skv| {
                if (skv.value_ptr.* != .string) return error.InvalidManifest;
                try sources.append(allocator, .{
                    .ref = try allocator.dupe(u8, skv.key_ptr.*),
                    .hash = try allocator.dupe(u8, skv.value_ptr.*.string),
                });
            }
        }
        std.mem.sort(SourceRow, sources.items, {}, sourceLessThan);
        try rows.append(allocator, .{
            .path = try allocator.dupe(u8, kv.key_ptr.*),
            .entry = .{
                .doc_hash = try allocator.dupe(u8, getString(entry_obj, "doc_hash") orelse return error.InvalidManifest),
                .sources = try sources.toOwnedSlice(allocator),
                .sources_hash = try allocator.dupe(u8, getString(entry_obj, "sources_hash") orelse return error.InvalidManifest),
                .entry_hash = try allocator.dupe(u8, getString(entry_obj, "entry_hash") orelse return error.InvalidManifest),
            },
        });
    }
    std.mem.sort(EntryRow, rows.items, {}, struct {
        fn lessThan(_: void, a: EntryRow, b: EntryRow) bool {
            return std.mem.lessThan(u8, a.path, b.path);
        }
    }.lessThan);
    return .{
        .version = if (obj.get("version")) |v| if (v == .integer) v.integer else version else version,
        .algo = try allocator.dupe(u8, getString(obj, "algo") orelse algo),
        .root = try allocator.dupe(u8, getString(obj, "root") orelse ""),
        .generated_at = try allocator.dupe(u8, getString(obj, "generated_at") orelse ""),
        .entries = try rows.toOwnedSlice(allocator),
    };
}

pub fn verify(stored: Manifest, current: Manifest) bool {
    return stored.version == current.version and
        std.mem.eql(u8, stored.algo, current.algo) and
        std.mem.eql(u8, stored.root, current.root);
}

pub fn diff(stored: Manifest, current: Manifest, allocator: std.mem.Allocator) ![]Change {
    var changes: std.ArrayList(Change) = .empty;
    errdefer deinitChanges(changes.items, allocator);
    for (stored.entries) |s| {
        const c = findEntry(current.entries, s.path) orelse {
            try appendChange(&changes, allocator, s.path, .deletion, "path not present in current scan");
            continue;
        };
        if (std.mem.eql(u8, s.entry.entry_hash, c.entry.entry_hash)) continue;
        if (!std.mem.eql(u8, s.entry.sources_hash, c.entry.sources_hash)) {
            try appendChange(&changes, allocator, s.path, .@"regenerate-candidate", "sources changed");
        } else if (!std.mem.eql(u8, s.entry.doc_hash, c.entry.doc_hash)) {
            try appendChange(&changes, allocator, s.path, .@"hand-edit", "doc hash differs; sources unchanged");
        }
    }
    for (current.entries) |c| {
        if (findEntry(stored.entries, c.path) == null) {
            try appendChange(&changes, allocator, c.path, .@"new-authoring", "path not present in stored manifest");
        }
    }
    std.mem.sort(Change, changes.items, {}, struct {
        fn lessThan(_: void, a: Change, b: Change) bool {
            return std.mem.lessThan(u8, a.path, b.path);
        }
    }.lessThan);
    return try changes.toOwnedSlice(allocator);
}

fn appendChange(changes: *std.ArrayList(Change), allocator: std.mem.Allocator, path: []const u8, signal: Signal, detail: []const u8) !void {
    try changes.append(allocator, .{
        .path = try allocator.dupe(u8, path),
        .signal = signal,
        .detail = try allocator.dupe(u8, detail),
    });
}

fn walkMarkdown(root: []const u8, dir_path: []const u8, allocator: std.mem.Allocator, overlays: []const SourceOverlay, rows: *std.ArrayList(EntryRow)) !void {
    var dir = try std.Io.Dir.cwd().openDir(fsIo(), dir_path, .{ .iterate = true });
    defer dir.close(fsIo());
    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        if (std.mem.eql(u8, entry.name, ".") or std.mem.eql(u8, entry.name, "..")) continue;
        const path = try std.fs.path.join(allocator, &.{ dir_path, entry.name });
        defer allocator.free(path);
        switch (entry.kind) {
            .directory => try walkMarkdown(root, path, allocator, overlays, rows),
            .file => {
                if (!std.ascii.endsWithIgnoreCase(entry.name, ".md")) {
                    continue;
                }
                const raw = try std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(16 * 1024 * 1024));
                defer allocator.free(raw);
                const norm = try normalize(allocator, raw);
                defer allocator.free(norm);
                const doc_hash = try hashAlloc(allocator, norm);
                const source_overlay = findOverlay(overlays, path);
                const sources = try cloneSources(allocator, if (source_overlay) |o| o.sources else &.{});
                errdefer deinitSources(sources, allocator);
                const sources_canonical = try canonicalSourcesJSON(allocator, sources);
                defer allocator.free(sources_canonical);
                const sources_hash = try hashAlloc(allocator, sources_canonical);
                const entry_hash_input = try std.fmt.allocPrint(allocator, "{s}|{s}", .{ doc_hash, sources_hash });
                defer allocator.free(entry_hash_input);
                const entry_hash = try hashAlloc(allocator, entry_hash_input);
                const rel = path;
                try rows.append(allocator, .{
                    .path = try allocator.dupe(u8, rel),
                    .entry = .{
                        .doc_hash = doc_hash,
                        .sources = sources,
                        .sources_hash = sources_hash,
                        .entry_hash = entry_hash,
                    },
                });
            },
            else => {},
        }
    }
}

fn findEntry(rows: []const EntryRow, path: []const u8) ?EntryRow {
    for (rows) |row| if (std.mem.eql(u8, row.path, path)) return row;
    return null;
}

fn computeRoot(rows: []const EntryRow, allocator: std.mem.Allocator) ![]const u8 {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);
    for (rows) |row| {
        try buf.appendSlice(allocator, row.path);
        try buf.append(allocator, 0);
        try buf.appendSlice(allocator, row.entry.entry_hash);
        try buf.append(allocator, '\n');
    }
    return try hashAlloc(allocator, buf.items);
}

fn hashAlloc(allocator: std.mem.Allocator, content: []const u8) ![]const u8 {
    const sum = std.hash.XxHash64.hash(0, content);
    return try std.fmt.allocPrint(allocator, "{x:0>16}", .{sum});
}

pub fn hashBytes(allocator: std.mem.Allocator, content: []const u8) ![]const u8 {
    return try hashAlloc(allocator, content);
}

fn writeFrontMatterNormalized(allocator: std.mem.Allocator, out: *std.ArrayList(u8), content: []const u8) ![]const u8 {
    if (!std.mem.startsWith(u8, content, "---\n")) return content;
    const rest = content[4..];
    const idx = std.mem.indexOf(u8, rest, "\n---\n") orelse return content;
    const fm = rest[0..idx];
    const body = rest[idx + 5 ..];
    try out.appendSlice(allocator, "---\n");
    var lines = std.mem.splitScalar(u8, fm, '\n');
    while (lines.next()) |line| {
        const trimmed = trimRight(line, " \t");
        const key = std.mem.trim(u8, trimmed, " \t");
        if (std.mem.startsWith(u8, key, "regenerated_at:")) continue;
        if (std.mem.startsWith(u8, key, "source_versions:")) continue;
        try out.appendSlice(allocator, trimmed);
        try out.append(allocator, '\n');
    }
    try out.appendSlice(allocator, "---\n");
    return body;
}

fn deinitEntryRow(row: EntryRow, allocator: std.mem.Allocator) void {
    allocator.free(row.path);
    allocator.free(row.entry.doc_hash);
    deinitSources(row.entry.sources, allocator);
    allocator.free(row.entry.sources_hash);
    allocator.free(row.entry.entry_hash);
}

fn deinitSources(sources: []const SourceRow, allocator: std.mem.Allocator) void {
    for (sources) |source| {
        allocator.free(source.ref);
        allocator.free(source.hash);
    }
    allocator.free(sources);
}

fn cloneSources(allocator: std.mem.Allocator, sources: []const SourceRow) ![]SourceRow {
    var out: std.ArrayList(SourceRow) = .empty;
    errdefer {
        for (out.items) |source| {
            allocator.free(source.ref);
            allocator.free(source.hash);
        }
        out.deinit(allocator);
    }
    for (sources) |source| {
        try out.append(allocator, .{
            .ref = try allocator.dupe(u8, source.ref),
            .hash = try allocator.dupe(u8, source.hash),
        });
    }
    std.mem.sort(SourceRow, out.items, {}, sourceLessThan);
    return try out.toOwnedSlice(allocator);
}

fn findOverlay(overlays: []const SourceOverlay, path: []const u8) ?SourceOverlay {
    for (overlays) |overlay| {
        if (std.mem.eql(u8, overlay.path, path)) return overlay;
    }
    return null;
}

fn sourceLessThan(_: void, a: SourceRow, b: SourceRow) bool {
    return std.mem.lessThan(u8, a.ref, b.ref);
}

fn canonicalSourcesJSON(allocator: std.mem.Allocator, sources: []const SourceRow) ![]const u8 {
    var buf: std.Io.Writer.Allocating = .init(allocator);
    errdefer buf.deinit();
    const w = &buf.writer;
    try w.print("{{", .{});
    for (sources, 0..) |source, i| {
        if (i > 0) try w.print(",", .{});
        try writeJSONString(w, source.ref);
        try w.print(":", .{});
        try writeJSONString(w, source.hash);
    }
    try w.print("}}", .{});
    try w.flush();
    return try buf.toOwnedSlice();
}

fn getString(obj: std.json.ObjectMap, key: []const u8) ?[]const u8 {
    const value = obj.get(key) orelse return null;
    if (value != .string) return null;
    return value.string;
}

fn writeJSONString(writer: *std.Io.Writer, s: []const u8) !void {
    try std.json.Stringify.encodeJsonString(s, .{}, writer);
}

fn writeFileAtomic(allocator: std.mem.Allocator, path: []const u8, content: []const u8) !void {
    if (std.fs.path.dirname(path)) |dir| {
        try std.Io.Dir.cwd().createDirPath(fsIo(), dir);
    }
    const tmp = try std.fmt.allocPrint(allocator, "{s}.tmp", .{path});
    defer allocator.free(tmp);
    {
        try std.Io.Dir.cwd().writeFile(fsIo(), .{ .sub_path = tmp, .data = content });
    }
    try std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), path, fsIo());
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

fn trimRight(s: []const u8, values: []const u8) []const u8 {
    var end = s.len;
    while (end > 0 and std.mem.indexOfScalar(u8, values, s[end - 1]) != null) : (end -= 1) {}
    return s[0..end];
}

test "normalize folds line endings and trims trailing whitespace" {
    const a = std.testing.allocator;
    const got = try normalize(a, "a  \r\nb\t\r\n\r\n");
    defer a.free(got);
    try std.testing.expectEqualStrings("a\nb\n", got);
}

test "diff classifies hand edit and new authoring" {
    const a = std.testing.allocator;
    const stored = Manifest{
        .version = version,
        .algo = algo,
        .root = "old",
        .generated_at = "now",
        .entries = &.{
            .{ .path = "docs/a.md", .entry = .{ .doc_hash = "a", .sources = &.{}, .sources_hash = "s", .entry_hash = "a|s" } },
        },
    };
    const current = Manifest{
        .version = version,
        .algo = algo,
        .root = "new",
        .generated_at = "now",
        .entries = &.{
            .{ .path = "docs/a.md", .entry = .{ .doc_hash = "b", .sources = &.{}, .sources_hash = "s", .entry_hash = "b|s" } },
            .{ .path = "docs/b.md", .entry = .{ .doc_hash = "x", .sources = &.{}, .sources_hash = "s", .entry_hash = "x|s" } },
        },
    };
    const changes = try diff(stored, current, a);
    defer deinitChanges(changes, a);
    try std.testing.expectEqual(@as(usize, 2), changes.len);
    try std.testing.expectEqual(Signal.@"hand-edit", changes[0].signal);
    try std.testing.expectEqual(Signal.@"new-authoring", changes[1].signal);
}
