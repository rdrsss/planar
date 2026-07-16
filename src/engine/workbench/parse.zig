//! engine/workbench/parse — Front-matter parser for workbench Markdown files.
//!
//! Workbench files begin with a YAML front-matter block delimited by `---`
//! lines. The parser extracts the YAML block, populates FrontMatter, and
//! returns the body text (everything after the closing `---` delimiter).
//!
//! FrontMatter mirrors Go's `internal/workbench.FrontMatter` struct exactly,
//! including the EntityRef type used for the Verifies / Cites / DerivesFrom
//! cross-reference lists. Field names follow the YAML key contract documented
//! in the Go parse.go source.
//!
//! Required fields: entity_kind, entity_id (must be positive integer), title,
//! and status. Artifacts additionally require artifact_kind. Status and
//! artifact-kind values are checked against the database-backed entity schema.
//!
//! The YAML parser implemented here is deliberately minimal: it handles the
//! flat key:value pairs and inline list items that the workbench renderer
//! produces. It is NOT a general-purpose YAML parser.

const std = @import("std");

// =========================================================================
// Types
// =========================================================================

/// EntityRef is a cross-reference of the form "<kind>:<id>".
/// Mirrors Go's workbench.EntityRef.
pub const EntityRef = struct {
    kind: []const u8,
    id: i64,
};

/// FrontMatter holds the parsed YAML front matter from a workbench file.
/// Mirrors Go's workbench.FrontMatter exactly.
/// All slice/string fields are owned by the allocator that was passed to parse().
pub const FrontMatter = struct {
    // Required identity fields.
    entity_kind: []const u8 = "",
    entity_id: i64 = 0,
    anchor_plan_id: i64 = 0,

    // Optional kind-specific fields.
    title: []const u8 = "",
    status: []const u8 = "",
    priority: i64 = 0,
    scope: []const u8 = "",
    artifact_kind: []const u8 = "",

    // Slice fields — owned by allocator.
    touches: []const []const u8 = &.{},
    verifies: []const EntityRef = &.{},
    cites: []const EntityRef = &.{},
    derives_from: []const EntityRef = &.{},
};

/// ParseResult holds the parsed FrontMatter and the body text
/// (content after the closing --- delimiter).
/// Both are owned by the allocator passed to parse().
pub const ParseResult = struct {
    frontmatter: FrontMatter,
    body: []const u8,
};

pub const Error = error{
    MalformedFrontmatter,
    MissingRequiredField,
    InvalidEntityKind,
    InvalidFieldValue,
    OutOfMemory,
};

/// DiagnosticReason identifies the precise parser rejection so callers can
/// render actionable messages without re-parsing the frontmatter themselves.
pub const DiagnosticReason = enum {
    missing_open_delimiter,
    missing_close_delimiter,
    tab_indentation,
    unquoted_colon,
    leading_dash_scalar,
    malformed_yaml,
    invalid_integer,
    invalid_entity_ref,
    missing_required_field,
    invalid_entity_kind,
    invalid_field_value,
};

/// Diagnostic describes the first schema or syntax error in a workbench file.
/// String fields borrow from `content` or static storage and require no deinit.
pub const Diagnostic = struct {
    err: Error,
    reason: DiagnosticReason,
    line: usize,
    field: []const u8 = "",
    expected: []const u8 = "",
};

// =========================================================================
// Public API
// =========================================================================

