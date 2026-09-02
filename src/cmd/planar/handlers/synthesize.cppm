/// @file synthesize.cppm
/// @brief Layer-3 composition for `planar synthesize`.
module;

export module planar.cmd.planar.handlers.synthesize;

import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

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
} // namespace planar::cmd::handlers
