//! cockpit/widgets/tree_navigator.zig — Shared collapsible tree-navigator widget.
//!
//! Renders a list of tree nodes with:
//!   - Indentation by depth (2 spaces per level)
//!   - Collapse/expand toggle on Enter
//!   - Selection cursor (j/k or arrow keys)
//!   - Status badge glyph and task-count badge per node
//!
//! Uses the libvaxis low-level Window / Cell API (not vxfw) because the
//! spec calls for custom low-level widgets for the tree.
//!
//! The widget is *stateless in layout*: the caller maintains the nodes
//! slice and the `selected_idx` / `scroll_offset` fields. The widget
//! just renders them deterministically each frame.
//!
//! Task 3957 (tree-navigator) — M2 spine widget.

const std = @import("std");
const vaxis = @import("vaxis");
const view_model = @import("../view_model.zig");

const Window = vaxis.Window;
const Style = vaxis.Style;

/// One node in the navigator tree. The caller populates and manages this
/// slice; the widget renders it.
pub const TreeNode = struct {
    /// Display label (not owned by the widget; caller must keep alive).
    label: []const u8,
    /// Tree depth for indentation (0 = root).
    depth: u32,
    /// Status badge.
    badge: view_model.StatusBadge,
    /// Summary count shown as "[n]" badge at row end, or null.
    count_badge: ?u32,
    /// Whether this node is collapsed.
    collapsed: bool,
    /// Whether this node's children should be hidden (i.e. parent is
    /// collapsed). Caller maintains this by walking the tree before
    /// passing the flat slice.
    hidden: bool,
};

/// State the caller keeps between frames.
pub const Navigator = struct {
    /// Selected visible node index (within the visible slice, not the
    /// full nodes slice).
    selected_idx: usize = 0,
    /// First visible row offset (for scrolling).
    scroll_offset: usize = 0,

    /// Move selection down by one visible node.
    pub fn moveDown(self: *Navigator, visible_count: usize) void {
        if (visible_count == 0) return;
        if (self.selected_idx + 1 < visible_count) {
            self.selected_idx += 1;
        }
    }

    /// Move selection up by one visible node.
    pub fn moveUp(self: *Navigator) void {
        if (self.selected_idx > 0) {
            self.selected_idx -= 1;
        }
    }

    /// Scroll so the selected item is visible within `height` rows.
    pub fn ensureVisible(self: *Navigator, height: usize) void {
        if (height == 0) return;
        if (self.selected_idx < self.scroll_offset) {
            self.scroll_offset = self.selected_idx;
        } else if (self.selected_idx >= self.scroll_offset + height) {
            self.scroll_offset = self.selected_idx - height + 1;
        }
    }
};

