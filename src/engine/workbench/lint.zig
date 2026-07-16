//! Workbench frontmatter validation for files and directory trees.

const std = @import("std");
const db = @import("db");
const parse = @import("parse.zig");

pub const Severity = enum { @"error", warning };

pub const Issue = struct {
    path: []const u8,
    line: usize,
    severity: Severity,
    code: []const u8,
    message: []const u8,
    hint: []const u8,
};

pub const Result = struct {
    files_scanned: usize,
    errors: usize,
    warnings: usize,
    issues: []const Issue,
};

/// Validate every Markdown file at `target`, which may name one file or a directory.
pub fn run(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    io: std.Io,
    target: []const u8,
) !Result {
    const stat = try std.Io.Dir.cwd().statFile(io, target, .{});
    var files: std.ArrayList([]const u8) = .empty;
    defer {
        for (files.items) |path| allocator.free(path);
        files.deinit(allocator);
    }
    switch (stat.kind) {
        .file => {
            if (!std.mem.endsWith(u8, target, ".md")) return error.InvalidInput;
            try files.append(allocator, try allocator.dupe(u8, target));
        },
        .directory => try collectMarkdown(allocator, io, target, &files),
        else => return error.InvalidInput,
    }
    std.mem.sort([]const u8, files.items, {}, struct {
        fn lessThan(_: void, lhs: []const u8, rhs: []const u8) bool {
            return std.mem.lessThan(u8, lhs, rhs);
        }
    }.lessThan);

    var issues: std.ArrayList(Issue) = .empty;
    errdefer {
        deinitIssueStrings(allocator, issues.items);
        issues.deinit(allocator);
    }
    var errors: usize = 0;
    var warnings: usize = 0;

    for (files.items) |path| {
        const content = try std.Io.Dir.cwd().readFileAlloc(io, path, allocator, .limited(16 * 1024 * 1024));
        defer allocator.free(content);
        const parsed = parse.parse(allocator, content) catch |err| {
            try appendParseIssue(allocator, &issues, path, content, err);
            errors += 1;
            continue;
        };
        defer parse.deinit(parsed, allocator);

        if (parsed.frontmatter.anchor_plan_id <= 0 or
            !try planExists(d, parsed.frontmatter.anchor_plan_id))
        {
            const message = if (parsed.frontmatter.anchor_plan_id <= 0)
                try allocator.dupe(u8, "front matter field 'anchor_plan_id' must reference an existing plan")
            else
                try std.fmt.allocPrint(allocator, "anchor_plan_id {d} does not reference an existing plan", .{parsed.frontmatter.anchor_plan_id});
            errdefer allocator.free(message);
            try issues.append(allocator, .{
                .path = try allocator.dupe(u8, path),
                .line = lineForKey(content, "anchor_plan_id"),
                .severity = .warning,
                .code = try allocator.dupe(u8, "anchor_plan_not_found"),
                .message = message,
                .hint = try allocator.dupe(u8, "set anchor_plan_id to the owning top-level plan ID"),
            });
            warnings += 1;
        }
    }

    return .{
        .files_scanned = files.items.len,
        .errors = errors,
        .warnings = warnings,
        .issues = try issues.toOwnedSlice(allocator),
    };
}

/// Release a lint result and all diagnostic strings.
pub fn deinitResult(allocator: std.mem.Allocator, result: Result) void {
    deinitIssues(allocator, result.issues);
}

fn collectMarkdown(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir_path: []const u8,
    files: *std.ArrayList([]const u8),
) !void {
    var dir = try std.Io.Dir.cwd().openDir(io, dir_path, .{ .iterate = true });
    defer dir.close(io);
    var iterator = dir.iterate();
    while (try iterator.next(io)) |entry| {
        if (entry.name.len > 0 and entry.name[0] == '.') continue;
        const child = try std.fs.path.join(allocator, &.{ dir_path, entry.name });
        defer allocator.free(child);
        switch (entry.kind) {
            .directory => try collectMarkdown(allocator, io, child, files),
            .file => if (std.mem.endsWith(u8, entry.name, ".md"))
                try files.append(allocator, try allocator.dupe(u8, child)),
            else => {},
        }
    }
}

fn planExists(d: *db.sqlite.Db, plan_id: i64) !bool {
    var stmt = d.prepare("select 1 from plans where id = ? limit 1") catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return error.QueryFailed;
    return switch (stmt.step() catch return error.QueryFailed) {
        .done => false,
        .row => true,
    };
}

