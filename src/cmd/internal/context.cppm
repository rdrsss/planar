/// @file context.cppm
/// @brief Invocation values with an injected, lazy database object.
module;
export module planar.cmd.internal.context;
import std;
import planar.cmd.internal.environment;

namespace planar::cmd::internal {
/// @brief Invocation values and an externally constructed database object.
/// @tparam Database The binary-specific lazy database type.
export template <class Database> class context {
  std::vector<std::string>  _argv;
  env_lookup                _env;
  std::filesystem::path     _cwd;
  std::shared_ptr<Database> _database;
  std::ostream*             _out;
  std::ostream*             _err;

public:
  /// @brief Hold one invocation's arguments, environment, streams, and database.
  /// @param argv Full command arguments.
  /// @param env Environment lookup.
  /// @param cwd Operator working directory.
  /// @param database Database object constructed by the caller.
  /// @param out Standard output stream.
  /// @param err Standard error stream.
  context(std::vector<std::string> argv, env_lookup env, std::filesystem::path cwd, std::shared_ptr<Database> database,
          std::ostream& out, std::ostream& err)
      : _argv(std::move(argv)), _env(std::move(env)), _cwd(std::move(cwd)), _database(std::move(database)), _out(&out),
        _err(&err) {
  }
  /// @brief Contexts cannot be copied.
  context(const context&) = delete;
  /// @brief Contexts cannot be copy-assigned.
  /// @return This context when assignment is available.
  context& operator=(const context&) = delete;
  /// @brief Move an invocation context.
  context(context&&) noexcept = default;
  /// @brief Move-assign an invocation context.
  /// @return This context.
  context& operator=(context&&) noexcept = default;

  /// @brief Return the full command arguments.
  /// @return Borrowed argument span.
  [[nodiscard]] auto argv() const -> std::span<const std::string> {
    return _argv;
  }
  /// @brief Return the environment lookup.
  /// @return Borrowed environment lookup.
  [[nodiscard]] auto env() const -> const env_lookup& {
    return _env;
  }
  /// @brief Return the operator working directory.
  /// @return Borrowed path.
  [[nodiscard]] auto cwd() const -> const std::filesystem::path& {
    return _cwd;
  }
  /// @brief Return the injected database object's path.
  /// @return Borrowed path.
  [[nodiscard]] auto db_path() const -> const std::filesystem::path& {
    return _database->path();
  }
  /// @brief Return the output stream.
  /// @return Borrowed stream.
  [[nodiscard]] auto out() const -> std::ostream& {
    return *_out;
  }
  /// @brief Return the error stream.
  /// @return Borrowed stream.
  [[nodiscard]] auto err() const -> std::ostream& {
    return *_err;
  }
  /// @brief Return the injected database object.
  /// @return Mutable borrowed database.
  [[nodiscard]] auto db() -> Database& {
    return *_database;
  }
  /// @brief Return the injected database object.
  /// @return Const borrowed database.
  [[nodiscard]] auto db() const -> const Database& {
    return *_database;
  }
};
} // namespace planar::cmd::internal
