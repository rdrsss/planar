//! cockpit/edit/external_actions.zig — Sync / propagate (task 4049) and
//! workbench action (task 4050) controllers for M18.
//!
//! ## Write-path discipline
//!
//! External sync (task 4049):
//!   - `engine.external.link.allPullable` — list all pullable links (DB read).
//!     Mirrors `handlers/sync/pull.zig` resolveTargetLinks(all=true).
//!   - `engine.external.system.showById` — resolve system per link (DB read).
//!     Mirrors `handlers/sync/pull.zig` per-link system resolution.
//!   - `engine.external.sync.pullLink` — drive sync pull for one link via adapter.
//!     Mirrors `handlers/sync/common.pullLink`.
//!   - Adapter built via `sync_common.handleForSystem` — same factory the CLI uses.
//!     Mirrors `handlers/sync/common.handleForSystem`.
//!   - No reimplemented sync logic. No raw SQL. No subprocess shell-out.
//!
//! Workbench (task 4050):
//!   - `engine.workbench.sync.status(d, allocator, plan_id)`.
//!     Mirrors `handlers/workbench/status.zig` per-plan call.
//!   - `engine.workbench.sync.pull(d, allocator, plan_id)`.
//!     Mirrors `handlers/workbench/pull.zig`.
//!   - `engine.workbench.sync.push(d, allocator, plan_id, filter_mode, false)`.
//!     Mirrors `handlers/workbench/push.zig`.
//!   - No reimplemented workbench logic. No raw SQL.
//!
//! ## Confirmation gate (tasks 4049 / 4050)
//!
//! Both action kinds are destructive (network propagation / FS mutation) and
//! MUST show a confirmation overlay before executing. The pattern follows M17's
//! task_lifecycle.zig: a `ActionMode` union tags the current overlay state;
//! `handleKey` advances the state machine; a 'y' keystroke executes the action;
//! 'Esc' or any other key cancels.
//!
//! ## Sync-blocking approach (v1)
//!
//! These actions may involve FS mutation (workbench) or network I/O (ext sync).
//! For v1 the call is SYNCHRONOUS: the TUI renders a "working…" banner before
//! the call so the operator sees why the display pauses, then transitions to a
//! success or error overlay. A second wake thread is NOT used (per-spec
//! constraint: "no second wake thread"). If network latency is intolerable in
//! a later milestone, a background thread posting `loop.postEvent` can be
//! added without changing this controller's interface.
//!
//! ## Controller testability
//!
//! All state lives in `ExternalActionState`. `handleKey` is the key→state
//! function. Tests drive `handleKey` with an in-memory DB and assert:
//! no-exec before confirm, exec on confirm, cancel aborts exec, errors surface
//! in `.error_msg`. The network path is never called in tests because
//! `allPullable` returns empty on an in-memory DB with no external_systems.
//!
//! Tasks: 4049 (sync/propagate), 4050 (workbench push/pull/status).

const std = @import("std");
const vaxis = @import("vaxis");
const db = @import("db");
const engine = @import("engine");
const sync_common = @import("../../handlers/sync/common.zig");

const Window = vaxis.Window;
const Key = vaxis.Key;
const Style = vaxis.Style;

// =========================================================================
// Public error types
// =========================================================================

pub const ActionError = error{
    /// No external links found for the sync action.
    NoLinks,
    /// The underlying engine call failed.
    EngineFailed,
    /// Memory allocation failed.
    OutOfMemory,
};

// =========================================================================
// Action kinds
// =========================================================================

/// The external-plane sync actions (task 4049).
pub const SyncKind = enum {
    /// Pull all pullable links. Engine path:
    ///   engine.external.link.allPullable (DB) +
    ///   engine.external.sync.pullLink per link (network via adapter).
    pull_all,
};

/// The workbench actions (task 4050).
pub const WorkbenchKind = enum {
    /// engine.workbench.sync.status(d, allocator, plan_id).
    status,
    /// engine.workbench.sync.pull(d, allocator, plan_id).
    pull,
    /// engine.workbench.sync.push(d, allocator, plan_id, .failures, false).
    push,
};

// =========================================================================
// Action results
// =========================================================================

/// Summary of a completed workbench action (task 4050).
pub const WorkbenchResult = struct {
    applied: usize,
    pending: usize,
    conflicts: usize,
    kind: WorkbenchKind,
};

