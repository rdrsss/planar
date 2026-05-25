//! engine/config/parse.zig — Minimal TOML parser for Planar's config schema.
//!
//! Supports the subset of TOML actually used by defaults.toml and valid
//! user config files:
//!
//!   Supported:
//!     - Comments starting with `#` (rest-of-line and full-line)
//!     - Top-level table headers [name] and dotted [a.b.c]
//!     - Table headers with quoted keys [external."github-issues"]
//!     - Key-value pairs:  key = value
//!     - String values: double-quoted ("...") and single-quoted ('...')
//!     - Integer values: bare decimal integers (optional leading -)
//!     - Boolean values: bare `true` or `false`
//!     - Inline arrays of strings: ["a", "b", "c"]  (used by parent_field_names)
//!
//!   Not supported (ParseFailed or UnsupportedFeature if encountered):
//!     - Inline tables { a = 1 }
//!     - Multi-line strings """..."""
//!     - Datetimes
//!     - Float values
//!     - Numeric underscores 1_000
//!     - Array-of-tables [[ name ]]
//!
//! Parse errors carry line+column and a message.
//!
//! D-engine-pattern: pure functions, no DB, no goroutines, no interfaces.

const std = @import("std");

// =========================================================================
// Public types
// =========================================================================

/// A parsed TOML value. String content is owned by the caller's allocator
/// (via the map returned by parse). Array content is a slice of owned strings.
pub const Value = union(enum) {
    string: []const u8,
    int: i64,
    bool: bool,
    array: [][]const u8,

    /// Free any heap memory this value holds.
    pub fn deinit(self: Value, allocator: std.mem.Allocator) void {
        switch (self) {
            .string => |s| allocator.free(s),
            .array => |arr| {
                for (arr) |s| allocator.free(s);
                allocator.free(arr);
            },
            .int, .bool => {},
        }
    }
};

/// Describes a parse error with file location.
/// `message` points into `msg_buf` — no heap allocation needed.
pub const ParseError = struct {
    line: u32,
    column: u32,
    message: []const u8,
    msg_buf: [256]u8 = undefined,
};

pub const Error = error{ ParseFailed, UnsupportedFeature, OutOfMemory };

// =========================================================================
// Public API
// =========================================================================

/// Parse TOML content into a flat map keyed by dotted-path strings.
///
/// For example, the section [external.jira.status] with key todo = "To Do"
/// produces the entry "external.jira.status.todo" → Value{ .string = "To Do" }.
///
/// The returned map and all its keys/values are owned by `allocator`.
/// Free the map with `deinitMap`.
///
/// On error, `out_err` is populated with location + message information.
pub fn parse(
    allocator: std.mem.Allocator,
    content: []const u8,
    out_err: *ParseError,
) Error!std.StringHashMapUnmanaged(Value) {
    var map: std.StringHashMapUnmanaged(Value) = .{};
    errdefer deinitMap(&map, allocator);

    var p = Parser{
        .allocator = allocator,
        .input = content,
        .pos = 0,
        .line = 1,
        .col = 1,
        .out_err = out_err,
        .current_section = "",
        .section_owned = false,
    };

    try p.run(&map);
    return map;
}

/// Release all memory owned by a map returned from parse().
pub fn deinitMap(map: *std.StringHashMapUnmanaged(Value), allocator: std.mem.Allocator) void {
    var it = map.iterator();
    while (it.next()) |entry| {
        allocator.free(entry.key_ptr.*);
        entry.value_ptr.deinit(allocator);
    }
    map.deinit(allocator);
}

// =========================================================================
// Parser internals
// =========================================================================

