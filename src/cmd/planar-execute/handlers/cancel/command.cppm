/// @file command.cppm
/// @brief Dispatch the planar-execute cancel command.
module;
export module planar.cmd.planar_execute.handlers.cancel.command;
import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.handlers.shared.client;

export namespace planar::cmd::execute::handlers::cancel {
/// @brief Parse and request cancellation of one run.
/// @param args Arguments after the cancel verb.
/// @return Process exit code.
auto execute(std::span<const std::string> args) -> int {
  auto const asked = parse_run_id_args(args, true);
  if (!asked.has_value()) {
    std::cerr << usage_text();
    return 2;
  }
  return client::cancel_run_verb(*asked);
}
} // namespace planar::cmd::execute::handlers::cancel
