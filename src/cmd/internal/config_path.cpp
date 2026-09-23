/// @file config_path.cpp
/// @brief Shared command configuration path resolution.
module planar.cmd.internal.config_path;
import std;
import planar.cmd.internal.environment;

namespace planar::cmd::internal {
auto resolve_config_path(const env_lookup& env) -> std::optional<std::filesystem::path> {
  auto const home = env("HOME");
  if (auto const raw = env("PLANAR_CONFIG_PATH"); raw.has_value() && !raw->empty()) {
    if (*raw == "~") {
      return home.has_value() ? std::optional{std::filesystem::path{*home}} : std::nullopt;
    }
    if (raw->starts_with("~/")) {
      if (!home.has_value()) {
        return std::nullopt;
      }
      return std::filesystem::path{*home} / std::string_view{*raw}.substr(2);
    }
    return std::filesystem::path{*raw};
  }
  if (!home.has_value()) {
    return std::nullopt;
  }
  return std::filesystem::path{*home} / ".planar" / "config.toml";
}
} // namespace planar::cmd::internal
