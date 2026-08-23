/// @file dispatch.cppm
/// @brief `planar.cmd.planar.dispatch` — argv in, exit code out: the one
/// place that parses, routes to a handler, renders help, and turns a
/// failure into bytes plus a code (plan 996, task 6105).
///
/// Port target: the body of `main` in zig/src/cmd/planar/main.zig, minus
/// the process plumbing.
///
/// ## Routing: a path-keyed table, not a `run` field and not an if-chain
///
/// etcli binds a leaf to code with a `run` field on the tree node
/// (`.run = cli.handler(list.handle)`). That is unavailable here on
/// purpose: `planar.cli.cmd` is a LAYER-1 type and its port deliberately
/// carries no handler field (its header says so explicitly), because what a
/// handler IS — this binary's `context`, its `domain_error` — is layer-3
/// vocabulary that has no business in a base library. Adding one would
/// mean either templating the layer-1 type over a layer-3 concept or
/// erasing through `void*` the way the Zig side does.
///
/// The alternative considered and rejected was an `if`/`else` chain over
/// `outcome.match.path` in `run` itself. It needs no new type, but at the
/// ~200 leaves this binary is heading for it is unreadable, unsearchable,
/// and — the part that actually matters — it makes "is every leaf in the
/// tree wired to something?" a question nobody can answer except by
/// reading every branch.
///
/// So: `std::map<std::string, handler_fn>` keyed by the space-joined
/// resolved path (`"workflow list"`). Registration is one line per leaf,
/// entirely inside layer 3, and it buys a property the other two shapes
/// cannot offer — `unregistered_leaves` below walks
/// `cli::all_leaves(root)` and reports any leaf with no handler, so
/// "someone added a tree node and forgot to wire it" is a FAILING TEST
/// rather than a runtime fallthrough an operator discovers. With ~200
/// verbs still to port, that gate is the reason this shape was chosen over
/// the simpler one.
///
/// ## Parse failures go to BOTH streams, and that is the oracle's shape
///
/// Captured, not assumed. `planar nosuchverb`:
///
///     stdout: "error: unknown subcommand (got nosuchverb) [in: planar]\n"
///     stderr: "error: UnknownSubcommand\n"
///     exit:   2
///
/// Two messages, two streams, one invocation. The first is etcli's own
/// formatter writing to the writer `cli.dispatch` was handed (main.zig
/// passes `ctx.stdout`); the second is main.zig's `exit.die(ctx, e, "{s}",
/// .{@errorName(e)})`. A reasonable person would have put the parse error
/// on stderr alone and been wrong. `run` reproduces both.
module;

export module planar.cmd.planar.dispatch;

import std;
import planar.cli;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd {

/// @brief The leaf-path -> handler table.
export using handler_table = std::map<std::string, handler_fn, std::less<>>;

/// @brief The key a resolved command path maps to: its segments joined
/// with single spaces, e.g. `{"workflow","list"}` -> `"workflow list"`.
/// @param path The resolved command path.
/// @return The table key.
export auto path_key(std::span<const std::string> path) -> std::string;

/// @brief Build the table binding every ported leaf to its handler.
/// @return The populated table.
export auto handlers() -> handler_table;

/// @brief Every leaf in `root` that `table` has no handler for.
///
/// The registration gate. A leaf added to the tree without a table entry
/// would otherwise be reachable from argv and fall through routing at
/// runtime; this turns it into a test failure at the point the tree
/// changes. Reported as `path_key` strings, sorted, so a failing
/// assertion names the offender.
/// @param root The command tree.
/// @param table The handler table.
/// @return The unwired leaf keys, in tree-walk order.
export auto unregistered_leaves(const cli::cmd& root, const handler_table& table) -> std::vector<std::string>;

/// @brief Every table key that does not correspond to a leaf in `root`.
///
/// The other direction of the same gate: a handler registered under a
/// misspelled or removed path is dead code that no argv can reach, and
/// looks exactly like working coverage until someone tries the verb.
/// @param root The command tree.
/// @param table The handler table.
/// @return The unreachable table keys, sorted.
export auto unreachable_handlers(const cli::cmd& root, const handler_table& table) -> std::vector<std::string>;

/// @brief Parse `argv` against `root`, then render help, run the matched
/// handler, or report a failure — writing to `ctx`'s streams throughout.
/// @param ctx The invocation context.
/// @param root The command tree.
/// @param table The handler table.
/// @return The process exit code.
export auto run(context& ctx, const cli::cmd& root, const handler_table& table) -> int;

} // namespace planar::cmd
