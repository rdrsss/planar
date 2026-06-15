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
//!   - Flag *values* are completed from static `completion` metadata or
//!     runtime dynamic callbacks; command and flag names are generated
//!     statically from the command tree.
//!   - Descriptions are escaped per shell (see `zshEscapeDesc` /
//!     `fishSingleQuote`); static completion values are validated to be
//!     shell-safe at comptime in `validate.zig`.
const std = @import("std");
const cmd_mod = @import("cmd.zig");
const flag_mod = @import("flag.zig");

pub const Shell = enum { bash, zsh, fish };
pub const Options = struct {
    include_hidden: bool = false,
    include_deprecated: bool = true,
};

/// Generate the completion script for `root` targeted at `shell`. The
/// returned slice is comptime-allocated and lives in `.rodata`.
pub fn script(comptime root: cmd_mod.Cmd, comptime shell: Shell) []const u8 {
    return scriptWithOptions(root, shell, .{});
}

pub fn scriptWithOptions(comptime root: cmd_mod.Cmd, comptime shell: Shell, comptime options: Options) []const u8 {
    @setEvalBranchQuota(20_000_000);
    return comptime switch (shell) {
        .bash => bashScript(root, options),
        .zsh => zshScript(root, options),
        .fish => fishScript(root, options),
    };
}

// =========================================================================
// Runtime dynamic completion
// =========================================================================

/// Runtime completion entrypoint, reached via the `__complete` builtin that
/// generated scripts invoke for dynamic flags. `words` are the tokens after
/// `__complete`: `[flag_long, prefix?]`. Finds the matching dynamic flag,
/// invokes its callback with the prefix, and prints candidates one per line.
pub fn complete(
    comptime root: cmd_mod.Cmd,
    words: []const []const u8,
    writer: *std.Io.Writer,
) std.Io.Writer.Error!void {
    if (words.len == 0) return;
    const flag_long = words[0];
    const prefix = if (words.len >= 2) words[1] else "";
    inline for (comptime allTreeFlags(root)) |f| {
        if (comptime f.completion.kind == .dynamic) {
            if (std.mem.eql(u8, f.long, flag_long)) {
                for (f.completion.callback.?(prefix)) |candidate| {
                    try writer.print("{s}\n", .{candidate});
                }
                try writer.flush();
                return;
            }
        }
    }
}

fn allTreeFlags(comptime root: cmd_mod.Cmd) []const flag_mod.Flag {
    comptime {
        var out: []const flag_mod.Flag = root.flags;
        for (cmd_mod.allNodes(root)) |node| out = out ++ node.cmd.flags;
        return out;
    }
}

// =========================================================================
// Bash
// =========================================================================

