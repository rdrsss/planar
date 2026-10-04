/// @file add.cppm
/// @brief CLI declaration for `decision add`.
export module planar.cmd.planar.handlers.decision.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
/// @brief Register the add CLI node.
/// @param decision Input decision.
/// @return Registered CLI node.
export auto attach_add(CLI::App& decision) -> CLI::App* {
  CLI::App* add = decision.add_subcommand("add", "Create a new decision record.");
  add_string(*add, "--body", "Decision body text; a leading @ reads it from a file");
  add_string(*add, "--rationale", "Why the decision was made; a leading @ reads it from a file");
  add_int(*add, "--plan", "Plan id to attach the decision to");
  add_string(*add, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_bool(*add, "--editor", "Accepted but not implemented; without --body it warns and the verb still refuses");
  add_json(*add, "Emit machine-readable JSON instead of text");
  add_positional(*add, "title", "Title of the new decision");
  return add;
}
} // namespace planar::cmd::handlers::decision_cli
