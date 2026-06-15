//! Comptime tree validator.
//!
//! `validate(root)` runs at compile time and `@compileError`s on:
//!   - duplicate long-flag names within a command (including inherited)
//!   - declared long-flag names or aliases that collide with implicit bool
//!     negation names within a command (including inherited)
//!   - duplicate short-flag chars within a command (including inherited)
//!   - duplicate sub-command names within a parent
//!   - invalid flag-group declarations such as duplicate group names, unknown
//!     members, duplicate members, partial visibility groups, and impossible
//!     required/exclusive combinations
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
        // Field-name derivation and the per-node checks loop over every flag
        // and positional in the tree; raise the branch quota so large command
        // trees validate without tripping the default 1000-branch limit.
        @setEvalBranchQuota(1_000_000);
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
                @compileError("validate: duplicate sub-command name or alias '" ++ name ++ "' under '" ++ node.name ++ "'");
            }
        }
    }

    // Build the combined flag set this command sees (inherited + own).
    const combined = parent_flags ++ node.flags;

    // Duplicate long names.
    for (combined, 0..) |a, i| {
        for (combined[i + 1 ..]) |b| {
            if (flagLongNamesOverlap(a, b)) |name| {
                @compileError("validate: duplicate flag long name or alias '" ++ name ++ "' in '" ++ node.name ++ "' (or inherited)");
            }
        }
    }
    validateBoolNegationCollisions(node.name, combined);

    // Duplicate short chars.
    for (combined, 0..) |a, i| {
        if (a.short == null) continue;
        for (combined[i + 1 ..]) |b| {
            if (b.short == null) continue;
            if (a.short.? == b.short.?) {
                @compileError("validate: duplicate flag short char '-" ++ &[_]u8{a.short.?} ++ "' in '" ++ node.name ++ "' (or inherited)");
            }
        }
    }

    // Per-flag invariants.
    for (node.flags) |f| {
        validateLongFlagName(node.name, f.long);
        validateFlagAliases(node.name, f);
        validateReservedFlagName(node.name, f);
        validateChoices(node.name, f);
        validateCount(node.name, f);
        validateList(node.name, f);
        validateDeprecation("flag", f.long, f.deprecated);
        validateCompletion("flag", f.long, f.completion);
        if (f.short) |short| validateShortFlagName(node.name, f.long, short);
        if (f.kind == .bool and f.value_name != null) {
            @compileError("validate: flag '" ++ f.long ++ "' is bool and cannot define value_name");
        }
        if (f.value_name) |value_name| {
            if (value_name.len == 0) {
                @compileError("validate: flag '" ++ f.long ++ "' has empty value_name");
            }
        }
        if (f.default) |d| {
            // Default tag must match Kind.
            const default_tag: flag.Kind = d;
            if (default_tag != f.kind) {
                @compileError("validate: flag '" ++ f.long ++ "' has default of kind ." ++ @tagName(default_tag) ++ " but declared kind ." ++ @tagName(f.kind));
            }
            // Required + default is contradictory (default makes it not-required).
            if (f.required) {
                @compileError("validate: flag '" ++ f.long ++ "' is required AND has a default; pick one");
            }
        }
    }

    var seen_optional_positional = false;
    for (node.positionals) |p| {
        validateFieldName(node.name, "positional", p.name);
        validateCompletion("positional", p.name, p.completion);
        if (p.kind == .choice) {
            @compileError("validate: positional '" ++ p.name ++ "' in command '" ++ node.name ++ "' cannot use kind .choice; choice is supported on flags only");
        }
        if (p.default) |d| {
            const default_tag: flag.Kind = d;
            if (default_tag != p.kind) {
                @compileError("validate: positional '" ++ p.name ++ "' in command '" ++ node.name ++ "' has default of kind ." ++ @tagName(default_tag) ++ " but declared kind ." ++ @tagName(p.kind));
            }
            if (p.required) {
                @compileError("validate: positional '" ++ p.name ++ "' in command '" ++ node.name ++ "' is required AND has a default; pick one");
            }
        }
        // A required positional may not follow an optional one: a single
        // supplied argument fills the earlier (optional) slot, leaving the
        // later required slot impossible to satisfy positionally.
        if (!p.required) {
            seen_optional_positional = true;
        } else if (seen_optional_positional) {
            @compileError("validate: required positional '" ++ p.name ++ "' follows an optional positional in command '" ++ node.name ++ "'; required positionals must be declared first");
        }
    }
    if (node.rest_field) |rest| validateRestFieldName(node.name, rest);
    validateGeneratedFieldNames(node, combined);
    validateFlagGroups(node.name, node.flag_groups, combined);

    // Manual documentation invariants.
    for (node.doc.examples, 0..) |example, i| {
        if (example.command.len == 0) {
            @compileError("validate: command '" ++ node.name ++ "' has doc example with empty command");
        }
        if (example.title.len > 0) {
            for (node.doc.examples[i + 1 ..]) |other| {
                if (std.mem.eql(u8, example.title, other.title)) {
                    @compileError("validate: command '" ++ node.name ++ "' has duplicate doc example title '" ++ example.title ++ "'");
                }
            }
        }
    }
    for (node.doc.exit_codes, 0..) |a, i| {
        if (a.desc.len == 0) {
            @compileError("validate: command '" ++ node.name ++ "' has exit code " ++ std.fmt.comptimePrint("{d}", .{a.code}) ++ " with empty description");
        }
        for (node.doc.exit_codes[i + 1 ..]) |b| {
            if (a.code == b.code) {
                @compileError("validate: command '" ++ node.name ++ "' has duplicate exit code " ++ std.fmt.comptimePrint("{d}", .{a.code}));
            }
        }
    }
    for (node.doc.notes) |note| {
        if (note.len == 0) {
            @compileError("validate: command '" ++ node.name ++ "' has empty doc note");
        }
    }
    for (node.doc.see_also) |entry| {
        if (entry.len == 0) {
            @compileError("validate: command '" ++ node.name ++ "' has empty see_also entry");
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
            @compileError("validate: command '" ++ command_name ++ "' has empty doc " ++ label ++ " entry");
        }
    }
}