fn bashScript(comptime root: cmd_mod.Cmd, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        out = out ++ "# " ++ root.name ++ " bash completion (auto-generated)\n";
        out = out ++ "# Source this file or place it in a directory loaded by bash-completion.\n\n";
        out = out ++ "_" ++ root.name ++ "() {\n";
        out = out ++
            \\    local cur prev path cmds flags values i value_prefix
            \\    cur="${COMP_WORDS[COMP_CWORD]}"
            \\    prev="${COMP_WORDS[COMP_CWORD-1]}"
            \\
            \\    path=""
            \\    for (( i=1; i<COMP_CWORD; i++ )); do
            \\        case "${COMP_WORDS[i]}" in
        ;
        out = out ++ bashPathValueSkipCases(root, options);
        out = out ++
            \\
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
        ;
        out = out ++ bashFlagValueCases(root, options);
        out = out ++
            \\
            \\    case "$path" in
            \\
        ;

        // Root case: empty path → suggest root's subcommands + flags.
        out = out ++ "        \"\")\n";
        out = out ++ "            cmds=\"" ++ joinCmdNames(root.cmds, options) ++ "\"\n";
        out = out ++ "            flags=\"" ++ joinFlagNames(root.flags, options) ++ "\"\n";
        out = out ++ "            values=\"" ++ joinPositionalValues(root.positionals) ++ "\"\n";
        out = out ++ "            ;;\n";

        // One case per non-root node.
        const nodes = cmd_mod.allNodes(root);
        for (nodes) |n| {
            if (!visibleCmd(n.cmd, options)) continue;
            out = out ++ "        \"" ++ joinPath(n.path) ++ "\")\n";
            out = out ++ "            cmds=\"" ++ joinCmdNames(n.cmd.cmds, options) ++ "\"\n";
            const inherited = cmd_mod.collectInheritedFlags(root, n.path);
            out = out ++ "            flags=\"" ++ joinFlagPair(inherited, n.cmd.flags, options) ++ "\"\n";
            out = out ++ "            values=\"" ++ joinPositionalValues(n.cmd.positionals) ++ "\"\n";
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
            \\        if [[ -n "$cmds" ]]; then
            \\            COMPREPLY=( $(compgen -W "$cmds" -- "$cur") )
            \\        else
            \\            COMPREPLY=( $(compgen -W "$values" -- "$cur") )
            \\        fi
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

fn zshScript(comptime root: cmd_mod.Cmd, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        out = out ++ "#compdef " ++ root.name ++ "\n";
        out = out ++ "# " ++ root.name ++ " zsh completion (auto-generated)\n\n";
        out = out ++ "_" ++ root.name ++ "() {\n";
        out = out ++
            \\    local cur prev path i value_prefix
            \\    cur="${words[CURRENT]}"
            \\    prev="${words[CURRENT-1]}"
            \\
            \\    path=""
            \\    for (( i=2; i<CURRENT; i++ )); do
            \\        case "${words[i]}" in
        ;
        out = out ++ zshPathValueSkipCases(root, options);
        out = out ++
            \\
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
        ;
        out = out ++ zshFlagValueCases(root, options);
        out = out ++
            \\
            \\    local -a cmds flags values
            \\    case "$path" in
            \\
        ;

        // Root case.
        out = out ++ "        \"\")\n";
        out = out ++ "            cmds=(" ++ zshCmdPairs(root.cmds, options) ++ ")\n";
        out = out ++ "            flags=(" ++ zshFlagPairs(root.flags, options) ++ ")\n";
        out = out ++ "            values=(" ++ zshWords(joinPositionalValues(root.positionals)) ++ ")\n";
        out = out ++ "            ;;\n";

        const nodes = cmd_mod.allNodes(root);
        for (nodes) |n| {
            if (!visibleCmd(n.cmd, options)) continue;
            out = out ++ "        \"" ++ joinPath(n.path) ++ "\")\n";
            out = out ++ "            cmds=(" ++ zshCmdPairs(n.cmd.cmds, options) ++ ")\n";
            const inherited = cmd_mod.collectInheritedFlags(root, n.path);
            out = out ++ "            flags=(" ++ zshFlagPairsPair(inherited, n.cmd.flags, options) ++ ")\n";
            out = out ++ "            values=(" ++ zshWords(joinPositionalValues(n.cmd.positionals)) ++ ")\n";
            out = out ++ "            ;;\n";
        }

        out = out ++
            \\    esac
            \\
            \\    if [[ "$cur" == -* ]]; then
            \\        _describe -t flags 'flags' flags
            \\    else
            \\        if (( ${#cmds} )); then
            \\            _describe -t commands 'commands' cmds
            \\        else
            \\            _describe -t values 'values' values
            \\        fi
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

fn fishScript(comptime root: cmd_mod.Cmd, comptime options: Options) []const u8 {
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
            \\    set -l skip_next 0
            \\    for word in $cmd[2..]
            \\        if test $skip_next -eq 1
            \\            set skip_next 0
            \\            continue
            \\        end
            \\        switch $word
        ;
        out = out ++ fishPathValueSkipCases(root, options);
        out = out ++
            \\
            \\            case '-*'
            \\                continue
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
            if (!visibleCmd(c, options)) continue;
            out = out ++ fishCmdLine(root.name, "", c);
        }
        for (root.flags) |f| {
            if (!visibleFlag(f, options)) continue;
            out = out ++ fishFlagLine(root.name, "", f);
        }
        out = out ++ fishPositionalLine(root.name, "", root.positionals);

        // Walk every other node.
        const nodes = cmd_mod.allNodes(root);
        for (nodes) |n| {
            if (!visibleCmd(n.cmd, options)) continue;
            const path_str = joinPath(n.path);
            for (n.cmd.cmds) |c| {
                if (!visibleCmd(c, options)) continue;
                out = out ++ fishCmdLine(root.name, path_str, c);
            }
            // Owned flags only — fish lets the user repeat globals freely;
            // listing inherited flags at every depth would just duplicate.
            for (n.cmd.flags) |f| {
                if (!visibleFlag(f, options)) continue;
                out = out ++ fishFlagLine(root.name, path_str, f);
            }
            out = out ++ fishPositionalLine(root.name, path_str, n.cmd.positionals);
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

fn bashPathValueSkipCases(comptime root: cmd_mod.Cmd, comptime options: Options) []const u8 {
    return comptime shellPathValueSkipCases(root, options, .bash);
}

fn zshPathValueSkipCases(comptime root: cmd_mod.Cmd, comptime options: Options) []const u8 {
    return comptime shellPathValueSkipCases(root, options, .zsh);
}

fn shellPathValueSkipCases(comptime root: cmd_mod.Cmd, comptime options: Options, comptime shell: Shell) []const u8 {
    comptime {
        var out: []const u8 = "";
        out = out ++ shellPathValueSkipCasesForFlags(root.flags, options, shell);
        for (cmd_mod.allNodes(root)) |n| {
            if (!visibleCmd(n.cmd, options)) continue;
            out = out ++ shellPathValueSkipCasesForFlags(n.cmd.flags, options, shell);
        }
        return out;
    }
}

fn shellPathValueSkipCasesForFlags(comptime flags: []const flag_mod.Flag, comptime options: Options, comptime shell: Shell) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (flags) |f| {
            if (!flagConsumesSeparateValue(f, options)) continue;
            out = out ++ "\n            " ++ flagCaseNamesWithShort(f) ++ ")\n";
            out = out ++ switch (shell) {
                .bash, .zsh => "                (( i++ ))\n                ;;\n",
                .fish => unreachable,
            };
        }
        return out;
    }
}

fn fishPathValueSkipCases(comptime root: cmd_mod.Cmd, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        out = out ++ fishPathValueSkipCasesForFlags(root.flags, options);
        for (cmd_mod.allNodes(root)) |n| {
            if (!visibleCmd(n.cmd, options)) continue;
            out = out ++ fishPathValueSkipCasesForFlags(n.cmd.flags, options);
        }
        return out;
    }
}

fn fishPathValueSkipCasesForFlags(comptime flags: []const flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (flags) |f| {
            if (!flagConsumesSeparateValue(f, options)) continue;
            out = out ++ "\n            case " ++ fishFlagCaseNamesWithShort(f) ++ "\n";
            out = out ++ "                set skip_next 1\n";
            out = out ++ "                continue\n";
        }
        return out;
    }
}

fn flagConsumesSeparateValue(comptime f: flag_mod.Flag, comptime options: Options) bool {
    return visibleFlag(f, options) and f.kind != .bool;
}

fn bashFlagValueCases(comptime root: cmd_mod.Cmd, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "    case \"$path\" in\n";
        out = out ++ "        \"\")\n";
        out = out ++ bashFlagValueCasesForPath(root.flags, options);
        out = out ++ "            ;;\n";
        for (cmd_mod.allNodes(root)) |n| {
            if (!visibleCmd(n.cmd, options)) continue;
            const inherited = cmd_mod.collectInheritedFlags(root, n.path);
            out = out ++ "        \"" ++ joinPath(n.path) ++ "\")\n";
            out = out ++ bashFlagValueCasesForPath(inherited ++ n.cmd.flags, options);
            out = out ++ "            ;;\n";
        }
        out = out ++ "    esac\n";
        return out;
    }
}

fn bashFlagValueCasesForPath(comptime flags: []const flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "            case \"$cur\" in\n";
        out = out ++ bashAttachedFlagValueCasesForFlags(flags, options);
        out = out ++ "            esac\n";
        out = out ++ "            case \"$prev\" in\n";
        out = out ++ bashFlagValueCasesForFlags(flags, options);
        out = out ++ "            esac\n";
        return out;
    }
}

fn bashAttachedFlagValueCasesForFlags(comptime flags: []const flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (flags) |f| {
            if (!visibleFlag(f, options)) continue;
            const comp = effectiveCompletion(f);
            if (comp.kind == .none) continue;
            out = out ++ "                " ++ attachedLongFlagCaseNames(f) ++ ")\n";
            out = out ++ "                    value_prefix=\"${cur#*=}\"\n";
            out = out ++ switch (comp.kind) {
                .values => "                    COMPREPLY=( $(compgen -W \"" ++ joinWords(comp.values) ++ "\" -- \"$value_prefix\") )\n",
                .files => "                    COMPREPLY=( $(compgen -f -- \"$value_prefix\") )\n",
                .directories => "                    COMPREPLY=( $(compgen -d -- \"$value_prefix\") )\n",
                .dynamic => "                    COMPREPLY=( $(compgen -W \"$(\"${COMP_WORDS[0]}\" __complete " ++ f.long ++ " \"$value_prefix\")\" -- \"$value_prefix\") )\n",
                .none => unreachable,
            };
            out = out ++ "                    COMPREPLY=( \"${COMPREPLY[@]/#/${cur%%=*}=}\" )\n";
            out = out ++ "                    return ;;\n";
            if (flagConsumesSeparateValue(f, options)) {
                if (f.short) |s| {
                    out = out ++ "                -" ++ &[_]u8{s} ++ "*)\n";
                    out = out ++ "                    value_prefix=\"${cur:2}\"\n";
                    out = out ++ switch (comp.kind) {
                        .values => "                    COMPREPLY=( $(compgen -W \"" ++ joinWords(comp.values) ++ "\" -- \"$value_prefix\") )\n",
                        .files => "                    COMPREPLY=( $(compgen -f -- \"$value_prefix\") )\n",
                        .directories => "                    COMPREPLY=( $(compgen -d -- \"$value_prefix\") )\n",
                        .dynamic => "                    COMPREPLY=( $(compgen -W \"$(\"${COMP_WORDS[0]}\" __complete " ++ f.long ++ " \"$value_prefix\")\" -- \"$value_prefix\") )\n",
                        .none => unreachable,
                    };
                    out = out ++ "                    COMPREPLY=( \"${COMPREPLY[@]/#/${cur:0:2}}\" )\n";
                    out = out ++ "                    return ;;\n";
                }
            }
        }
        return out;
    }
}

