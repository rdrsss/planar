/// @file command.cppm
/// @brief CLI declaration for `planar bench`.
export module planar.cmd.planar.handlers.bench.command;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.bench.start;
import planar.cmd.planar.handlers.bench.event;
import planar.cmd.planar.handlers.bench.touch;
import planar.cmd.planar.handlers.bench.harvest;
import planar.cmd.planar.handlers.bench.finish;
import planar.cmd.planar.handlers.bench.show;

namespace planar::cmd::handlers {
export auto declare_bench(CLI::App& root) -> void {
  CLI::App* bench = root.add_subcommand(
      "bench", "Record measurement-rig data for the vertical-slice decomposition experiment.\n\n  Arms: strict, eligibility, "
               "grouped (or any free-text pilot value).\n  Statuses: running, completed, aborted, error.\n  Touch kinds: "
               "declared, actual.\n\n  Workflow: bench start → bench event (repeat) → bench touch (repeat)\n            → bench "
               "harvest → bench finish → bench show --json.");
  bench->require_subcommand(0);

  bench_cli::attach_start(bench);

  bench_cli::attach_event(bench);

  bench_cli::attach_touch(bench);

  bench_cli::attach_harvest(bench);

  bench_cli::attach_finish(bench);

  bench_cli::attach_show(bench);
}
} // namespace planar::cmd::handlers
