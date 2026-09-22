/// @file outcomes.cppm
/// @brief CLI declaration for `models outcomes`.
export module planar.cmd.planar.handlers.models.outcomes;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
export auto attach_outcomes(CLI::App& models) -> CLI::App* {
  CLI::App* outcomes = models.add_subcommand(
      "outcomes", "Read-only. Excluded samples are shown deliberately: they are the\n  audit trail of the evidence boundary. "
                  "Hiding them would make the\n  evidence look thinner than it is and leave no way to check the\n  boundary "
                  "was applied correctly, and showing them without a named\n  reason would look like a bug.");
  add_string(*outcomes, "--limit", "Maximum rows to show (default 50)");
  add_json(*outcomes);
  return outcomes;
}
} // namespace planar::cmd::handlers::models_cli
