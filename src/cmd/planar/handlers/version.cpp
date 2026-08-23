/// @file version.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.version`.

module planar.cmd.planar.handlers.version;

import std;
import planar.cli;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

auto version(context& ctx, const cli::match_result& args) -> handler_result {
  (void)args;
  // `render_version_text` returns the complete stdout payload, trailing
  // newline included. Written verbatim; nothing is appended.
  ctx.out() << cli::render_version_text(cli::current_build_info(), cli::compiler_version_string());
  return {};
}

} // namespace planar::cmd::handlers
