//! Comptime man-page generator.
//!
//! `page(root, path, options)` returns portable `man(7)` roff text for one
//! command path. The string is built at comptime and can be printed or written
//! by downstream build/install code.

const std = @import("std");
const cmd_mod = @import("cmd.zig");
const doc_mod = @import("doc.zig");
const flag_mod = @import("flag.zig");

pub const Options = struct {
    section: u8 = 1,
    title: ?[]const u8 = null,
    date: []const u8 = "",
    source: []const u8 = "",
    manual: []const u8 = "",
    include_inherited_flags: bool = true,
    include_hidden: bool = false,
    include_deprecated: bool = true,
};

pub const Page = struct {
    name: []const u8,
    path: []const []const u8,
    data: []const u8,
};

pub fn page(
    comptime root: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const u8 {
    @setEvalBranchQuota(4_000_000);
    if (options.section != 1) {
        @compileError("cli.man.page: only section 1 is supported for now");
    }
    const target = comptime cmd_mod.findCmd(root, path) orelse @compileError(
        "cli.man.page: no command at path",
    );
    return comptime renderPage(root, target, path, options);
}

pub fn writePage(
    comptime root: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
    writer: *std.Io.Writer,
) std.Io.Writer.Error!void {
    try writer.writeAll(comptime page(root, path, options));
}

pub fn allPages(comptime root: cmd_mod.Cmd, comptime options: Options) []const Page {
    @setEvalBranchQuota(20_000_000);
    return comptime allPagesForNode(root, root, &.{}, options);
}

pub fn pageName(comptime root: cmd_mod.Cmd, comptime path: []const []const u8) []const u8 {
    comptime {
        var out: []const u8 = root.name;
        for (path) |seg| out = out ++ "-" ++ seg;
        return out;
    }
}

fn allPagesForNode(
    comptime root: cmd_mod.Cmd,
    comptime node: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const Page {
    comptime {
        var out: []const Page = &.{
            .{
                .name = pageName(root, path),
                .path = path,
                .data = renderPage(root, node, path, options),
            },
        };
        for (node.cmds) |child| {
            if (!visibleCmd(child, options)) continue;
            out = out ++ allPagesForNode(root, child, path ++ [_][]const u8{child.name}, options);
        }
        return out;
    }
}

fn renderPage(
    comptime root: cmd_mod.Cmd,
    comptime node: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const u8 {
    comptime {
        const name = pageName(root, path);
        const title = options.title orelse name;
        const command = commandPath(root, path);
        const desc = if (node.desc.len > 0) node.desc else "";
        const flags = flagsFor(root, node, path, options.include_inherited_flags);

        var out: []const u8 = "";
        out = out ++ ".TH \"" ++ roffQuoted(title) ++ "\" \"" ++ std.fmt.comptimePrint("{d}", .{options.section}) ++ "\"";
        out = out ++ " \"" ++ roffQuoted(options.date) ++ "\"";
        out = out ++ " \"" ++ roffQuoted(options.source) ++ "\"";
        out = out ++ " \"" ++ roffQuoted(options.manual) ++ "\"\n";

        out = out ++ ".SH NAME\n";
        out = out ++ roff(name);
        if (desc.len > 0) out = out ++ " \\- " ++ roff(desc);
        out = out ++ "\n";

        out = out ++ ".SH SYNOPSIS\n";
        out = out ++ ".B " ++ roff(command) ++ "\n";
        var synopsis_tail: []const u8 = "";
        if (flags.len > 0) synopsis_tail = synopsis_tail ++ " [OPTIONS]";
        if (node.cmds.len > 0) synopsis_tail = synopsis_tail ++ " <command>";
        for (node.positionals) |p| {
            synopsis_tail = synopsis_tail ++ " ";
            if (p.required) {
                synopsis_tail = synopsis_tail ++ "<" ++ roff(p.name) ++ ">";
            } else {
                synopsis_tail = synopsis_tail ++ "[" ++ roff(p.name) ++ "]";
            }
        }
        if (synopsis_tail.len > 0) out = out ++ ".RI \"" ++ roffQuoted(synopsis_tail) ++ "\"\n";

        const long_desc = if (node.long_desc.len > 0) node.long_desc else node.desc;
        if (long_desc.len > 0) {
            out = out ++ ".SH DESCRIPTION\n";
            out = out ++ ".PP\n" ++ roff(long_desc) ++ "\n";
        }

        if (hasVisibleCommands(node.cmds, options)) {
            out = out ++ ".SH COMMANDS\n";
            for (node.cmds) |child| {
                if (!visibleCmd(child, options)) continue;
                out = out ++ ".TP\n";
                out = out ++ ".B " ++ roff(child.name) ++ "\n";
                if (child.desc.len > 0) out = out ++ roff(child.desc) ++ "\n";
                if (child.deprecated) |d| out = out ++ roff(deprecationText(d)) ++ "\n";
            }
        }

        if (hasVisibleFlags(flags, options)) {
            out = out ++ ".SH OPTIONS\n";
            for (flags) |f| {
                if (!visibleFlag(f, options)) continue;
                out = out ++ renderFlag(f);
            }
        }

        if (hasEnv(flags, options)) {
            out = out ++ ".SH ENVIRONMENT\n";
            for (flags) |f| {
                if (!visibleFlag(f, options)) continue;
                out = out ++ renderEnv(f);
            }
        }

        if (node.positionals.len > 0) {
            out = out ++ ".SH ARGUMENTS\n";
            for (node.positionals) |p| out = out ++ renderPositional(p);
        }

        if (node.doc.examples.len > 0) {
            out = out ++ ".SH EXAMPLES\n";
            for (node.doc.examples) |example| out = out ++ renderExample(example);
        }

        if (node.doc.exit_codes.len > 0) {
            out = out ++ ".SH EXIT STATUS\n";
            for (node.doc.exit_codes) |exit_code| out = out ++ renderExitCode(exit_code);
        }

        if (node.doc.notes.len > 0) {
            out = out ++ ".SH NOTES\n";
            for (node.doc.notes) |note| out = out ++ ".PP\n" ++ roff(note) ++ "\n";
        }

        if (node.doc.files.len > 0) {
            out = out ++ ".SH FILES\n";
            for (node.doc.files) |file| out = out ++ ".TP\n.I " ++ roff(file) ++ "\n";
        }

        if (node.doc.homepage.len > 0 or node.doc.source_url.len > 0 or node.doc.version.len > 0) {
            out = out ++ ".SH PROJECT\n";
            if (node.doc.homepage.len > 0) out = out ++ ".TP\n.B Homepage\n" ++ roff(node.doc.homepage) ++ "\n";
            if (node.doc.source_url.len > 0) out = out ++ ".TP\n.B Source\n" ++ roff(node.doc.source_url) ++ "\n";
            if (node.doc.version.len > 0) out = out ++ ".TP\n.B Version\n" ++ roff(node.doc.version) ++ "\n";
        }

        if (node.doc.bugs.len > 0) {
            out = out ++ ".SH BUGS\n";
            for (node.doc.bugs) |bug| out = out ++ ".PP\n" ++ roff(bug) ++ "\n";
        }

        if (node.doc.authors.len > 0) {
            out = out ++ ".SH AUTHORS\n";
            for (node.doc.authors) |author| out = out ++ ".PP\n" ++ roff(author) ++ "\n";
        }

        if (node.doc.license.len > 0 or node.doc.copyright.len > 0) {
            out = out ++ ".SH COPYRIGHT\n";
            if (node.doc.copyright.len > 0) out = out ++ ".PP\n" ++ roff(node.doc.copyright) ++ "\n";
            if (node.doc.license.len > 0) out = out ++ ".PP\nLicense: " ++ roff(node.doc.license) ++ "\n";
        }

        if (node.doc.see_also.len > 0) {
            out = out ++ ".SH SEE ALSO\n";
            out = out ++ renderSeeAlso(node.doc.see_also) ++ "\n";
        }

        return out;
    }
}

fn flagsFor(
    comptime root: cmd_mod.Cmd,
    comptime node: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime include_inherited: bool,
) []const flag_mod.Flag {
    comptime {
        if (include_inherited) {
            return cmd_mod.collectInheritedFlags(root, path) ++ node.flags;
        }
        return node.flags;
    }
}

fn renderFlag(comptime f: flag_mod.Flag) []const u8 {
    comptime {
        const value = flagValuePlaceholder(f);
        var out: []const u8 = ".TP\n.B " ++ roffOption(f.long);
        if (value.len > 0) out = out ++ " " ++ value;
        if (f.short) |s| {
            out = out ++ ", " ++ roffOption("-" ++ &[_]u8{s});
            if (value.len > 0) out = out ++ " " ++ value;
        }
        out = out ++ "\n";
        out = out ++ "type: " ++ @tagName(f.kind);
        if (value.len > 0) out = out ++ ", value: " ++ value;
        if (f.required) out = out ++ ", required";
        if (f.default) |d| out = out ++ ", default: " ++ renderDefault(d);
        if (f.desc.len > 0) out = out ++ "\n" ++ roff(f.desc);
        if (f.deprecated) |d| out = out ++ "\n" ++ roff(deprecationText(d));
        out = out ++ "\n";
        return out;
    }
}

fn renderPositional(comptime p: flag_mod.Positional) []const u8 {
    comptime {
        var out: []const u8 = ".TP\n.I " ++ roff(p.name) ++ "\n";
        out = out ++ "type: " ++ @tagName(p.kind);
        if (!p.required) out = out ++ ", optional";
        if (p.desc.len > 0) out = out ++ "\n" ++ roff(p.desc);
        out = out ++ "\n";
        return out;
    }
}

fn renderEnv(comptime f: flag_mod.Flag) []const u8 {
    comptime {
        if (f.env == null) return "";
        var out: []const u8 = ".TP\n.B " ++ roff(f.env.?) ++ "\n";
        out = out ++ "Associated with " ++ roffOption(f.long) ++ " metadata. The parser does not read environment variables.\n";
        return out;
    }
}

fn renderExample(comptime example: doc_mod.Example) []const u8 {
    comptime {
        var out: []const u8 = "";
        if (example.title.len > 0) out = out ++ ".SS " ++ roff(example.title) ++ "\n";
        out = out ++ ".TP\n.B " ++ roff(example.command) ++ "\n";
        if (example.desc.len > 0) out = out ++ roff(example.desc) ++ "\n";
        return out;
    }
}

fn renderExitCode(comptime exit_code: doc_mod.ExitCode) []const u8 {
    comptime {
        return ".TP\n.B " ++ std.fmt.comptimePrint("{d}", .{exit_code.code}) ++ "\n" ++ roff(exit_code.desc) ++ "\n";
    }
}

fn renderSeeAlso(comptime see_also: []const []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (see_also, 0..) |item, idx| {
            if (idx > 0) out = out ++ ", ";
            out = out ++ roff(item);
        }
        return out;
    }
}

fn hasEnv(comptime flags: []const flag_mod.Flag, comptime options: Options) bool {
    comptime {
        for (flags) |f| if (visibleFlag(f, options) and f.env != null) return true;
        return false;
    }
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

fn deprecationText(comptime d: anytype) []const u8 {
    comptime {
        var out: []const u8 = "Deprecated";
        if (d.replacement) |replacement| out = out ++ "; use " ++ replacement;
        if (d.message.len > 0) out = out ++ "; " ++ d.message;
        return out ++ ".";
    }
}

fn renderDefault(comptime d: flag_mod.Default) []const u8 {
    comptime {
        return switch (d) {
            .bool => |b| if (b) "true" else "false",
            .string => |s| "\"" ++ roff(s) ++ "\"",
            .int => |i| std.fmt.comptimePrint("{d}", .{i}),
        };
    }
}

fn commandPath(comptime root: cmd_mod.Cmd, comptime path: []const []const u8) []const u8 {
    comptime {
        var out: []const u8 = root.name;
        for (path) |seg| out = out ++ " " ++ seg;
        return out;
    }
}

fn roffOption(comptime text: []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (text) |c| {
            if (c == '-') {
                out = out ++ "\\-";
            } else {
                out = out ++ roffChar(c);
            }
        }
        return out;
    }
}

fn roff(comptime text: []const u8) []const u8 {
    comptime {
        if (text.len == 0) return "";
        var out: []const u8 = "";
        var line_start = true;
        for (text) |c| {
            if (line_start and (c == '.' or c == '\'')) out = out ++ "\\&";
            out = out ++ roffChar(c);
            line_start = c == '\n';
        }
        return out;
    }
}

fn roffQuoted(comptime text: []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (roff(text)) |c| {
            if (c == '"') {
                out = out ++ "\\(dq";
            } else {
                out = out ++ &[_]u8{c};
            }
        }
        return out;
    }
}

fn roffChar(comptime c: u8) []const u8 {
    return switch (c) {
        '\\' => "\\e",
        else => &[_]u8{c},
    };
}

fn flagValuePlaceholder(comptime f: flag_mod.Flag) []const u8 {
    return f.value_name orelse fallbackValuePlaceholder(f.kind);
}

fn fallbackValuePlaceholder(comptime kind: flag_mod.Kind) []const u8 {
    return switch (kind) {
        .bool => "",
        .string => "VALUE",
        .int => "N",
    };
}

const test_root = cmd_mod.Cmd{
    .name = "tool",
    .desc = "Test tool",
    .flags = &.{
        .{ .long = "--verbose", .short = 'v', .desc = "Verbose output", .kind = .bool, .default = .{ .bool = false } },
    },
    .cmds = &.{
        .{
            .name = "run",
            .desc = "Run a target",
            .flags = &.{
                .{ .long = "--count", .desc = "Run count", .kind = .int, .default = .{ .int = 1 } },
            },
            .positionals = &.{
                .{ .name = "target", .desc = "Target name", .kind = .string },
            },
        },
    },
};

test "page renders root sections" {
    const text = comptime page(test_root, &.{}, .{});
    try std.testing.expect(std.mem.indexOf(u8, text, ".TH \"tool\" \"1\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, ".SH NAME") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "tool \\- Test tool") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, ".SH COMMANDS") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, ".SH OPTIONS") != null);
}

test "page renders inherited flags for leaf commands" {
    const text = comptime page(test_root, &.{"run"}, .{});
    try std.testing.expect(std.mem.indexOf(u8, text, "\\-\\-verbose") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\\-\\-count N") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, ".SH ARGUMENTS") != null);
}

test "page omits empty optional sections" {
    const root = cmd_mod.Cmd{ .name = "empty" };
    const text = comptime page(root, &.{}, .{});
    try std.testing.expect(std.mem.indexOf(u8, text, ".SH NAME") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, ".SH SYNOPSIS") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, ".SH DESCRIPTION") == null);
    try std.testing.expect(std.mem.indexOf(u8, text, ".SH COMMANDS") == null);
    try std.testing.expect(std.mem.indexOf(u8, text, ".SH OPTIONS") == null);
    try std.testing.expect(std.mem.indexOf(u8, text, ".SH ARGUMENTS") == null);
}

test "roff escapes text that could become syntax" {
    const root = cmd_mod.Cmd{
        .name = "escape",
        .desc = ".starts with macro\n'and control\nhas \\ slash",
        .flags = &.{
            .{ .long = "--dry-run", .desc = ".flag macro", .kind = .bool },
        },
    };
    const text = comptime page(root, &.{}, .{ .title = "escape \"quoted\"" });
    try std.testing.expect(std.mem.indexOf(u8, text, "escape \\(dqquoted\\(dq") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\\&.starts with macro") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\n\\&'and control") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\\e slash") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\\-\\-dry\\-run") != null);
}
