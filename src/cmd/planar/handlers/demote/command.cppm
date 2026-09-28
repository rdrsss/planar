/// @file command.cppm
/// @brief CLI declaration for `planar demote`.
export module planar.cmd.planar.handlers.demote.command;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {
/// @brief Provide the declare demote command operation.
/// @param root Input root.
export auto declare_demote(CLI::App& root) -> void {
  CLI::App* demote = root.add_subcommand(
      "demote", "Reverse a promotion — move an entity back to global personal scope.\n\n  The destination is always global; the "
                "optional --from flag names the\n  source association slug for clarity. Association-to-association\n  "
                "transitions go through promote.\n\n  Example:\n    planar demote task:42 --from project:planar");
  add_string(*demote, "--from", "Source association slug");
  add_json(*demote);
  add_positional_described(*demote, "ref", "Entity ref (kind:id)");
}
} // namespace planar::cmd::handlers
