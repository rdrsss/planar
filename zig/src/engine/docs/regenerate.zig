//! engine/docs/regenerate - re-render docs from provenance sources.

const std = @import("std");
const db = @import("db");
const manifest = @import("manifest.zig");
const promote = @import("promote.zig");

pub const Mode = enum { path, slug, all };

pub const Target = struct {
    path: []const u8,
    kind: []const u8,
    hand_edit_detected: bool,
};

pub const Result = struct {
    path: []const u8,
    kind: []const u8,
    hand_edit_detected: bool,
    action: []const u8,
    manifest_root: []const u8,
};

pub fn deinitTargets(targets: []const Target, allocator: std.mem.Allocator) void {
    for (targets) |target| {
        allocator.free(target.path);
        allocator.free(target.kind);
    }
    allocator.free(targets);
}

pub fn deinitResult(result: Result, allocator: std.mem.Allocator) void {
    allocator.free(result.path);
    allocator.free(result.kind);
    allocator.free(result.action);
    allocator.free(result.manifest_root);
}

pub fn candidates(d: *db.sqlite.Db, allocator: std.mem.Allocator) ![]Target {
    const stored = manifest.load(manifest.file_name, allocator) catch |e| switch (e) {
        error.FileNotFound => return try allocator.alloc(Target, 0),
        else => return e,
    };
    defer manifest.deinitManifest(stored, allocator);

    var out: std.ArrayList(Target) = .empty;
    errdefer deinitTargets(out.items, allocator);
    for (stored.entries) |entry| {
        if (entry.entry.sources.len == 0) continue;
        if (!pathExists(entry.path)) continue;
        var drifted = false;
        for (entry.entry.sources) |source| {
            const body = promote.resolveSourceBody(d, allocator, source.ref) catch {
                drifted = true;
                break;
            };
            defer allocator.free(body);
            const fresh_hash = try manifest.hashBytes(allocator, body);
            defer allocator.free(fresh_hash);
            if (!std.mem.eql(u8, fresh_hash, source.hash)) {
                drifted = true;
                break;
            }
        }
        if (!drifted) continue;
        const prov = parseProvenanceFile(allocator, entry.path) catch Provenance{};
        defer deinitProvenance(prov, allocator);
        try out.append(allocator, .{
            .path = try allocator.dupe(u8, entry.path),
            .kind = try allocator.dupe(u8, prov.kind orelse ""),
            .hand_edit_detected = try detectHandEdit(allocator, stored, entry.path),
        });
    }
    std.mem.sort(Target, out.items, {}, struct {
        fn lessThan(_: void, a: Target, b: Target) bool {
            return std.mem.lessThan(u8, a.path, b.path);
        }
    }.lessThan);
    return try out.toOwnedSlice(allocator);
}

