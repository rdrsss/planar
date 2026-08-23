//! Optional app-runner policy layered above parse/dispatch.

const std = @import("std");
const cmd_mod = @import("cmd.zig");
const err_mod = @import("error.zig");
const flag_mod = @import("flag.zig");
const help_mod = @import("help.zig");
const parser = @import("parser.zig");
const completion_mod = @import("completion.zig");

pub const ExitCodes = struct {
    success: u8 = 0,
    parse_error: u8 = 2,
    handler_error: u8 = 1,
};

/// Controls whether generated help is colorized. `.auto` (the default)
/// resolves to color only when stdout is a TTY and `NO_COLOR` is unset;
/// `.always`/`.never` force the decision. Because the writer abstraction hides
/// the underlying file descriptor, the caller reports TTY state via
/// `Options.stdout_tty` (the example program detects it from its stdout file).
pub const ColorMode = enum { auto, always, never };

pub const Options = struct {
    argv: []const []const u8,
    stdout: *std.Io.Writer,
    stderr: *std.Io.Writer,
    version: ?[]const u8 = null,
    about: ?[]const u8 = null,
    exit_codes: ExitCodes = .{},
    /// Optional hook invoked when a handler returns an error. It receives the
    /// error and the stderr writer and returns an exit code, or null to fall
    /// back to `exit_codes.handler_error`. When this hook is null the runner
    /// writes a concise `error: <name>` line to stderr. Use it to format
    /// handler errors, suppress the default line (when a handler already
    /// printed its own message), or map specific errors to exit codes.
    on_handler_error: ?*const fn (err: anyerror, stderr: *std.Io.Writer) anyerror!?u8 = null,
    /// Optional environment lookup for `Flag.env` fallback. Given a variable
    /// name, return its value or null. When set, the runner first resolves the
    /// command path from argv, then fills any visible flag with `.env` set and
    /// absent from argv with its environment value before parsing — so
    /// precedence is argv > env > default > required-error. The
    /// parser/dispatch APIs remain env-unaware. Returned values must outlive
    /// the call (e.g. slices into the process environment).
    env_lookup: ?*const fn (name: []const u8) ?[]const u8 = null,
    /// Colorization policy for generated help output (stdout). Defaults to
    /// `.auto`, which only colorizes when `stdout_tty` is true and `NO_COLOR`
    /// is absent (resolved via `env_lookup` when provided).
    color: ColorMode = .auto,
    /// Whether stdout is a terminal. The caller sets this (e.g. from
    /// `std.posix.isatty` on its stdout fd) because the writer hides the fd.
    /// Only consulted when `color == .auto`.
    stdout_tty: bool = false,
};

/// Resolve the effective help-color decision from the runner options.
fn useColor(options: Options) bool {
    return switch (options.color) {
        .never => false,
        .always => true,
        .auto => blk: {
            if (options.env_lookup) |lookup| {
                if (lookup("NO_COLOR")) |_| break :blk false;
            }
            break :blk options.stdout_tty;
        },
    };
}

/// Module-static backing store for the env-augmented argv. Like the parser's
/// buffers, the returned slice stays valid until the next `run`; single-
/// threaded by construction.
var env_argv_buf: [512][]const u8 = undefined;
var env_path_buf: [256][]const u8 = undefined;
var env_token_buf: [4096]u8 = undefined;
var env_token_len: usize = 0;

pub fn run(comptime root: cmd_mod.Cmd, options: Options) anyerror!u8 {
    // Dynamic-completion callback entrypoint, invoked by generated scripts as
    // `<prog> __complete <flag> <prefix>`.
    if (options.argv.len >= 2 and std.mem.eql(u8, options.argv[1], "__complete")) {
        try completion_mod.complete(root, options.argv[2..], options.stdout);
        return options.exit_codes.success;
    }

    if (try maybeBuiltin(root, options)) |code| return code;

    const argv = if (options.env_lookup) |lookup|
        augmentWithEnv(root, options.argv, lookup)
    else
        options.argv;

    var detail: err_mod.Detail = undefined;
    const result = parser.parse(root, argv, &detail) catch {
        try err_mod.format(detail, options.stderr);
        try options.stderr.flush();
        return options.exit_codes.parse_error;
    };

    switch (result) {
        .help => |path| {
            try writeResolvedHelp(root, path, useColor(options), options.stdout);
            try options.stdout.flush();
            return options.exit_codes.success;
        },
        .match => |u| {
            return invokeMatch(root, u, options);
        },
    }
}

