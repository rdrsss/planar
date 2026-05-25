//! engine/docs/promote - artifact/decision/plan to published docs promotion.

const std = @import("std");
const db = @import("db");
const manifest = @import("manifest.zig");

pub const doc_kinds = [_][]const u8{
    "feature",
    "getting_started",
    "adr_index",
    "changelog",
    "glossary",
    "research",
    "agents",
};

pub const ResolvedSource = struct {
    ref: []const u8,
    body: []const u8,
    hash: []const u8,
};

pub const Result = struct {
    path: []const u8,
    kind: []const u8,
    sources: []const manifest.SourceRow,
    manifest_root: []const u8,
};

pub fn deinitResolvedSources(sources: []const ResolvedSource, allocator: std.mem.Allocator) void {
    for (sources) |source| {
        allocator.free(source.ref);
        allocator.free(source.body);
        allocator.free(source.hash);
    }
    allocator.free(sources);
}

pub fn deinitResult(result: Result, allocator: std.mem.Allocator) void {
    allocator.free(result.path);
    allocator.free(result.kind);
    for (result.sources) |source| {
        allocator.free(source.ref);
        allocator.free(source.hash);
    }
    allocator.free(result.sources);
    allocator.free(result.manifest_root);
}

pub fn run(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: []const u8,
    sources: []const []const u8,
    slug: ?[]const u8,
    out_override: ?[]const u8,
    body_file: []const u8,
    title_override: ?[]const u8,
) !Result {
    if (!validKind(kind)) return error.InvalidInput;
    if (sources.len == 0) return error.InvalidInput;
    if (body_file.len == 0) return error.NoBodyProvided;

    const resolved = try resolveSources(d, allocator, sources);
    defer deinitResolvedSources(resolved, allocator);

    const out_path = try deriveOutPath(allocator, kind, slug, out_override);
    defer allocator.free(out_path);
    const title = try deriveTitle(allocator, title_override, slug, out_path);
    defer allocator.free(title);

    const body = try std.Io.Dir.cwd().readFileAlloc(fsIo(), body_file, allocator, std.Io.Limit.limited(16 * 1024 * 1024));
    defer allocator.free(body);
    const final = try composeDocument(allocator, .{
        .title = title,
        .kind = kind,
        .regenerated_by = "pl-doc-promote",
        .sources = resolved,
        .body = body,
    });
    defer allocator.free(final);
    try writeFileAtomic(allocator, out_path, final);

    const source_rows = try sourceRowsFromResolved(allocator, resolved);
    errdefer deinitSourceRows(source_rows, allocator);
    const root = try updateManifest(allocator, out_path, source_rows);

    return .{
        .path = try allocator.dupe(u8, out_path),
        .kind = try allocator.dupe(u8, kind),
        .sources = source_rows,
        .manifest_root = root,
    };
}

pub fn resolveSources(d: *db.sqlite.Db, allocator: std.mem.Allocator, refs: []const []const u8) ![]ResolvedSource {
    var out: std.ArrayList(ResolvedSource) = .empty;
    errdefer deinitResolvedSources(out.items, allocator);
    for (refs) |ref| {
        const body = try resolveSourceBody(d, allocator, ref);
        errdefer allocator.free(body);
        const hash = try manifest.hashBytes(allocator, body);
        errdefer allocator.free(hash);
        try out.append(allocator, .{
            .ref = try allocator.dupe(u8, ref),
            .body = body,
            .hash = hash,
        });
    }
    return try out.toOwnedSlice(allocator);
}

pub fn resolveSourceBody(d: *db.sqlite.Db, allocator: std.mem.Allocator, ref: []const u8) ![]const u8 {
    const parsed = try parseRef(ref);
    if (std.mem.eql(u8, parsed.kind, "artifact")) {
        return try queryTextByID(d, allocator, "select coalesce(body, '') from artifacts where id = ?", parsed.id);
    }
    if (std.mem.eql(u8, parsed.kind, "decision")) {
        const body = try queryTextByID(d, allocator, "select body from decisions where id = ?", parsed.id);
        errdefer allocator.free(body);
        const rationale = queryTextByID(d, allocator, "select coalesce(rationale, '') from decisions where id = ?", parsed.id) catch "";
        defer if (rationale.len > 0) allocator.free(rationale);
        if (rationale.len == 0) return body;
        const joined = try std.fmt.allocPrint(allocator, "{s}\n\n## Rationale\n\n{s}", .{ body, rationale });
        allocator.free(body);
        return joined;
    }
    if (std.mem.eql(u8, parsed.kind, "plan")) {
        return try queryTextByID(d, allocator, "select coalesce(summary, '') from plans where id = ?", parsed.id);
    }
    return error.InvalidEntityRef;
}

