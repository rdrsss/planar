/// @file hygiene.cppm
/// @brief CLI declaration for `health hygiene`.
export module planar.cmd.planar.handlers.health.hygiene;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::health_cli {
/// @brief Register the hygiene CLI node.
/// @param health Input health.
/// @return Registered CLI node.
export auto attach_hygiene(CLI::App* health) -> CLI::App* {
  CLI::App* hygiene = health->add_subcommand(
      "hygiene", "Find draft plans with zero tasks or only terminal tasks, tasks\n  left doing beyond a threshold, and questions "
                 "left open beyond a\n  threshold. Suggested repair commands are reported but never run.\n\n  This reporter "
                 "always exits 0 when the report is produced, even when\n  findings are present.");
  add_string(*hygiene, "--scope", "Limit findings to one association slug");
  add_int_default(*hygiene, "--stale-doing", "7", "Doing-task age threshold in days");
  add_int_default(*hygiene, "--stale-open", "30", "Open-question age threshold in days");
  // Inherited from `health`, and LAST — see this function's header.
  add_json(*hygiene);
  return hygiene;
}
} // namespace planar::cmd::handlers::health_cli
