/// @file version.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.version`.

module planar.cmd.planar.handlers.version;

import std;
import cli11;
import planar.cliapp.version;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

auto version(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  (void)args;
  // `render_version_text` returns the complete stdout payload, trailing
  // newline included. Written verbatim; nothing is appended.
  ctx.out() << cliapp::render_version_text(cliapp::current_build_info(), cliapp::compiler_version_string());
  return {};
}

/// @brief Declare the `version` leaf.
///
/// MOVED HERE FROM `tree.cpp` at M11.3f, where it had been the very
/// first verb this binary ever declared (task 6105). Its `node_spec`
/// twin agreed with it exactly.
auto declare_version(CLI::App& root) -> void {
  root.add_subcommand("version", "Print the planar version, commit, and C++ toolchain.");
}

} // namespace planar::cmd::handlers
