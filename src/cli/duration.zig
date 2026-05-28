//! cli.duration — shared duration-string parser used by every CLI flag
//! that accepts a human-readable interval (e.g. `--ttl`, `--stale-after`,
//! `--interval`).
//!
//! Accepts:
//!
//!   * Bare integers (back-compat): "0", "600", "10". Interpreted in the
//!     caller-supplied unit via `parseAsSeconds` (seconds) or
//!     `parseAsNanos` (nanoseconds; bare integer is seconds for human
//!     ergonomics, matching the legacy `--interval` parser).
//!   * ISO-style suffixed values: `<number><unit>` where unit is one of
//!     `ns`, `us`, `ms`, `s`, `m`, `h`. Examples: `500ms`, `10m`, `1h`.
//!
//! Returns `error.InvalidValue` on any malformed input (empty, no digits,
//! unknown unit, mid-string garbage). The caller is responsible for
//! mapping that to its own CLI error reporting.
//!
//! The integer-second back-compat path is REQUIRED — existing scripts and
//! integration tests pass bare ints to `--ttl 600`, `--stale-after 0`,
//! etc. Both forms must coexist; the new suffix forms are purely
//! additive.

const std = @import("std");

/// Parse a duration string and return its nanosecond value.
///
/// A bare integer (no unit suffix) is interpreted as SECONDS — that
/// matches the legacy `planar-watch --interval` parser semantics so
/// `--interval 1` continues to mean "one second".
pub fn parseNanos(text: []const u8) !u64 {
    const split = try splitNumUnit(text);
    return scaleToNanos(split.num, split.unit, .seconds);
}

/// Parse a duration string and return its value in whole seconds.
///
/// A bare integer (no unit suffix) is interpreted as SECONDS, matching
/// the legacy `--ttl <int>` / `--stale-after <int>` contract that
/// existing scripts and tests depend on.
///
/// Sub-second inputs (`500ms`, `100us`, `5ns`) round DOWN to the nearest
/// whole second; values below 1s therefore round to 0. The `--ttl` /
/// `--stale-after` surface is documented as seconds-granularity so this
/// truncation is the deliberate contract — operators wanting finer
/// granularity should be using `--interval`, which routes through
/// `parseNanos`.
pub fn parseSeconds(text: []const u8) !i64 {
    const split = try splitNumUnit(text);
    const ns = try scaleToNanos(split.num, split.unit, .seconds);
    const secs = ns / std.time.ns_per_s;
    return std.math.cast(i64, secs) orelse error.InvalidValue;
}

/// Backwards-compat alias for the legacy `parseDurationNs` name that
/// `planar-watch follow.zig` and `ps.zig` import. Kept so the wake-loop
/// module doesn't need to chase a rename.
pub const parseDurationNs = parseNanos;

const BareUnit = enum { seconds, nanoseconds };

const Split = struct { num: u64, unit: []const u8 };

fn splitNumUnit(text: []const u8) !Split {
    if (text.len == 0) return error.InvalidValue;

    var i: usize = 0;
    while (i < text.len and text[i] >= '0' and text[i] <= '9') i += 1;
    if (i == 0) return error.InvalidValue;

    const num = std.fmt.parseInt(u64, text[0..i], 10) catch return error.InvalidValue;
    const unit = std.mem.trim(u8, text[i..], " \t");
    return .{ .num = num, .unit = unit };
}

fn scaleToNanos(num: u64, unit: []const u8, bare: BareUnit) !u64 {
    if (unit.len == 0) {
        return switch (bare) {
            .seconds => num *| std.time.ns_per_s,
            .nanoseconds => num,
        };
    }
    if (std.mem.eql(u8, unit, "ns")) return num;
    if (std.mem.eql(u8, unit, "us")) return num *| std.time.ns_per_us;
    if (std.mem.eql(u8, unit, "ms")) return num *| std.time.ns_per_ms;
    if (std.mem.eql(u8, unit, "s")) return num *| std.time.ns_per_s;
    if (std.mem.eql(u8, unit, "m")) return num *| std.time.ns_per_min;
    if (std.mem.eql(u8, unit, "h")) return num *| std.time.ns_per_hour;
    return error.InvalidValue;
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

test "parseNanos: bare integer is seconds" {
    try std.testing.expectEqual(@as(u64, 0), try parseNanos("0"));
    try std.testing.expectEqual(@as(u64, std.time.ns_per_s), try parseNanos("1"));
    try std.testing.expectEqual(@as(u64, 600 * std.time.ns_per_s), try parseNanos("600"));
}

test "parseNanos: ns/us/ms/s/m/h units" {
    try std.testing.expectEqual(@as(u64, 500), try parseNanos("500ns"));
    try std.testing.expectEqual(@as(u64, 7 * std.time.ns_per_us), try parseNanos("7us"));
    try std.testing.expectEqual(@as(u64, 500 * std.time.ns_per_ms), try parseNanos("500ms"));
    try std.testing.expectEqual(@as(u64, std.time.ns_per_s), try parseNanos("1s"));
    try std.testing.expectEqual(@as(u64, 10 * std.time.ns_per_min), try parseNanos("10m"));
    try std.testing.expectEqual(@as(u64, std.time.ns_per_hour), try parseNanos("1h"));
}

test "parseSeconds: bare integer round-trips" {
    try std.testing.expectEqual(@as(i64, 0), try parseSeconds("0"));
    try std.testing.expectEqual(@as(i64, 600), try parseSeconds("600"));
    try std.testing.expectEqual(@as(i64, 1), try parseSeconds("1"));
}

test "parseSeconds: suffixed units convert" {
    try std.testing.expectEqual(@as(i64, 600), try parseSeconds("10m"));
    try std.testing.expectEqual(@as(i64, 3600), try parseSeconds("1h"));
    try std.testing.expectEqual(@as(i64, 5), try parseSeconds("5s"));
    try std.testing.expectEqual(@as(i64, 5000), try parseSeconds("5000s"));
}

test "parseSeconds: sub-second values round down" {
    try std.testing.expectEqual(@as(i64, 0), try parseSeconds("500ms"));
    try std.testing.expectEqual(@as(i64, 0), try parseSeconds("999ms"));
    try std.testing.expectEqual(@as(i64, 1), try parseSeconds("1000ms"));
    try std.testing.expectEqual(@as(i64, 0), try parseSeconds("100us"));
    try std.testing.expectEqual(@as(i64, 0), try parseSeconds("500ns"));
}

test "parseNanos: malformed input" {
    try std.testing.expectError(error.InvalidValue, parseNanos(""));
    try std.testing.expectError(error.InvalidValue, parseNanos("foo"));
    try std.testing.expectError(error.InvalidValue, parseNanos("5xx"));
    try std.testing.expectError(error.InvalidValue, parseNanos("1xyz"));
    try std.testing.expectError(error.InvalidValue, parseNanos("abc"));
    try std.testing.expectError(error.InvalidValue, parseNanos("ms"));
    try std.testing.expectError(error.InvalidValue, parseNanos("-5s"));
}

test "parseSeconds: malformed input" {
    try std.testing.expectError(error.InvalidValue, parseSeconds(""));
    try std.testing.expectError(error.InvalidValue, parseSeconds("foo"));
    try std.testing.expectError(error.InvalidValue, parseSeconds("5xx"));
    try std.testing.expectError(error.InvalidValue, parseSeconds("10x"));
}
