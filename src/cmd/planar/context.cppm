/// @file context.cppm
/// @brief `planar.cmd.planar.context` — everything a handler needs from the
/// process: argv, the environment, the working directory, the two output
/// streams, and the lazily-opened database (plan 996, task 6105).
///
/// Port target: zig/src/runtime/runtime.zig's `Ctx` + `init` + `current` +
/// `ensureDb` + `resolveDbPath`. The CONTRACT is preserved (same fields,
/// same lazy-DB rule, same `$PLANAR_DB` resolution, same PWD-first cwd);
/// the SHAPE deliberately is not, on two counts.
///
/// ## 1. An explicit `context&`, not a process-global singleton
///
/// The Zig runtime is a module-level `var current_ctx: ?Ctx` reached
/// through `runtime.current()`. Two things rule that out here. CLAUDE.md §
/// Zig Style — the house rule this tree inherits — says plainly: "No global
/// mutable state. State threads through explicit `App` / `Store`
/// parameters." And a singleton makes the handler layer untestable in
/// process: a Catch2 binary cannot run two handler invocations with two
/// different environments if the environment is a file-scope variable set
/// once. Every handler here takes `context&` as its first parameter, so a
/// test constructs one over a scratch root and runs the real handler with
/// no process state involved at all.
///
/// ## 2. The environment is a callable, and this is the ONLY module that
/// reads the real one
///
/// `env` is a lookup callable, not a snapshot of `std::getenv`. This is the
/// same structural HOME-safety mechanism `planar.engine.workflows.catalog`,
/// `planar.engine.local.manifest` and `planar.engine.workspace.identity`
/// already use, lifted to the layer that composes them: `process_env()`
/// below is the single function in `src/cmd/planar/` that touches the real
/// process environment, and nothing else in the binary calls `std::getenv`.
///
/// That is not decoration. Commit 3ec6c37 fixed a live defect of exactly
/// this shape one layer down: two `cli` parity tests shelled the reference
/// binary with the INHERITED environment, so `ctest` opened the operator's
/// real `~/.planar/planar.db` on every run — and because the runtime
/// applies pending migrations AUTOMATICALLY on first use, the next
/// migration to land on this branch would have migrated the operator's live
/// database past what every installed binary supports. A `context` cannot
/// do that by accident: it has no way to find `$HOME` unless its `env`
/// callable hands it one, and the test-facing constructor takes a map.
///
/// ## Lazy database acquisition is load-bearing, not an optimisation
///
/// `ensure_db()` opens and migrates on FIRST CALL and caches. Construction
/// does not touch SQLite. Help-only paths (`planar --help`, `planar
/// workflow --help`) and no-DB verbs (`version`, both `workflow` leaves)
/// therefore never open — let alone migrate — a database. Mirrors
/// zig/src/runtime/runtime.zig's own "Lazy DB acquisition" note, and it is
/// what makes it safe for a `--help` invocation to run against a
/// `$PLANAR_DB` pointing anywhere at all.
module;

export module planar.cmd.planar.context;

import std;
import planar.db;
import planar.cmd.planar.exit;

namespace planar::cmd {

/// @brief An environment lookup: variable name in, value or unset out.
///
/// Same signature `planar.engine.workflows.catalog::resolve_dirs`,
/// `planar.engine.local.manifest::resolve_home_and_root` and
/// `planar.engine.workspace.identity::env_lookup` already take, so a
/// `context`'s `env` member passes straight through to any of them with no
/// adapter.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief An `env_lookup` over the REAL process environment.
///
/// The only function in `src/cmd/planar/` that calls `std::getenv`. Every
/// other environment read in the binary goes through a `context`'s `env`
/// member, which a test can point at a scratch map instead.
/// @return A lookup reading the live process environment.
export auto process_env() -> env_lookup;

/// @brief An `env_lookup` over an explicit map — the test-facing
/// counterpart to `process_env`.
/// @param vars The variables to expose; anything absent reads as unset.
/// @return A lookup over a copy of `vars`.
export auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup;

/// @brief Resolve the database path exactly as
/// zig/src/runtime/runtime.zig's `resolveDbPath` does: `$PLANAR_DB` when
/// set, otherwise `$HOME/.planar/planar.db`.
///
/// `$PLANAR_HOME` is NOT consulted, and that is not an oversight — the Zig
/// original reads `PLANAR_DB` then `HOME` and nothing else. CLAUDE.md calls
/// mistaking one for the other "a common and costly mistake"; reproducing
/// the real precedence is the point.
/// @param env The environment to resolve from.
/// @return The resolved path, or `domain_error` when neither `$PLANAR_DB`
/// nor `$HOME` is set (the Zig original's `error.HomeNotSet`).
export auto resolve_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, domain_error>;

/// @brief The operator's working directory, PWD-first.
///
/// `$PWD` wins over `std::filesystem::current_path()`, mirroring
/// zig/src/cmd/planar/scope.zig's `operatorCwd` and, behind it, Go's
/// `os.Getwd`. The reason is recorded at plan 351 task 2375 and is real on
/// this platform: canonicalising resolves macOS's `/var` -> `/private/var`
/// symlink, so a project registered under `/var/...` would stop matching
/// the `projects.root_path` key `assoc add` wrote, and cwd-derived scope
/// would silently resolve to nothing.
/// @param env The environment to read `$PWD` from.
/// @return The working directory.
export auto operator_cwd(const env_lookup& env) -> std::filesystem::path;

/// @brief One invocation's process state, threaded explicitly through
/// dispatch into every handler.
///
/// Move-only, because it owns the database connection. Nothing that varies
/// per invocation lives anywhere else: `--json`, `--scope` and friends stay
/// in the parsed `cli::match_result` and are read at the call site, exactly
/// as zig/src/runtime/runtime.zig's own "Things explicitly NOT on Ctx" note
/// requires.
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
  /// @brief The environment lookup, for passing to engine surfaces that
  /// take one.
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
  /// Exists so a test can assert the NEGATIVE — that a `--help` path or a
  /// no-DB verb left SQLite untouched. That is the only way to prove the
  /// lazy-acquisition rule holds rather than assuming it, and a regression
  /// there is invisible in output.
  /// @return `true` if the connection is open.
  [[nodiscard]] auto db_opened() const -> bool {
    return _db.has_value();
  }

  /// @brief Open the database if it is not open yet, create its parent
  /// directory, apply pending migrations, verify the schema version, and
  /// return the cached handle. Subsequent calls are O(1).
  ///
  /// The post-migration version check reproduces the Zig runtime's
  /// `SchemaVersionAhead` guard: after `apply_all`, a stored version HIGHER
  /// than the newest migration this binary embeds means a newer binary
  /// migrated this database and this one is stale. `planar` maps that to
  /// exit 7 (`planar.cli.exit`; note `schema_version_behind` is NOT the
  /// same code on this binary — see that module's header).
  /// @return The open connection, or the failure as a `domain_error`.
  auto ensure_db() -> std::expected<db::connection*, domain_error>;
};

} // namespace planar::cmd
