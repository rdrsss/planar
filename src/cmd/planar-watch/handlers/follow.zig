//! follow.zig — Tier-2 wake loop shared across `planar-watch` verbs
//! that support `--follow`.
//!
//! Per tech-spec § "Live tail / follow implementation":
//!
//!   - The PUBLIC CONTRACT is the JSON event shape, the `--follow`
//!     semantics (initial snapshot, then incremental events ordered
//!     by occurrence time), and the watermark column set.
//!   - The TRANSPORT — how the loop knows when to re-query — is
//!     internal and evolves through the tier ladder without breaking
//!     consumers. M8 shipped Tier 1 (sleep-based poll); M9 (this
//!     file) shipped Tier 2 (kqueue on macOS/BSD, inotify on Linux,
//!     fall back to Tier-1 sleep on unsupported platforms). The
//!     wake-source abstraction lives in
//!     `engine.runtime.agentactivity.wake`; this file only orchestrates
//!     the loop body. `--interval` is now a HEARTBEAT (maximum poll
//!     fallback) under Tier 2: the wake fires on -wal change, but
//!     the interval still fires as a safety-net so a watcher that
//!     missed a wake event (e.g. coalesced notifications across a
//!     laptop sleep) recovers within one heartbeat.
//!
//! SIGINT handling: each follow verb installs a SIGINT handler that
//! flips `interrupted` (a global atomic). The wake loop checks
//! `interrupted` between iterations and returns cleanly on true. The
//! main exits 0 from a graceful interrupt — the operator typed
//! Ctrl-C to STOP, which is success, not an error.
//!
//! Default interval is 1 second (`default_interval_ns`); tests can
//! pass `--interval 100ms` for faster feedback.

const std = @import("std");
const builtin = @import("builtin");
const runtime = @import("runtime");
const engine = @import("engine");

const Wake = engine.runtime.agentactivity.wake.Wake;

/// Default poll interval (1 second). Tests override via `--interval`.
pub const default_interval_ns: u64 = 1 * std.time.ns_per_s;

/// Global "should I keep polling" flag. Flipped by the SIGINT handler
/// installed by `installSigintHandler`; checked by the poll loop
/// between iterations. Atomic because the signal handler is
/// asynchronous w.r.t. the main thread.
pub var interrupted: std.atomic.Value(bool) = .init(false);

/// True if SIGINT was received and the follow loop should exit.
pub fn shouldStop() bool {
    return interrupted.load(.acquire);
}

/// Install a SIGINT handler that flips `interrupted` and returns. The
/// poll loop sees the flip on its next iteration and exits cleanly.
///
/// POSIX-only — Windows builds skip signal installation (the follow
/// loop on Windows relies on Ctrl-C terminating the process at the OS
/// level, since std doesn't yet expose a portable signal API). Best-
/// effort: failure to install is logged but not fatal — the operator
/// can still `kill` the process from another terminal.
pub fn installSigintHandler() void {
    if (builtin.os.tag == .windows) return;
    const posix = std.posix;
    var act: posix.Sigaction = .{
        .handler = .{ .handler = sigintHandler },
        .mask = posix.sigemptyset(),
        .flags = 0,
    };
    posix.sigaction(posix.SIG.INT, &act, null);
}

fn sigintHandler(_: std.posix.SIG) callconv(.c) void {
    interrupted.store(true, .release);
}

/// Parse a duration spec into nanoseconds. Accepts bare integers
/// (interpreted as seconds for human ergonomics) and units `ns`,
/// `us`, `ms`, `s`. Returns `error.InvalidValue` on a malformed
/// input.
///
/// Examples:
///   "1"     → 1_000_000_000  (1 second)
///   "100ms" → 100_000_000    (100 ms)
///   "500ns" → 500
pub fn parseDurationNs(text: []const u8) !u64 {
    if (text.len == 0) return error.InvalidValue;

    // Find where the digits end.
    var i: usize = 0;
    while (i < text.len and (text[i] == '.' or (text[i] >= '0' and text[i] <= '9'))) i += 1;
    if (i == 0) return error.InvalidValue;

    const num_text = text[0..i];
    const unit = std.mem.trim(u8, text[i..], " \t");

    const num = std.fmt.parseInt(u64, num_text, 10) catch return error.InvalidValue;

    if (unit.len == 0 or std.mem.eql(u8, unit, "s")) return num *| std.time.ns_per_s;
    if (std.mem.eql(u8, unit, "ms")) return num *| std.time.ns_per_ms;
    if (std.mem.eql(u8, unit, "us")) return num *| std.time.ns_per_us;
    if (std.mem.eql(u8, unit, "ns")) return num;

    return error.InvalidValue;
}

