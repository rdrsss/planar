//! Comptime command schema generator.
//!
//! `json(root, options)` emits a deterministic flat JSON catalog intended for
//! LLMs, tool routers, and other structured consumers. It is not a replacement
//! for human-facing help or man pages.

const std = @import("std");
const cmd_mod = @import("cmd.zig");
const doc_mod = @import("doc.zig");
const flag_mod = @import("flag.zig");

pub const Options = struct {
    include_inherited_flags: bool = true,
    include_docs: bool = true,
    include_env_metadata: bool = true,
    // Additive opt-in view; schemaVersion stays 1 because callers request it.
    include_command_tree: bool = false,
    include_hidden: bool = false,
    include_deprecated: bool = true,
};

pub fn json(comptime root: cmd_mod.Cmd, comptime options: Options) []const u8 {
    @setEvalBranchQuota(8_000_000);
    return comptime renderRoot(root, options);
}

pub fn writeJson(
    comptime root: cmd_mod.Cmd,
    comptime options: Options,
    writer: *std.Io.Writer,
) std.Io.Writer.Error!void {
    try writer.writeAll(comptime json(root, options));
}

fn renderRoot(comptime root: cmd_mod.Cmd, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "{";
        out = out ++ "\"schemaVersion\":1,";
        out = out ++ "\"layout\":\"flat\",";
        out = out ++ "\"root\":" ++ jsonString(root.name) ++ ",";
        out = out ++ "\"commands\":[";
        out = out ++ renderCommand(root, root, &.{}, options);
        out = out ++ renderDescendantCommands(root, root, &.{}, options);
        out = out ++ "]";
        if (options.include_command_tree) {
            out = out ++ ",\"commandTree\":" ++ renderCommandTree(root, root, &.{}, options);
        }
        out = out ++ "}";
        return out;
    }
}

fn renderDescendantCommands(
    comptime root: cmd_mod.Cmd,
    comptime node: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (node.cmds) |child| {
            if (!visibleCmd(child, options)) continue;
            const child_path = path ++ &[_][]const u8{child.name};
            out = out ++ "," ++ renderCommand(root, child, child_path, options);
            out = out ++ renderDescendantCommands(root, child, child_path, options);
        }
        return out;
    }
}