const ParsedRef = struct { kind: []const u8, id: i64 };

pub fn parseRef(ref: []const u8) !ParsedRef {
    var s = ref;
    if (std.mem.startsWith(u8, s, "entity:")) s = s["entity:".len..];
    const colon = std.mem.indexOfScalar(u8, s, ':') orelse return error.InvalidEntityRef;
    if (colon == 0) return error.InvalidEntityRef;
    const id = std.fmt.parseInt(i64, s[colon + 1 ..], 10) catch return error.InvalidEntityRef;
    return .{ .kind = s[0..colon], .id = id };
}

fn queryTextByID(d: *db.sqlite.Db, allocator: std.mem.Allocator, sql: [:0]const u8, id: i64) ![]const u8 {
    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => error.NotFound,
        .row => try stmt.columnTextAlloc(0, allocator),
    };
}

pub const ComposeArgs = struct {
    title: []const u8,
    kind: []const u8,
    regenerated_by: []const u8,
    sources: []const ResolvedSource,
    body: []const u8,
};

pub fn composeDocument(allocator: std.mem.Allocator, args: ComposeArgs) ![]const u8 {
    var buf: std.Io.Writer.Allocating = .init(allocator);
    errdefer buf.deinit();
    const w = &buf.writer;
    try w.print("---\n", .{});
    try writeYamlString(w, "title", args.title);
    try writeYamlString(w, "doc_kind", args.kind);
    try w.print("template_version: 1\n", .{});
    try writeSourceArray(w, "source_artifacts", args.sources, "artifact");
    try writeSourceArray(w, "source_decisions", args.sources, "decision");
    try writeSourceArray(w, "source_plans", args.sources, "plan");
    try w.print("source_versions:\n", .{});
    for (args.sources) |source| {
        try w.print("  ", .{});
        try writeYamlQuoted(w, source.ref);
        try w.print(": ", .{});
        try writeYamlQuoted(w, source.hash);
        try w.print("\n", .{});
    }
    try w.print("regenerated_at: now\n", .{});
    try writeYamlString(w, "regenerated_by", args.regenerated_by);
    try w.print("---\n", .{});
    if (args.body.len > 0 and args.body[0] != '\n') try w.print("\n", .{});
    try w.writeAll(args.body);
    try w.flush();
    return try buf.toOwnedSlice();
}

fn writeYamlString(w: *std.Io.Writer, key: []const u8, value: []const u8) !void {
    try w.print("{s}: ", .{key});
    try writeYamlQuoted(w, value);
    try w.print("\n", .{});
}

fn writeYamlQuoted(w: *std.Io.Writer, value: []const u8) !void {
    try w.print("\"", .{});
    for (value) |c| switch (c) {
        '"' => try w.print("\\\"", .{}),
        '\\' => try w.print("\\\\", .{}),
        '\n' => try w.print("\\n", .{}),
        else => try w.writeByte(c),
    };
    try w.print("\"", .{});
}

fn writeSourceArray(w: *std.Io.Writer, key: []const u8, sources: []const ResolvedSource, kind: []const u8) !void {
    var count: usize = 0;
    for (sources) |source| {
        const parsed = parseRef(source.ref) catch continue;
        if (std.mem.eql(u8, parsed.kind, kind)) count += 1;
    }
    if (count == 0) return;
    try w.print("{s}:\n", .{key});
    for (sources) |source| {
        const parsed = parseRef(source.ref) catch continue;
        if (!std.mem.eql(u8, parsed.kind, kind)) continue;
        try w.print("  - ", .{});
        try writeYamlQuoted(w, source.ref);
        try w.print("\n", .{});
    }
}