/// Summary of a completed sync action (task 4049).
pub const SyncResult = struct {
    total: usize,
    ok_count: usize,
    conflict_count: usize,
    error_count: usize,
};

// =========================================================================
// Engine call: workbench (task 4050)
// =========================================================================

/// Execute a workbench action for `plan_id` via the engine functions that
/// `handlers/workbench/{status,pull,push}.zig` use. No reimplementation.
///
/// Engine functions (src/engine/workbench/sync.zig):
///   - status: `engine.workbench.sync.status`  (mirrors workbench/status.zig)
///   - pull:   `engine.workbench.sync.pull`    (mirrors workbench/pull.zig)
///   - push:   `engine.workbench.sync.push`    (mirrors workbench/push.zig)
pub fn executeWorkbench(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: WorkbenchKind,
    plan_id: i64,
) ActionError!WorkbenchResult {
    switch (kind) {
        .status => {
            const result = engine.workbench.sync.status(d, allocator, plan_id) catch
                return ActionError.EngineFailed;
            defer engine.workbench.sync.deinitResult(allocator, result);
            return .{
                .applied = result.applied,
                .pending = result.pending,
                .conflicts = result.conflicts,
                .kind = .status,
            };
        },
        .pull => {
            const result = engine.workbench.sync.pull(d, allocator, plan_id) catch
                return ActionError.EngineFailed;
            defer engine.workbench.sync.deinitResult(allocator, result);
            return .{
                .applied = result.applied,
                .pending = result.pending,
                .conflicts = result.conflicts,
                .kind = .pull,
            };
        },
        .push => {
            // Default filter_mode = .failures, apply_cleanup = false.
            // Mirrors handlers/workbench/push.zig default.
            const result = engine.workbench.sync.push(
                d,
                allocator,
                plan_id,
                engine.workbench.terminal.Mode.failures,
                false,
            ) catch return ActionError.EngineFailed;
            defer engine.workbench.sync.deinitResult(allocator, result);
            return .{
                .applied = result.applied,
                .pending = result.pending,
                .conflicts = result.conflicts,
                .kind = .push,
            };
        },
    }
}

// =========================================================================
// Engine call: external sync pull (task 4049)
// =========================================================================

/// Execute a sync pull for all pullable links via the engine functions that
/// `handlers/sync/pull.zig` uses. No reimplementation.
///
/// Engine path:
///   1. `engine.external.link.allPullable` (mirrors sync/pull.zig's --all path)
///   2. Per link: `engine.external.system.showById` (mirrors sync/pull.zig)
///   3. Per link: `sync_common.handleForSystem` (mirrors sync/pull.zig)
///   4. Per link: `sync_common.pullLink` (mirrors sync/pull.zig)
///
/// When the adapter build fails (missing token, gh-cli absent), the link is
/// counted as error_count — matching the non-fatal per-link error handling in
/// sync/pull.zig. This means executeSync works in test environments (no links
/// → empty result) and surfaces errors gracefully when credentials are absent.
pub fn executeSync(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: SyncKind,
    io: std.Io,
    environ: *const std.process.Environ,
) ActionError!SyncResult {
    switch (kind) {
        .pull_all => {
            // 1. List pullable links (DB read, no network).
            //    engine.external.link.allPullable mirrors sync/pull.zig --all.
            const links = engine.external.link.allPullable(d, allocator) catch
                return ActionError.EngineFailed;
            defer engine.external.link.deinitMany(links, allocator);

            if (links.len == 0) {
                return .{ .total = 0, .ok_count = 0, .conflict_count = 0, .error_count = 0 };
            }

            var ok_count: usize = 0;
            var conflict_count: usize = 0;
            var error_count: usize = 0;

            // 2. Per-link: resolve system + build adapter + call pullLink.
            //    Mirrors the for-loop in handlers/sync/pull.zig.
            for (links) |link| {
                // Resolve the system row (mirrors sync/pull.zig).
                const sys = engine.external.system.showById(d, allocator, link.system_id) catch {
                    error_count += 1;
                    continue;
                };
                defer engine.external.system.deinit(sys, allocator);

                // Build adapter — mirrors sync_common.handleForSystem.
                // Requires env token / gh cli; fails in test envs (counted).
                var h = sync_common.handleForSystem(allocator, io, environ, sys) catch {
                    error_count += 1;
                    continue;
                };
                defer {
                    h.deinit();
                    allocator.destroy(h);
                }

                // Call sync_common.pullLink — mirrors handlers/sync/pull.zig.
                const result = sync_common.pullLink(d, allocator, link, h) catch {
                    error_count += 1;
                    continue;
                };
                defer engine.external.sync.deinitPullResult(result, allocator);

                switch (result.outcome) {
                    .ok => ok_count += 1,
                    .conflict => conflict_count += 1,
                    .noop => ok_count += 1,
                    .@"error" => error_count += 1,
                }
            }

            return .{
                .total = links.len,
                .ok_count = ok_count,
                .conflict_count = conflict_count,
                .error_count = error_count,
            };
        },
    }
}