fn bashFlagValueCasesForFlags(comptime flags: []const flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (flags) |f| {
            if (!visibleFlag(f, options)) continue;
            const comp = effectiveCompletion(f);
            if (comp.kind == .none) continue;
            out = out ++ "                " ++ flagCaseNamesWithShort(f) ++ ")\n";
            out = out ++ switch (comp.kind) {
                .values => "                    COMPREPLY=( $(compgen -W \"" ++ joinWords(comp.values) ++ "\" -- \"$cur\") ); return ;;\n",
                .files => "                    COMPREPLY=( $(compgen -f -- \"$cur\") ); return ;;\n",
                .directories => "                    COMPREPLY=( $(compgen -d -- \"$cur\") ); return ;;\n",
                .dynamic => "                    COMPREPLY=( $(compgen -W \"$(\"${COMP_WORDS[0]}\" __complete " ++ f.long ++ " \"$cur\")\" -- \"$cur\") ); return ;;\n",
                .none => unreachable,
            };
        }
        return out;
    }
}

fn zshFlagValueCases(comptime root: cmd_mod.Cmd, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "    case \"$path\" in\n";
        out = out ++ "        \"\")\n";
        out = out ++ zshFlagValueCasesForPath(root.flags, options);
        out = out ++ "            ;;\n";
        for (cmd_mod.allNodes(root)) |n| {
            if (!visibleCmd(n.cmd, options)) continue;
            const inherited = cmd_mod.collectInheritedFlags(root, n.path);
            out = out ++ "        \"" ++ joinPath(n.path) ++ "\")\n";
            out = out ++ zshFlagValueCasesForPath(inherited ++ n.cmd.flags, options);
            out = out ++ "            ;;\n";
        }
        out = out ++ "    esac\n";
        return out;
    }
}

