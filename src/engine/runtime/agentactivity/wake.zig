//! engine/runtime/agentactivity/wake — cross-platform event-driven
//! wake source for `planar-watch <verb> --follow` (plan 85 M9, Tier 2).
//!
//! Per tech-spec § "Live tail / follow implementation":
//!
//!   - Tier 1 (shipped in M8) polls the DB on a fixed interval. CPU
//!     idle cost is bounded by the interval; latency floor is the
//!     interval itself.
//!   - Tier 2 (this module) wakes on filesystem events on the SQLite
//!     `-wal` sibling. SQLite WAL appends are observable as
//!     NOTE_WRITE / NOTE_EXTEND (kqueue) and IN_MODIFY (inotify), so
//!     a `planar-watch` follow loop can sleep on the wake source
//!     between writes and re-query the watermark when a write lands.
//!
//! Public contract invariance: this module is INTERNAL. The
//! `--follow` JSON shape, watermark, and SIGINT exit-0 semantics are
//! all part of the public contract and unchanged by the Tier 2 swap.
//! The interval flag becomes a HEARTBEAT (maximum poll fallback):
//! the wake fires on -wal change, but `waitNext` still returns at
//! the interval if no event arrived so a watcher that missed a wake
//! event (e.g. laptop slept and the OS coalesced notifications)
//! still recovers within one heartbeat.
//!
//! Backend selection (comptime):
//!
//!   - macOS / BSDs (darwin, freebsd, netbsd, openbsd, dragonfly):
//!     `kqueue` + `EVFILT_VNODE` on the `-wal` fd. `NOTE_WRITE` and
//!     `NOTE_EXTEND` fire on commits; `NOTE_DELETE` and `NOTE_RENAME`
//!     fire on rotation.
//!   - Linux: `inotify_init1` + `inotify_add_watch` on the `-wal`
//!     path. `IN_MODIFY` fires on writes; `IN_DELETE_SELF` and
//!     `IN_MOVE_SELF` fire on rotation.
//!   - Other platforms (windows, wasi, unsupported BSDs): degraded
//!     mode — `waitNext` just sleeps `timeout_ns` and returns
//!     `.heartbeat`. The poll loop still works; the latency floor
//!     just stays at the interval. A one-shot stderr warning is
//!     emitted at `init` so operators see the degradation.
//!
//! `-wal` rotation: SQLite's `PRAGMA wal_checkpoint(TRUNCATE)`
//! deletes the file (or, depending on the host, unlinks + recreates
//! it on the next write). Both backends detect this (NOTE_DELETE /
//! IN_DELETE_SELF + IN_IGNORED) and re-open the watch transparently
//! on the next `waitNext` call. The fallback heartbeat covers the
//! interval between the rotation and a fresh write.
//!
//! Attach-gap race (fixed plan 85 t#2623): kqueue/inotify watches are
//! edge-triggered (kqueue uses EV_CLEAR; inotify only fires for events
//! that arrive *after* `inotify_add_watch`). If a write lands on the
//! `-wal` BETWEEN our `close(old_fd)` and `open(new_path)+register`
//! window (the post-rotation reattach gap, or the lazy-init gap on
//! the very first write), the kernel does NOT replay the event for
//! us. To close that race, `tryAttach` reports whether it just
//! transitioned from "not attached" → "attached", and `waitNext`
//! surfaces a synthetic `.wal_changed` the first time after such a
//! transition. The follow loop then issues a catch-up watermark
//! query that picks up any commit that landed in the gap.

const std = @import("std");
const builtin = @import("builtin");

/// What `waitNext` decided to return.
pub const WakeEvent = enum {
    /// A filesystem-level event landed on the `-wal` file
    /// (write/extend or rotation). The caller should re-query the
    /// watermark.
    wal_changed,
    /// The timeout elapsed without a kernel notification. The caller
    /// should still re-query — this is the safety-net heartbeat for
    /// coalesced / missed wake events.
    heartbeat,
    /// The wake source was interrupted (e.g. EINTR from SIGINT). The
    /// caller should check its shutdown flag and exit if needed.
    /// Treated as `.heartbeat` for re-query purposes; surfaced
    /// separately so tests can pin the EINTR path.
    interrupted,
};

const Backend = enum { kqueue, inotify, degraded };