const Parser = struct {
    allocator: std.mem.Allocator,
    input: []const u8,
    pos: usize,
    line: u32,
    col: u32,
    out_err: *ParseError,
    /// Current section prefix (e.g. "external.jira.status"). Empty at top level.
    /// Len > 0 means it was allocated by dupe and must be freed.
    current_section: []const u8,
    /// Whether current_section is an allocated string that must be freed.
    section_owned: bool,

    fn fail(p: *Parser, comptime fmt: []const u8, args: anytype) Error {
        p.out_err.line = p.line;
        p.out_err.column = p.col;
        const msg = std.fmt.bufPrint(&p.out_err.msg_buf, fmt, args) catch "parse error";
        p.out_err.message = msg;
        return error.ParseFailed;
    }

    fn failAt(p: *Parser, l: u32, c: u32, comptime fmt: []const u8, args: anytype) Error {
        p.out_err.line = l;
        p.out_err.column = c;
        const msg = std.fmt.bufPrint(&p.out_err.msg_buf, fmt, args) catch "parse error";
        p.out_err.message = msg;
        return error.ParseFailed;
    }

    fn peek(p: *const Parser) ?u8 {
        if (p.pos >= p.input.len) return null;
        return p.input[p.pos];
    }

    fn advance(p: *Parser) void {
        if (p.pos >= p.input.len) return;
        if (p.input[p.pos] == '\n') {
            p.line += 1;
            p.col = 1;
        } else {
            p.col += 1;
        }
        p.pos += 1;
    }

    fn consume(p: *Parser) ?u8 {
        const ch = p.peek() orelse return null;
        p.advance();
        return ch;
    }

    /// Skip spaces and tabs (but not newlines).
    fn skipInlineWhitespace(p: *Parser) void {
        while (p.peek()) |ch| {
            if (ch == ' ' or ch == '\t') {
                p.advance();
            } else break;
        }
    }

    /// Skip to end of line (does not consume the newline).
    fn skipToEOL(p: *Parser) void {
        while (p.peek()) |ch| {
            if (ch == '\n') break;
            p.advance();
        }
    }

    /// Skip the current newline character (handles \r\n and \n).
    fn skipNewline(p: *Parser) void {
        const ch = p.peek() orelse return;
        if (ch == '\r') p.advance();
        if (p.peek() == @as(?u8, '\n')) p.advance();
    }

    fn run(p: *Parser, map: *std.StringHashMapUnmanaged(Value)) Error!void {
        defer if (p.section_owned) {
            p.allocator.free(p.current_section);
            p.section_owned = false;
        };

        while (true) {
            p.skipInlineWhitespace();
            const ch = p.peek() orelse break;

            switch (ch) {
                '\n', '\r' => p.skipNewline(),
                '#' => p.skipToEOL(),
                '[' => {
                    try p.parseTableHeader(map);
                },
                else => {
                    try p.parseKeyValue(map);
                },
            }
        }
    }

    fn parseTableHeader(p: *Parser, map: *std.StringHashMapUnmanaged(Value)) Error!void {
        _ = map;
        const open_line = p.line;
        const open_col = p.col;
        _ = p.consume(); // consume '['

        // Check for array-of-tables [[
        if (p.peek() == @as(?u8, '[')) {
            return p.failAt(open_line, open_col, "array-of-tables [[...]] is not supported", .{});
        }

        p.skipInlineWhitespace();

        // Parse the section key (possibly dotted, possibly quoted segments).
        var section_buf: std.ArrayList(u8) = .empty;
        defer section_buf.deinit(p.allocator);

        var first = true;
        while (true) {
            p.skipInlineWhitespace();
            const kch = p.peek() orelse return p.fail("unterminated table header", .{});

            if (kch == ']') {
                p.advance();
                break;
            }

            if (!first) {
                // Expect a '.' separator between key segments.
                if (kch != '.') {
                    return p.fail("expected '.' or ']' in table header, got '{c}'", .{kch});
                }
                p.advance(); // consume '.'
                try section_buf.append(p.allocator, '.');
                p.skipInlineWhitespace();
            }
            first = false;

            const seg = try p.parseKeySegment();
            defer p.allocator.free(seg);
            try section_buf.appendSlice(p.allocator, seg);
        }

        if (section_buf.items.len == 0) {
            return p.fail("empty table header", .{});
        }

        // Store the new section prefix (owned by allocator).
        if (p.section_owned) {
            p.allocator.free(p.current_section);
        }
        p.current_section = try p.allocator.dupe(u8, section_buf.items);
        p.section_owned = true;

        // Skip rest of line.
        p.skipInlineWhitespace();
        if (p.peek()) |c| {
            if (c == '#') p.skipToEOL();
            if (p.peek()) |nc| {
                if (nc != '\n' and nc != '\r') {
                    return p.fail("unexpected content after table header", .{});
                }
            }
        }
    }

    /// Parse a single key segment (bare or quoted).
    fn parseKeySegment(p: *Parser) Error![]u8 {
        const ch = p.peek() orelse return p.fail("expected key segment", .{});
        if (ch == '"') {
            return p.parseDoubleQuotedString();
        } else if (ch == '\'') {
            return p.parseSingleQuotedString();
        } else {
            return p.parseBareKey();
        }
    }

    /// Parse a bare key: alphanumerics, '-', '_'.
    fn parseBareKey(p: *Parser) Error![]u8 {
        const start = p.pos;
        const start_line = p.line;
        const start_col = p.col;
        while (p.peek()) |ch| {
            if (std.ascii.isAlphanumeric(ch) or ch == '-' or ch == '_') {
                p.advance();
            } else break;
        }
        if (p.pos == start) {
            return p.failAt(start_line, start_col, "expected key character", .{});
        }
        return p.allocator.dupe(u8, p.input[start..p.pos]);
    }

    fn parseKeyValue(p: *Parser, map: *std.StringHashMapUnmanaged(Value)) Error!void {
        const key_line = p.line;
        const key_col = p.col;
        _ = key_line;
        _ = key_col;

        // Parse the key (bare or dotted with optional quoted segments).
        var key_buf: std.ArrayList(u8) = .empty;
        defer key_buf.deinit(p.allocator);

        var first_seg = true;
        while (true) {
            p.skipInlineWhitespace();
            if (!first_seg) {
                if (p.peek() != @as(?u8, '.')) break;
                p.advance(); // consume '.'
                try key_buf.append(p.allocator, '.');
            }
            first_seg = false;

            const seg = try p.parseKeySegment();
            defer p.allocator.free(seg);
            try key_buf.appendSlice(p.allocator, seg);

            p.skipInlineWhitespace();
            // Check if there's a '.' next (continue dotted key) or '=' (end of key).
            if (p.peek() == @as(?u8, '.')) continue;
            break;
        }

        p.skipInlineWhitespace();
        const eq = p.consume() orelse return p.fail("expected '=' after key", .{});
        if (eq != '=') return p.fail("expected '=', got '{c}'", .{eq});
        p.skipInlineWhitespace();

        // Build dotted path: section + key.
        var full_key: []u8 = undefined;
        if (p.current_section.len > 0) {
            full_key = try std.fmt.allocPrint(p.allocator, "{s}.{s}", .{ p.current_section, key_buf.items });
        } else {
            full_key = try p.allocator.dupe(u8, key_buf.items);
        }
        errdefer p.allocator.free(full_key);

        const val = try p.parseValue();
        errdefer val.deinit(p.allocator);

        // Skip inline comment and whitespace after value.
        p.skipInlineWhitespace();
        if (p.peek() == @as(?u8, '#')) p.skipToEOL();

        // Insert (later entries overwrite earlier ones per TOML spec).
        const result = try map.getOrPut(p.allocator, full_key);
        if (result.found_existing) {
            // Free old key and value.
            p.allocator.free(result.key_ptr.*);
            result.value_ptr.deinit(p.allocator);
        }
        result.key_ptr.* = full_key;
        result.value_ptr.* = val;
    }

    fn parseValue(p: *Parser) Error!Value {
        const ch = p.peek() orelse return p.fail("expected value", .{});

        return switch (ch) {
            '"' => Value{ .string = try p.parseDoubleQuotedString() },
            '\'' => Value{ .string = try p.parseSingleQuotedString() },
            '[' => Value{ .array = try p.parseStringArray() },
            '{' => p.fail("inline tables are not supported", .{}),
            't', 'f' => try p.parseBool(),
            '-', '0'...'9' => try p.parseNumber(),
            else => p.fail("unexpected character '{c}' at start of value", .{ch}),
        };
    }

    fn parseDoubleQuotedString(p: *Parser) Error![]u8 {
        _ = p.consume(); // consume opening '"'

        // Check for triple-quote.
        if (p.peek() == @as(?u8, '"')) {
            p.advance();
            if (p.peek() == @as(?u8, '"')) {
                return p.fail("multi-line strings (\"\"\"...\"\"\") are not supported", .{});
            }
            // Empty string "".
            return p.allocator.dupe(u8, "") catch return error.OutOfMemory;
        }

        var buf: std.ArrayList(u8) = .empty;
        errdefer buf.deinit(p.allocator);

        while (true) {
            const ch = p.consume() orelse return p.fail("unterminated string", .{});
            if (ch == '"') break;
            if (ch == '\n') return p.fail("unterminated string (newline in string)", .{});
            if (ch == '\\') {
                const esc = p.consume() orelse return p.fail("unterminated escape sequence", .{});
                const escaped: u8 = switch (esc) {
                    '"' => '"',
                    '\\' => '\\',
                    'n' => '\n',
                    'r' => '\r',
                    't' => '\t',
                    '0' => 0,
                    else => return p.fail("unsupported escape sequence '\\{c}'", .{esc}),
                };
                try buf.append(p.allocator, escaped);
            } else {
                try buf.append(p.allocator, ch);
            }
        }

        return buf.toOwnedSlice(p.allocator);
    }

    fn parseSingleQuotedString(p: *Parser) Error![]u8 {
        _ = p.consume(); // consume opening '\''

        // Check for triple-quote '''
        if (p.peek() == @as(?u8, '\'')) {
            p.advance();
            if (p.peek() == @as(?u8, '\'')) {
                return p.fail("multi-line literal strings ('''...''') are not supported", .{});
            }
            // Empty string ''.
            return p.allocator.dupe(u8, "") catch return error.OutOfMemory;
        }

        var buf: std.ArrayList(u8) = .empty;
        errdefer buf.deinit(p.allocator);

        while (true) {
            const ch = p.consume() orelse return p.fail("unterminated string", .{});
            if (ch == '\'') break;
            if (ch == '\n') return p.fail("unterminated string (newline in string)", .{});
            // Literal string: no escape processing.
            try buf.append(p.allocator, ch);
        }

        return buf.toOwnedSlice(p.allocator);
    }

    fn parseBool(p: *Parser) Error!Value {
        // Try "true" or "false" (bare keywords).
        if (p.pos + 4 <= p.input.len and std.mem.eql(u8, p.input[p.pos .. p.pos + 4], "true")) {
            // Verify next char is not identifier-like.
            const after = if (p.pos + 4 < p.input.len) p.input[p.pos + 4] else 0;
            if (!std.ascii.isAlphanumeric(after) and after != '_') {
                p.pos += 4;
                p.col += 4;
                return Value{ .bool = true };
            }
        }
        if (p.pos + 5 <= p.input.len and std.mem.eql(u8, p.input[p.pos .. p.pos + 5], "false")) {
            const after = if (p.pos + 5 < p.input.len) p.input[p.pos + 5] else 0;
            if (!std.ascii.isAlphanumeric(after) and after != '_') {
                p.pos += 5;
                p.col += 5;
                return Value{ .bool = false };
            }
        }
        return p.fail("invalid bare value (expected true, false, or a quoted string)", .{});
    }

    fn parseNumber(p: *Parser) Error!Value {
        const start = p.pos;
        if (p.peek() == @as(?u8, '-')) p.advance();
        while (p.peek()) |ch| {
            if (ch >= '0' and ch <= '9') {
                p.advance();
            } else break;
        }
        // Check for float (dot after digits).
        if (p.peek() == @as(?u8, '.')) {
            return p.fail("float values are not supported", .{});
        }
        const raw = p.input[start..p.pos];
        const n = std.fmt.parseInt(i64, raw, 10) catch
            return p.fail("invalid integer '{s}'", .{raw});
        return Value{ .int = n };
    }

    /// Parse an array of strings: ["a", "b", ...] or ['a', 'b', ...]
    /// Mixed types or non-string elements return UnsupportedFeature.
    fn parseStringArray(p: *Parser) Error![][]const u8 {
        _ = p.consume(); // consume '['

        var items: std.ArrayList([]const u8) = .empty;
        errdefer {
            for (items.items) |s| p.allocator.free(s);
            items.deinit(p.allocator);
        }

        p.skipInlineWhitespace();

        while (true) {
            // Skip whitespace and newlines between elements.
            while (p.peek()) |ch| {
                if (ch == ' ' or ch == '\t' or ch == '\n' or ch == '\r') {
                    p.advance();
                } else if (ch == '#') {
                    p.skipToEOL();
                } else break;
            }

            const ch = p.peek() orelse return p.fail("unterminated array", .{});
            if (ch == ']') {
                p.advance();
                break;
            }

            const s = switch (ch) {
                '"' => try p.parseDoubleQuotedString(),
                '\'' => try p.parseSingleQuotedString(),
                else => return p.fail("only string arrays are supported; got '{c}'", .{ch}),
            };
            errdefer p.allocator.free(s);
            try items.append(p.allocator, s);

            // Skip whitespace then expect ',' or ']'.
            while (p.peek()) |wch| {
                if (wch == ' ' or wch == '\t') p.advance() else break;
            }
            const next = p.peek() orelse return p.fail("unterminated array", .{});
            if (next == ',') {
                p.advance();
            } else if (next != ']') {
                return p.fail("expected ',' or ']' in array, got '{c}'", .{next});
            }
        }

        return items.toOwnedSlice(p.allocator);
    }
};