/// Module-local wake source, initialized lazily on first call to
/// `interruptibleSleep`. The follow loop is single-threaded (one
/// per planar-watch process), so a singleton is safe.
var wake_storage: ?Wake = null;
var wake_init_failed: bool = false;

/// Wait up to `ns` nanoseconds for a `-wal` change, returning early
/// if `interrupted` is flipped. On Tier 2 platforms (kqueue/inotify)
/// this returns the moment SQLite commits to the WAL; on degraded
/// platforms it falls back to the M8 Tier-1 sleep-and-poll loop.
///
/// The `ns` argument is now a HEARTBEAT cadence — the maximum we'll
/// block before returning even if no kernel notification arrived.
/// That is the operator-visible meaning of `--interval` under Tier 2
/// and is the safety net for coalesced / missed kqueue events.
///
/// SIGINT latency is bounded by chunking the wait into ≤ 100ms
/// slices and checking `interrupted` between them. The wake source
/// itself is also interrupted by the signal (EINTR), so the slice
/// is the worst case, not the typical case.
pub fn interruptibleSleep(ns: u64) void {
    // Wake source is lazy-initialized so a non-follow path never
    // pays for it. Once initialized, every iteration reuses the
    // same fds.
    if (wake_storage == null and !wake_init_failed) {
        const ctx = runtime.current();
        wake_storage = Wake.init(ctx.allocator, ctx.db_path) catch blk: {
            wake_init_failed = true;
            break :blk null;
        };
    }

    // Fall back to a plain interruptible sleep when the wake source
    // failed to initialize (e.g. resource exhaustion). The poll-loop
    // contract holds either way; only the wake latency degrades.
    if (wake_storage == null) {
        return interruptibleSleepLegacy(ns);
    }

    const slice_ns: u64 = 100 * std.time.ns_per_ms;
    var remaining = ns;
    while (remaining > 0) {
        if (shouldStop()) return;
        const this_slice = if (remaining < slice_ns) remaining else slice_ns;
        const ev = wake_storage.?.waitNext(this_slice) catch .heartbeat;
        switch (ev) {
            // A -wal change landed — return now so the caller can
            // re-query the watermark. The remaining heartbeat budget
            // is consumed by the query; the next loop iteration
            // starts a fresh `waitNext`.
            .wal_changed => return,
            // SIGINT (EINTR). The shouldStop check at the top of
            // the next iteration will exit the outer poll loop.
            .interrupted => return,
            // Plain heartbeat — keep counting down so the configured
            // interval is still honored as the maximum delay.
            .heartbeat => {},
        }
        remaining -= this_slice;
    }
}

/// Legacy Tier-1 sleep, retained as the fallback when wake init
/// fails. Identical to the M8 implementation: chunked sleep slices
/// via the runtime Io, ~100ms each so SIGINT latency stays bounded
/// regardless of the configured `--interval`.
fn interruptibleSleepLegacy(ns: u64) void {
    const slice_ns: u64 = 100 * std.time.ns_per_ms;
    var remaining = ns;
    const ctx = runtime.current();
    while (remaining > 0) {
        if (shouldStop()) return;
        const this_slice = if (remaining < slice_ns) remaining else slice_ns;
        const dur: std.Io.Duration = .{ .nanoseconds = @as(i96, @intCast(this_slice)) };
        ctx.io.sleep(dur, .awake) catch return;
        remaining -= this_slice;
    }
}

/// Release the wake source. Idempotent. Called from the binary's
/// shutdown path so kqueue/inotify fds are returned to the kernel
/// promptly — not strictly required since the process is exiting,
/// but tidy.
pub fn closeWake() void {
    if (wake_storage) |*w| {
        w.close();
        wake_storage = null;
    }
    wake_init_failed = false;
}

test "parseDurationNs accepts bare seconds, ms, us, ns" {
    try std.testing.expectEqual(@as(u64, std.time.ns_per_s), try parseDurationNs("1"));
    try std.testing.expectEqual(@as(u64, std.time.ns_per_s), try parseDurationNs("1s"));
    try std.testing.expectEqual(@as(u64, 100 * std.time.ns_per_ms), try parseDurationNs("100ms"));
    try std.testing.expectEqual(@as(u64, 500), try parseDurationNs("500ns"));
}

test "parseDurationNs rejects malformed input" {
    try std.testing.expectError(error.InvalidValue, parseDurationNs(""));
    try std.testing.expectError(error.InvalidValue, parseDurationNs("abc"));
    try std.testing.expectError(error.InvalidValue, parseDurationNs("1xyz"));
}

// Keep the runtime import live so this module's tests can be
// reached from the binary's umbrella test pass without an orphaned
// import warning.
test {
    _ = runtime;
}
