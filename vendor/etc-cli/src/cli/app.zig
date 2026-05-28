//! Optional app-runner policy layered above parse/dispatch.

const std = @import("std");
const cmd_mod = @import("cmd.zig");
const err_mod = @import("error.zig");
const help_mod = @import("help.zig");
const parser = @import("parser.zig");

pub const ExitCodes = struct {
    success: u8 = 0,
    parse_error: u8 = 2,
    handler_error: u8 = 1,
};

pub const Options = struct {
    argv: []const []const u8,
    stdout: *std.Io.Writer,
    stderr: *std.Io.Writer,
    version: ?[]const u8 = null,
    about: ?[]const u8 = null,
    exit_codes: ExitCodes = .{},
};

pub fn run(comptime root: cmd_mod.Cmd, options: Options) anyerror!u8 {
    if (try maybeBuiltin(root, options)) |code| return code;

    var detail: err_mod.Detail = undefined;
    const result = parser.parse(root, options.argv, &detail) catch {
        try err_mod.format(detail, options.stderr);
        try options.stderr.flush();
        return options.exit_codes.parse_error;
    };

    switch (result) {
        .help => |path| {
            const text = helpForAnyPath(root, path) orelse comptime help_mod.helpText(root, &.{});
            try options.stdout.print("{s}", .{text});
            try options.stdout.flush();
            return options.exit_codes.success;
        },
        .match => |u| {
            return invokeMatch(root, u, options);
        },
    }
}

fn maybeBuiltin(comptime root: cmd_mod.Cmd, options: Options) !?u8 {
    if (options.argv.len != 2) return null;

    if (std.mem.eql(u8, options.argv[1], "--version") or std.mem.eql(u8, options.argv[1], "version")) {
        if (options.version) |version| {
            try options.stdout.print("{s}\n", .{version});
            try options.stdout.flush();
            return options.exit_codes.success;
        }
    }

    if (std.mem.eql(u8, options.argv[1], "--about") or std.mem.eql(u8, options.argv[1], "about")) {
        if (options.about) |about| {
            try options.stdout.print("{s}\n", .{about});
            try options.stdout.flush();
            return options.exit_codes.success;
        } else if (options.version) |version| {
            try options.stdout.print("{s} {s}\n", .{ root.name, version });
            try options.stdout.flush();
            return options.exit_codes.success;
        }
    }

    return null;
}

fn helpForAnyPath(comptime root: cmd_mod.Cmd, runtime_path: []const []const u8) ?[]const u8 {
    if (runtime_path.len == 0) return comptime help_mod.helpText(root, &.{});
    const nodes = comptime cmd_mod.allNodes(root);
    inline for (nodes) |node| {
        if (pathsEqual(node.path, runtime_path)) {
            return comptime help_mod.helpText(root, node.path);
        }
    }
    return null;
}

fn pathsEqual(a: []const []const u8, b: []const []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |aa, bb| {
        if (!std.mem.eql(u8, aa, bb)) return false;
    }
    return true;
}

fn invokeMatch(
    comptime root: cmd_mod.Cmd,
    result_union: parser.ResultUnion(root),
    options: Options,
) anyerror!u8 {
    const leaves = comptime cmd_mod.allLeaves(root);
    inline for (leaves) |leaf| {
        const tag_name = comptime pathToTag(leaf.path);
        if (std.mem.eql(u8, @tagName(std.meta.activeTag(result_union)), tag_name)) {
            if (leaf.cmd.run) |handler_ptr| {
                const handler_fn: cmd_mod.HandlerFn = @ptrCast(@alignCast(handler_ptr));
                const args = @field(result_union, tag_name);
                handler_fn(@ptrCast(&args)) catch |err| {
                    try options.stderr.print("error: handler failed: {s}\n", .{@errorName(err)});
                    try options.stderr.flush();
                    return options.exit_codes.handler_error;
                };
                try options.stdout.flush();
                return options.exit_codes.success;
            }

            const text = comptime help_mod.helpText(root, leaf.path);
            try options.stdout.print("{s}", .{text});
            try options.stdout.flush();
            return options.exit_codes.success;
        }
    }
    return options.exit_codes.parse_error;
}

fn pathToTag(comptime path: []const []const u8) []const u8 {
    comptime {
        if (path.len == 0) return "root";
        var joined: []const u8 = path[0];
        for (path[1..]) |seg| joined = joined ++ "_" ++ seg;
        var buf: [joined.len]u8 = undefined;
        for (joined, 0..) |c, i| buf[i] = if (c == '-') '_' else c;
        const final = buf;
        return &final;
    }
}