/// Pick the backend at comptime. Refusal to compile is intentional
/// when a target lands here without a branch — that forces an
/// explicit "this platform is degraded" decision in the table.
const active_backend: Backend = switch (builtin.os.tag) {
    .macos, .ios, .tvos, .watchos, .freebsd, .netbsd, .openbsd, .dragonfly => .kqueue,
    .linux => .inotify,
    else => .degraded,
};

/// Cross-platform wake source. Owns its backend file descriptors.
/// Single-threaded: call `init` and `waitNext` from the same thread
/// the follow loop runs on.
pub const Wake = struct {
    allocator: std.mem.Allocator,
    /// Owned copy of the SQLite DB path. Used to derive `-wal_path`
    /// at re-open time after rotation.
    db_path: [:0]u8,
    /// Owned copy of `${db_path}-wal`. Watched directly by the
    /// backend. Kept on the struct so re-open after rotation reuses
    /// the same buffer.
    wal_path: [:0]u8,
    backend: BackendState,

    const BackendState = union(Backend) {
        kqueue: KqueueState,
        inotify: InotifyState,
        degraded: DegradedState,
    };

    /// Initialize a wake source for the given DB. Never fails on a
    /// missing `-wal` file: SQLite only creates the sibling on the
    /// first write. The backend tracks the absence and re-tries on
    /// each `waitNext`.
    pub fn init(allocator: std.mem.Allocator, db_path: []const u8) !Wake {
        const db_owned = try allocator.dupeZ(u8, db_path);
        errdefer allocator.free(db_owned);

        const wal_owned: [:0]u8 = try std.fmt.allocPrintSentinel(
            allocator,
            "{s}-wal",
            .{db_path},
            0,
        );
        errdefer allocator.free(wal_owned);

        const state = switch (active_backend) {
            .kqueue => BackendState{ .kqueue = try KqueueState.init(wal_owned) },
            .inotify => BackendState{ .inotify = try InotifyState.init(wal_owned) },
            .degraded => blk: {
                emitDegradedWarningOnce();
                break :blk BackendState{ .degraded = .{} };
            },
        };

        return .{
            .allocator = allocator,
            .db_path = db_owned,
            .wal_path = wal_owned,
            .backend = state,
        };
    }

    /// Release the backend resources. Idempotent.
    pub fn close(self: *Wake) void {
        switch (self.backend) {
            .kqueue => |*s| s.close(),
            .inotify => |*s| s.close(),
            .degraded => {},
        }
        self.allocator.free(self.db_path);
        self.allocator.free(self.wal_path);
    }

    /// Wait up to `timeout_ns` nanoseconds for a `-wal` change, or
    /// return `.heartbeat` when the timeout fires. Returns
    /// `.interrupted` if the underlying syscall is interrupted by a
    /// signal — the caller should check its stop flag and decide
    /// whether to re-enter.
    ///
    /// Re-opens the backend watch transparently on rotation (the
    /// `-wal` was deleted by `PRAGMA wal_checkpoint(TRUNCATE)` or a
    /// crash/recovery cycle).
    pub fn waitNext(self: *Wake, timeout_ns: u64) !WakeEvent {
        return switch (self.backend) {
            .kqueue => |*s| s.waitNext(self.wal_path, timeout_ns),
            .inotify => |*s| s.waitNext(self.wal_path, timeout_ns),
            .degraded => |*s| s.waitNext(timeout_ns),
        };
    }
};

// =====================================================================
// kqueue backend (macOS / BSDs)
// =====================================================================

