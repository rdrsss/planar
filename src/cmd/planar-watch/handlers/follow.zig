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
const cli = @import("cli");
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
/// `us`, `ms`, `s`, `m`, `h`. Returns `error.InvalidValue` on a
/// malformed input.
///
/// Examples:
///   "1"     → 1_000_000_000  (1 second)
///   "100ms" → 100_000_000    (100 ms)
///   "500ns" → 500
///   "10m"   → 600_000_000_000 (10 minutes)
///
/// Implementation delegates to `cli.duration.parseNanos` so every flag
/// that accepts a human duration uses the same grammar.
pub fn parseDurationNs(text: []const u8) !u64 {
    return cli.duration.parseNanos(text);
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
///
/// Side effect — DB refresh: before returning to the caller, the
/// strict read-only DB handle is closed and re-opened. This
/// dodges a SQLite behavior where a long-lived read-only
/// connection's wrapped read transaction holds a stale snapshot
/// across another process's `PRAGMA wal_checkpoint(TRUNCATE)`
/// (the SHM-resident WAL header is reset by the truncate; the
/// pure-readonly connection cannot write its read-mark slot back
/// into SHM, so it never re-syncs to the new header and committed
/// UPDATE rows stay invisible to the wrapped reader forever).
/// Closing + re-opening is the smallest correct intervention — it
/// resets the connection's snapshot tracking cleanly with no
/// observable cost on the follow loop's per-wake cadence. See
/// plan 85 t#2623 for the regression. Every follow verb in this
/// binary funnels through this routine, so the fix is centralized.
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
            .wal_changed => {
                refreshDb();
                return;
            },
            // SIGINT (EINTR). The shouldStop check at the top of
            // the next iteration will exit the outer poll loop.
            .interrupted => return,
            // Plain heartbeat — keep counting down so the configured
            // interval is still honored as the maximum delay.
            .heartbeat => {},
        }
        remaining -= this_slice;
    }
    // Heartbeat path falls through here. Refresh on the way out so
    // the caller's next watermark query runs against a fresh
    // snapshot — the WAL-rotation issue documented above can also
    // surface on the heartbeat-only path under high writer churn.
    refreshDb();
}

/// Close + re-open the strict read-only DB singleton. Centralizes
/// the WAL-rotation snapshot-staleness workaround so every follow
/// verb in this binary inherits it via `interruptibleSleep` and
/// `interruptibleSleepLegacy`. Best-effort: a refresh failure
/// leaves the prior handle closed and surfaces as a query error
/// on the next iteration; the follow loop dies cleanly via the
/// per-verb `exit.die` rather than silently masking the state.
fn refreshDb() void {
    _ = runtime.refreshDbStrictReadOnly() catch {};
}

/// Legacy Tier-1 sleep, retained as the fallback when wake init
/// fails. Identical to the M8 implementation: chunked sleep slices
/// via the runtime Io, ~100ms each so SIGINT latency stays bounded
/// regardless of the configured `--interval`. Same DB-refresh
/// post-condition as `interruptibleSleep`.
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
    refreshDb();
}

/// Format the duration between `now_ms` (milliseconds since UNIX epoch)
/// and the timestamp represented by `then_iso` (an ISO 8601 UTC string
/// in the form `YYYY-MM-DDTHH:MM:SS.mmmZ` as emitted by SQLite's
/// `strftime('%Y-%m-%dT%H:%M:%fZ','now')`).
///
/// Output conventions:
///   delta < 5s    → "just now"   (covers future timestamps gracefully)
///   delta < 60s   → "Ns ago"
///   delta < 3600s → "Nm ago"
///   delta < 86400s→ "Nh ago"
///   delta ≥ 86400s→ "Nd ago"
///
/// The returned slice is caller-owned and allocated via `allocator`.
/// Returns `error.InvalidValue` when `then_iso` cannot be parsed.
/// Returns `error.OutOfMemory` on allocation failure.
pub fn relativeTime(allocator: std.mem.Allocator, now_ms: i64, then_iso: []const u8) ![]const u8 {
    const then_ms = isoToMs(then_iso) catch return error.InvalidValue;
    const delta_ms = now_ms - then_ms;

    // Future timestamps and near-zero deltas both map to "just now".
    if (delta_ms < 5_000) {
        return allocator.dupe(u8, "just now");
    }

    const delta_s = @divFloor(delta_ms, 1_000);
    if (delta_s < 60) {
        return std.fmt.allocPrint(allocator, "{d}s ago", .{delta_s});
    }
    const delta_m = @divFloor(delta_s, 60);
    if (delta_m < 60) {
        return std.fmt.allocPrint(allocator, "{d}m ago", .{delta_m});
    }
    const delta_h = @divFloor(delta_m, 60);
    if (delta_h < 24) {
        return std.fmt.allocPrint(allocator, "{d}h ago", .{delta_h});
    }
    const delta_d = @divFloor(delta_h, 24);
    return std.fmt.allocPrint(allocator, "{d}d ago", .{delta_d});
}