fn zshFlagValueCasesForPath(comptime flags: []const flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "            case \"$cur\" in\n";
        out = out ++ zshAttachedFlagValueCasesForFlags(flags, options);
        out = out ++ "            esac\n";
        out = out ++ "            case \"$prev\" in\n";
        out = out ++ zshFlagValueCasesForFlags(flags, options);
        out = out ++ "            esac\n";
        return out;
    }
}

fn zshAttachedFlagValueCasesForFlags(comptime flags: []const flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (flags) |f| {
            if (!visibleFlag(f, options)) continue;
            const comp = effectiveCompletion(f);
            if (comp.kind == .none) continue;
            out = out ++ "                " ++ attachedLongFlagCaseNames(f) ++ ")\n";
            out = out ++ "                    value_prefix=\"${cur#*=}\"\n";
            out = out ++ "                    compset -P \"${cur%%=*}=\"\n";
            out = out ++ switch (comp.kind) {
                .values => "                    _values 'values' " ++ zshWords(joinWords(comp.values)) ++ "; return ;;\n",
                .files => "                    _files; return ;;\n",
                .directories => "                    _files -/; return ;;\n",
                .dynamic => "                    compadd -- ${(f)\"$(\"$words[1]\" __complete " ++ f.long ++ " \"$value_prefix\")\"}; return ;;\n",
                .none => unreachable,
            };
            if (flagConsumesSeparateValue(f, options)) {
                if (f.short) |s| {
                    out = out ++ "                -" ++ &[_]u8{s} ++ "*)\n";
                    out = out ++ "                    value_prefix=\"${cur[3,-1]}\"\n";
                    out = out ++ "                    compset -P \"-" ++ &[_]u8{s} ++ "\"\n";
                    out = out ++ switch (comp.kind) {
                        .values => "                    _values 'values' " ++ zshWords(joinWords(comp.values)) ++ "; return ;;\n",
                        .files => "                    _files; return ;;\n",
                        .directories => "                    _files -/; return ;;\n",
                        .dynamic => "                    compadd -- ${(f)\"$(\"$words[1]\" __complete " ++ f.long ++ " \"$value_prefix\")\"}; return ;;\n",
                        .none => unreachable,
                    };
                }
            }
        }
        return out;
    }
}

