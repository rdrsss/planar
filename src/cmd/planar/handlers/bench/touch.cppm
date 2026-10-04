/// @file touch.cppm
/// @brief CLI declaration for `planar bench touch`.
export module planar.cmd.planar.handlers.bench.touch;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::bench_cli {
/// @brief Register the touch CLI node.
/// @param bench Input bench.
/// @return Registered CLI node.
export auto attach_touch(CLI::App* bench) -> CLI::App* {
  CLI::App* touch = bench->add_subcommand("touch", "Record a declared or actual file touch for a run.");
  add_int_required(*touch, "--task", k_undocumented);
  add_string_required(*touch, "--path", k_undocumented);
  add_string_required(*touch, "--kind", k_undocumented);
  add_positional(*touch, "run-uid", k_undocumented);
  return touch;
}
} // namespace planar::cmd::handlers::bench_cli
