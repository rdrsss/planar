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
import planar.cmd.planar.declare;

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

/// @brief Declare the `skills` leaf.
///
/// MOVED HERE FROM `tree.cpp` at M11.3f. Zero subcommands and zero
/// flags, which is the whole point: plan 918 M5 retired
/// `render`/`status`/`repair` and left the node registered so the verb
/// reports "no subcommands" rather than an unknown-verb error. With no
/// children it is a LEAF and needs a handler — see `skills` above.
/// @param root The root app to attach it to.
auto declare_skills(CLI::App& root) -> void {
  root.add_subcommand(
      "skills", "The unified skill source tree under skills/src/ is rendered by the\n  external scriptorium binary (plan 918). "
                "Planar no longer renders vendor\n  projections nor tracks their install-drift in-band; use `scriptorium\n  "
                "check`/`scriptorium status` instead. This command has no subcommands.");
}

} // namespace planar::cmd::handlers
