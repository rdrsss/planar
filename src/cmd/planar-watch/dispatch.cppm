/// @file dispatch.cppm
/// @brief `planar.cmd.planar_watch.dispatch` — argv in, exit code out for
/// the `planar-watch` binary (plan 996, task 6107).
///
/// Port target: the body of `main` in zig/src/cmd/planar-watch/main.zig.
///
/// Same path-keyed-table shape task 6105 settled (see
/// `planar.cmd.planar.dispatch`'s header for why a table rather than a
/// `run` field or an if-chain), and the same parse-error exit policy as
/// `planar-agent` — exit 1, not the operator binary's 2. Oracle-captured
/// on this binary specifically rather than assumed from the agent's:
///
///     $ planar-watch nosuchverb
///     stdout: "error: unknown subcommand (got nosuchverb) [in: planar-watch]\n"
///     stderr: "error: UnknownSubcommand\n"
///     exit:   1
///
/// ## The default verb is NOT reproduced, and it is a real divergence
///
/// zig/src/cmd/planar-watch/main.zig routes a bare invocation to `feed`
/// (its `--help` says so: "The default invocation with no args is the
/// activity feed"). `feed` is unported — blocked on
/// `engine.runtime.agentactivity`, see `planar.cmd.planar_watch.tree` —
/// so a bare `planar-watch` here falls through to the root help page.
/// Routing to a `feed` that does not exist would exit 64; rendering help
/// is the same thing the parser already does for a bare parent verb.
/// Named rather than silently inherited, and it closes when `feed` lands.
module;

export module planar.cmd.planar_watch.dispatch;

import std;
import planar.cli;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch {

/// @brief The leaf-path -> handler table.
export using handler_table = std::map<std::string, handler_fn, std::less<>>;

/// @brief The key a resolved command path maps to: its segments joined
/// with single spaces.
/// @param path The resolved command path.
/// @return The table key.
export auto path_key(std::span<const std::string> path) -> std::string;

/// @brief Build the table binding every ported leaf to its handler.
///
/// Takes `root` because both `schema` and `completion` emit a description
/// OF the tree; passing it in removes any chance of describing a different
/// tree than the one that answered the invocation. `root` must outlive the
/// returned table.
/// @param root The command tree the returned handlers close over.
/// @return The populated table.
export auto handlers(const cli::cmd& root) -> handler_table;

/// @brief Every leaf in `root` that `table` has no handler for.
/// @param root The command tree.
/// @param table The handler table.
/// @return The unwired leaf keys, in tree-walk order.
export auto unregistered_leaves(const cli::cmd& root, const handler_table& table) -> std::vector<std::string>;

/// @brief Every table key that does not correspond to a leaf in `root`.
/// @param root The command tree.
/// @param table The handler table.
/// @return The unreachable table keys, sorted.
export auto unreachable_handlers(const cli::cmd& root, const handler_table& table) -> std::vector<std::string>;

/// @brief Parse `argv` against `root`, then render help, run the matched
/// handler, or report a failure.
/// @param ctx The invocation context.
/// @param root The command tree.
/// @param table The handler table.
/// @return The process exit code, under `planar-watch`'s policy.
export auto run(context& ctx, const cli::cmd& root, const handler_table& table) -> int;

} // namespace planar::cmd::watch
