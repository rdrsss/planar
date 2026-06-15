//! Comptime help-text generator.
//!
//! `helpText(root, path)` returns a `[]const u8` string describing the
//! command at `path`: its `desc`, sub-commands (one line each), flags
//! (one line each with kind + default), and positionals. The entire
//! string is built at comptime so it lives in `.rodata` with zero
//! runtime cost beyond the `std.debug.print` (or writer call) at the
//! `--help` site.
//!
//! `--help` handling itself is in the parser: when the parser sees
//! `--help` (or `-h`) at any depth, it returns a `Help` sentinel value
//! the caller renders via this function.

const std = @import("std");
const cmd_mod = @import("cmd.zig");
const flag_mod = @import("flag.zig");
const duration_mod = @import("duration.zig");

pub const Options = struct {
    include_hidden: bool = false,
    include_deprecated: bool = true,
    /// Optional target width. Widths <= 48 switch tables to a compact
    /// two-line form that keeps descriptions out of narrow columns.
    width: ?usize = null,
    /// When true, section headers and command/flag names are wrapped in ANSI
    /// escapes. The help string is built at comptime, so the colored and plain
    /// forms are distinct comptime instantiations; `cli.run` picks one at
    /// runtime based on `ColorMode`, `NO_COLOR`, and TTY state.
    color: bool = false,
};

const ansi_bold = "\x1b[1m";
const ansi_accent = "\x1b[36m"; // cyan
const ansi_reset = "\x1b[0m";

/// Bold a section header when color is enabled. Headers do not participate in
/// column alignment, so wrapping them in escapes is safe.
fn boldHeader(comptime options: Options, comptime s: []const u8) []const u8 {
    return if (options.color) ansi_bold ++ s ++ ansi_reset else s;
}

/// Accent a command/flag name. Callers must compute column padding from the
/// uncolored label length before applying this, since the escapes are
/// zero-width on screen but real bytes in the string.
fn accentName(comptime options: Options, comptime s: []const u8) []const u8 {
    return if (options.color) ansi_accent ++ s ++ ansi_reset else s;
}

pub fn helpText(comptime root: cmd_mod.Cmd, comptime path: []const []const u8) []const u8 {
    return helpTextWithOptions(root, path, .{});
}

