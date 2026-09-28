/// @file command.cppm
/// @brief Dispatch the planar-execute host command family.
module;
export module planar.cmd.planar_execute.handlers.host.command;
import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.handlers.shared.client;

export namespace planar::cmd::execute::handlers::host {
/// @brief Parse a host status, drain, or stop command.
/// @param args Arguments after the host verb.
/// @return Process exit code.
auto execute(std::span<const std::string> args) -> int {
  if (args.empty() || (args[0] != "status" && args[0] != "drain" && args[0] != "stop")) {
    std::cerr << usage_text();
    return 2;
  }
  auto const asked = parse_run_id_args(args.subspan(1), false);
  if (!asked.has_value() || !asked->run_id.empty()) {
    std::cerr << usage_text();
    return 2;
  }
  return args[0] == "status" ? client::host_status(*asked) : client::host_lifecycle(args[0], *asked);
}
} // namespace planar::cmd::execute::handlers::host
