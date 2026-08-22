/// @file effective.cppm
/// @brief `planar.engine.config.effective` — defaults + file + env →
/// effective `config` (tech-spec § "engine buckets", plan 996 task 6032).
///
/// Behavior-preserving port (D2) of
/// `zig/src/engine/config/effective.zig`'s four-layer precedence
/// (captured from that file's own header comment, mirroring Go's
/// `src/internal/config/resolve.go`):
///
///   1. Env var (highest priority)
///   2. Per-association override (when `assoc_slug` is non-null) — the
///      Zig oracle applies this ONLY to the four
///      `external.jira.status.*` keys (`resolve()`'s `assoc_jira_todo` /
///      `_doing` / `_blocked` / `_done` locals feeding `pickStr`'s
///      `assoc_val` argument — no other key reads an assoc override).
///   3. Config file values
///   4. Embedded defaults (lowest priority)
///
/// Scope note (deliberate, not an oversight): the Zig oracle's
/// `resolve()` ALSO walks `models.<vendor>.<tier>` candidate-list keys,
/// `routing.<vendor>.<tier>.<work-type>` keys, and `roles.*`/
/// `role_vendors.*` keys (plan 540/899/586). None of those keys have an
/// embedded default any more — `defaults.toml`'s own test comment says so
/// ("plan 950 removed Planar's model catalog and role->tier map") — and
/// they belong to the tech-spec's separate `engine/ops` "models" bucket
/// (routing-table generation), not this task's config-plane scope (task
/// brief: "Config plane, templates #embed + overrides, init verb"). This
/// port therefore resolves exactly the keys `defaults.toml` actually
/// ships, matching the `config` struct's field set below. Porting the
/// model/routing/role surface is a separate `engine/ops` task.
module;

export module planar.engine.config.effective;

import std;
import planar.engine.config.toml;

namespace planar::engine::config {

/// @brief Which configuration layer supplied a resolved value. Mirrors
/// zig's `Provenance`.
export enum class provenance : std::uint8_t {
  embedded_default, ///< No file/env/assoc value was present; the shipped default won.
  config_file,      ///< `~/.planar/config.toml` supplied the value.
  assoc_override,   ///< A per-association `[associations."<slug>".…]` override supplied the value.
  env,              ///< An environment variable supplied the value.
};

/// @brief A resolved value together with its provenance. `env_var_name` is
/// non-empty only when `source_ == provenance::env`. Mirrors zig's
/// `ValueWithSource` (the plan 899 `candidates` field is not carried here —
/// see this file's header comment on the models/routing scope cut).
export struct value_with_source {
  std::string value;        ///< The resolved value, rendered as text ("true"/"false" for bool, decimal for int).
  provenance  source_;      ///< Which layer supplied `value`.
  std::string env_var_name; ///< The environment variable name, when `source_ == provenance::env`.
};

/// @brief The effective map: flat dotted-key → `value_with_source`.
/// `std::map` keeps keys in ascending lexicographic order natively, so
/// `sorted_keys` below is a plain iteration rather than needing zig's own
/// explicit post-hoc sort of a hash map.
export using effective_map = std::map<std::string, value_with_source, std::less<>>;

/// @brief `[external.jira.status]` — Planar status name → Jira workflow
/// status name.
export struct jira_status {
  std::string todo;    ///< Jira workflow status name for Planar's "todo".
  std::string doing;   ///< Jira workflow status name for Planar's "doing".
  std::string blocked; ///< Jira workflow status name for Planar's "blocked".
  std::string done;    ///< Jira workflow status name for Planar's "done".
};

/// @brief `[external.jira]`.
export struct external_jira {
  std::string base_url;  ///< The Jira instance's base URL.
  std::string user_env;  ///< Name of the env var holding the Jira HTTP Basic-auth username.
  std::string token_env; ///< Name of the env var holding the Jira HTTP Basic-auth token.
  jira_status status;    ///< Planar status → Jira workflow status name mapping.
};

/// @brief `[external.github-issues.status]` — Planar status name → GitHub
/// issue state/label.
export struct github_issues_status {
  std::string todo;  ///< GitHub issue state/label for Planar's "todo".
  std::string doing; ///< GitHub issue state/label for Planar's "doing".
  std::string done;  ///< GitHub issue state/label for Planar's "done".
};

/// @brief `[external.github-issues]`.
export struct external_github_issues {
  std::string          auth;      ///< Auth resolution mode ("token-env" / "gh-cli" / "oauth-stored").
  std::string          token_env; ///< Name of the env var holding the token, used only when `auth == "token-env"`.
  github_issues_status status;    ///< Planar status → GitHub issue state/label mapping.
};

/// @brief `[external.github-projects]`.
export struct external_github_projects {
  std::vector<std::string>
      parent_field_names; ///< Custom-field name candidates probed to discover the "parent" relationship; first match wins.
};

/// @brief `[external]`.
export struct external_config {
  external_jira            jira;            ///< `[external.jira]`.
  external_github_issues   github_issues;   ///< `[external.github-issues]`.
  external_github_projects github_projects; ///< `[external.github-projects]`.
};

/// @brief `[introspection.transcripts]` — per-vendor CLI transcript
/// capture, independently enable/path-overridable.
export struct transcripts_config {
  bool        claude_enabled = true;  ///< Whether the Claude transcript adapter is enabled.
  std::string claude_path;            ///< Operator override path; empty selects the built-in vendor location.
  bool        codex_enabled = true;   ///< Whether the Codex transcript adapter is enabled.
  std::string codex_path;             ///< Operator override path; empty selects the built-in vendor location.
  bool        copilot_enabled = true; ///< Whether the Copilot transcript adapter is enabled.
  std::string copilot_path;           ///< Operator override path; empty selects the built-in vendor location.
};

/// @brief `[introspection]` — opt-in CLI usage logging.
export struct introspection_config {
  bool               cli_log = false;     ///< Opt-in; default off (recording behavior without consent violates least surprise).
  std::int64_t       retention_days = 90; ///< How many days of cli_invocations rows to retain.
  transcripts_config transcripts;         ///< `[introspection.transcripts]`.
};

/// @brief `[defaults]`.
export struct defaults_config {
  std::string vendor; ///< Default vendor identity used when `$PLANAR_VENDOR` is unset.
  std::string scope;  ///< Default scope when no scope is active.
};

/// @brief `[workbench]`.
export struct workbench_config {
  std::string root; ///< Per-feature workbench root; `$PLANAR_WORKBENCH_ROOT` wins if set.
};

/// @brief `[templates]`.
export struct templates_config {
  std::string dir;         ///< Where the operator's JSON template files live.
  std::string default_set; ///< Default template set selected when no association override applies.
};

/// @brief The fully-resolved, typed configuration. Mirrors zig's `Config`
/// field-for-field (minus the models/routing/roles surface — see this
/// file's header comment).
export struct config {
  defaults_config      defaults;      ///< `[defaults]`.
  workbench_config     workbench;     ///< `[workbench]`.
  templates_config     templates;     ///< `[templates]`.
  external_config      external;      ///< `[external]`.
  introspection_config introspection; ///< `[introspection]`.
};

/// @brief Result of `resolve()`: the typed `config` plus the flat
/// per-key provenance map (`config show --effective`'s data source, once a
/// `cmd/` handler exists).
export struct resolved {
  config        cfg;       ///< The resolved, typed configuration.
  effective_map effective; ///< Flat dotted-key → value + provenance, for `config show --effective`.
};

/// @brief Error surface for `resolve()`.
export enum class effective_error : std::uint8_t {
  parse_failed, ///< The embedded defaults or the user's config file failed to parse.
};

/// @brief Injectable environment-variable accessor (mirrors zig's
/// `std.process.Environ` test-injectable parameter to `resolve()`).
export class env_view {
private:
  std::map<std::string, std::string, std::less<>> vars_;

public:
  env_view() = default;