fn validateCommandName(comptime name: []const u8) void {
    if (!isCliToken(name)) {
        @compileError("validate: invalid command name '" ++ name ++ "'; use alphanumeric characters and hyphens, starting with alphanumeric");
    }
}

fn validateCommandAliases(comptime node: cmd_mod.Cmd) void {
    for (node.aliases, 0..) |alias, i| {
        if (!isCliToken(alias)) {
            @compileError("validate: invalid command alias '" ++ alias ++ "' for command '" ++ node.name ++ "'");
        }
        if (std.mem.eql(u8, node.name, alias)) {
            @compileError("validate: command alias '" ++ alias ++ "' duplicates canonical command name '" ++ node.name ++ "'");
        }
        for (node.aliases[i + 1 ..]) |other| {
            if (std.mem.eql(u8, alias, other)) {
                @compileError("validate: duplicate command alias '" ++ alias ++ "' for command '" ++ node.name ++ "'");
            }
        }
    }
}

fn validateLongFlagName(comptime command_name: []const u8, comptime long: []const u8) void {
    if (!std.mem.startsWith(u8, long, "--") or long.len <= 2 or !isCliToken(long[2..])) {
        @compileError("validate: invalid long flag '" ++ long ++ "' in command '" ++ command_name ++ "'; use --name with alphanumeric characters and hyphens");
    }
}

fn validateFlagAliases(comptime command_name: []const u8, comptime f: flag.Flag) void {
    for (f.aliases, 0..) |alias, i| {
        validateLongFlagName(command_name, alias);
        if (std.mem.eql(u8, f.long, alias)) {
            @compileError("validate: flag alias '" ++ alias ++ "' duplicates canonical flag name '" ++ f.long ++ "'");
        }
        for (f.aliases[i + 1 ..]) |other| {
            if (std.mem.eql(u8, alias, other)) {
                @compileError("validate: duplicate flag alias '" ++ alias ++ "' for flag '" ++ f.long ++ "'");
            }
        }
    }
}

fn validateDeprecation(comptime kind: []const u8, comptime name: []const u8, comptime deprecated: anytype) void {
    if (deprecated) |d| {
        if (d.message.len == 0 and d.replacement == null) {
            @compileError("validate: deprecated " ++ kind ++ " '" ++ name ++ "' must define a message or replacement");
        }
    }
}

