//! Comptime tree validator.
//!
//! `validate(root)` runs at compile time and `@compileError`s on:
//!   - duplicate long-flag names within a command (including inherited)
//!   - duplicate short-flag chars within a command (including inherited)
//!   - duplicate sub-command names within a parent
//!   - invalid command, flag, positional, and rest-field syntax
//!   - generated args field-name collisions
//!   - Default tag mismatched against the flag's Kind
//!   - flag declared `required = true` with a `default` set (contradictory)
//!   - invalid manual metadata such as bool value names, empty examples, and
//!     duplicate example titles and exit-code entries
//!
//! Call once near the tree declaration:
//!
//!   const root = cli.Cmd{ ... };
//!   comptime cli.validate(root);
//!
//! Failures surface at the call site (not inside this file) thanks to
//! `@compileError`'s point-of-use semantics.

const std = @import("std");
const cmd_mod = @import("cmd.zig");
const flag = @import("flag.zig");

pub fn validate(comptime root: cmd_mod.Cmd) void {
    comptime {
        validateNode(root, &.{});
    }
}

fn validateNode(comptime node: cmd_mod.Cmd, comptime parent_flags: []const flag.Flag) void {
    validateCommandName(node.name);
    validateCommandAliases(node);
    validateDeprecation("command", node.name, node.deprecated);

    // Duplicate sub-command names.
    for (node.cmds, 0..) |a, i| {
        for (node.cmds[i + 1 ..]) |b| {
            if (commandNamesOverlap(a, b)) |name| {
                @compileError("cli.validate: duplicate sub-command name or alias '" ++ name ++ "' under '" ++ node.name ++ "'");
            }
        }
    }

    // Build the combined flag set this command sees (inherited + own).
    const combined = parent_flags ++ node.flags;

    // Duplicate long names.
    for (combined, 0..) |a, i| {
        for (combined[i + 1 ..]) |b| {
            if (flagLongNamesOverlap(a, b)) |name| {
                @compileError("cli.validate: duplicate flag long name or alias '" ++ name ++ "' in '" ++ node.name ++ "' (or inherited)");
            }
        }
    }

    // Duplicate short chars.
    for (combined, 0..) |a, i| {
        if (a.short == null) continue;
        for (combined[i + 1 ..]) |b| {
            if (b.short == null) continue;
            if (a.short.? == b.short.?) {
                @compileError("cli.validate: duplicate flag short char '-" ++ &[_]u8{a.short.?} ++ "' in '" ++ node.name ++ "' (or inherited)");
            }
        }
    }

    // Per-flag invariants.
    for (node.flags) |f| {
        validateLongFlagName(node.name, f.long);
        validateFlagAliases(node.name, f);
        validateDeprecation("flag", f.long, f.deprecated);
        validateCompletion("flag", f.long, f.completion);
        if (f.short) |short| validateShortFlagName(node.name, f.long, short);
        if (f.kind == .bool and f.value_name != null) {
            @compileError("cli.validate: flag '" ++ f.long ++ "' is bool and cannot define value_name");
        }
        if (f.value_name) |value_name| {
            if (value_name.len == 0) {
                @compileError("cli.validate: flag '" ++ f.long ++ "' has empty value_name");
            }
        }
        if (f.default) |d| {
            // Default tag must match Kind.
            const default_tag: flag.Kind = d;
            if (default_tag != f.kind) {
                @compileError("cli.validate: flag '" ++ f.long ++ "' has default of kind ." ++ @tagName(default_tag) ++ " but declared kind ." ++ @tagName(f.kind));
            }
            // Required + default is contradictory (default makes it not-required).
            if (f.required) {
                @compileError("cli.validate: flag '" ++ f.long ++ "' is required AND has a default; pick one");
            }
        }
    }

    for (node.positionals) |p| {
        validateFieldName(node.name, "positional", p.name);
        validateCompletion("positional", p.name, p.completion);
    }
    if (node.rest_field) |rest| validateRestFieldName(node.name, rest);
    validateGeneratedFieldNames(node, combined);

    // Manual documentation invariants.
    for (node.doc.examples, 0..) |example, i| {
        if (example.command.len == 0) {
            @compileError("cli.validate: command '" ++ node.name ++ "' has doc example with empty command");
        }
        if (example.title.len > 0) {
            for (node.doc.examples[i + 1 ..]) |other| {
                if (std.mem.eql(u8, example.title, other.title)) {
                    @compileError("cli.validate: command '" ++ node.name ++ "' has duplicate doc example title '" ++ example.title ++ "'");
                }
            }
        }
    }
    for (node.doc.exit_codes, 0..) |a, i| {
        if (a.desc.len == 0) {
            @compileError("cli.validate: command '" ++ node.name ++ "' has exit code " ++ std.fmt.comptimePrint("{d}", .{a.code}) ++ " with empty description");
        }
        for (node.doc.exit_codes[i + 1 ..]) |b| {
            if (a.code == b.code) {
                @compileError("cli.validate: command '" ++ node.name ++ "' has duplicate exit code " ++ std.fmt.comptimePrint("{d}", .{a.code}));
            }
        }
    }
    for (node.doc.notes) |note| {
        if (note.len == 0) {
            @compileError("cli.validate: command '" ++ node.name ++ "' has empty doc note");
        }
    }
    for (node.doc.see_also) |entry| {
        if (entry.len == 0) {
            @compileError("cli.validate: command '" ++ node.name ++ "' has empty see_also entry");
        }
    }
    validateStringList(node.name, "file", node.doc.files);
    validateStringList(node.name, "bug", node.doc.bugs);
    validateStringList(node.name, "author", node.doc.authors);

    // Recurse.
    for (node.cmds) |child| {
        validateNode(child, combined);
    }
}

