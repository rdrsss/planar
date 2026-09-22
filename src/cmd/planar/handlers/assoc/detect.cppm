/// @file detect.cppm
/// @brief CLI declaration for `assoc detect`.
export module planar.cmd.planar.handlers.assoc.detect;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::assoc_cli {
export auto attach_detect(CLI::App* assoc) -> CLI::App* {
  CLI::App* detect = assoc->add_subcommand("detect", "Propose (or apply) auto-detected associations for the current directory.");
  add_bool(*detect, "--apply");
  add_json(*detect);
  return detect;
}
} // namespace planar::cmd::handlers::assoc_cli
