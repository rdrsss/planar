//! cli.duration — optional duration-string parser for CLI flags that accept
//! a human-readable interval. Standalone convenience; the core parser does
//! not depend on it.
//!
//! Accepts:
//!
//!   * Bare integers: "0", "600", "10". A bare integer (no unit) is
//!     interpreted as SECONDS for human ergonomics.
//!   * Suffixed values: `<number><unit>` where unit is one of `ns`, `us`,
//!     `ms`, `s`, `m`, `h`. Examples: `500ms`, `10m`, `1h`.
//!
//! Returns `error.InvalidValue` on any malformed input (empty, no digits,
//! unknown unit, mid-string garbage, or a value that overflows `u64`
//! nanoseconds). The caller maps that to its own CLI error reporting.

const std = @import("std");

/// Parse a duration string and return its nanosecond value.
///
/// A bare integer (no unit suffix) is interpreted as SECONDS, so a value
/// like `1` means "one second".
pub fn parseNanos(text: []const u8) !u64 {
    const split = try splitNumUnit(text);
    return scaleToNanos(split.num, split.unit, .seconds);
}

/// Parse a duration string and return its value in whole seconds.
///
/// A bare integer (no unit suffix) is interpreted as SECONDS.
///
/// Sub-second inputs (`500ms`, `100us`, `5ns`) round DOWN to the nearest
/// whole second; values below 1s therefore round to 0. Callers wanting
/// finer granularity should use `parseNanos`.
pub fn parseSeconds(text: []const u8) !i64 {
    const split = try splitNumUnit(text);
    const ns = try scaleToNanos(split.num, split.unit, .seconds);
    const secs = ns / std.time.ns_per_s;
    return std.math.cast(i64, secs) orelse error.InvalidValue;
}

/// Comptime: render a nanosecond count as the largest unit that divides it
/// evenly (e.g. `600000000000` → "10m", `1500000000` → "1500ms"). Used to print
/// `.duration` flag defaults in generated help/man output.
pub fn formatNanos(comptime ns: u64) []const u8 {
    if (ns == 0) return "0s";
    const units = .{
        .{ "h", std.time.ns_per_hour },
        .{ "m", std.time.ns_per_min },
        .{ "s", std.time.ns_per_s },
        .{ "ms", std.time.ns_per_ms },
        .{ "us", std.time.ns_per_us },
        .{ "ns", 1 },
    };
    inline for (units) |u| {
        if (ns % u[1] == 0) return std.fmt.comptimePrint("{d}{s}", .{ ns / u[1], u[0] });
    }
    return std.fmt.comptimePrint("{d}ns", .{ns});
}

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
    // Overflowing the u64 nanosecond range is malformed input, not a value to
    // silently clamp to maxInt — map it to the module's InvalidValue contract.
    if (unit.len == 0) {
        return switch (bare) {
            .seconds => mulChecked(num, std.time.ns_per_s),
            .nanoseconds => num,
        };
    }
    if (std.mem.eql(u8, unit, "ns")) return num;
    if (std.mem.eql(u8, unit, "us")) return mulChecked(num, std.time.ns_per_us);
    if (std.mem.eql(u8, unit, "ms")) return mulChecked(num, std.time.ns_per_ms);
    if (std.mem.eql(u8, unit, "s")) return mulChecked(num, std.time.ns_per_s);
    if (std.mem.eql(u8, unit, "m")) return mulChecked(num, std.time.ns_per_min);
    if (std.mem.eql(u8, unit, "h")) return mulChecked(num, std.time.ns_per_hour);
    return error.InvalidValue;
}

fn mulChecked(a: u64, b: u64) !u64 {
    const product, const overflow = @mulWithOverflow(a, b);
    if (overflow != 0) return error.InvalidValue;
    return product;
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

test "formatNanos picks the largest evenly-dividing unit" {
    try std.testing.expectEqualStrings("0s", formatNanos(0));
    try std.testing.expectEqualStrings("1h", formatNanos(std.time.ns_per_hour));
    try std.testing.expectEqualStrings("10m", formatNanos(10 * std.time.ns_per_min));
    try std.testing.expectEqualStrings("1500ms", formatNanos(1500 * std.time.ns_per_ms));
}

test "duration: overflow is InvalidValue, not saturation" {
    // 18446744073709551615h would overflow u64 nanoseconds; the old code
    // saturated to maxInt(u64), contradicting the InvalidValue contract.
    try std.testing.expectError(error.InvalidValue, parseNanos("18446744073709551615h"));
    try std.testing.expectError(error.InvalidValue, parseNanos("99999999999999999999s"));
}
