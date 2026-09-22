/// @file profile.cppm
/// @brief `planar.cmd.planar_execute.profile` — execution profiles (plan 1033
/// M0, task 6494; tech-spec D10/D27).
///
/// A profile is the unit a Centurion host is started for: one host per
/// profile, keyed by its canonical state directory. It is configured in the
/// config plane as
///
///     [execute.profiles.<name>]
///     state_dir          = "~/.planar/execute/<name>"   # default shown
///     planar_db          = "~/.planar/planar.db"        # default: $PLANAR_DB, else this
///     allowed_roots      = ["~/code"]                   # default: none
///     idle_grace_seconds = 300                          # default
///     command_policy     = "…"                          # default: unset
///     bundle             = "…"                          # default: unset
///
///     [execute.profiles.<name>.providers.<vendor>]
///     <key> = "…"                                       # passed to Centurion verbatim (D27)
///
/// and read, like every other key `planar-execute` needs, through the sibling
/// `planar config show --json` (whose resolver surfaces every
/// `execute.profiles.*` key the file sets). A profile named `default` always
/// exists: unconfigured, it is exactly the defaults above.
///
/// Identity is canonical: `state_dir`, `planar_db`, `command_policy`,
/// `bundle` and every `allowed_roots` entry are `~`-expanded, made absolute,
/// and passed through `weakly_canonical` (the state directory may not exist
/// yet), so a path and a symlink to it resolve to the same profile identity.
///
/// Every refusal is a one-line reason naming the config file; nothing throws.
module;

export module planar.cmd.planar_execute.profile;

import std;
import planar.engine_execute;

namespace planar::cmd::execute {

/// @brief One resolved execution profile.
export struct profile {
  std::string                name;                     ///< The profile name.
  bool                       configured = false;       ///< The config file sets at least one key for it.
  std::string                state_dir;                ///< Canonical state directory: the profile's identity.
  std::string                planar_db;                ///< Canonical Planar database path.
  std::vector<std::string>   allowed_roots;            ///< Canonical roots, in configured order.
  std::int64_t               idle_grace_seconds = 300; ///< Host idle grace before retirement.
  std::optional<std::string> command_policy;           ///< Canonical command-policy path, when set.
  std::optional<std::string> bundle;                   ///< Canonical workflow-bundle path, when set.
  /// Provider configuration, `vendor -> key -> value`, verbatim.
  std::map<std::string, std::map<std::string, std::string>> providers;
};

/// @brief Environment lookup (`HOME`, `PLANAR_HOME`, `PLANAR_DB`); unset or
/// empty reads as absent.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief Resolve profile `name` from `planar config show --json` entries.
///
/// Validates EVERY configured profile, not only the requested one, so a
/// typo anywhere in the file is reported before it can matter.
/// @param entries The config lines.
/// @param name The requested profile (`default` when `--profile` is absent).
/// @param env Environment lookup.
/// @param config_path Called only to name the file in a refusal.
/// @return The profile, or a one-line reason (without the `planar-execute: ` prefix).
export auto resolve_profile(std::span<const engine::execute::config_entry> entries, std::string_view name, const env_lookup& env,
                            const std::function<std::string()>& config_path) -> std::expected<profile, std::string>;

/// @brief `profile` as a JSON object (no trailing newline), keys in
/// declaration order, unset paths as `null`.
/// @param value The profile.
/// @return The JSON text.
export auto profile_json(const profile& value) -> std::string;

/// @brief `profile` as `key: value` lines, newline-terminated.
/// @param value The profile.
/// @return The text.
export auto profile_text(const profile& value) -> std::string;

} // namespace planar::cmd::execute