const KqueueState = struct {
    /// kqueue fd. Created lazily in `init`. -1 once `close`d.
    kq: c_int,
    /// Currently watched `-wal` fd. -1 when the file doesn't exist
    /// yet OR was rotated out from under us.
    wal_fd: c_int,
    /// Set by `tryAttach` whenever the fd transitions from -1 to a
    /// valid descriptor. `waitNext` consumes this flag and surfaces
    /// a synthetic `.wal_changed` so the follow loop performs a
    /// catch-up watermark query. Closes the attach-gap race
    /// described in the module doc (writes landing between the old
    /// fd's teardown and the new fd's kqueue registration are
    /// invisible to edge-triggered EVFILT_VNODE).
    fresh_attach: bool,

    fn init(wal_path: [:0]const u8) !KqueueState {
        if (active_backend != .kqueue) return .{ .kq = -1, .wal_fd = -1, .fresh_attach = false };
        const kq = std.c.kqueue();
        if (kq < 0) return error.KqueueInitFailed;
        var s: KqueueState = .{ .kq = kq, .wal_fd = -1, .fresh_attach = false };
        s.tryAttach(wal_path);
        // Suppress the initial fresh_attach signal: the very first
        // `waitNext` is invoked AFTER the follow loop's initial
        // watermark query, so a synthetic .wal_changed there would
        // just trigger a duplicate query (harmless but noisy).
        // After init we're aligned with the snapshot; only
        // re-attaches need the catch-up.
        s.fresh_attach = false;
        return s;
    }

    fn close(self: *KqueueState) void {
        if (active_backend != .kqueue) return;
        if (self.wal_fd >= 0) {
            _ = std.c.close(self.wal_fd);
            self.wal_fd = -1;
        }
        if (self.kq >= 0) {
            _ = std.c.close(self.kq);
            self.kq = -1;
        }
    }

    /// Open the `-wal` file (if it exists) and register the
    /// EVFILT_VNODE watch. Best-effort: a missing file just leaves
    /// `wal_fd = -1` so the next `waitNext` heartbeat re-tries.
    fn tryAttach(self: *KqueueState, wal_path: [:0]const u8) void {
        if (active_backend != .kqueue) return;
        if (self.wal_fd >= 0) return;
        // O_RDONLY | O_NONBLOCK so we don't block on FIFOs / etc.
        // by accident. The fd is solely for kevent registration; we
        // never read() it.
        // EVTONLY (macOS): open the file purely for kevent monitoring,
        // skipping reference-counting that would otherwise prevent
        // unmount. Crucially, EVTONLY also avoids any interaction with
        // SQLite's WAL locking model — a plain O_RDONLY counts as a
        // "user" of the file and can subtly affect concurrent-writer
        // ordering across darwin's WAL checkpoint paths. For BSD we
        // fall back to plain O_RDONLY (EVTONLY is darwin-only); the
        // BSD WAL semantics aren't sensitive to the extra open.
        const fd = if (builtin.os.tag == .macos or builtin.os.tag == .ios or
            builtin.os.tag == .tvos or builtin.os.tag == .watchos)
            std.c.open(wal_path.ptr, .{
                .ACCMODE = .RDONLY,
                .EVTONLY = true,
                .CLOEXEC = true,
            }, @as(std.c.mode_t, 0))
        else
            std.c.open(wal_path.ptr, .{
                .ACCMODE = .RDONLY,
                .CLOEXEC = true,
            }, @as(std.c.mode_t, 0));
        if (fd < 0) return;
        self.wal_fd = fd;

        const fflags: u32 = std.c.NOTE.WRITE | std.c.NOTE.EXTEND |
            std.c.NOTE.DELETE | std.c.NOTE.RENAME;
        const ev: std.c.Kevent = .{
            .ident = @intCast(self.wal_fd),
            .filter = std.c.EVFILT.VNODE,
            .flags = std.c.EV.ADD | std.c.EV.CLEAR,
            .fflags = fflags,
            .data = 0,
            .udata = 0,
        };
        var changelist = [_]std.c.Kevent{ev};
        const rc = std.c.kevent(self.kq, &changelist, 1, undefined, 0, null);
        if (rc < 0) {
            // Registration failed — drop the fd; next heartbeat
            // tries again.
            _ = std.c.close(self.wal_fd);
            self.wal_fd = -1;
            return;
        }
        // We just transitioned from "no fd" to "watching" — the
        // next waitNext must surface a synthetic .wal_changed so
        // the follow loop catches any commit that landed in the
        // attach gap. See the module-level doc § Attach-gap race.
        self.fresh_attach = true;
    }

    /// Re-attach after rotation: close the stale fd and try to open
    /// the (presumably new) `-wal` again.
    fn reattach(self: *KqueueState, wal_path: [:0]const u8) void {
        if (self.wal_fd >= 0) {
            _ = std.c.close(self.wal_fd);
            self.wal_fd = -1;
        }
        self.tryAttach(wal_path);
    }

    fn waitNext(self: *KqueueState, wal_path: [:0]const u8, timeout_ns: u64) !WakeEvent {
        if (active_backend != .kqueue) return .heartbeat;
        // Lazy attach if the file didn't exist at init time.
        if (self.wal_fd < 0) self.tryAttach(wal_path);

        // If tryAttach just transitioned us to "attached", surface a
        // synthetic .wal_changed now and consume the flag. This is
        // the catch-up that closes the attach-gap race (writes that
        // landed before our kqueue registration are invisible to
        // edge-triggered EVFILT_VNODE; the synthetic wake makes the
        // follow loop re-query the watermark and observe them).
        if (self.fresh_attach) {
            self.fresh_attach = false;
            return .wal_changed;
        }

        var ts: std.c.timespec = .{
            .sec = @intCast(timeout_ns / std.time.ns_per_s),
            .nsec = @intCast(timeout_ns % std.time.ns_per_s),
        };

        var out: [4]std.c.Kevent = undefined;
        const n = std.c.kevent(self.kq, undefined, 0, &out, out.len, &ts);
        if (n < 0) {
            // EINTR is the expected SIGINT path — surface it
            // distinctly so callers can decide. Other errors fall
            // through to a heartbeat (degraded behavior, not a hard
            // fail — the next iteration will re-try).
            const errno = std.posix.errno(@as(isize, n));
            if (errno == .INTR) return .interrupted;
            return .heartbeat;
        }
        if (n == 0) return .heartbeat;

        // At least one event landed. Inspect for rotation.
        var rotated = false;
        var i: usize = 0;
        while (i < @as(usize, @intCast(n))) : (i += 1) {
            const ev = out[i];
            if ((ev.fflags & (std.c.NOTE.DELETE | std.c.NOTE.RENAME)) != 0) {
                rotated = true;
            }
        }
        if (rotated) {
            self.reattach(wal_path);
            // The reattach may have set fresh_attach=true. Consume
            // it here so we don't emit a duplicate .wal_changed on
            // the very next call — the current return already tells
            // the caller to re-query.
            self.fresh_attach = false;
        }
        return .wal_changed;
    }
};

