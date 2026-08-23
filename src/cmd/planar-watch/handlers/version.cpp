/// @file version.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.version`.

module planar.cmd.planar_watch.handlers.version;

import std;
import planar.cli;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

auto version(context& ctx, const cli::match_result& args) -> handler_result {
  (void)args;
  // Complete stdout payload, trailing newline included. Written verbatim.
  ctx.out() << cli::render_version_text("planar-watch", cli::current_build_info(), cli::compiler_version_string());
  return {};
}

} // namespace planar::cmd::watch::handlers
