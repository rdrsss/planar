//! engine/workbench/render — Entity → Markdown rendering for workbench files.
//!
//! render() produces a complete workbench file from a FrontMatter struct and
//! a body string. The output format is byte-compatible with Go's
//! internal/workbench/parse.go PrependFrontMatter:
//!
//!   ---
//!   <YAML fields in deterministic order>
//!   ---
//!
//!   <body>
//!
//! Round-trip invariant: render(fm, body) → parse(result) must reconstruct
//! the original FrontMatter and body without loss (modulo whitespace
//! trimming the parser applies to the body).
//!
//! The YAML serializer emits fields in the same order as Go's yaml.v3
//! (alphabetical within optional fields; required fields first).
//! Slices are emitted as block sequences ("- item\n").

const std = @import("std");
const parse = @import("parse.zig");

pub const FrontMatter = parse.FrontMatter;
pub const EntityRef = parse.EntityRef;

// =========================================================================
// Public API
// =========================================================================

/// render produces a complete workbench Markdown file from a FrontMatter
/// and a body string. The returned slice is owned by the caller.
///
/// Matches Go's PrependFrontMatter semantics:
///   - Front matter block: "---\n" + YAML + "---\n"
///   - Body appended with a separating "\n" when non-empty.
pub fn render(allocator: std.mem.Allocator, fm: FrontMatter, body: []const u8) ![]u8 {
    var buf: std.ArrayListUnmanaged(u8) = .empty;
    errdefer buf.deinit(allocator);

    // Opening delimiter.
    try buf.appendSlice(allocator, "---\n");

    // Required fields first (match Go's yaml.v3 order: alphabetical within struct tags).
    // Go's yaml.v3 uses struct field order, which in FrontMatter is:
    // entity_kind, entity_id, anchor_plan_id, title, status, priority, scope,
    // artifact_kind, touches, verifies, cites, derives-from.
    try appendStringField(allocator, &buf, "entity_kind", fm.entity_kind);
    try appendIntField(allocator, &buf, "entity_id", fm.entity_id);
    try appendIntField(allocator, &buf, "anchor_plan_id", fm.anchor_plan_id);

    // Optional fields — only emitted when non-zero/non-empty (mirrors omitempty).
    if (fm.title.len > 0) try appendStringField(allocator, &buf, "title", fm.title);
    if (fm.status.len > 0) try appendStringField(allocator, &buf, "status", fm.status);
    if (fm.priority != 0) try appendIntField(allocator, &buf, "priority", fm.priority);
    if (fm.scope.len > 0) try appendStringField(allocator, &buf, "scope", fm.scope);
    if (fm.artifact_kind.len > 0) try appendStringField(allocator, &buf, "artifact_kind", fm.artifact_kind);
    if (fm.touches.len > 0) try appendStringList(allocator, &buf, "touches", fm.touches);
    if (fm.verifies.len > 0) try appendRefList(allocator, &buf, "verifies", fm.verifies);
    if (fm.cites.len > 0) try appendRefList(allocator, &buf, "cites", fm.cites);
    if (fm.derives_from.len > 0) try appendRefList(allocator, &buf, "derives-from", fm.derives_from);

    // Closing delimiter.
    try buf.appendSlice(allocator, "---\n");

    // Body separator + body (matches Go's PrependFrontMatter).
    if (body.len > 0) {
        try buf.append(allocator, '\n');
        try buf.appendSlice(allocator, body);
    }

    return buf.toOwnedSlice(allocator);
}

// =========================================================================
// Internal helpers
// =========================================================================

fn appendStringField(
    allocator: std.mem.Allocator,
    buf: *std.ArrayListUnmanaged(u8),
    key: []const u8,
    value: []const u8,
) !void {
    // Check whether the value needs quoting. For workbench files the values
    // are simple identifiers, titles, or slug strings. We quote when the value
    // contains a colon followed by a space, starts with special YAML characters,
    // or is empty — matching yaml.v3 behaviour for the strings we produce.
    const needs_quote = needsYamlQuote(value);
    if (needs_quote) {
        const line = try std.fmt.allocPrint(allocator, "{s}: '{s}'\n", .{ key, value });
        defer allocator.free(line);
        try buf.appendSlice(allocator, line);
    } else {
        const line = try std.fmt.allocPrint(allocator, "{s}: {s}\n", .{ key, value });
        defer allocator.free(line);
        try buf.appendSlice(allocator, line);
    }
}

fn appendIntField(
    allocator: std.mem.Allocator,
    buf: *std.ArrayListUnmanaged(u8),
    key: []const u8,
    value: i64,
) !void {
    const line = try std.fmt.allocPrint(allocator, "{s}: {d}\n", .{ key, value });
    defer allocator.free(line);
    try buf.appendSlice(allocator, line);
}

