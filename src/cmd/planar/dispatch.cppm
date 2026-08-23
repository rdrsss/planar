/// @file dispatch.cppm
/// @brief `planar.cmd.planar.dispatch` — argv in, exit code out for
/// the `planar` operator binary (plan 996, tasks 6105 and 6123).
///
/// Port target: the body of `main` in zig/src/cmd/planar/main.zig.
///
/// ## Routing: a path-keyed table, not a `run` field and not an if-chain
///
/// CLI11 can bind a callback per subcommand (`App::callback`). That is not
/// used here, for the same reason etcli's `run` field was not: a handler
/// needs this binary's `context` and returns this binary's
/// `domain_error`, and threading those through a `std::function<void()>`
/// captured at tree-build time makes "which leaf produced this failure"
/// unrecoverable at the one place that maps a failure to an exit code.
///
/// So: `std::map<std::string, handler_fn>` keyed by the space-joined
/// resolved path. Registration is one line per leaf, and it buys a
/// property a callback tree cannot — `unregistered_leaves` walks
/// `cliapp::leaf_keys(root)` and reports any leaf with no handler, so
/// "someone added a tree node and forgot to wire it" is a FAILING TEST
/// rather than a runtime fallthrough an operator discovers.
///
/// ## Parse failures go to BOTH streams, and that shape is preserved
///
/// The oracle writes two messages for one parse failure:
///
///     $ planar nosuchverb
///     stdout: "error: unknown subcommand (got nosuchverb) [in: planar]\n"
///     stderr: "error: UnknownSubcommand\n"
///     exit:   2
///
/// A reasonable person would have put the parse error on stderr alone and
/// been wrong. Task 6123 re-baselined the WORDING (CLI11 writes its own
/// message and its own CamelCase error name) but deliberately kept the
/// SHAPE — formatted message to stdout, CamelCase tag to stderr, exit 2 —
/// because that is an operator/scripting contract rather than a parser
/// detail. `CLI::ParseError::get_name()` already returns CamelCase
/// (`ExtrasError`, `RequiredError`, `ValidationError`), so the stderr line
/// needed no translation table at all.
///
/// ## The bare-invocation TTY cockpit gate is NOT reproduced
///
/// zig/src/cmd/planar/main.zig routes a bare `planar` on a TTY to the
/// interactive cockpit. There is no cockpit in this tree, so a bare
/// invocation renders the root help page — which is exactly what the Zig
/// binary does when the gate refuses (non-TTY, `TERM=dumb`,
/// `PLANAR_NO_TUI`), i.e. what every scripted invocation already sees.
module;

export module planar.cmd.planar.dispatch;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd {

/// @brief The leaf-path -> handler table.
export using handler_table = std::map<std::string, handler_fn, std::less<>>;

/// @brief Build the table binding every ported leaf to its handler.
///
/// @return The populated table.
export auto handlers() -> handler_table;

/// @brief Every leaf in `root` that `table` has no handler for.
/// @param root The command tree.
/// @param table The handler table.
/// @return The unwired leaf keys, in tree-walk order.
export auto unregistered_leaves(const CLI::App& root, const handler_table& table) -> std::vector<std::string>;

/// @brief Every table key that does not correspond to a leaf in `root`.
///
/// The other direction of the same gate: a handler registered under a
/// misspelled or removed path is dead code that no argv can reach, and
/// looks exactly like working coverage until someone tries the verb.
/// @param root The command tree.
/// @param table The handler table.
/// @return The unreachable table keys, sorted.
export auto unreachable_handlers(const CLI::App& root, const handler_table& table) -> std::vector<std::string>;

/// @brief Parse `argv` against `root`, then render help, run the matched
/// handler, or report a failure — writing to `ctx`'s streams throughout.
/// @param ctx The invocation context.
/// @param root The command tree (mutated by CLI11's parse; the caller owns it).
/// @param table The handler table.
/// @return The process exit code, under the operator binary's policy.
export auto run(context& ctx, CLI::App& root, const handler_table& table) -> int;

} // namespace planar::cmd