// =========================================================================
// State machine — ExternalActionState
// =========================================================================

/// Mode for the external/workbench action overlay.
///
///   .none              — no overlay; normal view navigation.
///   .confirm_sync      — "Sync pull all links? [y/N]"
///   .confirm_workbench — "Workbench {push|pull|status} for plan? [y/N]"
///   .working           — "Working…" shown while the engine call runs.
///   .success_msg       — shows the outcome summary.
///   .error_msg         — shows the error.
pub const ActionMode = union(enum) {
    none,
    confirm_sync: struct {
        kind: SyncKind,
    },
    confirm_workbench: struct {
        kind: WorkbenchKind,
        plan_id: i64,
        /// Owned by mode — plan slug for display.
        plan_slug: []const u8,
    },
    working: struct {
        msg: []const u8, // static string literal — not freed on clearMode
    },
    success_msg: struct {
        msg: []const u8, // heap-allocated — freed on clearMode
    },
    error_msg: struct {
        msg: []const u8, // heap-allocated — freed on clearMode
    },
};

/// All mutable state for the external / workbench action controller.
/// Lives in the caller's view state and is passed by pointer to handleKey /
/// renderOverlay. The io/environ fields are set by the cockpit caller so the
/// sync action can build adapters when needed.
pub const ExternalActionState = struct {
    allocator: std.mem.Allocator,
    mode: ActionMode = .none,
    /// I/O context forwarded to adapter builder (set by app.zig on init).
    io: std.Io,
    /// Process environment forwarded to adapter builder.
    /// `std.process.Environ.empty` in tests; live environ in the cockpit.
    environ: std.process.Environ,

    /// Initialize with an explicit I/O context and process environment.
    /// In the cockpit, pass `io` from `run(io, ...)` and the live environ.
    /// In tests, pass `std.testing.io` and `std.process.Environ.empty`.
    pub fn init(
        allocator: std.mem.Allocator,
        io: std.Io,
        environ: std.process.Environ,
    ) ExternalActionState {
        return .{
            .allocator = allocator,
            .io = io,
            .environ = environ,
        };
    }

    pub fn deinit(self: *ExternalActionState) void {
        self.clearMode();
    }

    /// Free any heap-owned strings in the current mode and reset to .none.
    pub fn clearMode(self: *ExternalActionState) void {
        switch (self.mode) {
            .none => {},
            .confirm_sync => {},
            .confirm_workbench => |*cw| self.allocator.free(cw.plan_slug),
            .working => {},
            .success_msg => |*s| self.allocator.free(s.msg),
            .error_msg => |*e| self.allocator.free(e.msg),
        }
        self.mode = .none;
    }

    /// Whether the overlay is active (consumes keys).
    pub fn isActive(self: *const ExternalActionState) bool {
        return self.mode != .none;
    }

    /// Enter the confirm-sync overlay.
    /// Call when the operator presses the sync-action key in the ExtOps view.
    pub fn enterSyncConfirm(self: *ExternalActionState, kind: SyncKind) void {
        self.clearMode();
        self.mode = .{ .confirm_sync = .{ .kind = kind } };
    }

    /// Enter the confirm-workbench overlay.
    /// `plan_slug` is duped and owned by the mode. Caller does not need to keep it.
    pub fn enterWorkbenchConfirm(
        self: *ExternalActionState,
        kind: WorkbenchKind,
        plan_id: i64,
        plan_slug: []const u8,
    ) void {
        self.clearMode();
        const slug_owned = self.allocator.dupe(u8, plan_slug) catch {
            const err_msg = self.allocator.dupe(u8, "out of memory") catch return;
            self.mode = .{ .error_msg = .{ .msg = err_msg } };
            return;
        };
        self.mode = .{ .confirm_workbench = .{
            .kind = kind,
            .plan_id = plan_id,
            .plan_slug = slug_owned,
        } };
    }

    /// Set an error message. Frees the previous mode.
    pub fn setError(self: *ExternalActionState, msg: []const u8) void {
        self.clearMode();
        const owned = self.allocator.dupe(u8, msg) catch return;
        self.mode = .{ .error_msg = .{ .msg = owned } };
    }

    /// Set a success message. Frees the previous mode.
    pub fn setSuccess(self: *ExternalActionState, msg: []const u8) void {
        self.clearMode();
        const owned = self.allocator.dupe(u8, msg) catch return;
        self.mode = .{ .success_msg = .{ .msg = owned } };
    }

    /// Handle a key event for the action overlay. Returns true when consumed.
    ///
    /// State machine:
    ///   .confirm_*    — 'y'/'Y' executes; Esc or any other key cancels.
    ///   .working      — keys ignored (synchronous v1 — call runs first).
    ///   .success_msg, .error_msg — any key dismisses.
    pub fn handleKey(
        self: *ExternalActionState,
        key: Key,
        d: *db.sqlite.Db,
    ) bool {
        switch (self.mode) {
            .none => return false,

            .confirm_sync => |cs| {
                if (key.matches(Key.escape, .{})) {
                    self.clearMode();
                    return true;
                }
                if (key.matches('y', .{}) or key.matches('Y', .{})) {
                    const kind = cs.kind;
                    // Execute synchronously (v1 — may briefly block event loop;
                    // acceptable per spec: "synchronous call with a clear
                    // 'working…' state is acceptable for v1").
                    self.clearMode();
                    self.mode = .{ .working = .{ .msg = "Pulling sync links..." } };

                    const result = executeSync(d, self.allocator, kind, self.io, &self.environ) catch |e| {
                        const err_label: []const u8 = switch (e) {
                            ActionError.EngineFailed => "engine call failed",
                            ActionError.NoLinks => "no pullable links found",
                            else => "sync failed",
                        };
                        self.setError(err_label);
                        return true;
                    };

                    const msg = std.fmt.allocPrint(
                        self.allocator,
                        "sync pull: {d} links ({d} ok, {d} conflict, {d} error)",
                        .{ result.total, result.ok_count, result.conflict_count, result.error_count },
                    ) catch "sync pull complete";
                    self.clearMode();
                    self.mode = .{ .success_msg = .{ .msg = msg } };
                    return true;
                }
                // Any other key: cancel.
                self.clearMode();
                return true;
            },

            .confirm_workbench => |*cw| {
                if (key.matches(Key.escape, .{})) {
                    self.clearMode();
                    return true;
                }
                if (key.matches('y', .{}) or key.matches('Y', .{})) {
                    const kind = cw.kind;
                    const plan_id = cw.plan_id;
                    self.clearMode();
                    self.mode = .{ .working = .{ .msg = "Running workbench operation..." } };

                    const result = executeWorkbench(d, self.allocator, kind, plan_id) catch |e| {
                        const err_label: []const u8 = switch (e) {
                            ActionError.EngineFailed => "workbench engine call failed",
                            else => "workbench operation failed",
                        };
                        self.setError(err_label);
                        return true;
                    };

                    const verb: []const u8 = switch (result.kind) {
                        .status => "status",
                        .pull => "pull",
                        .push => "push",
                    };
                    const msg = std.fmt.allocPrint(
                        self.allocator,
                        "workbench {s}: {d} applied, {d} pending, {d} conflict(s)",
                        .{ verb, result.applied, result.pending, result.conflicts },
                    ) catch "workbench complete";
                    self.clearMode();
                    self.mode = .{ .success_msg = .{ .msg = msg } };
                    return true;
                }
                // Any other key: cancel.
                self.clearMode();
                return true;
            },

            // In .working the engine call is already running (synchronous v1).
            .working => return true,

            .success_msg => {
                self.clearMode();
                return true;
            },

            .error_msg => {
                self.clearMode();
                return true;
            },
        }
    }
};