fn zshFlagValueCasesForFlags(comptime flags: []const flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (flags) |f| {
            if (!visibleFlag(f, options)) continue;
            const comp = effectiveCompletion(f);
            if (comp.kind == .none) continue;
            out = out ++ "                " ++ flagCaseNamesWithShort(f) ++ ")\n";
            out = out ++ switch (comp.kind) {
                .values => "                    _values 'values' " ++ zshWords(joinWords(comp.values)) ++ "; return ;;\n",
                .files => "                    _files; return ;;\n",
                .directories => "                    _files -/; return ;;\n",
                .dynamic => "                    compadd -- ${(f)\"$(\"$words[1]\" __complete " ++ f.long ++ " \"$cur\")\"}; return ;;\n",
                .none => unreachable,
            };
        }
        return out;
    }
}

fn attachedLongFlagCaseNames(comptime f: flag_mod.Flag) []const u8 {
    comptime {
        var out: []const u8 = f.long ++ "=*";
        for (f.aliases) |alias| out = out ++ "|" ++ alias ++ "=*";
        return out;
    }
}

fn flagCaseNames(comptime f: flag_mod.Flag) []const u8 {
    comptime {
        var out: []const u8 = f.long;
        for (f.aliases) |alias| out = out ++ "|" ++ alias;
        return out;
    }
}

fn flagCaseNamesWithShort(comptime f: flag_mod.Flag) []const u8 {
    comptime {
        var out: []const u8 = flagCaseNames(f);
        if (f.short) |s| out = out ++ "|-" ++ &[_]u8{s};
        return out;
    }
}