/// parse reads a workbench file's front matter and body.
/// The caller owns all memory in the returned ParseResult.
/// Returns error.MalformedFrontmatter when the file does not start with "---"
/// or the closing "---" is absent. Returns error.MissingRequiredField when
/// a common or per-kind required field is absent, and InvalidFieldValue when
/// a kind-specific status or artifact kind is outside the shared schema.
pub fn parse(allocator: std.mem.Allocator, content: []const u8) Error!ParseResult {
    if (diagnose(content)) |diagnostic| return diagnostic.err;

    // File must start with "---\n".
    if (!std.mem.startsWith(u8, content, "---\n")) {
        return Error.MalformedFrontmatter;
    }

    // Find the closing "---" delimiter.
    const after_open = content[4..]; // skip opening "---\n"
    const yaml_block, const body = findClose(after_open) orelse return Error.MalformedFrontmatter;

    var fm = FrontMatter{};
    // On any error after this point we must free the partially-built FrontMatter.
    errdefer deinitFrontmatter(fm, allocator);

    try parseYaml(allocator, yaml_block, &fm);

    // Validate required fields.
    if (fm.entity_kind.len == 0) return Error.MissingRequiredField;
    if (fm.entity_id <= 0) return Error.MissingRequiredField;

    // Validate entity_kind values.
    const valid_kinds = [_][]const u8{
        "plan", "task", "artifact", "scenario", "decision", "question",
    };
    var kind_ok = false;
    for (valid_kinds) |k| {
        if (std.mem.eql(u8, fm.entity_kind, k)) {
            kind_ok = true;
            break;
        }
    }
    if (!kind_ok) return Error.InvalidEntityKind;

    const body_owned = try allocator.dupe(u8, body);

    return ParseResult{
        .frontmatter = fm,
        .body = body_owned,
    };
}

