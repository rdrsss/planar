/// @file command.cppm
/// @brief Dispatch the planar-execute submit command.
module;
export module planar.cmd.planar_execute.handlers.submit.command;
import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.handlers.shared.client;

export namespace planar::cmd::execute::handlers::submit {
/// @brief Parse and submit one workflow run.
/// @param args Arguments after the submit verb.
/// @return Process exit code.
auto execute(std::span<const std::string> args) -> int {
  auto const asked = parse_submit_args(args);
  if (!asked.has_value()) {
    std::cerr << usage_text();
    return 2;
  }
  return client::submit_run(*asked);
}
} // namespace planar::cmd::execute::handlers::submit
