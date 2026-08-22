/// @file completion.cppm
/// @brief `planar.cli.completion` — generates a shell-completion script from
/// a `cmd` tree.
///
/// Reduced-scope, behavior-preserving port of
/// zig/vendor/etcli/src/cli/completion.zig (D2, D9). etcli's generator
/// additionally wires per-flag *value* completion (a declared `values`
/// list, `files`/`directories`, or a `dynamic` runtime callback reached via
/// a `__complete` sub-invocation) and attached-`=value` completion cases.
/// This port keeps the mechanism etcli itself calls out as the durable
/// part — "take the current command-line tokens, strip flags, join the
/// remainder into a path string, and switch on that path to decide which
/// subcommands and flags to suggest" — for command-name and flag-name
/// completion, and defers flag-*value* completion (the `completion`
/// metadata on `flag`/`positional` that etcli's `meta.Completion` carries)
/// as out of scope: no verb this task models declares a value-completion
/// set, and the path-keyed script-generation shape below is unchanged by
/// adding it later.
module;

export module planar.cli.completion;

import std;
import planar.cli.cmd;

namespace planar::cli {

/// @brief Target shell for `generate_script`.
export enum class shell : std::uint8_t { bash, zsh, fish };

/// @brief Generate the completion script for `root` targeted at `sh`.
/// @param root The command tree to generate completions for.
/// @param sh The target shell.
/// @return The generated script text, ready to source (bash/zsh) or write
/// to `~/.config/fish/completions/<name>.fish` (fish).
export auto generate_script(cmd const& root, shell sh) -> std::string;

} // namespace planar::cli