/// Validate workbench YAML syntax and the per-entity frontmatter schema.
/// The same check gates `parse`, so lint and runtime sync accept identical files.
pub fn diagnose(content: []const u8) ?Diagnostic {
    if (!std.mem.startsWith(u8, content, "---\n")) {
        return .{
            .err = error.MalformedFrontmatter,
            .reason = .missing_open_delimiter,
            .line = 1,
        };
    }

    const after_open = content[4..];
    const yaml, _ = findClose(after_open) orelse return .{
        .err = error.MalformedFrontmatter,
        .reason = .missing_close_delimiter,
        .line = countLines(content),
    };

    var entity_kind: []const u8 = "";
    var status: []const u8 = "";
    var artifact_kind: []const u8 = "";
    var entity_id: i64 = 0;
    var saw_entity_kind = false;
    var saw_entity_id = false;
    var saw_title = false;
    var saw_status = false;
    var saw_artifact_kind = false;
    var entity_kind_line: usize = 2;
    var status_line: usize = 6;
    var artifact_kind_line: usize = 7;

    const ListField = enum { none, touches, verifies, cites, derives_from };
    var current_list: ListField = .none;
    var lines = std.mem.splitScalar(u8, yaml, '\n');
    var line_number: usize = 2;
    while (lines.next()) |raw_line| : (line_number += 1) {
        var indent_end: usize = 0;
        while (indent_end < raw_line.len and (raw_line[indent_end] == ' ' or raw_line[indent_end] == '\t')) : (indent_end += 1) {
            if (raw_line[indent_end] == '\t') {
                return .{
                    .err = error.MalformedFrontmatter,
                    .reason = .tab_indentation,
                    .line = line_number,
                };
            }
        }
        const line = std.mem.trimEnd(u8, raw_line, " \t\r");
        if (line.len == 0) continue;

        if (std.mem.startsWith(u8, line, "- ")) {
            if (current_list == .none) {
                return .{
                    .err = error.MalformedFrontmatter,
                    .reason = .leading_dash_scalar,
                    .line = line_number,
                };
            }
            if (current_list != .touches) {
                const item = std.mem.trim(u8, line[2..], " \t");
                if (!validEntityRef(item)) {
                    return .{
                        .err = error.MalformedFrontmatter,
                        .reason = .invalid_entity_ref,
                        .line = line_number,
                    };
                }
            }
            continue;
        }

        const colon_pos = std.mem.indexOfScalar(u8, line, ':') orelse return .{
            .err = error.MalformedFrontmatter,
            .reason = .malformed_yaml,
            .line = line_number,
        };
        const key = std.mem.trim(u8, line[0..colon_pos], " \t");
        if (key.len == 0) return .{
            .err = error.MalformedFrontmatter,
            .reason = .malformed_yaml,
            .line = line_number,
        };
        const raw_value = std.mem.trim(u8, line[colon_pos + 1 ..], " \t");
        if (raw_value.len > 0 and (raw_value[0] == '\'' or raw_value[0] == '"')) {
            if (raw_value.len < 2 or raw_value[raw_value.len - 1] != raw_value[0]) {
                return .{
                    .err = error.MalformedFrontmatter,
                    .reason = .malformed_yaml,
                    .line = line_number,
                };
            }
        } else {
            if (std.mem.indexOf(u8, raw_value, ": ") != null) {
                return .{
                    .err = error.MalformedFrontmatter,
                    .reason = .unquoted_colon,
                    .line = line_number,
                };
            }
            if (std.mem.startsWith(u8, raw_value, "- ")) {
                return .{
                    .err = error.MalformedFrontmatter,
                    .reason = .leading_dash_scalar,
                    .line = line_number,
                };
            }
        }
        const value = stripYamlQuotes(raw_value);
        current_list = .none;

        if (std.mem.eql(u8, key, "entity_kind")) {
            entity_kind = value;
            saw_entity_kind = value.len > 0;
            entity_kind_line = line_number;
        } else if (std.mem.eql(u8, key, "entity_id")) {
            entity_id = std.fmt.parseInt(i64, value, 10) catch return .{
                .err = error.MalformedFrontmatter,
                .reason = .invalid_integer,
                .line = line_number,
                .field = "entity_id",
            };
            saw_entity_id = true;
        } else if (std.mem.eql(u8, key, "anchor_plan_id") or
            std.mem.eql(u8, key, "priority"))
        {
            _ = std.fmt.parseInt(i64, value, 10) catch return .{
                .err = error.MalformedFrontmatter,
                .reason = .invalid_integer,
                .line = line_number,
                .field = key,
            };
        } else if (std.mem.eql(u8, key, "title")) {
            saw_title = value.len > 0;
        } else if (std.mem.eql(u8, key, "status")) {
            status = value;
            saw_status = value.len > 0;
            status_line = line_number;
        } else if (std.mem.eql(u8, key, "artifact_kind")) {
            artifact_kind = value;
            saw_artifact_kind = value.len > 0;
            artifact_kind_line = line_number;
        } else if (std.mem.eql(u8, key, "touches")) {
            current_list = .touches;
        } else if (std.mem.eql(u8, key, "verifies")) {
            current_list = .verifies;
        } else if (std.mem.eql(u8, key, "cites")) {
            current_list = .cites;
        } else if (std.mem.eql(u8, key, "derives-from")) {
            current_list = .derives_from;
        }
    }

    if (!saw_entity_kind) return missingField("entity_kind", 2);
    if (!saw_entity_id or entity_id <= 0) return missingField("entity_id", 3);
    if (!isEntityKind(entity_kind)) return .{
        .err = error.InvalidEntityKind,
        .reason = .invalid_entity_kind,
        .line = entity_kind_line,
        .field = "entity_kind",
        .expected = "plan, task, artifact, scenario, decision, or question",
    };
    if (!saw_title) return missingField("title", 5);
    if (!saw_status) return missingField("status", 6);
    const expected_status = statusesForKind(entity_kind);
    if (!valueInList(status, expected_status)) return .{
        .err = error.InvalidFieldValue,
        .reason = .invalid_field_value,
        .line = status_line,
        .field = "status",
        .expected = expected_status,
    };
    if (std.mem.eql(u8, entity_kind, "artifact")) {
        if (!saw_artifact_kind) return missingField("artifact_kind", 7);
        const artifact_kinds = "tech_spec, adr, design_note, summary, readme, generated, other, product_spec, roadmap, research, getting_started, changelog_entry, glossary_term, or test_spec";
        if (!valueInList(artifact_kind, artifact_kinds)) return .{
            .err = error.InvalidFieldValue,
            .reason = .invalid_field_value,
            .line = artifact_kind_line,
            .field = "artifact_kind",
            .expected = artifact_kinds,
        };
    }
    return null;
}

/// deinit releases all memory owned by a ParseResult.
pub fn deinit(result: ParseResult, allocator: std.mem.Allocator) void {
    deinitFrontmatter(result.frontmatter, allocator);
    allocator.free(result.body);
}

