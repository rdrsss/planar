/// @file context.cppm
/// @brief `planar.cmd.planar_agent.context` — everything a `planar-agent`
/// handler needs from the process, and the CONSUMER database-acquisition
/// policy that distinguishes this binary from the operator one (plan 996,
/// task 6107).
///
/// Port target: zig/src/runtime/runtime.zig's `Ctx` plus
/// `ensureDbConsumer` (NOT `ensureDb`).
///
/// ## The database policy is the point of this file
///
/// `planar.cmd.planar.context::ensure_db()` opens, CREATES the parent
/// directory, APPLIES pending migrations, and only then checks the
/// version. This one deliberately does none of the first three. Quoting
/// zig/src/runtime/runtime.zig directly: "`planar init` is the only verb
/// that applies migrations; every other binary is a consumer of the
/// schema, not its owner."
///
/// So `ensure_db()` here:
///
///   1. opens read/write (agent verbs DO write — `agent_actions`,
///      `agent_work_claims`, the `routing_dispatch_*` tables; this binary
///      is not the read-only one); `journal_mode = WAL` and
///      `busy_timeout = 5000` are set once, centrally, by
///      `db::connection::open` itself (task 6842, decision D7) rather
///      than duplicated here,
///   2. NEVER calls `apply_all`, and
///   3. refuses BOTH directions of schema skew — `schema_version_behind`
///      (live DB older than this binary's embedded max; the operator runs
///      `planar init`) and `schema_version_ahead` (live DB newer; the
///      operator upgrades the binary) — writing the Zig original's
///      remediation line to stderr before returning.
///
/// The two-directional refusal is not shared with `planar` either: that
/// binary has no `schema_version_behind` path at all, because it migrates
/// forward instead. This is the same divergence
/// `planar.cmd.planar_agent.exit` maps to a different EXIT CODE (7 here,
/// 1 there), and the two halves have to agree — a context that never
/// raised the kind would make that exit-code row unreachable and its test
/// vacuous.
///
/// ## Everything else is the shape task 6105 settled
///
/// An explicit `context&` rather than the Zig runtime's process-global
/// singleton (CLAUDE.md § Zig Style forbids global mutable state, and a
/// singleton makes in-process handler tests impossible); an environment
/// LOOKUP CALLABLE rather than a `std::getenv` snapshot, with
/// `process_env()` the only function in this directory that reads the real
/// process environment. See `planar.cmd.planar.context`'s header for the
/// full argument and for the live defect (commit 3ec6c37) that motivated
/// the second.
module;

export module planar.cmd.planar_agent.context;

import std;
import planar.db;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {

/// @brief An environment lookup: variable name in, value or unset out.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief An `env_lookup` over the REAL process environment.
///
/// The only function in `src/cmd/planar-agent/` that calls `std::getenv`.
/// @return A lookup reading the live process environment.
export auto process_env() -> env_lookup;

/// @brief An `env_lookup` over an explicit map — the test-facing
/// counterpart to `process_env`.
/// @param vars The variables to expose; anything absent reads as unset.
/// @return A lookup over a copy of `vars`.
export auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup;

/// @brief Resolve the database path: `$PLANAR_DB` when set, otherwise
/// `$HOME/.planar/planar.db`. `$PLANAR_HOME` is deliberately NOT consulted
/// (CLAUDE.md calls confusing the two "a common and costly mistake").
/// @param env The environment to resolve from.
/// @return The resolved path, or `domain_error` when neither is set.
export auto resolve_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, domain_error>;

/// @brief The operator's working directory, PWD-first (see
/// `planar.cmd.planar.context::operator_cwd` for why canonicalising is
/// wrong on this platform).
/// @param env The environment to read `$PWD` from.
/// @return The working directory.
export auto operator_cwd(const env_lookup& env) -> std::filesystem::path;

/// @brief One `planar-agent` invocation's process state.
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
  ///
  /// Exists so a test can assert the NEGATIVE — that `version`, `schema`
  /// and every `--help` path left SQLite untouched.
  /// @return `true` if the connection is open.
  [[nodiscard]] auto db_opened() const -> bool {
    return _db.has_value();
  }

  /// @brief Open the database as a schema CONSUMER and return the cached
  /// handle: read/write at the driver level, but never migrating, and
  /// refusing on skew in either direction. See this file's header.
  ///
  /// Writes the Zig original's remediation line to `err()` before
  /// returning a skew failure, so the operator sees the fix and not only
  /// the error tag.
  /// @return The open connection, or the failure as a `domain_error`.
  auto ensure_db() -> std::expected<db::connection*, domain_error>;
};

} // namespace planar::cmd::agent
