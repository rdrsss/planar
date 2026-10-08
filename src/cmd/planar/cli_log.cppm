/// @file cli_log.cppm
/// @brief `planar.cmd.planar.cli_log` — the opt-in `cli_invocations`
/// capture hook (plan 996, task 6073).
///
/// Port target: `zig/src/cmd/planar/cli_log.zig`. One row per top-level
/// `planar` invocation when `[introspection].cli_log` is true, written on
/// the exit path, and COMPLETELY FAIL-OPEN: any failure to record leaves
/// the command's stdout, stderr and exit code byte-identical to the
/// logging-off run.
///
/// ## The privacy invariant, as DERIVED FROM THE ORACLE rather than
/// paraphrased from its comment
///
/// The Zig module states it as "flag VALUES are never recorded". That is
/// true and it is load-bearing, but on its own it does not tell a porter
/// where the line actually falls. The boundary below was derived by
/// running the built oracle against a scratch database with logging on and
/// reading `cli_invocations` back, argv by argv. This table is the
/// PRE-CHANGE oracle transcript, kept as history: since task 7369 the verb
/// slot records only tokens the live CLI tree names (or a structured
/// operand in slot 2), so `resume SENTINEL` now records `resume <unknown>`.
///
///     argv                                    verb_path        args_shape
///     -------------------------------------   --------------   ----------------
///     health                                  health           (empty)
///     task show 6073 --json                   task show        <pos:1> --json
///     plan show my-secret-plan-slug           plan show        <pos:1>
///     task show --plan=SENTINEL               task show        --plan
///     plan show -- SENTINEL extra             plan show        <pos:2>
///     plan show --scope SENTINEL x            plan show        <pos:1> --scope
///     resume SENTINEL                         resume SENTINEL  (empty)
///     tree SENTINEL                           tree SENTINEL    (empty)
///
/// Read off those rows, the contract has three parts, and the third is the
/// one a porter gets wrong:
///
///   1. FLAG VALUES ARE NEVER RECORDED, in all four spellings: a separate
///      following token (`--plan 42`), the inline form (`--plan=42`), and
///      the two short-attached forms (`-p42`, `-p=42`). Only the NAME
///      survives. Note the inline row: the recorded name is `--plan`, with
///      the `=` DROPPED. The Zig comment says "up to (and including) =";
///      the Zig CODE slices `tok[0..eq_pos]`, which excludes it, and the
///      oracle rows agree with the code. This port follows the code.
///   2. POSITIONAL VALUES ARE NEVER RECORDED either. They are COUNTED, and
///      the count is emitted as `<pos:N>`. Everything after a bare `--` is
///      a positional.
///   3. THE VERB SLOT IS RECORDED ONLY WHEN THE LIVE CLI TREE NAMES IT. The
///      first up to `max_verb_depth` (2) non-flag tokens appearing BEFORE
///      any flag are joined into `verb_path`. A token is recorded as typed
///      only when the live tree names it at that depth (`task`, then
///      `add`) or, in slot 2 only, when it is a structured operand (a bare id such as
///      `resume 6073`, an entity ref such as `tree plan:42`); any other
///      token is recorded as `<unknown>`. Flag NAMES follow the same rule:
///      a name is recorded only when the resolved verb (or an ancestor)
///      declares it, else `--<unknown>` (task 7369).
///
/// Structured operands in slot 2 stay verbatim because the oracle recorded
/// them and the shape is bounded to one `word:digits` or digits token. `cli_log.t.cpp` pins the guarantee and the
/// kept exception.
///
/// ## Fail-open, and what that costs
///
/// Every failure arm returns rather than reporting: no config file, an
/// unparseable one, logging off, no database file, a database that will
/// not open, a failed insert. `record` cannot fail. The one thing it must
/// never do is have SIDE EFFECTS, which is why `open_existing_database`
/// below checks for the file first — `db::connection::open` CREATES the
/// file, so logging about a bare `planar --help` on a fresh machine would
/// otherwise leave an empty database behind. It also never MIGRATES: the
/// Zig header records that migrating from the telemetry path silently
/// advanced an operator's live database to a dev build's schema twice.
module;

