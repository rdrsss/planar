/// @file command.cppm
/// @brief CLI declarations for planar-agent action.
module;
export module planar.cmd.planar_agent.handlers.action.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::action_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- action start / end -------------------------------------------------
  CLI::App* action = root.add_subcommand("action", "Nested action lifecycle (sub-tool-calls inside a claim).");
  action->require_subcommand(0);
  CLI::App* action_start = action->add_subcommand("start", "Start a nested action under a claim (child of the claim's role "
                                                           "action).");
  shared::add_claim(*action_start, "Claim token the action attaches to");
  action_start->add_option("--kind")->description("Action kind (planner|coder|tool_call|heartbeat|...)")->required();
  action_start->add_option("--entity")->description("Optional entity ref kind:id");
  action_start->add_option("--vendor-role")->description("Optional vendor role tag");
  action_start->add_option("--repo-root")->description("Absolute path of checkout to probe locality against");
  shared::add_no_locality_probe(*action_start, "Skip the git locality probe");
  action_start->add_option("--metadata")
      ->description("Opaque text (typically JSON) persisted on the action row; validated as well-formed JSON when supplied");
  shared::add_json(*action_start, "Emit machine-readable JSON instead of text");

  CLI::App* action_end = action->add_subcommand("end", "Close a nested action started under a claim.");
  action_end->add_option("--action")
      ->description("Action id returned by `action start`")
      ->required()
      ->check(cliapp::zig_int_validator());
  // A plain string with a hand-rolled validator in the handler, NOT a
  // choice — the oracle's declaration. Making it a choice would add the
  // value set to the help page and to the schema catalog.
  action_end->add_option("--outcome")->description("ok | error | aborted | timeout (default ok)")->default_str("ok");
  action_end->add_option("--summary")->description("Optional free-text summary recorded on the action");
  shared::add_json(*action_end, "Emit machine-readable JSON instead of text");
}
} // namespace planar::cmd::agent::handlers::action_cli
