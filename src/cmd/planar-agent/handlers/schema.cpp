/// @file schema.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.schema`.

module planar.cmd.planar_agent.handlers.schema;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.schema;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

auto schema(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result {
  (void)args;
  // FRAGMENT renderer: `schema_json` documents itself as returning no
  // trailing newline, so the newline is appended here — matching
  // handlers/schema.zig's `writeAll(catalog)` + `writeAll("\n")`. See this
  // handler's module header for why that is not a blanket rule.
  // The single-argument overload: measured zero divergence between
  // `summary` and `description` across all of `planar-agent`'s nodes (see
  // `planar.cmd.planar_agent.surface`'s header and `planar.cliapp.schema`'s
  // header, divergence 1), so there is nothing for a summary table to
  // supply that `description` does not already carry.
  ctx.out() << cliapp::schema_json(root) << '\n';
  return {};
}

} // namespace planar::cmd::agent::handlers
