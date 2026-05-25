//! Comptime shell-completion script generator.
//!
//! Mirrors the help.zig pattern: walk the `Cmd` tree at compile time and
//! emit a complete shell script as a `[]const u8` that lives in `.rodata`.
//! No runtime tree traversal — the generated script is a pure constant
//! you can `writer.writeAll` to stdout from a `completion <shell>`
//! handler.
//!
//! All three shells use the same model: take the current command-line
//! tokens, strip flags, join the remainder into a "path" string, and
//! switch on that path to decide which subcommands and flags to suggest.
//! Bash drives this with a function + `compgen`; zsh with the same shape
//! plus `_describe` (so descriptions appear in completion menus); fish
//! emits one flat `complete` line per option gated by a path-match helper.
//!
//! Limitations (intentional, v1):
//!   - Flag *values* are not completed (only flag names).
//!   - Positional arguments are not suggested.
//!   - Descriptions are emitted verbatim; if a desc contains a colon (zsh)
//!     or single-quote (fish), the script will misbehave. Avoid those in
//!     descs for now, or extend the escape helpers below.
const std = @import("std");
const cmd_mod = @import("cmd.zig");
const flag_mod = @import("flag.zig");

pub const Shell = enum { bash, zsh, fish };

/// Generate the completion script for `root` targeted at `shell`. The
/// returned slice is comptime-allocated and lives in `.rodata`.
pub fn script(comptime root: cmd_mod.Cmd, comptime shell: Shell) []const u8 {
    @setEvalBranchQuota(20_000_000);
    return comptime switch (shell) {
        .bash => bashScript(root),
        .zsh => zshScript(root),
        .fish => fishScript(root),
    };
}

// =========================================================================
// Bash
// =========================================================================

fn bashScript(comptime root: cmd_mod.Cmd) []const u8 {
    comptime {
        var out: []const u8 = "";
        out = out ++ "# " ++ root.name ++ " bash completion (auto-generated)\n";
        out = out ++ "# Source this file or place it in a directory loaded by bash-completion.\n\n";
        out = out ++ "_" ++ root.name ++ "() {\n";
        out = out ++
            \\    local cur path cmds flags i
            \\    cur="${COMP_WORDS[COMP_CWORD]}"
            \\    path=""
            \\    for (( i=1; i<COMP_CWORD; i++ )); do
            \\        case "${COMP_WORDS[i]}" in
            \\            -*) ;;
            \\            *)
            \\                if [[ -z "$path" ]]; then
            \\                    path="${COMP_WORDS[i]}"
            \\                else
            \\                    path="$path ${COMP_WORDS[i]}"
            \\                fi
            \\                ;;
            \\        esac
            \\    done
            \\
            \\    case "$path" in
            \\
        ;

        // Root case: empty path → suggest root's subcommands + flags.
        out = out ++ "        \"\")\n";
        out = out ++ "            cmds=\"" ++ joinCmdNames(root.cmds) ++ "\"\n";
        out = out ++ "            flags=\"" ++ joinFlagNames(root.flags) ++ "\"\n";
        out = out ++ "            ;;\n";

        // One case per non-root node.
        const nodes = cmd_mod.allNodes(root);
        for (nodes) |n| {
            out = out ++ "        \"" ++ joinPath(n.path) ++ "\")\n";
            out = out ++ "            cmds=\"" ++ joinCmdNames(n.cmd.cmds) ++ "\"\n";
            const inherited = cmd_mod.collectInheritedFlags(root, n.path);
            out = out ++ "            flags=\"" ++ joinFlagPair(inherited, n.cmd.flags) ++ "\"\n";
            out = out ++ "            ;;\n";
        }

        out = out ++
            \\        *)
            \\            cmds=""
            \\            flags=""
            \\            ;;
            \\    esac
            \\
            \\    if [[ "$cur" == -* ]]; then
            \\        COMPREPLY=( $(compgen -W "$flags" -- "$cur") )
            \\    else
            \\        COMPREPLY=( $(compgen -W "$cmds" -- "$cur") )
            \\    fi
            \\}
            \\
            \\
        ;
        out = out ++ "complete -F _" ++ root.name ++ " " ++ root.name ++ "\n";
        return out;
    }
}

// =========================================================================
// Zsh
// =========================================================================

