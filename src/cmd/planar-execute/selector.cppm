/// @file selector.cppm
/// @brief `planar.cmd.planar_execute.selector` — which engine a `run` uses
/// (plan 1033 M0, task 6485, decision 1017 / tech-spec D5).
///
/// Decision 1007 moves workflow execution to a Centurion host; until plan
/// 1033's cutover the embedded runner stays the default and both must be
/// reachable by name. Resolution, first hit wins:
///
///   1. `--engine <embedded|centurion>` on `run`         source `flag: --engine`
///   2. `$PLANAR_EXECUTE_ENGINE` (empty counts as unset)  source `env: PLANAR_EXECUTE_ENGINE`
///   3. `execute.engine` from the sibling `planar config show --json`,
///      whose own provenance label is reported verbatim (`config file`,
///      `embedded default`)
///   4. `embedded`, when the sibling `planar` reports no such key at all
///
/// Step 3 is a process spawn, so it is taken only when 1 and 2 are both
/// absent. The config plane is read through `planar` because this binary
/// may not link a config reader (see `planar.engine_execute`'s
/// `read_planar_config`).
///
/// A bad FLAG value is a usage failure (exit 2, in `parse_run_args`); a bad
/// env or config value, or an unreadable config, is reported here as a
/// one-line reason the caller prints and exits 1 on. The resolution never
/// touches stdout: `run`'s result channel is byte-identical whichever engine
/// was chosen, and `profile show` is the one verb that prints it.
///
/// Every fallible boundary returns `std::expected`; nothing throws.
module;

export module planar.cmd.planar_execute.selector;

import std;
import planar.engine_execute;

namespace planar::cmd::execute {

/// @brief The two execution engines.
export enum class engine_kind : std::uint8_t {
  embedded,  ///< The in-process Lua runner (`planar.engine_execute`).
  centurion, ///< A Centurion host; refused at dispatch until plan 1033 M2 lands it.
};

/// @brief The canonical spelling of `engine`.
/// @param engine The engine.
/// @return `embedded` or `centurion`.
export auto engine_name(engine_kind engine) -> std::string_view;

/// @brief Parse an engine name. Exact, lowercase, no aliases.
/// @param text The candidate.
/// @return The engine, or unset for anything else.
export auto parse_engine(std::string_view text) -> std::optional<engine_kind>;

/// @brief A resolved engine and the provenance that chose it.
export struct engine_choice {
  engine_kind engine = engine_kind::embedded; ///< The engine.
  std::string source;                         ///< Where it came from, e.g. `flag: --engine`.
};

/// @brief Reads `execute.engine` from the config plane; injected so the
/// resolution is testable without a sibling `planar`.
export using config_reader = std::function<std::expected<std::optional<engine::execute::config_value>, std::string>()>;

/// @brief Resolve the engine: flag, then env, then config, then `embedded`.
/// @param flag The already-validated `--engine` value, or unset.
/// @param env The raw `$PLANAR_EXECUTE_ENGINE`, or unset.
/// @param read_config Called only when `flag` and `env` are both absent.
/// @return The choice, or a one-line reason (without the `planar-execute: ` prefix).
export auto resolve_engine(std::optional<std::string_view> flag, std::optional<std::string_view> env,
                           config_reader const& read_config) -> std::expected<engine_choice, std::string>;

/// @brief The `config_reader` the binary uses: the sibling `planar` in `bin_dir`.
/// @param bin_dir The trusted sibling-binary directory.
/// @return The reader.
export auto sibling_config_reader(std::string bin_dir) -> config_reader;

/// @brief Render `profile show`'s payload.
/// @param choice The resolved engine.
/// @param json JSON (`{"engine":…,"engine_source":…}`) rather than `key: value` lines.
/// @return The payload, trailing newline included.
export auto render_profile(engine_choice const& choice, bool json) -> std::string;

} // namespace planar::cmd::execute
