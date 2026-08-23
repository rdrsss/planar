/// @file version.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.version`.

module planar.cmd.planar.handlers.version;

import std;
import cli11;
import planar.cliapp.version;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

auto version(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  (void)args;
  // `render_version_text` returns the complete stdout payload, trailing
  // newline included. Written verbatim; nothing is appended.
  ctx.out() << cliapp::render_version_text(cliapp::current_build_info(), cliapp::compiler_version_string());
  return {};
}

} // namespace planar::cmd::handlers
