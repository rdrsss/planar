//! follow.zig — Tier-1 poll loop shared across `planar-watch` verbs
//! that support `--follow`.
//!
//! Per tech-spec § "Live tail / follow implementation":
//!
//!   - The PUBLIC CONTRACT is the JSON event shape, the `--follow`
//!     semantics (initial snapshot, then incremental events ordered
//!     by occurrence time), and the watermark column set.
//!   - The TRANSPORT — how the loop knows when to re-query — is
//!     internal and may evolve through the tier ladder without
//!     breaking consumers. M8 ships Tier 1 (poll); M9 swaps in Tier
//!     2 (kqueue / inotify on the SQLite `-wal` file). The poll loop
//!     LIVES IN THIS FILE precisely so the M9 swap is a single-file
//!     change behind the same public surface.
//!
//! SIGINT handling: each follow verb installs a SIGINT handler that
//! flips `interrupted` (a global atomic). The poll loop checks
//! `interrupted` between iterations and returns cleanly on true. The
//! main exits 0 from a graceful interrupt — the operator typed
//! Ctrl-C to STOP, which is success, not an error.
//!
//! Default interval is 1 second (`default_interval_ns`); tests can
//! pass `--interval 100ms` for faster feedback.

const std = @import("std");
const builtin = @import("builtin");
const runtime = @import("runtime");

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

/// Sleep for `ns` nanoseconds, returning early if `interrupted` is
/// flipped during the sleep. Implementation: chunk the sleep into
/// ~100ms slices via the runtime Io and check the flag between them
/// so SIGINT latency is bounded regardless of the configured
/// `--interval`.
///
/// Uses the process Io from `runtime.current()` so the sleep honors
/// the same threading-model the rest of the binary uses.
pub fn interruptibleSleep(ns: u64) void {
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
