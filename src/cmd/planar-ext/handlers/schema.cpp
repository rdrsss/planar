/// @file schema.cpp
/// @brief Implementation of `planar.cmd.planar_ext.handlers.schema`.

module planar.cmd.planar_ext.handlers.schema;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.schema;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.handler;

namespace planar::cmd::ext::handlers {

auto schema(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result {
  (void)args;
  // The single-argument overload: this tree carries no summary table
  // (nothing to diverge from, since there is no oracle), so "summary" and
  // "description" report the same string for every node.
  ctx.out() << cliapp::schema_json(root) << '\n';
  return {};
}

} // namespace planar::cmd::ext::handlers
