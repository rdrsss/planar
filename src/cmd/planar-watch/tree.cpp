/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar_watch.tree`.

module planar.cmd.planar_watch.tree;

import std;
import planar.cli;

namespace planar::cmd::watch {

auto root_command() -> cli::cmd {
  cli::cmd version{.name = "version", .desc = "Print the planar-watch version, commit, and zig runtime."};
  cli::cmd completion{
      .name        = "completion",
      .desc        = "Generate the autocompletion script for the specified shell.",
      .positionals = {cli::positional{.name = "shell", .desc = "Shell: bash, zsh, or fish", .required = true}},
  };
  cli::cmd schema{.name = "schema", .desc = "Print the full command tree as a JSON catalog (flags, aliases, positionals)."};

  // Unlike `planar-agent`, this root has a genuinely DISTINCT `long_desc`:
  // `planar-watch schema` reports a "summary" ("Read-only viewer for live
  // agent activity (…)") different from its "description" (the multi-line
  // block below), and `planar-watch --help` renders that block FLUSH LEFT,
  // which is `planar.cli.help`'s long_desc branch. Both strings are
  // transcribed from the oracle catalog; the em dash and the `→` are the
  // oracle's own bytes and must survive verbatim.
  return cli::cmd{
      .name      = "planar-watch",
      .desc      = "Read-only viewer for live agent activity (feed / ps / claims / actions / plans / log / tree / run).",
      .long_desc = "planar-watch is the human-facing live cockpit for agent activity.\n"
                   "\n"
                   "  The default invocation with no args is the activity feed.\n"
                   "  Subcommands narrow the view; `--follow` turns each one into a\n"
                   "  streaming view that emits new rows as the underlying tables\n"
                   "  change. The binary opens the database in strict read-only mode\n"
                   "  (SQLITE_OPEN_READONLY) — every write SQL string is rejected by\n"
                   "  the SQLite driver itself, the second line of defense behind\n"
                   "  this binary's `no write verbs registered` capability boundary.\n"
                   "\n"
                   "  `tree` renders the orchestrator → sub-agent forest by walking\n"
                   "  agent_actions.parent_action_id chains.",
      .cmds      = {version, completion, schema},
  };
}

auto forbidden_verbs() -> std::vector<std::string_view> {
  return {// Every planar-agent write verb.
          "pull", "claim", "heartbeat", "complete", "fail", "release", "block", "action", "ingest", "reconcile", "abort", "peek",
          // Every planar planning-entity verb.
          "plan", "task", "decision", "question", "scenario", "artifact", "annotate", "init", "workbench", "doc", "spec",
          "templates", "ext", "sync", "promote", "demote", "capture"};
}

} // namespace planar::cmd::watch