fn renderCommand(
    comptime root: cmd_mod.Cmd,
    comptime node: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const u8 {
    comptime {
        var out: []const u8 = "{";
        out = out ++ renderCommandFields(root, node, path, options);
        out = out ++ "}";
        return out;
    }
}

fn renderCommandFields(
    comptime root: cmd_mod.Cmd,
    comptime node: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const u8 {
    comptime {
        var out: []const u8 = "";
        out = out ++ "\"name\":" ++ jsonString(node.name) ++ ",";
        out = out ++ "\"aliases\":" ++ renderStringArray(node.aliases) ++ ",";
        out = out ++ "\"hidden\":" ++ boolText(node.hidden) ++ ",";
        out = out ++ "\"deprecated\":" ++ renderDeprecation(node.deprecated) ++ ",";
        out = out ++ "\"path\":" ++ renderStringArray(path) ++ ",";
        out = out ++ "\"command\":" ++ jsonString(commandPath(root, path)) ++ ",";
        out = out ++ "\"summary\":" ++ jsonString(if (options.include_docs) node.desc else "") ++ ",";
        out = out ++ "\"description\":" ++ jsonString(if (options.include_docs) (if (node.long_desc.len > 0) node.long_desc else node.desc) else "") ++ ",";
        out = out ++ "\"subcommands\":" ++ renderSubcommands(node.cmds, options) ++ ",";
        out = out ++ "\"flags\":" ++ renderFlags(root, node, path, options) ++ ",";
        out = out ++ "\"flagGroups\":" ++ renderFlagGroups(root, node, path, options) ++ ",";
        out = out ++ "\"positionals\":" ++ renderPositionals(node.positionals, options);
        if (options.include_docs) out = out ++ ",\"docs\":" ++ renderDocs(node.doc);
        return out;
    }
}

fn renderFlags(
    comptime root: cmd_mod.Cmd,
    comptime node: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const u8 {
    comptime {
        const inherited = if (options.include_inherited_flags) cmd_mod.collectInheritedFlags(root, path) else &.{};
        var out: []const u8 = "[";
        var first = true;
        for (inherited) |f| {
            if (!visibleFlag(f, options)) continue;
            if (!first) out = out ++ ",";
            first = false;
            out = out ++ renderFlag(f, "inherited", options);
        }
        for (node.flags) |f| {
            if (!visibleFlag(f, options)) continue;
            if (!first) out = out ++ ",";
            first = false;
            out = out ++ renderFlag(f, "local", options);
        }
        out = out ++ "]";
        return out;
    }
}

fn renderFlagGroups(
    comptime root: cmd_mod.Cmd,
    comptime node: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const u8 {
    comptime {
        const inherited: []const flag_mod.Flag = if (options.include_inherited_flags) cmd_mod.collectInheritedFlags(root, path) else &.{};
        const visible_flags = inherited ++ node.flags;
        var out: []const u8 = "[";
        var first = true;
        for (node.flag_groups) |group| {
            if (!visibleFlagGroup(group, visible_flags, options)) continue;
            if (!first) out = out ++ ",";
            first = false;
            out = out ++ renderFlagGroup(group, options);
        }
        out = out ++ "]";
        return out;
    }
}

fn renderFlagGroup(comptime group: flag_mod.FlagGroup, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "{";
        out = out ++ "\"name\":" ++ jsonString(group.name) ++ ",";
        out = out ++ "\"mode\":" ++ jsonString(@tagName(group.mode)) ++ ",";
        out = out ++ "\"flags\":" ++ renderStringArray(group.flags) ++ ",";
        out = out ++ "\"description\":" ++ jsonString(if (options.include_docs) group.desc else "");
        out = out ++ "}";
        return out;
    }
}

fn renderFlag(comptime f: flag_mod.Flag, comptime source: []const u8, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "{";
        out = out ++ "\"long\":" ++ jsonString(f.long) ++ ",";
        out = out ++ "\"aliases\":" ++ renderStringArray(f.aliases) ++ ",";
        out = out ++ "\"hidden\":" ++ boolText(f.hidden) ++ ",";
        out = out ++ "\"deprecated\":" ++ renderDeprecation(f.deprecated) ++ ",";
        out = out ++ "\"short\":";
        if (f.short) |s| {
            out = out ++ jsonString(&[_]u8{s});
        } else {
            out = out ++ "null";
        }
        out = out ++ ",";
        out = out ++ "\"kind\":" ++ jsonString(@tagName(f.kind)) ++ ",";
        out = out ++ "\"choices\":" ++ renderStringArray(f.choices) ++ ",";
        out = out ++ "\"list\":" ++ boolText(f.list) ++ ",";
        out = out ++ "\"count\":" ++ boolText(f.count) ++ ",";
        out = out ++ "\"required\":" ++ boolText(f.required) ++ ",";
        out = out ++ "\"source\":" ++ jsonString(source) ++ ",";
        out = out ++ "\"valueName\":";
        if (f.value_name) |name| {
            out = out ++ jsonString(name);
        } else {
            out = out ++ jsonString(fallbackValueName(f.kind));
        }
        out = out ++ ",";
        out = out ++ "\"default\":" ++ renderDefault(f.default) ++ ",";
        out = out ++ "\"description\":" ++ jsonString(if (options.include_docs) f.desc else "") ++ ",";
        out = out ++ "\"completion\":" ++ renderCompletion(f.completion);
        if (options.include_env_metadata) {
            out = out ++ ",\"env\":";
            if (f.env) |env| {
                out = out ++ jsonString(env) ++ ",\"envBehavior\":\"cli-run-fallback\"";
            } else {
                out = out ++ "null";
            }
        }
        out = out ++ "}";
        return out;
    }
}

fn renderPositionals(comptime positionals: []const flag_mod.Positional, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "[";
        for (positionals, 0..) |p, idx| {
            if (idx > 0) out = out ++ ",";
            out = out ++ "{";
            out = out ++ "\"name\":" ++ jsonString(p.name) ++ ",";
            out = out ++ "\"kind\":" ++ jsonString(@tagName(p.kind)) ++ ",";
            out = out ++ "\"required\":" ++ boolText(p.required) ++ ",";
            out = out ++ "\"default\":" ++ renderDefault(p.default) ++ ",";
            out = out ++ "\"description\":" ++ jsonString(if (options.include_docs) p.desc else "") ++ ",";
            out = out ++ "\"completion\":" ++ renderCompletion(p.completion);
            out = out ++ "}";
        }
        out = out ++ "]";
        return out;
    }
}

fn renderDocs(comptime doc: doc_mod.Doc) []const u8 {
    comptime {
        var out: []const u8 = "{";
        out = out ++ "\"examples\":" ++ renderExamples(doc.examples) ++ ",";
        out = out ++ "\"exitCodes\":" ++ renderExitCodes(doc.exit_codes) ++ ",";
        out = out ++ "\"notes\":" ++ renderStringArray(doc.notes) ++ ",";
        out = out ++ "\"seeAlso\":" ++ renderStringArray(doc.see_also) ++ ",";
        out = out ++ "\"files\":" ++ renderStringArray(doc.files) ++ ",";
        out = out ++ "\"bugs\":" ++ renderStringArray(doc.bugs) ++ ",";
        out = out ++ "\"authors\":" ++ renderStringArray(doc.authors) ++ ",";
        out = out ++ "\"homepage\":" ++ jsonString(doc.homepage) ++ ",";
        out = out ++ "\"license\":" ++ jsonString(doc.license) ++ ",";
        out = out ++ "\"copyright\":" ++ jsonString(doc.copyright) ++ ",";
        out = out ++ "\"version\":" ++ jsonString(doc.version) ++ ",";
        out = out ++ "\"sourceUrl\":" ++ jsonString(doc.source_url);
        out = out ++ "}";
        return out;
    }
}

fn renderExamples(comptime examples: []const doc_mod.Example) []const u8 {
    comptime {
        var out: []const u8 = "[";
        for (examples, 0..) |example, idx| {
            if (idx > 0) out = out ++ ",";
            out = out ++ "{";
            out = out ++ "\"title\":" ++ jsonString(example.title) ++ ",";
            out = out ++ "\"command\":" ++ jsonString(example.command) ++ ",";
            out = out ++ "\"description\":" ++ jsonString(example.desc);
            out = out ++ "}";
        }
        out = out ++ "]";
        return out;
    }
}

fn renderExitCodes(comptime exit_codes: []const doc_mod.ExitCode) []const u8 {
    comptime {
        var out: []const u8 = "[";
        for (exit_codes, 0..) |exit_code, idx| {
            if (idx > 0) out = out ++ ",";
            out = out ++ "{";
            out = out ++ "\"code\":" ++ std.fmt.comptimePrint("{d}", .{exit_code.code}) ++ ",";
            out = out ++ "\"description\":" ++ jsonString(exit_code.desc);
            out = out ++ "}";
        }
        out = out ++ "]";
        return out;
    }
}

fn renderSubcommands(comptime cmds: []const cmd_mod.Cmd, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "[";
        var first = true;
        for (cmds) |child| {
            if (!visibleCmd(child, options)) continue;
            if (!first) out = out ++ ",";
            first = false;
            out = out ++ jsonString(child.name);
        }
        out = out ++ "]";
        return out;
    }
}

