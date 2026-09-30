/// @file agentstore.cpp
/// @brief Agent database access policy for this command binary.
module planar.cmd.planar_watch.agentstore;

import std;
import planar.db;
import planar.db.agentdb;
import planar.engine.hostqueue;
import planar.cmd.planar_watch.exit;

namespace planar::cmd::watch {

agent_store::agent_store(std::filesystem::path path, std::optional<db::connection> connection)
    : _path(std::move(path)), _connection(std::move(connection)) {
}
auto agent_store::path() const -> const std::filesystem::path& {
  return _path;
}
auto agent_store::present() const -> bool {
  return false;
}
auto agent_store::connection() -> db::connection* {
  return nullptr;
}
auto agent_store::entries() -> std::expected<std::vector<engine::hostqueue::entry>, domain_error> {
  return std::unexpected(error_from_body(domain_error_kind::not_implemented, "agent store: not implemented"));
}
auto agent_store::history(std::optional<std::int64_t>) -> std::expected<std::vector<engine::hostqueue::history_row>, domain_error> {
  return std::unexpected(error_from_body(domain_error_kind::not_implemented, "agent store: not implemented"));
}
auto agent_open_error(const db::agent::open_error&) -> domain_error {
  return error_from_body(domain_error_kind::not_implemented, "agent store: not implemented");
}
auto open_agent_store_at(const std::filesystem::path&) -> std::expected<agent_store, domain_error> {
  return std::unexpected(error_from_body(domain_error_kind::not_implemented, "agent store: not implemented"));
}
auto open_agent_store(const agent_env_lookup&) -> std::expected<agent_store, domain_error> {
  return std::unexpected(error_from_body(domain_error_kind::not_implemented, "agent store: not implemented"));
}

} // namespace planar::cmd::watch
