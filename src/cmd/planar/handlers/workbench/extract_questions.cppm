/// @file extract_questions.cppm
/// @brief CLI declaration for `workbench extract-questions`.
export module planar.cmd.planar.handlers.workbench.extract_questions;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the extract questions CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_extract_questions(CLI::App& workbench) -> CLI::App* {
  CLI::App* extract_questions =
      workbench.add_subcommand("extract-questions", "Parse Open questions from top-level workbench specs (read-only).");
  add_json(*extract_questions, "Emit machine-readable JSON instead of text");
  add_positional(*extract_questions, "plan", "Anchor plan id or slug");
  return extract_questions;
}
} // namespace planar::cmd::handlers::workbench_cli
