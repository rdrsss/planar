/// @file command.cppm
/// @brief CLI declarations for planar-agent context.
module;
export module planar.cmd.planar_agent.handlers.context.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::context_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- context add / capsule / list / resolve -------------------------------
  CLI::App* context = root.add_subcommand(
      "context", "Run-scoped working-memory records (add / list / resolve / capsule). Used by an external workflow harness.");
  context->require_subcommand(0);
  CLI::App* context_add =
      context->add_subcommand("add", "Write a context_records row stamped from the claim's run_id, stage, session_id.");
  context_add->add_option("--claim")->description("Claim token that owns this context record")->required();
  context_add->add_option("--kind")->description("Record kind: finding|risk|artifact|followup|summary|capsule")->required();
  context_add->add_option("--body")->description("Record body text")->required();
  context_add->add_option("--compiled-from")
      ->description("Comma-separated context_record ids this capsule was compiled from (capsule kind only)");
  shared::add_json(*context_add, "Emit machine-readable JSON instead of text");

  CLI::App* context_capsule = context->add_subcommand(
      "capsule", "Write a compiled capsule context_records row, run-keyed (claim_id=NULL, decision 456).");
  context_capsule->add_option("--run")->description("workflow_runs.id \xE2\x80\x94 the run that owns this capsule")->required();
  context_capsule->add_option("--stage")->description("Stage name this capsule distills (e.g. 'plan', 'code')")->required();
  context_capsule->add_option("--body")->description("Compiled capsule body text")->required();
  context_capsule->add_option("--compiled-from")
      ->description("Comma-separated context_record ids this capsule distills (provenance)");
  context_capsule->add_option("--session")
      ->description("session_id (integer). Optional \xE2\x80\x94 an ephemeral session is created when omitted.");
  shared::add_json(*context_capsule, "Emit machine-readable JSON instead of text");

  CLI::App* context_list =
      context->add_subcommand("list", "List context_records for a run, with optional stage/status/kind filters.");
  context_list->add_option("--run")->description("Run id (integer) to query")->required();
  context_list->add_option("--stage")->description("Filter to records from this stage");
  context_list->add_option("--status")->description("Filter by status: active|consumed|superseded");
  context_list->add_option("--kind")->description("Filter by kind: finding|risk|artifact|followup|summary|capsule");
  shared::add_json(*context_list, "Emit machine-readable JSON instead of text");

  CLI::App* context_resolve = context->add_subcommand(
      "resolve", "Transition context_records active \xE2\x86\x92 consumed|superseded (single record or bulk stage sweep).");
  context_resolve->add_option("--id")->description("Single record id to transition");
  context_resolve->add_option("--run")->description("Run id for bulk stage sweep (use with --stage)");
  context_resolve->add_option("--stage")->description("Stage name for bulk sweep (use with --run)");
  context_resolve->add_option("--status")->description("Target status: consumed|superseded")->required();
  shared::add_json(*context_resolve, "Emit machine-readable JSON instead of text");
}
} // namespace planar::cmd::agent::handlers::context_cli
