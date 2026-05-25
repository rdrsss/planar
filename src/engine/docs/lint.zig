//! engine/docs/lint - citation/reference validation for published docs.

const std = @import("std");
const db = @import("db");

pub const IssueType = enum {
    undeclared_citation,
    unused_declaration,
    unresolvable_external,
    unresolvable_planar,
    malformed_entry,

    pub fn toText(self: IssueType) []const u8 {
        return @tagName(self);
    }
};

pub const Issue = struct {
    type: IssueType,
    path: []const u8,
    detail: []const u8,
};

pub fn deinitIssue(issue: Issue, allocator: std.mem.Allocator) void {
    allocator.free(issue.path);
    allocator.free(issue.detail);
}

pub fn deinitIssues(issues: []const Issue, allocator: std.mem.Allocator) void {
    for (issues) |issue| deinitIssue(issue, allocator);
    allocator.free(issues);
}

pub const Options = struct {
    no_urls: bool = false,
    refs_only: bool = false,
};

pub const Error = anyerror;

const Reference = struct {
    id: []const u8,
    kind: []const u8 = "",
    url: []const u8 = "",
    entity: []const u8 = "",
};

fn deinitReferences(refs: []const Reference, allocator: std.mem.Allocator) void {
    for (refs) |r| {
        allocator.free(r.id);
        if (r.kind.len > 0) allocator.free(r.kind);
        if (r.url.len > 0) allocator.free(r.url);
        if (r.entity.len > 0) allocator.free(r.entity);
    }
    allocator.free(refs);
}

pub fn validateFile(
    doc_path: []const u8,
    d: ?*db.sqlite.Db,
    allocator: std.mem.Allocator,
    opts: Options,
) Error![]Issue {
    const content = try std.Io.Dir.cwd().readFileAlloc(fsIo(), doc_path, allocator, std.Io.Limit.limited(16 * 1024 * 1024));
    defer allocator.free(content);
    return try validateContent(doc_path, content, d, allocator, opts);
}

pub fn validateContent(
    doc_path: []const u8,
    content: []const u8,
    d: ?*db.sqlite.Db,
    allocator: std.mem.Allocator,
    opts: Options,
) Error![]Issue {
    const refs = parseReferences(content, allocator) catch |e| {
        var issues: std.ArrayList(Issue) = .empty;
        errdefer deinitIssues(issues.items, allocator);
        try appendIssue(&issues, allocator, .malformed_entry, doc_path, @errorName(e));
        return try issues.toOwnedSlice(allocator);
    };
    defer deinitReferences(refs, allocator);

    var cited = try collectCitations(content, allocator);
    defer deinitCitations(&cited, allocator);

    var issues: std.ArrayList(Issue) = .empty;
    errdefer {
        for (issues.items) |issue| deinitIssue(issue, allocator);
        issues.deinit(allocator);
    }

    if (!opts.refs_only) {
        var cit_it = cited.iterator();
        while (cit_it.next()) |entry| {
            if (findReference(refs, entry.key_ptr.*) == null) {
                const detail = try std.fmt.allocPrint(
                    allocator,
                    "body cites [^{s}] but front matter declares no such reference",
                    .{entry.key_ptr.*},
                );
                defer allocator.free(detail);
                try appendIssue(&issues, allocator, .undeclared_citation, doc_path, detail);
            }
        }
    }

    for (refs) |r| {
        if (!opts.refs_only and !cited.contains(r.id)) {
            const detail = try std.fmt.allocPrint(
                allocator,
                "reference \"{s}\" declared in front matter but never cited",
                .{r.id},
            );
            defer allocator.free(detail);
            try appendIssue(&issues, allocator, .unused_declaration, doc_path, detail);
        }

        if (std.mem.eql(u8, r.kind, "external")) {
            if (r.url.len == 0) {
                const detail = try std.fmt.allocPrint(allocator, "external reference \"{s}\" missing required url", .{r.id});
                defer allocator.free(detail);
                try appendIssue(&issues, allocator, .malformed_entry, doc_path, detail);
            } else if (!opts.no_urls and !looksLikeURL(r.url)) {
                const detail = try std.fmt.allocPrint(allocator, "reference \"{s}\": invalid url {s}", .{ r.id, r.url });
                defer allocator.free(detail);
                try appendIssue(&issues, allocator, .unresolvable_external, doc_path, detail);
            }
        } else if (std.mem.eql(u8, r.kind, "planar")) {
            if (r.entity.len == 0) {
                const detail = try std.fmt.allocPrint(allocator, "planar reference \"{s}\" missing required entity", .{r.id});
                defer allocator.free(detail);
                try appendIssue(&issues, allocator, .malformed_entry, doc_path, detail);
            } else if (resolveEntity(d, r.entity) catch false == false) {
                const detail = try std.fmt.allocPrint(allocator, "reference \"{s}\": {s} not found", .{ r.id, r.entity });
                defer allocator.free(detail);
                try appendIssue(&issues, allocator, .unresolvable_planar, doc_path, detail);
            }
        } else if (r.kind.len == 0) {
            const detail = try std.fmt.allocPrint(allocator, "reference \"{s}\" missing required kind (external|planar)", .{r.id});
            defer allocator.free(detail);
            try appendIssue(&issues, allocator, .malformed_entry, doc_path, detail);
        } else {
            const detail = try std.fmt.allocPrint(allocator, "reference \"{s}\" has unknown kind \"{s}\" (allowed: external, planar)", .{ r.id, r.kind });
            defer allocator.free(detail);
            try appendIssue(&issues, allocator, .malformed_entry, doc_path, detail);
        }
    }

    return try issues.toOwnedSlice(allocator);
}