fn fishFlagCaseNamesWithShort(comptime f: flag_mod.Flag) []const u8 {
    comptime {
        var out: []const u8 = "'" ++ fishSingleQuote(f.long) ++ "'";
        for (f.aliases) |alias| out = out ++ " '" ++ fishSingleQuote(alias) ++ "'";
        if (f.short) |s| out = out ++ " '-" ++ &[_]u8{s} ++ "'";
        return out;
    }
}

fn joinCmdNames(comptime cmds: []const cmd_mod.Cmd, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        var first = true;
        for (cmds) |c| {
            if (!visibleCmd(c, options)) continue;
            if (!first) out = out ++ " ";
            first = false;
            out = out ++ c.name;
        }
        return out;
    }
}

fn joinFlagNames(comptime flags: []const flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        var first = true;
        for (flags) |f| {
            if (!visibleFlag(f, options)) continue;
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
    comptime options: Options,
) []const u8 {
    comptime {
        const out: []const u8 = joinFlagNames(inherited, options);
        if (owned.len > 0) {
            // joinFlagNames already appends --help -h; insert owned BEFORE that.
            // Simplest: rebuild from a merged slice.
            const merged = inherited ++ owned;
            return joinFlagNames(merged, options);
        }
        return out;
    }
}

fn zshCmdPairs(comptime cmds: []const cmd_mod.Cmd, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        var first = true;
        for (cmds) |c| {
            if (!visibleCmd(c, options)) continue;
            if (!first) out = out ++ " ";
            first = false;
            out = out ++ "\"" ++ zshEscape(c.name) ++ ":" ++ zshEscapeDesc(c.desc) ++ "\"";
        }
        return out;
    }
}

