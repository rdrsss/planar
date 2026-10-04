/// @file command.cppm
/// @brief CLI declarations for planar-agent pull.
module;
export module planar.cmd.planar_agent.handlers.pull.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::pull_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- pull ---------------------------------------------------------------
  CLI::App* pull = root.add_subcommand("pull", "Atomically pick the next eligible task, claim it, and flip status to doing.");
  shared::add_vendor(*pull, "Vendor tag (default: planar-agent)", "Vendor session id (e.g. claude:s1)");
  pull->add_option("--role")->description("Role name (planner|coder|reviewer|test_coder|...)");
  shared::add_ttl(*pull, "Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)");
  pull->add_option("--purpose")->description("Free-text purpose recorded on the claim");
  pull->add_option("--base-ref")->description("Git ref the work is based on");
  pull->add_option("--worktree")->description("Worktree id or path for isolation context");
  pull->add_option("--repo-root")->description("Absolute path of checkout to probe locality against");
  shared::add_no_locality_probe(*pull, "Skip the git locality probe");
  pull->add_option("--metadata")
      ->description("Opaque text (typically JSON) persisted on the dispatch action row; validated as well-formed "
                    "JSON when supplied");
  pull->add_option("--parent-action")
      ->description("Parent action id; wires the new action as a child of this action in `planar-watch tree` "
                    "(cross-session hierarchy)")
      ->check(cliapp::zig_int_validator());
  shared::add_run_stage(*pull);
  shared::add_json(*pull, shared::k_undocumented);
  pull->add_option("plan-id")->description("Plan id to pull from")->required()->check(cliapp::zig_int_validator());
}
} // namespace planar::cmd::agent::handlers::pull_cli