fn appendIssue(
    issues: *std.ArrayList(Issue),
    allocator: std.mem.Allocator,
    issue_type: IssueType,
    path: []const u8,
    detail: []const u8,
) !void {
    try issues.append(allocator, .{
        .type = issue_type,
        .path = try allocator.dupe(u8, path),
        .detail = try allocator.dupe(u8, detail),
    });
}

fn parseReferences(content: []const u8, allocator: std.mem.Allocator) ![]Reference {
    const fm = frontMatter(content) orelse return try allocator.alloc(Reference, 0);
    var refs: std.ArrayList(Reference) = .empty;
    errdefer deinitReferences(refs.items, allocator);

    var in_refs = false;
    var current: ?usize = null;
    var lines = std.mem.splitScalar(u8, fm, '\n');
    while (lines.next()) |raw_line| {
        const line = trimRight(raw_line, " \t\r");
        if (line.len == 0) continue;
        const indent = countIndent(line);
        const trimmed = std.mem.trim(u8, line, " \t");
        if (indent == 0) {
            in_refs = std.mem.eql(u8, trimmed, "references:");
            current = null;
            continue;
        }
        if (!in_refs) continue;
        if (indent == 2 and std.mem.endsWith(u8, trimmed, ":")) {
            const id = trimRight(trimmed[0 .. trimmed.len - 1], " \t");
            try refs.append(allocator, .{ .id = try allocator.dupe(u8, id) });
            current = refs.items.len - 1;
            continue;
        }
        if (indent >= 4 and current != null) {
            const colon = std.mem.indexOfScalar(u8, trimmed, ':') orelse continue;
            const key = std.mem.trim(u8, trimmed[0..colon], " \t");
            const value = unquote(std.mem.trim(u8, trimmed[colon + 1 ..], " \t"));
            const idx = current.?;
            if (std.mem.eql(u8, key, "kind")) refs.items[idx].kind = try allocator.dupe(u8, value) else if (std.mem.eql(u8, key, "url")) refs.items[idx].url = try allocator.dupe(u8, value) else if (std.mem.eql(u8, key, "entity")) refs.items[idx].entity = try allocator.dupe(u8, value);
        }
    }
    return try refs.toOwnedSlice(allocator);
}

fn collectCitations(content: []const u8, allocator: std.mem.Allocator) !std.StringHashMap(void) {
    var out = std.StringHashMap(void).init(allocator);
    errdefer out.deinit();
    const body = stripFrontMatter(content);
    var i: usize = 0;
    while (i + 3 <= body.len) : (i += 1) {
        if (body[i] != '[' or body[i + 1] != '^') continue;
        const start = i + 2;
        var end = start;
        while (end < body.len and isRefChar(body[end])) : (end += 1) {}
        if (end == start or end >= body.len or body[end] != ']') continue;
        if (end + 1 < body.len and body[end + 1] == ':') continue;
        const id = body[start..end];
        if (!out.contains(id)) try out.put(try allocator.dupe(u8, id), {});
        i = end;
    }
    return out;
}

