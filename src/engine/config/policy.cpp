/// @file policy.cpp
/// @brief Implementation of `planar.engine.config.policy` (see policy.cppm).

module planar.engine.config.policy;

import std;
import planar.engine.config.toml;
import planar.json_text;
import planar.sha256;

namespace planar::engine::config {

namespace {

constexpr std::string_view k_boundary_note =
    "policy keys are facts Planar can check at one verb; a key that says what happens next is workflow and is not accepted";

auto is_policy_key(std::string_view name) -> bool {
  return std::ranges::contains(policy_keys, name);
}

auto known_keys_text() -> std::string {
  return std::views::join_with(policy_keys, std::string_view{", "}) | std::ranges::to<std::string>();
}

/// @brief The expected-type phrase for `key`, used in findings.
auto expected_type(std::string_view key) -> std::string_view {
  if (key == "task_gate") {
    return "a non-empty array of strings (an argv, for example [\"make\", \"test\"])";
  }
  if (key == "confirm_before") {
    return "an array of verb paths (for example [\"task done\", \"plan closeout\"])";
  }
  return "a boolean (true or false)";
}

/// @brief Whether `path` is a verb path: lowercase words joined by single spaces.
auto is_verb_path(std::string_view path) -> bool {
  if (path.empty() || path.front() == ' ' || path.back() == ' ') {
    return false;
  }
  char previous = ' ';
  for (const char c : path) {
    const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!word && c != ' ') {
      return false;
    }
    if (c == ' ' && previous == ' ') {
      return false;
    }
    previous = c;
  }
  return true;
}

/// @brief Check that `value` has the type `key` requires.
auto well_typed(std::string_view key, const toml_value& value) -> bool {
  if (key == "task_gate") {
    return value.kind_ == toml_value::kind::array && !value.array_.empty() &&
           std::ranges::none_of(value.array_, [](const std::string& element) { return element.empty(); });
  }
  if (key == "confirm_before") {
    return value.kind_ == toml_value::kind::array && std::ranges::all_of(value.array_, is_verb_path);
  }
  return value.kind_ == toml_value::kind::boolean;
}

/// @brief Store `value` (already well typed) into the field `key` names.
void assign(policy_values& out, std::string_view key, const toml_value& value) {
  if (key == "review_required") {
    out.review_required = value.bool_;
  } else if (key == "task_gate") {
    out.task_gate = value.array_;
  } else if (key == "require_model") {
    out.require_model = value.bool_;
  } else if (key == "confirm_before") {
    out.confirm_before = value.array_;
  } else if (key == "require_scenario_coverage") {
    out.require_scenario_coverage = value.bool_;
  } else if (key == "use_queue") {
    out.use_queue = value.bool_;
  }
}

auto association_section(std::string_view slug) -> std::string {
  return std::format("associations.\"{}\".policy", slug);
}

/// @brief The part of `key` after `prefix`, when `key` starts with it.
auto after(std::string_view key, std::string_view prefix) -> std::optional<std::string_view> {
  if (key.starts_with(prefix)) {
    return key.substr(prefix.size());
  }
  return std::nullopt;
}

/// @brief The slug and policy sub-key of an `associations.<slug>.policy[.<sub>]` flattened key.
struct association_policy_key {
  std::string_view slug; ///< The association slug.
  std::string_view sub;  ///< The text after `.policy.`, empty when the key is the table name itself.
};

auto split_association_key(std::string_view key) -> std::optional<association_policy_key> {
  const auto rest = after(key, "associations.");
  if (!rest.has_value()) {
    return std::nullopt;
  }
  if (rest->ends_with(".policy")) {
    return association_policy_key{.slug = rest->substr(0, rest->size() - std::string_view{".policy"}.size()), .sub = {}};
  }
  const auto at = rest->find(".policy.");
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  return association_policy_key{.slug = rest->substr(0, at), .sub = rest->substr(at + std::string_view{".policy."}.size())};
}

auto refuse(std::string key, std::string_view sub) -> policy_finding {
  if (!is_policy_key(sub)) {
    return policy_finding{.key     = key,
                          .message = std::format("unknown policy key ({}; known keys: {})", k_boundary_note, known_keys_text())};
  }
  return policy_finding{.key = key, .message = std::format("must be {}", expected_type(sub))};
}

/// @brief Resolve one key through the association table, then the top-level one.
void resolve_key(const toml_map& file, std::optional<std::string_view> assoc_slug, std::string_view key, resolved_policy& out) {
  const auto try_section = [&](std::string_view prefix, std::string section, policy_source_kind kind) {
    const auto it = file.find(std::format("{}.{}", prefix, key));
    if (it == file.end() || !well_typed(key, it->second)) {
      return false;
    }
    assign(out.values, key, it->second);
    out.sources[std::string(key)] = policy_key_source{.kind = kind, .section = std::move(section)};
    return true;
  };
  if (assoc_slug.has_value() && !assoc_slug->empty() &&
      try_section(std::format("associations.{}.policy", *assoc_slug), association_section(*assoc_slug),
                  policy_source_kind::association)) {
    return;
  }
  if (try_section("policy", "policy", policy_source_kind::top_level)) {
    return;
  }
  out.sources[std::string(key)] = policy_key_source{};
}

} // namespace