fn zshScript(comptime root: cmd_mod.Cmd) []const u8 {
    comptime {
        var out: []const u8 = "";
        out = out ++ "#compdef " ++ root.name ++ "\n";
        out = out ++ "# " ++ root.name ++ " zsh completion (auto-generated)\n\n";
        out = out ++ "_" ++ root.name ++ "() {\n";
        out = out ++
            \\    local cur path i
            \\    cur="${words[CURRENT]}"
            \\    path=""
            \\    for (( i=2; i<CURRENT; i++ )); do
            \\        case "${words[i]}" in
            \\            -*) ;;
            \\            *)
            \\                if [[ -z "$path" ]]; then
            \\                    path="${words[i]}"
            \\                else
            \\                    path="$path ${words[i]}"
            \\                fi
            \\                ;;
            \\        esac
            \\    done
            \\
            \\    local -a cmds flags
            \\    case "$path" in
            \\
        ;

        // Root case.
        out = out ++ "        \"\")\n";
        out = out ++ "            cmds=(" ++ zshCmdPairs(root.cmds) ++ ")\n";
        out = out ++ "            flags=(" ++ zshFlagPairs(root.flags) ++ ")\n";
        out = out ++ "            ;;\n";

        const nodes = cmd_mod.allNodes(root);
        for (nodes) |n| {
            out = out ++ "        \"" ++ joinPath(n.path) ++ "\")\n";
            out = out ++ "            cmds=(" ++ zshCmdPairs(n.cmd.cmds) ++ ")\n";
            const inherited = cmd_mod.collectInheritedFlags(root, n.path);
            out = out ++ "            flags=(" ++ zshFlagPairsPair(inherited, n.cmd.flags) ++ ")\n";
            out = out ++ "            ;;\n";
        }

        out = out ++
            \\    esac
            \\
            \\    if [[ "$cur" == -* ]]; then
            \\        _describe -t flags 'flags' flags
            \\    else
            \\        _describe -t commands 'commands' cmds
            \\    fi
            \\}
            \\
            \\
        ;
        out = out ++ "_" ++ root.name ++ " \"$@\"\n";
        return out;
    }
}

// =========================================================================
// Fish
// =========================================================================

fn fishScript(comptime root: cmd_mod.Cmd) []const u8 {
    comptime {
        var out: []const u8 = "";
        out = out ++ "# " ++ root.name ++ " fish completion (auto-generated)\n\n";

        // Helper: __fish_<bin>_path "<expected path>" → returns 0 if the
        // current non-flag tokens after the bin name join to <expected path>.
        out = out ++ "function __fish_" ++ root.name ++ "_path\n";
        out = out ++
            \\    set -l cmd (commandline -opc)
            \\    set -l path
            \\    set -l first 1
            \\    for word in $cmd[2..]
            \\        if string match -q -- '-*' $word
            \\            continue
            \\        end
            \\        if test $first -eq 1
            \\            set path $word
            \\            set first 0
            \\        else
            \\            set path "$path $word"
            \\        end
            \\    end
            \\    test "$path" = "$argv[1]"
            \\end
            \\
            \\
        ;

        // Root commands and flags.
        for (root.cmds) |c| {
            out = out ++ fishCmdLine(root.name, "", c);
        }
        for (root.flags) |f| {
            out = out ++ fishFlagLine(root.name, "", f);
        }

        // Walk every other node.
        const nodes = cmd_mod.allNodes(root);
        for (nodes) |n| {
            const path_str = joinPath(n.path);
            for (n.cmd.cmds) |c| {
                out = out ++ fishCmdLine(root.name, path_str, c);
            }
            // Owned flags only — fish lets the user repeat globals freely;
            // listing inherited flags at every depth would just duplicate.
            for (n.cmd.flags) |f| {
                out = out ++ fishFlagLine(root.name, path_str, f);
            }
        }

        return out;
    }
}

// =========================================================================
// Small comptime helpers
// =========================================================================

fn joinPath(comptime path: []const []const u8) []const u8 {
    comptime {
        if (path.len == 0) return "";
        var out: []const u8 = path[0];
        for (path[1..]) |seg| out = out ++ " " ++ seg;
        return out;
    }
}

fn joinCmdNames(comptime cmds: []const cmd_mod.Cmd) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (cmds, 0..) |c, i| {
            if (i > 0) out = out ++ " ";
            out = out ++ c.name;
        }
        return out;
    }
}

fn joinFlagNames(comptime flags: []const flag_mod.Flag) []const u8 {
    comptime {
        var out: []const u8 = "";
        var first = true;
        for (flags) |f| {
            if (!first) out = out ++ " ";
            first = false;
            out = out ++ f.long;
            if (f.short) |s| out = out ++ " -" ++ &[_]u8{s};
        }
        if (!first) out = out ++ " ";
        out = out ++ "--help -h";
        return out;
    }
}

fn joinFlagPair(
    comptime inherited: []const flag_mod.Flag,
    comptime owned: []const flag_mod.Flag,
) []const u8 {
    comptime {
        const out: []const u8 = joinFlagNames(inherited);
        if (owned.len > 0) {
            // joinFlagNames already appends --help -h; insert owned BEFORE that.
            // Simplest: rebuild from a merged slice.
            const merged = inherited ++ owned;
            return joinFlagNames(merged);
        }
        return out;
    }
}

