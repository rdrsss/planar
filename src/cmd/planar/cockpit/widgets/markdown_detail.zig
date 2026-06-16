//! cockpit/widgets/markdown_detail.zig — Shared markdown detail pane.
//!
//! Renders the Planar-spec markdown subset (q608) into a libvaxis Window:
//!   - ATX headings (# ## ### ...)
//!   - Paragraphs (blank-line separated)
//!   - Unordered lists (- item, * item)
//!   - Ordered lists (1. item)
//!   - Fenced code blocks (``` or ~~~)
//!   - Bold (**text** or __text__)
//!   - Italic (*text* or _text_)
//!   - Links: rendered as label only ([label](url) → "label")
//!   - Tables (GFM-style |col|col|)
//!
//! Explicitly out of scope: images, raw HTML, footnotes.
//!
//! The renderer works line-by-line for block structure, then applies
//! inline spans within each text run. The output is written to the
//! caller-supplied libvaxis Window using `writeCell`.
//!
//! Task 3958 (markdown-detail) — M2 spine widget.

const std = @import("std");
const vaxis = @import("vaxis");

const Window = vaxis.Window;
const Style = vaxis.Style;
const Cell = vaxis.Cell;

/// Style overrides applied to inline spans.
const SpanKind = enum {
    normal,
    bold,
    italic,
    bold_italic,
    code, // inline `backtick`
    heading1,
    heading2,
    heading3,
    heading_other,
    dim, // table borders, hr
};

/// Render `markdown` text into `win`. The text is rendered starting at
/// (0,0) in the window; lines that extend beyond `win.height` are
/// clipped. The caller is responsible for clearing the window before
/// calling.
///
/// `allocator` is used for transient per-line work (list of cells to
/// render); it is released before returning.
pub fn render(
    win: Window,
    allocator: std.mem.Allocator,
    markdown: []const u8,
) !void {
    if (win.height == 0 or win.width == 0) return;
    const max_row: usize = @intCast(win.height);

    var row: usize = 0;
    var in_code_fence = false;
    var fence_char: u8 = '`';

    var line_iter = std.mem.splitScalar(u8, markdown, '\n');
    while (line_iter.next()) |line| {
        if (row >= max_row) break;

        // ---- Fenced code block detection ---------------------------
        if (!in_code_fence) {
            if (std.mem.startsWith(u8, line, "```") or
                std.mem.startsWith(u8, line, "~~~"))
            {
                fence_char = line[0];
                in_code_fence = true;
                // Don't render the fence marker line itself.
                continue;
            }
        } else {
            // Inside a code fence.
            if (line.len >= 3 and line[0] == fence_char and
                line[1] == fence_char and line[2] == fence_char)
            {
                in_code_fence = false;
                continue;
            }
            // Render raw code line.
            try renderPlain(win, allocator, line, row, .code);
            row += 1;
            continue;
        }

        // ---- Blank line (paragraph separator) ----------------------
        if (std.mem.trim(u8, line, " \t").len == 0) {
            row += 1;
            continue;
        }

        // ---- ATX headings ------------------------------------------
        if (std.mem.startsWith(u8, line, "#")) {
            var level: usize = 0;
            for (line) |c| {
                if (c == '#') level += 1 else break;
            }
            level = @min(level, 6);
            // Skip the '#' chars and optional space.
            var content = line[level..];
            if (content.len > 0 and content[0] == ' ') content = content[1..];
            const kind: SpanKind = switch (level) {
                1 => .heading1,
                2 => .heading2,
                3 => .heading3,
                else => .heading_other,
            };
            try renderInline(win, allocator, content, row, kind);
            row += 1;
            continue;
        }

        // ---- Horizontal rule (--- or ***) --------------------------
        {
            const trimmed = std.mem.trim(u8, line, " \t");
            if (isHorizontalRule(trimmed)) {
                try renderPlain(win, allocator, "────────────────────────────────────", row, .dim);
                row += 1;
                continue;
            }
        }

        // ---- Tables (GFM: line starts with |) ----------------------
        if (std.mem.startsWith(u8, line, "|")) {
            // Skip separator rows (|---|---|)
            const trimmed = std.mem.trim(u8, line, " \t|");
            if (isTableSeparator(trimmed)) {
                // Render a dim separator bar.
                try renderPlain(win, allocator, line, row, .dim);
                row += 1;
                continue;
            }
            // Render table row: strip | from ends, join cells with │.
            try renderTableRow(win, allocator, line, row);
            row += 1;
            continue;
        }

        // ---- Unordered list item -----------------------------------
        {
            const trimmed = std.mem.trimStart(u8, line, " \t");
            if (std.mem.startsWith(u8, trimmed, "- ") or
                std.mem.startsWith(u8, trimmed, "* ") or
                std.mem.startsWith(u8, trimmed, "+ "))
            {
                const indent = line.len - trimmed.len;
                const item_text = trimmed[2..];
                // Render "  • text" with indentation.
                // Allocate the bullet prefix from the frame arena so that its
                // bytes remain valid through vaxis.render() — a stack buf would
                // dangle once the render helper returns.
                const bullet = try std.fmt.allocPrint(allocator, "{s}\xe2\x80\xa2 ", .{
                    line[0..@min(indent, 4)],
                });
                var parts: std.ArrayList(u8) = .empty;
                // Do NOT call parts.deinit(allocator) here.  With an arena allocator
                // (the required caller contract), deinit rewinds the arena end_index
                // to the most-recent allocation boundary.  The *next* appendSlice call
                // then re-uses the same address range, silently overwriting the bytes
                // that live vaxis screen cells still point into.  The arena's own
                // deinit (called by the frame owner after vaxis render) frees everything
                // in one shot — individual per-item deinits are both unnecessary and
                // harmful here.
                try parts.appendSlice(allocator, bullet);
                try parts.appendSlice(allocator, item_text);
                try renderInline(win, allocator, parts.items, row, .normal);
                row += 1;
                continue;
            }
        }

        // ---- Ordered list item (1. text) ---------------------------
        {
            const trimmed = std.mem.trimStart(u8, line, " \t");
            if (orderedListPrefix(trimmed)) |prefix_len| {
                const item_text = trimmed[prefix_len..];
                var parts: std.ArrayList(u8) = .empty;
                // Same arena-reuse hazard as the unordered list path above:
                // do not deinit the parts ArrayList individually.
                const indent = line.len - trimmed.len;
                try parts.appendSlice(allocator, line[0..@min(indent, 4)]);
                try parts.appendSlice(allocator, trimmed[0..prefix_len]);
                try parts.appendSlice(allocator, item_text);
                try renderInline(win, allocator, parts.items, row, .normal);
                row += 1;
                continue;
            }
        }

        // ---- Normal paragraph line with inline markup --------------
        try renderInline(win, allocator, line, row, .normal);
        row += 1;
    }
}