/// deinitFrontmatter releases all allocator-owned strings in a FrontMatter.
pub fn deinitFrontmatter(fm: FrontMatter, allocator: std.mem.Allocator) void {
    if (fm.entity_kind.len > 0) allocator.free(fm.entity_kind);
    if (fm.title.len > 0) allocator.free(fm.title);
    if (fm.status.len > 0) allocator.free(fm.status);
    if (fm.scope.len > 0) allocator.free(fm.scope);
    if (fm.artifact_kind.len > 0) allocator.free(fm.artifact_kind);

    for (fm.touches) |s| allocator.free(s);
    if (fm.touches.len > 0) allocator.free(fm.touches);

    for (fm.verifies) |r| allocator.free(r.kind);
    if (fm.verifies.len > 0) allocator.free(fm.verifies);

    for (fm.cites) |r| allocator.free(r.kind);
    if (fm.cites.len > 0) allocator.free(fm.cites);

    for (fm.derives_from) |r| allocator.free(r.kind);
    if (fm.derives_from.len > 0) allocator.free(fm.derives_from);
}

// =========================================================================
// Internal helpers
// =========================================================================

fn missingField(field: []const u8, line: usize) Diagnostic {
    return .{
        .err = error.MissingRequiredField,
        .reason = .missing_required_field,
        .line = line,
        .field = field,
        .expected = "a non-empty value",
    };
}

fn countLines(content: []const u8) usize {
    if (content.len == 0) return 1;
    return std.mem.count(u8, content, "\n") + @as(usize, @intFromBool(content[content.len - 1] != '\n'));
}

fn isEntityKind(kind: []const u8) bool {
    const valid = [_][]const u8{ "plan", "task", "artifact", "scenario", "decision", "question" };
    for (valid) |candidate| {
        if (std.mem.eql(u8, kind, candidate)) return true;
    }
    return false;
}

fn statusesForKind(kind: []const u8) []const u8 {
    if (std.mem.eql(u8, kind, "plan")) return "draft, active, paused, done, or abandoned";
    if (std.mem.eql(u8, kind, "task")) return "todo, doing, blocked, done, or cancelled";
    if (std.mem.eql(u8, kind, "artifact")) return "draft, active, superseded, or retired";
    if (std.mem.eql(u8, kind, "scenario")) return "draft, ready, verified, failing, or retired";
    if (std.mem.eql(u8, kind, "decision")) return "proposed, accepted, superseded, or withdrawn";
    return "open, answered, or wontfix";
}

fn valueInList(value: []const u8, list: []const u8) bool {
    var parts = std.mem.splitScalar(u8, list, ',');
    while (parts.next()) |raw| {
        var candidate = std.mem.trim(u8, raw, " ");
        if (std.mem.startsWith(u8, candidate, "or ")) candidate = candidate[3..];
        if (std.mem.eql(u8, value, candidate)) return true;
    }
    return false;
}

fn validEntityRef(value: []const u8) bool {
    const colon = std.mem.indexOfScalar(u8, value, ':') orelse return false;
    const kind = std.mem.trim(u8, value[0..colon], " \t");
    const id_text = std.mem.trim(u8, value[colon + 1 ..], " \t");
    if (kind.len == 0 or id_text.len == 0) return false;
    const id = std.fmt.parseInt(i64, id_text, 10) catch return false;
    return id > 0;
}

/// findClose returns the yaml_block and the body, or null if no closing --- is found.
fn findClose(rest: []const u8) ?struct { []const u8, []const u8 } {
    // Look for "\n---\n" — the delimiter is on its own line.
    if (std.mem.indexOf(u8, rest, "\n---\n")) |end| {
        const yaml_block = rest[0..end];
        // Body starts after the "\n---\n" (5 bytes), trimming a leading newline.
        var body = rest[end + 5 ..];
        if (body.len > 0 and body[0] == '\n') body = body[1..];
        return .{ yaml_block, body };
    }
    // Try at EOF: "\n---" with nothing after.
    if (std.mem.endsWith(u8, rest, "\n---")) {
        const end = rest.len - 4;
        return .{ rest[0..end], "" };
    }
    return null;
}

