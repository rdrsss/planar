/// @file policy.cppm
/// @brief `planar.engine.config.policy` — the per-scope policy keys of
/// `config.toml` (plan 1142, tech spec component 6, "Policy").
///
/// Shape: a top-level `[policy]` table and per-association
/// `[associations."<slug>".policy]` tables of flat keys. Resolution per key:
/// association, then top level, then the embedded default. NO environment
/// variable participates, by design: the agent being policed must not be able
/// to switch a check off from its own environment. This module takes no
/// environment accessor at all, so no later change can add one by accident.
///
/// Boundary rule: a key is admitted only if it is a fact Planar can check at
/// one verb, or only displays. A key that says what happens next (a step, a
/// retry, a branch or a condition) is workflow and is refused, as is any
/// unknown key. `validate_policy` is the refusal: it names every offending key.
///
/// Keys (type, default): `review_required` (bool, false), `task_gate` (non-empty
/// argv array of strings, unset), `require_model` (bool, false),
/// `confirm_before` (array of verb paths such as `"task done"`, empty),
/// `require_scenario_coverage` (bool, false), `use_queue` (bool, false).
///
/// Entry points: `validate_policy` (every finding, for `planar config
/// validate`), `effective_policy` (the typed result plus per-key sources, which
/// refuses an invalid document), `resolve_policy_lenient` (the same resolution
/// that falls through a mistyped value, for display) and `policy_digest`.
///
/// Enforcement is not here: `complete`, `claim`, `pull` and `spec ingest` read
/// the struct in their own modules. Error boundary: `std::expected`; nothing
/// throws.
module;

export module planar.engine.config.policy;

import std;
import planar.engine.config.toml;

namespace planar::engine::config {

/// @brief The policy in force for one scope.
export struct policy_values {
  bool                                    review_required = false; ///< `complete` requires an approving review row.
  std::optional<std::vector<std::string>> task_gate;               ///< Argv a recorded queued run must match; unset when no gate.
  bool                                    require_model = false;   ///< A coder `claim` / `pull` must name a model.
  std::vector<std::string>                confirm_before;          ///< Verb paths needing operator confirmation (display only).
  bool require_scenario_coverage = false;                          ///< `spec ingest --strict` requires scenario coverage.
  bool use_queue                 = false;                          ///< Builds and tests go through the host queue.

  /// @brief Member-wise equality.
  /// @return Whether every field is equal.
  auto operator==(const policy_values&) const -> bool = default;
};

/// @brief Which layer supplied one policy key.
export enum class policy_source_kind : std::uint8_t {
  embedded_default, ///< The shipped default won.
  top_level,        ///< The top-level `[policy]` table supplied the value.
  association,      ///< An `[associations."<slug>".policy]` table supplied the value.
};

/// @brief Where one policy key's value came from.
export struct policy_key_source {
  policy_source_kind kind = policy_source_kind::embedded_default; ///< The winning layer.
  /// The TOML table that supplied the value: `policy` or
  /// `associations."<slug>".policy`. Empty for the embedded default.
  std::string section;
};

/// @brief One refused policy value or key.
export struct policy_finding {
  std::string key;     ///< The dotted key as written, for example `policy.after_review`.
  std::string message; ///< What is wrong and what would be accepted.
};

/// @brief The typed policy plus the source of every key.
export struct resolved_policy {
  policy_values values; ///< The resolved values.
  std::map<std::string, policy_key_source, std::less<>>
      sources; ///< Bare key name (for example `use_queue`) to its source; one entry per key in `policy_keys`.
};

/// @brief The admitted policy keys, in the order every listing prints them.
export inline constexpr std::array<std::string_view, 6> policy_keys = {
    "review_required", "task_gate", "require_model", "confirm_before", "require_scenario_coverage", "use_queue",
};

/// @brief Check every `policy.*` and `associations.<slug>.policy.*` key of a
/// parsed document: the key must be admitted and its value must have the
/// key's type.
/// @param doc The flattened document from `parse_toml`.
/// @return Every refused key, ascending by key; empty when the policy tables are valid or absent.
export auto validate_policy(const toml_map& doc) -> std::vector<policy_finding>;

/// @brief Resolve the policy for one scope, refusing an invalid document.
/// @param file The user's flattened config file (an empty map for no file).
/// @param assoc_slug The association whose `policy` table applies, or unset for top level only.
/// @return The values and sources, or every finding `validate_policy` reports.
export auto effective_policy(const toml_map& file, std::optional<std::string_view> assoc_slug)
    -> std::expected<resolved_policy, std::vector<policy_finding>>;

/// @brief Resolve the policy for one scope, falling through a mistyped value to
/// the next layer instead of refusing. Used for display, where `config
/// validate` is what reports the mistake.
/// @param file The user's flattened config file (an empty map for no file).
/// @param assoc_slug The association whose `policy` table applies, or unset for top level only.
/// @return The values and sources.
export auto resolve_policy_lenient(const toml_map& file, std::optional<std::string_view> assoc_slug) -> resolved_policy;

/// @brief Render one key of `values` as text, the way `config show` prints it:
/// `true` or `false`, a JSON array of strings, or `(unset)` for an absent `task_gate`.
/// @param values The values.
/// @param key A name from `policy_keys`.
/// @return The text; empty for a name that is not a policy key.
export auto render_policy_value(const policy_values& values, std::string_view key) -> std::string;

/// @brief The canonical JSON serialization of `values`: object keys in a fixed
/// (alphabetical) order, no whitespace, `task_gate` as `null` when unset, and
/// every array in its given element order.
/// @param values The values.
/// @return The JSON text.
export auto policy_canonical_json(const policy_values& values) -> std::string;

/// @brief The digest a claim records for the policy it ran under: SHA-256 over
/// `policy_canonical_json`, as 64 lowercase hex characters.
/// @param values The values.
/// @return The digest.
export auto policy_digest(const policy_values& values) -> std::string;

} // namespace planar::engine::config
