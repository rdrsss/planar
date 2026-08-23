/// @file schema.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.schema`.

module planar.cmd.planar_agent.handlers.schema;

import std;
import planar.cli;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

auto schema(context& ctx, const cli::match_result& args, const cli::cmd& root) -> handler_result {
  (void)args;
  // FRAGMENT renderer: `schema_json` documents itself as returning no
  // trailing newline, so the newline is appended here — matching
  // handlers/schema.zig's `writeAll(catalog)` + `writeAll("\n")`. See this
  // handler's module header for why that is not a blanket rule.
  ctx.out() << cli::schema_json(root) << '\n';
  return {};
}

} // namespace planar::cmd::agent::handlers