pub fn runPath(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    path: []const u8,
    body_file: []const u8,
    force: bool,
    merge: bool,
    out_override: ?[]const u8,
) !Result {
    if (force and merge) return error.InvalidInput;
    if (body_file.len == 0) return error.NoBodyProvided;

    const prior = manifest.load(manifest.file_name, allocator) catch |e| switch (e) {
        error.FileNotFound => null,
        else => return e,
    };
    defer if (prior) |p| manifest.deinitManifest(p, allocator);

    const hand_edit = if (prior) |p| try detectHandEdit(allocator, p, path) else false;
    if (hand_edit and !force and !merge) return error.HandEditDetected;

    const prov = try parseProvenanceFile(allocator, path);
    defer deinitProvenance(prov, allocator);
    const refs = try provenanceRefs(allocator, prov);
    defer allocator.free(refs);
    if (refs.len == 0) return error.InvalidInput;
    const resolved = try promote.resolveSources(d, allocator, refs);
    defer promote.deinitResolvedSources(resolved, allocator);

    const body = try std.Io.Dir.cwd().readFileAlloc(fsIo(), body_file, allocator, std.Io.Limit.limited(16 * 1024 * 1024));
    defer allocator.free(body);
    const final = try promote.composeDocument(allocator, .{
        .title = prov.title orelse std.fs.path.stem(std.fs.path.basename(path)),
        .kind = prov.kind orelse "",
        .regenerated_by = "pl-doc-regenerate",
        .sources = resolved,
        .body = body,
    });
    defer allocator.free(final);

    const write_path = if (merge and hand_edit)
        try std.fmt.allocPrint(allocator, "{s}.regenerated.md", .{path})
    else if (out_override) |out| if (out.len > 0) try allocator.dupe(u8, out) else try allocator.dupe(u8, path) else try allocator.dupe(u8, path);
    defer allocator.free(write_path);
    const action = if (merge and hand_edit) "merged" else if (force and hand_edit) "forced" else "regenerated";

    try writeFileAtomic(allocator, write_path, final);
    const source_rows = try sourceRowsFromResolved(allocator, resolved);
    defer deinitSourceRows(source_rows, allocator);
    const root = try promote.updateManifest(allocator, write_path, source_rows);

    return .{
        .path = try allocator.dupe(u8, write_path),
        .kind = try allocator.dupe(u8, prov.kind orelse ""),
        .hand_edit_detected = hand_edit,
        .action = try allocator.dupe(u8, action),
        .manifest_root = root,
    };
}

pub const Provenance = struct {
    title: ?[]const u8 = null,
    kind: ?[]const u8 = null,
    refs: []const []const u8 = &.{},
};

pub fn deinitProvenance(prov: Provenance, allocator: std.mem.Allocator) void {
    if (prov.title) |v| allocator.free(v);
    if (prov.kind) |v| allocator.free(v);
    for (prov.refs) |ref| allocator.free(ref);
    allocator.free(prov.refs);
}

pub fn parseProvenanceFile(allocator: std.mem.Allocator, path: []const u8) !Provenance {
    const raw = try std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(16 * 1024 * 1024));
    defer allocator.free(raw);
    return try parseProvenance(allocator, raw);
}

pub fn parseProvenance(allocator: std.mem.Allocator, content: []const u8) !Provenance {
    const fm = frontMatter(content) orelse return error.InvalidInput;
    var prov = Provenance{};
    errdefer deinitProvenance(prov, allocator);
    var refs: std.ArrayList([]const u8) = .empty;
    var current_array: ?[]const u8 = null;
    var lines = std.mem.splitScalar(u8, fm, '\n');
    while (lines.next()) |raw_line| {
        const line = trimRight(raw_line, " \t\r");
        const trimmed = std.mem.trim(u8, line, " \t");
        if (trimmed.len == 0) continue;
        if (std.mem.startsWith(u8, trimmed, "title:")) {
            prov.title = try allocator.dupe(u8, unquote(std.mem.trim(u8, trimmed["title:".len..], " \t")));
            current_array = null;
        } else if (std.mem.startsWith(u8, trimmed, "doc_kind:")) {
            prov.kind = try allocator.dupe(u8, unquote(std.mem.trim(u8, trimmed["doc_kind:".len..], " \t")));
            current_array = null;
        } else if (std.mem.eql(u8, trimmed, "source_artifacts:") or
            std.mem.eql(u8, trimmed, "source_decisions:") or
            std.mem.eql(u8, trimmed, "source_plans:"))
        {
            current_array = trimmed;
        } else if (current_array != null and std.mem.startsWith(u8, trimmed, "- ")) {
            try refs.append(allocator, try allocator.dupe(u8, unquote(std.mem.trim(u8, trimmed[2..], " \t"))));
        } else if (!std.mem.startsWith(u8, line, " ")) {
            current_array = null;
        }
    }
    prov.refs = try refs.toOwnedSlice(allocator);
    return prov;
}