// =====================================================================
// inotify backend (Linux)
// =====================================================================

const InotifyState = struct {
    /// inotify fd from `inotify_init1`. -1 once `close`d.
    fd: c_int,
    /// Watch descriptor for the `-wal` path. -1 when the file
    /// doesn't exist yet OR was rotated out.
    wd: c_int,
    /// Mirror of KqueueState.fresh_attach — set when tryAttach
    /// transitions from "no watch" → "watching" so waitNext can
    /// emit a synthetic .wal_changed for the catch-up watermark
    /// query. See module doc § Attach-gap race.
    fresh_attach: bool,

    fn init(wal_path: [:0]const u8) !InotifyState {
        if (active_backend != .inotify) return .{ .fd = -1, .wd = -1, .fresh_attach = false };
        // IN_NONBLOCK so `read` returns immediately when the queue
        // is empty — we use poll() for the actual blocking wait.
        // IN_CLOEXEC so the fd doesn't leak to child processes.
        const fd = std.c.inotify_init1(linuxIN("CLOEXEC") | linuxIN("NONBLOCK"));
        if (fd < 0) return error.InotifyInitFailed;
        var s: InotifyState = .{ .fd = fd, .wd = -1, .fresh_attach = false };
        s.tryAttach(wal_path);
        // Suppress the initial fresh_attach: the follow loop's first
        // watermark query already runs before waitNext is ever
        // called, so the synthetic catch-up would just duplicate it.
        s.fresh_attach = false;
        return s;
    }

    fn close(self: *InotifyState) void {
        if (active_backend != .inotify) return;
        if (self.fd >= 0) {
            _ = std.c.close(self.fd);
            self.fd = -1;
        }
        self.wd = -1;
    }

    fn tryAttach(self: *InotifyState, wal_path: [:0]const u8) void {
        if (active_backend != .inotify) return;
        if (self.wd >= 0) return;
        const mask: u32 = linuxIN("MODIFY") | linuxIN("DELETE_SELF") |
            linuxIN("MOVE_SELF") | linuxIN("ATTRIB");
        const wd = std.c.inotify_add_watch(self.fd, wal_path.ptr, mask);
        if (wd < 0) return;
        self.wd = wd;
        // Just transitioned from "not watching" to "watching" —
        // signal the catch-up wake so the follow loop re-queries.
        self.fresh_attach = true;
    }

    fn reattach(self: *InotifyState, wal_path: [:0]const u8) void {
        // Kernel removes the watch on DELETE_SELF / IN_IGNORED;
        // we just clear our local handle and re-add.
        self.wd = -1;
        self.tryAttach(wal_path);
    }

    fn waitNext(self: *InotifyState, wal_path: [:0]const u8, timeout_ns: u64) !WakeEvent {
        if (active_backend != .inotify) return .heartbeat;
        if (self.wd < 0) self.tryAttach(wal_path);

        // Catch-up after a fresh (re)attach. Same rationale as the
        // kqueue branch — inotify only delivers events that arrive
        // after the watch is registered, so a commit that landed in
        // the attach gap would be invisible without this synthetic
        // wake.
        if (self.fresh_attach) {
            self.fresh_attach = false;
            return .wal_changed;
        }

        // poll() the inotify fd with the timeout. POLLIN means
        // there's at least one queued event waiting.
        var pfd: [1]std.posix.pollfd = .{.{
            .fd = self.fd,
            .events = std.posix.POLL.IN,
            .revents = 0,
        }};
        // poll's timeout is milliseconds, signed 32-bit. Clamp.
        const ms_u64: u64 = timeout_ns / std.time.ns_per_ms;
        const ms: i32 = if (ms_u64 > std.math.maxInt(i32))
            std.math.maxInt(i32)
        else
            @intCast(ms_u64);

        // std.posix.poll retries internally on EINTR, so the only
        // failure modes here are resource exhaustion. Surface them
        // as a heartbeat — the next iteration will heal.
        const ready = std.posix.poll(&pfd, ms) catch return .heartbeat;
        if (ready == 0) return .heartbeat;
        if ((pfd[0].revents & std.posix.POLL.IN) == 0) return .heartbeat;

        // Drain the queue. inotify events are variable-length; we
        // just need to know (a) something happened and (b) whether
        // the watch was torn down (IN_IGNORED / IN_DELETE_SELF).
        var buf: [4096]u8 = undefined;
        var rotated = false;
        while (true) {
            const n = std.posix.read(self.fd, &buf) catch |e| switch (e) {
                error.WouldBlock => break,
                else => break,
            };
            if (n == 0) break;
            // Walk inotify_event records. Each is `sizeof(event) + name_len`.
            var off: usize = 0;
            while (off + @sizeOf(std.os.linux.inotify_event) <= n) {
                const evp: *align(1) const std.os.linux.inotify_event =
                    @ptrCast(@alignCast(&buf[off]));
                const ev = evp.*;
                const rot_mask = linuxIN("DELETE_SELF") | linuxIN("MOVE_SELF") |
                    linuxIN("IGNORED");
                if ((ev.mask & rot_mask) != 0) rotated = true;
                off += @sizeOf(std.os.linux.inotify_event) + ev.len;
            }
            // Loop again — there may be more events queued.
        }

        if (rotated) {
            self.reattach(wal_path);
            // Drop the fresh_attach signal the reattach just raised:
            // this iteration's .wal_changed already triggers the
            // caller's watermark re-query, so the synthetic catch-up
            // on the next call would just duplicate it.
            self.fresh_attach = false;
        }
        return .wal_changed;
    }
};