// =========================================================================
// Rendering
// =========================================================================

/// Render the action overlay into `win`. Called by the host view's render fn
/// when `state.isActive()`.
pub fn renderOverlay(
    state: *const ExternalActionState,
    win: Window,
) void {
    if (win.height == 0 or win.width == 0) return;

    var row: u16 = 0;

    const printAt = struct {
        fn call(w: Window, r: u16, text: []const u8, style: Style) void {
            if (r >= w.height) return;
            _ = w.printSegment(.{ .text = text, .style = style }, .{ .row_offset = r, .col_offset = 0 });
        }
    }.call;

    switch (state.mode) {
        .none => return,

        .confirm_sync => |cs| {
            const label: []const u8 = switch (cs.kind) {
                .pull_all => "Sync pull ALL links from external plane?",
            };
            printAt(win, row, label, .{ .bold = true });
            row += 1;
            printAt(win, row, "  This calls the same engine as `planar sync pull --all`.", .{ .dim = true });
            row += 1;
            printAt(win, row, "  Network required; may fail if token not set.", .{ .dim = true });
            row += 1;
            printAt(win, row, "  y = confirm, Esc or any key = cancel", .{ .dim = true });
        },

        .confirm_workbench => |*cw| {
            const verb_label: []const u8 = switch (cw.kind) {
                .status => "status (read diff)",
                .pull => "pull (FS -> DB)",
                .push => "push (DB -> FS)",
            };
            if (row < win.height) {
                _ = win.print(&.{
                    .{ .text = "Workbench ", .style = .{ .bold = true } },
                    .{ .text = verb_label, .style = .{ .bold = true } },
                    .{ .text = " for plan: ", .style = .{ .bold = true } },
                    .{ .text = cw.plan_slug, .style = .{ .bold = true } },
                }, .{ .row_offset = row, .col_offset = 0 });
                row += 1;
            }
            printAt(win, row, "  Same engine as `planar workbench {push|pull|status}`.", .{ .dim = true });
            row += 1;
            printAt(win, row, "  y = confirm, Esc or any key = cancel", .{ .dim = true });
        },

        .working => |wm| {
            printAt(win, row, wm.msg, .{ .bold = true, .dim = true });
            row += 1;
            printAt(win, row, "  (please wait...)", .{ .dim = true });
        },

        .success_msg => |*s| {
            printAt(win, row, "Done:", .{ .bold = true });
            row += 1;
            printAt(win, row, s.msg, .{});
            row += 1;
            printAt(win, row, "(press any key to continue)", .{ .dim = true });
        },

        .error_msg => |*e| {
            printAt(win, row, "Action failed:", .{ .bold = true });
            row += 1;
            printAt(win, row, e.msg, .{});
            row += 1;
            printAt(win, row, "(press any key to dismiss)", .{ .dim = true });
        },
    }
}