  /// @brief Build a view over an explicit set of variables (tests).
  /// @param vars The "NAME" → value pairs this view reports.
  explicit env_view(std::map<std::string, std::string, std::less<>> vars) : vars_(std::move(vars)) {
  }

  /// @brief A view reporting no variables at all — the "no env" test case.
  /// @return An `env_view` whose `get` always returns `std::nullopt`.
  static auto empty() -> env_view {
    return env_view{};
  }

  /// @brief A view backed by the real process environment (production
  /// callers — `init`/future `cmd/` handlers).
  /// @return An `env_view` whose `get` falls back to `std::getenv`.
  static auto from_process() -> env_view;

  /// @brief Look up `name`. An empty string is treated the same as unset
  /// (mirrors every `pickStr`/`resolveParentFieldNames` call site in the
  /// zig oracle, which all guard `ev.len > 0` before honoring an env
  /// override).
  /// @param name The environment variable name.
  /// @return The value, or `std::nullopt` if unset or empty.
  [[nodiscard]] auto get(std::string_view name) const -> std::optional<std::string>;
};

/// @brief Resolve the effective configuration from the four layers.
/// @param file_content The raw TOML bytes from `~/.planar/config.toml`, or
/// `std::nullopt` if the file does not exist (missing-file and
/// present-but-empty are both legal — both mean "everything falls through
/// to the embedded defaults" for `file_content == std::nullopt`, or
/// "an empty document contributes nothing" for `file_content ==
/// std::optional{""}`).
/// @param env The environment-variable accessor (injectable for tests).
/// @param assoc_slug The active association's slug, or `std::nullopt` for
/// no per-association override (only `external.jira.status.*` reads this
/// — see this file's header comment).
/// @return The resolved config + effective map, or
/// `effective_error::parse_failed` if the embedded defaults or the user's
/// file failed to parse.
export auto resolve(std::optional<std::string_view> file_content, const env_view& env, std::optional<std::string_view> assoc_slug)
    -> std::expected<resolved, effective_error>;

/// @brief Report whether `name` refers to a sensitive value that should be
/// masked in `config show` output. Mirrors zig's `sensitiveName`
/// (case-insensitive; exact match on "token"/"password"/"secret", or a
/// "_token"/"_password"/"_secret"/"_key" suffix).
/// @param name The bare key name (the LAST dotted segment, e.g. "token_env").
/// @return `true` if `name` is sensitive.
export auto sensitive_name(std::string_view name) -> bool;

/// @brief Return every key in `eff`, in ascending lexicographic order.
/// Since `effective_map` is a `std::map`, iteration is already sorted —
/// this exists purely as the named API surface `config show --effective`
/// wants (mirrors zig's `sortedKeys`, which has to sort a hash map
/// explicitly; this port gets the sort for free from the container choice).
/// @param eff The effective map to enumerate.
/// @return Every key, ascending.
export auto sorted_keys(const effective_map& eff) -> std::vector<std::string>;

} // namespace planar::engine::config