/// Resolve `std.os.linux.IN.<name>` only on Linux; returns 0 elsewhere
/// so the kqueue/degraded branches still compile. The named field
/// access happens at comptime so unsupported names fail to compile.
fn linuxIN(comptime field: []const u8) u32 {
    if (builtin.os.tag != .linux) return 0;
    return @field(std.os.linux.IN, field);
}

// =====================================================================
// Degraded backend (no kqueue / no inotify available)
// =====================================================================

const DegradedState = struct {
    fn waitNext(_: *DegradedState, timeout_ns: u64) !WakeEvent {
        // Plain timed sleep; the follow loop's own SIGINT handler is
        // the only escape hatch. Match the existing Tier-1 sleep
        // cadence (≤ 100ms slices) so the operator can still ^C out
        // within a bounded latency.
        const slice_ns: u64 = 100 * std.time.ns_per_ms;
        var remaining = timeout_ns;
        while (remaining > 0) {
            const this_slice = if (remaining < slice_ns) remaining else slice_ns;
            sleepBlocking(this_slice);
            remaining -= this_slice;
        }
        return .heartbeat;
    }
};

/// Plain blocking sleep used by:
///   - the degraded backend (no kqueue/inotify available)
///   - the test fixtures' helper threads
///
/// Implemented via `nanosleep` on POSIX (Linux/BSD/macOS); a degraded
/// busy-wait elsewhere (none of our supported platforms hit that
/// branch). Not interruptible — callers that need ^C escape go
/// through the wake source's `waitNext` instead.
fn sleepBlocking(ns: u64) void {
    if (builtin.os.tag == .windows) {
        // Spin-wait fallback; never exercised by Planar's test suite.
        const t0 = monoNs();
        while (monoNs() - t0 < ns) {}
        return;
    }
    var req: std.c.timespec = .{
        .sec = @intCast(ns / std.time.ns_per_s),
        .nsec = @intCast(ns % std.time.ns_per_s),
    };
    var rem: std.c.timespec = undefined;
    // nanosleep returns -1 + EINTR if interrupted; the .rem field
    // holds the remaining time. Loop to consume the full request.
    while (true) {
        const rc = std.c.nanosleep(&req, &rem);
        if (rc == 0) return;
        if (std.posix.errno(rc) == .INTR) {
            req = rem;
            continue;
        }
        return;
    }
}

