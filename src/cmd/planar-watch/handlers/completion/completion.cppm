/// @file completion.cppm
/// @brief `planar.cmd.planar_watch.handlers.completion` — the
/// `planar-watch completion <shell>` leaf (plan 996, task 6107).
///
/// Port target: zig/src/cmd/planar-watch/handlers/completion.zig, whose
/// header records the property this port keeps: the script is generated
/// "from this binary's root command tree, so we get per-binary completions
/// without sharing state between the three command surfaces." The tree is
/// therefore passed in rather than rebuilt, for the same reason `schema`
/// takes it — a completion script describing a different tree than the one
/// dispatch routes through is worse than none.
///
/// ## Why this leaf is in the subset
///
/// It is the only ported leaf on this binary with a REQUIRED POSITIONAL and
/// a real failure path, and both are oracle-captured:
///
///     $ planar-watch completion badshell
///     stderr: "error: unsupported shell 'badshell'; supported: bash, zsh, fish\n"
///     exit:   2                       (invalid_input — NOT a parse error)
///
///     $ planar-watch completion
///     stdout: "error: required positional missing: <shell>\n"
///     stderr: "error: MissingRequiredPositional\n"
///     exit:   1                       (parse error, this binary's policy)
///
/// Those two exits are the discrimination that matters. A shell the parser
/// accepted but the handler rejects is exit 2; a positional the parser
/// never saw is exit 1. Collapsing this binary's exit policy to a single
/// code — the easiest way to get the port wrong — cannot pass both.
///
/// ## The generated script is NOT byte-comparable to the oracle
///
/// `planar.cliapp.completion`'s own header says so: the port keeps
/// command-name and flag-name completion and DEFERS flag-*value*
/// completion (etcli-zig's `values` / `files` / `dynamic` metadata). No
/// verb in this tree declares a value-completion set, so nothing is lost
/// functionally, but the emitted script differs from the oracle's and no
/// test here claims otherwise. What IS pinned: exit 0, a non-empty script
/// per shell, three DIFFERENT scripts, and the two failure paths above.
module;

export module planar.cmd.planar_watch.handlers.completion;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief Handle `planar-watch completion <shell>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments; `shell` is the required positional.
/// @param root The command tree to generate completions for.
/// @return Success, or an `invalid_input` (exit 2) carrying the oracle's
/// `unsupported shell '<x>'; supported: bash, zsh, fish` body when the
/// positional names something other than bash/zsh/fish.
export auto completion(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result;

} // namespace planar::cmd::watch::handlers