fn zshFlagPairs(comptime flags: []const flag_mod.Flag, comptime options: Options) []const u8 {
    comptime {
        var out: []const u8 = "";
        var first = true;
        for (flags) |f| {
            if (!visibleFlag(f, options)) continue;
            if (!first) out = out ++ " ";
            first = false;
            out = out ++ "\"" ++ zshEscape(f.long) ++ ":" ++ zshEscapeDesc(f.desc) ++ "\"";
            if (f.short) |s| {
                out = out ++ " \"-" ++ &[_]u8{s} ++ ":" ++ zshEscapeDesc(f.desc) ++ "\"";
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
    comptime options: Options,
) []const u8 {
    comptime {
        const merged = inherited ++ owned;
        return zshFlagPairs(merged, options);
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
            " -f -a '" ++ fishSingleQuote(c.name) ++ "'";
        if (c.desc.len > 0) out = out ++ " -d '" ++ fishSingleQuote(c.desc) ++ "'";
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
        const comp = effectiveCompletion(f);
        var out: []const u8 = "complete -c " ++ bin ++
            " -n '__fish_" ++ bin ++ "_path \"" ++ path ++ "\"'" ++
            fishFlagCompletionPrefix(comp) ++
            " -l '" ++ fishSingleQuote(long_bare) ++ "'";
        if (f.short) |s| out = out ++ " -s " ++ &[_]u8{s};
        out = out ++ if (comp.kind == .dynamic)
            " -a '(" ++ bin ++ " __complete " ++ f.long ++ " (commandline -ct))'"
        else
            fishCompletionArgs(comp);
        if (f.desc.len > 0) out = out ++ " -d '" ++ fishSingleQuote(f.desc) ++ "'";
        out = out ++ "\n";
        return out;
    }
}

fn fishPositionalLine(
    comptime bin: []const u8,
    comptime path: []const u8,
    comptime positionals: []const flag_mod.Positional,
) []const u8 {
    comptime {
        const values = joinPositionalValues(positionals);
        if (values.len == 0) return "";
        return "complete -c " ++ bin ++
            " -n '__fish_" ++ bin ++ "_path \"" ++ path ++ "\"'" ++
            " -f -a '" ++ fishSingleQuote(values) ++ "'\n";
    }
}

fn joinWords(comptime values: []const []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (values, 0..) |value, idx| {
            if (idx > 0) out = out ++ " ";
            out = out ++ value;
        }
        return out;
    }
}

fn joinPositionalValues(comptime positionals: []const flag_mod.Positional) []const u8 {
    comptime {
        var out: []const u8 = "";
        var first = true;
        for (positionals) |p| {
            if (p.completion.kind != .values) continue;
            for (p.completion.values) |value| {
                if (!first) out = out ++ " ";
                first = false;
                out = out ++ value;
            }
        }
        return out;
    }
}

fn zshWords(comptime words: []const u8) []const u8 {
    comptime {
        if (words.len == 0) return "";
        var out: []const u8 = "";
        var current: []const u8 = "";
        for (words) |c| {
            if (c == ' ') {
                if (current.len > 0) {
                    if (out.len > 0) out = out ++ " ";
                    out = out ++ "\"" ++ zshEscape(current) ++ "\"";
                    current = "";
                }
            } else {
                current = current ++ &[_]u8{c};
            }
        }
        if (current.len > 0) {
            if (out.len > 0) out = out ++ " ";
            out = out ++ "\"" ++ zshEscape(current) ++ "\"";
        }
        return out;
    }
}

fn fishFlagCompletionPrefix(comptime completion: anytype) []const u8 {
    return switch (completion.kind) {
        .files, .directories => "",
        .none, .values, .dynamic => " -f",
    };
}

fn fishCompletionArgs(comptime completion: anytype) []const u8 {
    comptime {
        return switch (completion.kind) {
            .none => "",
            .values => " -a '" ++ fishSingleQuote(joinWords(completion.values)) ++ "'",
            .files => "",
            .directories => " -a '(__fish_complete_directories)'",
            .dynamic => "", // handled inline in fishFlagLine
        };
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

/// Completion to emit for a flag. A `.choice` flag auto-completes its declared
/// `choices` (unless it carries an explicit completion), so the author declares
/// the value set once. Validation guarantees choices are shell-safe.
fn effectiveCompletion(comptime f: flag_mod.Flag) @TypeOf(f.completion) {
    if (f.completion.kind == .none) {
        // Choice flags complete their declared set; path flags complete files.
        if (f.kind == .choice) return .{ .kind = .values, .values = f.choices };
        if (f.kind == .path) return .{ .kind = .files };
    }
    return f.completion;
}

fn zshEscape(comptime s: []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (s) |c| {
            out = switch (c) {
                '\\' => out ++ "\\\\",
                '"' => out ++ "\\\"",
                else => out ++ &[_]u8{c},
            };
        }
        return out;
    }
}

fn zshEscapeDesc(comptime s: []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (s) |c| {
            out = switch (c) {
                '\\' => out ++ "\\\\",
                '"' => out ++ "\\\"",
                ':' => out ++ "\\:",
                // Description prose is free-form; inside the zsh double-quoted
                // completion entries, `$` and backtick would otherwise expand.
                '$' => out ++ "\\$",
                '`' => out ++ "\\`",
                else => out ++ &[_]u8{c},
            };
        }
        return out;
    }
}

fn fishSingleQuote(comptime s: []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        for (s) |c| {
            out = switch (c) {
                '\\' => out ++ "\\\\",
                '\'' => out ++ "\\'",
                else => out ++ &[_]u8{c},
            };
        }
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

fn dynHostCandidates(prefix: []const u8) []const []const u8 {
    _ = prefix;
    return &.{ "alpha", "beta" };
}

const dyn_root = cmd_mod.Cmd{
    .name = "tool",
    .flags = &.{
        .{ .long = "--host", .kind = .string, .completion = .{ .kind = .dynamic, .callback = dynHostCandidates } },
    },
};

test "dynamic completion wires __complete in all shells and complete() runs the callback" {
    inline for (.{ Shell.bash, Shell.zsh, Shell.fish }) |sh| {
        const s = comptime script(dyn_root, sh);
        try std.testing.expect(std.mem.indexOf(u8, s, "__complete --host") != null);
    }

    var buf: [256]u8 = undefined;
    var w = std.Io.Writer.fixed(&buf);
    try complete(dyn_root, &.{ "--host", "al" }, &w);
    const out = w.buffered();
    try std.testing.expect(std.mem.indexOf(u8, out, "alpha\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "beta\n") != null);
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
