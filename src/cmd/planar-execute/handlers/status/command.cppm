/// @file command.cppm
/// @brief Dispatch the planar-execute status command.
module;
export module planar.cmd.planar_execute.handlers.status.command;
import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.handlers.shared.client;

export namespace planar::cmd::execute::handlers::status {
/// @brief Parse and show one run or a profile's daemon status.
/// @param args Arguments after the status verb.
/// @return Process exit code.
auto execute(std::span<const std::string> args) -> int {
  auto const asked = parse_run_id_args(args, false);
  if (!asked.has_value()) {
    std::cerr << usage_text();
    return 2;
  }
  return client::status_run(*asked);
}
} // namespace planar::cmd::execute::handlers::status