export module planar.cmd.planar.cli_log;

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;

namespace planar::cmd {

/// @brief The privacy-safe summary of one invocation's arguments.
export struct parsed_args_shape {
  /// @brief The subcommand chain: the first up to two non-flag tokens
  /// before any flag, joined by a space. A token the live CLI tree does not
  /// name, and that is not a structured operand, is `<unknown>` — see this
  /// module's header, part 3.
  std::string verb_path;
  /// @brief Flag NAMES (`--<unknown>` for one the resolved verb does not
  /// declare) and positional ARITY, e.g. `"<pos:1> --plan --json"`.
  /// Flag and positional VALUES are never present here.
  std::string args_shape;
};

/// @brief The number of leading subcommand tokens folded into `verb_path`.
///
/// Planar's CLI has at most two subcommand levels (`task add`, `workbench
/// push`). A non-flag token beyond this is a positional and is counted
/// rather than recorded.
export inline constexpr std::size_t max_verb_depth = 2;

/// @brief Build the privacy-safe shape from argv AFTER the binary name.
///
/// Pure, and deliberately so: the whole privacy invariant lives in this
/// function, and a pure function is one a test can attack directly with
/// hostile argv rather than having to reach it through a process.
/// @param argv_tail The argument vector with argv[0] already dropped.
/// @return The verb path and the value-free argument shape.
export auto parse_args(std::span<const std::string> argv_tail) -> parsed_args_shape;

/// @brief The `cli_invocations.error_category` value for a handler failure.
///
/// Mirrors `zig/src/cmd/planar/cli_log.zig`'s `categoryFor`: the same
/// classification the exit-code table makes, expressed as the category
/// string the migration's CHECK constraint accepts.
/// @param kind The domain-error kind a handler raised.
/// @return The category, or unset when the kind maps to success.
export auto category_for(domain_error_kind kind) -> std::optional<std::string_view>;

/// @brief Write one `cli_invocations` row, pruning expired rows first.
///
/// The testable core of `record`: takes an already-open connection and an
/// already-computed shape, so a Catch2 case can drive it over a scratch
/// database and read the row back. Fail-open like its caller.
/// @param conn An open, already-migrated connection.
/// @param shape The value-free argument shape.
/// @param exit_code The process exit code this invocation will use.
/// @param category The error category, unset on success.
/// @param duration_ms The wall-clock duration, unset to omit it.
/// @param retention_days How many days of rows to keep.
/// @return `true` if a row was inserted.
export auto write_invocation(db::connection& conn, const parsed_args_shape& shape, int exit_code,
                             std::optional<std::string_view> category, std::optional<std::int64_t> duration_ms,
                             std::int64_t retention_days) -> bool;

/// @brief Resolve the config file path: `$PLANAR_CONFIG_PATH` (with a
/// leading `~` expanded), else `$HOME/.planar/config.toml`.
///
/// Ported from `zig/src/cmd/planar/handlers/config/path.zig`'s
/// `resolveConfigPath`. It lives here because `cli_log` is currently its
/// only caller; when `config path` is ported it should call THIS rather
/// than grow a second copy.
/// @param env The environment lookup.
/// @return The path, or unset when neither variable yields one.
export auto resolve_config_path(const env_lookup& env) -> std::optional<std::filesystem::path>;

/// @brief Record one invocation. Fail-open: every failure arm returns
/// silently, and nothing here can change the caller's output or exit code.
///
/// Called from `main` after the verb has returned, so `exit_code` is final.
/// @param ctx The invocation's context.
/// @param exit_code The process exit code.
/// @param kind The domain-error kind that caused the failure, unset on
/// success.
/// @param duration The wall-clock duration of the invocation.
export auto record(context& ctx, int exit_code, std::optional<domain_error_kind> kind,
                   std::optional<std::chrono::milliseconds> duration) -> void;

} // namespace planar::cmd