/// Build an argv with environment fallbacks for flags visible at the
/// argv-resolved command path. The injected tokens sit immediately after the
/// matched command path, where the parser already sees the leaf's inherited +
/// local flags. Only flags absent from argv whose env var resolves are added.
fn augmentWithEnv(
    comptime root: cmd_mod.Cmd,
    argv: []const []const u8,
    lookup: *const fn (name: []const u8) ?[]const u8,
) []const []const u8 {
    if (argv.len == 0) return argv;
    const path = resolveCommandPath(root, argv);

    env_token_len = 0;
    env_argv_buf[0] = argv[0];
    var n: usize = 1;

    var injected = false;
    if (path.len == 0) {
        appendEnvFallbacksForPath(root, path, argv, lookup, &n);
        injected = true;
    }

    const max_depth = comptime treeDepth(root);
    var path_len: usize = 0;
    var ancestors: [max_depth + 1]cmd_mod.Cmd = undefined;
    ancestors[0] = root;
    var current: cmd_mod.Cmd = root;
    var passthrough = false;

    var i: usize = 1;
    while (i < argv.len) : (i += 1) {
        const tok = argv[i];
        if (n >= env_argv_buf.len) return argv; // overflow: fall back to raw argv
        env_argv_buf[n] = tok;
        n += 1;

        if (tok.len == 0) continue;

        if (passthrough or tok[0] != '-' or std.mem.eql(u8, tok, "-")) {
            if (!passthrough) {
                var matched = false;
                for (current.cmds) |c| {
                    if (commandMatches(c, tok)) {
                        if (path_len >= env_path_buf.len) return argv;
                        path_len += 1;
                        ancestors[path_len] = c;
                        current = c;
                        matched = true;
                        if (!injected and path_len == path.len) {
                            appendEnvFallbacksForPath(root, path, argv, lookup, &n);
                            injected = true;
                        }
                        break;
                    }
                }
                if (matched) continue;
            }
            continue;
        }

        if (std.mem.eql(u8, tok, "--")) {
            passthrough = true;
            continue;
        }

        if (flagWantsValue(ancestors[0 .. path_len + 1], tok)) {
            i += 1;
            if (i < argv.len) {
                if (n >= env_argv_buf.len) return argv;
                env_argv_buf[n] = argv[i];
                n += 1;
            }
        }
    }

    if (!injected) {
        appendEnvFallbacksForPath(root, path, argv, lookup, &n);
    }

    return env_argv_buf[0..n];
}

fn appendEnvFallbacksForPath(
    comptime root: cmd_mod.Cmd,
    path: []const []const u8,
    argv: []const []const u8,
    lookup: *const fn (name: []const u8) ?[]const u8,
    n: *usize,
) void {
    if (path.len == 0) {
        appendEnvFallbacks(root.flags, argv, lookup, n);
        return;
    }

    const nodes = comptime cmd_mod.allNodes(root);
    inline for (nodes) |node| {
        if (pathsEqual(node.path, path)) {
            const visible_flags = comptime cmd_mod.collectInheritedFlags(root, node.path) ++ node.cmd.flags;
            appendEnvFallbacks(visible_flags, argv, lookup, n);
            return;
        }
    }
}

fn appendEnvFallbacks(
    comptime flags: []const flag_mod.Flag,
    argv: []const []const u8,
    lookup: *const fn (name: []const u8) ?[]const u8,
    n: *usize,
) void {
    inline for (flags) |f| {
        if (comptime f.env != null) {
            if (!argvHasFlag(flags, argv, f)) {
                if (lookup(f.env.?)) |value| {
                    if (f.kind == .bool) {
                        if (makeInlineBoolEnvToken(f.long, value)) |tok| {
                            if (n.* + 1 <= env_argv_buf.len) {
                                env_argv_buf[n.*] = tok;
                                n.* += 1;
                            }
                        }
                    } else {
                        if (n.* + 2 <= env_argv_buf.len) {
                            env_argv_buf[n.*] = f.long;
                            env_argv_buf[n.* + 1] = value;
                            n.* += 2;
                        }
                    }
                }
            }
        }
    }
}