/// Parse an ISO 8601 UTC string of the form produced by SQLite's
/// `strftime('%Y-%m-%dT%H:%M:%fZ','now')`:
///   `YYYY-MM-DDTHH:MM:SS.mmmZ`  (millisecond precision)
///   `YYYY-MM-DDTHH:MM:SSZ`      (second precision, e.g. older rows)
///
/// Returns milliseconds since UNIX epoch, or `error.InvalidValue` on
/// any parse failure.
fn isoToMs(iso: []const u8) error{InvalidValue}!i64 {
    // Minimum valid: "YYYY-MM-DDTHH:MM:SSZ" = 20 chars
    if (iso.len < 20) return error.InvalidValue;
    // Must end with 'Z'
    if (iso[iso.len - 1] != 'Z') return error.InvalidValue;

    const year = parseInt4(iso[0..4]) catch return error.InvalidValue;
    if (iso[4] != '-') return error.InvalidValue;
    const month = parseInt2(iso[5..7]) catch return error.InvalidValue;
    if (iso[7] != '-') return error.InvalidValue;
    const day = parseInt2(iso[8..10]) catch return error.InvalidValue;
    if (iso[10] != 'T') return error.InvalidValue;
    const hour = parseInt2(iso[11..13]) catch return error.InvalidValue;
    if (iso[13] != ':') return error.InvalidValue;
    const min = parseInt2(iso[14..16]) catch return error.InvalidValue;
    if (iso[16] != ':') return error.InvalidValue;
    const sec = parseInt2(iso[17..19]) catch return error.InvalidValue;

    // Optional sub-second: ".NNN" before the 'Z'
    var ms: i64 = 0;
    if (iso.len > 20 and iso[19] == '.') {
        // Fraction part: up to 3 digits before 'Z'
        const frac_end = iso.len - 1; // points at 'Z'
        const frac_start: usize = 20;
        const frac_len = frac_end - frac_start;
        if (frac_len == 0 or frac_len > 3) {
            // Non-standard precision — truncate or reject conservatively.
            if (frac_len > 3) {
                // Take only the first 3 digits.
                ms = parseInt3(iso[frac_start .. frac_start + 3]) catch return error.InvalidValue;
            }
        } else {
            var raw = parseInt3(iso[frac_start..frac_end]) catch return error.InvalidValue;
            // Normalise: ".1" → 100ms, ".12" → 120ms, ".123" → 123ms
            if (frac_len == 1) raw *= 100;
            if (frac_len == 2) raw *= 10;
            ms = raw;
        }
    } else if (iso.len != 20) {
        // "YYYY-MM-DDTHH:MM:SSZ" is exactly 20 chars; anything else is invalid.
        return error.InvalidValue;
    }

    // Validate ranges.
    if (month < 1 or month > 12) return error.InvalidValue;
    if (day < 1 or day > 31) return error.InvalidValue;
    if (hour > 23 or min > 59 or sec > 60) return error.InvalidValue; // 60 for leap seconds

    // Days since UNIX epoch (1970-01-01). Use std.time.epoch helpers.
    const year_u: u32 = @intCast(year);
    // Count days from epoch year (1970) to start of `year`.
    var days: i64 = 0;
    var y: u32 = 1970;
    while (y < year_u) : (y += 1) {
        days += if (isLeapYear(y)) 366 else 365;
    }
    if (year_u < 1970) {
        // Handle dates before epoch by going backward.
        y = year_u;
        while (y < 1970) : (y += 1) {
            days -= if (isLeapYear(y)) 366 else 365;
        }
    }

    // Days within the year up to the start of `month`.
    const leap = isLeapYear(year_u);
    const month_days = [_]u8{ 31, if (leap) 29 else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    var m: u8 = 1;
    while (m < month) : (m += 1) {
        days += month_days[m - 1];
    }

    days += @as(i64, day) - 1;

    const total_secs = days * 86400 +
        @as(i64, hour) * 3600 +
        @as(i64, min) * 60 +
        @as(i64, sec);
    return total_secs * 1000 + ms;
}

fn isLeapYear(y: u32) bool {
    return (y % 4 == 0 and y % 100 != 0) or (y % 400 == 0);
}

fn parseInt4(s: []const u8) !i64 {
    if (s.len != 4) return error.InvalidValue;
    return std.fmt.parseInt(i64, s, 10);
}

fn parseInt3(s: []const u8) !i64 {
    if (s.len > 3) return error.InvalidValue;
    return std.fmt.parseInt(i64, s, 10);
}

fn parseInt2(s: []const u8) !i64 {
    if (s.len != 2) return error.InvalidValue;
    return std.fmt.parseInt(i64, s, 10);
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

// A representative "now" anchor for relativeTime unit tests.
// 2026-05-29T12:00:00.000Z = 1780056000000 ms since epoch.
const test_now_ms: i64 = 1_780_056_000_000;

test "relativeTime: < 5s returns 'just now'" {
    const a = std.testing.allocator;
    // then_iso is 3 seconds before now_ms.
    // now_ms = 1748520000000  → 2026-05-29T12:00:00.000Z
    // minus 3s               → 2026-05-29T11:59:57.000Z
    const s = try relativeTime(a, test_now_ms, "2026-05-29T11:59:57.000Z");
    defer a.free(s);
    try std.testing.expectEqualStrings("just now", s);
}

test "relativeTime: 15s ago" {
    const a = std.testing.allocator;
    // 15 seconds before anchor
    const s = try relativeTime(a, test_now_ms, "2026-05-29T11:59:45.000Z");
    defer a.free(s);
    try std.testing.expectEqualStrings("15s ago", s);
}

test "relativeTime: 3m ago" {
    const a = std.testing.allocator;
    // 3 minutes = 180 seconds before anchor
    const s = try relativeTime(a, test_now_ms, "2026-05-29T11:57:00.000Z");
    defer a.free(s);
    try std.testing.expectEqualStrings("3m ago", s);
}

test "relativeTime: 2h ago" {
    const a = std.testing.allocator;
    // 2 hours = 7200 seconds before anchor
    const s = try relativeTime(a, test_now_ms, "2026-05-29T10:00:00.000Z");
    defer a.free(s);
    try std.testing.expectEqualStrings("2h ago", s);
}

test "relativeTime: future timestamp returns 'just now'" {
    const a = std.testing.allocator;
    // 5 seconds in the future relative to anchor
    const s = try relativeTime(a, test_now_ms, "2026-05-29T12:00:05.000Z");
    defer a.free(s);
    try std.testing.expectEqualStrings("just now", s);
}

test "relativeTime: > 24h returns Nd ago" {
    const a = std.testing.allocator;
    // 36 hours = 1.5 days before anchor → 1d ago
    const s = try relativeTime(a, test_now_ms, "2026-05-28T00:00:00.000Z");
    defer a.free(s);
    // 36h / 24 = 1d
    try std.testing.expectEqualStrings("1d ago", s);
}

test "relativeTime: 5d ago" {
    const a = std.testing.allocator;
    // 5 days before anchor
    const s = try relativeTime(a, test_now_ms, "2026-05-24T12:00:00.000Z");
    defer a.free(s);
    try std.testing.expectEqualStrings("5d ago", s);
}

test "relativeTime: rejects malformed ISO" {
    const a = std.testing.allocator;
    try std.testing.expectError(error.InvalidValue, relativeTime(a, test_now_ms, "not-a-date"));
    try std.testing.expectError(error.InvalidValue, relativeTime(a, test_now_ms, ""));
}

test "parseDurationNs accepts bare seconds, ms, us, ns, m, h" {
    try std.testing.expectEqual(@as(u64, std.time.ns_per_s), try parseDurationNs("1"));
    try std.testing.expectEqual(@as(u64, std.time.ns_per_s), try parseDurationNs("1s"));
    try std.testing.expectEqual(@as(u64, 100 * std.time.ns_per_ms), try parseDurationNs("100ms"));
    try std.testing.expectEqual(@as(u64, 500), try parseDurationNs("500ns"));
    try std.testing.expectEqual(@as(u64, 10 * std.time.ns_per_min), try parseDurationNs("10m"));
    try std.testing.expectEqual(@as(u64, std.time.ns_per_hour), try parseDurationNs("1h"));
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
