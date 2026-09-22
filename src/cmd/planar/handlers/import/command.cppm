/// @file import.cppm
/// @brief `planar import`: layer-3 composition of staging and plan creation.
module;

export module planar.cmd.planar.handlers.importer;

import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import cli11;

namespace planar::cmd::handlers {
/// @brief Handle `planar import <repo-root> [--interpret] [--apply]
/// [--apply-removals] [--accept-spec <slugs>|all] [--no-forward-specs]
/// [--scope <s>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the domain error the staging/reconciliation step
/// produced (invalid arguments, a missing repo root, or a database apply
/// failure).
export auto import_repo(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Declare the `import` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_import(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