fn validateStringList(
    comptime command_name: []const u8,
    comptime label: []const u8,
    comptime values: []const []const u8,
) void {
    for (values) |value| {
        if (value.len == 0) {
            @compileError("cli.validate: command '" ++ command_name ++ "' has empty doc " ++ label ++ " entry");
        }
    }
}

fn validateCommandName(comptime name: []const u8) void {
    if (!isCliToken(name)) {
        @compileError("cli.validate: invalid command name '" ++ name ++ "'; use alphanumeric characters and hyphens, starting with alphanumeric");
    }
}

fn validateCommandAliases(comptime node: cmd_mod.Cmd) void {
    for (node.aliases, 0..) |alias, i| {
        if (!isCliToken(alias)) {
            @compileError("cli.validate: invalid command alias '" ++ alias ++ "' for command '" ++ node.name ++ "'");
        }
        if (std.mem.eql(u8, node.name, alias)) {
            @compileError("cli.validate: command alias '" ++ alias ++ "' duplicates canonical command name '" ++ node.name ++ "'");
        }
        for (node.aliases[i + 1 ..]) |other| {
            if (std.mem.eql(u8, alias, other)) {
                @compileError("cli.validate: duplicate command alias '" ++ alias ++ "' for command '" ++ node.name ++ "'");
            }
        }
    }
}

fn validateLongFlagName(comptime command_name: []const u8, comptime long: []const u8) void {
    if (!std.mem.startsWith(u8, long, "--") or long.len <= 2 or !isCliToken(long[2..])) {
        @compileError("cli.validate: invalid long flag '" ++ long ++ "' in command '" ++ command_name ++ "'; use --name with alphanumeric characters and hyphens");
    }
}

fn validateFlagAliases(comptime command_name: []const u8, comptime f: flag.Flag) void {
    for (f.aliases, 0..) |alias, i| {
        validateLongFlagName(command_name, alias);
        if (std.mem.eql(u8, f.long, alias)) {
            @compileError("cli.validate: flag alias '" ++ alias ++ "' duplicates canonical flag name '" ++ f.long ++ "'");
        }
        for (f.aliases[i + 1 ..]) |other| {
            if (std.mem.eql(u8, alias, other)) {
                @compileError("cli.validate: duplicate flag alias '" ++ alias ++ "' for flag '" ++ f.long ++ "'");
            }
        }
    }
}

fn validateDeprecation(comptime kind: []const u8, comptime name: []const u8, comptime deprecated: anytype) void {
    if (deprecated) |d| {
        if (d.message.len == 0 and d.replacement == null) {
            @compileError("cli.validate: deprecated " ++ kind ++ " '" ++ name ++ "' must define a message or replacement");
        }
    }
}

fn validateCompletion(comptime kind: []const u8, comptime name: []const u8, comptime completion: anytype) void {
    switch (completion.kind) {
        .none => {
            if (completion.values.len != 0) {
                @compileError("cli.validate: " ++ kind ++ " '" ++ name ++ "' has completion values but kind .none");
            }
        },
        .values => {
            if (completion.values.len == 0) {
                @compileError("cli.validate: " ++ kind ++ " '" ++ name ++ "' has completion kind .values with no values");
            }
            for (completion.values, 0..) |value, i| {
                if (value.len == 0) {
                    @compileError("cli.validate: " ++ kind ++ " '" ++ name ++ "' has empty completion value");
                }
                for (completion.values[i + 1 ..]) |other| {
                    if (std.mem.eql(u8, value, other)) {
                        @compileError("cli.validate: " ++ kind ++ " '" ++ name ++ "' has duplicate completion value '" ++ value ++ "'");
                    }
                }
            }
        },
        .files, .directories => {
            if (completion.values.len != 0) {
                @compileError("cli.validate: " ++ kind ++ " '" ++ name ++ "' file completion cannot also define static values");
            }
        },
    }
}

fn validateShortFlagName(comptime command_name: []const u8, comptime long: []const u8, comptime short: u8) void {
    if (!isAsciiAlnum(short)) {
        @compileError("cli.validate: invalid short flag '-" ++ &[_]u8{short} ++ "' for '" ++ long ++ "' in command '" ++ command_name ++ "'; use one alphanumeric character");
    }
}

fn validateFieldName(
    comptime command_name: []const u8,
    comptime kind: []const u8,
    comptime name: []const u8,
) void {
    if (!isCliToken(name)) {
        @compileError("cli.validate: invalid " ++ kind ++ " name '" ++ name ++ "' in command '" ++ command_name ++ "'; use alphanumeric characters and hyphens, starting with alphanumeric");
    }
}

