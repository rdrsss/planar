/// @file version.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.version`.

module planar.cmd.planar_agent.handlers.version;

import std;
import cli11;
import planar.cliapp.version;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

auto version(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  (void)args;
  // `render_version_text` returns the complete stdout payload, trailing
  // newline included. Written verbatim; nothing is appended.
  ctx.out() << cliapp::render_version_text("planar-agent", cliapp::current_build_info(), cliapp::compiler_version_string());
  return {};
}

} // namespace planar::cmd::agent::handlers
