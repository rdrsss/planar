//! cockpit/widgets/view_switcher.zig — View-switcher chrome.
//!
//! Maintains a registry of named views and handles keyboard switching
//! between them. Renders a tab bar along the top of the cockpit.
//!
//! Navigation:
//!   - '1'–'9' jump to a view by 1-based index.
//!   - Tab / Shift-Tab cycle to next / previous view.
//!
//! The view registry is a comptime-fixed array of ViewEntry values.
//! The caller adds entries at startup and the switcher keeps an active
//! index. No heap allocation: all names are string literals.
//!
//! Task 3963 (pane-switching) — M2 spine widget.

const std = @import("std");
const vaxis = @import("vaxis");
const view_model = @import("../view_model.zig");

const Window = vaxis.Window;
const Style = vaxis.Style;
const Key = vaxis.Key;

/// One registered view entry.
pub const ViewEntry = struct {
    id: view_model.ViewId,
    /// Short display name shown in the tab bar.
    name: []const u8,
    /// Single-key shortcut shown in the tab bar (e.g. '1').
    key: u8,
};

/// Maximum number of registered views. Fixed at comptime.
pub const MAX_VIEWS = 8;

/// View-switcher state managed by the caller.
pub const ViewSwitcher = struct {
    entries: [MAX_VIEWS]ViewEntry = undefined,
    count: usize = 0,
    active_idx: usize = 0,

    /// Register a view. Returns error if full.
    pub fn register(self: *ViewSwitcher, entry: ViewEntry) !void {
        if (self.count >= MAX_VIEWS) return error.ViewRegistryFull;
        self.entries[self.count] = entry;
        self.count += 1;
    }

    /// The currently active view's entry.
    pub fn active(self: *const ViewSwitcher) ?ViewEntry {
        if (self.count == 0) return null;
        return self.entries[self.active_idx];
    }

    /// Switch to the next view (wraps around).
    pub fn next(self: *ViewSwitcher) void {
        if (self.count == 0) return;
        self.active_idx = (self.active_idx + 1) % self.count;
    }

    /// Switch to the previous view (wraps around).
    pub fn prev(self: *ViewSwitcher) void {
        if (self.count == 0) return;
        if (self.active_idx == 0) {
            self.active_idx = self.count - 1;
        } else {
            self.active_idx -= 1;
        }
    }

    /// Try to handle a key event for view switching. Returns true when
    /// the key was consumed (active_idx changed or was confirmed).
    pub fn handleKey(self: *ViewSwitcher, key: Key) bool {
        // Tab → next view.
        if (key.matches(Key.tab, .{})) {
            self.next();
            return true;
        }
        // Shift-Tab → previous view.
        if (key.matches(Key.tab, .{ .shift = true })) {
            self.prev();
            return true;
        }
        // '1'–'9' direct jump.
        if (key.codepoint >= '1' and key.codepoint <= '9') {
            const idx: usize = @intCast(key.codepoint - '1');
            if (idx < self.count) {
                self.active_idx = idx;
                return true;
            }
        }
        return false;
    }

    /// Switch to a view by id. Returns false if not registered.
    pub fn switchTo(self: *ViewSwitcher, id: view_model.ViewId) bool {
        for (self.entries[0..self.count], 0..) |entry, i| {
            if (entry.id == id) {
                self.active_idx = i;
                return true;
            }
        }
        return false;
    }

    /// Render the tab bar into the top row of `win`. The row at y=0 is
    /// consumed; callers should use `win.child(.{.y_off=1, ...})` for
    /// the content area below the tab bar.
    pub fn renderTabBar(self: *const ViewSwitcher, win: Window) void {
        if (win.height == 0 or win.width == 0) return;
        const w: usize = @intCast(win.width);
        var col: usize = 0;

        // Clear the tab bar row.
        while (col < w) : (col += 1) {
            win.writeCell(@intCast(col), 0, .{
                .char = .{ .grapheme = " ", .width = 1 },
                .style = .{},
            });
        }
        col = 0;

        for (self.entries[0..self.count], 0..) |entry, i| {
            if (col >= w) break;
            const is_active = (i == self.active_idx);
            const tab_style: Style = if (is_active)
                .{ .bold = true, .reverse = true }
            else
                .{ .dim = true };

            // Render " [key] name " per tab.
            const tab_parts: [4][]const u8 = .{
                " [",
                &[_]u8{entry.key},
                "] ",
                entry.name,
            };
            for (tab_parts) |part| {
                for (part) |byte| {
                    if (col >= w) break;
                    const ch: [1]u8 = .{byte};
                    win.writeCell(@intCast(col), 0, .{
                        .char = .{ .grapheme = &ch, .width = 1 },
                        .style = tab_style,
                    });
                    col += 1;
                }
            }
            // Separator between tabs.
            if (col < w and !is_active) {
                win.writeCell(@intCast(col), 0, .{
                    .char = .{ .grapheme = " ", .width = 1 },
                    .style = .{},
                });
                col += 1;
            }
        }
    }
};

// =========================================================================
// Tests
// =========================================================================

test "view_switcher: register and active" {
    var vs: ViewSwitcher = .{};
    try vs.register(.{ .id = .agent_monitor, .name = "Monitor", .key = '1' });
    try vs.register(.{ .id = .scope_explorer, .name = "Explorer", .key = '2' });
    try std.testing.expectEqual(@as(usize, 2), vs.count);
    try std.testing.expectEqual(view_model.ViewId.agent_monitor, vs.active().?.id);
}