fn zshCmdPairs(comptime cmds: []const cmd_mod.Cmd) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (cmds, 0..) |c, i| {
            if (i > 0) out = out ++ " ";
            out = out ++ "\"" ++ c.name ++ ":" ++ c.desc ++ "\"";
        }
        return out;
    }
}

fn zshFlagPairs(comptime flags: []const flag_mod.Flag) []const u8 {
    comptime {
        var out: []const u8 = "";
        var first = true;
        for (flags) |f| {
            if (!first) out = out ++ " ";
            first = false;
            out = out ++ "\"" ++ f.long ++ ":" ++ f.desc ++ "\"";
            if (f.short) |s| {
                out = out ++ " \"-" ++ &[_]u8{s} ++ ":" ++ f.desc ++ "\"";
            }
        }
        if (!first) out = out ++ " ";
        out = out ++ "\"--help:Show help\" \"-h:Show help\"";
        return out;
    }
}

fn zshFlagPairsPair(
    comptime inherited: []const flag_mod.Flag,
    comptime owned: []const flag_mod.Flag,
) []const u8 {
    comptime {
        const merged = inherited ++ owned;
        return zshFlagPairs(merged);
    }
}

fn fishCmdLine(
    comptime bin: []const u8,
    comptime path: []const u8,
    comptime c: cmd_mod.Cmd,
) []const u8 {
    comptime {
        var out: []const u8 = "complete -c " ++ bin ++
            " -n '__fish_" ++ bin ++ "_path \"" ++ path ++ "\"'" ++
            " -f -a '" ++ c.name ++ "'";
        if (c.desc.len > 0) out = out ++ " -d '" ++ c.desc ++ "'";
        out = out ++ "\n";
        return out;
    }
}

fn fishFlagLine(
    comptime bin: []const u8,
    comptime path: []const u8,
    comptime f: flag_mod.Flag,
) []const u8 {
    comptime {
        // Strip leading "--" from long name; fish wants the bare name with -l.
        const long_bare = if (f.long.len > 2 and f.long[0] == '-' and f.long[1] == '-')
            f.long[2..]
        else if (f.long.len > 1 and f.long[0] == '-')
            f.long[1..]
        else
            f.long;
        var out: []const u8 = "complete -c " ++ bin ++
            " -n '__fish_" ++ bin ++ "_path \"" ++ path ++ "\"'" ++
            " -l '" ++ long_bare ++ "'";
        if (f.short) |s| out = out ++ " -s " ++ &[_]u8{s};
        if (f.desc.len > 0) out = out ++ " -d '" ++ f.desc ++ "'";
        out = out ++ "\n";
        return out;
    }
}

// =========================================================================
// Tests
// =========================================================================

const test_root = cmd_mod.Cmd{
    .name = "tool",
    .desc = "Test tool",
    .flags = &.{
        .{ .long = "--verbose", .short = 'v', .desc = "Verbose output", .kind = .bool, .default = .{ .bool = false } },
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
                },
                .{
                    .name = "list",
                    .desc = "List tasks",
                },
            },
        },
    },
};

test "bash script includes top-level commands and inherited flags" {
    const s = comptime script(test_root, .bash);
    try std.testing.expect(std.mem.indexOf(u8, s, "_tool()") != null);
    try std.testing.expect(std.mem.indexOf(u8, s, "complete -F _tool tool") != null);
    // Root case suggests "task" and "--verbose".
    try std.testing.expect(std.mem.indexOf(u8, s, "cmds=\"task\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, s, "--verbose") != null);
    // "task add" case suggests --title and inherits --verbose.
    try std.testing.expect(std.mem.indexOf(u8, s, "\"task add\")") != null);
    try std.testing.expect(std.mem.indexOf(u8, s, "--title") != null);
}

test "zsh script uses compdef and _describe" {
    const s = comptime script(test_root, .zsh);
    try std.testing.expect(std.mem.indexOf(u8, s, "#compdef tool") != null);
    try std.testing.expect(std.mem.indexOf(u8, s, "_describe -t commands") != null);
    // Description from the Cmd appears next to its name.
    try std.testing.expect(std.mem.indexOf(u8, s, "\"task:Manage tasks\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, s, "\"add:Add a task\"") != null);
}

test "fish script emits per-path complete lines with descriptions" {
    const s = comptime script(test_root, .fish);
    try std.testing.expect(std.mem.indexOf(u8, s, "function __fish_tool_path") != null);
    // Root command suggestion.
    try std.testing.expect(std.mem.indexOf(u8, s, "__fish_tool_path \"\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, s, "-a 'task' -d 'Manage tasks'") != null);
    // Nested suggestion gated on the parent path.
    try std.testing.expect(std.mem.indexOf(u8, s, "__fish_tool_path \"task\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, s, "-a 'add' -d 'Add a task'") != null);
    // Flag with short form gets both -l and -s.
    try std.testing.expect(std.mem.indexOf(u8, s, "-l 'verbose' -s v") != null);
}