fn validateCompletion(comptime kind: []const u8, comptime name: []const u8, comptime completion: anytype) void {
    switch (completion.kind) {
        .none => {
            if (completion.values.len != 0) {
                @compileError("validate: " ++ kind ++ " '" ++ name ++ "' has completion values but kind .none");
            }
        },
        .values => {
            if (completion.values.len == 0) {
                @compileError("validate: " ++ kind ++ " '" ++ name ++ "' has completion kind .values with no values");
            }
            for (completion.values, 0..) |value, i| {
                if (value.len == 0) {
                    @compileError("validate: " ++ kind ++ " '" ++ name ++ "' has empty completion value");
                }
                // Static completion values are emitted into generated bash
                // (`compgen -W`) and zsh (`_values`) scripts. Reject shell
                // metacharacters and whitespace so a value cannot inject
                // command substitution or break the generated word list.
                // Dynamic/complex values belong in application-owned
                // completion commands (see README).
                for (value) |c| {
                    if (!isSafeCompletionChar(c)) {
                        @compileError("validate: " ++ kind ++ " '" ++ name ++ "' has completion value '" ++ value ++ "' with an unsafe character; static values must be shell-safe (alphanumerics and - _ . / @ % + , =)");
                    }
                }
                for (completion.values[i + 1 ..]) |other| {
                    if (std.mem.eql(u8, value, other)) {
                        @compileError("validate: " ++ kind ++ " '" ++ name ++ "' has duplicate completion value '" ++ value ++ "'");
                    }
                }
            }
        },
        .files, .directories => {
            if (completion.values.len != 0) {
                @compileError("validate: " ++ kind ++ " '" ++ name ++ "' file completion cannot also define static values");
            }
        },
        .dynamic => {
            if (completion.callback == null) {
                @compileError("validate: " ++ kind ++ " '" ++ name ++ "' has completion kind .dynamic but no callback; use cli.Completion.dynamic(fn)");
            }
            if (completion.values.len != 0) {
                @compileError("validate: " ++ kind ++ " '" ++ name ++ "' dynamic completion cannot also define static values");
            }
        },
    }
    if (completion.kind != .dynamic and completion.callback != null) {
        @compileError("validate: " ++ kind ++ " '" ++ name ++ "' sets a completion callback but kind is not .dynamic");
    }
}

fn validateReservedFlagName(comptime command_name: []const u8, comptime f: flag.Flag) void {
    // `--help` and `-h` are intercepted by the parser before flag matching, so
    // a user-declared flag with those names would compile, render in
    // help/completion/schema, yet never parse. Reject them at comptime.
    if (std.mem.eql(u8, f.long, "--help")) {
        @compileError("validate: flag long name '--help' in command '" ++ command_name ++ "' is reserved by the parser; remove or rename it");
    }
    for (f.aliases) |alias| {
        if (std.mem.eql(u8, alias, "--help")) {
            @compileError("validate: flag alias '--help' on flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' is reserved by the parser; remove or rename it");
        }
    }
    if (f.short) |short| {
        if (short == 'h') {
            @compileError("validate: short flag '-h' on flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' is reserved by the parser; remove or rename it");
        }
    }
}

fn validateChoices(comptime command_name: []const u8, comptime f: flag.Flag) void {
    if (f.kind == .choice) {
        if (f.choices.len == 0) {
            @compileError("validate: choice flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' must declare at least one entry in `choices`");
        }
        if (f.value_name != null) {
            @compileError("validate: choice flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' cannot define value_name; the choice list is the placeholder");
        }
        for (f.choices, 0..) |choice, i| {
            if (choice.len == 0) {
                @compileError("validate: choice flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' has an empty choice");
            }
            for (choice) |c| {
                if (!isSafeCompletionChar(c)) {
                    @compileError("validate: choice flag '" ++ f.long ++ "' choice '" ++ choice ++ "' has an unsafe character; choices must be shell-safe (alphanumerics and - _ . / @ % + , =)");
                }
            }
            for (f.choices[i + 1 ..]) |other| {
                if (std.mem.eql(u8, choice, other)) {
                    @compileError("validate: choice flag '" ++ f.long ++ "' has duplicate choice '" ++ choice ++ "'");
                }
            }
        }
        // A choice default must be one of the declared choices. Guard on the
        // tag so a mismatched-kind default produces the dedicated error below
        // rather than a raw union-access failure here.
        if (f.default) |d| {
            const default_tag: flag.Kind = d;
            if (default_tag == .choice) {
                var found = false;
                for (f.choices) |choice| {
                    if (std.mem.eql(u8, choice, d.choice)) found = true;
                }
                if (!found) {
                    @compileError("validate: choice flag '" ++ f.long ++ "' default '" ++ d.choice ++ "' is not one of its choices");
                }
            }
        }
    } else if (f.choices.len != 0) {
        @compileError("validate: flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' declares `choices` but kind is not .choice");
    }
}

fn validateList(comptime command_name: []const u8, comptime f: flag.Flag) void {
    if (!f.list) return;
    switch (f.kind) {
        .string, .path, .choice => {},
        else => @compileError("validate: list flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' must be kind .string, .path, or .choice"),
    }
    if (f.default != null) {
        @compileError("validate: list flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' cannot define a default; the empty list is the default");
    }
}

