/// @file main.cppm
/// @brief Assemble planar-agent root commands in their established order.
module;
export module planar.cmd.planar_agent.main;
import std;
import cli11;
import planar.cliapp.schema;
import planar.cliapp.surface;
import planar.cmd.planar_agent.docs;
import planar.cmd.planar_agent.handlers.version.command;
import planar.cmd.planar_agent.handlers.pull.command;
import planar.cmd.planar_agent.handlers.peek.command;
import planar.cmd.planar_agent.handlers.complete.command;
import planar.cmd.planar_agent.handlers.fail.command;
import planar.cmd.planar_agent.handlers.release.command;
import planar.cmd.planar_agent.handlers.block.command;
import planar.cmd.planar_agent.handlers.claim.command;
import planar.cmd.planar_agent.handlers.heartbeat.command;
import planar.cmd.planar_agent.handlers.claim_associate.command;
import planar.cmd.planar_agent.handlers.action.command;
import planar.cmd.planar_agent.handlers.ingest.command;
import planar.cmd.planar_agent.handlers.reconcile.command;
import planar.cmd.planar_agent.handlers.abort.command;
import planar.cmd.planar_agent.handlers.schema.command;
import planar.cmd.planar_agent.handlers.run.command;
import planar.cmd.planar_agent.handlers.dispatch.command;
import planar.cmd.planar_agent.handlers.context.command;
import planar.cmd.planar_agent.handlers.queue.command;

namespace planar::cmd::agent {
/// @brief Build the root CLI application.
/// @return Root CLI application.
export auto root_app() -> std::unique_ptr<CLI::App> {
  // Every description below is transcribed from the Zig node. The four
  // Declaration order matches the oracle's `handlers/cmd.zig` registry.
  auto app = std::make_unique<CLI::App>("Agent-callable coordination binary (pull / claim / complete / heartbeat / reconcile).\n"
                                        "\n"
                                        "  This binary writes agent_actions, agent_work_claims, the routing_dispatch_*\n"
                                        "  tables, and the host-queue tables (queue_entries, queue_history,\n"
                                        "  queue_schema). It changes tasks.status only as part of a coordinated\n"
                                        "  claim operation; planning entities are written by planar.",
                                        "planar-agent");
  app->require_subcommand(0);

  handlers::version_cli::add(*app);
  handlers::pull_cli::add(*app);
  handlers::peek_cli::add(*app);
  handlers::complete_cli::add(*app);
  handlers::fail_cli::add(*app);
  handlers::release_cli::add(*app);
  handlers::block_cli::add(*app);
  handlers::claim_cli::add(*app);
  handlers::heartbeat_cli::add(*app);
  handlers::claim_associate_cli::add(*app);
  handlers::action_cli::add(*app);
  handlers::ingest_cli::add(*app);
  handlers::reconcile_cli::add(*app);
  handlers::abort_cli::add(*app);
  handlers::schema_cli::add(*app);
  handlers::run_cli::add(*app);
  handlers::dispatch_cli::add(*app);
  handlers::context_cli::add(*app);
  handlers::queue_cli::add(*app);

  // Help renders the same page it rendered before every bool flag gained
  // its `--no-X` negation — see `planar.cliapp.surface::hide_negations_in_help`.
  // Must come AFTER the whole tree exists.
  cliapp::hide_negations_in_help(*app);
  cliapp::install_docs_footers(*app, surface_docs());
  return app;
}

/// @brief List verbs excluded from this binary.
/// @return Computed value.
export auto forbidden_verbs() -> std::vector<std::string_view> {
  return {"plan", "task",      "decision", "question", "scenario", "artifact", "annotate", "init",      "workbench", "doc",
          "spec", "templates", "ext",      "sync",     "promote",  "demote",   "capture",  "dashboard", "tree",      "health"};
}

} // namespace planar::cmd::agent