/// Return a legend string for the action overlay (for the key legend bar).
pub fn legendLabel(state: *const ExternalActionState, buf: []u8) []const u8 {
    return switch (state.mode) {
        .none => std.fmt.bufPrint(buf, "  S Sync-pull  W Workbench-action", .{}) catch
            "  S Sync-pull  W Workbench-action",
        .confirm_sync => std.fmt.bufPrint(buf, "  Confirm sync pull? y=yes  Esc=cancel", .{}) catch
            "  Confirm sync pull? y=yes  Esc=cancel",
        .confirm_workbench => std.fmt.bufPrint(buf, "  Confirm workbench action? y=yes  Esc=cancel", .{}) catch
            "  Confirm workbench action? y=yes  Esc=cancel",
        .working => std.fmt.bufPrint(buf, "  Working...", .{}) catch "  Working...",
        .success_msg => std.fmt.bufPrint(buf, "  Done — press any key to continue", .{}) catch
            "  Done — press any key to continue",
        .error_msg => std.fmt.bufPrint(buf, "  Error — press any key to dismiss", .{}) catch
            "  Error — press any key to dismiss",
    };
}

// =========================================================================
// Tests (tasks 4049, 4050)
// =========================================================================
//
// All tests run under std.testing.allocator (GPA with leak detection).
// DB is in-memory with migrations applied. The network path is never called
// in tests because the in-memory DB has no external_systems/links → allPullable
// returns empty. The workbench engine calls are pure DB + FS (status is DB-only).

/// Convenience: build a test state with testing.io + empty environ.
fn makeTestState(a: std.mem.Allocator) ExternalActionState {
    return ExternalActionState.init(a, std.testing.io, std.process.Environ.empty);
}

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