fn deinitCitations(cited: *std.StringHashMap(void), allocator: std.mem.Allocator) void {
    var it = cited.iterator();
    while (it.next()) |entry| allocator.free(entry.key_ptr.*);
    cited.deinit();
}

fn findReference(refs: []const Reference, id: []const u8) ?Reference {
    for (refs) |r| if (std.mem.eql(u8, r.id, id)) return r;
    return null;
}

fn frontMatter(content: []const u8) ?[]const u8 {
    if (!std.mem.startsWith(u8, content, "---\n")) return null;
    const rest = content[4..];
    const idx = std.mem.indexOf(u8, rest, "\n---\n") orelse return null;
    return rest[0..idx];
}

fn stripFrontMatter(content: []const u8) []const u8 {
    if (!std.mem.startsWith(u8, content, "---\n")) return content;
    const rest = content[4..];
    const idx = std.mem.indexOf(u8, rest, "\n---\n") orelse return content;
    return rest[idx + 5 ..];
}

fn countIndent(s: []const u8) usize {
    var n: usize = 0;
    while (n < s.len and s[n] == ' ') : (n += 1) {}
    return n;
}

fn unquote(s: []const u8) []const u8 {
    if (s.len >= 2 and ((s[0] == '"' and s[s.len - 1] == '"') or (s[0] == '\'' and s[s.len - 1] == '\''))) {
        return s[1 .. s.len - 1];
    }
    return s;
}

fn trimRight(s: []const u8, values: []const u8) []const u8 {
    var end = s.len;
    while (end > 0 and std.mem.indexOfScalar(u8, values, s[end - 1]) != null) : (end -= 1) {}
    return s[0..end];
}

fn isRefChar(c: u8) bool {
    return std.ascii.isAlphanumeric(c) or c == '-' or c == '_';
}

fn looksLikeURL(s: []const u8) bool {
    return std.mem.startsWith(u8, s, "http://") or std.mem.startsWith(u8, s, "https://");
}

fn resolveEntity(d: ?*db.sqlite.Db, ref: []const u8) !bool {
    const live = d orelse return false;
    var s = ref;
    if (std.mem.startsWith(u8, s, "entity:")) s = s["entity:".len..];
    const colon = std.mem.indexOfScalar(u8, s, ':') orelse return Error.InvalidEntityRef;
    const kind = s[0..colon];
    const id = std.fmt.parseInt(i64, s[colon + 1 ..], 10) catch return Error.InvalidEntityRef;
    const table: [:0]const u8 = if (std.mem.eql(u8, kind, "plan"))
        "plans"
    else if (std.mem.eql(u8, kind, "task"))
        "tasks"
    else if (std.mem.eql(u8, kind, "question"))
        "questions"
    else if (std.mem.eql(u8, kind, "test_scenario"))
        "test_scenarios"
    else if (std.mem.eql(u8, kind, "artifact"))
        "artifacts"
    else if (std.mem.eql(u8, kind, "decision"))
        "decisions"
    else if (std.mem.eql(u8, kind, "session"))
        "sessions"
    else
        return Error.InvalidEntityRef;

    const sql = try std.fmt.allocPrint(std.heap.page_allocator, "select count(*) from {s} where id = {d}", .{ table, id });
    defer std.heap.page_allocator.free(sql);
    const sql_z = try std.heap.page_allocator.dupeZ(u8, sql);
    defer std.heap.page_allocator.free(sql_z);
    return (live.intQuery(sql_z) catch return Error.QueryFailed) > 0;
}

test "validateContent reports undeclared citation" {
    const a = std.testing.allocator;
    const issues = try validateContent("docs/x.md", "body [^missing]\n", null, a, .{ .no_urls = true });
    defer deinitIssues(issues, a);
    try std.testing.expectEqual(@as(usize, 1), issues.len);
    try std.testing.expectEqual(IssueType.undeclared_citation, issues[0].type);
}

test "validateContent accepts declared external citation offline" {
    const a = std.testing.allocator;
    const content =
        "---\n" ++
        "references:\n" ++
        "  spec:\n" ++
        "    kind: external\n" ++
        "    url: https://example.com\n" ++
        "---\n" ++
        "body [^spec]\n";
    const issues = try validateContent("docs/x.md", content, null, a, .{ .no_urls = true });
    defer deinitIssues(issues, a);
    try std.testing.expectEqual(@as(usize, 0), issues.len);
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}
