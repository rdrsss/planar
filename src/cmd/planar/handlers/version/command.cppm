/// @file src/cmd/planar/handlers/version/command.cppm
/// @brief `planar.cmd.planar.handlers.version` — the `planar version` leaf
/// (plan 996, task 6105).
///
/// The shared renderer emits six text positions and a JSON metadata object.
/// Version queries do not open the database or resolve a project scope.
module;

export module planar.cmd.planar.handlers.version;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar version`.
/// @param ctx The invocation context.
/// @param args The parsed arguments (including the optional --json flag).
/// @return Success; this leaf has no failure path.
export auto version(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `version` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_version(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
