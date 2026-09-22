/// @file facts_stage.cppm
/// @brief CLI declaration for `task facts stage`.
export module planar.cmd.planar.handlers.task.facts_stage;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_facts_stage(CLI::App* facts) -> CLI::App* {
  CLI::App* facts_stage = facts->add_subcommand(
      "stage", "Stage this task's routing facts under operator provenance.\n\n  `spec ingest --apply` is the only other "
               "writer of routing facts, and it\n  rebuilds an entire anchor plan, so a hand-filed task could never obtain\n"
               "  them and an edited task could never restage them. This stages exactly\n  one task, stamped `operator-v1`, "
               "and never touches a sibling's facts.\n\n  Citation facts are staged only for artifacts this task already "
               "cites\n  AND references explicitly in its body; it never invents a citation.");
  add_json(*facts_stage);
  add_positional(*facts_stage, "task-id");
  return facts_stage;
}
} // namespace planar::cmd::handlers::task_cli
