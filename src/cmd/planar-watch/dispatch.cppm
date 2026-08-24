/// @file dispatch.cppm
/// @brief `planar.cmd.planar_watch.dispatch` — argv in, exit code out for
/// the `planar-watch` binary (plan 996, tasks 6107 and 6123).
///
/// Port target: the body of `main` in zig/src/cmd/planar-watch/main.zig.
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
///     $ planar-watch nosuchverb
///     stdout: "error: unknown subcommand (got nosuchverb) [in: planar-watch]\n"
///     stderr: "error: UnknownSubcommand\n"
///     exit:   1
///
/// A reasonable person would have put the parse error on stderr alone and
/// been wrong. Task 6123 re-baselined the WORDING (CLI11 writes its own
/// message and its own CamelCase error name) but deliberately kept the
/// SHAPE — formatted message to stdout, CamelCase tag to stderr, exit 1 —
/// because that is an operator/scripting contract rather than a parser
/// detail. `CLI::ParseError::get_name()` already returns CamelCase
/// (`ExtrasError`, `RequiredError`, `ValidationError`), so the stderr line
/// needed no translation table at all.
///
/// ## The default verb, reproduced as an argv rewrite (task 6136)
///
/// zig/src/cmd/planar-watch/main.zig routes a bare invocation to `feed`,
/// and it does so by REWRITING ARGV before the parser ever runs — its root
/// `cli.Cmd` declares no flags of its own, and the oracle's own `schema`
/// catalog confirms it (root `flags` is empty in both trees). So the
/// faithful port is `inject_default_verb` below, NOT declaring `feed`'s
/// nine flags on the root node: doing that would put nine flags in this
/// binary's catalog that the oracle's does not have, and
/// `src/cmd/catalog_parity.hpp` compares the two byte for byte.
///
/// Until this task, `planar-watch --json` exited 1 with `ExtrasError`
/// where the oracle streams NDJSON, and a bare `planar-watch` rendered the
/// root help page and exited 0 — the SILENT SUCCESS shape
/// `planar.cliapp.surface`'s header calls out as worse than an absent
/// node. Both now route to `feed`, which is unported, so both answer exit
/// 64 `not implemented`. That is the correct refusal: loud, and it becomes
/// the real feed the moment `feed` lands with no further change here.
module;

export module planar.cmd.planar_watch.dispatch;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch {

/// @brief The leaf-path -> handler table.
export using handler_table = std::map<std::string, handler_fn, std::less<>>;

/// @brief Build the table binding every ported leaf to its handler.
///
/// Takes `root` because both `schema` and `completion` emit a description
/// OF the tree; passing it in removes any chance of describing a different
/// tree than the one that answered the invocation. `root` must outlive the
/// returned table.
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

/// @brief Rewrite `argv` to name the default verb `feed` when the operator
/// supplied no verb at all.
///
/// The oracle's `maybeInjectDefaultVerb` carries a hardcoded list of ten
/// known verb names it checks the first token against. That list is
/// REDUNDANT and is deliberately not transcribed: every branch it guards
/// (`return raw_args`) is also what the function's final fallthrough does,
/// and no verb name can begin with `-`, so the observable rule reduces to
/// the three cases below with no behavioural difference. Reproducing the
/// list would additionally have frozen it at the oracle's ten while this
/// tree declares thirteen verbs.
/// @param argv The full process argv, `argv[0]` included.
/// @return The rewritten argv: unchanged when a verb (or `--help`/`-h`)
/// leads, otherwise with `feed` spliced in at position 1.
export auto inject_default_verb(std::span<std::string const> argv) -> std::vector<std::string>;

/// @brief Parse `argv` against `root`, then render help, run the matched
/// handler, or report a failure — writing to `ctx`'s streams throughout.
/// @param ctx The invocation context.
/// @param root The command tree (mutated by CLI11's parse; the caller owns it).
/// @param table The handler table.
/// @return The process exit code, under `planar-watch`'s policy.
export auto run(context& ctx, CLI::App& root, const handler_table& table) -> int;

} // namespace planar::cmd::watch