pub fn helpTextWithOptions(
    comptime root: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const u8 {
    @setEvalBranchQuota(2_000_000);
    const target = comptime cmd_mod.findCmd(root, path) orelse @compileError(
        "helpText: no command at path",
    );
    return comptime renderCmd(root, target, path, options);
}

pub fn writeText(
    comptime root: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
    writer: *std.Io.Writer,
) std.Io.Writer.Error!void {
    try writer.writeAll(comptime helpTextWithOptions(root, path, options));
}

fn renderCmd(
    comptime root: cmd_mod.Cmd,
    comptime node: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const u8 {
    comptime {
        var out: []const u8 = "";
        const flags = cmd_mod.collectInheritedFlags(root, path) ++ node.flags;

        // Header: "USAGE: <name> [flags] [sub] [positionals]"
        const full_path = renderPath(path, node.name);
        out = out ++ full_path ++ "\n";
        // Prefer long_desc (multi-line prose) when set; fall back to
        // the one-line desc otherwise. long_desc is printed verbatim
        // with a single leading newline so its own indentation /
        // formatting survives. desc continues to be used in the parent's
        // COMMANDS table (rendered below at the subcommand-list site).
        if (node.long_desc.len > 0) {
            out = out ++ "\n" ++ node.long_desc ++ "\n";
        } else if (node.desc.len > 0) {
            out = out ++ "\n  " ++ node.desc ++ "\n";
        }

        // Usage line synthesis.
        var usage: []const u8 = "\n" ++ boldHeader(options, "USAGE:") ++ "\n  " ++ full_path;
        if (hasVisibleFlags(flags, options)) usage = usage ++ " [flags]";
        if (hasVisibleCommands(node.cmds, options)) usage = usage ++ " <command>";
        for (node.positionals) |p| {
            if (p.required) {
                usage = usage ++ " <" ++ p.name ++ ">";
            } else {
                usage = usage ++ " [" ++ p.name ++ "]";
            }
        }
        out = out ++ usage ++ "\n";

        // Sub-commands.
        if (hasVisibleCommands(node.cmds, options)) {
            out = out ++ "\n" ++ boldHeader(options, "COMMANDS:") ++ "\n";
            for (node.cmds) |c| {
                if (!visibleCmd(c, options)) continue;
                const label = cmdLabel(c);
                if (compact(options)) {
                    out = out ++ "  " ++ accentName(options, label);
                    if (c.deprecated) |d| out = out ++ deprecationSuffix(d);
                    out = out ++ "\n";
                    if (c.desc.len > 0) out = out ++ "      " ++ c.desc ++ "\n";
                    continue;
                }
                out = out ++ "  " ++ accentName(options, label) ++ padTo(label, 16);
                if (c.desc.len > 0) out = out ++ c.desc;
                if (c.deprecated) |d| out = out ++ deprecationSuffix(d);
                out = out ++ "\n";
            }
        }

        // Flags.
        if (hasVisibleFlags(flags, options)) {
            out = out ++ "\n" ++ boldHeader(options, "FLAGS:") ++ "\n";
            for (flags) |f| {
                if (!visibleFlag(f, options)) continue;
                out = out ++ "  " ++ renderFlagLine(f, options) ++ "\n";
            }
        }

        // Flag groups.
        if (hasVisibleFlagGroups(node.flag_groups, flags, options)) {
            out = out ++ "\n" ++ boldHeader(options, "FLAG GROUPS:") ++ "\n";
            for (node.flag_groups) |group| {
                if (!visibleFlagGroup(group, flags, options)) continue;
                out = out ++ "  " ++ renderFlagGroupLine(group, options) ++ "\n";
            }
        }

        // Environment fallback.
        if (hasVisibleEnv(flags, options)) {
            out = out ++ "\n" ++ boldHeader(options, "ENVIRONMENT:") ++ "\n";
            for (flags) |f| {
                if (!visibleFlag(f, options)) continue;
                out = out ++ renderEnvLine(f, options);
            }
        }

        // Positionals.
        if (node.positionals.len > 0) {
            out = out ++ "\n" ++ boldHeader(options, "POSITIONAL ARGUMENTS:") ++ "\n";
            for (node.positionals) |p| {
                out = out ++ "  <" ++ p.name ++ ">" ++ padTo(p.name, if (compact(options)) 8 else 14) ++ "(" ++ @tagName(p.kind) ++ ")";
                if (!p.required) out = out ++ " optional";
                if (p.default) |d| out = out ++ " default=" ++ renderDefault(d);
                if (p.desc.len > 0) out = out ++ " — " ++ p.desc;
                out = out ++ "\n";
            }
        }

        return out;
    }
}

/// The COMMANDS-column label for a subcommand: its name followed by any
/// declared aliases, e.g. `status, st`. Aliases parse to the same command,
/// so surfacing them here keeps help consistent with completion and schema.
fn cmdLabel(comptime c: cmd_mod.Cmd) []const u8 {
    comptime {
        var out: []const u8 = c.name;
        for (c.aliases) |alias| out = out ++ ", " ++ alias;
        return out;
    }
}

fn renderPath(comptime path: []const []const u8, comptime leaf_name: []const u8) []const u8 {
    comptime {
        if (path.len == 0) return leaf_name;
        var out: []const u8 = path[0];
        for (path[1..]) |seg| out = out ++ " " ++ seg;
        return out;
    }
}

fn renderFlagLine(comptime f: flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        var label: []const u8 = f.long;
        if (f.short) |s| label = label ++ ", -" ++ &[_]u8{s};
        for (f.aliases) |alias| label = label ++ ", " ++ alias;
        // Pad from the uncolored label width so columns stay aligned, then
        // colorize the name in place.
        var out: []const u8 = accentName(options, label);
        out = out ++ padTo(label, if (compact(options)) 16 else 22) ++ "(" ++ flagKindLabel(f) ++ ")";
        if (f.list or f.count) out = out ++ " (repeatable)";
        if (f.required) out = out ++ " required";
        if (f.default) |d| out = out ++ " default=" ++ renderDefault(d);
        if (f.desc.len > 0) {
            if (compact(options)) {
                out = out ++ "\n      " ++ f.desc;
            } else {
                out = out ++ " — " ++ f.desc;
            }
        }
        if (f.deprecated) |d| out = out ++ deprecationSuffix(d);
        return out;
    }
}

fn renderFlagGroupLine(comptime group: flag_mod.FlagGroup, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = group.name ++ padTo(group.name, if (compact(options)) 14 else 20);
        out = out ++ flagGroupModeLabel(group.mode) ++ ": " ++ joinFlagNames(group.flags);
        if (group.desc.len > 0) {
            if (compact(options)) {
                out = out ++ "\n      " ++ group.desc;
            } else {
                out = out ++ " — " ++ group.desc;
            }
        }
        return out;
    }
}