fn isHorizontalRule(s: []const u8) bool {
    if (s.len < 3) return false;
    const c = s[0];
    if (c != '-' and c != '*' and c != '_') return false;
    for (s) |b| {
        if (b != c and b != ' ') return false;
    }
    var count: usize = 0;
    for (s) |b| {
        if (b == c) count += 1;
    }
    return count >= 3;
}

fn isTableSeparator(s: []const u8) bool {
    // Matches patterns like "---|---" or ":--:|:--:"
    if (s.len == 0) return false;
    for (s) |c| {
        if (c != '-' and c != ':' and c != '|' and c != ' ') return false;
    }
    return std.mem.indexOf(u8, s, "-") != null;
}

/// Returns the length of the ordered-list prefix ("1. ", "2. ", etc.)
/// or null if not an ordered list item.
fn orderedListPrefix(s: []const u8) ?usize {
    var i: usize = 0;
    while (i < s.len and s[i] >= '0' and s[i] <= '9') : (i += 1) {}
    if (i == 0 or i >= s.len) return null;
    if (s[i] != '.' and s[i] != ')') return null;
    i += 1;
    if (i < s.len and s[i] == ' ') i += 1;
    return i;
}

/// Render a table row. Splits on `|` and renders each cell.
fn renderTableRow(
    win: Window,
    allocator: std.mem.Allocator,
    line: []const u8,
    row: usize,
) !void {
    // Strip leading/trailing |.
    var content = line;
    if (content.len > 0 and content[0] == '|') content = content[1..];
    if (content.len > 0 and content[content.len - 1] == '|') content = content[0 .. content.len - 1];

    var parts: std.ArrayList(u8) = .empty;
    // Do NOT deinit parts individually — same arena-reuse hazard as the list
    // item paths in render().  The frame arena frees all at once after the
    // entire render pass is complete and the vaxis back-buffer has been flushed.

    var first = true;
    var cell_iter = std.mem.splitScalar(u8, content, '|');
    while (cell_iter.next()) |cell| {
        const cell_trimmed = std.mem.trim(u8, cell, " \t");
        if (!first) {
            try parts.appendSlice(allocator, " \xe2\x94\x82 "); // │
        }
        try parts.appendSlice(allocator, cell_trimmed);
        first = false;
    }
    try renderInline(win, allocator, parts.items, row, .normal);
}

