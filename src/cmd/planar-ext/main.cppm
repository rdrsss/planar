/// @file main.cppm
/// @brief Assemble planar-ext root commands in their established order.
module;
export module planar.cmd.planar_ext.main;
import std;
import cli11;
import planar.cliapp.schema;
import planar.cliapp.surface;
import planar.cmd.planar_ext.docs;
import planar.cmd.planar_ext.handlers.version.command;
import planar.cmd.planar_ext.handlers.schema.command;
import planar.cmd.planar_ext.handlers.ext.command;
import planar.cmd.planar_ext.handlers.sync.command;
namespace planar::cmd::ext {
/// @brief Build the root CLI application.
/// @return Root CLI application.
export auto root_app() -> std::unique_ptr<CLI::App> {
  // Task 6419 moved the `ext`/`sync` verb family in; this description no
  // longer describes a skeleton. Task 6421 landed `ext propagate` itself —
  // the GitHub parent-issue arm only — and task 6428 landed the
  // `--restrategize`/`--verify-counterparts`-family flags on top of it; see
  // handlers/ext/propagate.cppm for what still refuses explicitly (a Jira
  // system and a multi-repo GitHub feature).
  auto app = std::make_unique<CLI::App>("Host of the external-plane propagation and sync verbs (`ext`, `sync`),\n"
                                        "  moved off `planar` at plan 996, task 6419.\n"
                                        "\n"
                                        "  This binary's connection is restricted at the SQLite authorizer level\n"
                                        "  to read-write on exactly external_links, external_systems, and\n"
                                        "  sync_events \xe2\x80\x94 every other table is read-only through this\n"
                                        "  process, enforced by the driver rather than by convention.",
                                        "planar-ext");

  // A bare `planar-ext` renders root help rather than failing.
  app->require_subcommand(0);

  handlers::version_cli::add(*app);
  handlers::schema_cli::add(*app);
  handlers::ext_cli::add(*app);
  handlers::sync_cli::add(*app);

  // Help renders the same page it rendered before every bool flag gained
  // its `--no-X` negation.
  cliapp::hide_negations_in_help(*app);
  cliapp::install_docs_footers(*app, surface_docs());
  return app;
}

} // namespace planar::cmd::ext