// =========================================================================
// Tests
// =========================================================================

test "parse: empty file returns empty map" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    var map = try parse(a, "", &pe);
    defer deinitMap(&map, a);
    try std.testing.expectEqual(@as(usize, 0), map.count());
}

test "parse: file with only comments returns empty map" {
    const a = std.testing.allocator;
    const content =
        \\# This is a comment
        \\# Another comment
        \\   # Indented comment
    ;
    var pe: ParseError = undefined;
    var map = try parse(a, content, &pe);
    defer deinitMap(&map, a);
    try std.testing.expectEqual(@as(usize, 0), map.count());
}

test "parse: simple top-level string key" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    var map = try parse(a, "vendor = \"claude\"\n", &pe);
    defer deinitMap(&map, a);
    const val = map.get("vendor") orelse return error.TestFailed;
    try std.testing.expectEqualStrings("claude", val.string);
}

test "parse: single-quoted string" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    var map = try parse(a, "scope = 'global'\n", &pe);
    defer deinitMap(&map, a);
    const val = map.get("scope") orelse return error.TestFailed;
    try std.testing.expectEqualStrings("global", val.string);
}

test "parse: bool true and false" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    var map = try parse(a, "enabled = true\ndisabled = false\n", &pe);
    defer deinitMap(&map, a);
    try std.testing.expectEqual(true, map.get("enabled").?.bool);
    try std.testing.expectEqual(false, map.get("disabled").?.bool);
}

