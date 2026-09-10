/// @file dispatch.cppm
/// @brief `planar.cmd.planar_agent.dispatch` — argv in, exit code out for
/// the `planar-agent` binary (plan 996, tasks 6107 and 6123).
///
/// Port target: the body of `main` in zig/src/cmd/planar-agent/main.zig.
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
/// ## Parse failures go to stderr ONLY (decision 1004, task 6271)
///
/// The oracle writes two messages for one parse failure:
///
///     $ planar-agent nosuchverb
///     stdout: "error: unknown subcommand (got nosuchverb) [in: planar-agent]\n"
///     stderr: "error: UnknownSubcommand\n"
///     exit:   1
///
/// Task 6123 re-baselined the WORDING (CLI11 writes its own message and
/// its own CamelCase error name) but at the time kept the oracle's SHAPE —
/// formatted message to stdout, CamelCase tag to stderr. Decision 1004
/// (task 6271) reverses that: a parse failure now writes ONE line to
/// stderr and nothing to stdout, so it presents identically to a handler
/// refusal. The CamelCase tag (`CLI::ParseError::get_name()`, e.g.
/// `ExtrasError`, `RequiredError`, `ValidationError`) is DROPPED rather
/// than kept as a second stderr line — see `planar.cmd.planar.dispatch`'s
/// header for the full rationale. This is a deliberate, recorded
/// divergence from the oracle, which keeps its own split untouched. The
/// exit code is unaffected.
///
/// A bare `planar-agent`, and a bare `planar-agent action`, render that
/// node's help page and exit 0 — the same thing the deleted parser did for
/// a bare parent verb.
module;

export module planar.cmd.planar_agent.dispatch;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent {

/// @brief The leaf-path -> handler table.
export using handler_table = std::map<std::string, handler_fn, std::less<>>;

/// @brief Build the table binding every ported leaf to its handler.
///
/// Takes `root` because `schema` emits a description OF the tree; passing
/// it in removes any chance of describing a different tree than the one
/// that answered the invocation. `root` must outlive the returned table.
/// @param root The command tree the returned handlers close over.
/// @return The populated table.
export auto handlers(const CLI::App& root) -> handler_table;

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
/// @return The process exit code, under `planar-agent`'s policy.
export auto run(context& ctx, CLI::App& root, const handler_table& table) -> int;

} // namespace planar::cmd::agent