fn renderEnvLine(comptime f: flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        if (f.env == null) return "";
        const env = f.env.?;
        var out: []const u8 = "  " ++ env ++ padTo(env, if (compact(options)) 16 else 22);
        out = out ++ "cli.run fallback for " ++ f.long ++ "; parse/dispatch env-unaware\n";
        return out;
    }
}

fn compact(comptime options: Options) bool {
    return if (options.width) |width| width <= 48 else false;
}

fn visibleCmd(comptime c: cmd_mod.Cmd, comptime options: Options) bool {
    if (c.hidden and !options.include_hidden) return false;
    if (c.deprecated != null and !options.include_deprecated) return false;
    return true;
}

fn visibleFlag(comptime f: flag_mod.Flag, comptime options: Options) bool {
    if (f.hidden and !options.include_hidden) return false;
    if (f.deprecated != null and !options.include_deprecated) return false;
    return true;
}

fn hasVisibleCommands(comptime cmds: []const cmd_mod.Cmd, comptime options: Options) bool {
    for (cmds) |c| if (visibleCmd(c, options)) return true;
    return false;
}

fn hasVisibleFlags(comptime flags: []const flag_mod.Flag, comptime options: Options) bool {
    for (flags) |f| if (visibleFlag(f, options)) return true;
    return false;
}

fn hasVisibleEnv(comptime flags: []const flag_mod.Flag, comptime options: Options) bool {
    for (flags) |f| if (visibleFlag(f, options) and f.env != null) return true;
    return false;
}

fn hasVisibleFlagGroups(
    comptime groups: []const flag_mod.FlagGroup,
    comptime flags: []const flag_mod.Flag,
    comptime options: Options,
) bool {
    for (groups) |group| if (visibleFlagGroup(group, flags, options)) return true;
    return false;
}

fn visibleFlagGroup(
    comptime group: flag_mod.FlagGroup,
    comptime flags: []const flag_mod.Flag,
    comptime options: Options,
) bool {
    if (group.flags.len == 0) return false;
    for (group.flags) |member| {
        const flag = flagByLong(flags, member) orelse return false;
        if (!visibleFlag(flag, options)) return false;
    }
    return true;
}

fn flagByLong(comptime flags: []const flag_mod.Flag, comptime long: []const u8) ?flag_mod.Flag {
    for (flags) |f| {
        if (std.mem.eql(u8, f.long, long)) return f;
    }
    return null;
}

fn deprecationSuffix(comptime d: anytype) []const u8 {
    comptime {
        var out: []const u8 = " (deprecated";
        if (d.replacement) |replacement| out = out ++ "; use " ++ replacement;
        if (d.message.len > 0) out = out ++ "; " ++ d.message;
        return out ++ ")";
    }
}