/// Render the tree navigator into `win`. Nodes is the full flat list
/// (hidden ones are skipped); `nav` carries the scroll/selection state.
///
/// `arena` is the per-frame allocator. Single-character grapheme slices for
/// ASCII text (writeCell calls) must be arena-allocated: the back buffer stores
/// borrowed grapheme pointers and they must remain valid through vaxis.render().
pub fn render(
    win: Window,
    nodes: []const TreeNode,
    nav: *const Navigator,
    arena: std.mem.Allocator,
) void {
    if (win.height == 0 or win.width == 0) return;
    const h: usize = @intCast(win.height);
    const w: usize = @intCast(win.width);

    var visible_idx: usize = 0;
    var row: usize = 0;

    for (nodes) |node| {
        if (node.hidden) continue;
        defer visible_idx += 1;

        // Scroll: skip nodes before the scroll window.
        if (visible_idx < nav.scroll_offset) continue;
        if (row >= h) break;

        const is_selected = (visible_idx == nav.selected_idx);

        const sel_style: Style = if (is_selected)
            .{ .reverse = true }
        else
            .{};

        // ---- Clear the row with selection background ----------------
        var col: usize = 0;
        while (col < w) : (col += 1) {
            win.writeCell(@intCast(col), @intCast(row), .{
                .char = .{ .grapheme = " ", .width = 1 },
                .style = sel_style,
            });
        }
        col = 0;

        // ---- Indentation (2 spaces per depth) ----------------------
        const indent = node.depth * 2;
        col = @min(indent, w);

        // ---- Collapse/expand indicator: ▶ (collapsed) or ▼ (expanded) -----
        if (col < w) {
            const indicator: []const u8 = if (node.collapsed) "\xe2\x96\xb6" else "\xe2\x96\xbc";
            win.writeCell(@intCast(col), @intCast(row), .{
                .char = .{ .grapheme = indicator, .width = 1 },
                .style = sel_style,
            });
            col += 1;
        }
        if (col < w) {
            win.writeCell(@intCast(col), @intCast(row), .{
                .char = .{ .grapheme = " ", .width = 1 },
                .style = sel_style,
            });
            col += 1;
        }

        // ---- Status badge glyph ------------------------------------
        if (col < w) {
            const glyph = node.badge.glyph();
            const badge_style: Style = if (is_selected)
                .{ .reverse = true }
            else
                badgeStyle(node.badge);
            win.writeCell(@intCast(col), @intCast(row), .{
                .char = .{ .grapheme = glyph, .width = 1 },
                .style = badge_style,
            });
            col += 1;
        }
        if (col < w) {
            win.writeCell(@intCast(col), @intCast(row), .{
                .char = .{ .grapheme = " ", .width = 1 },
                .style = sel_style,
            });
            col += 1;
        }

        // ---- Label text (clipped to row width) ---------------------
        // IMPORTANT: writeCell stores a borrowed grapheme pointer in the back
        // buffer. Using `const ch: [1]u8 = .{byte}` produces a loop-iteration-
        // scoped local that is freed before vaxis.render() — a classic UAF.
        // We use node.label's bytes as subslices (node.label is heap-allocated
        // and long-lived) so the grapheme pointer is stable across the flush.
        //
        var graphemes = vaxis.unicode.graphemeIterator(node.label);
        while (graphemes.next()) |item| {
            const grapheme = item.bytes(node.label);
            const width = win.gwidth(grapheme);
            if (width == 0) continue;
            const cell_width: usize = width;
            if (col + cell_width > w) break;
            win.writeCell(@intCast(col), @intCast(row), .{
                .char = .{ .grapheme = grapheme, .width = @intCast(width) },
                .style = sel_style,
            });
            col += cell_width;
        }

        // ---- Count badge "[n]" at the end, right-aligned if space --
        if (node.count_badge) |cnt| {
            // Allocate via arena so the grapheme slices remain valid through flush.
            const badge_text = std.fmt.allocPrint(arena, "[{d}]", .{cnt}) catch "";
            if (badge_text.len > 0 and w >= badge_text.len) {
                const badge_col = w - badge_text.len;
                if (badge_col > col) {
                    // Enough space to draw it without overlapping the label.
                    var bc: usize = badge_col;
                    for (badge_text, 0..) |_, bi| {
                        if (bc >= w) break;
                        // Slice into the arena-allocated badge_text — stable pointer.
                        win.writeCell(@intCast(bc), @intCast(row), .{
                            .char = .{ .grapheme = badge_text[bi .. bi + 1], .width = 1 },
                            .style = .{ .dim = true },
                        });
                        bc += 1;
                    }
                }
            }
        }

        row += 1;
    }
}

fn badgeStyle(badge: view_model.StatusBadge) Style {
    return switch (badge) {
        .active => .{ .fg = .{ .index = 2 } }, // green
        .stale => .{ .fg = .{ .index = 1 } }, // red
        .done => .{ .dim = true },
        .doing => .{ .fg = .{ .index = 3 } }, // yellow
        .blocked => .{ .fg = .{ .index = 1 } }, // red
        .draft => .{},
        .todo => .{},
        .cancelled => .{ .dim = true },
        .paused => .{ .fg = .{ .index = 5 } }, // magenta
        .abandoned => .{ .dim = true },
        .superseded => .{ .dim = true }, // superseded: visually de-emphasised like abandoned
        .none => .{},
    };
}

