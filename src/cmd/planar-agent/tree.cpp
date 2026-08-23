/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar_agent.tree`.

module planar.cmd.planar_agent.tree;

import std;
import planar.cli;

namespace planar::cmd::agent {

auto root_command() -> cli::cmd {
  // Every `desc` below is transcribed from the Zig node and then CHECKED
  // against the oracle's rendered `--help` bytes (tree.t.cpp), which is the
  // direction that catches a mistake: a wrong word changes the page.
  cli::cmd version{.name = "version", .desc = "Print the planar-agent version, commit, and zig runtime."};
  cli::cmd schema{.name = "schema", .desc = "Print the full command tree as a JSON catalog (flags, aliases, positionals)."};

  // No `long_desc`, and that is READ OFF THE RENDERED PAGE rather than off
  // the schema catalog. `planar-agent schema` reports "summary" and
  // "description" as the same string, which looks like a long_desc equal to
  // the desc — but the Zig emitter FALLS BACK to desc when long_desc is
  // empty, so the catalog cannot tell the two apart. The oracle's
  // `--help` can: it renders the sentence INDENTED by two spaces, which is
  // `planar.cli.help`'s desc branch; the long_desc branch emits flush left
  // (as planar-watch's genuinely-distinct long_desc does). Setting
  // long_desc here would silently shift the line two columns.
  return cli::cmd{
      .name = "planar-agent",
      .desc = "Agent-callable coordination binary (pull / claim / complete / heartbeat / reconcile).",
      .cmds = {version, schema},
  };
}

auto forbidden_verbs() -> std::vector<std::string_view> {
  return {"plan", "task",      "decision", "question", "scenario", "artifact", "annotate", "init",      "workbench", "doc",
          "spec", "templates", "ext",      "sync",     "promote",  "demote",   "capture",  "dashboard", "tree",      "health"};
}

} // namespace planar::cmd::agent