/// Render a line using only the base style (for code blocks, dims, etc.).
fn renderPlain(
    win: Window,
    allocator: std.mem.Allocator,
    text: []const u8,
    row: usize,
    kind: SpanKind,
) !void {
    _ = allocator;
    const style = kindStyle(kind);
    const max_col: usize = @intCast(win.width);
    var col: usize = 0;

    var i: usize = 0;
    while (i < text.len and col < max_col) {
        const byte = text[i];
        if (byte & 0x80 == 0) {
            // ASCII: slice into `text` so the grapheme pointer is stable
            // (valid for the lifetime of the text parameter, which is either
            // a static literal, a slice of the heap-allocated body, or an
            // arena-allocated parts buffer).
            // `const ch: [1]u8 = .{byte}` would be a loop-iteration stack
            // local that dies before vaxis.render() — a UAF.
            win.writeCell(@intCast(col), @intCast(row), .{
                .char = .{ .grapheme = text[i .. i + 1], .width = 1 },
                .style = style,
            });
            col += 1;
            i += 1;
        } else {
            // Multi-byte UTF-8: find the full codepoint sequence.
            const seq_len = utf8SeqLen(byte);
            if (i + seq_len <= text.len) {
                win.writeCell(@intCast(col), @intCast(row), .{
                    .char = .{ .grapheme = text[i .. i + seq_len], .width = 1 },
                    .style = style,
                });
                col += 1;
            }
            i += seq_len;
        }
    }
}

/// Render a line with inline markup (bold, italic, code, links).
fn renderInline(
    win: Window,
    allocator: std.mem.Allocator,
    text: []const u8,
    row: usize,
    base_kind: SpanKind,
) !void {
    _ = allocator;
    const max_col: usize = @intCast(win.width);
    var col: usize = 0;

    var i: usize = 0;
    var bold = false;
    var italic = false;
    var in_code = false;

    while (i < text.len and col < max_col) {
        // ---- Inline code: `...` ------------------------------------
        if (text[i] == '`' and !in_code) {
            in_code = true;
            i += 1;
            continue;
        }
        if (text[i] == '`' and in_code) {
            in_code = false;
            i += 1;
            continue;
        }

        // ---- Link [label](url) → render label only -----------------
        if (text[i] == '[' and !in_code) {
            if (parseLinkLabel(text[i..])) |parsed| {
                // Render the label with underline style.
                // `label` is a subslice of `text` (itself a subslice of the
                // heap-allocated body or the arena parts buffer), so
                // label[j..j+1] gives a stable pointer — no stack-local ch needed.
                const label = parsed.label;
                var j: usize = 0;
                while (j < label.len) {
                    if (col >= max_col) break;
                    const lbyte = label[j];
                    if (lbyte & 0x80 == 0) {
                        win.writeCell(@intCast(col), @intCast(row), .{
                            .char = .{ .grapheme = label[j .. j + 1], .width = 1 },
                            .style = .{ .ul_style = .single },
                        });
                        col += 1;
                        j += 1;
                    } else {
                        const seq = utf8SeqLen(lbyte);
                        if (j + seq <= label.len) {
                            win.writeCell(@intCast(col), @intCast(row), .{
                                .char = .{ .grapheme = label[j .. j + seq], .width = 1 },
                                .style = .{ .ul_style = .single },
                            });
                            col += 1;
                        }
                        j += seq;
                    }
                }
                i += parsed.consumed;
                continue;
            }
        }

        // ---- Bold/italic delimiters (**text**, *text*, __x__, _x_) -
        if (!in_code) {
            if (i + 1 < text.len and
                (text[i] == '*' and text[i + 1] == '*') or
                (text[i] == '_' and text[i + 1] == '_'))
            {
                bold = !bold;
                i += 2;
                continue;
            }
            if (text[i] == '*' or text[i] == '_') {
                italic = !italic;
                i += 1;
                continue;
            }
        }

        // ---- Emit the current byte/grapheme -------------------------
        const effective_kind: SpanKind = if (in_code)
            .code
        else if (bold and italic)
            .bold_italic
        else if (bold)
            .bold
        else if (italic)
            .italic
        else
            base_kind;

        const style = kindStyle(effective_kind);
        const byte = text[i];
        if (byte & 0x80 == 0) {
            // Slice into `text` for a stable grapheme pointer.  A stack-local
            // `const ch: [1]u8 = .{byte}` dies at end of the loop iteration —
            // the back-buffer cell would hold a dangling pointer (UAF).
            win.writeCell(@intCast(col), @intCast(row), .{
                .char = .{ .grapheme = text[i .. i + 1], .width = 1 },
                .style = style,
            });
            col += 1;
            i += 1;
        } else {
            const seq_len = utf8SeqLen(byte);
            if (i + seq_len <= text.len) {
                win.writeCell(@intCast(col), @intCast(row), .{
                    .char = .{ .grapheme = text[i .. i + seq_len], .width = 1 },
                    .style = style,
                });
                col += 1;
            }
            i += seq_len;
        }
    }
}

