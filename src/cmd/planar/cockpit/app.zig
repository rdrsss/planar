//! cockpit/app.zig — Cockpit shell with spine, wake integration, and Scope Explorer.
//!
//! Milestone 3: Scope Explorer (anchor view, default landing).
//!   - DB-wiring (task 4055): DB path is now threaded through the runtime context;
//!     bare `planar` and `planar explore` both call `run` + `openDb` (not
//!     `runWithoutDb`). The live wake-redraw loop is fully exercised:
//!     WAL change → .db_changed → re-query → repaint.
//!   - openDb-completeness (task 4056): schema-ahead refusal (DbSchemaMismatch)
//!     via assertSchemaCompatible; busy/locked distinction (DbLocked) via file
//!     existence pre-check + driver error mapping.
//!   - Scope Explorer (tasks 3966–3970): collapsible plan tree, scope-filter
//!     toggle, plan-drill child rows, split detail pane, collapse/expand keys.
//!
//! Milestone 2 (preserved):
//!   - Wake integration (tasks 4013 + 3961).
//!   - View-switcher chrome (1–9 / Tab / Shift-Tab).
//!   - Split-layout with focus toggle (Tab within the content area).
//!   - Minimum terminal-size guard (task 3965).
//!
//! Entry points:
//!   `run(io, alloc, env_map, db_path, db_handle)` — full cockpit with DB.
//!   `runWithoutDb(io, alloc, env_map)` — kept for backwards-compat tests only.

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");

const Key = vaxis.Key;
const Style = vaxis.Style;
const Vaxis = vaxis.Vaxis;
const Window = vaxis.Window;
const Winsize = vaxis.Winsize;

const engine = @import("engine");
const wake_mod = engine.runtime.agentactivity.wake;
const view_model = @import("view_model.zig");
const view_switcher = @import("widgets/view_switcher.zig");
const split_layout = @import("widgets/split_layout.zig");
const scope_explorer = @import("views/scope_explorer.zig");

/// Minimum usable terminal dimensions.
const MIN_WIDTH: u16 = 40;
const MIN_HEIGHT: u16 = 8;

/// Wake heartbeat interval: ≤1 second.
const WAKE_HEARTBEAT_NS: u64 = 900 * std.time.ns_per_ms;

/// Event type for the cockpit main loop. Extends M1's basic events with
/// the DB-changed signal from the wake thread.
pub const Event = union(enum) {
    key_press: Key,
    winsize: Winsize,
    /// Fired by the wake thread when the WAL changes OR on the heartbeat
    /// tick (the coalesced/missed-wake backstop, task 3961).
    db_changed,
};

/// Error returned when the DB cannot be opened for the cockpit.
pub const DbOpenError = error{
    DbMissing,
    DbLocked,
    DbSchemaMismatch,
    DbOpenFailed,
};

