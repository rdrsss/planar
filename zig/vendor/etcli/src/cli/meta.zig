//! Shared command/flag metadata.

pub const Deprecation = struct {
    message: []const u8 = "",
    replacement: ?[]const u8 = null,
};

pub const CompletionKind = enum { none, values, files, directories, dynamic };

/// Signature for a dynamic completion callback. Receives the current word
/// prefix and returns candidate completions. Invoked at runtime by
/// `cli.complete` (reached via the `__complete` builtin in generated scripts).
pub const CompletionFn = *const fn (prefix: []const u8) []const []const u8;

pub const Completion = struct {
    kind: CompletionKind = .none,
    values: []const []const u8 = &.{},
    callback: ?CompletionFn = null,

    pub fn none() Completion {
        return .{};
    }

    pub fn valueChoices(comptime choices: []const []const u8) Completion {
        return .{ .kind = .values, .values = choices };
    }

    /// Runtime-computed completions. The generated shell scripts invoke the
    /// program's `__complete` builtin, which calls this callback.
    pub fn dynamic(callback: CompletionFn) Completion {
        return .{ .kind = .dynamic, .callback = callback };
    }

    pub const files: Completion = .{ .kind = .files };
    pub const directories: Completion = .{ .kind = .directories };
};
