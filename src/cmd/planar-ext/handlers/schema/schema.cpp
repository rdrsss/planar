/// @file schema.cpp
/// @brief Implementation of `planar.cmd.planar_ext.handlers.schema`.

module planar.cmd.planar_ext.handlers.schema;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.schema;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.docs;
import planar.cmd.planar_ext.exit;
import planar.cmd.planar_ext.handler;

namespace planar::cmd::ext::handlers {

auto schema(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result {
  // The single-argument overload: this tree carries no summary table
  // (nothing to diverge from, since there is no oracle), so "summary" and
  // "description" report the same string for every node.
  auto const selected = cliapp::select_schema(cliapp::schema_json(root, {}, {}, surface_docs()), cliapp::schema_request_of(args));
  if (!selected) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, selected.error()));
  }
  ctx.out() << *selected << '\n';
  return {};
}

} // namespace planar::cmd::ext::handlers
