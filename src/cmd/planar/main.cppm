/// @file main.cppm
/// @brief Assemble the `planar` command tree in authoritative catalog order.
export module planar.cmd.planar.main;

import std;
import cli11;
import planar.cliapp.surface;
import planar.cmd.planar.handlers.explore.command;
import planar.cmd.planar.handlers.annotate;
import planar.cmd.planar.handlers.artifact;
import planar.cmd.planar.handlers.document;
import planar.cmd.planar.handlers.assoc;
import planar.cmd.planar.handlers.audit;
import planar.cmd.planar.handlers.capture;
import planar.cmd.planar.handlers.completion.command;
import planar.cmd.planar.handlers.schema.command;
import planar.cmd.planar.handlers.closure;
import planar.cmd.planar.handlers.config;
import planar.cmd.planar.handlers.dashboard;
import planar.cmd.planar.handlers.decision;
import planar.cmd.planar.handlers.feedback;
import planar.cmd.planar.handlers.groups;
import planar.cmd.planar.handlers.handoff;
import planar.cmd.planar.handlers.health;
import planar.cmd.planar.handlers.importer;
import planar.cmd.planar.handlers.init;
import planar.cmd.planar.handlers.link;
import planar.cmd.planar.handlers.links;
import planar.cmd.planar.handlers.local;
import planar.cmd.planar.handlers.models;
import planar.cmd.planar.handlers.plan;
import planar.cmd.planar.handlers.promote.command;
import planar.cmd.planar.handlers.demote.command;
import planar.cmd.planar.handlers.question;
import planar.cmd.planar.handlers.report;
import planar.cmd.planar.handlers.resume;
import planar.cmd.planar.handlers.bench.command;
import planar.cmd.planar.handlers.run.command;
import planar.cmd.planar.handlers.scenario;
import planar.cmd.planar.handlers.scope;
import planar.cmd.planar.handlers.search;
import planar.cmd.planar.handlers.skills;
import planar.cmd.planar.handlers.spec_ingest;
import planar.cmd.planar.handlers.synthesize;
import planar.cmd.planar.handlers.task;
import planar.cmd.planar.handlers.templates;
import planar.cmd.planar.handlers.test_spec;
import planar.cmd.planar.handlers.tree;
import planar.cmd.planar.handlers.unlink;
import planar.cmd.planar.handlers.version;
import planar.cmd.planar.handlers.workbench;
import planar.cmd.planar.handlers.workflow;
import planar.cmd.planar.handlers.workspace;

namespace planar::cmd {

/// @brief Build the root CLI application.
/// @return Root CLI application.
export auto root_app() -> std::unique_ptr<CLI::App>;

auto root_app() -> std::unique_ptr<CLI::App> {
  auto app = std::make_unique<CLI::App>("Planning + agent operations CLI.", "planar");
  // A bare `planar` must render root help rather than fail.
  app->require_subcommand(0);

  // Root declaration order is observable in help and the schema catalog.
  // Keep this sequence pinned when changing command modules.
  handlers::declare_init(*app);     // 1
  handlers::declare_scope(*app);    // 2
  handlers::declare_assoc(*app);    // 3
  handlers::declare_plan(*app);     // 4
  handlers::declare_task(*app);     // 5
  handlers::declare_question(*app); // 6
  handlers::declare_scenario(*app); // 7
  handlers::declare_decision(*app); // 8
  handlers::declare_artifact(*app); // 9
  handlers::declare_document(*app);
  handlers::declare_annotate(*app);   // 10
  handlers::declare_promote(*app);    // 11
  handlers::declare_demote(*app);     // 12
  handlers::declare_workbench(*app);  // 13
  handlers::declare_workspace(*app);  // 14
  handlers::declare_link(*app);       // 15
  handlers::declare_unlink(*app);     // 16
  handlers::declare_links(*app);      // 17
  handlers::declare_resume(*app);     // 18
  handlers::declare_handoff(*app);    // 19
  handlers::declare_capture(*app);    // 20
  handlers::declare_audit(*app);      // 21
  handlers::declare_health(*app);     // 22
  handlers::declare_models(*app);     // 23
  handlers::declare_dashboard(*app);  // 24
  handlers::declare_spec(*app);       // 25
  handlers::declare_test_spec(*app);  // 26
  handlers::declare_config(*app);     // 27
  handlers::declare_templates(*app);  // 28
  handlers::declare_tree(*app);       // 29
  handlers::declare_search(*app);     // 30
  handlers::declare_local(*app);      // 31
  handlers::declare_skills(*app);     // 32
  handlers::declare_import(*app);     // 33
  handlers::declare_synthesize(*app); // 34
  handlers::declare_version(*app);    // 35
  handlers::declare_completion(*app); // 36
  handlers::declare_schema(*app);     // 37
  handlers::declare_report(*app);     // 38
  handlers::declare_bench(*app);      // 39
  handlers::declare_closure(*app);    // 40
  handlers::declare_run(*app);        // 41
  handlers::declare_groups(*app);     // 42
  handlers::declare_explore(*app);    // 43
  handlers::declare_workflow(*app);   // 44
  handlers::declare_feedback(*app);   // 45

  // Help renders the same page it rendered before every bool flag gained
  // its `--no-X` negation — see `planar.cliapp.surface::hide_negations_in_help`.
  // Must come AFTER the whole tree exists.
  cliapp::hide_negations_in_help(*app);
  return app;
}

} // namespace planar::cmd
