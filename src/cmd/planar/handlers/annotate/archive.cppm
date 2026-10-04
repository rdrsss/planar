/// @file archive.cppm
/// @brief CLI declaration for `annotate archive`.
export module planar.cmd.planar.handlers.annotate.archive;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the archive CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_archive(CLI::App& annotate) -> CLI::App* {
  CLI::App* archive = annotate.add_subcommand("archive", "Archive an annotation.");
  add_json(*archive, k_undocumented);
  add_positional(*archive, "annotation-id", k_undocumented);
  return archive;
}
} // namespace planar::cmd::handlers::annotate_cli
