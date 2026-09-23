/// @file environment.cppm
/// @brief Process values shared by the command binaries.
module;
export module planar.cmd.internal.environment;
import std;

namespace planar::cmd::internal {
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

export auto process_env() -> env_lookup;
export auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup;
export auto resolve_db_path(const env_lookup& env) -> std::optional<std::filesystem::path>;

/// @brief Planar verifies PWD against the process cwd; its consumers retain
/// their existing PWD-first behavior until that contract is changed.
export enum class cwd_policy { verified, pwd_first };
export auto operator_cwd(const env_lookup& env, cwd_policy policy) -> std::filesystem::path;
} // namespace planar::cmd::internal
