/// @file config_path.cppm
/// @brief Resolve the operator configuration path for command consumers.
module;
export module planar.cmd.internal.config_path;
import std;
import planar.cmd.internal.environment;

namespace planar::cmd::internal {
/// @brief Resolve PLANAR_CONFIG_PATH or HOME's default config path.
/// @param env Environment lookup.
/// @return Config path when the required environment is available.
export auto resolve_config_path(const env_lookup& env) -> std::optional<std::filesystem::path>;
} // namespace planar::cmd::internal