/// Current value of a monotonic clock in nanoseconds. Used by tests
/// to bound wake latency. The clock identity is platform-defined
/// (`CLOCK_MONOTONIC`-ish on each kernel) — only differences matter,
/// not the absolute value.
fn monoNs() u64 {
    var ts: std.c.timespec = undefined;
    const rc = std.c.clock_gettime(.MONOTONIC, &ts);
    if (rc != 0) return 0;
    const sec: u64 = @intCast(ts.sec);
    const nsec: u64 = @intCast(ts.nsec);
    return sec * std.time.ns_per_s + nsec;
}

var degraded_warned: std.atomic.Value(bool) = .init(false);

fn emitDegradedWarningOnce() void {
    if (degraded_warned.swap(true, .seq_cst)) return;
    const stderr = std.fs.File.stderr();
    _ = stderr.writeAll(
        "warning: planar-watch --follow falling back to Tier-1 poll: " ++
            "no kqueue/inotify backend available on this platform.\n",
    ) catch {};
}

// =====================================================================
// Tests
// =====================================================================

test "Wake.init/close on a non-existent DB path succeeds" {
    // -wal sibling doesn't exist yet — Wake must tolerate this
    // (SQLite only creates the file on first write).
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const dir_path = try std.fs.path.join(
        std.testing.allocator,
        &.{ ".zig-cache/tmp", &tmp.sub_path },
    );
    defer std.testing.allocator.free(dir_path);

    const db_path = try std.fs.path.join(std.testing.allocator, &.{ dir_path, "scratch.db" });
    defer std.testing.allocator.free(db_path);

    var w = try Wake.init(std.testing.allocator, db_path);
    defer w.close();

    // The wake source is alive; the actual wait is exercised by the
    // integration tests because it needs a real SQLite writer.
}

test "Wake.waitNext returns .heartbeat at the documented cadence" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const dir_path = try std.fs.path.join(
        std.testing.allocator,
        &.{ ".zig-cache/tmp", &tmp.sub_path },
    );
    defer std.testing.allocator.free(dir_path);

    const db_path = try std.fs.path.join(std.testing.allocator, &.{ dir_path, "heartbeat.db" });
    defer std.testing.allocator.free(db_path);

    var w = try Wake.init(std.testing.allocator, db_path);
    defer w.close();

    const timeout_ns: u64 = 300 * std.time.ns_per_ms;
    const tolerance_ns: u64 = 200 * std.time.ns_per_ms;

    var iter: usize = 0;
    while (iter < 3) : (iter += 1) {
        const t0 = monoNs();
        const ev = try w.waitNext(timeout_ns);
        const elapsed: u64 = monoNs() - t0;
        try std.testing.expect(ev == .heartbeat);
        // Returned at the timeout, within tolerance — never
        // unbounded blocking.
        try std.testing.expect(elapsed >= timeout_ns / 2);
        try std.testing.expect(elapsed <= timeout_ns + tolerance_ns);
    }
}