test "parse: integer value" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    var map = try parse(a, "count = 42\n", &pe);
    defer deinitMap(&map, a);
    try std.testing.expectEqual(@as(i64, 42), map.get("count").?.int);
}

test "parse: inline comment on value line" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    var map = try parse(a, "vendor = \"claude\"  # default vendor\n", &pe);
    defer deinitMap(&map, a);
    const val = map.get("vendor") orelse return error.TestFailed;
    try std.testing.expectEqualStrings("claude", val.string);
}

test "parse: section header creates dotted keys" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    const content =
        \\[defaults]
        \\vendor = "claude"
        \\scope = "global"
    ;
    var map = try parse(a, content, &pe);
    defer deinitMap(&map, a);
    try std.testing.expectEqualStrings("claude", map.get("defaults.vendor").?.string);
    try std.testing.expectEqualStrings("global", map.get("defaults.scope").?.string);
}

test "parse: dotted table header [external.jira.status]" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    const content =
        \\[external.jira.status]
        \\todo = "To Do"
        \\doing = "In Progress"
    ;
    var map = try parse(a, content, &pe);
    defer deinitMap(&map, a);
    try std.testing.expectEqualStrings("To Do", map.get("external.jira.status.todo").?.string);
    try std.testing.expectEqualStrings("In Progress", map.get("external.jira.status.doing").?.string);
}

