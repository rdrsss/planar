/// @file resolve.cppm
/// @brief CLI declaration for `models resolve`.
export module planar.cmd.planar.handlers.models.resolve;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
export auto attach_resolve(CLI::App& models) -> CLI::App* {
  CLI::App* resolve = models.add_subcommand(
      "resolve",
      "Read-only. Answers \"what tier should this role run at, and is that\n  answer backed by anything?\". Task-bound "
      "roles (coder, test-coder,\n  reviewer, research, janitor) resolve from the task's compiled profile;\n  pre-task "
      "roles (planner, spec-reviewer, ingestor, orchestrator) resolve\n  from a planning packet, which establishes "
      "readiness but classifies no\n  work because no unit of work exists yet.\n\n  When the authoritative packet is absent "
      "or unready the result reports\n  the configured static fallback AND the reason, and never a derived work\n  type — a "
      "tier shown without provenance reads identically to one derived\n  from real evidence.");
  add_string_required(*resolve, "--role",
                      "planner|spec-reviewer|ingestor|orchestrator|coder|test-coder|reviewer|research|janitor");
  add_string(*resolve, "--task", "Task id (required for task-bound roles)");
  add_string(*resolve, "--plan", "Anchor plan id (pre-task roles)");
  add_string(*resolve, "--fallback-tier", "Configured static fallback tier (default medium)");
  add_json(*resolve);
  return resolve;
}
} // namespace planar::cmd::handlers::models_cli
