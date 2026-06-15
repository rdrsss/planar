//! cockpit/widgets/split_layout.zig — Shared split-layout widget.
//!
//! Wraps libvaxis's `vxfw.SplitView` with cockpit-specific focus-toggle
//! and resize-aware behaviour. The split is always horizontal (navigator
//! pane left | detail pane right).
//!
//! Focus management:
//!   - `focus` tracks which pane has keyboard focus: .navigator or .detail.
//!   - Tab / Shift-Tab cycles focus between panes.
//!   - The focused pane receives a distinct border highlight.
//!   - Resize: the navigator pane occupies `nav_width` columns; the detail
//!     pane fills the rest. When the terminal is narrower than the
//!     minimum, the min-size guard in `app.zig` fires first.
//!
//! This widget is low-level (uses vaxis Window directly, not vxfw) to
//! keep layout control explicit.
//!
//! Task 3959 (split-layout) — M2 spine widget.

const std = @import("std");
const vaxis = @import("vaxis");

const Window = vaxis.Window;
const Style = vaxis.Style;

/// Which pane currently has keyboard focus.
pub const FocusedPane = enum {
    navigator,
    detail,

    pub fn toggle(self: FocusedPane) FocusedPane {
        return switch (self) {
            .navigator => .detail,
            .detail => .navigator,
        };
    }
};

/// Split-layout state managed by the caller across frames.
pub const SplitLayout = struct {
    /// Width in columns of the left (navigator) pane.
    nav_width: u16 = 40,
    /// Minimum usable navigator width.
    min_nav_width: u16 = 10,
    /// Maximum navigator width (0 = no limit).
    max_nav_width: u16 = 80,
    /// Which pane has keyboard focus.
    focus: FocusedPane = .navigator,

    /// Toggle focus between navigator and detail pane.
    pub fn toggleFocus(self: *SplitLayout) void {
        self.focus = self.focus.toggle();
    }

    /// Clamp nav_width to the available terminal width. Returns the
    /// effective widths of the two panes and the separator column.
    /// `total_width` is the full terminal column count.
    pub fn effectiveWidths(self: SplitLayout, total_width: u16) struct {
        nav: u16,
        sep: u16,
        detail: u16,
    } {
        if (total_width == 0) return .{ .nav = 0, .sep = 0, .detail = 0 };
        const clamped = @min(
            if (self.max_nav_width > 0) self.max_nav_width else total_width,
            @max(self.min_nav_width, self.nav_width),
        );
        const nav = @min(clamped, total_width -| 1);
        const sep: u16 = 1;
        const detail = total_width -| nav -| sep;
        return .{ .nav = nav, .sep = sep, .detail = detail };
    }

    /// Split `win` into navigator and detail sub-windows. Returns a
    /// tuple of (nav_win, detail_win). The separator column is drawn
    /// by this function (vertical bar).
    pub fn splitWindow(
        self: SplitLayout,
        win: Window,
    ) struct { nav: Window, detail: Window } {
        const widths = self.effectiveWidths(@intCast(win.width));
        const nav_win = win.child(.{
            .x_off = 0,
            .y_off = 0,
            .width = widths.nav,
            .height = win.height,
        });
        const detail_win = win.child(.{
            .x_off = widths.nav + widths.sep,
            .y_off = 0,
            .width = widths.detail,
            .height = win.height,
        });

        // Draw the separator.
        const sep_col: u16 = widths.nav;
        const sep_style: Style = .{ .fg = .{ .index = 8 } }; // dark gray
        var row: u16 = 0;
        while (row < win.height) : (row += 1) {
            win.writeCell(sep_col, row, .{
                .char = .{ .grapheme = "\xe2\x94\x82", .width = 1 }, // │
                .style = sep_style,
            });
        }

        return .{ .nav = nav_win, .detail = detail_win };
    }

    /// Draw a focused border indicator (top edge) on the active pane.
    pub fn drawFocusBorder(self: SplitLayout, nav_win: Window, detail_win: Window) void {
        const focused_win = switch (self.focus) {
            .navigator => nav_win,
            .detail => detail_win,
        };
        const focus_style: Style = .{ .fg = .{ .index = 6 } }; // cyan
        if (focused_win.width == 0 or focused_win.height == 0) return;
        var col: u16 = 0;
        while (col < focused_win.width) : (col += 1) {
            focused_win.writeCell(col, 0, .{
                .char = .{ .grapheme = "\xe2\x94\x80", .width = 1 }, // ─
                .style = focus_style,
            });
        }
    }
};

// =========================================================================
// Tests
// =========================================================================

test "split_layout: effectiveWidths sums to total_width" {
    const sl: SplitLayout = .{ .nav_width = 40 };
    const w = sl.effectiveWidths(100);
    try std.testing.expectEqual(@as(u16, 100), w.nav + w.sep + w.detail);
}

test "split_layout: effectiveWidths clamps nav to min" {
    const sl: SplitLayout = .{ .nav_width = 5, .min_nav_width = 10 };
    const w = sl.effectiveWidths(80);
    try std.testing.expect(w.nav >= sl.min_nav_width);
}

test "split_layout: effectiveWidths with zero total returns zero" {
    const sl: SplitLayout = .{};
    const w = sl.effectiveWidths(0);
    try std.testing.expectEqual(@as(u16, 0), w.nav);
    try std.testing.expectEqual(@as(u16, 0), w.detail);
}

test "split_layout: toggleFocus cycles between panes" {
    var sl: SplitLayout = .{};
    try std.testing.expectEqual(FocusedPane.navigator, sl.focus);
    sl.toggleFocus();
    try std.testing.expectEqual(FocusedPane.detail, sl.focus);
    sl.toggleFocus();
    try std.testing.expectEqual(FocusedPane.navigator, sl.focus);
}

test "split_layout: FocusedPane.toggle" {
    try std.testing.expectEqual(FocusedPane.detail, FocusedPane.navigator.toggle());
    try std.testing.expectEqual(FocusedPane.navigator, FocusedPane.detail.toggle());
}

test "split_layout compiles" {
    std.testing.refAllDecls(@This());
}
