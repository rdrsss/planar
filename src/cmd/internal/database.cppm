/// @file database.cppm
/// @brief Lazy database holder with an injected binary-specific access policy.
module;
export module planar.cmd.internal.database;
import std;
import planar.db;

namespace planar::cmd::internal {
/// @brief Lazy connection holder using a binary-specific open policy.
/// @tparam Policy Access and schema policy for the command binary.
export template <class Policy> class database {
  std::filesystem::path         _path;
  std::ostream*                 _err;
  std::optional<db::connection> _connection;

public:
  /// @brief Error type returned by the policy.
  using error = typename Policy::error;

  /// @brief Construct an unopened database object.
  /// @param path Database path.
  /// @param err Destination for nonfatal diagnostics.
  database(std::filesystem::path path, std::ostream& err) : _path(std::move(path)), _err(&err) {
  }
  /// @brief Database holders cannot be copied.
  database(const database&) = delete;
  /// @brief Database holders cannot be copy-assigned.
  /// @return This holder when assignment is available.
  database& operator=(const database&) = delete;
  /// @brief Move a database holder.
  database(database&&) noexcept = default;
  /// @brief Move-assign a database holder.
  /// @return This holder.
  database& operator=(database&&) noexcept = default;

  /// @brief Return the configured database path.
  /// @return Borrowed path.
  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return _path;
  }
  /// @brief Report whether the connection has been opened.
  /// @return True after a successful open.
  [[nodiscard]] auto opened() const -> bool {
    return _connection.has_value();
  }

  /// @brief Open through the policy on first use and reuse that connection.
  /// @return Borrowed connection or a policy error.
  auto ensure_db() -> std::expected<db::connection*, error> {
    if (!_connection.has_value()) {
      auto opened = Policy::open(_path, *_err);
      if (!opened) {
        return std::unexpected(std::move(opened.error()));
      }
      _connection.emplace(std::move(*opened));
    }
    return &*_connection;
  }

  /// @brief Close and reopen through the policy.
  /// @return Borrowed replacement connection or a policy error.
  auto refresh_db() -> std::expected<db::connection*, error> {
    _connection.reset();
    return ensure_db();
  }
};
} // namespace planar::cmd::internal