/// Attempt to open the Planar DB at `db_path`. Returns a clean-failure
/// error (never a half-rendered TUI) when:
///   - The file does not exist (DbMissing).
///   - The file is locked/busy (DbLocked): exists but the SQLite driver
///     cannot open it (permissions) or an ExecFailed/PrepareFailed during
///     the migrations probe indicates a WAL write-lock held by another
///     process.
///   - The schema version is ahead of what this binary expects
///     (DbSchemaMismatch): a newer binary migrated the DB — operator must
///     upgrade planar.
///   - Any other SQLite error (DbOpenFailed).
///
/// Tasks 4055 (DB-wiring) + 4056 (openDb-completeness).
pub fn openDb(io: std.Io, db_path: []const u8, allocator: std.mem.Allocator) DbOpenError!db.sqlite.Db {
    // Check existence before opening to give a cleaner error message.
    std.Io.Dir.cwd().access(io, db_path, .{}) catch |e| switch (e) {
        error.FileNotFound => return DbOpenError.DbMissing,
        // Other access errors (permissions, etc.) → file may exist but
        // we cannot read it, which is a "locked" or "no-access" state.
        else => return DbOpenError.DbLocked,
    };

    // Duplicate to a sentinel-terminated C string for the SQLite API.
    const db_path_z: [:0]const u8 = allocator.dupeZ(u8, db_path) catch
        return DbOpenError.DbOpenFailed;
    defer allocator.free(db_path_z);

    var d = db.sqlite.Db.open(db_path_z.ptr) catch {
        // Db.open returns OpenFailed for any driver-level refusal (e.g.
        // SQLITE_CANTOPEN due to permissions). The file exists (checked
        // above) but cannot be opened → treat as locked/inaccessible.
        return DbOpenError.DbLocked;
    };
    errdefer d.close();

    // Apply pending migrations idempotently. A failure here (ExecFailed /
    // PrepareFailed / StepFailed from the schema_migrations read) is the
    // symptom of a WAL write-lock held by another process: the file exists
    // and opens but the first SQLite query fails with SQLITE_BUSY or
    // SQLITE_LOCKED. Map to DbLocked so the caller can show a useful message.
    db.migrate.applyAll(&d, allocator) catch {
        return DbOpenError.DbLocked;
    };

    // Schema-ahead guard: if the DB was migrated by a newer binary, the
    // cockpit refuses rather than operating against an unknown schema.
    // Maps to DbSchemaMismatch so the caller can surface both versions.
    var db_version: u32 = 0;
    var emb_max: u32 = 0;
    db.migrate.assertSchemaCompatible(&d, &db_version, &emb_max) catch {
        return DbOpenError.DbSchemaMismatch;
    };

    return d;
}

/// Context passed to the wake thread. Must outlive the thread.
const WakeThreadCtx = struct {
    loop: *vaxis.Loop(Event),
    db_path: []const u8,
    stop: std.atomic.Value(bool),
    allocator: std.mem.Allocator,
};

/// Wake thread entry point (tasks 4013 + 3961).
///
/// Owns a `Wake` instance and calls `loop.postEvent(.db_changed)` on:
///   - `.wal_changed`: a real WAL write — edge-driven, near-immediate.
///   - `.heartbeat`: the ≤1 s `waitNext` timeout — the missed-wake backstop.
///
/// The thread exits cleanly when `ctx.stop` is set to true.
fn wakeThreadFn(ctx: *WakeThreadCtx) void {
    var w = wake_mod.Wake.init(ctx.allocator, ctx.db_path) catch return;
    defer w.close();

    while (!ctx.stop.load(.seq_cst)) {
        const ev = w.waitNext(WAKE_HEARTBEAT_NS) catch break;
        switch (ev) {
            .wal_changed, .heartbeat => {
                // Post db_changed for BOTH wake events (task 3961: heartbeat
                // is the coalesced/missed-wake backstop).
                ctx.loop.postEvent(.db_changed) catch {};
            },
            .interrupted => {
                // SIGINT or EINTR — check the stop flag and re-enter.
            },
        }
    }
}

