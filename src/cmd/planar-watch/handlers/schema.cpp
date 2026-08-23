/// @file schema.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.schema`.

module planar.cmd.planar_watch.handlers.schema;

import std;
import planar.cli;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

auto schema(context& ctx, const cli::match_result& args, const cli::cmd& root) -> handler_result {
  (void)args;
  ctx.out() << cli::schema_json(root) << '\n';
  return {};
}

} // namespace planar::cmd::watch::handlers