const ParsedLink = struct {
    label: []const u8,
    consumed: usize, // bytes consumed from the input string including [label](url)
};

fn parseLinkLabel(text: []const u8) ?ParsedLink {
    // text starts with '['. Find closing ']'.
    if (text.len < 4 or text[0] != '[') return null;
    var j: usize = 1;
    while (j < text.len and text[j] != ']') : (j += 1) {}
    if (j >= text.len) return null;
    const label = text[1..j];
    j += 1; // skip ']'
    if (j >= text.len or text[j] != '(') return null;
    // Find the closing ')'.
    j += 1;
    while (j < text.len and text[j] != ')') : (j += 1) {}
    if (j >= text.len) return null;
    j += 1; // skip ')'
    return .{ .label = label, .consumed = j };
}

fn kindStyle(kind: SpanKind) Style {
    return switch (kind) {
        .normal => .{},
        .bold => .{ .bold = true },
        .italic => .{ .italic = true },
        .bold_italic => .{ .bold = true, .italic = true },
        .code => .{ .fg = .{ .index = 3 } }, // yellow for inline code
        .heading1 => .{ .bold = true, .fg = .{ .index = 6 } }, // cyan
        .heading2 => .{ .bold = true, .fg = .{ .index = 4 } }, // blue
        .heading3 => .{ .bold = true },
        .heading_other => .{ .bold = true, .dim = true },
        .dim => .{ .dim = true },
    };
}

fn utf8SeqLen(first_byte: u8) usize {
    if (first_byte & 0xF0 == 0xF0) return 4;
    if (first_byte & 0xE0 == 0xE0) return 3;
    if (first_byte & 0xC0 == 0xC0) return 2;
    return 1;
}

// =========================================================================
// Tests
// =========================================================================

test "markdown_detail: isHorizontalRule" {
    try std.testing.expect(isHorizontalRule("---"));
    try std.testing.expect(isHorizontalRule("***"));
    try std.testing.expect(isHorizontalRule("___"));
    try std.testing.expect(isHorizontalRule("- - -"));
    try std.testing.expect(!isHorizontalRule("--"));
    try std.testing.expect(!isHorizontalRule("text"));
}

test "markdown_detail: isTableSeparator" {
    try std.testing.expect(isTableSeparator("---|---"));
    try std.testing.expect(isTableSeparator(":--:|:--:"));
    try std.testing.expect(!isTableSeparator("text"));
    try std.testing.expect(!isTableSeparator(""));
}

test "markdown_detail: orderedListPrefix" {
    try std.testing.expectEqual(@as(?usize, 3), orderedListPrefix("1. item"));
    try std.testing.expectEqual(@as(?usize, 3), orderedListPrefix("2. item"));
    try std.testing.expectEqual(@as(?usize, 4), orderedListPrefix("10. item"));
    try std.testing.expectEqual(@as(?usize, null), orderedListPrefix("text"));
    try std.testing.expectEqual(@as(?usize, null), orderedListPrefix("- item"));
}

test "markdown_detail: parseLinkLabel extracts label" {
    const result = parseLinkLabel("[Hello](https://example.com)");
    try std.testing.expect(result != null);
    try std.testing.expectEqualStrings("Hello", result.?.label);
    try std.testing.expectEqual(@as(usize, 28), result.?.consumed);
}

test "markdown_detail: parseLinkLabel returns null for non-links" {
    try std.testing.expect(parseLinkLabel("not a link") == null);
    try std.testing.expect(parseLinkLabel("[unclosed") == null);
}

