/// @file main.cppm
/// @brief Assemble planar-watch root commands in their established order.
module;
export module planar.cmd.planar_watch.main;
import std;
import cli11;
import planar.cliapp.schema;
import planar.cliapp.surface;
import planar.cmd.planar_watch.docs;
import planar.cmd.planar_watch.handlers.feed.command;
import planar.cmd.planar_watch.handlers.ps.command;
import planar.cmd.planar_watch.handlers.claims.command;
import planar.cmd.planar_watch.handlers.actions.command;
import planar.cmd.planar_watch.handlers.plans.command;
import planar.cmd.planar_watch.handlers.log.command;
import planar.cmd.planar_watch.handlers.tree.command;
import planar.cmd.planar_watch.handlers.run.command;
import planar.cmd.planar_watch.handlers.sync_events.command;
import planar.cmd.planar_watch.handlers.diagnose.command;
import planar.cmd.planar_watch.handlers.queue.command;
import planar.cmd.planar_watch.handlers.version.command;
import planar.cmd.planar_watch.handlers.completion.command;
import planar.cmd.planar_watch.handlers.schema.command;

namespace planar::cmd::watch {
/// @brief Build the root CLI application.
/// @return Root CLI application.
export auto root_app() -> std::unique_ptr<CLI::App> {
  // The root's description is the multi-line block the oracle's own
  // `--help` renders; `planar-watch schema` reports the one-line
  // "Read-only viewer for live agent activity (…)" as its summary. A
  // `CLI::App` carries ONE description string where `cli::cmd` carried
  // `desc` and `long_desc` separately, so the longer, operator-facing one
  // is what survives — see `planar.cliapp.schema`'s header, divergence 1.
  // The em dash and the `→` are the oracle's own bytes and must survive
  // verbatim.
  auto app = std::make_unique<CLI::App>("planar-watch is the human-facing live cockpit for agent activity.\n"
                                        "\n"
                                        "  The default invocation with no args is the activity feed.\n"
                                        "  Subcommands narrow the view; `--follow` turns each one into a\n"
                                        "  streaming view that emits new rows as the underlying tables\n"
                                        "  change. The binary opens the database in strict read-only mode\n"
                                        "  (SQLITE_OPEN_READONLY) \xe2\x80\x94 every write SQL string is rejected by\n"
                                        "  the SQLite driver itself, the second line of defense behind\n"
                                        "  this binary's `no write verbs registered` capability boundary.\n"
                                        "\n"
                                        "  `tree` renders the orchestrator \xe2\x86\x92 sub-agent forest by walking\n"
                                        "  agent_actions.parent_action_id chains.",
                                        "planar-watch");

  // A bare `planar-watch` must render root help rather than fail, so the
  // root requires no subcommand and `dispatch::run` treats "matched a node
  // that has children, but none of them" as a help request.
  app->require_subcommand(0);

  // Declaration ORDER matches the oracle's `handlers/cmd.zig` registry:
  // feed, ps, claims, actions, plans, log, tree, run, sync-events, version,
  // completion, schema — `feed`, `run` (+ its two children) and
  // `sync-events` used to come from generated surface data applied after
  // this block (`cliapp::apply_surface`), folded in here directly at task
  // 6613 (M11.1) so there is exactly one declaration site per node. Every
  // leaf below has a real handler (`dispatch.cpp`'s table); none refuses at
  // exit 64. Each description is the oracle node's LONG description
  // transcribed verbatim — `CLI::App` carries one description string, and
  // this tree's settled choice is that the longer operator-facing one
  // survives (see the root's note above, and `src/cmd/planar/tree.cpp`'s
  // `workbench`).
  //
  // TRANSCRIPTION INCLUDES THE `--follow` SENTENCES, which promise a
  // streaming arm this build refuses (exit 64). Left verbatim rather than
  // edited: the description is a D2 transcription, the divergence is named
  // in this binary's CMakeLists and in the two handler modules' headers,
  // and the runtime refusal is loud rather than silent. A reader who
  // follows the help page gets an explicit "not implemented in this build",
  // not a snapshot pretending to be a stream.

  handlers::feed_cli::add(*app);
  handlers::ps_cli::add(*app);
  handlers::claims_cli::add(*app);
  handlers::actions_cli::add(*app);
  handlers::plans_cli::add(*app);
  handlers::log_cli::add(*app);
  handlers::tree_cli::add(*app);
  handlers::run_cli::add(*app);
  handlers::sync_events_cli::add(*app);
  handlers::queue_cli::add(*app);
  handlers::diagnose_cli::add(*app);
  handlers::version_cli::add(*app);
  handlers::completion_cli::add(*app);
  handlers::schema_cli::add(*app);

  // Every node registered above (including `feed`, `run`/`run list`/`run show`
  // and `sync-events`) remains hand-transcribed — task 6613 (M11.1) folded
  // `surface.cpp`'s generated `node_spec` table in, matching the
  // `planar-ext` shape (one declaration site per node, no
  // `cliapp::apply_surface` precedence rule). `planar.cmd.planar_watch.surface`
  // still exists but now holds only the summary-vs-description divergence
  // table (`surface_summaries`, consumed by `handlers::schema`) and the
  // (currently empty) unported-leaf inventory `dispatch.cpp` still reads —
  // neither is a second declaration of a node this tree already owns.

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
  return {// Every planar-agent write verb.
          "pull", "claim", "heartbeat", "complete", "fail", "release", "block", "action", "ingest", "reconcile", "abort", "peek",
          // Every planar planning-entity verb.
          "plan", "task", "decision", "question", "scenario", "artifact", "annotate", "init", "workbench", "doc", "spec",
          "templates", "ext", "sync", "promote", "demote", "capture"};
}

} // namespace planar::cmd::watch
