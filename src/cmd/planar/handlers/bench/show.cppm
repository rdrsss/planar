/// @file show.cppm
/// @brief CLI declaration for `planar bench show`.
export module planar.cmd.planar.handlers.bench.show;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::bench_cli {
/// @brief Register the show CLI node.
/// @param bench Input bench.
/// @return Registered CLI node.
export auto attach_show(CLI::App* bench) -> CLI::App* {
  CLI::App* show = bench->add_subcommand("show", "Show a run's full state (header + events + touches).");
  add_json(*show, k_undocumented);
  add_positional(*show, "run-uid", k_undocumented);
  return show;
}
} // namespace planar::cmd::handlers::bench_cli
