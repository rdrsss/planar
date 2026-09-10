/// @file completion.cppm
/// @brief `planar.cliapp.completion` — generates a shell-completion script
/// from a built `CLI::App` (plan 996, task 6123).
///
/// ## CLI11 HAS NO COMPLETION GENERATOR. This is what that costs.
///
/// The task brief asked for the accounting, so: CLI11 2.7.2 ships no
/// completion facility of any kind — no `bash`/`zsh`/`fish` emitter, no
/// `__complete` protocol hook, nothing. Its entire introspection surface is
/// the `get_*` accessors this module walks. So "adopt CLI11's completion"
/// was never an option to weigh; the only two options were KEEP the
/// generator (retargeted from the deleted `cli::cmd` tree onto `CLI::App`)
/// or DROP `planar-watch completion` as a verb.
///
/// Kept, because dropping it removes an operator-facing verb the oracle
/// exposes, and the generator is a pure function of a tree — the same
/// argument that keeps the schema emitter (see `planar.cliapp.schema`).
/// The mechanism etcli-zig itself called out as the durable part is
/// preserved unchanged: take the current command-line tokens, strip flags,
/// join the remainder into a path string, and switch on that path to
/// decide which subcommands and flags to suggest.
///
/// NOTHING was lost in the swap that was not already deferred. The
/// predecessor (`planar.cli.completion`) had already dropped flag-VALUE
/// completion (etcli-zig's `values`/`files`/`directories`/`dynamic`
/// metadata and its `__complete` sub-invocation) as out of scope, and its
/// generated script was explicitly NOT oracle-byte-comparable — its own
/// CMakeLists said so and no test claimed otherwise. This port emits the
/// same script shape from the same information. The one difference is the
/// SOURCE of a flag's description: `CLI::Option::get_description()` rather
/// than `cli::flag::desc`, which is the same string the tree author wrote.
module;

export module planar.cliapp.completion;

import std;
import cli11;

namespace planar::cliapp {

/// @brief Target shell for `generate_script`.
export enum class shell : std::uint8_t { bash, zsh, fish };

/// @brief Generate the completion script for `root` targeted at `sh`.
/// @param root The command tree to generate completions for.
/// @param sh The target shell.
/// @return The generated script text, ready to source (bash/zsh) or write
/// to `~/.config/fish/completions/<name>.fish` (fish).
export auto generate_script(const CLI::App& root, shell sh) -> std::string;

} // namespace planar::cliapp
