/// @file command.cppm
/// @brief CLI declarations for planar-agent claim.
module;
export module planar.cmd.planar_agent.handlers.claim.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::claim_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- claim --------------------------------------------------------------
  CLI::App* claim =
      root.add_subcommand("claim", "Direct entity claim; task claims atomically transition todo to doing by default.");
  claim->add_option("--entity")->description("Entity ref: task:<id> | plan:<id> | plan_step:<id>")->required();
  shared::add_vendor(*claim, "Vendor tag (default: planar-agent)", "Vendor session id (e.g. claude:s1)");
  claim->add_option("--role")->description("Role name (planner|coder|reviewer|test_coder|...)");
  claim->add_option("--model")->description(
      "Model actually used, recorded verbatim as an opaque string. Never validated against a supported list.");
  shared::add_ttl(*claim, "Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)");
  claim->add_option("--purpose")->description("Free-text purpose recorded on the claim");
  claim->add_option("--worktree")->description("Worktree id or path for isolation context");
  claim->add_option("--repo-root")->description("Absolute path of checkout to probe locality against");
  shared::add_no_locality_probe(*claim, "Skip the git locality probe");
  cliapp::add_bool_flag(*claim, "--no-transition",
                        "Claim without changing task status (plan and plan_step are always unchanged)");
  cliapp::add_bool_flag(*claim, "--force", "Take over an existing live claim (operator recovery)");
  shared::add_run_stage(*claim);
  shared::add_json(*claim, shared::k_undocumented);
}
} // namespace planar::cmd::agent::handlers::claim_cli
