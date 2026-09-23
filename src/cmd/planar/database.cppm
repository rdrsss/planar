/// @file database.cppm
/// @brief Database access policy for this command binary.
module;
export module planar.cmd.planar.database;
import std;
import planar.db;
import planar.cmd.internal.database;
import planar.cmd.planar.exit;

namespace planar::cmd {
/// @brief Opens the operator's writable database and applies migrations.
export struct database_policy {
  /// @brief Error type returned when opening the database fails.
  using error = domain_error;
  /// @brief Open and migrate the operator database.
  /// @param path Database path.
  /// @param error_stream Destination for nonfatal diagnostics.
  /// @return An open connection or a command error.
  static auto open(const std::filesystem::path& path, std::ostream& error_stream) -> std::expected<db::connection, domain_error>;
};
/// @brief Lazily opened operator database object.
export using database = internal::database<database_policy>;
} // namespace planar::cmd