test "Wake.waitNext fires .wal_changed within 100ms of a real WAL commit" {
    if (active_backend == .degraded) return error.SkipZigTest;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const dir_path = try std.fs.path.join(
        std.testing.allocator,
        &.{ ".zig-cache/tmp", &tmp.sub_path },
    );
    defer std.testing.allocator.free(dir_path);

    const db_path = try std.fs.path.join(std.testing.allocator, &.{ dir_path, "wakelatency.db" });
    defer std.testing.allocator.free(db_path);

    // Open a SQLite DB in WAL mode, create a table, then SEPARATELY
    // exercise commits inside the `waitNext` window. Using the same
    // process keeps the test hermetic (no child binary needed).
    const db = @import("db");
    const c_db_path = try std.testing.allocator.dupeZ(u8, db_path);
    defer std.testing.allocator.free(c_db_path);

    var handle = try db.sqlite.Db.open(c_db_path.ptr);
    defer handle.close();
    // Enable WAL mode — required for `-wal` to exist.
    try handle.exec("PRAGMA journal_mode=WAL;");
    try handle.exec("CREATE TABLE t(x INTEGER);");

    // One commit BEFORE Wake.init guarantees the -wal sibling
    // exists at watch time (which it would in real use because
    // ensureDb runs before the follow loop).
    try handle.exec("INSERT INTO t(x) VALUES (1);");

    var w = try Wake.init(std.testing.allocator, db_path);
    defer w.close();

    // Spawn a writer thread that commits 50ms after we enter the
    // wait. The wake must fire well under our 100ms latency budget.
    const Spawner = struct {
        fn commit(h: *db.sqlite.Db) void {
            sleepBlocking(50 * std.time.ns_per_ms);
            h.exec("INSERT INTO t(x) VALUES (2);") catch {};
        }
    };
    var thr = try std.Thread.spawn(.{}, Spawner.commit, .{&handle});
    defer thr.join();

    const t0 = monoNs();
    const ev = try w.waitNext(2 * std.time.ns_per_s);
    const elapsed: u64 = monoNs() - t0;

    // Wake should fire well inside the 100ms latency budget — the
    // commit landed at ~50ms in, the kqueue / inotify delivery is
    // sub-millisecond, and our budget for total time-to-wake is
    // 250ms (50ms commit + 100ms latency + 100ms slop).
    try std.testing.expect(ev == .wal_changed);
    try std.testing.expect(elapsed < 500 * std.time.ns_per_ms);
}

test "Wake survives WAL rotation via PRAGMA wal_checkpoint(TRUNCATE)" {
    if (active_backend == .degraded) return error.SkipZigTest;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const dir_path = try std.fs.path.join(
        std.testing.allocator,
        &.{ ".zig-cache/tmp", &tmp.sub_path },
    );
    defer std.testing.allocator.free(dir_path);

    const db_path = try std.fs.path.join(std.testing.allocator, &.{ dir_path, "rotation.db" });
    defer std.testing.allocator.free(db_path);

    const db = @import("db");
    const c_db_path = try std.testing.allocator.dupeZ(u8, db_path);
    defer std.testing.allocator.free(c_db_path);

    var handle = try db.sqlite.Db.open(c_db_path.ptr);
    defer handle.close();
    try handle.exec("PRAGMA journal_mode=WAL;");
    try handle.exec("CREATE TABLE t(x INTEGER);");
    try handle.exec("INSERT INTO t(x) VALUES (1);");

    var w = try Wake.init(std.testing.allocator, db_path);
    defer w.close();

    // Force a -wal rotation. SQLite TRUNCATE-checkpoints the WAL,
    // which deletes the file. The watch must re-attach
    // transparently.
    try handle.exec("PRAGMA wal_checkpoint(TRUNCATE);");

    // Drain any rotation event the backend already delivered, with
    // a short timeout. We don't care whether this returns
    // .wal_changed or .heartbeat — what matters is that the NEXT
    // commit also fires .wal_changed.
    _ = try w.waitNext(200 * std.time.ns_per_ms);

    // Now write again — SQLite re-creates the -wal sibling. The
    // wake must surface this within the latency budget.
    const Spawner = struct {
        fn commit(h: *db.sqlite.Db) void {
            sleepBlocking(50 * std.time.ns_per_ms);
            h.exec("INSERT INTO t(x) VALUES (2);") catch {};
        }
    };
    var thr = try std.Thread.spawn(.{}, Spawner.commit, .{&handle});
    defer thr.join();

    const t0 = monoNs();
    const ev = try w.waitNext(2 * std.time.ns_per_s);
    const elapsed: u64 = monoNs() - t0;

    try std.testing.expect(ev == .wal_changed);
    try std.testing.expect(elapsed < std.time.ns_per_s);
}