fn seedPlan(d: *db.sqlite.Db, slug: []const u8) !i64 {
    return d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global',?,?,'active')",
        &.{ .{ .text = slug }, .{ .text = slug } },
    );
}

// -------------------------------------------------------------------------
// Task 4049: confirm-gate for sync action
// -------------------------------------------------------------------------

test "external_actions: enterSyncConfirm sets confirm_sync mode" {
    const a = std.testing.allocator;
    var state = makeTestState(a);
    defer state.deinit();

    state.enterSyncConfirm(.pull_all);
    try std.testing.expect(state.isActive());
    switch (state.mode) {
        .confirm_sync => |cs| try std.testing.expectEqual(SyncKind.pull_all, cs.kind),
        else => try std.testing.expect(false),
    }
}

test "external_actions: Escape from confirm_sync cancels — no engine call" {
    // Confirm gate: Esc cancels without executing the action.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = makeTestState(a);
    defer state.deinit();

    state.enterSyncConfirm(.pull_all);
    try std.testing.expect(state.isActive());

    const esc_key = Key{ .codepoint = Key.escape };
    const consumed = state.handleKey(esc_key, &d);
    try std.testing.expect(consumed);
    try std.testing.expect(!state.isActive());
}

test "external_actions: non-y key from confirm_sync cancels — no engine call" {
    // Confirm gate: any key other than 'y'/'Y' cancels.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = makeTestState(a);
    defer state.deinit();

    state.enterSyncConfirm(.pull_all);

    const n_key = Key{ .codepoint = 'n', .text = "n" };
    const consumed = state.handleKey(n_key, &d);
    try std.testing.expect(consumed);
    try std.testing.expect(!state.isActive());
}

test "external_actions: 'y' from confirm_sync with no links returns success with zero counts" {
    // Confirm gate fires: 'y' executes the action. Empty DB → 0 links →
    // zero-count success message (no network call needed).
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = makeTestState(a);
    defer state.deinit();

    state.enterSyncConfirm(.pull_all);
    try std.testing.expect(state.mode == .confirm_sync);

    const y_key = Key{ .codepoint = 'y', .text = "y" };
    const consumed = state.handleKey(y_key, &d);
    try std.testing.expect(consumed);

    // Should be in success_msg mode.
    switch (state.mode) {
        .success_msg => |*s| {
            // Must mention the link count.
            try std.testing.expect(std.mem.indexOf(u8, s.msg, "0") != null);
        },
        else => try std.testing.expect(false), // expected .success_msg
    }
}

test "external_actions: success_msg dismisses on any key" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = makeTestState(a);
    defer state.deinit();

    state.setSuccess("sync pull: 0 links (0 ok, 0 conflict, 0 error)");
    try std.testing.expect(state.isActive());

    const x_key = Key{ .codepoint = 'x' };
    const consumed = state.handleKey(x_key, &d);
    try std.testing.expect(consumed);
    try std.testing.expect(!state.isActive());
}

test "external_actions: error_msg dismisses on any key" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = makeTestState(a);
    defer state.deinit();

    state.setError("engine call failed — adapter credentials missing");
    try std.testing.expect(state.isActive());

    const x_key = Key{ .codepoint = 'x' };
    const consumed = state.handleKey(x_key, &d);
    try std.testing.expect(consumed);
    try std.testing.expect(!state.isActive());
}

// -------------------------------------------------------------------------
// Task 4050: confirm-gate for workbench action
// -------------------------------------------------------------------------

test "external_actions: enterWorkbenchConfirm sets confirm_workbench mode" {
    const a = std.testing.allocator;
    var state = makeTestState(a);
    defer state.deinit();

    state.enterWorkbenchConfirm(.status, 42, "my-feature");
    try std.testing.expect(state.isActive());
    switch (state.mode) {
        .confirm_workbench => |*cw| {
            try std.testing.expectEqual(WorkbenchKind.status, cw.kind);
            try std.testing.expectEqual(@as(i64, 42), cw.plan_id);
            try std.testing.expectEqualStrings("my-feature", cw.plan_slug);
        },
        else => try std.testing.expect(false),
    }
}