/// parseYaml parses a minimal YAML block (flat key:value and simple lists)
/// into *FrontMatter. Values are allocated with the given allocator.
fn parseYaml(allocator: std.mem.Allocator, yaml: []const u8, fm: *FrontMatter) Error!void {
    var lines = std.mem.splitScalar(u8, yaml, '\n');

    // State for collecting multi-line list values.
    const ListField = enum { none, touches, verifies, cites, derives_from };
    var current_list: ListField = .none;
    var touches_buf: std.ArrayListUnmanaged([]const u8) = .empty;
    defer {
        if (current_list != .none) {
            for (touches_buf.items) |s| allocator.free(s);
            touches_buf.deinit(allocator);
        }
    }
    var verifies_buf: std.ArrayListUnmanaged(EntityRef) = .empty;
    defer {
        for (verifies_buf.items) |r| allocator.free(r.kind);
        verifies_buf.deinit(allocator);
    }
    var cites_buf: std.ArrayListUnmanaged(EntityRef) = .empty;
    defer {
        for (cites_buf.items) |r| allocator.free(r.kind);
        cites_buf.deinit(allocator);
    }
    var derives_buf: std.ArrayListUnmanaged(EntityRef) = .empty;
    defer {
        for (derives_buf.items) |r| allocator.free(r.kind);
        derives_buf.deinit(allocator);
    }

    while (lines.next()) |raw_line| {
        const line = std.mem.trimEnd(u8, raw_line, " \t\r");
        if (line.len == 0) continue;

        // List item (starts with "- ").
        if (std.mem.startsWith(u8, line, "- ")) {
            const item = std.mem.trim(u8, line[2..], " \t");
            switch (current_list) {
                .touches => {
                    const s = try allocator.dupe(u8, item);
                    try touches_buf.append(allocator, s);
                },
                .verifies, .cites, .derives_from => {
                    const ref = try parseEntityRef(allocator, item);
                    switch (current_list) {
                        .verifies => try verifies_buf.append(allocator, ref),
                        .cites => try cites_buf.append(allocator, ref),
                        .derives_from => try derives_buf.append(allocator, ref),
                        else => unreachable,
                    }
                },
                .none => {}, // top-level list item without a known key — ignore
            }
            continue;
        }

        // Key: value pair.
        const colon_pos = std.mem.indexOfScalar(u8, line, ':') orelse continue;
        const key = std.mem.trim(u8, line[0..colon_pos], " \t");
        const val_raw = line[colon_pos + 1 ..];
        const val_trimmed = std.mem.trim(u8, val_raw, " \t");
        // Strip surrounding single or double quotes (yaml.v3 emits single-quoted strings
        // for values containing ": " etc.; we need to round-trip cleanly).
        const val = stripYamlQuotes(val_trimmed);

        // Reset list state when we encounter a new key.
        current_list = .none;

        if (std.mem.eql(u8, key, "entity_kind")) {
            fm.entity_kind = try allocator.dupe(u8, val);
        } else if (std.mem.eql(u8, key, "entity_id")) {
            fm.entity_id = parseInt(val) catch return Error.MalformedFrontmatter;
        } else if (std.mem.eql(u8, key, "anchor_plan_id")) {
            fm.anchor_plan_id = parseInt(val) catch return Error.MalformedFrontmatter;
        } else if (std.mem.eql(u8, key, "title")) {
            fm.title = try allocator.dupe(u8, val);
        } else if (std.mem.eql(u8, key, "status")) {
            fm.status = try allocator.dupe(u8, val);
        } else if (std.mem.eql(u8, key, "priority")) {
            fm.priority = parseInt(val) catch return Error.MalformedFrontmatter;
        } else if (std.mem.eql(u8, key, "scope")) {
            fm.scope = try allocator.dupe(u8, val);
        } else if (std.mem.eql(u8, key, "artifact_kind")) {
            fm.artifact_kind = try allocator.dupe(u8, val);
        } else if (std.mem.eql(u8, key, "touches")) {
            // "touches:" introduces a list on subsequent lines.
            current_list = .touches;
        } else if (std.mem.eql(u8, key, "verifies")) {
            current_list = .verifies;
        } else if (std.mem.eql(u8, key, "cites")) {
            current_list = .cites;
        } else if (std.mem.eql(u8, key, "derives-from")) {
            current_list = .derives_from;
        }
        // Unknown keys are silently ignored (unknown-field handling mirrors Go).
    }

    // Commit collected list fields.
    if (touches_buf.items.len > 0) {
        fm.touches = try touches_buf.toOwnedSlice(allocator);
        touches_buf = .empty; // prevent double-free via defer
    }
    if (verifies_buf.items.len > 0) {
        fm.verifies = try verifies_buf.toOwnedSlice(allocator);
        verifies_buf = .empty;
    }
    if (cites_buf.items.len > 0) {
        fm.cites = try cites_buf.toOwnedSlice(allocator);
        cites_buf = .empty;
    }
    if (derives_buf.items.len > 0) {
        fm.derives_from = try derives_buf.toOwnedSlice(allocator);
        derives_buf = .empty;
    }
}