/// Run the cockpit shell with full DB and wake integration.
///
/// `db_path` is the absolute path to the Planar SQLite DB. Call
/// `openDb` first to surface any DB open failures before entering the
/// alt-screen.
///
/// `db_handle` must already be open; the caller retains ownership and
/// must close it after `run` returns.
pub fn run(
    io: std.Io,
    alloc: std.mem.Allocator,
    env_map: *std.process.Environ.Map,
    db_path: []const u8,
    db_handle: *db.sqlite.Db,
) !void {
    // 4 KiB write buffer for the TTY.
    var tty_buf: [4096]u8 = undefined;
    var tty = try vaxis.Tty.init(io, &tty_buf);
    defer tty.deinit();

    var vx = try vaxis.init(io, alloc, env_map, .{});
    defer vx.deinit(alloc, tty.writer());

    var loop: vaxis.Loop(Event) = .init(io, &tty, &vx);
    try loop.installResizeHandler();
    try loop.start();
    defer loop.stop();

    // ---- Wake thread (tasks 4013 + 3961) --------------------------------
    var wake_ctx: WakeThreadCtx = .{
        .loop = &loop,
        .db_path = db_path,
        .stop = .init(false),
        .allocator = alloc,
    };
    const wake_thread = try std.Thread.spawn(.{}, wakeThreadFn, .{&wake_ctx});
    defer {
        wake_ctx.stop.store(true, .seq_cst);
        wake_thread.join();
    }

    // ---- Scope Explorer state (task 3966, M3 default landing view) ------
    var explorer: scope_explorer.ExplorerState = scope_explorer.ExplorerState.init(alloc);
    defer explorer.deinit();

    // Determine cwd-scope filter on launch. Falls back to .all when the
    // cwd is outside any registered repo (q607: cwd-derived default).
    const cwd_for_scope = std.Io.Dir.realPathFileAlloc(
        std.Io.Dir.cwd(),
        io,
        ".",
        alloc,
    ) catch null;
    if (cwd_for_scope) |cwd| {
        defer alloc.free(cwd);
        if (view_model.cwdScopeProjectId(db_handle, alloc, cwd)) |pid| {
            explorer.filter = .{ .repo = pid };
            explorer.filter_is_cwd = true;
        }
    }

    // Initial load of the Scope Explorer tree.
    try explorer.reload(db_handle);

    // ---- Spine state ----------------------------------------------------
    // Scope Explorer is the default (index 0) landing view per the spec.
    var vs: view_switcher.ViewSwitcher = .{};
    try vs.register(.{ .id = .scope_explorer, .name = "Explorer", .key = '1' });
    try vs.register(.{ .id = .agent_monitor, .name = "Monitor", .key = '2' });
    try vs.register(.{ .id = .task_board, .name = "Tasks", .key = '3' });

    var sl: split_layout.SplitLayout = .{};

    // Enter the alternate screen.
    try vx.enterAltScreen(tty.writer());
    try vx.queryTerminal(tty.writer(), .fromSeconds(1));

    // Render the initial frame.
    try renderFrame(&vx, tty.writer(), alloc, &vs, &sl, &explorer);

    // Main event loop.
    while (true) {
        const event = try loop.nextEvent();
        var need_render = true;
        switch (event) {
            .key_press => |key| {
                // Global quit.
                if (key.matches('q', .{}) or key.matches('c', .{ .ctrl = true })) {
                    break;
                }
                // View-switcher keys (Tab/Shift-Tab/1-9).
                if (vs.handleKey(key)) {
                    // View switched — render.
                } else if (key.matches(Key.tab, .{})) {
                    sl.toggleFocus();
                } else {
                    // Route keys to the active view.
                    const active = vs.active();
                    if (active != null and active.?.id == .scope_explorer) {
                        if (explorer.handleKey(key, db_handle)) {
                            // Consumed by explorer — render.
                        } else {
                            need_render = false;
                        }
                    } else {
                        need_render = false;
                    }
                }
            },
            .winsize => |ws| {
                try vx.resize(alloc, tty.writer(), ws);
            },
            .db_changed => {
                // Re-query the active view's data on WAL change or heartbeat.
                // Task 4055: this is the live wake-redraw path exercised end-to-end.
                const active = vs.active();
                if (active != null and active.?.id == .scope_explorer) {
                    explorer.reload(db_handle) catch {};
                }
            },
        }
        if (need_render) {
            try renderFrame(&vx, tty.writer(), alloc, &vs, &sl, &explorer);
        }
    }
    // Terminal restored by deferred vx.deinit.
}

