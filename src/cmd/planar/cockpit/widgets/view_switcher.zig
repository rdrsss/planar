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
/// Set to 24 to give headroom for all planned cockpit milestones (M11–M17+).
/// Numeric quick-keys cover '1'–'9' (views 1–9); views 10+ are reachable
/// via Tab/Shift-Tab cycling only (no numeric key assigned).
pub const MAX_VIEWS = 24;

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
    ///
    /// `arena` is the per-frame allocator. The key byte in each tab label is
    /// arena-allocated so the grapheme pointer is stable through vaxis.render().
    pub fn renderTabBar(self: *const ViewSwitcher, win: Window, arena: std.mem.Allocator) void {
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
            // IMPORTANT: writeCell stores borrowed grapheme pointers in the back
            // buffer — they must remain valid through vaxis.render(). A
            // loop-iteration-local `const ch: [1]u8 = .{byte}` or `key_slice`
            // dies when the outer for loop advances to the next entry, causing a
            // UAF. Fix: arena-allocate the key byte slice so its backing memory
            // lives until the next frame reset (after render).
            //
            // " [" and "] " are string literals (static storage — always valid).
            // entry.name is heap-allocated state (always valid).
            // entry.key is a u8 value field; allocate a one-byte slice via arena.
            const key_str = std.fmt.allocPrint(arena, "{c}", .{entry.key}) catch " ";
            const tab_parts: [4][]const u8 = .{
                " [",
                key_str,
                "] ",
                entry.name,
            };
            for (tab_parts) |part| {
                // Iterate by UTF-8 codepoint so that multi-byte characters in
                // tab names (e.g. non-ASCII labels) are emitted as a single cell.
                // The old `pi += 1` loop emitted one byte per cell, splitting
                // multi-byte sequences into invalid 1-byte graphemes.
                var pi: usize = 0;
                while (pi < part.len and col < w) {
                    const byte = part[pi];
                    const seq_len: usize = std.unicode.utf8ByteSequenceLength(byte) catch {
                        pi += 1;
                        continue;
                    };
                    if (pi + seq_len > part.len) break;
                    // Slice into `part` for a stable grapheme pointer.
                    win.writeCell(@intCast(col), 0, .{
                        .char = .{ .grapheme = part[pi .. pi + seq_len], .width = 1 },
                        .style = tab_style,
                    });
                    col += 1;
                    pi += seq_len;
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
    try vs.register(.{ .id = .decision_log, .name = "D", .key = '4' });
    try std.testing.expect(vs.switchTo(.task_board));
    try std.testing.expectEqual(@as(usize, 2), vs.active_idx);
    // Switch to decision_log by id.
    try std.testing.expect(vs.switchTo(.decision_log));
    try std.testing.expectEqual(@as(usize, 3), vs.active_idx);
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

test "view_switcher: renderTabBar — non-ASCII tab name characters are NOT split (task 4195)" {
    // Regression test for the byte-split bug fixed in task 4195.
    //
    // The bug: the tab part render loop advanced `pi += 1` per iteration, emitting
    // one byte per cell.  For a 2+ byte UTF-8 codepoint this splits the sequence
    // into invalid 1-byte graphemes (→ U+FFFD or garbled output).
    //
    // The fix: advance by cpSeqLen (via std.unicode.utf8ByteSequenceLength) so the
    // full sequence is emitted as a single grapheme.
    //
    // RED-BEFORE: on the old loop the assertion for é (0xC3 0xA9) FAILED because
    // the two bytes were emitted as two separate cells instead of one grapheme.
    // GREEN-AFTER: the full 2-byte sequence appears as a single cell grapheme.
    const a = std.testing.allocator;
    var vs: ViewSwitcher = .{};
    // Tab name "Caf\xc3\xa9" = "Café" — the 'é' (U+00E9) is a 2-byte sequence.
    try vs.register(.{ .id = .agent_monitor, .name = "Caf\xc3\xa9", .key = '1' });

    const win_w: u16 = 40;
    const win_h: u16 = 2;
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
    vs.renderTabBar(win, frame_arena.allocator());

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
    // ASCII prefix of the name must appear.
    try std.testing.expect(std.mem.indexOf(u8, text, "Caf") != null);
    // é (U+00E9, 0xC3 0xA9) must appear as a 2-byte grapheme (not split bytes).
    try std.testing.expect(std.mem.indexOf(u8, text, "\xc3\xa9") != null);
}

test "view_switcher compiles" {
    std.testing.refAllDecls(@This());
}
