/// @file context.cppm
/// @brief Binary-facing name for the shared, database-injected invocation context.
module;
export module planar.cmd.planar.context;
import std;
import planar.cmd.internal.context;
import planar.cmd.internal.environment;
export import planar.cmd.planar.database;
import planar.cmd.planar.exit;

namespace planar::cmd {
/// @brief Environment lookup used by this command binary.
export using env_lookup = internal::env_lookup;
/// @brief Invocation context with an injected database object.
export using context = internal::context<database>;
/// @brief Look up variables from the process environment.
/// @return Environment lookup callable.
export auto process_env() -> env_lookup;
/// @brief Create an environment lookup from supplied values.
/// @param vars Environment values.
/// @return Environment lookup callable.
export auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup;
/// @brief Resolve the command database path.
/// @param env Environment lookup.
/// @return Database path or a command error.
export auto resolve_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, domain_error>;
/// @brief Resolve this binary's operator working directory.
/// @param env Environment lookup.
/// @return Working directory path.
export auto operator_cwd(const env_lookup& env) -> std::filesystem::path;
} // namespace planar::cmd
