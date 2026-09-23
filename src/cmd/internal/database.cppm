/// @file database.cppm
/// @brief Lazy database holder with an injected binary-specific access policy.
module;
export module planar.cmd.internal.database;
import std;
import planar.db;

namespace planar::cmd::internal {
export template <class Policy> class database {
  std::filesystem::path         _path;
  std::ostream*                 _err;
  std::optional<db::connection> _connection;

public:
  using error = typename Policy::error;

  database(std::filesystem::path path, std::ostream& err) : _path(std::move(path)), _err(&err) {
  }
  database(const database&)                = delete;
  database& operator=(const database&)     = delete;
  database(database&&) noexcept            = default;
  database& operator=(database&&) noexcept = default;

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return _path;
  }
  [[nodiscard]] auto opened() const -> bool {
    return _connection.has_value();
  }

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

  auto refresh_db() -> std::expected<db::connection*, error> {
    _connection.reset();
    return ensure_db();
  }
};
} // namespace planar::cmd::internal
