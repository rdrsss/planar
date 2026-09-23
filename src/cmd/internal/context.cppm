/// @file context.cppm
/// @brief Invocation values with an injected, lazy database object.
module;
export module planar.cmd.internal.context;
import std;
import planar.cmd.internal.environment;

namespace planar::cmd::internal {
export template <class Database> class context {
  std::vector<std::string>  _argv;
  env_lookup                _env;
  std::filesystem::path     _cwd;
  std::shared_ptr<Database> _database;
  std::ostream*             _out;
  std::ostream*             _err;

public:
  context(std::vector<std::string> argv, env_lookup env, std::filesystem::path cwd, std::shared_ptr<Database> database,
          std::ostream& out, std::ostream& err)
      : _argv(std::move(argv)), _env(std::move(env)), _cwd(std::move(cwd)), _database(std::move(database)), _out(&out),
        _err(&err) {
  }
  context(const context&)                = delete;
  context& operator=(const context&)     = delete;
  context(context&&) noexcept            = default;
  context& operator=(context&&) noexcept = default;

  [[nodiscard]] auto argv() const -> std::span<const std::string> {
    return _argv;
  }
  [[nodiscard]] auto env() const -> const env_lookup& {
    return _env;
  }
  [[nodiscard]] auto cwd() const -> const std::filesystem::path& {
    return _cwd;
  }
  [[nodiscard]] auto db_path() const -> const std::filesystem::path& {
    return _database->path();
  }
  [[nodiscard]] auto out() const -> std::ostream& {
    return *_out;
  }
  [[nodiscard]] auto err() const -> std::ostream& {
    return *_err;
  }
  [[nodiscard]] auto db() -> Database& {
    return *_database;
  }
  [[nodiscard]] auto db() const -> const Database& {
    return *_database;
  }
};
} // namespace planar::cmd::internal
