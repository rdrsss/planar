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

pub const Options = struct {
    include_hidden: bool = false,
    include_deprecated: bool = true,
    /// Optional target width. Widths <= 48 switch tables to a compact
    /// two-line form that keeps descriptions out of narrow columns.
    width: ?usize = null,
};

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
    return comptime renderCmd(target, path, options);
}

pub fn writeText(
    comptime root: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
    writer: *std.Io.Writer,
) std.Io.Writer.Error!void {
    try writer.writeAll(comptime helpTextWithOptions(root, path, options));
}

fn renderCmd(comptime node: cmd_mod.Cmd, comptime path: []const []const u8, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";

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
        var usage: []const u8 = "\nUSAGE:\n  " ++ full_path;
        if (hasVisibleFlags(node.flags, options)) usage = usage ++ " [flags]";
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
            out = out ++ "\nCOMMANDS:\n";
            for (node.cmds) |c| {
                if (!visibleCmd(c, options)) continue;
                if (compact(options)) {
                    out = out ++ "  " ++ c.name;
                    if (c.deprecated) |d| out = out ++ deprecationSuffix(d);
                    out = out ++ "\n";
                    if (c.desc.len > 0) out = out ++ "      " ++ c.desc ++ "\n";
                    continue;
                }
                out = out ++ "  " ++ c.name ++ padTo(c.name, 16);
                if (c.desc.len > 0) out = out ++ c.desc;
                if (c.deprecated) |d| out = out ++ deprecationSuffix(d);
                out = out ++ "\n";
            }
        }

        // Flags.
        if (hasVisibleFlags(node.flags, options)) {
            out = out ++ "\nFLAGS:\n";
            for (node.flags) |f| {
                if (!visibleFlag(f, options)) continue;
                out = out ++ "  " ++ renderFlagLine(f, options) ++ "\n";
            }
        }

        // Positionals.
        if (node.positionals.len > 0) {
            out = out ++ "\nPOSITIONAL ARGUMENTS:\n";
            for (node.positionals) |p| {
                out = out ++ "  <" ++ p.name ++ ">" ++ padTo(p.name, if (compact(options)) 8 else 14) ++ "(" ++ @tagName(p.kind) ++ ")";
                if (!p.required) out = out ++ " optional";
                if (p.desc.len > 0) out = out ++ " — " ++ p.desc;
                out = out ++ "\n";
            }
        }

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
        var out: []const u8 = f.long;
        if (f.short) |s| out = out ++ ", -" ++ &[_]u8{s};
        out = out ++ padTo(out, if (compact(options)) 16 else 22) ++ "(" ++ @tagName(f.kind) ++ ")";
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
            .string => |s| "\"" ++ s ++ "\"",
            .int => |i| std.fmt.comptimePrint("{d}", .{i}),
        };
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
}
