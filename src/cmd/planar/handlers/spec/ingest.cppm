/// @file ingest.cppm
/// @brief CLI declaration for `spec ingest`.
export module planar.cmd.planar.handlers.spec.ingest;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::spec_cli {
/// @brief Register the ingest CLI node.
/// @param spec Input spec.
/// @return Registered CLI node.
export auto attach_ingest(CLI::App* spec) -> CLI::App* {
  CLI::App* ingest = spec->add_subcommand("ingest", "Decompose workbench spec documents into the task graph.");
  add_bool(*ingest, "--apply", "Commit the proposed changes; without it, preview only");
  add_bool(*ingest, "--apply-removals",
           "Also commit proposed removals (cancel orphan tasks, abandon orphan plans); requires --apply");
  add_string_default(*ingest, "--format", "text", "Output format: text, json (default: text)");
  add_string(*ingest, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_bool(*ingest, "--strict", "Reject the ingest when scenario coverage is incomplete or a task slug collides");
  add_json(*ingest, "Emit machine-readable JSON instead of text");
  add_positional(*ingest, "plan", "Plan id or slug to ingest the workbench specs into");
  // Hidden variadic "rest" positional -- see this function's header.
  ingest->add_option("extra-plans")->expected(0, -1)->group("");
  return ingest;
}
} // namespace planar::cmd::handlers::spec_cli
