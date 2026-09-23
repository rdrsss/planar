/// @file receipt.cppm
/// @brief CLI declaration for `annotate receipt`.
export module planar.cmd.planar.handlers.annotate.receipt;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the receipt CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_receipt(CLI::App& annotate) -> CLI::App* {
  CLI::App* receipt = annotate.add_subcommand("receipt", "Look up a durable annotation command receipt.");
  add_string(*receipt, "--source-uuid");
  add_string(*receipt, "--operation-id");
  add_json(*receipt);
  return receipt;
}
} // namespace planar::cmd::handlers::annotate_cli
