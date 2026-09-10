/// @file context.cppm
/// @brief `planar.cmd.planar_watch.context` — the `planar-watch`
/// invocation context, and the STRICT READ-ONLY database acquisition that
/// is this binary's defining invariant (plan 996, task 6107).
///
/// Port target: zig/src/runtime/runtime.zig's `ensureDbStrictReadOnly`.
///
/// ## The read-only handle is an invariant, not a convention
///
/// CLAUDE.md § four-binary boundary states it flatly — "`planar-watch` —
/// **read-only viewer**. It opens SQLite via `file:?mode=ro`. This is a
/// hard invariant, not a convention." — and the binary's own `--help` page
/// advertises it to operators: "The binary opens the database in strict
/// read-only mode (SQLITE_OPEN_READONLY) — every write SQL string is
/// rejected by the SQLite driver itself, the second line of defense behind
/// this binary's `no write verbs registered` capability boundary."
///
/// Two lines of defense, and they are independent:
///
///   1. the VERB SET (`tree.cppm`) registers no write verb, so no code path
///      in this binary composes a mutating statement at all; and
///   2. this handle, which the SQLite driver itself refuses to mutate, so
///      even a mistake in (1) cannot write.
///
/// (2) is the one that is easy to test vacuously — "we called
/// `open_read_only`, therefore it is read-only" proves nothing about
/// whether writes are actually refused. `context.t.cpp` instead executes a
/// real `insert` through the handle this class hands out and requires it to
/// FAIL. Mutating `open_read_only` back to `open` in this file makes that
/// test fail; that break-probe was run, and it is the evidence, not the
/// call itself.
///
/// ## Three observable consequences, all of them ported deliberately
///
///   - NO PARENT DIRECTORY CREATION and no file creation. The Zig original
///     is explicit: "The strict path does NOT create the parent dir (the
///     writable bootstraps do that). A read-only viewer running before
///     `planar init` should fail loudly with a schema-handshake error, not
///     silently create empty state." `planar.cmd.planar.context::ensure_db`
///     does the opposite — it creates and migrates. Pointing this binary at
///     a nonexistent path therefore FAILS where the operator binary would
///     bootstrap, and that difference is itself a test.
///   - NO `journal_mode = WAL` PRAGMA. WAL is writer-side configuration and
///     a read-only handle cannot set it; only `busy_timeout` is set here,
///     best-effort, so a SELECT waits politely behind a concurrent writer.
///     Copying the agent context's PRAGMA pair would emit a spurious
///     warning on every single invocation.
///   - NO MIGRATION, and refusal on skew in BOTH directions
///     (`schema_version_behind` / `schema_version_ahead`, both exit 7 under
///     `planar.cmd.planar_watch.exit`). A read-only viewer could not
///     migrate even if it wanted to.
///
/// Everything else — an explicit `context&` rather than a process-global,
/// an environment LOOKUP CALLABLE with `process_env()` the only reader of
/// the real environment — is the shape task 6105 settled; see
/// `planar.cmd.planar.context`'s header for that argument.
module;

export module planar.cmd.planar_watch.context;

import std;
import planar.db;
import planar.cmd.planar_watch.exit;

