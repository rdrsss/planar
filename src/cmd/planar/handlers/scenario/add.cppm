/// @file add.cppm
/// @brief CLI declaration for `scenario add`.
export module planar.cmd.planar.handlers.scenario.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
/// @brief Register the add CLI node.
/// @param scenario Input scenario.
/// @return Registered CLI node.
export auto attach_add(CLI::App& scenario) -> CLI::App* {
  CLI::App* add = scenario.add_subcommand("add", "Create a new test scenario.");
  add_string(*add, "--body", "Scenario description; @<file> reads it from a file");
  add_string(*add, "--scope", "Scope slug to create in (default: cwd-derived write scope)");
  add_int(*add, "--related", "Artifact id (spec or ADR) the scenario relates to");
  add_int(*add, "--plan", "Attach the scenario to this plan id");
  add_bool(*add, "--editor", "Accepted for parity; not implemented, create proceeds inline");
  add_json(*add, "Emit machine-readable JSON instead of text");
  add_positional(*add, "title", "Scenario title");
  return add;
}
} // namespace planar::cmd::handlers::scenario_cli