fn validateRestFieldName(comptime command_name: []const u8, comptime name: []const u8) void {
    if (!isArgsFieldToken(name)) {
        @compileError("cli.validate: invalid rest_field name '" ++ name ++ "' in command '" ++ command_name ++ "'; use a generated args field name with alphanumeric characters and underscores");
    }
}

fn validateGeneratedFieldNames(comptime node: cmd_mod.Cmd, comptime combined_flags: []const flag.Flag) void {
    for (combined_flags, 0..) |a, i| {
        const a_name = comptime flag.flagFieldName(a);
        for (combined_flags[i + 1 ..]) |b| {
            const b_name = comptime flag.flagFieldName(b);
            if (std.mem.eql(u8, a_name, b_name)) {
                fieldCollision(node.name, a_name);
            }
        }
        for (node.positionals) |p| {
            const p_name = comptime flag.positionalFieldName(p);
            if (std.mem.eql(u8, a_name, p_name)) {
                fieldCollision(node.name, a_name);
            }
        }
        if (node.rest_field) |rest| {
            if (std.mem.eql(u8, a_name, rest)) {
                fieldCollision(node.name, a_name);
            }
        }
    }

    for (node.positionals, 0..) |a, i| {
        const a_name = comptime flag.positionalFieldName(a);
        for (node.positionals[i + 1 ..]) |b| {
            const b_name = comptime flag.positionalFieldName(b);
            if (std.mem.eql(u8, a_name, b_name)) {
                fieldCollision(node.name, a_name);
            }
        }
        if (node.rest_field) |rest| {
            if (std.mem.eql(u8, a_name, rest)) {
                fieldCollision(node.name, a_name);
            }
        }
    }
}

fn fieldCollision(comptime command_name: []const u8, comptime field_name: []const u8) noreturn {
    @compileError("cli.validate: generated args field '" ++ field_name ++ "' collides in command '" ++ command_name ++ "'");
}

fn commandNamesOverlap(comptime a: cmd_mod.Cmd, comptime b: cmd_mod.Cmd) ?[]const u8 {
    if (std.mem.eql(u8, a.name, b.name)) return a.name;
    for (a.aliases) |name| {
        if (std.mem.eql(u8, name, b.name)) return name;
        for (b.aliases) |other| if (std.mem.eql(u8, name, other)) return name;
    }
    for (b.aliases) |name| {
        if (std.mem.eql(u8, name, a.name)) return name;
    }
    return null;
}

fn flagLongNamesOverlap(comptime a: flag.Flag, comptime b: flag.Flag) ?[]const u8 {
    if (std.mem.eql(u8, a.long, b.long)) return a.long;
    for (a.aliases) |name| {
        if (std.mem.eql(u8, name, b.long)) return name;
        for (b.aliases) |other| if (std.mem.eql(u8, name, other)) return name;
    }
    for (b.aliases) |name| {
        if (std.mem.eql(u8, name, a.long)) return name;
    }
    return null;
}

fn isCliToken(comptime s: []const u8) bool {
    if (s.len == 0) return false;
    if (!isAsciiAlnum(s[0])) return false;
    if (s[s.len - 1] == '-') return false;
    for (s[1..]) |c| {
        if (!isAsciiAlnum(c) and c != '-') return false;
    }
    return true;
}

fn isAsciiAlnum(comptime c: u8) bool {
    return (c >= 'a' and c <= 'z') or
        (c >= 'A' and c <= 'Z') or
        (c >= '0' and c <= '9');
}

fn isArgsFieldToken(comptime s: []const u8) bool {
    if (s.len == 0) return false;
    if (!isAsciiAlnum(s[0])) return false;
    for (s[1..]) |c| {
        if (!isAsciiAlnum(c) and c != '_') return false;
    }
    return true;
}

// ---- tests ----

test "validate accepts a well-formed tree" {
    const root = cmd_mod.Cmd{
        .name = "tool",
        .flags = &.{
            .{ .long = "--verbose", .short = 'v', .kind = .bool, .default = .{ .bool = false } },
        },
        .cmds = &.{
            .{
                .name = "add",
                .flags = &.{
                    .{ .long = "--title", .short = 't', .kind = .string, .required = true, .value_name = "TITLE" },
                },
                .positionals = &.{
                    .{ .name = "target-id" },
                },
                .rest_field = "tail",
                .doc = .{
                    .examples = &.{
                        .{ .title = "Add target", .command = "tool add --title demo 42" },
                    },
                    .exit_codes = &.{
                        .{ .code = 0, .desc = "Success." },
                    },
                    .notes = &.{"Notes are rendered in generated docs."},
                    .see_also = &.{"tool(1)"},
                },
            },
        },
    };
    comptime validate(root);
}

// Negative tests are awkward to write because @compileError aborts the
// build; we rely on the validator firing at the call site for any tree
// that violates invariants. The acceptance test above is a smoke check
// that valid trees compile cleanly.
