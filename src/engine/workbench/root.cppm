/// @file root.cppm
/// @brief `planar.engine.workbench.root` — resolving `$PLANAR_WORKBENCH_ROOT`
/// (plan 996, task 6037).
///
/// Behavior-preserving port (D2) of `resolveRoot` in
/// zig/src/engine/workbench/sync.zig.
///
/// ## FILESYSTEM SAFETY IS THE WHOLE REASON THIS IS ITS OWN MODULE
///
/// Every other function in this bucket takes an already-resolved absolute
/// root. This is the ONLY one that decides where the workbench lives, and
/// getting it wrong does not fail a test — it writes into, or deletes from,
/// the operator's real `~/.planar/workbench/`. `workbench archive` deletes
/// a whole tree; `workbench gc` unlinks files.
///
/// So the protection is structural, the same mechanism `engine_workflows`
/// and `engine_local` use: `resolve_root` takes an explicit `env_lookup`
/// callable and this module calls `std::getenv` NOWHERE. A test cannot
/// reach a real HOME through it, because the code has no way to find one.
/// The CLI layer supplies the real environment.
///
/// ## Precedence, oracle-verified
///
///   1. `$PLANAR_WORKBENCH_ROOT`, when set and non-empty
///   2. `workbench.root` from the config file at `$PLANAR_CONFIG_PATH`,
///      else `$HOME/.planar/config.toml` — when present, readable, parsed,
///      and NON-EMPTY
///   3. `$HOME/.planar/workbench`
///
/// with `~` and `~/...` expanded against `$HOME` at layers 1 and 2. With
/// none of the three available the result is `unresolved` — never `.`, and
/// never the current directory. Each layer was probed by running
/// `workbench list` under a scratch config and observing which directory
/// the binary created: a `[workbench]` section works, a top-level dotted
/// `workbench.root = "..."` works identically, `~/x` lands under HOME, and
/// an EMPTY value falls through to layer 3.
///
/// ## Why the TOML read is duplicated rather than shared
///
/// `planar.engine.config.toml` already flattens a TOML document into
/// exactly the `workbench.root` key this needs — but `engine_config` is a
/// layer-2 `engine_*` peer and D15/D18 forbid a same-layer edge outright
/// (`cmake/architecture.cmake` FATALs at configure time, it does not warn).
/// D19's remedy (move the shared primitive down to layer 1) is not
/// proportionate here: this module needs ONE string key out of one
/// document, not a TOML front end, and dragging Glaze plus the whole
/// flattening pass down a layer to serve a single lookup would be the
/// layering inverted. So `read_config_workbench_root` below is a narrow,
/// single-key scan, following the precedent `engine_grouping` set when it
/// duplicated its `closures` read rather than depend on `engine_closure`.
/// It is deliberately NOT a TOML parser and does not pretend to be one.
module;

export module planar.engine.workbench.root;

import std;

namespace planar::engine::workbench::root {

/// @brief Reads one environment variable.
///
/// Returns unset for a variable that is not present. A variable present but
/// EMPTY should be reported as an empty string, not as unset — the two are
/// distinguishable and layer 1's "set and non-empty" test depends on it.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief Why the root could not be resolved.
export enum class root_error : std::uint8_t {
  unresolved, ///< No env var, no usable config value, and no `HOME`.
};

/// @brief Resolve the workbench root.
/// @param env The environment accessor (see this file's header — there is
/// no `getenv` fallback, by design).
/// @param read_file Reads a file's whole contents, or returns unset when it
/// is absent or unreadable. Injected for the same reason `env` is: so a
/// test cannot reach a real config file.
/// @return The absolute root path, or `root_error::unresolved`.
export auto resolve_root(const env_lookup&                                                              env,
                         const std::function<std::optional<std::string>(const std::filesystem::path&)>& read_file)
    -> std::expected<std::string, root_error>;

/// @brief Resolve the workbench root, reading config files from the real
/// filesystem.
///
/// The environment is STILL injected — this convenience overload only
/// supplies the file reader.
/// @param env The environment accessor.
/// @return The absolute root path, or `root_error::unresolved`.
export auto resolve_root(const env_lookup& env) -> std::expected<std::string, root_error>;

/// @brief Extract `workbench.root` from a TOML document.
///
/// A narrow single-key scan, not a TOML parser — see this file's header for
/// why it is not `planar.engine.config.toml`. Understands full-line and
/// trailing `#` comments (never inside a quoted value), `[workbench]` table
/// headers with optionally quoted segments, and single- or double-quoted
/// string values. Any other shape yields unset, which falls through to the
/// next precedence layer exactly as an unreadable file would.
/// @param content The config file's bytes.
/// @return The raw (un-tilde-expanded) value, or unset.
export auto read_config_workbench_root(std::string_view content) -> std::optional<std::string>;

/// @brief Expand a leading `~` or `~/` against `$HOME`.
/// @param path The raw path.
/// @param env The environment accessor.
/// @return The expanded path, or `root_error::unresolved` when the path
/// needs `HOME` and `HOME` is absent.
export auto expand_tilde(std::string_view path, const env_lookup& env) -> std::expected<std::string, root_error>;

} // namespace planar::engine::workbench::root
