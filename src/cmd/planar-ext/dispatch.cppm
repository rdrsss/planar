/// @file dispatch.cppm
/// @brief `planar.cmd.planar_ext.dispatch` — argv in, exit code out for the
/// `planar-ext` binary (plan 996, task 6418).
///
/// Same routing shape as the other three binaries: a path-keyed
/// `std::map<std::string, handler_fn>` rather than a `CLI::App::callback`
/// tree, for the same reason — a handler needs THIS binary's `context` and
/// returns THIS binary's `domain_error`, and `unregistered_leaves` /
/// `unreachable_handlers` turn "declared but unwired" and "wired but
/// unreachable" into failing tests instead of runtime surprises.
module;

export module planar.cmd.planar_ext.dispatch;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.handler;

namespace planar::cmd::ext {

/// @brief The leaf-path -> handler table.
export using handler_table = std::map<std::string, handler_fn, std::less<>>;

/// @brief Build the table binding every leaf to its handler.
/// @param root The command tree the returned handlers close over.
/// @return The populated table.
export auto handlers(const CLI::App& root) -> handler_table;

/// @brief Every leaf in `root` that `table` has no handler for.
/// @param root The command tree.
/// @param table The handler table.
/// @return The unwired leaf keys, in tree-walk order.
export auto unregistered_leaves(const CLI::App& root, const handler_table& table) -> std::vector<std::string>;

/// @brief Every table key that does not correspond to a leaf in `root`.
/// @param root The command tree.
/// @param table The handler table.
/// @return The unreachable table keys, sorted.
export auto unreachable_handlers(const CLI::App& root, const handler_table& table) -> std::vector<std::string>;

/// @brief Parse `argv` against `root`, then render help, run the matched
/// handler, or report a failure — writing to `ctx`'s streams throughout.
/// @param ctx The invocation context.
/// @param root The command tree (mutated by CLI11's parse; the caller owns it).
/// @param table The handler table.
/// @return The process exit code, under `planar-ext`'s policy.
export auto run(context& ctx, CLI::App& root, const handler_table& table) -> int;

} // namespace planar::cmd::ext
