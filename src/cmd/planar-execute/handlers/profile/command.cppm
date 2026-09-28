/// @file command.cppm
/// @brief Dispatch the manually parsed planar-execute profile command.
module;
#include <cstdlib>
export module planar.cmd.planar_execute.handlers.profile.command;
import std;
import planar.engine_execute;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.engine;
import planar.cmd.planar_execute.profile;
import planar.cmd.planar_execute.selector;
import planar.cmd.planar_execute.handlers.shared.engine_env;

namespace planar::cmd::execute::handlers::profile {
/// @brief Execute this command.
/// @param args Input args.
/// @param out Input out.
/// @param err Input err.
/// @return Process exit code.
export auto execute(std::span<const std::string> args, std::ostream& out, std::ostream& err) -> int {
  auto const asked = parse_profile_args(args);
  if (!asked.has_value()) {
    err << usage_text();
    return 2;
  }
  auto const bin_dir = executable_dir();
  auto const entries = engine::execute::read_planar_config_all(bin_dir);
  if (!entries.has_value()) {
    err << "planar-execute: cannot read the config plane: " << entries.error() << '\n';
    return 1;
  }
  auto const choice = resolve_engine(std::nullopt, shared::engine_env(), entries_config_reader(*entries));
  if (!choice.has_value()) {
    err << "planar-execute: " << choice.error() << '\n';
    return 1;
  }
  auto const resolved = resolve_profile(
      *entries, asked->name,
      [](std::string_view name) -> std::optional<std::string> {
        char const* raw = std::getenv(std::string{name}.c_str()); // NOLINT(concurrency-mt-unsafe) — single-threaded startup.
        return raw == nullptr || *raw == '\0' ? std::nullopt : std::optional<std::string>{raw};
      },
      [&bin_dir] {
        auto const path = engine::execute::read_planar_config_path(bin_dir);
        return path.has_value() ? *path : std::string{"the planar config file"};
      });
  if (!resolved.has_value()) {
    err << "planar-execute: " << resolved.error() << '\n';
    return 1;
  }
  out << render_profile(*choice, *resolved, asked->json);
  return 0;
}
} // namespace planar::cmd::execute::handlers::profile