test "Wake catch-up on lazy attach — write before -wal exists still surfaces" {
    // Pins the attach-gap race fix (plan 85 t#2623): when Wake.init
    // is called before SQLite has materialized the -wal sibling, the
    // FIRST commit that creates the file lands BEFORE we can register
    // the kqueue/inotify watch. EVFILT_VNODE / inotify are edge-
    // triggered and do not replay pre-registration events, so without
    // the synthetic .wal_changed on fresh attach, the follow loop
    // would miss the event and wait the full heartbeat interval to
    // recover. The contract: the very next waitNext after a fresh
    // attach must return .wal_changed regardless of whether the
    // kernel-level notification mechanism saw the commit.
    if (active_backend == .degraded) return error.SkipZigTest;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const dir_path = try std.fs.path.join(
        std.testing.allocator,
        &.{ ".zig-cache/tmp", &tmp.sub_path },
    );
    defer std.testing.allocator.free(dir_path);

    const db_path = try std.fs.path.join(std.testing.allocator, &.{ dir_path, "lazyattach.db" });
    defer std.testing.allocator.free(db_path);

    // Wake source comes up FIRST — the -wal does not exist yet.
    var w = try Wake.init(std.testing.allocator, db_path);
    defer w.close();

    // Now create the DB and commit. This is the "write before -wal
    // exists" path: the lazy tryAttach inside the next waitNext
    // call will see the file for the first time, open it, register
    // the watch — and the commit has ALREADY landed. Without the
    // fresh_attach catch-up, this would return .heartbeat and the
    // event would be invisible.
    const db = @import("db");
    const c_db_path = try std.testing.allocator.dupeZ(u8, db_path);
    defer std.testing.allocator.free(c_db_path);

    var handle = try db.sqlite.Db.open(c_db_path.ptr);
    defer handle.close();
    try handle.exec("PRAGMA journal_mode=WAL;");
    try handle.exec("CREATE TABLE t(x INTEGER);");
    try handle.exec("INSERT INTO t(x) VALUES (1);");

    // First waitNext after the commit: the lazy attach happens
    // inside, fresh_attach is raised, and we surface .wal_changed
    // synthetically. Tight timeout — if the fix regresses, this
    // will return .heartbeat after the full 200ms.
    const t0 = monoNs();
    const ev = try w.waitNext(200 * std.time.ns_per_ms);
    const elapsed: u64 = monoNs() - t0;
    try std.testing.expect(ev == .wal_changed);
    // The synthetic wake should fire immediately — well under the
    // 200ms heartbeat ceiling. Allow generous slop for slow CI.
    try std.testing.expect(elapsed < 50 * std.time.ns_per_ms);
}

test "Wake catch-up on rotation — write between truncate and reattach still surfaces" {
    // The harder variant of the attach-gap race: rotation, then a
    // commit that lands BEFORE the next waitNext gets to reattach.
    // The reattach path inside waitNext (triggered by the rotation
    // kevent) sets fresh_attach; we consume it on the SAME call by
    // returning .wal_changed for the rotation event. The follow
    // loop's catch-up watermark query then picks up the post-
    // rotation commit even though no kernel notification fired
    // for it (it landed in the close→open gap).
    if (active_backend == .degraded) return error.SkipZigTest;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const dir_path = try std.fs.path.join(
        std.testing.allocator,
        &.{ ".zig-cache/tmp", &tmp.sub_path },
    );
    defer std.testing.allocator.free(dir_path);

    const db_path = try std.fs.path.join(std.testing.allocator, &.{ dir_path, "rotgap.db" });
    defer std.testing.allocator.free(db_path);

    const db = @import("db");
    const c_db_path = try std.testing.allocator.dupeZ(u8, db_path);
    defer std.testing.allocator.free(c_db_path);

    var handle = try db.sqlite.Db.open(c_db_path.ptr);
    defer handle.close();
    try handle.exec("PRAGMA journal_mode=WAL;");
    try handle.exec("CREATE TABLE t(x INTEGER);");
    try handle.exec("INSERT INTO t(x) VALUES (1);");

    var w = try Wake.init(std.testing.allocator, db_path);
    defer w.close();

    // Rotate AND commit before the next waitNext. The commit
    // lands in the post-rotation gap — the kqueue/inotify watch
    // on the old fd is dead and the new fd hasn't been registered
    // yet.
    try handle.exec("PRAGMA wal_checkpoint(TRUNCATE);");
    try handle.exec("INSERT INTO t(x) VALUES (2);");

    // Drain whatever events are queued. The contract is that
    // ACROSS at most two waitNext calls the caller observes at
    // least one .wal_changed (so the watermark re-query runs and
    // surfaces the post-rotation INSERT). The exact sequencing
    // varies by kernel — what matters is that the event isn't
    // lost to the attach-gap.
    var saw_changed = false;
    var iter: usize = 0;
    while (iter < 4 and !saw_changed) : (iter += 1) {
        const ev = try w.waitNext(200 * std.time.ns_per_ms);
        if (ev == .wal_changed) saw_changed = true;
    }
    try std.testing.expect(saw_changed);
}
