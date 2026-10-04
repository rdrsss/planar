/// @file command.cppm
/// @brief CLI declarations for planar-agent abort.
module;
export module planar.cmd.planar_agent.handlers.abort.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::abort_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* abort_cmd =
      root.add_subcommand("abort", "Operator force-release of a stuck claim (any session, not just the owner).");
  shared::add_claim(*abort_cmd, "Claim token to force-release");
  abort_cmd->add_option("--reason")->description("Optional reason recorded on the claim and audit row");
  // No default here, unlike `fail`'s "unknown" — an abort records a
  // category only when the operator names one.
  abort_cmd->add_option("--category")
      ->description("Optional closed failure category for the recovered claim")
      ->check(CLI::IsMember{shared::failure_categories()});
  shared::add_vendor(*abort_cmd, "Vendor tag for the aborting session", "Vendor session id for the aborting session");
  cliapp::add_bool_flag(*abort_cmd, "--override-supervisor",
                        "Abort an engine-supervised claim (refused otherwise; logged as supervisor_override)");
  shared::add_json(*abort_cmd, shared::k_undocumented);
}
} // namespace planar::cmd::agent::handlers::abort_cli