fn appendParseIssue(
    allocator: std.mem.Allocator,
    issues: *std.ArrayList(Issue),
    path: []const u8,
    content: []const u8,
    err: parse.Error,
) !void {
    const code, const message, const hint, const line = switch (err) {
        error.MalformedFrontmatter => .{
            "malformed_frontmatter",
            "front matter is malformed",
            malformedHint(content),
            @as(usize, 1),
        },
        error.MissingRequiredField => .{
            "missing_required_field",
            "front matter is missing required identity field 'entity_kind' or 'entity_id'",
            "add entity_kind and a positive integer entity_id",
            missingIdentityLine(content),
        },
        error.InvalidEntityKind => .{
            "invalid_entity_kind",
            "front matter field 'entity_kind' is not supported",
            "use plan, task, artifact, scenario, decision, or question",
            lineForKey(content, "entity_kind"),
        },
        error.OutOfMemory => return error.OutOfMemory,
    };
    try issues.append(allocator, .{
        .path = try allocator.dupe(u8, path),
        .line = line,
        .severity = .@"error",
        .code = try allocator.dupe(u8, code),
        .message = try allocator.dupe(u8, message),
        .hint = try allocator.dupe(u8, hint),
    });
}

fn malformedHint(content: []const u8) []const u8 {
    if (!std.mem.startsWith(u8, content, "---\n")) return "start the file with a '---' front matter delimiter";
    if (std.mem.indexOf(u8, content[4..], "\n---") == null) return "add a closing '---' delimiter on its own line";
    if (std.mem.indexOfScalar(u8, content, '\t') != null) return "replace tab indentation with spaces";
    return "check integer fields and entity-reference lists in the front matter block";
}

fn missingIdentityLine(content: []const u8) usize {
    if (lineForKey(content, "entity_kind") == 1) return lineForKey(content, "entity_id");
    return lineForKey(content, "entity_kind");
}

fn lineForKey(content: []const u8, key: []const u8) usize {
    var lines = std.mem.splitScalar(u8, content, '\n');
    var line_number: usize = 1;
    while (lines.next()) |line| : (line_number += 1) {
        const trimmed = std.mem.trimStart(u8, line, " \t");
        if (std.mem.startsWith(u8, trimmed, key) and
            trimmed.len > key.len and trimmed[key.len] == ':') return line_number;
    }
    return 1;
}

fn deinitIssues(allocator: std.mem.Allocator, issues: []const Issue) void {
    deinitIssueStrings(allocator, issues);
    if (issues.len > 0) allocator.free(issues);
}

fn deinitIssueStrings(allocator: std.mem.Allocator, issues: []const Issue) void {
    for (issues) |issue| {
        allocator.free(issue.path);
        allocator.free(issue.code);
        allocator.free(issue.message);
        allocator.free(issue.hint);
    }
}

test "parse diagnostics distinguish malformed missing and invalid kind" {
    var issues: std.ArrayList(Issue) = .empty;
    defer {
        deinitIssueStrings(std.testing.allocator, issues.items);
        issues.deinit(std.testing.allocator);
    }

    try appendParseIssue(std.testing.allocator, &issues, "missing.md", "body\n", error.MalformedFrontmatter);
    try appendParseIssue(std.testing.allocator, &issues, "identity.md", "---\nentity_kind: task\n---\n", error.MissingRequiredField);
    try appendParseIssue(std.testing.allocator, &issues, "kind.md", "---\nentity_kind: widget\nentity_id: 1\n---\n", error.InvalidEntityKind);

    try std.testing.expectEqualStrings("malformed_frontmatter", issues.items[0].code);
    try std.testing.expectEqualStrings("missing_required_field", issues.items[1].code);
    try std.testing.expectEqualStrings("invalid_entity_kind", issues.items[2].code);
}

test "malformed hints cover delimiters tabs and scalar failures" {
    try std.testing.expectEqualStrings("start the file with a '---' front matter delimiter", malformedHint("body"));
    try std.testing.expectEqualStrings("add a closing '---' delimiter on its own line", malformedHint("---\nentity_id: 1\n"));
    try std.testing.expectEqualStrings("replace tab indentation with spaces", malformedHint("---\nentity_id:\tbad\n---\n"));
    try std.testing.expectEqualStrings("check integer fields and entity-reference lists in the front matter block", malformedHint("---\nentity_id: bad\n---\n"));
}
