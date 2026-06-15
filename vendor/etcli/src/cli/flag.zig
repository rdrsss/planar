//! Flag and Positional spec types for the cli library.
//!
//! `Flag` and `Positional` are pure data structs that the operator declares
//! at comptime in their command tree. The parser and the typed-args generator
//! read these specs at comptime to derive runtime parser behavior and to
//! generate the per-command result struct.

const std = @import("std");
const meta_mod = @import("meta.zig");

/// Value kind for a flag or positional. Drives both parsing and the type of
/// the corresponding field on the generated args struct. `.choice` is a
/// string constrained at parse time to a declared `choices` set.
pub const Kind = enum { bool, string, int, float, duration, path, choice };

/// Comptime-known default value for a flag. The tag must match the flag's
/// declared `kind`; the validator checks this at compile time. A `.duration`
/// value is nanoseconds; a `.path` value is an unvalidated filesystem path.
pub const Default = union(Kind) {
    bool: bool,
    string: []const u8,
    int: i64,
    float: f64,
    duration: u64,
    path: []const u8,
    choice: []const u8,
};

/// Mode for a command-level flag group. Groups are declared on `Cmd` and
/// reference canonical long flag names visible at that command path.
pub const FlagGroupMode = enum {
    mutually_exclusive,
    required_one,
    required_exactly_one,
};

/// Command metadata describing a relationship between multiple visible flags.
/// `flags` contains canonical long names such as "--json"; aliases and short
/// forms resolve to those names in validation/enforcement layers.
pub const FlagGroup = struct {
    name: []const u8,
    mode: FlagGroupMode,
    flags: []const []const u8,
    desc: []const u8 = "",
};

/// A flag spec. `long` is the canonical long form (e.g. "--verbose"); `short`
/// is an optional single-char alias (e.g. 'v' for "-v"). `kind` drives the
/// generated field's type; `default` provides a fallback when the flag is
/// absent. `required = true` errors at parse time if absent and no default.
pub const Flag = struct {
    long: []const u8,
    aliases: []const []const u8 = &.{},
    short: ?u8 = null,
    hidden: bool = false,
    deprecated: ?meta_mod.Deprecation = null,
    desc: []const u8 = "",
    kind: Kind = .string,
    /// When true the flag may repeat (`--tag a --tag b`) and its generated
    /// field is `[]const []const u8` (default empty). v1 supports list elements
    /// of kind `.string`, `.path`, or `.choice`. A list flag takes no `default`.
    list: bool = false,
    /// When true the flag takes no value and counts its occurrences: `-vvv`
    /// (or `--verbose --verbose --verbose`) yields `3`. The generated field is
    /// `u32` (default `0`). Requires `kind == .bool`, and is mutually exclusive
    /// with `list`, `default`, `required`, `choices`, and `--no-` negation.
    count: bool = false,
    /// Manual/help placeholder for non-bool flag values, such as PATH or
    /// COUNT. Parsing is still driven only by `kind`.
    value_name: ?[]const u8 = null,
    /// Allowed values for a `.choice` flag. Required (non-empty) when
    /// `kind == .choice`, and must be empty otherwise. The set drives
    /// parse-time membership validation and auto-populates shell completion.
    choices: []const []const u8 = &.{},
    default: ?Default = null,
    required: bool = false,
    env: ?[]const u8 = null,
    /// Optional value validator run after kind coercion. Receives the raw
    /// argument string; return `null` to accept, or an error-message string to
    /// reject (surfaced as `InvalidValue` with that message).
    validator: ?*const fn ([]const u8) ?[]const u8 = null,
    completion: meta_mod.Completion = .{},
};

/// A positional argument spec. Positionals are consumed in declaration order
/// after the parser has resolved the leaf command. Anything after `--` is
/// passthrough and skipped by positional matching.
pub const Positional = struct {
    name: []const u8,
    desc: []const u8 = "",
    kind: Kind = .string,
    required: bool = true,
    /// Optional default applied when the positional is omitted. Makes the
    /// generated field non-optional. Contradictory with `required = true`.
    default: ?Default = null,
    /// Optional value validator; see `Flag.validator`.
    validator: ?*const fn ([]const u8) ?[]const u8 = null,
    completion: meta_mod.Completion = .{},
};

/// Comptime: the field type for a given Kind. Used to generate args structs.
pub fn ValueType(comptime k: Kind) type {
    return switch (k) {
        .bool => bool,
        .string, .choice, .path => []const u8,
        .int => i64,
        .float => f64,
        .duration => u64,
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

test "FlagGroup declares command-level flag relationships" {
    const group = FlagGroup{
        .name = "output-format",
        .mode = .required_exactly_one,
        .flags = &.{ "--json", "--yaml", "--text" },
        .desc = "Choose one output format.",
    };
    try std.testing.expectEqual(FlagGroupMode.required_exactly_one, group.mode);
    try std.testing.expectEqual(@as(usize, 3), group.flags.len);
    try std.testing.expectEqualStrings("--json", group.flags[0]);
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
