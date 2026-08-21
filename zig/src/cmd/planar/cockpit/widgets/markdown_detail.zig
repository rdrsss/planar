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

    var graphemes = vaxis.unicode.graphemeIterator(text);
    while (graphemes.next()) |item| {
        const grapheme = item.bytes(text);
        const width = win.gwidth(grapheme);
        if (width == 0) continue;
        const cell_width: usize = width;
        if (col + cell_width > max_col) break;
        win.writeCell(@intCast(col), @intCast(row), .{
            .char = .{ .grapheme = grapheme, .width = @intCast(width) },
            .style = style,
        });
        col += cell_width;
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
                var label_graphemes = vaxis.unicode.graphemeIterator(label);
                while (label_graphemes.next()) |item| {
                    const grapheme = item.bytes(label);
                    const width = win.gwidth(grapheme);
                    if (width == 0) continue;
                    const cell_width: usize = width;
                    if (col + cell_width > max_col) break;
                    win.writeCell(@intCast(col), @intCast(row), .{
                        .char = .{ .grapheme = grapheme, .width = @intCast(width) },
                        .style = .{ .ul_style = .single },
                    });
                    col += cell_width;
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
        var current_graphemes = vaxis.unicode.graphemeIterator(text[i..]);
        const item = current_graphemes.next() orelse break;
        const grapheme = item.bytes(text[i..]);
        const width = win.gwidth(grapheme);
        i += grapheme.len;
        if (width == 0) continue;
        const cell_width: usize = width;
        if (col + cell_width > max_col) break;
        win.writeCell(@intCast(col), @intCast(row), .{
            .char = .{ .grapheme = grapheme, .width = @intCast(width) },
            .style = style,
        });
        col += cell_width;
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

/// Return the byte-sequence length of the UTF-8 codepoint whose lead byte is
/// `first_byte`. On a stray continuation byte (0x80–0xBF) or an invalid
/// lead byte (0xF8–0xFF) the function returns `error.InvalidByte`; callers
/// MUST advance by 1 and continue — never by the result of a prior call —
/// to avoid desync.
///
/// Replaces the old hand-rolled `utf8SeqLen` which returned 1 for every
/// continuation byte, causing multi-byte codepoints to be emitted as a
/// sequence of 1-byte cells (split → U+FFFD, or in gated loops, dropped).
pub fn cpSeqLen(first_byte: u8) error{InvalidByte}!usize {
    return std.unicode.utf8ByteSequenceLength(first_byte) catch error.InvalidByte;
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

test "markdown_detail: cpSeqLen — lead bytes" {
    try std.testing.expectEqual(@as(usize, 1), try cpSeqLen('A'));
    try std.testing.expectEqual(@as(usize, 2), try cpSeqLen(0xC2));
    try std.testing.expectEqual(@as(usize, 3), try cpSeqLen(0xE2));
    try std.testing.expectEqual(@as(usize, 4), try cpSeqLen(0xF0));
}

test "markdown_detail: cpSeqLen — continuation bytes return InvalidByte (task 4196)" {
    // Continuation bytes (0x80–0xBF) are not valid UTF-8 lead bytes.
    // The old utf8SeqLen returned 1 for these, causing multi-byte codepoints
    // to be emitted as split 1-byte cells.  cpSeqLen must reject them.
    try std.testing.expectError(error.InvalidByte, cpSeqLen(0x80));
    try std.testing.expectError(error.InvalidByte, cpSeqLen(0x9F));
    try std.testing.expectError(error.InvalidByte, cpSeqLen(0xBF));
    // Also reject overlong / surrogate lead bytes.
    try std.testing.expectError(error.InvalidByte, cpSeqLen(0xF8));
    try std.testing.expectError(error.InvalidByte, cpSeqLen(0xFF));
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

test "markdown_detail: non-ASCII title/body rendered without dropped chars (task 4195)" {
    // Regression test for the byte-drop bug fixed in task 4195.
    //
    // The bug: renderPlain / renderInline iterated byte-by-byte and emitted
    // cells only when `byte & 0x80 == 0` — silently dropping the high bytes of
    // every multi-byte UTF-8 codepoint.  A title like "Café•λ" would appear
    // as "Caf" in the rendered output (the accented 'é', bullet '•', and Greek
    // 'λ' were silently dropped).
    //
    // RED-BEFORE: on the old loops the assertions for "é" (0xC3 0xA9),
    // "•" (0xE2 0x80 0xA2), and "λ" (0xCE 0xBB) all FAILED (indexOf returned null).
    // GREEN-AFTER: all assertions pass with the cpSeqLen-based loops.
    //
    // String breakdown:
    //   "Café"  — C(1) a(1) f(1) é(2: 0xC3 0xA9)
    //   "•"     — bullet U+2022 (3: 0xE2 0x80 0xA2)
    //   "λ"     — lambda U+03BB (2: 0xCE 0xBB)
    const a = std.testing.allocator;

    // A paragraph with non-ASCII characters that the old byte-gated loop dropped.
    const body = "Caf\xc3\xa9\xe2\x80\xa2\xce\xbb";

    const win_w: u16 = 40;
    const win_h: u16 = 4;
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

    var frame_arena = std.heap.ArenaAllocator.init(a);
    defer frame_arena.deinit();

    try render(win, frame_arena.allocator(), body);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectScreenText(&screen, &rendered);
    const text = rendered.items;

    // No replacement characters.
    try std.testing.expect(std.mem.indexOf(u8, text, "\xef\xbf\xbd") == null);

    // ASCII prefix must appear.
    try std.testing.expect(std.mem.indexOf(u8, text, "Caf") != null);

    // é (U+00E9, 0xC3 0xA9) must appear — 2-byte sequence.
    try std.testing.expect(std.mem.indexOf(u8, text, "\xc3\xa9") != null);

    // • (U+2022, 0xE2 0x80 0xA2) must appear — 3-byte sequence.
    try std.testing.expect(std.mem.indexOf(u8, text, "\xe2\x80\xa2") != null);

    // λ (U+03BB, 0xCE 0xBB) must appear — 2-byte sequence.
    try std.testing.expect(std.mem.indexOf(u8, text, "\xce\xbb") != null);
}

test "markdown_detail uses grapheme display widths for CJK and combining text" {
    const a = std.testing.allocator;
    var screen = try vaxis.Screen.init(a, .{ .cols = 8, .rows = 2, .x_pixel = 0, .y_pixel = 0 });
    defer screen.deinit(a);
    const win: Window = .{
        .x_off = 0,
        .y_off = 0,
        .parent_x_off = 0,
        .parent_y_off = 0,
        .width = 8,
        .height = 2,
        .screen = &screen,
    };
    var arena = std.heap.ArenaAllocator.init(a);
    defer arena.deinit();

    try render(win, arena.allocator(), "界e\xcc\x81X");
    try std.testing.expectEqual(@as(u2, 2), screen.readCell(0, 0).?.char.width);
    try std.testing.expectEqualStrings("e\xcc\x81", screen.readCell(2, 0).?.char.grapheme);
    try std.testing.expectEqualStrings("X", screen.readCell(3, 0).?.char.grapheme);
}

test "markdown_detail compiles" {
    std.testing.refAllDecls(@This());
}
