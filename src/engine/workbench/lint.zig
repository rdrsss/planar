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
            if (err == error.OutOfMemory) return error.OutOfMemory;
            const diagnostic = parse.diagnose(content) orelse return error.InvalidInput;
            try appendParseIssue(allocator, &issues, path, diagnostic);
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
    diagnostic: parse.Diagnostic,
) !void {
    const code = switch (diagnostic.err) {
        error.MalformedFrontmatter => "malformed_frontmatter",
        error.MissingRequiredField => "missing_required_field",
        error.InvalidEntityKind => "invalid_entity_kind",
        error.InvalidFieldValue => "invalid_field_value",
        error.OutOfMemory => return error.OutOfMemory,
    };
    const message = switch (diagnostic.err) {
        error.MalformedFrontmatter => try malformedMessage(allocator, diagnostic),
        error.MissingRequiredField => try std.fmt.allocPrint(allocator, "front matter is missing required field '{s}'", .{diagnostic.field}),
        error.InvalidEntityKind => try allocator.dupe(u8, "front matter field 'entity_kind' is not supported"),
        error.InvalidFieldValue => try std.fmt.allocPrint(allocator, "front matter field '{s}' has an unsupported value", .{diagnostic.field}),
        error.OutOfMemory => return error.OutOfMemory,
    };
    errdefer allocator.free(message);
    const hint = try diagnosticHint(allocator, diagnostic);
    errdefer allocator.free(hint);
    const path_owned = try allocator.dupe(u8, path);
    errdefer allocator.free(path_owned);
    const code_owned = try allocator.dupe(u8, code);
    errdefer allocator.free(code_owned);
    try issues.append(allocator, .{
        .path = path_owned,
        .line = diagnostic.line,
        .severity = .@"error",
        .code = code_owned,
        .message = message,
        .hint = hint,
    });
}

fn malformedMessage(allocator: std.mem.Allocator, diagnostic: parse.Diagnostic) ![]const u8 {
    const detail = switch (diagnostic.reason) {
        .missing_open_delimiter => "opening delimiter is missing",
        .missing_close_delimiter => "closing delimiter is missing",
        .tab_indentation => "tab indentation is not valid YAML",
        .unquoted_colon => "an unquoted scalar contains ': '",
        .leading_dash_scalar => "an unquoted scalar begins with '- '",
        .invalid_integer => "an integer field is invalid",
        .invalid_entity_ref => "an entity reference is invalid",
        else => "YAML syntax is invalid",
    };
    return std.fmt.allocPrint(allocator, "front matter is malformed: {s}", .{detail});
}

fn diagnosticHint(allocator: std.mem.Allocator, diagnostic: parse.Diagnostic) ![]const u8 {
    return switch (diagnostic.reason) {
        .missing_open_delimiter => allocator.dupe(u8, "start the file with a '---' front matter delimiter"),
        .missing_close_delimiter => allocator.dupe(u8, "add a closing '---' delimiter on its own line"),
        .tab_indentation => allocator.dupe(u8, "replace tab indentation with spaces"),
        .unquoted_colon => allocator.dupe(u8, "quote the scalar with single or double quotes because it contains ': '"),
        .leading_dash_scalar => allocator.dupe(u8, "quote the scalar because values beginning with '- ' are YAML sequence indicators"),
        .invalid_integer => std.fmt.allocPrint(allocator, "set {s} to a base-10 integer", .{diagnostic.field}),
        .invalid_entity_ref => allocator.dupe(u8, "use '<entity-kind>:<positive-id>' for each reference"),
        .missing_required_field => std.fmt.allocPrint(allocator, "add required front matter field '{s}'", .{diagnostic.field}),
        .invalid_entity_kind, .invalid_field_value => std.fmt.allocPrint(allocator, "use one of: {s}", .{diagnostic.expected}),
        .malformed_yaml => allocator.dupe(u8, "check YAML key/value syntax in the front matter block"),
    };
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

test "parse diagnostics distinguish malformed missing invalid kind and invalid value" {
    var issues: std.ArrayList(Issue) = .empty;
    defer {
        deinitIssueStrings(std.testing.allocator, issues.items);
        issues.deinit(std.testing.allocator);
    }

    try appendParseIssue(std.testing.allocator, &issues, "missing.md", parse.diagnose("body\n").?);
    try appendParseIssue(std.testing.allocator, &issues, "identity.md", parse.diagnose("---\nentity_kind: task\n---\n").?);
    try appendParseIssue(std.testing.allocator, &issues, "kind.md", parse.diagnose("---\nentity_kind: widget\nentity_id: 1\ntitle: Widget\nstatus: active\n---\n").?);
    try appendParseIssue(std.testing.allocator, &issues, "status.md", parse.diagnose("---\nentity_kind: task\nentity_id: 1\ntitle: Task\nstatus: bogus\n---\n").?);

    try std.testing.expectEqualStrings("malformed_frontmatter", issues.items[0].code);
    try std.testing.expectEqualStrings("missing_required_field", issues.items[1].code);
    try std.testing.expectEqualStrings("invalid_entity_kind", issues.items[2].code);
    try std.testing.expectEqualStrings("invalid_field_value", issues.items[3].code);
}
