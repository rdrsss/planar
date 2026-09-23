/// @file version.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.version`.

module planar.cmd.planar_watch.handlers.version;

import std;
import cli11;
import planar.cliapp.version;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

auto version(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  (void)args;
  // Complete stdout payload, trailing newline included. Written verbatim.
  ctx.out() << cliapp::render_version_text("planar-watch", cliapp::current_build_info(), cliapp::compiler_version_string());
  return {};
}

} // namespace planar::cmd::watch::handlers
