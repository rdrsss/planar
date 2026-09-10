/// @file synthesize.cppm
/// @brief Layer-3 composition for `planar synthesize`.
module;

export module planar.cmd.planar.handlers.synthesize;

import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import cli11;

namespace planar::cmd::handlers {
/// @brief Handle `planar synthesize <repo-root> [--apply]
/// [--apply-removals] [--accept-spec <slugs>|all] [--no-forward-specs]
/// [--greenfield|--non-greenfield] [--layout <l>] [--scope <s>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the domain error the staging/reconciliation step
/// produced (invalid arguments, a missing repo root, or a database apply
/// failure).
export auto synthesize(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Declare the `synthesize` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_synthesize(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
