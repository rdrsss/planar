/// @file version.cppm
/// @brief `planar.cmd.planar_watch.handlers.version` — the `planar-watch
/// version` leaf (plan 996, task 6107).
///
/// The shared renderer emits six text positions and a JSON metadata object.
/// Version queries do not open the database or resolve a project scope.
module;

export module planar.cmd.planar_watch.handlers.version;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief Handle `planar-watch version`.
/// @param ctx The invocation context.
/// @param args The parsed arguments (including the optional --json flag).
/// @return Success; this leaf has no failure path.
export auto version(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::watch::handlers