fn validateCount(comptime command_name: []const u8, comptime f: flag.Flag) void {
    if (!f.count) return;
    if (f.kind != .bool) {
        @compileError("validate: count flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' must be kind .bool; it takes no value");
    }
    if (f.list) {
        @compileError("validate: flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' cannot be both count and list");
    }
    if (f.required) {
        @compileError("validate: count flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' cannot be required; it defaults to 0");
    }
    if (f.default != null) {
        @compileError("validate: count flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' cannot define a default; 0 is the default");
    }
    if (f.value_name != null) {
        @compileError("validate: count flag '" ++ f.long ++ "' in command '" ++ command_name ++ "' cannot define value_name; it takes no value");
    }
}

fn validateShortFlagName(comptime command_name: []const u8, comptime long: []const u8, comptime short: u8) void {
    if (!isAsciiAlnum(short)) {
        @compileError("validate: invalid short flag '-" ++ &[_]u8{short} ++ "' for '" ++ long ++ "' in command '" ++ command_name ++ "'; use one alphanumeric character");
    }
}

fn validateFieldName(
    comptime command_name: []const u8,
    comptime kind: []const u8,
    comptime name: []const u8,
) void {
    if (!isCliToken(name)) {
        @compileError("validate: invalid " ++ kind ++ " name '" ++ name ++ "' in command '" ++ command_name ++ "'; use alphanumeric characters and hyphens, starting with alphanumeric");
    }
}

