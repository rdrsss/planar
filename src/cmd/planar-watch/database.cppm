/// @file database.cppm
/// @brief Database access policy for this command binary.
module;
export module planar.cmd.planar_watch.database;
import std;
import planar.db;
import planar.cmd.internal.database;
import planar.cmd.planar_watch.exit;

namespace planar::cmd::watch {
/// @brief Opens the viewer's read-only database.
export struct database_policy {
  /// @brief Error type returned when opening the database fails.
  using error = domain_error;
  /// @brief Open a schema-compatible read-only database.
  /// @param path Database path.
  /// @param error_stream Destination for nonfatal diagnostics.
  /// @return An open connection or a command error.
  static auto open(const std::filesystem::path& path, std::ostream& error_stream) -> std::expected<db::connection, domain_error>;
};
/// @brief Lazily opened viewer database object.
export using database = internal::database<database_policy>;
} // namespace planar::cmd::watch