fn deriveOutPath(allocator: std.mem.Allocator, kind: []const u8, slug: ?[]const u8, out_override: ?[]const u8) ![]const u8 {
    if (out_override) |out| if (out.len > 0) return try allocator.dupe(u8, out);
    if (std.mem.eql(u8, kind, "feature")) {
        const s = slug orelse return error.InvalidInput;
        return try std.fs.path.join(allocator, &.{ "docs", "features", try suffixMd(allocator, s) });
    }
    if (std.mem.eql(u8, kind, "research")) {
        const s = slug orelse return error.InvalidInput;
        return try std.fs.path.join(allocator, &.{ "docs", "research", try suffixMd(allocator, s) });
    }
    if (std.mem.eql(u8, kind, "getting_started")) return try allocator.dupe(u8, "docs/getting-started.md");
    if (std.mem.eql(u8, kind, "adr_index")) return try allocator.dupe(u8, "docs/adrs.md");
    if (std.mem.eql(u8, kind, "changelog")) return try allocator.dupe(u8, "docs/changelog.md");
    if (std.mem.eql(u8, kind, "glossary")) return try allocator.dupe(u8, "docs/glossary.md");
    return error.InvalidInput;
}

fn suffixMd(allocator: std.mem.Allocator, slug: []const u8) ![]const u8 {
    if (std.mem.endsWith(u8, slug, ".md")) return try allocator.dupe(u8, slug);
    return try std.fmt.allocPrint(allocator, "{s}.md", .{slug});
}

fn deriveTitle(allocator: std.mem.Allocator, explicit: ?[]const u8, slug: ?[]const u8, path: []const u8) ![]const u8 {
    if (explicit) |title| if (title.len > 0) return try allocator.dupe(u8, title);
    const base = slug orelse std.fs.path.stem(std.fs.path.basename(path));
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var cap_next = true;
    for (base) |c| {
        if (c == '-' or c == '_' or c == ' ') {
            try out.append(allocator, ' ');
            cap_next = true;
            continue;
        }
        try out.append(allocator, if (cap_next) std.ascii.toUpper(c) else c);
        cap_next = false;
    }
    return try out.toOwnedSlice(allocator);
}

fn validKind(kind: []const u8) bool {
    for (doc_kinds) |candidate| if (std.mem.eql(u8, kind, candidate)) return true;
    return false;
}

fn sourceRowsFromResolved(allocator: std.mem.Allocator, sources: []const ResolvedSource) ![]manifest.SourceRow {
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

pub fn updateManifest(allocator: std.mem.Allocator, out_path: []const u8, sources: []const manifest.SourceRow) ![]const u8 {
    var overlays: std.ArrayList(manifest.SourceOverlay) = .empty;
    defer overlays.deinit(allocator);
    const prior = manifest.load(manifest.file_name, allocator) catch |e| switch (e) {
        error.FileNotFound => null,
        else => return e,
    };
    if (prior) |p| {
        defer manifest.deinitManifest(p, allocator);
        for (p.entries) |entry| {
            if (entry.entry.sources.len == 0) continue;
            if (std.mem.eql(u8, entry.path, out_path)) continue;
            try overlays.append(allocator, .{ .path = entry.path, .sources = entry.entry.sources });
        }
    }
    try overlays.append(allocator, .{ .path = out_path, .sources = sources });
    const m = try manifest.buildWithSources(manifest.default_docs_root, allocator, overlays.items);
    defer manifest.deinitManifest(m, allocator);
    const root = try allocator.dupe(u8, m.root);
    try manifest.write(manifest.file_name, m, allocator);
    return root;
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

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

test "parseRef accepts entity prefix" {
    const got = try parseRef("entity:artifact:42");
    try std.testing.expectEqualStrings("artifact", got.kind);
    try std.testing.expectEqual(@as(i64, 42), got.id);
}

test "composeDocument writes provenance fences" {
    const a = std.testing.allocator;
    const sources = [_]ResolvedSource{.{ .ref = "artifact:1", .body = "body", .hash = "abcd" }};
    const doc = try composeDocument(a, .{
        .title = "Feature",
        .kind = "feature",
        .regenerated_by = "test",
        .sources = &sources,
        .body = "Hello\n",
    });
    defer a.free(doc);
    try std.testing.expect(std.mem.startsWith(u8, doc, "---\n"));
    try std.testing.expect(std.mem.indexOf(u8, doc, "source_artifacts:") != null);
}