namespace planar::cmd::watch {

/// @brief An environment lookup: variable name in, value or unset out.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief An `env_lookup` over the REAL process environment.
///
/// The only function in `src/cmd/planar-watch/` that calls `std::getenv`.
/// @return A lookup reading the live process environment.
export auto process_env() -> env_lookup;

/// @brief An `env_lookup` over an explicit map — the test-facing
/// counterpart to `process_env`.
/// @param vars The variables to expose; anything absent reads as unset.
/// @return A lookup over a copy of `vars`.
export auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup;

/// @brief Resolve the database path: `$PLANAR_DB` when set, otherwise
/// `$HOME/.planar/planar.db`.
/// @param env The environment to resolve from.
/// @return The resolved path, or `domain_error` when neither is set.
export auto resolve_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, domain_error>;

/// @brief The operator's working directory, PWD-first.
/// @param env The environment to read `$PWD` from.
/// @return The working directory.
export auto operator_cwd(const env_lookup& env) -> std::filesystem::path;

/// @brief One `planar-watch` invocation's process state.
export class context {
private:
  std::vector<std::string>      _argv;
  env_lookup                    _env;
  std::filesystem::path         _cwd;
  std::filesystem::path         _db_path;
  std::ostream*                 _out = nullptr;
  std::ostream*                 _err = nullptr;
  std::optional<db::connection> _db;

public:
  /// @brief Construct a context over an explicit environment.
  /// @param argv The full argv, including argv[0].
  /// @param env The environment lookup.
  /// @param cwd The operator's working directory.
  /// @param db_path The resolved database path.
  /// @param out The stdout stream.
  /// @param err The stderr stream.
  context(std::vector<std::string> argv, env_lookup env, std::filesystem::path cwd, std::filesystem::path db_path,
          std::ostream& out, std::ostream& err)
      : _argv(std::move(argv)), _env(std::move(env)), _cwd(std::move(cwd)), _db_path(std::move(db_path)), _out(&out), _err(&err) {
  }

  context(const context&)            = delete;
  context& operator=(const context&) = delete;
  /// @brief Transfers ownership of `other`'s state, connection included.
  /// @param other The context to move from.
  context(context&& other) noexcept = default;
  /// @brief Transfers ownership of `other`'s state, connection included.
  /// @param other The context to move from.
  /// @return `*this`.
  context& operator=(context&& other) noexcept = default;

  /// @brief Closes the database connection, if one was ever opened.
  ~context() = default;

  /// @brief The full argv, including argv[0].
  /// @return The argument vector.
  [[nodiscard]] auto argv() const -> std::span<const std::string> {
    return _argv;
  }
  /// @brief The environment lookup.
  /// @return The lookup.
  [[nodiscard]] auto env() const -> const env_lookup& {
    return _env;
  }
  /// @brief The operator's working directory.
  /// @return The working directory.
  [[nodiscard]] auto cwd() const -> const std::filesystem::path& {
    return _cwd;
  }
  /// @brief The resolved database path. Reading it does NOT open anything.
  /// @return The database path.
  [[nodiscard]] auto db_path() const -> const std::filesystem::path& {
    return _db_path;
  }
  /// @brief The stdout stream.
  /// @return The stdout stream.
  [[nodiscard]] auto out() const -> std::ostream& {
    return *_out;
  }
  /// @brief The stderr stream.
  /// @return The stderr stream.
  [[nodiscard]] auto err() const -> std::ostream& {
    return *_err;
  }

  /// @brief True once `ensure_db` has actually opened the connection.
  /// @return `true` if the connection is open.
  [[nodiscard]] auto db_opened() const -> bool {
    return _db.has_value();
  }

  /// @brief Open the database in STRICT read-only mode and return the
  /// cached handle. See this file's header for the three observable
  /// consequences (no file creation, no WAL pragma, no migration).
  ///
  /// The cached handle IS the strict read-only one; every caller in this
  /// process that retrieves it gets the same write-refusing connection.
  /// @return The open read-only connection, or the failure as a
  /// `domain_error`.
  auto ensure_db() -> std::expected<db::connection*, domain_error>;

  /// @brief Close the cached read-only connection (if any) and reopen it.
  ///
  /// Works around a SQLite behavior where a long-lived read-only
  /// connection's wrapped read transaction holds a stale snapshot across
  /// another process's `PRAGMA wal_checkpoint(TRUNCATE)` — the pure
  /// read-only handle cannot write its read-mark back into the shared SHM
  /// segment, so it never re-syncs to the truncated WAL header and
  /// already-committed rows stay invisible to it forever. `--follow`'s
  /// wake loop (`handlers::follow::interruptible_sleep`) calls this before
  /// every re-query. Ported from `zig`'s `runtime.refreshDbStrictReadOnly`
  /// (plan 85 t#2623). A reopen failure surfaces as a query error on the
  /// next call to `ensure_db`, rather than silently masking the state.
  /// @return The freshly opened connection, or the failure.
  auto refresh_db() -> std::expected<db::connection*, domain_error> {
    _db.reset();
    return ensure_db();
  }
};

} // namespace planar::cmd::watch