/// Run the cockpit shell without a DB. Kept for backwards-compatibility
/// with the M1 explore handler path (now superseded by `run` in M3).
/// Integration tests that need a no-TTY path exercise the gate check
/// path instead; this function exists so the symbol compiles cleanly.
pub fn runWithoutDb(
    io: std.Io,
    alloc: std.mem.Allocator,
    env_map: *std.process.Environ.Map,
) !void {
    var tty_buf: [4096]u8 = undefined;
    var tty = try vaxis.Tty.init(io, &tty_buf);
    defer tty.deinit();

    var vx = try vaxis.init(io, alloc, env_map, .{});
    defer vx.deinit(alloc, tty.writer());

    var loop: vaxis.Loop(Event) = .init(io, &tty, &vx);
    try loop.installResizeHandler();
    try loop.start();
    defer loop.stop();

    try vx.enterAltScreen(tty.writer());
    try vx.queryTerminal(tty.writer(), .fromSeconds(1));

    var vs: view_switcher.ViewSwitcher = .{};
    try vs.register(.{ .id = .scope_explorer, .name = "Explorer", .key = '1' });
    var sl: split_layout.SplitLayout = .{};

    // Render a placeholder frame (no DB data available).
    try renderFrameNoDb(&vx, tty.writer(), alloc, &vs, &sl);

    while (true) {
        const event = try loop.nextEvent();
        switch (event) {
            .key_press => |key| {
                if (key.matches('q', .{}) or key.matches('c', .{ .ctrl = true })) break;
                _ = vs.handleKey(key);
            },
            .winsize => |ws| {
                try vx.resize(alloc, tty.writer(), ws);
            },
            .db_changed => {},
        }
        try renderFrameNoDb(&vx, tty.writer(), alloc, &vs, &sl);
    }
}

