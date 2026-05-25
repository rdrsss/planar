//! Flag and Positional spec types for the cli library.
//!
//! `Flag` and `Positional` are pure data structs that the operator declares
//! at comptime in their command tree. The parser and the typed-args generator
//! read these specs at comptime to derive runtime parser behavior and to
//! generate the per-command result struct.

const std = @import("std");

/// Value kind for a flag or positional. Drives both parsing and the type of
/// the corresponding field on the generated args struct.
pub const Kind = enum { bool, string, int };

/// Comptime-known default value for a flag. The tag must match the flag's
/// declared `kind`; the validator checks this at compile time.
pub const Default = union(Kind) {
    bool: bool,
    string: []const u8,
    int: i64,
};

/// A flag spec. `long` is the canonical long form (e.g. "--verbose"); `short`
/// is an optional single-char alias (e.g. 'v' for "-v"). `kind` drives the
/// generated field's type; `default` provides a fallback when the flag is
/// absent. `required = true` errors at parse time if absent and no default.
pub const Flag = struct {
    long: []const u8,
    short: ?u8 = null,
    desc: []const u8 = "",
    kind: Kind = .string,
    default: ?Default = null,
    required: bool = false,
    env: ?[]const u8 = null,
};

/// A positional argument spec. Positionals are consumed in declaration order
/// after the parser has resolved the leaf command. Anything after `--` is
/// passthrough and skipped by positional matching.
pub const Positional = struct {
    name: []const u8,
    desc: []const u8 = "",
    kind: Kind = .string,
    required: bool = true,
};

/// Comptime: the field type for a given Kind. Used to generate args structs.
pub fn ValueType(comptime k: Kind) type {
    return switch (k) {
        .bool => bool,
        .string => []const u8,
        .int => i64,
    };
}

/// Comptime: derive a Zig-safe field name from a flag's `--long-name`.
/// Strips the leading "--" and replaces hyphens with underscores. The
/// 0.16 type-reify builtins take `[]const u8` field names; no sentinel
/// is required.
pub fn flagFieldName(comptime flag: Flag) []const u8 {
    const stripped = comptime stripDashes(flag.long);
    return comptime hyphenToUnderscore(stripped);
}

/// Comptime: derive a Zig-safe field name from a positional's `name`.
pub fn positionalFieldName(comptime p: Positional) []const u8 {
    return comptime hyphenToUnderscore(p.name);
}

fn stripDashes(comptime s: []const u8) []const u8 {
    if (std.mem.startsWith(u8, s, "--")) return s[2..];
    if (s.len > 0 and s[0] == '-') return s[1..];
    return s;
}

fn hyphenToUnderscore(comptime s: []const u8) []const u8 {
    comptime {
        var buf: [s.len]u8 = undefined;
        for (s, 0..) |c, i| {
            buf[i] = if (c == '-') '_' else c;
        }
        const final = buf;
        return &final;
    }
}

// ---- tests ----

test "ValueType maps Kind to the right Zig type" {
    try std.testing.expectEqual(bool, ValueType(.bool));
    try std.testing.expectEqual([]const u8, ValueType(.string));
    try std.testing.expectEqual(i64, ValueType(.int));
}

test "flagFieldName strips leading -- and converts hyphens" {
    const f = Flag{ .long = "--no-auto-promote" };
    try std.testing.expectEqualStrings("no_auto_promote", flagFieldName(f));
}

test "flagFieldName handles single-dash flag" {
    const f = Flag{ .long = "-x" };
    try std.testing.expectEqualStrings("x", flagFieldName(f));
}

test "positionalFieldName converts hyphens" {
    const p = Positional{ .name = "task-id" };
    try std.testing.expectEqualStrings("task_id", positionalFieldName(p));
}
