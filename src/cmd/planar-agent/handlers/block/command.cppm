/// @file command.cppm
/// @brief CLI declarations for planar-agent block.
module;
export module planar.cmd.planar_agent.handlers.block.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::block_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* block = root.add_subcommand("block", "Atomically park the task on an external blocker.");
  shared::add_claim(*block, "Claim token returned by pull/claim");
  block->add_option("--blocker")
      ->description("Task id of the blocker (entity_links target)")
      ->required()
      ->check(cliapp::zig_int_validator());
  block->add_option("--reason")->description("Free-text reason recorded on the claim");
  shared::add_no_locality_probe(*block, "Skip the git locality probe and commit collection");
  shared::add_supervision(*block, true);
  shared::add_json(*block, "Emit machine-readable JSON instead of text");
}
} // namespace planar::cmd::agent::handlers::block_cli
