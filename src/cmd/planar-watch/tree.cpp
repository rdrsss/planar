/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar_watch.tree`.

module planar.cmd.planar_watch.tree;

import std;
import cli11;

namespace planar::cmd::watch {

auto root_app() -> std::unique_ptr<CLI::App> {
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

  app->add_subcommand("version", "Print the planar-watch version, commit, and zig runtime.");

  CLI::App* completion = app->add_subcommand("completion", "Generate the autocompletion script for the specified shell.");
  completion->add_option("shell")->description("Shell: bash, zsh, or fish")->required();

  app->add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");

  return app;
}

auto forbidden_verbs() -> std::vector<std::string_view> {
  return {// Every planar-agent write verb.
          "pull", "claim", "heartbeat", "complete", "fail", "release", "block", "action", "ingest", "reconcile", "abort", "peek",
          // Every planar planning-entity verb.
          "plan", "task", "decision", "question", "scenario", "artifact", "annotate", "init", "workbench", "doc", "spec",
          "templates", "ext", "sync", "promote", "demote", "capture"};
}

} // namespace planar::cmd::watch
