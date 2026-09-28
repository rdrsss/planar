/// @file command.cppm
/// @brief Dispatch the planar-execute follow command.
module;
export module planar.cmd.planar_execute.handlers.follow.command;
import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.handlers.shared.client;

export namespace planar::cmd::execute::handlers::follow {
/// @brief Parse and follow one run's event stream.
/// @param args Arguments after the follow verb.
/// @return Process exit code.
auto execute(std::span<const std::string> args) -> int {
  auto const asked = parse_run_id_args(args, true);
  if (!asked.has_value()) {
    std::cerr << usage_text();
    return 2;
  }
  return client::follow_run_verb(*asked);
}
} // namespace planar::cmd::execute::handlers::follow
