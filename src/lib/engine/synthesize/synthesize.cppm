/// @file synthesize.cppm
/// @brief Hermetic request staging and validated cache loading for `synthesize`.
module;

export module planar.engine.synthesize;

import std;
import planar.json_dom;

namespace planar::engine::synthesize {

export enum class provider : std::uint8_t { shell, anthropic, openai };
export enum class mode : std::uint8_t { pending, cache_hit };
export enum class error : std::uint8_t { not_found, invalid_input, io };

export struct area {
  std::string  name;
  std::string  path;
  std::int64_t source_files    = 0;
  std::int64_t test_files      = 0;
  double       signal_strength = 0.0;
};

export struct request {
  std::string                                      repo_root;
  std::string                                      repo_slug;
  std::string                                      fingerprint;
  std::string                                      readme;
  std::vector<std::pair<std::string, std::string>> docs;
  std::vector<std::pair<std::string, std::string>> guide_files;
  std::vector<std::string>                         tree_summary;
  std::string                                      layout;
  std::vector<area>                                areas;
  std::int64_t                                     total_files = 0;
  std::int64_t                                     total_lines = 0;
  bool                                             has_tests   = false;
  bool                                             has_ci      = false;
  bool                                             greenfield  = false;
  std::string                                      code_layout;
};

export struct options {
  std::optional<std::string> provider_override;
  std::optional<std::string> code_layout;
  bool                       apply                  = false;
  bool                       treat_as_greenfield    = false;
  bool                       treat_as_nongreenfield = false;
};

export struct outcome {
  mode                                mode_     = mode::pending;
  provider                            provider_ = provider::shell;
  request                             request_;
  std::filesystem::path               cache_path;
  std::filesystem::path               pending_path;
  std::string                         message;
  std::optional<json_dom::json_value> result;
};

export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

export auto provider_name(provider value) -> std::string_view;
export auto run(const std::filesystem::path& root, const std::filesystem::path& planar_home, const options& opts,
                const env_lookup& env) -> std::expected<outcome, error>;

} // namespace planar::engine::synthesize