fn renderDefault(comptime d: flag_mod.Default) []const u8 {
    comptime {
        return switch (d) {
            .bool => |b| if (b) "true" else "false",
            .string, .choice, .path => |s| "\"" ++ s ++ "\"",
            .int => |i| std.fmt.comptimePrint("{d}", .{i}),
            .float => |x| std.fmt.comptimePrint("{d}", .{x}),
            .duration => |ns| duration_mod.formatNanos(ns),
        };
    }
}

fn flagGroupModeLabel(comptime mode: flag_mod.FlagGroupMode) []const u8 {
    return switch (mode) {
        .mutually_exclusive => "mutually exclusive",
        .required_one => "at least one required",
        .required_exactly_one => "exactly one required",
    };
}

fn joinFlagNames(comptime flags: []const []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (flags, 0..) |name, idx| {
            if (idx > 0) out = out ++ ", ";
            out = out ++ name;
        }
        return out;
    }
}

/// Label shown in the flag table: the kind name, or the `a|b|c` choice list
/// for a choice flag so the allowed values are visible at a glance.
fn flagKindLabel(comptime f: flag_mod.Flag) []const u8 {
    comptime {
        if (f.count) return "count";
        if (f.kind == .choice) return joinChoices(f.choices);
        return @tagName(f.kind);
    }
}

fn joinChoices(comptime choices: []const []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (choices, 0..) |c, i| {
            if (i > 0) out = out ++ "|";
            out = out ++ c;
        }
        return out;
    }
}

fn padTo(comptime s: []const u8, comptime width: usize) []const u8 {
    comptime {
        if (s.len >= width) return "  ";
        var pad: []const u8 = "";
        var remaining: usize = width - s.len;
        while (remaining > 0) : (remaining -= 1) pad = pad ++ " ";
        return pad;
    }
}

// ---- tests ----

const test_root = cmd_mod.Cmd{
    .name = "tool",
    .desc = "Test tool for the cli library",
    .flags = &.{
        .{ .long = "--verbose", .short = 'v', .desc = "Enable verbose output", .kind = .bool, .default = .{ .bool = false } },
    },
    .cmds = &.{
        .{
            .name = "task",
            .desc = "Manage tasks",
            .cmds = &.{
                .{
                    .name = "add",
                    .desc = "Add a task",
                    .flags = &.{
                        .{ .long = "--title", .desc = "Task title", .kind = .string, .required = true },
                    },
                    .flag_groups = &.{
                        .{
                            .name = "task-input",
                            .mode = .required_one,
                            .flags = &.{ "--verbose", "--title" },
                            .desc = "Choose a task input.",
                        },
                    },
                    .positionals = &.{
                        .{ .name = "scope", .desc = "Optional scope", .kind = .string, .required = false },
                    },
                },
            },
        },
    },
};

test "helpText for root includes top-level commands and flags" {
    const text = comptime helpText(test_root, &.{});
    try std.testing.expect(std.mem.indexOf(u8, text, "Test tool") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "task") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "--verbose") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "Enable verbose output") != null);
}

test "helpText for leaf includes flags and positionals" {
    const text = comptime helpText(test_root, &.{ "task", "add" });
    try std.testing.expect(std.mem.indexOf(u8, text, "Add a task") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "--title") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "required") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "<scope>") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "optional") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "FLAG GROUPS:") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "task-input") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "at least one required: --verbose, --title") != null);
}

test "helpText colorizes headers and names only when color is enabled" {
    const plain = comptime helpText(test_root, &.{});
    try std.testing.expect(std.mem.indexOf(u8, plain, "\x1b[") == null);

    const colored = comptime helpTextWithOptions(test_root, &.{}, .{ .color = true });
    try std.testing.expect(std.mem.indexOf(u8, colored, "\x1b[1mUSAGE:\x1b[0m") != null);
    try std.testing.expect(std.mem.indexOf(u8, colored, "\x1b[1mFLAGS:\x1b[0m") != null);
    try std.testing.expect(std.mem.indexOf(u8, colored, "\x1b[36m--verbose") != null);
    // Column alignment is computed from the uncolored label, so the kind tag
    // still appears (the escapes don't push it off into a different column).
    try std.testing.expect(std.mem.indexOf(u8, colored, "(bool)") != null);
}