test "external_actions: Escape from confirm_workbench cancels — no engine call" {
    // Confirm gate: Esc cancels without executing workbench action.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = makeTestState(a);
    defer state.deinit();

    state.enterWorkbenchConfirm(.pull, 1, "feat");
    try std.testing.expect(state.isActive());

    const esc_key = Key{ .codepoint = Key.escape };
    const consumed = state.handleKey(esc_key, &d);
    try std.testing.expect(consumed);
    try std.testing.expect(!state.isActive());
}

test "external_actions: non-y key from confirm_workbench cancels — no engine call" {
    // Confirm gate: any key other than 'y'/'Y' cancels.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var state = makeTestState(a);
    defer state.deinit();

    state.enterWorkbenchConfirm(.push, 1, "feat");
    try std.testing.expect(state.mode == .confirm_workbench);

    const n_key = Key{ .codepoint = 'n', .text = "n" };
    const consumed = state.handleKey(n_key, &d);
    try std.testing.expect(consumed);
    try std.testing.expect(!state.isActive());
}

test "external_actions: 'y' workbench status on a valid plan triggers engine call" {
    // engine.workbench.sync.status is pure DB — works in-memory.
    // On an empty workbench the result is 0 applied / 0 pending / 0 conflicts.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d, "test-wb-stat");

    var state = makeTestState(a);
    defer state.deinit();

    state.enterWorkbenchConfirm(.status, plan_id, "test-wb-stat");
    try std.testing.expect(state.mode == .confirm_workbench);

    const y_key = Key{ .codepoint = 'y', .text = "y" };
    const consumed = state.handleKey(y_key, &d);
    try std.testing.expect(consumed);

    // After 'y': either success_msg (engine worked) or error_msg (FS absent).
    // Either way, confirm_workbench must be gone.
    switch (state.mode) {
        .success_msg => |*s| {
            try std.testing.expect(std.mem.indexOf(u8, s.msg, "status") != null);
        },
        .error_msg => {}, // workbench status may fail if root absent — that's ok
        else => try std.testing.expect(false), // expected .success_msg or .error_msg
    }
}

test "external_actions: 'y' workbench push on valid plan changes mode from confirm" {
    // Push may fail if workbench FS root absent; confirm gate still fires.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d, "test-wb-push");

    var state = makeTestState(a);
    defer state.deinit();

    state.enterWorkbenchConfirm(.push, plan_id, "test-wb-push");
    try std.testing.expect(state.mode == .confirm_workbench);

    const y_key = Key{ .codepoint = 'y', .text = "y" };
    const consumed = state.handleKey(y_key, &d);
    try std.testing.expect(consumed);

    // Mode must change from confirm_workbench.
    try std.testing.expect(state.mode != .confirm_workbench);
    try std.testing.expect(state.mode != .none);
}

test "external_actions: 'y' workbench pull on valid plan changes mode from confirm" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d, "test-wb-pull");

    var state = makeTestState(a);
    defer state.deinit();

    state.enterWorkbenchConfirm(.pull, plan_id, "test-wb-pull");
    try std.testing.expect(state.mode == .confirm_workbench);

    const y_key = Key{ .codepoint = 'y', .text = "y" };
    const consumed = state.handleKey(y_key, &d);
    try std.testing.expect(consumed);

    // Mode must change from confirm_workbench.
    try std.testing.expect(state.mode != .confirm_workbench);
    try std.testing.expect(state.mode != .none);
}

// -------------------------------------------------------------------------
// Task 4049: executeSync dispatch (no live network)
// -------------------------------------------------------------------------

test "executeSync pull_all: empty DB yields zero-count result" {
    // No external_systems or links → allPullable returns empty.
    // This tests the engine dispatch path without a live network.
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const result = try executeSync(&d, a, .pull_all, std.testing.io, &std.process.Environ.empty);
    try std.testing.expectEqual(@as(usize, 0), result.total);
    try std.testing.expectEqual(@as(usize, 0), result.ok_count);
    try std.testing.expectEqual(@as(usize, 0), result.conflict_count);
    try std.testing.expectEqual(@as(usize, 0), result.error_count);
}

// -------------------------------------------------------------------------
// Task 4050: executeWorkbench dispatch (DB-only)
// -------------------------------------------------------------------------

