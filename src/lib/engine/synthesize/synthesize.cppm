/// @file synthesize.cppm
/// @brief Hermetic request staging and validated cache loading for `synthesize`.
module;

export module planar.engine.synthesize;

import std;
import planar.json_dom;

namespace planar::engine::synthesize {

/// @brief Which LLM transport staged the (pending or cached) interpretation.
export enum class provider : std::uint8_t { shell, anthropic, openai };
/// @brief Whether staging is still awaiting the vendor skill or already has
/// a validated cache to reconcile.
export enum class mode : std::uint8_t { pending, cache_hit };
/// @brief Why filesystem staging or cache validation could not proceed.
export enum class error : std::uint8_t { not_found, invalid_input, io };

/// @brief One detected code area (a top-level directory or file group) and
/// the signal evidence gathered for it during staging.
export struct area {
  std::string  name;                  ///< Human-readable area label.
  std::string  path;                  ///< Path relative to the repo root.
  std::int64_t source_files    = 0;   ///< Source files observed in this area.
  std::int64_t test_files      = 0;   ///< Test files observed in this area.
  double       signal_strength = 0.0; ///< Relative weight for ranking areas.
};

/// @brief The result of scanning a repo for synthesis without touching SQLite.
export struct request {
  std::string                                      repo_root;           ///< Canonical repository root.
  std::string                                      repo_slug;           ///< Directory-derived stable slug.
  std::string                                      fingerprint;         ///< Stable request fingerprint.
  std::string                                      readme;              ///< First README heading, or empty.
  std::vector<std::pair<std::string, std::string>> docs;                ///< Relative-path/body pairs for markdown docs.
  std::vector<std::pair<std::string, std::string>> guide_files;         ///< AGENTS/CLAUDE guide relative-path/body pairs.
  std::vector<std::string>                         tree_summary;        ///< Top-level directory entries observed.
  std::string                                      layout;              ///< Detected or operator-supplied code layout.
  std::vector<area>                                areas;               ///< Detected code areas and their evidence.
  std::int64_t                                     total_files = 0;     ///< Total files walked.
  std::int64_t                                     total_lines = 0;     ///< Total lines walked across source files.
  bool                                             has_tests   = false; ///< Whether any test file was observed.
  bool                                             has_ci      = false; ///< Whether a CI config file was observed.
  bool                                             greenfield  = false; ///< Whether the repo has no committed history yet.
  std::string                                      code_layout;         ///< Resolved layout after operator overrides.
};

/// @brief Operator-supplied flags for a `synthesize` invocation.
export struct options {
  std::optional<std::string> provider_override;              ///< Explicit provider override, or unset to resolve normally.
  std::optional<std::string> code_layout;                    ///< Operator-forced layout, bypassing detection.
  bool                       apply                  = false; ///< Whether to reconcile a validated cache into SQLite.
  bool                       treat_as_greenfield    = false; ///< Force greenfield handling regardless of detection.
  bool                       treat_as_nongreenfield = false; ///< Force non-greenfield handling regardless of detection.
};

/// @brief A staged/cached synthesis result visible to the handler.
export struct outcome {
  mode                                mode_     = mode::pending;   ///< Pending staging vs. a validated cache hit.
  provider                            provider_ = provider::shell; ///< The transport that will/did produce the cache.
  request                             request_;                    ///< The deterministic scan this outcome staged.
  std::filesystem::path               cache_path;                  ///< Where a validated result is (or will be) read.
  std::filesystem::path               pending_path;                ///< Where the pending request was (or will be) written.
  std::string                         message;                     ///< Human-readable status line for text output.
  std::optional<json_dom::json_value> result;                      ///< Parsed cache payload, once validated.
};

/// @brief Environment-variable lookup, injected so staging stays testable
/// without touching the real process environment.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief Render a `provider` value as its lowercase wire spelling.
/// @param value The provider to render.
/// @return The lowercase wire spelling (`"shell"`, `"anthropic"`, or `"openai"`).
export auto provider_name(provider value) -> std::string_view;
/// @brief Scan the repo, stage a pending request or load/validate an
/// existing cache, and reconcile it into SQLite when `opts.apply` is set.
/// @param root Existing repository directory.
/// @param planar_home Operator state root for LLM handoff files.
/// @param opts Operator-supplied flags for this invocation.
/// @param env Environment-variable lookup, injected for testability.
/// @return The staged/cached outcome, or a filesystem/validation error.
export auto run(const std::filesystem::path& root, const std::filesystem::path& planar_home, const options& opts,
                const env_lookup& env) -> std::expected<outcome, error>;

} // namespace planar::engine::synthesize
