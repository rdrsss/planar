/// @file version.cppm
/// @brief `planar.cmd.planar_ext.handlers.version` — the `planar-ext
/// version` leaf (plan 996, task 6418).
///
/// Same shape as the other three binaries' `version` leaf: the cheapest
/// end-to-end proof that this binary's spine works, and it opens no
/// database.
module;

export module planar.cmd.planar_ext.handlers.version;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.handler;

namespace planar::cmd::ext::handlers {

/// @brief Handle `planar-ext version`.
/// @param ctx The invocation context.
/// @param args The parsed arguments (the leaf declares none).
/// @return Success; this leaf has no failure path.
export auto version(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::ext::handlers
