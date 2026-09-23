/// @file context.cppm
/// @brief Binary-facing name for the shared, database-injected invocation context.
module;
export module planar.cmd.planar_ext.context;
import std;
import planar.cmd.internal.context;
import planar.cmd.internal.environment;
export import planar.cmd.planar_ext.database;
import planar.cmd.planar_ext.exit;

namespace planar::cmd::ext {
export using env_lookup = internal::env_lookup;
export using context    = internal::context<database>;
export auto process_env() -> env_lookup;
export auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup;
export auto resolve_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, domain_error>;
export auto operator_cwd(const env_lookup& env) -> std::filesystem::path;
} // namespace planar::cmd::ext
