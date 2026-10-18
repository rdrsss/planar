/// @file context.cpp
/// @brief Command-facing adapters for shared invocation environment helpers.
module planar.cmd.planar_agent.context;
import std;
import planar.cmd.internal.environment;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {
auto process_env() -> env_lookup {
  return internal::process_env();
}
auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup {
  return internal::map_env(std::move(vars));
}
auto resolve_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, domain_error> {
  auto path = internal::resolve_db_path(env);
  if (!path) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           "neither PLANAR_DB nor HOME is set; cannot locate the Planar database"));
  }
  return *path;
}
auto operator_cwd(const env_lookup& env) -> std::filesystem::path {
  return internal::operator_cwd(env, internal::cwd_policy::pwd_first);
}
} // namespace planar::cmd::agent