test "parse: quoted table header key [external.\"github-issues\"]" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    const content =
        \\[external."github-issues"]
        \\auth = "gh-cli"
    ;
    var map = try parse(a, content, &pe);
    defer deinitMap(&map, a);
    try std.testing.expectEqualStrings("gh-cli", map.get("external.github-issues.auth").?.string);
}

test "parse: full defaults.toml — check key count and spot values" {
    const a = std.testing.allocator;
    const defaults_content = @embedFile("defaults.toml");
    var pe: ParseError = undefined;
    var map = try parse(a, defaults_content, &pe);
    defer deinitMap(&map, a);

    // Should have non-zero entries.
    try std.testing.expect(map.count() > 0);

    // Spot-check known values.
    try std.testing.expectEqualStrings("claude", map.get("defaults.vendor").?.string);
    try std.testing.expectEqualStrings("global", map.get("defaults.scope").?.string);
    try std.testing.expectEqualStrings("~/.planar/workbench", map.get("workbench.root").?.string);
    try std.testing.expectEqualStrings("~/.planar/templates", map.get("templates.dir").?.string);
    try std.testing.expectEqualStrings("default", map.get("templates.default_set").?.string);
    try std.testing.expectEqualStrings("JIRA_USER", map.get("external.jira.user_env").?.string);
    try std.testing.expectEqualStrings("JIRA_TOKEN", map.get("external.jira.token_env").?.string);
    try std.testing.expectEqualStrings("To Do", map.get("external.jira.status.todo").?.string);
    try std.testing.expectEqualStrings("In Progress", map.get("external.jira.status.doing").?.string);
    try std.testing.expectEqualStrings("Blocked", map.get("external.jira.status.blocked").?.string);
    try std.testing.expectEqualStrings("Done", map.get("external.jira.status.done").?.string);
    try std.testing.expectEqualStrings("gh-cli", map.get("external.github-issues.auth").?.string);
    try std.testing.expectEqualStrings("GITHUB_TOKEN", map.get("external.github-issues.token_env").?.string);
    try std.testing.expectEqualStrings("open", map.get("external.github-issues.status.todo").?.string);
    try std.testing.expectEqualStrings("open", map.get("external.github-issues.status.doing").?.string);
    try std.testing.expectEqualStrings("closed", map.get("external.github-issues.status.done").?.string);
}

