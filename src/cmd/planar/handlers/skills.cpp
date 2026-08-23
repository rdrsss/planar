/// @file skills.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.skills`.

module planar.cmd.planar.handlers.skills;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.walk;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import planar.cmd.planar.tree;

namespace planar::cmd::handlers {

auto skills(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  static_cast<void>(args);
  // `CLI::App::help()` returns the COMPLETE page, terminator included —
  // the same contract `dispatch::run` already relies on for `--help`.
  // Nothing is appended here.
  auto const      root = root_app();
  const CLI::App* node = cliapp::find_node(*root, std::array<std::string, 1>{"skills"});
  ctx.out() << (node != nullptr ? node->help() : root->help());
  return {};
}

} // namespace planar::cmd::handlers
