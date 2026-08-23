//! Parse-error types and formatter.
//!
//! Errors are values, not panics, and not auto-stderr-prints. The parser
//! returns a `Parse` error union; if the caller wants a friendly message,
//! they hand the returned `Detail` to `format` and decide where to write it.

const std = @import("std");
const flag_mod = @import("flag.zig");

/// Tagged error kinds returned from `parser.parse`. Each kind is matched
/// against a `Detail` payload that carries the offending input slice and
/// any structured context (which command, which flag, etc.).
pub const Parse = error{
    UnknownFlag,
    MissingValue,
    InvalidValue,
    MissingRequired,
    MissingRequiredPositional,
    TooManyPositionals,
    UnknownSubcommand,
    UnexpectedArgument,
    DuplicateFlag,
    FlagGroupViolation,
};

/// Structured error context. The parser fills this in alongside returning a
/// `Parse` error so the caller can render a useful message via `format`.
/// Pointers are into the argv / cmd tree the caller owns; no allocation.
pub const Detail = struct {
    kind: Parse,
    /// The offending argv slice, when applicable.
    arg: ?[]const u8 = null,
    /// The flag long name, when applicable.
    flag: ?[]const u8 = null,
    /// The positional name, when applicable.
    positional: ?[]const u8 = null,
    /// The matched-so-far command path (space-separated), useful in
    /// "unknown subcommand of `tool task ...`" style messages.
    cmd_path: ?[]const u8 = null,
    /// Optional nearest known flag or subcommand spelling.
    suggestion: ?[]const u8 = null,
    /// Optional custom message from a value validator.
    message: ?[]const u8 = null,
    /// The flag group name, when applicable.
    group: ?[]const u8 = null,
    /// The flag group mode, when applicable.
    group_mode: ?flag_mod.FlagGroupMode = null,
    /// Canonical long flag names involved in a group violation.
    group_flags: []const []const u8 = &.{},
};

/// A `Detail` plus a stable, machine-readable `kind_name` string. Use this
/// (via `structured`) when emitting errors as JSON or structured logs so
/// consumers can branch on `kind_name` rather than a localized message. The
/// `kind_name` values are part of the public contract (see `docs/release.md`).
/// Re-exported from the package root as `StructuredError`.
pub const Structured = struct {
    kind: Parse,
    kind_name: []const u8,
    arg: ?[]const u8 = null,
    flag: ?[]const u8 = null,
    positional: ?[]const u8 = null,
    cmd_path: ?[]const u8 = null,
    suggestion: ?[]const u8 = null,
    message: ?[]const u8 = null,
    group: ?[]const u8 = null,
    group_mode: ?flag_mod.FlagGroupMode = null,
    group_flags: []const []const u8 = &.{},
};

/// Convert a `Detail` into a `Structured` value, attaching the stable
/// `kind_name`. No allocation; string fields alias the original `Detail`.
/// Re-exported from the package root as `structuredError`.
pub fn structured(detail: Detail) Structured {
    return .{
        .kind = detail.kind,
        .kind_name = kindName(detail.kind),
        .arg = detail.arg,
        .flag = detail.flag,
        .positional = detail.positional,
        .cmd_path = detail.cmd_path,
        .suggestion = detail.suggestion,
        .message = detail.message,
        .group = detail.group,
        .group_mode = detail.group_mode,
        .group_flags = detail.group_flags,
    };
}

/// Stable snake_case name for a `Parse` error kind, suitable for JSON output
/// and structured logs. These strings are part of the public contract — treat
/// a rename as a breaking change. Re-exported from the package root as
/// `errorKindName`.
pub fn kindName(kind: Parse) []const u8 {
    return switch (kind) {
        Parse.UnknownFlag => "unknown_flag",
        Parse.MissingValue => "missing_value",
        Parse.InvalidValue => "invalid_value",
        Parse.MissingRequired => "missing_required",
        Parse.MissingRequiredPositional => "missing_required_positional",
        Parse.TooManyPositionals => "too_many_positionals",
        Parse.UnknownSubcommand => "unknown_subcommand",
        Parse.UnexpectedArgument => "unexpected_argument",
        Parse.DuplicateFlag => "duplicate_flag",
        Parse.FlagGroupViolation => "flag_group_violation",
    };
}