fn makeInlineBoolEnvToken(comptime long: []const u8, value: []const u8) ?[]const u8 {
    const needed = long.len + 1 + value.len;
    if (env_token_len + needed > env_token_buf.len) return null;
    const start = env_token_len;
    @memcpy(env_token_buf[start..][0..long.len], long);
    env_token_buf[start + long.len] = '=';
    @memcpy(env_token_buf[start + long.len + 1 ..][0..value.len], value);
    env_token_len += needed;
    return env_token_buf[start..env_token_len];
}

fn argvHasFlag(
    comptime flags: []const flag_mod.Flag,
    argv: []const []const u8,
    comptime target: flag_mod.Flag,
) bool {
    var i: usize = 1;
    while (i < argv.len) : (i += 1) {
        const tok = argv[i];
        if (tok.len == 0) continue;
        if (std.mem.eql(u8, tok, "--")) break;
        if (tok.len < 2 or tok[0] != '-') continue;

        if (tokenNamesFlag(flags, target, tok)) return true;

        if (tokenWantsSeparatedValue(flags, tok)) {
            i += 1;
        }
    }
    return false;
}

fn tokenNamesFlag(
    comptime flags: []const flag_mod.Flag,
    comptime target: flag_mod.Flag,
    tok: []const u8,
) bool {
    if (tok.len >= 2 and tok[0] == '-' and tok[1] == '-') {
        if (flagLongMatches(target, tok)) return true;
        if (target.kind == .bool and flagNegationMatches(target, tok)) return true;
        if (startsWithInlineLongValue(target.long, tok)) return true;
        inline for (target.aliases) |alias| {
            if (startsWithInlineLongValue(alias, tok)) return true;
        }
        return false;
    }

    if (tok.len >= 2 and tok[0] == '-') {
        return shortTokenNamesFlag(flags, target, tok);
    }

    return false;
}

fn startsWithInlineLongValue(comptime name: []const u8, tok: []const u8) bool {
    return std.mem.startsWith(u8, tok, name) and tok.len > name.len and tok[name.len] == '=';
}

fn shortTokenNamesFlag(
    comptime flags: []const flag_mod.Flag,
    comptime target: flag_mod.Flag,
    tok: []const u8,
) bool {
    if (target.short == null) return false;
    const target_short = target.short.?;

    if (tok.len == 2) return tok[1] == target_short;

    const first_idx = matchShortFlag(flags, tok[1]) orelse return false;
    const first_flag = flags[first_idx];
    if (first_flag.kind != .bool) return tok[1] == target_short;

    var pos: usize = 1;
    while (pos < tok.len) : (pos += 1) {
        const idx = matchShortFlag(flags, tok[pos]) orelse return false;
        if (flags[idx].kind != .bool) return false;
    }

    if (target.kind != .bool) return false;
    for (tok[1..]) |short| {
        if (short == target_short) return true;
    }
    return false;
}

fn tokenWantsSeparatedValue(comptime flags: []const flag_mod.Flag, tok: []const u8) bool {
    if (tok.len >= 2 and tok[0] == '-' and tok[1] == '-') {
        inline for (flags) |f| {
            if (flagLongMatches(f, tok)) return f.kind != .bool;
        }
        return false;
    }

    if (tok.len == 2 and tok[0] == '-') {
        const idx = matchShortFlag(flags, tok[1]) orelse return false;
        return flags[idx].kind != .bool;
    }

    return false;
}

fn matchShortFlag(comptime flags: []const flag_mod.Flag, short: u8) ?usize {
    inline for (flags, 0..) |f, idx| {
        if (f.short) |s| {
            if (s == short) return idx;
        }
    }
    return null;
}