auto validate_policy(const toml_map& doc) -> std::vector<policy_finding> {
  std::vector<policy_finding> findings;
  for (const auto& [key, value] : doc) {
    if (key == "policy") {
      findings.push_back({.key = key, .message = "policy must be a table of policy keys"});
    } else if (const auto sub = after(key, "policy."); sub.has_value()) {
      if (!is_policy_key(*sub) || !well_typed(*sub, value)) {
        findings.push_back(refuse(key, *sub));
      }
    } else if (const auto assoc = split_association_key(key); assoc.has_value()) {
      if (assoc->sub.empty()) {
        findings.push_back({.key = key, .message = "policy must be a table of policy keys"});
      } else if (!is_policy_key(assoc->sub) || !well_typed(assoc->sub, value)) {
        findings.push_back(refuse(key, assoc->sub));
      }
    }
  }
  return findings;
}

auto resolve_policy_lenient(const toml_map& file, std::optional<std::string_view> assoc_slug) -> resolved_policy {
  resolved_policy out;
  for (const auto key : policy_keys) {
    resolve_key(file, assoc_slug, key, out);
  }
  return out;
}

auto effective_policy(const toml_map& file, std::optional<std::string_view> assoc_slug)
    -> std::expected<resolved_policy, std::vector<policy_finding>> {
  if (auto findings = validate_policy(file); !findings.empty()) {
    return std::unexpected(std::move(findings));
  }
  return resolve_policy_lenient(file, assoc_slug);
}

namespace {

auto json_array(const std::vector<std::string>& items) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += ',';
    }
    json_text::append_json_string(out, items[i]);
  }
  out += ']';
  return out;
}

auto json_bool(bool value) -> std::string_view {
  return value ? "true" : "false";
}

} // namespace

auto render_policy_value(const policy_values& values, std::string_view key) -> std::string {
  if (key == "review_required") {
    return std::string{json_bool(values.review_required)};
  }
  if (key == "task_gate") {
    return values.task_gate.has_value() ? json_array(*values.task_gate) : std::string{"(unset)"};
  }
  if (key == "require_model") {
    return std::string{json_bool(values.require_model)};
  }
  if (key == "confirm_before") {
    return json_array(values.confirm_before);
  }
  if (key == "require_scenario_coverage") {
    return std::string{json_bool(values.require_scenario_coverage)};
  }
  if (key == "use_queue") {
    return std::string{json_bool(values.use_queue)};
  }
  return {};
}

auto policy_canonical_json(const policy_values& values) -> std::string {
  std::string out = "{";
  out += std::format("\"confirm_before\":{}", json_array(values.confirm_before));
  out += std::format(",\"require_model\":{}", json_bool(values.require_model));
  out += std::format(",\"require_scenario_coverage\":{}", json_bool(values.require_scenario_coverage));
  out += std::format(",\"review_required\":{}", json_bool(values.review_required));
  out += std::format(",\"task_gate\":{}", values.task_gate.has_value() ? json_array(*values.task_gate) : std::string{"null"});
  out += std::format(",\"use_queue\":{}", json_bool(values.use_queue));
  out += '}';
  return out;
}

auto policy_digest(const policy_values& values) -> std::string {
  return sha256::hex(policy_canonical_json(values));
}

} // namespace planar::engine::config
