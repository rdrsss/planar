//! cockpit/app.zig — M1 cockpit shell (scaffold).
//!
//! Milestone 1: enter/leave alt-screen, display an on-screen key legend,
//! exit cleanly on `q` / Ctrl-C (terminal restored, process exits 0).
//!
//! No views, no view-model, no wake integration, no editing. This is the
//! scaffold only; the catalog of views lands in M2+ milestones.
//!
//! Entry points:
//!   `run(io, alloc, env_map)` — launched by bare `planar` on a TTY or
//!                               by `planar explore`. Blocks until quit.

const std = @import("std");
const vaxis = @import("vaxis");

const Key = vaxis.Key;
const Style = vaxis.Style;
const Vaxis = vaxis.Vaxis;
const Window = vaxis.Window;
const Winsize = vaxis.Winsize;

/// Event type for this shell. We only need key presses and window resize.
pub const Event = union(enum) {
    key_press: Key,
    winsize: Winsize,
};

/// Run the cockpit shell. Blocks until the operator quits (`q` or Ctrl-C).
/// Restores the terminal on exit (alt-screen leave, cursor restore).
/// Called by bare `planar` on a TTY and by `planar explore`.
pub fn run(io: std.Io, alloc: std.mem.Allocator, env_map: *std.process.Environ.Map) !void {
    // 4 KiB write buffer for the TTY (alt-screen commands, rendered cells).
    var tty_buf: [4096]u8 = undefined;
    var tty = try vaxis.Tty.init(io, &tty_buf);
    defer tty.deinit();

    var vx = try vaxis.init(io, alloc, env_map, .{});
    defer vx.deinit(alloc, tty.writer());

    var loop: vaxis.Loop(Event) = .init(io, &tty, &vx);
    try loop.installResizeHandler();
    try loop.start();
    defer loop.stop();

    // Enter the alternate screen so the main terminal scrollback is undisturbed.
    try vx.enterAltScreen(tty.writer());
    // Query terminal capabilities (kitty keyboard, RGB color, etc.).
    // 1-second timeout so we don't hang on terminals that don't respond.
    try vx.queryTerminal(tty.writer(), .fromSeconds(1));

    // Render the initial frame before blocking on the first event.
    try renderFrame(&vx, tty.writer(), alloc);

    // Main event loop.
    while (true) {
        const event = try loop.nextEvent();
        switch (event) {
            .key_press => |key| {
                // `q` or Ctrl-C → exit the cockpit cleanly.
                if (key.matches('q', .{}) or key.matches('c', .{ .ctrl = true })) {
                    break;
                }
            },
            .winsize => |ws| {
                try vx.resize(alloc, tty.writer(), ws);
            },
        }
        try renderFrame(&vx, tty.writer(), alloc);
    }
    // Terminal is restored by the deferred `vx.deinit` call above.
}

/// Render one frame of the M1 scaffold UI.
/// Shows a banner, a separator, and the key legend.
fn renderFrame(vx: *Vaxis, tty_writer: *std.Io.Writer, _: std.mem.Allocator) !void {
    const win = vx.window();
    win.clear();

    if (win.width < 20 or win.height < 4) {
        renderTooSmall(win);
        try vx.render(tty_writer);
        return;
    }

    // ---- Banner (row 0) -----------------------------------------------
    _ = win.printSegment(.{
        .text = "Planar Cockpit  [M1 scaffold]",
        .style = .{ .bold = true },
    }, .{ .row_offset = 0, .col_offset = 0 });

    // ---- Separator (row 1) --------------------------------------------
    var col: u16 = 0;
    while (col < win.width) : (col += 1) {
        win.writeCell(col, 1, .{
            .char = .{ .grapheme = "\xe2\x94\x80" }, // U+2500 BOX DRAWINGS LIGHT HORIZONTAL
            .style = .{},
        });
    }

    // ---- Key legend (last usable row) ---------------------------------
    _ = win.printSegment(.{
        .text = "  q  Quit     Ctrl-C  Quit",
        .style = .{},
    }, .{ .row_offset = win.height - 1, .col_offset = 0 });

    try vx.render(tty_writer);
}

/// Show a "terminal too small" notice when the window is below usable size.
fn renderTooSmall(win: Window) void {
    _ = win.printSegment(.{
        .text = "Terminal too small",
        .style = .{},
    }, .{ .row_offset = 0, .col_offset = 0 });
}

test "cockpit app compiles" {
    // Ensure the module and its imports resolve cleanly. No runtime test —
    // the cockpit requires a real TTY. The view-model (M2+) will be
    // unit-tested against an in-memory screen buffer.
    @import("std").testing.refAllDecls(@This());
}