fn resolveCommandPath(comptime root: cmd_mod.Cmd, argv: []const []const u8) []const []const u8 {
    if (argv.len == 0) return &.{};

    const max_depth = comptime treeDepth(root);
    var path_len: usize = 0;
    var ancestors: [max_depth + 1]cmd_mod.Cmd = undefined;
    ancestors[0] = root;
    var current: cmd_mod.Cmd = root;
    var passthrough = false;

    var i: usize = 1;
    while (i < argv.len) : (i += 1) {
        const tok = argv[i];
        if (tok.len == 0) continue;

        if (passthrough or tok[0] != '-' or std.mem.eql(u8, tok, "-")) {
            if (!passthrough) {
                var matched = false;
                for (current.cmds) |c| {
                    if (commandMatches(c, tok)) {
                        if (path_len >= env_path_buf.len) return env_path_buf[0..path_len];
                        env_path_buf[path_len] = c.name;
                        path_len += 1;
                        ancestors[path_len] = c;
                        current = c;
                        matched = true;
                        break;
                    }
                }
                if (matched) continue;
            }
            continue;
        }

        if (std.mem.eql(u8, tok, "--")) {
            passthrough = true;
            continue;
        }

        if (flagWantsValue(ancestors[0 .. path_len + 1], tok)) {
            i += 1;
        }
    }

    return env_path_buf[0..path_len];
}

fn flagWantsValue(scope: []const cmd_mod.Cmd, tok: []const u8) bool {
    if (tok.len >= 2 and tok[0] == '-' and tok[1] == '-') {
        for (scope) |node| {
            for (node.flags) |f| {
                if (flagLongMatches(f, tok)) return f.kind != .bool;
            }
        }
        return false;
    }

    if (tok.len == 2 and tok[0] == '-') {
        for (scope) |node| {
            for (node.flags) |f| {
                if (f.short) |s| if (s == tok[1]) return f.kind != .bool;
            }
        }
        return false;
    }

    return false;
}

fn commandMatches(command: cmd_mod.Cmd, tok: []const u8) bool {
    if (std.mem.eql(u8, command.name, tok)) return true;
    for (command.aliases) |alias| {
        if (std.mem.eql(u8, alias, tok)) return true;
    }
    return false;
}

fn flagLongMatches(f: flag_mod.Flag, tok: []const u8) bool {
    if (std.mem.eql(u8, f.long, tok)) return true;
    for (f.aliases) |alias| {
        if (std.mem.eql(u8, alias, tok)) return true;
    }
    return false;
}

fn flagNegationMatches(comptime f: flag_mod.Flag, tok: []const u8) bool {
    if (std.mem.startsWith(u8, f.long, "--")) {
        const negated = "--no-" ++ f.long[2..];
        if (std.mem.eql(u8, negated, tok)) return true;
    }
    inline for (f.aliases) |alias| {
        if (std.mem.startsWith(u8, alias, "--")) {
            const negated = "--no-" ++ alias[2..];
            if (std.mem.eql(u8, negated, tok)) return true;
        }
    }
    return false;
}

fn treeDepth(comptime root: cmd_mod.Cmd) usize {
    comptime {
        var max: usize = 0;
        depthWalk(root, 0, &max);
        return @max(max, 1);
    }
}

fn depthWalk(comptime node: cmd_mod.Cmd, comptime current: usize, comptime max: *usize) void {
    comptime {
        if (current > max.*) max.* = current;
        for (node.cmds) |c| depthWalk(c, current + 1, max);
    }
}

