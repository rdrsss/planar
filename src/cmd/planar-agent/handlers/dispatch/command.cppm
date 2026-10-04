/// @file command.cppm
/// @brief CLI declarations for planar-agent dispatch.
module;
export module planar.cmd.planar_agent.handlers.dispatch.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::dispatch_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- dispatch preview / confirm -------------------------------------------
  CLI::App* dispatch = root.add_subcommand(
      "dispatch", "Routing dispatch authorization (preview / confirm). Binds current state to a single-use token.");
  dispatch->require_subcommand(0);
  CLI::App* dispatch_preview =
      dispatch->add_subcommand("preview", "Bind current routing state to a single-use, expiry-bound confirmation token.");
  dispatch_preview->add_option("--task")->description("Task id this dispatch targets");
  dispatch_preview->add_option("--work-item")->description("Logical work item id")->required();
  dispatch_preview->add_option("--project")->description("Project id")->required();
  dispatch_preview->add_option("--validation-policy")->description("Validation policy version")->required();
  dispatch_preview->add_option("--routing-policy")->description("Routing policy version")->required();
  dispatch_preview->add_option("--profile-rule")->description("Profile rule version")->required();
  dispatch_preview->add_option("--vendor")->description("Vendor (opaque)")->required();
  dispatch_preview->add_option("--role")->description("Role (opaque)")->required();
  dispatch_preview->add_option("--tier")->description("small|medium|large")->required();
  dispatch_preview->add_option("--work-type")->description("schema|engine|architectural|cli|feature|mechanical")->required();
  dispatch_preview->add_option("--complexity")->description("bounded|standard|high-risk")->required();
  dispatch_preview->add_option("--packet-digest")->description("Digest of the authoritative packet")->required();
  dispatch_preview->add_option("--profile-digest")->description("Digest of the compiled profile")->required();
  dispatch_preview->add_option("--policy-digest")->description("Digest of the policy snapshot")->required();
  dispatch_preview->add_option("--capability-digest")->description("Digest of the host capability snapshot")->required();
  dispatch_preview->add_option("--candidate")->description("Requested candidate row id")->required();
  dispatch_preview->add_option("--host")->description("Host id whose capability snapshot was consulted")->required();
  dispatch_preview->add_option("--class")->description("fallback|default|override|declared_experiment")->required();
  dispatch_preview->add_option("--experiment")->description("Experiment id (required for declared_experiment)");
  dispatch_preview->add_option("--claim")->description("Claim token this dispatch is bound to");
  dispatch_preview->add_option("--claim-status")->description("Claim status observed at preview time");
  dispatch_preview->add_option("--evidence-state")->description("evidential|observational")->required();
  dispatch_preview->add_option("--expires-at")->description("RFC3339 instant after which the token is dead")->required();
  shared::add_json(*dispatch_preview, shared::k_undocumented);

  CLI::App* dispatch_confirm = dispatch->add_subcommand(
      "confirm", "Revalidate a preview token against current state and atomically write the dispatch snapshot.");
  dispatch_confirm->add_option("--token")->description("Preview token to spend")->required();
  dispatch_confirm->add_option("--dispatch-key")->description("Unique key for the resulting dispatch")->required();
  dispatch_confirm->add_option("--now")->description("RFC3339 instant to evaluate expiry against")->required();
  dispatch_confirm->add_option("--packet-digest")->description("Currently observed packet digest")->required();
  dispatch_confirm->add_option("--profile-digest")->description("Currently observed profile digest")->required();
  dispatch_confirm->add_option("--policy-digest")->description("Currently observed policy digest")->required();
  dispatch_confirm->add_option("--capability-digest")->description("Currently observed capability digest")->required();
  dispatch_confirm->add_option("--candidate")->description("Currently resolved candidate row id")->required();
  dispatch_confirm->add_option("--vendor")->description("Currently resolved vendor")->required();
  dispatch_confirm->add_option("--role")->description("Currently resolved role")->required();
  dispatch_confirm->add_option("--tier")->description("Currently resolved tier")->required();
  dispatch_confirm->add_option("--work-type")->description("Currently resolved work type")->required();
  dispatch_confirm->add_option("--complexity")->description("Currently resolved complexity")->required();
  dispatch_confirm->add_option("--validation-policy")->description("Currently active validation policy version")->required();
  dispatch_confirm->add_option("--routing-policy")->description("Currently active routing policy version")->required();
  dispatch_confirm->add_option("--claim")->description("Currently held claim token");
  dispatch_confirm->add_option("--claim-status")->description("Currently observed claim status");
  dispatch_confirm->add_option("--reviewer")->description("Reviewer disposition to record (default required)");
  dispatch_confirm->add_option("--decision")->description("confirmed|overridden (default confirmed)");
  shared::add_json(*dispatch_confirm, shared::k_undocumented);
}
} // namespace planar::cmd::agent::handlers::dispatch_cli