/// Render one frame of the cockpit UI with live Scope Explorer data.
///
/// Minimum-size guard (task 3965): when the terminal is below
/// MIN_WIDTH×MIN_HEIGHT, render "Terminal too small" notice only.
fn renderFrame(
    vx: *Vaxis,
    tty_writer: *std.Io.Writer,
    alloc: std.mem.Allocator,
    vs: *const view_switcher.ViewSwitcher,
    sl: *const split_layout.SplitLayout,
    explorer: *const scope_explorer.ExplorerState,
) !void {
    const win = vx.window();
    win.clear();

    // ---- Minimum size guard (task 3965) --------------------------------
    if (win.width < MIN_WIDTH or win.height < MIN_HEIGHT) {
        renderTooSmall(win);
        try vx.render(tty_writer);
        return;
    }

    // ---- Tab bar (view-switcher chrome, task 3963) ---------------------
    const tab_bar_win = win.child(.{
        .x_off = 0,
        .y_off = 0,
        .width = win.width,
        .height = 1,
    });
    vs.renderTabBar(tab_bar_win);

    // ---- Content area (below the tab bar) -----------------------------
    if (win.height < 2) {
        try vx.render(tty_writer);
        return;
    }
    const content_win = win.child(.{
        .x_off = 0,
        .y_off = 1,
        .width = win.width,
        .height = win.height - 1,
    });

    // ---- Key legend bar (bottom row of content) -----------------------
    // Show scope label for the Explorer view, generic legend for others.
    var scope_buf: [32]u8 = undefined;
    const legend_row: u16 = content_win.height -| 1;
    const active = vs.active();
    if (active != null and active.?.id == .scope_explorer) {
        const scope_lbl = scope_explorer.scopeLabel(explorer, &scope_buf);
        var legend_buf: [128]u8 = undefined;
        const legend = std.fmt.bufPrint(
            &legend_buf,
            "  q Quit  j/k Move  Enter Expand  a All-scopes  Tab Focus  [{s}]",
            .{scope_lbl},
        ) catch "  q Quit  j/k Move  Enter Expand  a All-scopes  Tab Focus";
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else {
        _ = content_win.printSegment(.{
            .text = "  q Quit  Tab Focus  1-3 View  Ctrl-C Quit",
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    }

    // ---- Split layout (task 3959) -------------------------------------
    if (content_win.height < 2) {
        try vx.render(tty_writer);
        return;
    }
    const body_win = content_win.child(.{
        .x_off = 0,
        .y_off = 0,
        .width = content_win.width,
        .height = content_win.height -| 1,
    });

    const panes = sl.splitWindow(body_win);
    sl.drawFocusBorder(panes.nav, panes.detail);

    // ---- Route content rendering to the active view -------------------
    if (active != null and active.?.id == .scope_explorer) {
        try scope_explorer.render(explorer, panes.nav, panes.detail, alloc);
    } else {
        // Placeholder for views not yet implemented (M4+).
        _ = panes.nav.printSegment(.{
            .text = "[Navigator — M4+]",
            .style = .{ .dim = true },
        }, .{ .row_offset = 1, .col_offset = 1 });
        _ = panes.detail.printSegment(.{
            .text = "[Detail pane — M4+]",
            .style = .{ .dim = true },
        }, .{ .row_offset = 1, .col_offset = 1 });
    }

    try vx.render(tty_writer);
}

/// Render one frame of the no-DB cockpit (backward-compat path).
fn renderFrameNoDb(
    vx: *Vaxis,
    tty_writer: *std.Io.Writer,
    _: std.mem.Allocator,
    vs: *const view_switcher.ViewSwitcher,
    sl: *const split_layout.SplitLayout,
) !void {
    const win = vx.window();
    win.clear();

    if (win.width < MIN_WIDTH or win.height < MIN_HEIGHT) {
        renderTooSmall(win);
        try vx.render(tty_writer);
        return;
    }

    const tab_bar_win = win.child(.{
        .x_off = 0,
        .y_off = 0,
        .width = win.width,
        .height = 1,
    });
    vs.renderTabBar(tab_bar_win);

    if (win.height < 2) {
        try vx.render(tty_writer);
        return;
    }
    const content_win = win.child(.{
        .x_off = 0,
        .y_off = 1,
        .width = win.width,
        .height = win.height - 1,
    });

    const legend_row: u16 = content_win.height -| 1;
    _ = content_win.printSegment(.{
        .text = "  q Quit  Tab Focus  Ctrl-C Quit",
        .style = .{ .dim = true },
    }, .{ .row_offset = legend_row, .col_offset = 0 });

    if (content_win.height < 2) {
        try vx.render(tty_writer);
        return;
    }
    const body_win = content_win.child(.{
        .x_off = 0,
        .y_off = 0,
        .width = content_win.width,
        .height = content_win.height -| 1,
    });

    const panes = sl.splitWindow(body_win);
    sl.drawFocusBorder(panes.nav, panes.detail);

    _ = panes.nav.printSegment(.{
        .text = "(no DB — run `planar init`)",
        .style = .{ .dim = true },
    }, .{ .row_offset = 1, .col_offset = 1 });

    try vx.render(tty_writer);
}

/// Show a "terminal too small" notice when the window is below usable size.
/// Task 3965: this replaces a broken multi-pane layout with a clear message.
fn renderTooSmall(win: Window) void {
    if (win.height == 0 or win.width == 0) return;
    const msg = "Terminal too small (need 40x8 minimum)";
    _ = win.printSegment(.{
        .text = msg,
        .style = .{},
    }, .{ .row_offset = 0, .col_offset = 0 });
}

// Pull every cockpit sub-module into the test build so their `test` blocks
// are covered under `zig build test` (exe test root = main.zig, which
// imports cockpit/app.zig). Without this, Zig lazy-eval would skip
// tree_navigator.zig and markdown_detail.zig entirely.
const tree_navigator = @import("widgets/tree_navigator.zig");
const markdown_detail = @import("widgets/markdown_detail.zig");
const scope_explorer_mod = @import("views/scope_explorer.zig");

// =========================================================================
// Tests
// =========================================================================

test "cockpit app: MIN_WIDTH and MIN_HEIGHT constants" {
    // Pin the minimum-size threshold so reviewers can see it changed.
    try std.testing.expectEqual(@as(u16, 40), MIN_WIDTH);
    try std.testing.expectEqual(@as(u16, 8), MIN_HEIGHT);
}

test "cockpit app: WAKE_HEARTBEAT_NS is ≤1 second" {
    // The spec says the heartbeat is ≤1s; verify the constant.
    try std.testing.expect(WAKE_HEARTBEAT_NS <= std.time.ns_per_s);
}

test "cockpit app: openDb returns DbMissing for nonexistent path" {
    const result = openDb(std.testing.io, "/nonexistent/path/to/planar.db", std.testing.allocator);
    try std.testing.expectError(DbOpenError.DbMissing, result);
}

test "cockpit app: openDb returns DbSchemaMismatch for schema-ahead DB" {
    // Create a DB, apply migrations, then manually bump the schema version
    // beyond the embedded max to simulate a schema-ahead condition.
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const dir_path = try std.fs.path.join(
        std.testing.allocator,
        &.{ ".zig-cache/tmp", &tmp.sub_path },
    );
    defer std.testing.allocator.free(dir_path);
    const db_path = try std.fs.path.join(std.testing.allocator, &.{ dir_path, "ahead.db" });
    defer std.testing.allocator.free(db_path);

    const db_path_z = try std.testing.allocator.dupeZ(u8, db_path);
    defer std.testing.allocator.free(db_path_z);

    var raw_db = try db.sqlite.Db.open(db_path_z.ptr);
    try db.migrate.applyAll(&raw_db, std.testing.allocator);
    // Insert a fake future version to force SchemaVersionAhead.
    _ = raw_db.execParams(
        "insert into schema_migrations (version, description) values (99999, 'future')",
        &.{},
    ) catch {};
    raw_db.close();

    const result = openDb(std.testing.io, db_path, std.testing.allocator);
    try std.testing.expectError(DbOpenError.DbSchemaMismatch, result);
}

test "cockpit app: openDb opens a real in-memory-backed file" {
    // Use a temp dir to create a real SQLite file that openDb can open.
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    // Resolve the tmp path.
    const dir_path = try std.fs.path.join(
        std.testing.allocator,
        &.{ ".zig-cache/tmp", &tmp.sub_path },
    );
    defer std.testing.allocator.free(dir_path);
    const db_path = try std.fs.path.join(std.testing.allocator, &.{ dir_path, "test.db" });
    defer std.testing.allocator.free(db_path);

    // Pre-create the file and apply migrations via a raw open first.
    const db_path_z = try std.testing.allocator.dupeZ(u8, db_path);
    defer std.testing.allocator.free(db_path_z);
    var raw_db = try db.sqlite.Db.open(db_path_z.ptr);
    try db.migrate.applyAll(&raw_db, std.testing.allocator);
    raw_db.close();

    // Now openDb should succeed.
    var opened = try openDb(std.testing.io, db_path, std.testing.allocator);
    defer opened.close();
    // Verified: db opened and schema validated.
}

test "cockpit app: WakeThreadCtx stop flag is atomic" {
    // Verify that the stop flag compiles and can be set/read atomically.
    var ctx: WakeThreadCtx = .{
        .loop = undefined,
        .db_path = "/tmp/test.db",
        .stop = .init(false),
        .allocator = std.testing.allocator,
    };
    try std.testing.expect(!ctx.stop.load(.seq_cst));
    ctx.stop.store(true, .seq_cst);
    try std.testing.expect(ctx.stop.load(.seq_cst));
}

test "cockpit app: view_switcher default landing is scope_explorer" {
    // Task 3966: Scope Explorer must be the first registered view (default
    // landing) per the spec decision "The Scope Explorer is the default
    // landing view."
    var vs: view_switcher.ViewSwitcher = .{};
    try vs.register(.{ .id = .scope_explorer, .name = "Explorer", .key = '1' });
    try vs.register(.{ .id = .agent_monitor, .name = "Monitor", .key = '2' });
    const act = vs.active();
    try std.testing.expect(act != null);
    try std.testing.expectEqual(view_model.ViewId.scope_explorer, act.?.id);
}

test "cockpit app compiles" {
    std.testing.refAllDecls(@This());
}

// Ensure lazy-imported modules are pulled into the test build.
// This is the load-bearing coverage anchor: without these, Zig's
// lazy evaluation skips tree_navigator.zig and markdown_detail.zig.
test "cockpit spine modules compile" {
    std.testing.refAllDecls(tree_navigator);
    std.testing.refAllDecls(markdown_detail);
    std.testing.refAllDecls(view_model);
    std.testing.refAllDecls(view_switcher);
    std.testing.refAllDecls(split_layout);
    std.testing.refAllDecls(scope_explorer_mod);
}
