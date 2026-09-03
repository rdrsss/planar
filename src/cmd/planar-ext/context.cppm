/// @file context.cppm
/// @brief `planar.cmd.planar_ext.context` — everything a `planar-ext`
/// handler needs from the process, and this binary's database-acquisition
/// AND write-capability policy (plan 996, task 6418).
///
/// ## Schema policy: a CONSUMER, like `planar-agent`
///
/// `ensure_db()` opens read/write, creates the parent directory, and NEVER
/// applies migrations — `planar init` is the only verb that migrates; every
/// other binary consumes the schema it finds. It refuses on skew in BOTH
/// directions (`schema_version_behind` / `schema_version_ahead`), exactly
/// like `planar.cmd.planar_agent.context`. See that module's header for the
/// full argument; it is not repeated here.
///
/// ## Write-capability policy: THE POINT OF THIS BINARY (decision 995)
///
/// Immediately after opening, `ensure_db()` calls
/// `db::connection::restrict_writes_to` with EXACTLY the three tables
/// decision 995 names writable from this binary: `external_links`,
/// `external_systems`, `sync_events`. Every planning table stays
/// READ-only through this connection — not by omission (no verb happens
/// to write it), but ENFORCED: SQLite's own authorizer denies any
/// `INSERT`/`UPDATE`/`DELETE` against any other table at prepare time,
/// regardless of how the SQL was composed. See `restrict_writes_to`'s own
/// header for why that matters here specifically — `src/lib/engine/
/// external/sync.cpp`'s `table_for`-interpolated `UPDATE` is exactly the
/// shape a source-text boundary check cannot see, and this one does not
/// need to see it: it inspects the PARSED statement.
///
/// This is a deliberate divergence from the other three binaries, whose
/// capability boundary is their VERB SET alone (CLAUDE.md § "Four-binary
/// boundary": "the capability boundary is each binary's verb set, not
/// runtime ACLs"). `planar-ext` adds a second, independent enforcement
/// layer at the connection level, because task 6412/decision 995's whole
/// premise is that the code landing here (~10,700 lines, tasks 6419-6421)
/// is being extracted FROM a codebase where a write surface was measured
/// wrong once already by trusting source text. The runtime authorizer
/// makes that class of mistake structurally impossible to ship unnoticed:
/// any future handler that writes the wrong table fails loudly at
/// `prepare()`, not silently in a review someone forgot to reread.
module;

export module planar.cmd.planar_ext.context;

import std;
import planar.db;
import planar.cmd.planar_ext.exit;

namespace planar::cmd::ext {

/// @brief An environment lookup: variable name in, value or unset out.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief An `env_lookup` over the REAL process environment.
/// @return A lookup reading the live process environment.
export auto process_env() -> env_lookup;

/// @brief An `env_lookup` over an explicit map — the test-facing
/// counterpart to `process_env`.
/// @param vars The variables to expose; anything absent reads as unset.
/// @return A lookup over a copy of `vars`.
export auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup;

/// @brief Resolve the database path: `$PLANAR_DB` when set, otherwise
/// `$HOME/.planar/planar.db`. `$PLANAR_HOME` is deliberately NOT
/// consulted.
/// @param env The environment to resolve from.
/// @return The resolved path, or `domain_error` when neither is set.
export auto resolve_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, domain_error>;

/// @brief The operator's working directory, PWD-first.
/// @param env The environment to read `$PWD` from.
/// @return The working directory.
export auto operator_cwd(const env_lookup& env) -> std::filesystem::path;

/// @brief The exact set of tables `planar-ext` may write to (decision
/// 995). Exposed so the capability-boundary test drives the SAME list
/// `ensure_db()` installs, rather than a hand-copied duplicate that could
/// drift from it silently.
/// @return The write allowlist.
export auto write_allowlist() -> std::vector<std::string>;

/// @brief One `planar-ext` invocation's process state.
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
  /// @brief The stdout stream. Renderer payloads are written here verbatim.
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

  /// @brief Open the database as a schema CONSUMER, restrict it to the
  /// decision-995 write allowlist, and return the cached handle. See this
  /// file's header.
  /// @return The open connection, or the failure as a `domain_error`.
  auto ensure_db() -> std::expected<db::connection*, domain_error>;
};

} // namespace planar::cmd::ext
