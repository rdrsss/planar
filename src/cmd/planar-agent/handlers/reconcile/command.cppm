/// @file command.cppm
/// @brief CLI declarations for planar-agent reconcile.
module;
export module planar.cmd.planar_agent.handlers.reconcile.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::reconcile_cli {
export auto add(CLI::App& root) -> void {
  // --- reconcile / abort --------------------------------------------------
  CLI::App* reconcile =
      root.add_subcommand("reconcile", "Operator recovery: mark expired claims stale, close orphaned actions, abandon dead "
                                       "runs.");
  cliapp::add_bool_flag(*reconcile, "--dry-run", "Report candidates without writing");
  reconcile->add_option("--stale-after")
      ->description("Additional grace beyond lease expiry (default 0s; accepts bare int seconds or suffixed "
                    "duration: 10m, 1h, 500ms)")
      ->default_str("0");
  // Sentinel-zero default, where `--plan` right below is a plain optional.
  // Both mean "global sweep"; the asymmetry is the oracle's.
  reconcile->add_option("--session")
      ->description("Scope the sweep to a single session id (0 = global sweep, the default)")
      ->check(cliapp::zig_int_validator())
      ->default_str("0");
  reconcile->add_option("--plan")
      ->description("Scope the sweep to claims/actions/runs belonging to this plan id (0 or absent = global sweep)")
      ->check(cliapp::zig_int_validator());
  reconcile->add_option("--category")
      ->description("Optional closed failure category applied to claims made stale")
      ->check(CLI::IsMember{shared::failure_categories()});
  cliapp::add_bool_flag(*reconcile, "--override-supervisor",
                        "Also reconcile engine-supervised claims and centurion runs (skipped by default; logged as "
                        "supervisor_override)");
  shared::add_json(*reconcile);
}
} // namespace planar::cmd::agent::handlers::reconcile_cli
