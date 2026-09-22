/// @file ingest.cppm
/// @brief CLI declaration for `spec ingest`.
export module planar.cmd.planar.handlers.spec.ingest;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::spec_cli {
export auto attach_ingest(CLI::App* spec) -> CLI::App* {
  CLI::App* ingest = spec->add_subcommand("ingest", "Decompose workbench spec documents into the task graph.");
  add_bool(*ingest, "--apply");
  add_bool(*ingest, "--apply-removals");
  add_string_default(*ingest, "--format", "text");
  add_string(*ingest, "--scope");
  add_bool(*ingest, "--strict");
  add_json(*ingest);
  add_positional(*ingest, "plan");
  // Hidden variadic "rest" positional -- see this function's header.
  ingest->add_option("extra-plans")->expected(0, -1)->group("");
  return ingest;
}
} // namespace planar::cmd::handlers::spec_cli
