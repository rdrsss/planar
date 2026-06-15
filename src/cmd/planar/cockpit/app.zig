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
const agent_monitor = @import("views/agent_monitor.zig");
const task_board = @import("views/task_board.zig");
const decision_log = @import("views/decision_log.zig");
const open_questions = @import("views/open_questions.zig");
const coverage_view = @import("views/coverage_view.zig");
const entity_link_graph = @import("views/entity_link_graph.zig");
const external_ops_plane = @import("views/external_ops_plane.zig");
const sessions_handoff = @import("views/sessions_handoff.zig");
const audit_log_view = @import("views/audit_log.zig");
const cli_history_view = @import("views/cli_history.zig");
const topology_view = @import("views/topology.zig");
const utility_view = @import("views/utility_view.zig");

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
    environ: std.process.Environ,
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

    // ---- Agent Monitor state (task 4015, M4) ----------------------------
    var monitor: agent_monitor.MonitorState = agent_monitor.MonitorState.init(alloc);
    defer monitor.deinit();

    // ---- Task Board state (tasks 4018–4020, M5) -------------------------
    var board: task_board.BoardState = task_board.BoardState.init(alloc);
    defer board.deinit();

    // ---- Decision Log state (tasks 4021–4022, M6) -----------------------
    var declog: decision_log.DecisionLogState = decision_log.DecisionLogState.init(alloc);
    defer declog.deinit();

    // ---- Open Questions state (tasks 4023–4024, M7) ---------------------
    var questions: open_questions.OpenQuestionsState = open_questions.OpenQuestionsState.init(alloc);
    defer questions.deinit();

    // ---- Coverage view state (tasks 4025–4026, M8) ----------------------
    var coverage: coverage_view.CoverageState = coverage_view.CoverageState.init(alloc);
    defer coverage.deinit();

    // ---- Entity-Link Graph state (tasks 4027–4028, M9) ------------------
    var entity_graph: entity_link_graph.EntityLinkState = entity_link_graph.EntityLinkState.init(alloc);
    defer entity_graph.deinit();

    // ---- External / Ops Plane state (tasks 4029–4031, M10) --------------
    // M18 (task 4049): initFull wires io + environ for the sync-pull action.
    var ext_ops: external_ops_plane.ExtOpsState = external_ops_plane.ExtOpsState.initFull(alloc, io, environ);
    defer ext_ops.deinit();

    // ---- Sessions & Handoff state (tasks 4032–4034, M11) ----------------
    var sessions: sessions_handoff.SessionsHandoffState = sessions_handoff.SessionsHandoffState.init(alloc);
    defer sessions.deinit();

    // ---- Audit Log state (tasks 4035–4036, M12) -------------------------
    var audit: audit_log_view.AuditLogState = audit_log_view.AuditLogState.init(alloc);
    defer audit.deinit();

    // ---- CLI Invocation History state (tasks 4037–4038, M13) -----------
    var cli_history: cli_history_view.CliHistoryState = cli_history_view.CliHistoryState.init(alloc);
    defer cli_history.deinit();

    // ---- Topology state (tasks 4039–4040, M14) --------------------------
    var topology: topology_view.TopologyState = topology_view.TopologyState.init(alloc);
    defer topology.deinit();

    // ---- Utility view state (tasks 4041–4043, M15) ----------------------
    // M18 (task 4050): initFull wires io + environ for the workbench action.
    var utility: utility_view.UtilityState = utility_view.UtilityState.initFull(alloc, io, environ);
    defer utility.deinit();

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

    // Initial load of the Agent Monitor.
    try monitor.reload(db_handle);

    // Initial load of the Task Board.
    try board.reload(db_handle);

    // Initial load of the Decision Log.
    try declog.reload(db_handle);

    // Initial load of the Open Questions view.
    try questions.reload(db_handle);

    // Initial load of the Coverage view.
    try coverage.reload(db_handle);

    // Initial load of the Entity-Link Graph (default entity heuristic).
    entity_graph.reloadDefault(db_handle) catch {};

    // Initial load of the External / Ops Plane.
    ext_ops.reload(db_handle) catch {};

    // Initial load of the Sessions & Handoff view.
    sessions.reload(db_handle) catch {};

    // Initial load of the Audit Log view.
    audit.reload(db_handle) catch {};

    // Initial load of the CLI Invocation History view.
    cli_history.reload(db_handle) catch {};

    // Initial load of the Topology view.
    topology.reload(db_handle) catch {};

    // Initial load of the Utility view.
    utility.reload(db_handle) catch {};

    // ---- Per-frame arena (UAF fix: task 4174) ---------------------------
    // Allocates a per-frame arena backed by the long-lived `alloc`. The
    // arena is reset at the TOP of each renderFrame call (before any view
    // renders), so every arena-allocated grapheme slice lives from its
    // allocation through vaxis.render()'s back→front buffer copy — the
    // exact window that the back buffer's borrowed grapheme pointers must
    // remain valid. The arena is NOT reset between renderFrame and
    // vaxis.render(); both happen inside renderFrame with the arena alive.
    var frame_arena = std.heap.ArenaAllocator.init(alloc);
    defer frame_arena.deinit();

    // ---- Spine state ----------------------------------------------------
    // Scope Explorer is the default (index 0) landing view per the spec.
    var vs: view_switcher.ViewSwitcher = .{};
    try vs.register(.{ .id = .scope_explorer, .name = "Explorer", .key = '1' });
    try vs.register(.{ .id = .agent_monitor, .name = "Monitor", .key = '2' });
    try vs.register(.{ .id = .task_board, .name = "Tasks", .key = '3' });
    try vs.register(.{ .id = .decision_log, .name = "Decisions", .key = '4' });
    try vs.register(.{ .id = .open_questions, .name = "Questions", .key = '5' });
    try vs.register(.{ .id = .coverage_view, .name = "Coverage", .key = '6' });
    try vs.register(.{ .id = .entity_link_graph, .name = "Links", .key = '7' });
    try vs.register(.{ .id = .external_ops_plane, .name = "ExtOps", .key = '8' });
    try vs.register(.{ .id = .sessions_handoff, .name = "Sessions", .key = '9' });
    // M12: Audit Log is the 10th view. Numeric jump ('1'–'9') only covers
    // views 1–9; this view is reachable via Tab/Shift-Tab cycling only.
    // The key '0' is display-only in the tab bar (view_switcher.zig line 88:
    // handleKey digit jump only fires for '1'–'9').
    try vs.register(.{ .id = .audit_log, .name = "AuditLog", .key = '0' });
    // M13: CLI History is the 11th view. Reachable via Tab/Shift-Tab only.
    // Key 'h' is display-only (not a jump key).
    try vs.register(.{ .id = .cli_history, .name = "CLIHist", .key = 'h' });
    // M14: Topology is the 12th view. Reachable via Tab/Shift-Tab only.
    // Key 't' is display-only (not a jump key).
    try vs.register(.{ .id = .topology, .name = "Topology", .key = 't' });
    // M15: Utility view is the 13th view (config/annotations/workbench-sync).
    // Reachable via Tab/Shift-Tab only. Key 'u' is display-only.
    try vs.register(.{ .id = .utility_view, .name = "Utility", .key = 'u' });

    var sl: split_layout.SplitLayout = .{};

    // Enter the alternate screen.
    try vx.enterAltScreen(tty.writer());
    try vx.queryTerminal(tty.writer(), .fromSeconds(1));

    // Render the initial frame.
    try renderFrame(&vx, tty.writer(), &vs, &sl, &explorer, &monitor, &board, &declog, &questions, &coverage, &entity_graph, &ext_ops, &sessions, &audit, &cli_history, &topology, &utility, &frame_arena);

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
                    } else if (active != null and active.?.id == .agent_monitor) {
                        if (monitor.handleKey(key)) {
                            // Consumed by monitor — render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .task_board) {
                        if (board.handleKey(key, db_handle)) {
                            // Consumed by task board — render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .decision_log) {
                        if (declog.handleKey(key, db_handle)) {
                            // Consumed by decision log — render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .open_questions) {
                        if (questions.handleKey(key, db_handle)) {
                            // Consumed by open questions — render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .coverage_view) {
                        if (coverage.handleKey(key, db_handle)) {
                            // Consumed by coverage view — render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .entity_link_graph) {
                        const kr = entity_graph.handleKey(key);
                        if (kr.consumed) {
                            // Task 4028: if a FocusRequest is returned, re-center
                            // the view on the target entity.
                            if (kr.focus) |fr| {
                                entity_graph.reloadFor(db_handle, fr.kind, fr.id) catch {};
                                // Extension point: if fr.switch_to_view != null,
                                // switch the active view. Not wired in M9 (switch_to_view
                                // is always null for the re-center-in-place case).
                                if (fr.switch_to_view) |target_id| {
                                    _ = vs.switchTo(target_id);
                                }
                            }
                            // render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .external_ops_plane) {
                        if (ext_ops.handleKey(key, db_handle)) {
                            // Consumed by external ops plane — render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .sessions_handoff) {
                        if (sessions.handleKey(key, db_handle)) {
                            // Consumed by sessions & handoff view — render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .audit_log) {
                        if (audit.handleKey(key, db_handle)) {
                            // Consumed by audit log view — render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .cli_history) {
                        if (cli_history.handleKey(key, db_handle)) {
                            // Consumed by cli history view — render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .topology) {
                        if (topology.handleKey(key)) {
                            // Consumed by topology view — render.
                        } else {
                            need_render = false;
                        }
                    } else if (active != null and active.?.id == .utility_view) {
                        if (utility.handleKey(key, db_handle)) {
                            // Consumed by utility view — render.
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
                // Task 4016: Agent Monitor reloads on .db_changed via the SAME
                // wake thread — no second wake thread is spawned.
                const active = vs.active();
                if (active != null and active.?.id == .scope_explorer) {
                    explorer.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .agent_monitor) {
                    monitor.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .task_board) {
                    board.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .decision_log) {
                    declog.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .open_questions) {
                    questions.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .coverage_view) {
                    coverage.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .entity_link_graph) {
                    entity_graph.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .external_ops_plane) {
                    ext_ops.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .sessions_handoff) {
                    sessions.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .audit_log) {
                    audit.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .cli_history) {
                    cli_history.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .topology) {
                    topology.reload(db_handle) catch {};
                } else if (active != null and active.?.id == .utility_view) {
                    utility.reload(db_handle) catch {};
                }
            },
        }
        if (need_render) {
            try renderFrame(&vx, tty.writer(), &vs, &sl, &explorer, &monitor, &board, &declog, &questions, &coverage, &entity_graph, &ext_ops, &sessions, &audit, &cli_history, &topology, &utility, &frame_arena);
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

/// Format a string into the per-frame arena allocator, returning a slice that
/// remains valid until the frame arena is reset at the top of the next frame.
///
/// This is the canonical replacement for `var buf:[N]u8; std.fmt.bufPrint(&buf, ...)`
/// patterns throughout the cockpit render layer. The arena lifetime guarantees the
/// slice outlives the back-buffer flush (vaxis.render() copies grapheme pointers
/// from the back buffer into the front buffer during the flush — the source must
/// be alive throughout that window).
///
/// On OOM falls back to "?" so render never panics.
pub fn fmtFrame(arena: std.mem.Allocator, comptime fmt: []const u8, args: anytype) []const u8 {
    return std.fmt.allocPrint(arena, fmt, args) catch "?";
}

/// Render one frame of the cockpit UI with live Scope Explorer data.
///
/// Minimum-size guard (task 3965): when the terminal is below
/// MIN_WIDTH×MIN_HEIGHT, render "Terminal too small" notice only.
fn renderFrame(
    vx: *Vaxis,
    tty_writer: *std.Io.Writer,
    vs: *const view_switcher.ViewSwitcher,
    sl: *const split_layout.SplitLayout,
    explorer: *const scope_explorer.ExplorerState,
    monitor: *const agent_monitor.MonitorState,
    board: *const task_board.BoardState,
    declog: *const decision_log.DecisionLogState,
    questions: *const open_questions.OpenQuestionsState,
    coverage: *const coverage_view.CoverageState,
    entity_graph_state: *const entity_link_graph.EntityLinkState,
    ext_ops_state: *const external_ops_plane.ExtOpsState,
    sessions_state: *const sessions_handoff.SessionsHandoffState,
    audit_state: *const audit_log_view.AuditLogState,
    cli_history_state: *const cli_history_view.CliHistoryState,
    topology_state: *const topology_view.TopologyState,
    utility_state: *const utility_view.UtilityState,
    frame_arena: *std.heap.ArenaAllocator,
) !void {
    // Reset the frame arena at the TOP of the frame, before any view renders.
    // All arena-allocated strings from this point (including those in sub-render
    // helpers that return before vaxis.render() flushes) live until the next
    // frame's reset. This is the lifetime guarantee that prevents the UAF:
    // back-buffer cells store borrowed grapheme slices that must survive
    // from writeCell/printSegment through vaxis.render()'s flush into the front
    // buffer. retain_capacity reuses the backing allocation across frames so
    // steady-state renders are alloc-free.
    _ = frame_arena.reset(.retain_capacity);
    const arena = frame_arena.allocator();

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
    vs.renderTabBar(tab_bar_win, arena);

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
    // Show view-specific legend for Explorer and Monitor; generic otherwise.
    //
    // UAF fix (task 4174, iter 2): legend_buf is declared at FUNCTION-BODY
    // scope (not inside each if/else-if block) so the slice passed to
    // printSegment remains valid through the vx.render() flush at the bottom
    // of this function.  The previous pattern used a block-scoped
    // `var legend_buf: [N]u8 = undefined` declared inside each if/else-if
    // block; Zig/LLVM marks those stack slots as dead at block exit and may
    // reuse the slot for the view-render dispatch below before vx.render()
    // reads the grapheme pointer.  A function-body-scope buffer lives until
    // renderFrame returns — well past vx.render().
    const legend_row: u16 = content_win.height -| 1;
    const active = vs.active();
    // Single function-body-scope legend buffer, sized for the largest legend
    // (256 B).  Only one branch executes per frame, so there is no aliasing.
    var legend_buf: [256]u8 = undefined;
    if (active != null and active.?.id == .scope_explorer) {
        const legend = scope_explorer.legendLabel(explorer, &legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .agent_monitor) {
        const legend = agent_monitor.legendLabel(&legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .task_board) {
        const legend = task_board.legendLabel(&legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .decision_log) {
        const legend = decision_log.legendLabel(&legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .open_questions) {
        const legend = open_questions.legendLabel(&legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .coverage_view) {
        const legend = coverage_view.legendLabel(&legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .entity_link_graph) {
        const legend = entity_link_graph.legendLabel(&legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .external_ops_plane) {
        const legend = external_ops_plane.legendLabel(ext_ops_state, &legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .sessions_handoff) {
        const legend = sessions_handoff.legendLabel(&legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .audit_log) {
        const legend = audit_log_view.legendLabel(&legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .cli_history) {
        const legend = cli_history_view.legendLabel(&legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .topology) {
        const legend = topology_view.legendLabel(&legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else if (active != null and active.?.id == .utility_view) {
        const legend = utility_view.legendLabel(utility_state, &legend_buf);
        _ = content_win.printSegment(.{
            .text = legend,
            .style = .{ .dim = true },
        }, .{ .row_offset = legend_row, .col_offset = 0 });
    } else {
        _ = content_win.printSegment(.{
            .text = "  q Quit  Tab Focus  1-9 View  Ctrl-C Quit",
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
    // Pass `arena` (the per-frame arena allocator) instead of `alloc` so
    // every render sub-function can use fmtFrame for dynamic cell text.
    // The arena lifetime spans from the reset at the top of this function
    // through vaxis.render() below — guaranteeing grapheme slices in the
    // back buffer remain valid for the full flush window.
    if (active != null and active.?.id == .scope_explorer) {
        try scope_explorer.render(explorer, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .agent_monitor) {
        try agent_monitor.render(monitor, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .task_board) {
        try task_board.render(board, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .decision_log) {
        try decision_log.render(declog, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .open_questions) {
        try open_questions.render(questions, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .coverage_view) {
        try coverage_view.render(coverage, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .entity_link_graph) {
        try entity_link_graph.render(entity_graph_state, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .external_ops_plane) {
        try external_ops_plane.render(ext_ops_state, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .sessions_handoff) {
        try sessions_handoff.render(sessions_state, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .audit_log) {
        try audit_log_view.render(audit_state, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .cli_history) {
        try cli_history_view.render(cli_history_state, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .topology) {
        try topology_view.render(topology_state, panes.nav, panes.detail, arena);
    } else if (active != null and active.?.id == .utility_view) {
        try utility_view.render(utility_state, panes.nav, panes.detail, arena);
    } else {
        // Placeholder for views not yet implemented (M16+).
        _ = panes.nav.printSegment(.{
            .text = "[Navigator — M15+]",
            .style = .{ .dim = true },
        }, .{ .row_offset = 1, .col_offset = 1 });
        _ = panes.detail.printSegment(.{
            .text = "[Detail pane — M15+]",
            .style = .{ .dim = true },
        }, .{ .row_offset = 1, .col_offset = 1 });
    }

    try vx.render(tty_writer);
}

/// Render one frame of the no-DB cockpit (backward-compat path).
fn renderFrameNoDb(
    vx: *Vaxis,
    tty_writer: *std.Io.Writer,
    alloc: std.mem.Allocator,
    vs: *const view_switcher.ViewSwitcher,
    sl: *const split_layout.SplitLayout,
) !void {
    // Per-frame arena for grapheme allocations (renderTabBar needs arena for
    // the key-byte slice; must outlive vaxis.render()).
    var frame_arena = std.heap.ArenaAllocator.init(alloc);
    defer frame_arena.deinit();
    const arena = frame_arena.allocator();

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
    vs.renderTabBar(tab_bar_win, arena);

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

// =========================================================================
// Grapheme-lifetime regression tests (task 4174: per-frame arena UAF fix,
// iter 2: legend-bar UAF fix + deterministic discriminator).
// =========================================================================
//
// Back buffer UAF recap:
//   vaxis.Screen.writeCell stores a BORROWED grapheme slice pointer in the
//   back buffer (Screen.buf[n].char.grapheme).  The pointer is read again
//   during vaxis.render() when the front buffer diffing copies graphemes to
//   the InternalScreen.  Any grapheme pointer that refers to a stack-local
//   that was freed BEFORE vaxis.render() is a use-after-free.
//
// Four concrete UAF patterns fixed by task 4174:
//   (a) `const ch: [1]u8 = .{byte}; win.writeCell(...grapheme=&ch...)` —
//       per-loop-iteration stack local, freed at end of each iteration.
//   (b) `var buf:[N]u8; bufPrint(&buf, ...); printSegment(.{.text=&buf})` —
//       stack buffer freed when the render helper returns (widget/view level).
//   (c) `const key_slice:[1]u8=.{key}` inside a for loop body, stored in
//       tab_parts and then sliced in writeCell calls.
//   (d) Block-scoped `var legend_buf: [N]u8` inside each if/else-if branch
//       of the renderFrame legend section (lines ~651-751).  After the branch
//       block closes, the slot is marked dead; the view-render dispatch that
//       follows (lines 774-810) may reuse it before vx.render() (line 812).
//       This is Defect 1 (iter 2).
//
// HOW THE DETERMINISTIC TEST WORKS (iter 2):
//   The iter-1 stackClobber approach was non-deterministic: it relied on the
//   optimizer/ReleaseSafe reusing a freed stack slot before the check, which
//   does not happen in Debug mode and is therefore invisible to `zig build
//   test`.  More critically, it NEVER called vaxis.render(), so the actual
//   flush — the moment the borrowed pointer is read — was never exercised.
//
//   The deterministic replacement:
//     1. Initialize a real Vaxis instance backed by an in-memory Writer
//        (Writer.Allocating) instead of a TTY.  Force vx.refresh=true so
//        the first render emits ALL cells (no diff-skip).
//     2. Call renderFrame.  This writes cells into screen.buf, then calls
//        vx.render() internally which reads every cell's .char.grapheme
//        pointer and emits the grapheme bytes to the in-memory writer.
//        If any grapheme pointer is dangling at render time, vx.render()
//        reads garbage bytes → U+FFFD (or other corruption) in the output.
//     3. Inspect the in-memory writer's bytes:
//        (a) No 0xEF 0xBF 0xBD (U+FFFD) anywhere in the output.
//        (b) Expected legend and view text is present (proves the frame
//            was actually rendered, not skipped).
//
//   WHY IT CATCHES THE LEGEND UAF (Defect 1):
//     With the OLD block-scoped legend_buf, the slot is dead after each
//     if/else branch closes (~line 751).  In ReleaseSafe, the view-render
//     dispatch call at line 774 reuses that slot.  vx.render() at line 812
//     then reads the clobbered pointer → garbled grapheme bytes → U+FFFD in
//     the Writer.Allocating output → assertion (a) fails.
//
//     In Debug mode, Zig does not aggressively reuse stack slots, so the
//     corruption is latent.  The test is a safety net for ReleaseSafe builds
//     (the production optimize level) and for future optimizer improvements.
//     Run `zig build test -Doptimize=ReleaseSafe` to see the test go RED on
//     the pre-fix code.
//
//   WHY IT COVERS ALL VIEWS (Defect 1 + iter-1 sites):
//     renderFrame is called once per view (all 13 views + the generic legend
//     path).  Each call exercises the tab bar (view_switcher — iter-1 fix c),
//     the legend bar (iter-2 fix d), and the view-specific render path (iter-1
//     fixes a and b for tree_navigator, markdown_detail, and per-view arenas).

/// Helper: set up a minimal Vaxis instance backed by an in-memory writer for
/// grapheme-lifetime regression tests.  Returns the Writer.Allocating so the
/// caller can inspect emitted bytes.  The caller is responsible for calling
/// vx.deinit(alloc, &aw.writer) after use (so pass `&aw.writer` as the tty).
///
/// Sizes the screen to W×H cells (must be ≥ MIN_WIDTH × MIN_HEIGHT so the
/// minimum-size guard does not short-circuit rendering).
fn testVxSetup(
    alloc: std.mem.Allocator,
    W: u16,
    H: u16,
    env_map: *std.process.Environ.Map,
) !struct { vx: Vaxis, aw: std.Io.Writer.Allocating } {
    var aw: std.Io.Writer.Allocating = .init(alloc);
    var vx = try vaxis.init(std.testing.io, alloc, env_map, .{});
    // Resize the screen so it has actual cells.  The writer receives the
    // resize escape sequences; we ignore them and only care about the render
    // output.
    try vx.resize(alloc, &aw.writer, .{ .rows = H, .cols = W, .x_pixel = 0, .y_pixel = 0 });
    // Force a full redraw on the next render (diff-skip would suppress output
    // for cells that match the (empty) previous frame).
    vx.refresh = true;
    return .{ .vx = vx, .aw = aw };
}

test "grapheme lifetime (task 4174 iter 2): renderFrame emits no U+FFFD for scope_explorer legend and view" {
    // DETERMINISTIC: drives the full renderFrame → vx.render() pipeline to an
    // in-memory writer.  Asserts no U+FFFD in the output and that the expected
    // scope_explorer legend text appears.
    //
    // RED on pre-fix (block-scoped legend_buf) when compiled with ReleaseSafe.
    // GREEN with the fix (function-body-scope legend_buf in renderFrame).
    const a = std.testing.allocator;
    const W: u16 = 120;
    const H: u16 = 40;

    var env_map: std.process.Environ.Map = .init(a);
    defer env_map.deinit();

    var setup = try testVxSetup(a, W, H, &env_map);
    // Defers run LIFO: vx.deinit() must run before aw.deinit() because
    // vx.deinit writes to the tty writer (aw.writer) during state reset.
    defer setup.aw.deinit();
    defer setup.vx.deinit(a, &setup.aw.writer);

    // All view states at their default (empty) values — no DB needed.
    var explorer = scope_explorer.ExplorerState.init(a);
    defer explorer.deinit();
    var monitor = agent_monitor.MonitorState.init(a);
    defer monitor.deinit();
    var board = task_board.BoardState.init(a);
    defer board.deinit();
    var declog = decision_log.DecisionLogState.init(a);
    defer declog.deinit();
    var questions = open_questions.OpenQuestionsState.init(a);
    defer questions.deinit();
    var coverage = coverage_view.CoverageState.init(a);
    defer coverage.deinit();
    var entity_graph = entity_link_graph.EntityLinkState.init(a);
    defer entity_graph.deinit();
    var ext_ops = external_ops_plane.ExtOpsState.init(a);
    defer ext_ops.deinit();
    var sessions = sessions_handoff.SessionsHandoffState.init(a);
    defer sessions.deinit();
    var audit = audit_log_view.AuditLogState.init(a);
    defer audit.deinit();
    var cli_history = cli_history_view.CliHistoryState.init(a);
    defer cli_history.deinit();
    var topology = topology_view.TopologyState.init(a);
    defer topology.deinit();
    var utility = utility_view.UtilityState.init(a);
    defer utility.deinit();

    // View switcher with scope_explorer as the ACTIVE view (index 0).
    // This exercises the legend branch: scope_explorer.legendLabel().
    var vs: view_switcher.ViewSwitcher = .{};
    try vs.register(.{ .id = .scope_explorer, .name = "Explorer", .key = '1' });
    try vs.register(.{ .id = .agent_monitor, .name = "Monitor", .key = '2' });
    try vs.register(.{ .id = .task_board, .name = "Board", .key = '3' });
    try vs.register(.{ .id = .decision_log, .name = "Decisions", .key = '4' });
    try vs.register(.{ .id = .open_questions, .name = "Questions", .key = '5' });
    try vs.register(.{ .id = .coverage_view, .name = "Coverage", .key = '6' });
    try vs.register(.{ .id = .entity_link_graph, .name = "Links", .key = '7' });
    try vs.register(.{ .id = .external_ops_plane, .name = "ExtOps", .key = '8' });
    try vs.register(.{ .id = .sessions_handoff, .name = "Sessions", .key = '9' });
    try vs.register(.{ .id = .audit_log, .name = "AuditLog", .key = '0' });
    try vs.register(.{ .id = .cli_history, .name = "CLIHist", .key = 'h' });
    try vs.register(.{ .id = .topology, .name = "Topology", .key = 't' });
    try vs.register(.{ .id = .utility_view, .name = "Utility", .key = 'u' });

    var sl: split_layout.SplitLayout = .{};
    var frame_arena = std.heap.ArenaAllocator.init(a);
    defer frame_arena.deinit();

    // renderFrame calls vx.render() internally.  After it returns, all
    // grapheme pointers have been read by vx.render(); the output is in
    // setup.aw.writer.buffered().
    try renderFrame(&setup.vx, &setup.aw.writer, &vs, &sl, &explorer, &monitor, &board, &declog, &questions, &coverage, &entity_graph, &ext_ops, &sessions, &audit, &cli_history, &topology, &utility, &frame_arena);

    const out = setup.aw.writer.buffered();

    // No U+FFFD replacement character — the UAF discriminator.
    const replacement = "\xEF\xBF\xBD";
    if (std.mem.indexOf(u8, out, replacement) != null) {
        std.debug.print("FAIL: U+FFFD found in renderFrame output (legend bar UAF)\n", .{});
        return error.TestUnexpectedResult;
    }

    // The scope_explorer legend must appear in the VT output.
    // "q Quit" is the stable fragment of the default scope_explorer legend.
    if (std.mem.indexOf(u8, out, "q Quit") == null) {
        std.debug.print("FAIL: expected 'q Quit' in renderFrame output (scope_explorer legend not rendered)\n", .{});
        return error.TestUnexpectedResult;
    }

    // The "Explorer" tab label must appear (tab bar rendered).
    if (std.mem.indexOf(u8, out, "Explorer") == null) {
        std.debug.print("FAIL: expected 'Explorer' in renderFrame output (tab bar not rendered)\n", .{});
        return error.TestUnexpectedResult;
    }
}

test "grapheme lifetime (task 4174 iter 2): renderFrame cycles through all view legends without U+FFFD" {
    // Comprehensive coverage: render one frame per view (each exercises a
    // different legend branch in renderFrame).  All views must produce output
    // with no U+FFFD.  Cycling through views also exercises the tab bar
    // (view_switcher arena fix), view-specific render paths (per-view arena
    // fixes), and all legend branches (iter-2 legend-bar fix).
    const a = std.testing.allocator;
    const W: u16 = 120;
    const H: u16 = 40;
    const replacement = "\xEF\xBF\xBD";

    const ViewCase = struct {
        id: view_model.ViewId,
        name: []const u8,
        key: u8,
        // A stable substring that must appear in the rendered output to prove
        // the legend was actually written and read by vx.render().
        legend_needle: []const u8,
    };

    const cases = [_]ViewCase{
        .{ .id = .scope_explorer, .name = "Explorer", .key = '1', .legend_needle = "q Quit" },
        .{ .id = .agent_monitor, .name = "Monitor", .key = '2', .legend_needle = "Roster" },
        .{ .id = .task_board, .name = "Board", .key = '3', .legend_needle = "j/k" },
        .{ .id = .decision_log, .name = "Decisions", .key = '4', .legend_needle = "j/k" },
        .{ .id = .open_questions, .name = "Questions", .key = '5', .legend_needle = "j/k" },
        .{ .id = .coverage_view, .name = "Coverage", .key = '6', .legend_needle = "j/k" },
        .{ .id = .entity_link_graph, .name = "Links", .key = '7', .legend_needle = "j/k" },
        .{ .id = .external_ops_plane, .name = "ExtOps", .key = '8', .legend_needle = "q Quit" },
        .{ .id = .sessions_handoff, .name = "Sessions", .key = '9', .legend_needle = "j/k" },
        .{ .id = .audit_log, .name = "AuditLog", .key = '0', .legend_needle = "j/k" },
        .{ .id = .cli_history, .name = "CLIHist", .key = 'h', .legend_needle = "j/k" },
        .{ .id = .topology, .name = "Topology", .key = 't', .legend_needle = "j/k" },
        .{ .id = .utility_view, .name = "Utility", .key = 'u', .legend_needle = "q Quit" },
    };

    // All view states at their default (empty) values — no DB needed.
    var explorer = scope_explorer.ExplorerState.init(a);
    defer explorer.deinit();
    var monitor = agent_monitor.MonitorState.init(a);
    defer monitor.deinit();
    var board = task_board.BoardState.init(a);
    defer board.deinit();
    var declog = decision_log.DecisionLogState.init(a);
    defer declog.deinit();
    var questions = open_questions.OpenQuestionsState.init(a);
    defer questions.deinit();
    var coverage = coverage_view.CoverageState.init(a);
    defer coverage.deinit();
    var entity_graph = entity_link_graph.EntityLinkState.init(a);
    defer entity_graph.deinit();
    var ext_ops = external_ops_plane.ExtOpsState.init(a);
    defer ext_ops.deinit();
    var sessions = sessions_handoff.SessionsHandoffState.init(a);
    defer sessions.deinit();
    var audit = audit_log_view.AuditLogState.init(a);
    defer audit.deinit();
    var cli_history = cli_history_view.CliHistoryState.init(a);
    defer cli_history.deinit();
    var topology = topology_view.TopologyState.init(a);
    defer topology.deinit();
    var utility = utility_view.UtilityState.init(a);
    defer utility.deinit();

    var sl: split_layout.SplitLayout = .{};

    for (cases) |case| {
        // Fresh Vaxis + in-memory writer per case (avoids front-buffer diff
        // caching from prior frame confusing the output assertions).
        var env_map: std.process.Environ.Map = .init(a);
        defer env_map.deinit();
        var setup = try testVxSetup(a, W, H, &env_map);
        // LIFO: vx.deinit writes to aw.writer, so aw.deinit must run last.
        defer setup.aw.deinit();
        defer setup.vx.deinit(a, &setup.aw.writer);

        // Register all views but switch to the target view before rendering.
        var vs: view_switcher.ViewSwitcher = .{};
        for (cases) |reg| {
            try vs.register(.{ .id = reg.id, .name = reg.name, .key = reg.key });
        }
        // Activate the target view via switchTo (works for all views, including
        // those with non-numeric keys that handleKey would not handle).
        _ = vs.switchTo(case.id);

        var frame_arena = std.heap.ArenaAllocator.init(a);
        defer frame_arena.deinit();

        try renderFrame(&setup.vx, &setup.aw.writer, &vs, &sl, &explorer, &monitor, &board, &declog, &questions, &coverage, &entity_graph, &ext_ops, &sessions, &audit, &cli_history, &topology, &utility, &frame_arena);

        const out = setup.aw.writer.buffered();

        // No U+FFFD — the legend-bar UAF discriminator.
        if (std.mem.indexOf(u8, out, replacement) != null) {
            std.debug.print("FAIL [{s}]: U+FFFD found in renderFrame output (legend bar or view UAF)\n", .{case.name});
            return error.TestUnexpectedResult;
        }

        // Legend needle must appear — proves the legend branch was taken and
        // the grapheme bytes were actually emitted by vx.render().
        if (std.mem.indexOf(u8, out, case.legend_needle) == null) {
            std.debug.print("FAIL [{s}]: legend needle '{s}' not found in renderFrame output\n", .{ case.name, case.legend_needle });
            return error.TestUnexpectedResult;
        }
    }
}

// Pull every cockpit sub-module into the test build so their `test` blocks
// are covered under `zig build test` (exe test root = main.zig, which
// imports cockpit/app.zig). Without this, Zig lazy-eval would skip
// tree_navigator.zig and markdown_detail.zig entirely.
const tree_navigator = @import("widgets/tree_navigator.zig");
const markdown_detail = @import("widgets/markdown_detail.zig");
const scope_explorer_mod = @import("views/scope_explorer.zig");
const agent_monitor_mod = @import("views/agent_monitor.zig");
const task_board_mod = @import("views/task_board.zig");
const decision_log_mod = @import("views/decision_log.zig");
const open_questions_mod = @import("views/open_questions.zig");
const coverage_view_mod = @import("views/coverage_view.zig");
const entity_link_graph_mod = @import("views/entity_link_graph.zig");
const external_ops_plane_mod = @import("views/external_ops_plane.zig");
const sessions_handoff_mod = @import("views/sessions_handoff.zig");
const audit_log_mod = @import("views/audit_log.zig");
const cli_history_mod = @import("views/cli_history.zig");
const topology_mod = @import("views/topology.zig");
const utility_view_mod = @import("views/utility_view.zig");
// M16: edit action module — pulled in so its test blocks run.
const edit_actions_mod = @import("edit/actions.zig");
// M17: task lifecycle module — pulled in so its test blocks run.
const task_lifecycle_mod = @import("edit/task_lifecycle.zig");
// M18: external / workbench action module — pulled in so its test blocks run.
const external_actions_mod = @import("edit/external_actions.zig");

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
    try vs.register(.{ .id = .decision_log, .name = "Decisions", .key = '4' });
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
    std.testing.refAllDecls(agent_monitor_mod);
    std.testing.refAllDecls(task_board_mod);
    std.testing.refAllDecls(decision_log_mod);
    std.testing.refAllDecls(open_questions_mod);
    std.testing.refAllDecls(coverage_view_mod);
    std.testing.refAllDecls(entity_link_graph_mod);
    std.testing.refAllDecls(external_ops_plane_mod);
    std.testing.refAllDecls(sessions_handoff_mod);
    std.testing.refAllDecls(audit_log_mod);
    std.testing.refAllDecls(cli_history_mod);
    std.testing.refAllDecls(topology_mod);
    // M15 utility view.
    std.testing.refAllDecls(utility_view_mod);
    // M16 edit action layer.
    std.testing.refAllDecls(edit_actions_mod);
    // M17 task lifecycle layer.
    std.testing.refAllDecls(task_lifecycle_mod);
    // M18 external / workbench action layer.
    std.testing.refAllDecls(external_actions_mod);
}