fn appendStringList(
    allocator: std.mem.Allocator,
    buf: *std.ArrayListUnmanaged(u8),
    key: []const u8,
    items: []const []const u8,
) !void {
    const header = try std.fmt.allocPrint(allocator, "{s}:\n", .{key});
    defer allocator.free(header);
    try buf.appendSlice(allocator, header);
    for (items) |item| {
        const line = try std.fmt.allocPrint(allocator, "- {s}\n", .{item});
        defer allocator.free(line);
        try buf.appendSlice(allocator, line);
    }
}

fn appendRefList(
    allocator: std.mem.Allocator,
    buf: *std.ArrayListUnmanaged(u8),
    key: []const u8,
    refs: []const EntityRef,
) !void {
    const header = try std.fmt.allocPrint(allocator, "{s}:\n", .{key});
    defer allocator.free(header);
    try buf.appendSlice(allocator, header);
    for (refs) |ref| {
        const line = try std.fmt.allocPrint(allocator, "- {s}:{d}\n", .{ ref.kind, ref.id });
        defer allocator.free(line);
        try buf.appendSlice(allocator, line);
    }
}

/// needsYamlQuote returns true for values that the Go yaml.v3 library would quote.
/// For the subset of strings workbench renders, this covers values starting with
/// special characters or containing ": ".
fn needsYamlQuote(s: []const u8) bool {
    if (s.len == 0) return true;
    // Values starting with yaml indicators.
    switch (s[0]) {
        '{', '}', '[', ']', ',', '#', '&', '*', '!', '|', '>', '\'', '"', '%', '@', '`' => return true,
        '-' => if (s.len > 1 and s[1] == ' ') return true,
        else => {},
    }
    // Values containing ": " or trailing colon.
    if (std.mem.indexOf(u8, s, ": ") != null) return true;
    if (s[s.len - 1] == ':') return true;
    return false;
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;
const parseModule = @import("parse.zig");

test "render: required fields only" {
    const fm = FrontMatter{
        .entity_kind = "plan",
        .entity_id = 1,
        .anchor_plan_id = 1,
    };
    const result = try render(testing.allocator, fm, "");
    defer testing.allocator.free(result);

    try testing.expectEqualStrings(
        "---\nentity_kind: plan\nentity_id: 1\nanchor_plan_id: 1\n---\n",
        result,
    );
}

test "render: with body separator" {
    const fm = FrontMatter{
        .entity_kind = "task",
        .entity_id = 5,
        .anchor_plan_id = 2,
        .title = "Do something",
        .status = "todo",
    };
    const body = "# Task 5: Do something\n\n**Status:** todo\n";
    const result = try render(testing.allocator, fm, body);
    defer testing.allocator.free(result);

    // Body separator "\n" must appear between "---\n" and body.
    try testing.expect(std.mem.indexOf(u8, result, "---\n\n#") != null);
    // Frontmatter fields must appear.
    try testing.expect(std.mem.indexOf(u8, result, "title: Do something\n") != null);
    try testing.expect(std.mem.indexOf(u8, result, "status: todo\n") != null);
}

test "render: touches list" {
    var touches_arr = [_][]const u8{ "repo-a", "repo-b" };
    const fm = FrontMatter{
        .entity_kind = "task",
        .entity_id = 7,
        .anchor_plan_id = 3,
        .touches = &touches_arr,
    };
    const result = try render(testing.allocator, fm, "");
    defer testing.allocator.free(result);

    try testing.expect(std.mem.indexOf(u8, result, "touches:\n") != null);
    try testing.expect(std.mem.indexOf(u8, result, "- repo-a\n") != null);
    try testing.expect(std.mem.indexOf(u8, result, "- repo-b\n") != null);
}

test "render: verifies list" {
    var verifies_arr = [_]EntityRef{.{ .kind = "task", .id = 99 }};
    const fm = FrontMatter{
        .entity_kind = "artifact",
        .entity_id = 11,
        .anchor_plan_id = 4,
        .verifies = &verifies_arr,
    };
    const result = try render(testing.allocator, fm, "");
    defer testing.allocator.free(result);

    try testing.expect(std.mem.indexOf(u8, result, "verifies:\n") != null);
    try testing.expect(std.mem.indexOf(u8, result, "- task:99\n") != null);
}

test "render → parse round-trip: all fields" {
    var touches_arr = [_][]const u8{ "repo-x", "repo-y" };
    var verifies_arr = [_]EntityRef{.{ .kind = "task", .id = 10 }};
    var cites_arr = [_]EntityRef{.{ .kind = "artifact", .id = 3 }};
    var derives_arr = [_]EntityRef{.{ .kind = "artifact", .id = 5 }};

    const fm = FrontMatter{
        .entity_kind = "artifact",
        .entity_id = 42,
        .anchor_plan_id = 7,
        .title = "Tech Spec: Auth",
        .status = "active",
        .priority = 50,
        .scope = "assoc:acme",
        .artifact_kind = "tech_spec",
        .touches = &touches_arr,
        .verifies = &verifies_arr,
        .cites = &cites_arr,
        .derives_from = &derives_arr,
    };
    const body = "## Content\n\nSome content here.\n";

    const rendered = try render(testing.allocator, fm, body);
    defer testing.allocator.free(rendered);

    const parsed = try parseModule.parse(testing.allocator, rendered);
    defer parseModule.deinit(parsed, testing.allocator);

    // Round-trip assertions — the load-bearing claim.
    try testing.expectEqualStrings(fm.entity_kind, parsed.frontmatter.entity_kind);
    try testing.expectEqual(fm.entity_id, parsed.frontmatter.entity_id);
    try testing.expectEqual(fm.anchor_plan_id, parsed.frontmatter.anchor_plan_id);
    try testing.expectEqualStrings(fm.title, parsed.frontmatter.title);
    try testing.expectEqualStrings(fm.status, parsed.frontmatter.status);
    try testing.expectEqual(fm.priority, parsed.frontmatter.priority);
    try testing.expectEqualStrings(fm.scope, parsed.frontmatter.scope);
    try testing.expectEqualStrings(fm.artifact_kind, parsed.frontmatter.artifact_kind);
    try testing.expectEqual(fm.touches.len, parsed.frontmatter.touches.len);
    try testing.expectEqualStrings(fm.touches[0], parsed.frontmatter.touches[0]);
    try testing.expectEqualStrings(fm.touches[1], parsed.frontmatter.touches[1]);
    try testing.expectEqual(fm.verifies.len, parsed.frontmatter.verifies.len);
    try testing.expectEqualStrings(fm.verifies[0].kind, parsed.frontmatter.verifies[0].kind);
    try testing.expectEqual(fm.verifies[0].id, parsed.frontmatter.verifies[0].id);
    try testing.expectEqual(fm.cites.len, parsed.frontmatter.cites.len);
    try testing.expectEqualStrings(fm.cites[0].kind, parsed.frontmatter.cites[0].kind);
    try testing.expectEqual(fm.cites[0].id, parsed.frontmatter.cites[0].id);
    try testing.expectEqual(fm.derives_from.len, parsed.frontmatter.derives_from.len);
    try testing.expectEqualStrings(fm.derives_from[0].kind, parsed.frontmatter.derives_from[0].kind);
    try testing.expectEqual(fm.derives_from[0].id, parsed.frontmatter.derives_from[0].id);
    // Body round-trip.
    try testing.expectEqualStrings(body, parsed.body);
}

test "render → parse round-trip: minimal (plan entity)" {
    const fm = FrontMatter{
        .entity_kind = "plan",
        .entity_id = 1,
        .anchor_plan_id = 1,
        .title = "My Plan",
        .status = "active",
    };
    const body = "# Plan 1: My Plan\n\n**Status:** active\n";

    const rendered = try render(testing.allocator, fm, body);
    defer testing.allocator.free(rendered);

    const parsed = try parseModule.parse(testing.allocator, rendered);
    defer parseModule.deinit(parsed, testing.allocator);

    try testing.expectEqualStrings("plan", parsed.frontmatter.entity_kind);
    try testing.expectEqual(@as(i64, 1), parsed.frontmatter.entity_id);
    try testing.expectEqualStrings("My Plan", parsed.frontmatter.title);
    try testing.expectEqualStrings("active", parsed.frontmatter.status);
    try testing.expectEqualStrings(body, parsed.body);
}

test "render: priority field emitted when non-zero" {
    const fm = FrontMatter{
        .entity_kind = "task",
        .entity_id = 3,
        .anchor_plan_id = 1,
        .priority = 100,
    };
    const result = try render(testing.allocator, fm, "");
    defer testing.allocator.free(result);

    try testing.expect(std.mem.indexOf(u8, result, "priority: 100\n") != null);
}

test "render: priority field omitted when zero" {
    const fm = FrontMatter{
        .entity_kind = "task",
        .entity_id = 3,
        .anchor_plan_id = 1,
        .priority = 0,
    };
    const result = try render(testing.allocator, fm, "");
    defer testing.allocator.free(result);

    try testing.expect(std.mem.indexOf(u8, result, "priority") == null);
}

test "render: trailing newline after body" {
    const fm = FrontMatter{
        .entity_kind = "question",
        .entity_id = 2,
        .anchor_plan_id = 1,
    };
    const body = "body line\n";
    const result = try render(testing.allocator, fm, body);
    defer testing.allocator.free(result);

    // Last character must be '\n'.
    try testing.expect(result[result.len - 1] == '\n');
}
