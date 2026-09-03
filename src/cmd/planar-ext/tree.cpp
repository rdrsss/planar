/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar_ext.tree`.

module planar.cmd.planar_ext.tree;

import std;
import cli11;
import planar.cliapp.surface;

namespace planar::cmd::ext {

auto root_app() -> std::unique_ptr<CLI::App> {
  // Describes both the eventual purpose of the binary AND today's actual
  // scope, so an operator reading `--help` on this skeleton is not misled
  // into thinking `ext`/`sync` verbs already live here.
  auto app = std::make_unique<CLI::App>(
      "planar-ext will host the external-plane propagation and sync verbs\n"
      "  (`ext`, `sync`) currently on `planar` (plan 996, task 6412).\n"
      "\n"
      "  This build is the SKELETON step: no ext/sync verb has moved here\n"
      "  yet, and `planar` still owns the complete surface. This binary's\n"
      "  connection is restricted at the SQLite authorizer level to\n"
      "  read-write on exactly external_links, external_systems, and\n"
      "  sync_events \xe2\x80\x94 every other table is read-only through this\n"
      "  process, enforced by the driver rather than by convention.",
      "planar-ext");

  // A bare `planar-ext` renders root help rather than failing.
  app->require_subcommand(0);

  app->add_subcommand("version", "Print the planar-ext version, commit, and compiler.");
  app->add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");

  // Help renders the same page it rendered before every bool flag gained
  // its `--no-X` negation (neither leaf here declares one today, but this
  // keeps the tree consistent with its siblings as flags land).
  cliapp::hide_negations_in_help(*app);
  return app;
}

} // namespace planar::cmd::ext
