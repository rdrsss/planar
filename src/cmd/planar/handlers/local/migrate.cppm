/// @file migrate.cppm
/// @brief CLI declaration for `local migrate`.
export module planar.cmd.planar.handlers.local.migrate;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::local_cli {
/// @brief Register the migrate CLI node.
/// @param local Input local.
/// @return Registered CLI node.
export auto attach_migrate(CLI::App* local) -> CLI::App* {
  CLI::App* migrate = local->add_subcommand("migrate", "Migrate skills/agents to new Planar version.");
  add_bool(*migrate, "--dry-run", k_undocumented);
  add_json(*migrate, k_undocumented);
  return migrate;
}
} // namespace planar::cmd::handlers::local_cli
