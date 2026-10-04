/// @file add.cppm
/// @brief CLI declaration for `artifact add`.
export module planar.cmd.planar.handlers.artifact.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the add CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_add(CLI::App& artifact) -> CLI::App* {
  CLI::App* add = artifact.add_subcommand("add", "Register a new artifact.");
  add_string(*add, "--body", k_undocumented);
  add_string_required(*add, "--kind", k_undocumented);
  add_string(*add, "--from-file", k_undocumented);
  add_string(*add, "--source-path", k_undocumented);
  add_string(*add, "--scope", k_undocumented);
  add_string_default(*add, "--status", "draft", k_undocumented);
  add_int(*add, "--plan", k_undocumented);
  add_bool_default_true(*add, "--editor", k_undocumented);
  add_json(*add, k_undocumented);
  add_positional(*add, "title", k_undocumented);
  return add;
}
} // namespace planar::cmd::handlers::artifact_cli
