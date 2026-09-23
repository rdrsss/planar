/// @file command.cppm
/// @brief CLI declarations for planar-agent ingest.
module;
export module planar.cmd.planar_agent.handlers.ingest.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::ingest_cli {
export auto add(CLI::App& root) -> void {
  // --- ingest ---------------------------------------------------------------
  CLI::App* ingest = root.add_subcommand(
      "ingest", "Translate a vendor hook event into store primitives (claude + copilot adapters wired; codex reserved).");
  ingest->add_option("--vendor")->description("Vendor tag (claude|copilot wired; codex reserved)")->required();
  ingest->add_option("--event")->description("Event JSON: @<file> reads from path; @- reads from stdin")->required();
  shared::add_json(*ingest);
}
} // namespace planar::cmd::agent::handlers::ingest_cli
