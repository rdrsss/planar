/// @file version.cpp
/// @brief Implementation of `planar.cmd.planar_ext.handlers.version`.

module planar.cmd.planar_ext.handlers.version;

import std;
import cli11;
import planar.cliapp.version;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.handler;

namespace planar::cmd::ext::handlers {

auto version(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  (void)args;
  // Complete stdout payload, trailing newline included. Written verbatim.
  ctx.out() << cliapp::render_version_text("planar-ext", cliapp::current_build_info(), cliapp::compiler_version_string());
  return {};
}

} // namespace planar::cmd::ext::handlers