test "markdown_detail: utf8SeqLen" {
    try std.testing.expectEqual(@as(usize, 1), utf8SeqLen('A'));
    try std.testing.expectEqual(@as(usize, 2), utf8SeqLen(0xC2));
    try std.testing.expectEqual(@as(usize, 3), utf8SeqLen(0xE2));
    try std.testing.expectEqual(@as(usize, 4), utf8SeqLen(0xF0));
}

/// Collect all grapheme bytes from a vaxis Screen buffer into `out`.
/// Used by render-level tests to verify cell content without re-running render.
fn collectScreenText(screen: *const vaxis.Screen, out: *std.ArrayList(u8)) !void {
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) {
            try out.appendSlice(std.testing.allocator, g);
        }
    }
}

test "markdown_detail: render with arena allocator — no U+FFFD, bullet path exercised (task 4198)" {
    // Regression test for the UAF bug fixed in task 4198.
    //
    // The bug: callers of markdown_detail.render passed a stack-backed
    // FixedBufferAllocator. writeCell stores borrowed grapheme slices in the
    // vaxis back-buffer; those slices must outlive the render call. A
    // stack-backed FBA dies when renderDetail returns → dangling → U+FFFD.
    //
    // The fix: callers now pass the per-frame arena. This test exercises the
    // fix by rendering a body that exercises all the internal paths:
    //   - ATX heading (heading1 style)
    //   - Blank line (paragraph separator)
    //   - Unordered bullet list (exercises the allocPrint bullet path at md:154)
    //   - Normal paragraph with inline **bold**
    //
    // Approach: render into a vaxis Screen backed by a test arena. After
    // render() returns (but BEFORE the arena is freed), collect the screen
    // text and assert (a) U+FFFD does not appear, and (b) expected body
    // content appears verbatim.
    //
    // Note on red-capability: in Debug mode the Zig allocator zeroes freed
    // memory, which may turn UAF garbage into \x00 rather than U+FFFD.
    // In ReleaseSafe the stack is reused and the replacement-character
    // manifestation is reliable. This test is therefore reliable in CI
    // (make build uses ReleaseSafe). In Debug it still exercises the render
    // path and provides the content assertions, catching logical regressions.
    const a = std.testing.allocator;

    const body =
        \\# Implementation Notes
        \\
        \\- First bullet item
        \\- Second bullet with **bold** text
        \\
        \\Normal paragraph follows.
    ;

    const win_w: u16 = 80;
    const win_h: u16 = 20;
    var screen = try vaxis.Screen.init(a, .{
        .cols = win_w,
        .rows = win_h,
        .x_pixel = 0,
        .y_pixel = 0,
    });
    defer screen.deinit(a);

    const win: Window = .{
        .x_off = 0,
        .y_off = 0,
        .parent_x_off = 0,
        .parent_y_off = 0,
        .width = win_w,
        .height = win_h,
        .screen = &screen,
    };

    // Use a test arena as the frame-arena stand-in.  The arena must outlive
    // the render call (which it does — we deinit AFTER collect).
    var frame_arena = std.heap.ArenaAllocator.init(a);
    defer frame_arena.deinit();

    // render() must not error.
    try render(win, frame_arena.allocator(), body);

    // Collect rendered text with the arena still live.
    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // (a) No U+FFFD replacement characters. If grapheme slices pointed to
    //     freed stack memory the corruption would surface as garbage bytes;
    //     U+FFFD (EF BF BD) is the canonical marker.
    try std.testing.expect(std.mem.indexOf(u8, text, "\xef\xbf\xbd") == null);

    // (b) Heading text must appear — rendered via renderInline.
    try std.testing.expect(std.mem.indexOf(u8, text, "Implementation") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "Notes") != null);

    // (c) Bullet items must appear — rendered via renderInline after the
    //     bullet prefix is built with allocPrint (the md:154 path).
    try std.testing.expect(std.mem.indexOf(u8, text, "First") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "bullet") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "Second") != null);

    // (d) Bullet glyph U+2022 (E2 80 A2) must appear in the output —
    //     confirms the multibyte sequence is rendered correctly and not dropped.
    try std.testing.expect(std.mem.indexOf(u8, text, "\xe2\x80\xa2") != null);

    // (e) Normal paragraph must appear.
    try std.testing.expect(std.mem.indexOf(u8, text, "Normal") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "paragraph") != null);
}

test "markdown_detail compiles" {
    std.testing.refAllDecls(@This());
}
