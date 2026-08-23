/// @file skills.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.skills`.

module planar.cmd.planar.handlers.skills;

import std;
import planar.cli;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import planar.cmd.planar.tree;

namespace planar::cmd::handlers {

auto skills(context& ctx, const cli::match_result& args) -> handler_result {
  static_cast<void>(args);
  // `render_help` returns the COMPLETE page, terminator included — the
  // same contract `dispatch::run` already relies on for `--help`. Nothing
  // is appended here.
  std::vector<std::string> const path{"skills"};
  ctx.out() << cli::render_help(root_command(), path);
  return {};
}

} // namespace planar::cmd::handlers
