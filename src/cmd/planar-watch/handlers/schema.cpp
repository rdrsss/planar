/// @file schema.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.schema`.

module planar.cmd.planar_watch.handlers.schema;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.schema;
import planar.cmd.planar_watch.surface;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

auto schema(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result {
  (void)args;
  // The summaries table is what makes `"summary"` differ from
  // `"description"` where the oracle's does. See
  // `planar.cliapp.schema`'s header, divergence 1.
  ctx.out() << cliapp::schema_json(root, surface_summaries()) << '\n';
  return {};
}

} // namespace planar::cmd::watch::handlers