/// parseEntityRef parses "<kind>:<id>" into an EntityRef.
fn parseEntityRef(allocator: std.mem.Allocator, s: []const u8) Error!EntityRef {
    const colon = std.mem.indexOfScalar(u8, s, ':') orelse return Error.MalformedFrontmatter;
    const kind_str = std.mem.trim(u8, s[0..colon], " \t");
    const id_str = std.mem.trim(u8, s[colon + 1 ..], " \t");
    if (kind_str.len == 0 or id_str.len == 0) return Error.MalformedFrontmatter;
    const id = parseInt(id_str) catch return Error.MalformedFrontmatter;
    if (id <= 0) return Error.MalformedFrontmatter;
    const kind_owned = try allocator.dupe(u8, kind_str);
    return EntityRef{ .kind = kind_owned, .id = id };
}

/// stripYamlQuotes removes surrounding single or double quotes from a YAML
/// scalar value if present. 'foo' → foo, "foo" → foo, foo → foo.
fn stripYamlQuotes(s: []const u8) []const u8 {
    if (s.len >= 2) {
        if ((s[0] == '\'' and s[s.len - 1] == '\'') or
            (s[0] == '"' and s[s.len - 1] == '"'))
        {
            return s[1 .. s.len - 1];
        }
    }
    return s;
}