test "tree_navigator: Navigator moveDown/moveUp bounds" {
    var nav: Navigator = .{};
    nav.moveDown(5);
    try std.testing.expectEqual(@as(usize, 1), nav.selected_idx);
    nav.moveDown(5);
    nav.moveDown(5);
    nav.moveDown(5);
    nav.moveDown(5);
    // At max (4).
    try std.testing.expectEqual(@as(usize, 4), nav.selected_idx);
    // Cannot go further down past visible_count-1.
    nav.moveDown(5);
    try std.testing.expectEqual(@as(usize, 4), nav.selected_idx);
    nav.moveUp();
    try std.testing.expectEqual(@as(usize, 3), nav.selected_idx);
    // Move up past zero — should stay at 0.
    nav.selected_idx = 0;
    nav.moveUp();
    try std.testing.expectEqual(@as(usize, 0), nav.selected_idx);
}

test "tree_navigator: ensureVisible adjusts scroll_offset" {
    var nav: Navigator = .{ .selected_idx = 10, .scroll_offset = 0 };
    nav.ensureVisible(5);
    // selected_idx=10, height=5: scroll_offset should become 6.
    try std.testing.expectEqual(@as(usize, 6), nav.scroll_offset);

    // Move selection up; scroll should follow.
    nav.selected_idx = 3;
    nav.ensureVisible(5);
    try std.testing.expectEqual(@as(usize, 3), nav.scroll_offset);
}

test "tree_navigator: Navigator moveDown with zero visible_count is safe" {
    var nav: Navigator = .{};
    nav.moveDown(0);
    try std.testing.expectEqual(@as(usize, 0), nav.selected_idx);
}

test "tree_navigator: render — non-ASCII label characters are NOT dropped (task 4195)" {
    // Regression test for the byte-drop bug fixed in task 4195.
    //
    // The bug: the label render loop gated on `byte < 0x80` and silently
    // dropped every high byte — a label like "Café•λ" rendered as "Caf".
    //
    // RED-BEFORE: on the old loop the assertions for é (0xC3 0xA9),
    // • (0xE2 0x80 0xA2), and λ (0xCE 0xBB) all FAILED (indexOf returned null).
    // GREEN-AFTER: all assertions pass after converting to utf8ByteSequenceLength.
    const a = std.testing.allocator;
    // "Café•λ": 'é' = 2-byte, '•' = 3-byte, 'λ' = 2-byte.
    const label = "Caf\xc3\xa9\xe2\x80\xa2\xce\xbb";

    const nodes = [_]TreeNode{.{
        .label = label,
        .depth = 0,
        .badge = .none,
        .count_badge = null,
        .collapsed = false,
        .hidden = false,
    }};
    const nav: Navigator = .{};

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
    render(win, &nodes, &nav, frame_arena.allocator());

    // Collect all grapheme bytes emitted into the screen.
    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) {
            try rendered.appendSlice(a, g);
        }
    }
    const text = rendered.items;

    // No replacement characters.
    try std.testing.expect(std.mem.indexOf(u8, text, "\xef\xbf\xbd") == null);
    // ASCII prefix must appear.
    try std.testing.expect(std.mem.indexOf(u8, text, "Caf") != null);
    // é (U+00E9) must appear — 2-byte sequence.
    try std.testing.expect(std.mem.indexOf(u8, text, "\xc3\xa9") != null);
    // • (U+2022) must appear — 3-byte sequence.
    try std.testing.expect(std.mem.indexOf(u8, text, "\xe2\x80\xa2") != null);
    // λ (U+03BB) must appear — 2-byte sequence.
    try std.testing.expect(std.mem.indexOf(u8, text, "\xce\xbb") != null);
}

test "tree_navigator compiles" {
    std.testing.refAllDecls(@This());
}