const alias_root = cmd_mod.Cmd{
    .name = "tool",
    .cmds = &.{
        .{
            .name = "status",
            .aliases = &.{ "st", "stat" },
            .desc = "Show status",
            .flags = &.{
                .{ .long = "--output", .short = 'o', .aliases = &.{ "--out", "--format" }, .desc = "Output format", .kind = .string },
            },
        },
    },
};

test "helpText surfaces command and flag aliases" {
    const root_text = comptime helpText(alias_root, &.{});
    try std.testing.expect(std.mem.indexOf(u8, root_text, "status, st, stat") != null);

    const leaf_text = comptime helpText(alias_root, &.{"status"});
    try std.testing.expect(std.mem.indexOf(u8, leaf_text, "--output, -o, --out, --format") != null);
}

const inherited_root = cmd_mod.Cmd{
    .name = "tool",
    .flags = &.{
        .{ .long = "--root", .desc = "Root flag", .kind = .bool },
    },
    .cmds = &.{
        .{
            .name = "parent",
            .flags = &.{
                .{ .long = "--parent", .desc = "Parent flag", .kind = .string },
            },
            .cmds = &.{
                .{
                    .name = "leaf",
                    .flags = &.{
                        .{ .long = "--leaf", .desc = "Leaf flag", .kind = .int },
                    },
                },
            },
        },
    },
};

test "helpText renders inherited flags before local flags" {
    const parent = comptime helpText(inherited_root, &.{"parent"});
    try std.testing.expect(std.mem.indexOf(u8, parent, "USAGE:\n  parent [flags] <command>") != null);
    const parent_root_idx = std.mem.indexOf(u8, parent, "--root").?;
    const parent_local_idx = std.mem.indexOf(u8, parent, "--parent").?;
    try std.testing.expect(parent_root_idx < parent_local_idx);

    const leaf = comptime helpText(inherited_root, &.{ "parent", "leaf" });
    try std.testing.expect(std.mem.indexOf(u8, leaf, "USAGE:\n  parent leaf [flags]") != null);
    const root_idx = std.mem.indexOf(u8, leaf, "--root").?;
    const parent_idx = std.mem.indexOf(u8, leaf, "--parent").?;
    const leaf_idx = std.mem.indexOf(u8, leaf, "--leaf").?;
    try std.testing.expect(root_idx < parent_idx);
    try std.testing.expect(parent_idx < leaf_idx);
}

const hidden_only_root = cmd_mod.Cmd{
    .name = "tool",
    .flags = &.{
        .{ .long = "--hidden-root", .hidden = true, .kind = .bool },
    },
    .cmds = &.{
        .{
            .name = "leaf",
        },
    },
};

test "helpText omits flags section and usage marker when only hidden inherited flags apply" {
    const text = comptime helpText(hidden_only_root, &.{"leaf"});
    try std.testing.expect(std.mem.indexOf(u8, text, "USAGE:\n  leaf\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "[flags]") == null);
    try std.testing.expect(std.mem.indexOf(u8, text, "FLAGS:") == null);
    try std.testing.expect(std.mem.indexOf(u8, text, "--hidden-root") == null);

    const hidden_text = comptime helpTextWithOptions(hidden_only_root, &.{"leaf"}, .{ .include_hidden = true });
    try std.testing.expect(std.mem.indexOf(u8, hidden_text, "USAGE:\n  leaf [flags]\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, hidden_text, "FLAGS:") != null);
    try std.testing.expect(std.mem.indexOf(u8, hidden_text, "--hidden-root") != null);
}
