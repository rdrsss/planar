/// @file show.cppm
/// @brief CLI declaration for `config show`.
export module planar.cmd.planar.handlers.config.show;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::config_cli {
/// @brief Register the show CLI node.
/// @param config Input config.
/// @return Registered CLI node.
export auto attach_show(CLI::App* config) -> CLI::App* {
  CLI::App* show = config->add_subcommand("show", "Print the resolved configuration.");
  add_bool(*show, "--effective", "Show each key with its provenance");
  add_bool(*show, "--raw", "Show the raw on-disk config values");
  add_bool(*show, "--defaults", "Show the embedded defaults");
  add_string(*show, "--scope", "Scope slug whose per-association overrides apply");
  add_string_default(*show, "--format", "text", "Accepted but not read; use --json for JSON output");
  add_json(*show, "Emit machine-readable JSON instead of text");
  return show;
}
} // namespace planar::cmd::handlers::config_cli
