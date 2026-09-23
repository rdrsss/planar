/// @file database.cppm
/// @brief Database access policy for this command binary.
module;
export module planar.cmd.planar_ext.database;
import std;
import planar.db;
import planar.cmd.internal.database;
import planar.cmd.planar_ext.exit;

namespace planar::cmd::ext {
/// @brief Tables the external-plane binary may write.
/// @return The SQLite write-authorizer allowlist.
export auto write_allowlist() -> std::vector<std::string>;
/// @brief Opens a database with external-plane writes restricted by SQLite.
export struct database_policy {
  /// @brief Error type returned when opening the database fails.
  using error = domain_error;
  /// @brief Open a schema-compatible external-plane database.
  /// @param path Database path.
  /// @param error_stream Destination for nonfatal diagnostics.
  /// @return An open connection or a command error.
  static auto open(const std::filesystem::path& path, std::ostream& error_stream) -> std::expected<db::connection, domain_error>;
};
/// @brief Lazily opened external-plane database object.
export using database = internal::database<database_policy>;
} // namespace planar::cmd::ext
