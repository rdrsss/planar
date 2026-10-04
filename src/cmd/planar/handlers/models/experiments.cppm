/// @file experiments.cppm
/// @brief CLI declaration for `models experiments`.
export module planar.cmd.planar.handlers.models.experiments;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the experiments CLI node.
/// @param models Input models.
/// @return Registered CLI node.
export auto attach_experiments(CLI::App& models) -> CLI::App* {
  CLI::App* experiments = models.add_subcommand(
      "experiments", "Read-only. Shows each experiment's frozen manifest identity (the\n  cohort it governs, its manifest "
                     "digest, and when an operator approved\n  it) alongside how many terminal samples it has produced and how "
                     "many\n  of those count toward a recommendation. The two counts differ whenever\n  a run was recorded but "
                     "excluded; reporting only the eligible count\n  would understate what actually ran.");
  add_json(*experiments, k_undocumented);
  return experiments;
}
} // namespace planar::cmd::handlers::models_cli
