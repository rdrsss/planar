//! Shared command/flag metadata.

pub const Deprecation = struct {
    message: []const u8 = "",
    replacement: ?[]const u8 = null,
};

pub const CompletionKind = enum { none, values, files, directories };

pub const Completion = struct {
    kind: CompletionKind = .none,
    values: []const []const u8 = &.{},

    pub fn none() Completion {
        return .{};
    }

    pub fn valueChoices(comptime choices: []const []const u8) Completion {
        return .{ .kind = .values, .values = choices };
    }

    pub const files: Completion = .{ .kind = .files };
    pub const directories: Completion = .{ .kind = .directories };
};