fn validateRestFieldName(comptime command_name: []const u8, comptime name: []const u8) void {
    if (!isArgsFieldToken(name)) {
        @compileError("validate: invalid rest_field name '" ++ name ++ "' in command '" ++ command_name ++ "'; use a generated args field name with alphanumeric characters and underscores");
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

fn validateFlagGroups(
    comptime command_name: []const u8,
    comptime groups: []const flag.FlagGroup,
    comptime visible_flags: []const flag.Flag,
) void {
    for (groups, 0..) |group, group_index| {
        validateFieldName(command_name, "flag group", group.name);
        switch (group.mode) {
            .mutually_exclusive, .required_one, .required_exactly_one => {},
        }

        for (groups[group_index + 1 ..]) |other| {
            if (std.mem.eql(u8, group.name, other.name)) {
                @compileError("validate: command '" ++ command_name ++ "' has duplicate flag group name '" ++ group.name ++ "'");
            }
        }

        if (group.flags.len == 0) {
            @compileError("validate: flag group '" ++ group.name ++ "' in command '" ++ command_name ++ "' must declare at least one flag");
        }

        var first_hidden: ?[]const u8 = null;
        var first_visible: ?[]const u8 = null;
        var first_deprecated: ?[]const u8 = null;
        var first_current: ?[]const u8 = null;
        var first_required: ?[]const u8 = null;
        var second_required: ?[]const u8 = null;

        for (group.flags, 0..) |member, member_index| {
            const resolved = comptime resolveGroupMember(command_name, group.name, member, visible_flags);
            if (!std.mem.eql(u8, member, resolved.long)) {
                @compileError("validate: flag group '" ++ group.name ++ "' in command '" ++ command_name ++ "' references '" ++ member ++ "'; use canonical long flag '" ++ resolved.long ++ "'");
            }

            for (group.flags[member_index + 1 ..]) |other_member| {
                const other = comptime resolveGroupMember(command_name, group.name, other_member, visible_flags);
                if (std.mem.eql(u8, resolved.long, other.long)) {
                    @compileError("validate: flag group '" ++ group.name ++ "' in command '" ++ command_name ++ "' has duplicate flag member '" ++ resolved.long ++ "'");
                }
            }

            if (resolved.hidden) {
                if (first_visible) |visible| {
                    @compileError("validate: flag group '" ++ group.name ++ "' in command '" ++ command_name ++ "' mixes hidden flag '" ++ resolved.long ++ "' with visible flag '" ++ visible ++ "'");
                }
                if (first_hidden == null) first_hidden = resolved.long;
            } else {
                if (first_hidden) |hidden| {
                    @compileError("validate: flag group '" ++ group.name ++ "' in command '" ++ command_name ++ "' mixes visible flag '" ++ resolved.long ++ "' with hidden flag '" ++ hidden ++ "'");
                }
                if (first_visible == null) first_visible = resolved.long;
            }

            if (resolved.deprecated != null) {
                if (first_current) |current| {
                    @compileError("validate: flag group '" ++ group.name ++ "' in command '" ++ command_name ++ "' mixes deprecated flag '" ++ resolved.long ++ "' with current flag '" ++ current ++ "'");
                }
                if (first_deprecated == null) first_deprecated = resolved.long;
            } else {
                if (first_deprecated) |deprecated| {
                    @compileError("validate: flag group '" ++ group.name ++ "' in command '" ++ command_name ++ "' mixes current flag '" ++ resolved.long ++ "' with deprecated flag '" ++ deprecated ++ "'");
                }
                if (first_current == null) first_current = resolved.long;
            }

            if (resolved.required) {
                if (first_required == null) {
                    first_required = resolved.long;
                } else if (second_required == null) {
                    second_required = resolved.long;
                }
            }
        }

        if (second_required) |second| {
            const first = first_required.?;
            switch (group.mode) {
                .mutually_exclusive => {
                    @compileError("validate: flag group '" ++ group.name ++ "' in command '" ++ command_name ++ "' has incompatible mode .mutually_exclusive because required members '" ++ first ++ "' and '" ++ second ++ "' cannot both be satisfied");
                },
                .required_exactly_one => {
                    @compileError("validate: flag group '" ++ group.name ++ "' in command '" ++ command_name ++ "' has incompatible mode .required_exactly_one because required members '" ++ first ++ "' and '" ++ second ++ "' cannot both be satisfied");
                },
                .required_one => {},
            }
        }
    }
}

fn resolveGroupMember(
    comptime command_name: []const u8,
    comptime group_name: []const u8,
    comptime member: []const u8,
    comptime visible_flags: []const flag.Flag,
) flag.Flag {
    for (visible_flags) |f| {
        if (std.mem.eql(u8, member, f.long)) return f;
        for (f.aliases) |alias| {
            if (std.mem.eql(u8, member, alias)) return f;
        }
        if (f.short) |short| {
            if (member.len == 2 and member[0] == '-' and member[1] == short) return f;
        }
    }

    @compileError("validate: flag group '" ++ group_name ++ "' in command '" ++ command_name ++ "' references unknown flag '" ++ member ++ "'; members must name visible flags at that command path");
}

fn fieldCollision(comptime command_name: []const u8, comptime field_name: []const u8) noreturn {
    @compileError("validate: generated args field '" ++ field_name ++ "' collides in command '" ++ command_name ++ "'");
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

fn validateBoolNegationCollisions(comptime command_name: []const u8, comptime flags: []const flag.Flag) void {
    for (flags) |f| {
        if (f.kind != .bool) continue;
        validateBoolNegationName(command_name, flags, f.long);
        for (f.aliases) |alias| {
            validateBoolNegationName(command_name, flags, alias);
        }
    }
}

fn validateBoolNegationName(
    comptime command_name: []const u8,
    comptime flags: []const flag.Flag,
    comptime source_name: []const u8,
) void {
    if (!std.mem.startsWith(u8, source_name, "--")) return;

    const negated = "--no-" ++ source_name[2..];
    for (flags) |candidate| {
        if (std.mem.eql(u8, candidate.long, negated)) {
            boolNegationCollision(command_name, negated);
        }
        for (candidate.aliases) |alias| {
            if (std.mem.eql(u8, alias, negated)) {
                boolNegationCollision(command_name, negated);
            }
        }
    }
}

fn boolNegationCollision(comptime command_name: []const u8, comptime name: []const u8) noreturn {
    @compileError("validate: flag long name or alias '" ++ name ++ "' in '" ++ command_name ++ "' (or inherited) collides with implicit bool negation");
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

fn isSafeCompletionChar(comptime c: u8) bool {
    if (isAsciiAlnum(c)) return true;
    return switch (c) {
        '-', '_', '.', '/', '@', '%', '+', ',', '=' => true,
        else => false,
    };
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

test "validate accepts leaf flag groups over inherited and local flags" {
    const root = cmd_mod.Cmd{
        .name = "tool",
        .flags = &.{
            .{ .long = "--verbose", .kind = .bool },
        },
        .cmds = &.{
            .{
                .name = "run",
                .flags = &.{
                    .{ .long = "--json", .kind = .bool },
                    .{ .long = "--yaml", .kind = .bool },
                },
                .flag_groups = &.{
                    .{
                        .name = "output",
                        .mode = .required_exactly_one,
                        .flags = &.{ "--json", "--yaml" },
                    },
                    .{
                        .name = "selection",
                        .mode = .required_one,
                        .flags = &.{ "--verbose", "--json" },
                    },
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