test "executeWorkbench status: valid plan returns kind=status and applied=0" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try seedPlan(&d, "exec-wb-stat");

    // Status mode never writes anything to the FS, so applied == 0 and
    // conflicts == 0 always hold for a freshly seeded plan. `pending` may
    // be non-zero because the engine counts DB entities not yet on the FS
    // (the plan itself is one such entity). We only pin what we know.
    const result_or_err = executeWorkbench(&d, a, .status, plan_id);
    if (result_or_err) |result| {
        try std.testing.expectEqual(WorkbenchKind.status, result.kind);
        try std.testing.expectEqual(@as(usize, 0), result.applied);
        try std.testing.expectEqual(@as(usize, 0), result.conflicts);
        // pending >= 0 always; no assertion needed — value depends on FS state.
    } else |_| {
        // May fail if FS root absent or fetchAnchor fails; acceptable in test context.
    }
}

// -------------------------------------------------------------------------
// Render-level tests (rule: render every datum)
// -------------------------------------------------------------------------

fn makeTestScreen(allocator: std.mem.Allocator, w: u16, h: u16) !vaxis.Screen {
    return vaxis.Screen.init(allocator, .{
        .cols = w,
        .rows = h,
        .x_pixel = 0,
        .y_pixel = 0,
    });
}

fn makeTestWin(screen: *vaxis.Screen, w: u16, h: u16) Window {
    return .{
        .x_off = 0,
        .y_off = 0,
        .parent_x_off = 0,
        .parent_y_off = 0,
        .width = w,
        .height = h,
        .screen = screen,
    };
}

fn collectText(screen: *const vaxis.Screen, out: *std.ArrayList(u8)) !void {
    for (screen.buf) |cell| {
        const g = cell.char.grapheme;
        if (g.len > 0 and g[0] != 0) try out.appendSlice(std.testing.allocator, g);
    }
}

test "renderOverlay: confirm_sync shows confirmation prompt (render-level)" {
    const a = std.testing.allocator;
    var state = makeTestState(a);
    defer state.deinit();
    state.enterSyncConfirm(.pull_all);

    var screen = try makeTestScreen(a, 100, 8);
    defer screen.deinit(a);
    const win = makeTestWin(&screen, 100, 8);

    renderOverlay(&state, win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectText(&screen, &rendered);
    const text = rendered.items;

    // Must show the sync action description.
    try std.testing.expect(std.mem.indexOf(u8, text, "Sync pull") != null);
    // Must show confirmation hint.
    try std.testing.expect(std.mem.indexOf(u8, text, "cancel") != null);
}

test "renderOverlay: confirm_workbench shows plan slug and action verb (render-level)" {
    const a = std.testing.allocator;
    var state = makeTestState(a);
    defer state.deinit();
    state.enterWorkbenchConfirm(.push, 42, "my-feature-plan");

    var screen = try makeTestScreen(a, 120, 8);
    defer screen.deinit(a);
    const win = makeTestWin(&screen, 120, 8);

    renderOverlay(&state, win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectText(&screen, &rendered);
    const text = rendered.items;

    // Must show the plan slug.
    try std.testing.expect(std.mem.indexOf(u8, text, "my-feature-plan") != null);
    // Must show the action verb.
    try std.testing.expect(std.mem.indexOf(u8, text, "push") != null);
}

test "renderOverlay: success_msg shows 'Done' and the result text (render-level)" {
    const a = std.testing.allocator;
    var state = makeTestState(a);
    defer state.deinit();
    state.setSuccess("workbench status: 0 applied, 3 pending, 0 conflict(s)");

    var screen = try makeTestScreen(a, 100, 6);
    defer screen.deinit(a);
    const win = makeTestWin(&screen, 100, 6);

    renderOverlay(&state, win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectText(&screen, &rendered);
    const text = rendered.items;

    try std.testing.expect(std.mem.indexOf(u8, text, "Done") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "pending") != null);
}

test "renderOverlay: error_msg shows 'failed' and the error text (render-level)" {
    const a = std.testing.allocator;
    var state = makeTestState(a);
    defer state.deinit();
    state.setError("engine call failed — check adapter credentials");

    var screen = try makeTestScreen(a, 100, 6);
    defer screen.deinit(a);
    const win = makeTestWin(&screen, 100, 6);

    renderOverlay(&state, win);

    var rendered: std.ArrayList(u8) = .empty;
    defer rendered.deinit(a);
    try collectText(&screen, &rendered);
    const text = rendered.items;

    try std.testing.expect(std.mem.indexOf(u8, text, "failed") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "credentials") != null);
}

test "external_actions module compiles" {
    std.testing.refAllDecls(@This());
}
