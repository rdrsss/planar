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
/// ## A node can be a GROUP and a LEAF at once (plan 996, task 6040)
///
/// `handoff` and `resume` are both. Each carries subcommands AND its own
/// `run` with its own positional, and the oracle DISPATCHES the parent
/// when no child is named:
///
///     $ planar handoff        -> exit 2, "no active session (run
///                                `planar capture session` first)"
///     $ planar resume         -> exit 1, "no active task in cwd-derived
///                                scope; pass <task-id> explicitly"
///     $ planar capture        -> exit 0, help page
///     $ planar feedback       -> exit 0, help page
///
/// The last two are pure groups, so the previous rule — "a matched node
/// with children renders help" — was right for every verb this binary had
/// before this task and wrong for these two. The rule is therefore
/// narrowed rather than replaced: a matched node with children renders
/// help ONLY WHEN THE TABLE HAS NO ENTRY FOR IT. A registered handler on a
/// group node means the group is dual and the handler wins.
///
/// That keeps `capture`, `feedback`, `workbench`, `workflow`, `annotate`,
/// `workspace` and bare `planar` on the help path (none is registered) and
/// routes `handoff` / `resume` to their handlers, with no per-verb special
/// case anywhere.
///
/// `unreachable_handlers` widened to match: it walks EVERY node rather
/// than only childless ones, because a handler on a dual group node is now
/// genuinely reachable. `unregistered_leaves` did NOT widen — every
/// childless leaf must still have a handler; a group having one stays
/// optional.
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
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd {

/// @brief The leaf-path -> handler table.
export using handler_table = std::map<std::string, handler_fn, std::less<>>;

/// @brief Build the table binding every leaf to a handler.
///
/// Two populations, and the difference is the point. The verbs that are
/// IMPLEMENTED are registered one line each, by hand. The verbs that are
/// merely DECLARED — the ~190 the full-surface catalog carries so
/// `cli_usage_lint` can resolve them — are registered in bulk from
/// `planar.cmd.planar.surface`'s generated inventory, each bound to a
/// handler that refuses at exit 64 naming the verb. Neither population
/// exits 0 without doing the work.
/// @param root The command tree. Borrowed and captured by the `schema` and
/// `completion` entries, which describe the tree rather than the database,
/// so it must outlive the returned table.
/// @return The populated table.
export auto handlers(const CLI::App& root) -> handler_table;

/// @brief Every leaf in `root` that `table` has no handler for.
/// @param root The command tree.
/// @param table The handler table.
/// @return The unwired leaf keys, in tree-walk order.
export auto unregistered_leaves(const CLI::App& root, const handler_table& table) -> std::vector<std::string>;

/// @brief Every table key that does not correspond to ANY node in `root`.
///
/// The other direction of the same gate: a handler registered under a
/// misspelled or removed path is dead code that no argv can reach, and
/// looks exactly like working coverage until someone tries the verb.
///
/// Walks every node, not only childless ones, because a dual group-and-leaf
/// node (`handoff`, `resume`) legitimately carries a handler — see this
/// module's header. A key naming no node at all is still dead.
/// @param root The command tree.
/// @param table The handler table.
/// @return The unreachable table keys, sorted.
export auto unreachable_handlers(const CLI::App& root, const handler_table& table) -> std::vector<std::string>;

/// @brief How an invocation ended: the exit code, plus enough to classify
/// it for `cli_invocations.error_category`.
export struct run_outcome {
  /// @brief The process exit code, under the operator binary's policy.
  int code = 0;
  /// @brief The domain-error kind behind a non-zero `code`, when there is
  /// one to name. Unset on success AND on a failure with no domain error
  /// to classify.
  std::optional<domain_error_kind> kind;
  /// @brief Whether this invocation is ELIGIBLE for invocation capture.
  ///
  /// False only for a worktree-gate refusal, and that is the oracle's
  /// behaviour rather than a policy choice here:
  /// `zig/src/cmd/planar/worktree_gate.zig:157` calls `std.process.exit`
  /// DIRECTLY, bypassing `exit.die` — the one place the Zig binary calls
  /// `cli_log.record` on a failure path. So a gated invocation writes no
  /// row, which running the oracle confirms: three `exit 8` invocations
  /// against a logging-enabled scratch database produced no
  /// `cli_invocations` rows at all.
  bool loggable = true;
};

/// @brief Parse `argv` against `root`, then render help, run the matched
/// handler, or report a failure — writing to `ctx`'s streams throughout.
/// @param ctx The invocation context.
/// @param root The command tree (mutated by CLI11's parse; the caller owns it).
/// @param table The handler table.
/// @return The process exit code, under the operator binary's policy.
export auto run(context& ctx, CLI::App& root, const handler_table& table) -> int;

/// @brief `run`, reporting how the invocation ended rather than only its
/// code. `main` uses this so `cli_log` can record the same error CATEGORY
/// the oracle does — classifying by exit code alone would file every
/// `not_found` as `internal`.
/// @param ctx The invocation context.
/// @param root The command tree (mutated by CLI11's parse; the caller owns it).
/// @param table The handler table.
/// @return The outcome.
export auto run_detailed(context& ctx, CLI::App& root, const handler_table& table) -> run_outcome;

} // namespace planar::cmd