fn maybeBuiltin(comptime root: cmd_mod.Cmd, options: Options) !?u8 {
    if (options.argv.len != 2) return null;
    const arg = options.argv[1];

    // `--version`/`--about` are conventional and always honored. The bare
    // words `version`/`about` are only intercepted when the tree does not
    // declare a real subcommand by that name, so a user's `version` command
    // is never silently shadowed by the runner.
    const want_version = std.mem.eql(u8, arg, "--version") or
        (std.mem.eql(u8, arg, "version") and comptime !hasSubcommand(root, "version"));
    if (want_version) {
        if (options.version) |version| {
            try options.stdout.print("{s}\n", .{version});
            try options.stdout.flush();
            return options.exit_codes.success;
        }
    }

    const want_about = std.mem.eql(u8, arg, "--about") or
        (std.mem.eql(u8, arg, "about") and comptime !hasSubcommand(root, "about"));
    if (want_about) {
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

fn hasSubcommand(comptime root: cmd_mod.Cmd, comptime name: []const u8) bool {
    for (root.cmds) |c| {
        if (std.mem.eql(u8, c.name, name)) return true;
        for (c.aliases) |alias| {
            if (std.mem.eql(u8, alias, name)) return true;
        }
    }
    return false;
}

fn helpForAnyPath(comptime root: cmd_mod.Cmd, runtime_path: []const []const u8, comptime color: bool) ?[]const u8 {
    if (runtime_path.len == 0) return comptime help_mod.helpTextWithOptions(root, &.{}, .{ .color = color });
    const nodes = comptime cmd_mod.allNodes(root);
    inline for (nodes) |node| {
        if (pathsEqual(node.path, runtime_path)) {
            return comptime help_mod.helpTextWithOptions(root, node.path, .{ .color = color });
        }
    }
    return null;
}

/// Write the help page for `path`, choosing the colored or plain comptime
/// variant at runtime. Both variants are distinct `.rodata` strings; the
/// runtime branch selects one.
fn writeResolvedHelp(
    comptime root: cmd_mod.Cmd,
    path: []const []const u8,
    use_color: bool,
    writer: *std.Io.Writer,
) !void {
    const text = if (use_color)
        helpForAnyPath(root, path, true) orelse comptime help_mod.helpTextWithOptions(root, &.{}, .{ .color = true })
    else
        helpForAnyPath(root, path, false) orelse comptime help_mod.helpTextWithOptions(root, &.{}, .{ .color = false });
    try writer.print("{s}", .{text});
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
            // Warn (once) when the invoked command itself is deprecated.
            // `dispatch` (single-writer callback mode) does not emit these.
            if (comptime leaf.cmd.deprecated != null) {
                try emitDeprecation(options.stderr, leaf.cmd.name, leaf.cmd.deprecated.?);
            }
            // Warn for each deprecated flag the parse actually matched. The
            // parser records the canonical long names; we cross-reference the
            // leaf's visible flags to recover each one's deprecation metadata.
            const dep_flags = comptime cmd_mod.collectInheritedFlags(root, leaf.path) ++ leaf.cmd.flags;
            const seen_dep = parser.deprecatedFlagsSeen();
            inline for (dep_flags) |f| {
                if (comptime f.deprecated != null) {
                    for (seen_dep) |seen_long| {
                        if (std.mem.eql(u8, seen_long, f.long)) {
                            try emitDeprecation(options.stderr, f.long, f.deprecated.?);
                            break;
                        }
                    }
                }
            }
            if (leaf.cmd.run) |handler_ptr| {
                const handler_fn: cmd_mod.HandlerFn = @ptrCast(@alignCast(handler_ptr));
                const args = @field(result_union, tag_name);
                handler_fn(@ptrCast(&args)) catch |err| {
                    const code = if (options.on_handler_error) |hook|
                        try hook(err, options.stderr)
                    else blk: {
                        try options.stderr.print("error: {s}\n", .{@errorName(err)});
                        break :blk null;
                    };
                    try options.stderr.flush();
                    return code orelse options.exit_codes.handler_error;
                };
                try options.stdout.flush();
                return options.exit_codes.success;
            }

            try writeResolvedHelp(root, leaf.path, useColor(options), options.stdout);
            try options.stdout.flush();
            return options.exit_codes.success;
        }
    }
    // The result union was built from one of `leaves`, so exactly one tag
    // matches above. Reaching here means the leaf/tag sets drifted apart.
    unreachable;
}

fn emitDeprecation(stderr: *std.Io.Writer, comptime name: []const u8, comptime d: anytype) std.Io.Writer.Error!void {
    try stderr.print("warning: '{s}' is deprecated", .{name});
    if (d.replacement) |replacement| try stderr.print("; use {s}", .{replacement});
    if (d.message.len > 0) try stderr.print("; {s}", .{d.message});
    try stderr.print("\n", .{});
    try stderr.flush();
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