test "parse: string array [\"a\", \"b\", \"c\"]" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    const content = "parent_field_names = [\"Parent\", \"Initiative\", \"Tracking\"]\n";
    var map = try parse(a, content, &pe);
    defer deinitMap(&map, a);
    const arr = map.get("parent_field_names").?.array;
    try std.testing.expectEqual(@as(usize, 3), arr.len);
    try std.testing.expectEqualStrings("Parent", arr[0]);
    try std.testing.expectEqualStrings("Initiative", arr[1]);
    try std.testing.expectEqualStrings("Tracking", arr[2]);
}

test "parse error: unclosed double-quote returns ParseFailed with line/col" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    const content = "vendor = \"unclosed\n";
    const result = parse(a, content, &pe);
    try std.testing.expectError(error.ParseFailed, result);
    try std.testing.expect(pe.line > 0);
}

test "parse error: malformed table header [ returns ParseFailed with location" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    const content = "[\n";
    const result = parse(a, content, &pe);
    try std.testing.expectError(error.ParseFailed, result);
    try std.testing.expect(pe.line > 0);
}

test "parse error: inline table {a=1} returns ParseFailed" {
    const a = std.testing.allocator;
    var pe: ParseError = undefined;
    const content = "opts = {a = 1}\n";
    const result = parse(a, content, &pe);
    try std.testing.expectError(error.ParseFailed, result);
}