fn parseInt(s: []const u8) !i64 {
    return std.fmt.parseInt(i64, s, 10);
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

test "parse: happy path — required + optional fields" {
    const content =
        \\---
        \\entity_kind: task
        \\entity_id: 42
        \\anchor_plan_id: 10
        \\title: My Task
        \\status: doing
        \\priority: 50
        \\---
        \\
        \\# Body content
        \\
    ;
    const result = try parse(testing.allocator, content);
    defer deinit(result, testing.allocator);

    try testing.expectEqualStrings("task", result.frontmatter.entity_kind);
    try testing.expectEqual(@as(i64, 42), result.frontmatter.entity_id);
    try testing.expectEqual(@as(i64, 10), result.frontmatter.anchor_plan_id);
    try testing.expectEqualStrings("My Task", result.frontmatter.title);
    try testing.expectEqualStrings("doing", result.frontmatter.status);
    try testing.expectEqual(@as(i64, 50), result.frontmatter.priority);
    try testing.expectEqualStrings("# Body content\n", result.body);
}

test "parse: missing entity_kind → MissingRequiredField" {
    const content =
        \\---
        \\entity_id: 1
        \\anchor_plan_id: 1
        \\---
        \\
    ;
    const result = parse(testing.allocator, content);
    try testing.expectError(Error.MissingRequiredField, result);
}

test "parse: missing entity_id → MissingRequiredField" {
    const content =
        \\---
        \\entity_kind: plan
        \\anchor_plan_id: 1
        \\---
        \\
    ;
    const result = parse(testing.allocator, content);
    try testing.expectError(Error.MissingRequiredField, result);
}

test "parse: zero entity_id → MissingRequiredField" {
    const content =
        \\---
        \\entity_kind: plan
        \\entity_id: 0
        \\anchor_plan_id: 1
        \\---
        \\
    ;
    const result = parse(testing.allocator, content);
    try testing.expectError(Error.MissingRequiredField, result);
}

test "parse: no opening --- → MalformedFrontmatter" {
    const content = "entity_kind: plan\nentity_id: 1\n";
    const result = parse(testing.allocator, content);
    try testing.expectError(Error.MalformedFrontmatter, result);
}

test "parse: no closing --- → MalformedFrontmatter" {
    const content =
        \\---
        \\entity_kind: plan
        \\entity_id: 1
        \\anchor_plan_id: 1
        \\
    ;
    const result = parse(testing.allocator, content);
    try testing.expectError(Error.MalformedFrontmatter, result);
}

test "parse: invalid entity_kind → InvalidEntityKind" {
    const content =
        \\---
        \\entity_kind: widget
        \\entity_id: 1
        \\anchor_plan_id: 1
        \\---
        \\
    ;
    const result = parse(testing.allocator, content);
    try testing.expectError(Error.InvalidEntityKind, result);
}

test "parse: empty body is OK" {
    const content =
        \\---
        \\entity_kind: decision
        \\entity_id: 7
        \\anchor_plan_id: 3
        \\title: Empty decision
        \\status: proposed
        \\---
        \\
    ;
    const result = try parse(testing.allocator, content);
    defer deinit(result, testing.allocator);
    try testing.expectEqual(@as(usize, 0), result.body.len);
}

test "parse: touches list" {
    const content =
        \\---
        \\entity_kind: task
        \\entity_id: 5
        \\anchor_plan_id: 2
        \\title: Touched task
        \\status: todo
        \\touches:
        \\- repo-a
        \\- repo-b
        \\---
        \\
    ;
    const result = try parse(testing.allocator, content);
    defer deinit(result, testing.allocator);
    try testing.expectEqual(@as(usize, 2), result.frontmatter.touches.len);
    try testing.expectEqualStrings("repo-a", result.frontmatter.touches[0]);
    try testing.expectEqualStrings("repo-b", result.frontmatter.touches[1]);
}

test "parse: verifies list with EntityRef" {
    const content =
        \\---
        \\entity_kind: artifact
        \\entity_id: 11
        \\anchor_plan_id: 4
        \\title: Verification artifact
        \\status: active
        \\artifact_kind: test_spec
        \\verifies:
        \\- task:99
        \\- artifact:12
        \\---
        \\
    ;
    const result = try parse(testing.allocator, content);
    defer deinit(result, testing.allocator);
    try testing.expectEqual(@as(usize, 2), result.frontmatter.verifies.len);
    try testing.expectEqualStrings("task", result.frontmatter.verifies[0].kind);
    try testing.expectEqual(@as(i64, 99), result.frontmatter.verifies[0].id);
    try testing.expectEqualStrings("artifact", result.frontmatter.verifies[1].kind);
    try testing.expectEqual(@as(i64, 12), result.frontmatter.verifies[1].id);
}

test "parse: UTF-8 body content" {
    const content =
        \\---
        \\entity_kind: question
        \\entity_id: 3
        \\anchor_plan_id: 1
        \\title: Héllo Wörld
        \\status: open
        \\---
        \\
        \\Ünïcödé body 🎉
        \\
    ;
    const result = try parse(testing.allocator, content);
    defer deinit(result, testing.allocator);
    try testing.expectEqualStrings("Héllo Wörld", result.frontmatter.title);
    try testing.expectEqualStrings("Ünïcödé body 🎉\n", result.body);
}

test "parse: derives-from list" {
    const content =
        \\---
        \\entity_kind: artifact
        \\entity_id: 20
        \\anchor_plan_id: 5
        \\title: Derived artifact
        \\status: active
        \\artifact_kind: tech_spec
        \\derives-from:
        \\- artifact:10
        \\---
        \\
    ;
    const result = try parse(testing.allocator, content);
    defer deinit(result, testing.allocator);
    try testing.expectEqual(@as(usize, 1), result.frontmatter.derives_from.len);
    try testing.expectEqualStrings("artifact", result.frontmatter.derives_from[0].kind);
    try testing.expectEqual(@as(i64, 10), result.frontmatter.derives_from[0].id);
}