fn provenanceRefs(allocator: std.mem.Allocator, prov: Provenance) ![]const []const u8 {
    var out = try allocator.alloc([]const u8, prov.refs.len);
    for (prov.refs, 0..) |ref, i| out[i] = ref;
    return out;
}

fn detectHandEdit(allocator: std.mem.Allocator, prior: manifest.Manifest, path: []const u8) !bool {
    const entry = findEntry(prior, path) orelse return false;
    const raw = try std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(16 * 1024 * 1024));
    defer allocator.free(raw);
    const norm = try manifest.normalize(allocator, raw);
    defer allocator.free(norm);
    const hash = try manifest.hashBytes(allocator, norm);
    defer allocator.free(hash);
    return !std.mem.eql(u8, hash, entry.entry.doc_hash);
}

fn findEntry(m: manifest.Manifest, path: []const u8) ?manifest.EntryRow {
    for (m.entries) |entry| if (std.mem.eql(u8, entry.path, path)) return entry;
    return null;
}

fn sourceRowsFromResolved(allocator: std.mem.Allocator, sources: []const promote.ResolvedSource) ![]manifest.SourceRow {
    var out: std.ArrayList(manifest.SourceRow) = .empty;
    errdefer deinitSourceRows(out.items, allocator);
    for (sources) |source| {
        try out.append(allocator, .{
            .ref = try allocator.dupe(u8, source.ref),
            .hash = try allocator.dupe(u8, source.hash),
        });
    }
    return try out.toOwnedSlice(allocator);
}

fn deinitSourceRows(sources: []const manifest.SourceRow, allocator: std.mem.Allocator) void {
    for (sources) |source| {
        allocator.free(source.ref);
        allocator.free(source.hash);
    }
    allocator.free(sources);
}

fn writeFileAtomic(allocator: std.mem.Allocator, path: []const u8, content: []const u8) !void {
    if (std.fs.path.dirname(path)) |dir| try std.Io.Dir.cwd().createDirPath(fsIo(), dir);
    const tmp = try std.fmt.allocPrint(allocator, "{s}.tmp", .{path});
    defer allocator.free(tmp);
    {
        try std.Io.Dir.cwd().writeFile(fsIo(), .{ .sub_path = tmp, .data = content });
    }
    try std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), path, fsIo());
}

fn pathExists(path: []const u8) bool {
    std.Io.Dir.cwd().access(fsIo(), path, .{}) catch return false;
    return true;
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

fn frontMatter(content: []const u8) ?[]const u8 {
    if (!std.mem.startsWith(u8, content, "---\n")) return null;
    const rest = content[4..];
    const idx = std.mem.indexOf(u8, rest, "\n---\n") orelse return null;
    return rest[0..idx];
}

fn trimRight(s: []const u8, values: []const u8) []const u8 {
    var end = s.len;
    while (end > 0 and std.mem.indexOfScalar(u8, values, s[end - 1]) != null) : (end -= 1) {}
    return s[0..end];
}

fn unquote(s: []const u8) []const u8 {
    if (s.len >= 2 and ((s[0] == '"' and s[s.len - 1] == '"') or (s[0] == '\'' and s[s.len - 1] == '\''))) {
        return s[1 .. s.len - 1];
    }
    return s;
}

test "parseProvenance extracts source refs" {
    const a = std.testing.allocator;
    const raw =
        "---\n" ++
        "title: \"T\"\n" ++
        "doc_kind: \"feature\"\n" ++
        "source_artifacts:\n" ++
        "  - \"artifact:1\"\n" ++
        "---\n" ++
        "body\n";
    const prov = try parseProvenance(a, raw);
    defer deinitProvenance(prov, a);
    try std.testing.expectEqualStrings("feature", prov.kind.?);
    try std.testing.expectEqual(@as(usize, 1), prov.refs.len);
    try std.testing.expectEqualStrings("artifact:1", prov.refs[0]);
}