/// Render a parse error to a writer in the form `error: <kind>: <context>`.
/// Caller decides whether stdout, stderr, or a buffered log gets the output.
pub fn format(detail: Detail, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("error: ", .{});
    switch (detail.kind) {
        Parse.UnknownFlag => try writer.print("unknown flag", .{}),
        Parse.MissingValue => try writer.print("flag missing value", .{}),
        Parse.InvalidValue => try writer.print("invalid value", .{}),
        Parse.MissingRequired => try writer.print("required flag missing", .{}),
        Parse.MissingRequiredPositional => try writer.print("required positional missing", .{}),
        Parse.TooManyPositionals => try writer.print("too many positional arguments", .{}),
        Parse.UnknownSubcommand => try writer.print("unknown subcommand", .{}),
        Parse.UnexpectedArgument => try writer.print("unexpected argument", .{}),
        Parse.DuplicateFlag => try writer.print("flag specified more than once", .{}),
        Parse.FlagGroupViolation => try writer.print("flag group violation", .{}),
    }
    if (detail.message) |m| try writer.print(" ({s})", .{m});
    if (detail.flag) |f| try writer.print(": {s}", .{f});
    if (detail.positional) |p| try writer.print(": <{s}>", .{p});
    if (detail.group) |g| {
        try writer.print(": {s}", .{g});
        if (detail.group_mode) |mode| try writer.print(" ({s})", .{@tagName(mode)});
        if (detail.group_flags.len > 0) {
            try writer.print(" [", .{});
            for (detail.group_flags, 0..) |name, idx| {
                if (idx > 0) try writer.print(", ", .{});
                try writer.print("{s}", .{name});
            }
            try writer.print("]", .{});
        }
    }
    if (detail.arg) |a| try writer.print(" (got {s})", .{a});
    if (detail.cmd_path) |c| try writer.print(" [in: {s}]", .{c});
    if (detail.suggestion) |s| try writer.print("; did you mean {s}?", .{s});
    try writer.print("\n", .{});
}

// ---- tests ----

test "format renders unknown flag with context" {
    var buf: [256]u8 = undefined;
    var stream = std.Io.Writer.fixed(&buf);
    try format(.{
        .kind = Parse.UnknownFlag,
        .flag = "--bogus",
        .cmd_path = "tool task",
    }, &stream);
    const out = stream.buffered();
    try std.testing.expect(std.mem.indexOf(u8, out, "unknown flag") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "--bogus") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "tool task") != null);
}

test "structured exposes stable parse error fields" {
    const value = structured(.{
        .kind = Parse.UnknownSubcommand,
        .arg = "statsu",
        .cmd_path = "tool",
        .suggestion = "status",
    });
    try std.testing.expectEqual(Parse.UnknownSubcommand, value.kind);
    try std.testing.expectEqualStrings("unknown_subcommand", value.kind_name);
    try std.testing.expectEqualStrings("statsu", value.arg.?);
    try std.testing.expectEqualStrings("status", value.suggestion.?);
}

test "structured exposes flag group violation fields" {
    const value = structured(.{
        .kind = Parse.FlagGroupViolation,
        .group = "output",
        .group_mode = .mutually_exclusive,
        .group_flags = &.{ "--json", "--yaml" },
        .cmd_path = "tool render",
    });
    try std.testing.expectEqual(Parse.FlagGroupViolation, value.kind);
    try std.testing.expectEqualStrings("flag_group_violation", value.kind_name);
    try std.testing.expectEqualStrings("output", value.group.?);
    try std.testing.expectEqual(flag_mod.FlagGroupMode.mutually_exclusive, value.group_mode.?);
    try std.testing.expectEqual(@as(usize, 2), value.group_flags.len);
    try std.testing.expectEqualStrings("--json", value.group_flags[0]);
}
