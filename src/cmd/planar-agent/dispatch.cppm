/// @file dispatch.cppm
/// @brief `planar.cmd.planar_agent.dispatch` — argv in, exit code out for
/// the `planar-agent` binary (plan 996, task 6107).
///
/// Port target: the body of `main` in zig/src/cmd/planar-agent/main.zig,
/// minus the process plumbing.
///
/// Same path-keyed-table shape task 6105 settled for `planar`, and for the
/// same reasons (see `planar.cmd.planar.dispatch`'s header: layer-1
/// `planar.cli.cmd` carries no handler field, and an if/else chain makes
/// "is every leaf wired?" unanswerable). What is NOT shared is the exit
/// policy, and this module is where that shows:
///
///   `run` routes parse failures through
///   `cli::exit_code_for(domain_error_kind::parse_error,
///   binary_kind::planar_agent)` — exit 1 — NOT through
///   `cli::exit_code_for_parse_error_planar_binary`, which is hardcoded to
///   the operator binary's exit 2. That layer-1 function was renamed in
///   task 6066 specifically because a `planar-agent` call site written as
///   `exit_code_for(err.kind)` would silently pick it up by overload
///   resolution and apply the wrong policy. Verified against the oracle
///   this task: `planar-agent nosuchverb` exits 1, `planar nosuchverb`
///   exits 2, identical argv shape.
///
/// The two-stream parse-error behaviour IS shared, and is oracle-captured
/// on this binary too:
///
///     $ planar-agent nosuchverb
///     stdout: "error: unknown subcommand (got nosuchverb) [in: planar-agent]\n"
///     stderr: "error: UnknownSubcommand\n"
///     exit:   1
module;

export module planar.cmd.planar_agent.dispatch;

import std;
import planar.cli;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent {

/// @brief The leaf-path -> handler table.
export using handler_table = std::map<std::string, handler_fn, std::less<>>;

/// @brief The key a resolved command path maps to: its segments joined
/// with single spaces.
/// @param path The resolved command path.
/// @return The table key.
export auto path_key(std::span<const std::string> path) -> std::string;

/// @brief Build the table binding every ported leaf to its handler.
///
/// Takes `root` because the `schema` leaf emits the very tree dispatch
/// routes through; passing it in rather than rebuilding it inside the
/// handler removes any chance of the catalog describing a different tree
/// than the one that answered the invocation. `root` must outlive the
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
/// handler, or report a failure — writing to `ctx`'s streams throughout.
/// @param ctx The invocation context.
/// @param root The command tree.
/// @param table The handler table.
/// @return The process exit code, under `planar-agent`'s policy.
export auto run(context& ctx, const cli::cmd& root, const handler_table& table) -> int;

} // namespace planar::cmd::agent
