/// @file command.cppm
/// @brief CLI declaration for `planar promote`.
export module planar.cmd.planar.handlers.promote.command;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {
export auto declare_promote(CLI::App& root) -> void {
  CLI::App* promote = root.add_subcommand(
      "promote", "Promote an entity from its current scope to a named association.\n\n  Valid entity kinds: plan, task, "
                 "question, test_scenario (alias:\n  scenario), artifact, decision.\n\n  Examples:\n    planar promote task:42 "
                 "--to org:acme\n    planar promote plan:7 --to project:planar");
  add_string_required(*promote, "--to", "Target association slug");
  add_json(*promote);
  add_positional_described(*promote, "ref", "Entity ref (kind:id)");
}
} // namespace planar::cmd::handlers
