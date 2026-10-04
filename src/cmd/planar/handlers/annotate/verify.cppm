/// @file verify.cppm
/// @brief CLI declaration for `annotate verify`.
export module planar.cmd.planar.handlers.annotate.verify;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the verify CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_verify(CLI::App& annotate) -> CLI::App* {
  CLI::App* verify = annotate.add_subcommand("verify", "Verify annotation anchors against workspace state.");
  add_string(*verify, "--anchor-path", "Verify only annotations anchored to this path");
  add_string(*verify, "--scope", "Restrict to this scope slug");
  add_json(*verify, "Emit machine-readable JSON instead of text");
  return verify;
}
} // namespace planar::cmd::handlers::annotate_cli