test "view_switcher: next wraps around" {
    var vs: ViewSwitcher = .{};
    try vs.register(.{ .id = .agent_monitor, .name = "M", .key = '1' });
    try vs.register(.{ .id = .scope_explorer, .name = "E", .key = '2' });
    vs.next();
    try std.testing.expectEqual(@as(usize, 1), vs.active_idx);
    vs.next();
    try std.testing.expectEqual(@as(usize, 0), vs.active_idx);
}

test "view_switcher: prev wraps around" {
    var vs: ViewSwitcher = .{};
    try vs.register(.{ .id = .agent_monitor, .name = "M", .key = '1' });
    try vs.register(.{ .id = .scope_explorer, .name = "E", .key = '2' });
    vs.prev();
    try std.testing.expectEqual(@as(usize, 1), vs.active_idx);
    vs.prev();
    try std.testing.expectEqual(@as(usize, 0), vs.active_idx);
}

test "view_switcher: switchTo by id" {
    var vs: ViewSwitcher = .{};
    try vs.register(.{ .id = .agent_monitor, .name = "M", .key = '1' });
    try vs.register(.{ .id = .scope_explorer, .name = "E", .key = '2' });
    try vs.register(.{ .id = .task_board, .name = "T", .key = '3' });
    try std.testing.expect(vs.switchTo(.task_board));
    try std.testing.expectEqual(@as(usize, 2), vs.active_idx);
    // Switch back to agent_monitor by id.
    try std.testing.expect(vs.switchTo(.agent_monitor));
    try std.testing.expectEqual(@as(usize, 0), vs.active_idx);
}

test "view_switcher: register at MAX_VIEWS returns error" {
    var vs: ViewSwitcher = .{};
    var i: usize = 0;
    while (i < MAX_VIEWS) : (i += 1) {
        try vs.register(.{
            .id = .agent_monitor,
            .name = "X",
            .key = @intCast('1' + i),
        });
    }
    const result = vs.register(.{ .id = .agent_monitor, .name = "overflow", .key = '9' });
    try std.testing.expectError(error.ViewRegistryFull, result);
}

test "view_switcher: next with zero count is safe" {
    var vs: ViewSwitcher = .{};
    vs.next();
    try std.testing.expectEqual(@as(usize, 0), vs.active_idx);
    try std.testing.expect(vs.active() == null);
}

test "view_switcher: handleKey Shift-Tab cycles to previous view" {
    // Task 4056: missing Shift-Tab unit test for handleKey.
    var vs: ViewSwitcher = .{};
    try vs.register(.{ .id = .agent_monitor, .name = "M", .key = '1' });
    try vs.register(.{ .id = .scope_explorer, .name = "E", .key = '2' });
    try vs.register(.{ .id = .task_board, .name = "T", .key = '3' });

    // Start at index 0 (agent_monitor). Shift-Tab should wrap to last (task_board).
    const shift_tab = Key{ .codepoint = Key.tab, .mods = .{ .shift = true } };
    const consumed = vs.handleKey(shift_tab);
    try std.testing.expect(consumed);
    try std.testing.expectEqual(@as(usize, 2), vs.active_idx);

    // Shift-Tab again → index 1.
    const consumed2 = vs.handleKey(shift_tab);
    try std.testing.expect(consumed2);
    try std.testing.expectEqual(@as(usize, 1), vs.active_idx);

    // Shift-Tab again → index 0.
    const consumed3 = vs.handleKey(shift_tab);
    try std.testing.expect(consumed3);
    try std.testing.expectEqual(@as(usize, 0), vs.active_idx);
}

test "view_switcher: handleKey Tab cycles to next view" {
    var vs: ViewSwitcher = .{};
    try vs.register(.{ .id = .agent_monitor, .name = "M", .key = '1' });
    try vs.register(.{ .id = .scope_explorer, .name = "E", .key = '2' });

    const tab = Key{ .codepoint = Key.tab, .mods = .{} };
    const consumed = vs.handleKey(tab);
    try std.testing.expect(consumed);
    try std.testing.expectEqual(@as(usize, 1), vs.active_idx);

    // Tab again → wraps to 0.
    const consumed2 = vs.handleKey(tab);
    try std.testing.expect(consumed2);
    try std.testing.expectEqual(@as(usize, 0), vs.active_idx);
}

test "view_switcher: handleKey digit direct-jump" {
    var vs: ViewSwitcher = .{};
    try vs.register(.{ .id = .agent_monitor, .name = "M", .key = '1' });
    try vs.register(.{ .id = .scope_explorer, .name = "E", .key = '2' });
    try vs.register(.{ .id = .task_board, .name = "T", .key = '3' });

    const key2 = Key{ .codepoint = '2', .mods = .{} };
    const consumed = vs.handleKey(key2);
    try std.testing.expect(consumed);
    try std.testing.expectEqual(@as(usize, 1), vs.active_idx);

    // Out-of-range digit (e.g. '9' with only 3 views) → not consumed.
    const key9 = Key{ .codepoint = '9', .mods = .{} };
    const consumed9 = vs.handleKey(key9);
    try std.testing.expect(!consumed9);
    // Active index unchanged.
    try std.testing.expectEqual(@as(usize, 1), vs.active_idx);
}

test "view_switcher compiles" {
    std.testing.refAllDecls(@This());
}
