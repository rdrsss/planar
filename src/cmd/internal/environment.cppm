/// @file environment.cppm
/// @brief Process values shared by the command binaries.
module;
export module planar.cmd.internal.environment;
import std;

namespace planar::cmd::internal {
/// @brief Look up one environment variable without taking a process snapshot.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief Create a lookup over the process environment.
/// @return Environment lookup callable.
export auto process_env() -> env_lookup;
/// @brief Create a deterministic lookup over supplied values.
/// @param vars Environment values.
/// @return Environment lookup callable.
export auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup;
/// @brief Resolve the database path from PLANAR_DB or HOME.
/// @param env Environment lookup.
/// @return Database path when either variable is available.
export auto resolve_db_path(const env_lookup& env) -> std::optional<std::filesystem::path>;

/// @brief Planar verifies PWD against the process cwd; its consumers retain
/// their existing PWD-first behavior until that contract is changed.
export enum class cwd_policy { verified, pwd_first };
/// @brief Resolve the working directory under the selected binary policy.
/// @param env Environment lookup.
/// @param policy PWD verification behavior.
/// @return Working directory path.
export auto operator_cwd(const env_lookup& env, cwd_policy policy) -> std::filesystem::path;
} // namespace planar::cmd::internal