fn renderCommandTree(
    comptime root: cmd_mod.Cmd,
    comptime node: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: Options,
) []const u8 {
    comptime {
        var out: []const u8 = "{";
        out = out ++ renderCommandFields(root, node, path, options);
        out = out ++ ",\"children\":[";
        var first = true;
        for (node.cmds) |child| {
            if (!visibleCmd(child, options)) continue;
            if (!first) out = out ++ ",";
            first = false;
            out = out ++ renderCommandTree(root, child, path ++ &[_][]const u8{child.name}, options);
        }
        out = out ++ "]}";
        return out;
    }
}

fn renderDeprecation(comptime deprecated: anytype) []const u8 {
    comptime {
        if (deprecated == null) return "null";
        const d = deprecated.?;
        var out: []const u8 = "{";
        out = out ++ "\"message\":" ++ jsonString(d.message) ++ ",";
        out = out ++ "\"replacement\":";
        if (d.replacement) |replacement| {
            out = out ++ jsonString(replacement);
        } else {
            out = out ++ "null";
        }
        out = out ++ "}";
        return out;
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

fn renderStringArray(comptime values: []const []const u8) []const u8 {
    comptime {
        var out: []const u8 = "[";
        for (values, 0..) |value, idx| {
            if (idx > 0) out = out ++ ",";
            out = out ++ jsonString(value);
        }
        out = out ++ "]";
        return out;
    }
}

fn renderCompletion(comptime completion: anytype) []const u8 {
    comptime {
        var out: []const u8 = "{";
        out = out ++ "\"kind\":" ++ jsonString(@tagName(completion.kind)) ++ ",";
        out = out ++ "\"values\":" ++ renderStringArray(completion.values);
        out = out ++ "}";
        return out;
    }
}

fn renderDefault(comptime default: ?flag_mod.Default) []const u8 {
    comptime {
        if (default == null) return "null";
        return switch (default.?) {
            .bool => |b| boolText(b),
            .string, .choice, .path => |s| jsonString(s),
            .int => |i| std.fmt.comptimePrint("{d}", .{i}),
            .float => |x| std.fmt.comptimePrint("{d}", .{x}),
            // Nanoseconds — precise integer for machine consumers.
            .duration => |ns| std.fmt.comptimePrint("{d}", .{ns}),
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

fn fallbackValueName(comptime kind: flag_mod.Kind) []const u8 {
    return switch (kind) {
        .bool => "",
        .string => "VALUE",
        .int => "N",
        .float => "X",
        .duration => "DURATION",
        .path => "PATH",
        .choice => "",
    };
}

fn boolText(comptime value: bool) []const u8 {
    return if (value) "true" else "false";
}

fn jsonString(comptime text: []const u8) []const u8 {
    comptime {
        var out: []const u8 = "\"";
        for (text) |c| {
            out = out ++ switch (c) {
                '"' => "\\\"",
                '\\' => "\\\\",
                '\n' => "\\n",
                '\r' => "\\r",
                '\t' => "\\t",
                0x08 => "\\b",
                0x0c => "\\f",
                // Remaining C0 control characters have no short escape and
                // are invalid raw in a JSON string; emit a \u00XX escape.
                0x00...0x07, 0x0b, 0x0e...0x1f => std.fmt.comptimePrint("\\u{x:0>4}", .{c}),
                else => &[_]u8{c},
            };
        }
        out = out ++ "\"";
        return out;
    }
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
            .desc = "Run command",
            .long_desc = "Run command with docs.",
            .flags = &.{
                .{ .long = "--name", .desc = "Name value", .kind = .string, .value_name = "NAME", .required = true, .env = "TOOL_NAME" },
            },
            .flag_groups = &.{
                .{
                    .name = "run-input",
                    .mode = .required_one,
                    .flags = &.{ "--verbose", "--name" },
                    .desc = "Choose a run input.",
                },
            },
            .positionals = &.{
                .{ .name = "target", .desc = "Target value", .kind = .string },
            },
            .doc = .{
                .examples = &.{.{ .title = "Run", .command = "tool run --name demo target", .desc = "Runs a target." }},
                .exit_codes = &.{.{ .code = 0, .desc = "Success." }},
                .notes = &.{"Schema data is generated at comptime."},
                .see_also = &.{"tool(1)"},
            },
        },
    },
};

test "json emits flat schema for root and child commands" {
    const text = comptime json(test_root, .{});
    try std.testing.expect(std.mem.indexOf(u8, text, "\"schemaVersion\":1") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\"layout\":\"flat\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\"path\":[]") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\"path\":[\"run\"]") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\"source\":\"inherited\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\"envBehavior\":\"cli-run-fallback\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\"flagGroups\":[{\"name\":\"run-input\",\"mode\":\"required_one\",\"flags\":[\"--verbose\",\"--name\"],\"description\":\"Choose a run input.\"}]") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "\"examples\":[") != null);
}

test "json escapes strings" {
    const root = cmd_mod.Cmd{
        .name = "escape",
        .desc = "quote \" slash \\ newline\n",
    };
    const text = comptime json(root, .{});
    try std.testing.expect(std.mem.indexOf(u8, text, "quote \\\" slash \\\\ newline\\n") != null);
}

test "json escapes control characters" {
    const root = cmd_mod.Cmd{
        .name = "ctrl",
        .desc = "bell\x07 back\x08 form\x0c vtab\x0b unit\x1f",
    };
    const text = comptime json(root, .{});
    try std.testing.expect(std.mem.indexOf(u8, text, "bell\\u0007") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "back\\b") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "form\\f") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "vtab\\u000b") != null);
    try std.testing.expect(std.mem.indexOf(u8, text, "unit\\u001f") != null);
    // No raw control byte should survive into the output.
    try std.testing.expect(std.mem.indexOfScalar(u8, text, 0x07) == null);
}
