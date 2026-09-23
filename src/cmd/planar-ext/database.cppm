/// @file database.cppm
/// @brief Database access policy for this command binary.
module;
export module planar.cmd.planar_ext.database;
import std;
import planar.db;
import planar.cmd.internal.database;
import planar.cmd.planar_ext.exit;

namespace planar::cmd::ext {
export auto write_allowlist() -> std::vector<std::string>;
export struct database_policy {
  using error = domain_error;
  static auto open(const std::filesystem::path& path, std::ostream& error_stream) -> std::expected<db::connection, domain_error>;
};
export using database = internal::database<database_policy>;
} // namespace planar::cmd::ext
