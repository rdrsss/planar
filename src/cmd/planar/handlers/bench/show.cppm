/// @file show.cppm
/// @brief CLI declaration for `planar bench show`.
export module planar.cmd.planar.handlers.bench.show;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::bench_cli {
export auto attach_show(CLI::App* bench) -> CLI::App* {
  CLI::App* show = bench->add_subcommand("show", "Show a run's full state (header + events + touches).");
  add_json(*show);
  add_positional(*show, "run-uid");
  return show;
}
} // namespace planar::cmd::handlers::bench_cli
