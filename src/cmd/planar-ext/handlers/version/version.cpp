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
  auto const info     = cliapp::current_build_info();
  auto const compiler = cliapp::compiler_version_string();
  // Complete stdout payload, trailing newline included. Written verbatim.
  ctx.out() << (cliapp::flag_bool(args, "--json") ? cliapp::render_version_json(info, compiler)
                                                  : cliapp::render_version_text("planar-ext", info, compiler));
  return {};
}

} // namespace planar::cmd::ext::handlers
