/// @file effective.cpp
/// @brief Implementation of `planar.engine.config.effective` (see
/// effective.cppm). `defaults.toml` is embedded via `#embed` — same
/// mechanism as `planar.db.migrations` (D5), just a single fixed file
/// rather than a configure-time-enumerated set, so no generated
/// implementation unit is needed for this half of the module.

module;

#include <cstdlib>

module planar.engine.config.effective;

import std;
import planar.engine.config.toml;

namespace planar::engine::config {

namespace {

constexpr unsigned char k_defaults_toml_bytes[] = {
#embed "defaults.toml"
};

// Not `constexpr`: built via `reinterpret_cast` over a `#embed`'d byte
// array, which is not a core constant expression — same rationale as
// migrations.generated.cpp / templates_embed.generated.cpp's `k_all`. The
// array itself is still statically allocated and this view is dynamically
// initialized exactly once, before main().
const std::string_view k_defaults_toml(reinterpret_cast<const char*>(k_defaults_toml_bytes), sizeof(k_defaults_toml_bytes));

/// @brief Render a `toml_value` as text the way zig's `pickStr` does when
/// a string-typed key happens to land on a bool/string entry: string
/// as-is, bool as "true"/"false", int/array as "" (not expected for a
/// string-typed field).
auto as_text(const toml_value& v) -> std::string {
  switch (v.kind_) {
  case toml_value::kind::string:
    return v.string_;
  case toml_value::kind::boolean:
    return v.bool_ ? "true" : "false";
  case toml_value::kind::integer:
  case toml_value::kind::array:
    return "";
  }
  return ""; // unreachable — every enumerator handled above.
}

/// @brief Record `value` + its provenance under `key` in `eff`, overwriting
/// any prior entry (mirrors every `getOrPut` + free-and-overwrite dance in
/// the zig oracle — trivial here since `std::string` owns its own memory).
void record(effective_map& eff, std::string_view key, std::string value, provenance source, std::string env_var_name = "") {
  eff[std::string(key)] =
      value_with_source{.value = std::move(value), .source_ = source, .env_var_name = std::move(env_var_name)};
}

/// @brief Resolve a single string-typed key across env → assoc → file →
/// default, recording provenance in `eff`. Mirrors zig's `pickStr`.
/// @param key The dotted effective-map key to record under.
/// @param env_name The environment variable to check first, or empty for none.
/// @param assoc_val An already-looked-up per-association override value, or empty for none.
/// @param file_map The parsed user config file (may be empty).
/// @param def_map The parsed embedded defaults.
/// @param eff The effective map to record provenance into.
/// @return The resolved value ("" if the key was absent at every layer — no provenance entry is recorded in that case).
auto pick_str(std::string_view key, std::string_view env_name, const env_view& env, const std::optional<std::string>& assoc_val,
              const toml_map& file_map, const toml_map& def_map, effective_map& eff) -> std::string {
  if (!env_name.empty()) {
    if (auto ev = env.get(env_name); ev.has_value() && !ev->empty()) {
      record(eff, key, *ev, provenance::env, std::string(env_name));
      return *ev;
    }
  }
  if (assoc_val.has_value() && !assoc_val->empty()) {
    record(eff, key, *assoc_val, provenance::assoc_override);
    return *assoc_val;
  }
  if (auto it = file_map.find(key); it != file_map.end()) {
    auto text = as_text(it->second);
    if (!text.empty()) {
      record(eff, key, text, provenance::config_file);
      return text;
    }
  }
  if (auto it = def_map.find(key); it != def_map.end()) {
    auto text = as_text(it->second);
    if (!text.empty()) {
      record(eff, key, text, provenance::embedded_default);
      return text;
    }
  }
  return "";
}

/// @brief Resolve a bool-typed key: file, then default. Mirrors zig's
/// `pickBool`.
auto pick_bool(std::string_view key, const toml_map& file_map, const toml_map& def_map, effective_map& eff, bool default_val)
    -> bool {
  if (auto it = file_map.find(key); it != file_map.end()) {
    const bool b = it->second.kind_ == toml_value::kind::boolean  ? it->second.bool_
                   : it->second.kind_ == toml_value::kind::string ? it->second.string_ == "true"
                                                                  : default_val;
    record(eff, key, b ? "true" : "false", provenance::config_file);
    return b;
  }
  if (auto it = def_map.find(key); it != def_map.end()) {
    const bool b = it->second.kind_ == toml_value::kind::boolean  ? it->second.bool_
                   : it->second.kind_ == toml_value::kind::string ? it->second.string_ == "true"
                                                                  : default_val;
    record(eff, key, b ? "true" : "false", provenance::embedded_default);
    return b;
  }
  return default_val;
}

/// @brief Resolve an int-typed key: file, then default. Mirrors zig's
/// `pickInt`.
auto pick_int(std::string_view key, const toml_map& file_map, const toml_map& def_map, effective_map& eff,
              std::int64_t default_val) -> std::int64_t {
  if (auto it = file_map.find(key); it != file_map.end()) {
    const std::int64_t n = it->second.kind_ == toml_value::kind::integer ? it->second.int_ : default_val;
    record(eff, key, std::to_string(n), provenance::config_file);
    return n;
  }
  if (auto it = def_map.find(key); it != def_map.end()) {
    const std::int64_t n = it->second.kind_ == toml_value::kind::integer ? it->second.int_ : default_val;
    record(eff, key, std::to_string(n), provenance::embedded_default);
    return n;
  }
  return default_val;
}

/// @brief Split `s` by `delim`, trimming " \t" from each part and dropping
/// empty parts. Mirrors zig's `splitAndTrim` (used for the
/// `PLANAR_GITHUB_PROJECTS_PARENT_FIELDS` env var).
auto split_and_trim(std::string_view s, char delim) -> std::vector<std::string> {
  std::vector<std::string> parts;
  std::size_t              start = 0;
  while (start <= s.size()) {
    const auto end           = s.find(delim, start);
    const auto part          = s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    const auto trimmed_begin = part.find_first_not_of(" \t");
    if (trimmed_begin != std::string_view::npos) {
      const auto trimmed_end = part.find_last_not_of(" \t");
      parts.emplace_back(part.substr(trimmed_begin, trimmed_end - trimmed_begin + 1));
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return parts;
}

/// @brief Resolve `external.github-projects.parent_field_names`: env
/// (comma-separated) → file array → default array. Mirrors zig's
/// `resolveParentFieldNames`.
auto resolve_parent_field_names(const env_view& env, const toml_map& file_map, const toml_map& def_map, effective_map& eff)
    -> std::vector<std::string> {
  constexpr std::string_view key = "external.github-projects.parent_field_names";

  if (auto ev = env.get("PLANAR_GITHUB_PROJECTS_PARENT_FIELDS"); ev.has_value() && !ev->empty()) {
    auto fields = split_and_trim(*ev, ',');
    record(eff, key, std::views::join_with(fields, std::string_view{", "}) | std::ranges::to<std::string>(), provenance::env,
           "PLANAR_GITHUB_PROJECTS_PARENT_FIELDS");
    return fields;
  }

  if (auto it = file_map.find(key);
      it != file_map.end() && it->second.kind_ == toml_value::kind::array && !it->second.array_.empty()) {
    const auto& arr = it->second.array_;
    record(eff, key, std::views::join_with(arr, std::string_view{", "}) | std::ranges::to<std::string>(),
           provenance::config_file);
    return arr;
  }

  if (auto it = def_map.find(key);
      it != def_map.end() && it->second.kind_ == toml_value::kind::array && !it->second.array_.empty()) {
    const auto& arr = it->second.array_;
    record(eff, key, std::views::join_with(arr, std::string_view{", "}) | std::ranges::to<std::string>(),
           provenance::embedded_default);
    return arr;
  }

  return {};
}

/// @brief Store `raw` as the candidate list for `key`, with
/// `candidates[0]` also landing in `.value` so every scalar-compatible
/// reader is unaffected — D5's "list[0] is the tier default" holds by
/// construction. Mirrors zig's `storeModelTierCandidates`.
void store_model_tier_candidates(effective_map& eff, std::string_view key, std::vector<std::string> raw, provenance source) {
  auto value = raw.front();
  eff[std::string(key)] =
      value_with_source{.value = std::move(value), .source_ = source, .env_var_name = "", .candidates = std::move(raw)};
}

/// @brief Resolve a `models.<vendor>.<tier>` key as a candidate list (plan
/// 899 D3/D5): the value may be a scalar (one candidate) or an array (an
/// ordered list) at either layer, file wins over default. Records nothing
/// when the key is absent or empty at both layers, mirroring `pick_str`'s
/// "nothing found → no provenance entry". Mirrors zig's
/// `pickModelTierCandidates`.
void pick_model_tier_candidates(std::string_view key, const toml_map& file_map, const toml_map& def_map, effective_map& eff) {
  const auto try_layer = [&](const toml_map& map, provenance source) {
    auto it = map.find(key);
    if (it == map.end()) {
      return false;
    }
    if (it->second.kind_ == toml_value::kind::string && !it->second.string_.empty()) {
      store_model_tier_candidates(eff, key, {it->second.string_}, source);
      return true;
    }
    if (it->second.kind_ == toml_value::kind::array && !it->second.array_.empty()) {
      store_model_tier_candidates(eff, key, it->second.array_, source);
      return true;
    }
    // An int/bool-valued tier key falls through to the next layer, exactly
    // as the oracle's `.int, .bool => {}` arm does.
    return false;
  };
  if (try_layer(file_map, provenance::config_file)) {
    return;
  }
  static_cast<void>(try_layer(def_map, provenance::embedded_default));
}

/// @brief Resolve the models / routing / roles / role_vendors surface into
/// `eff`. Mirrors `effective.zig:515-608` step for step, including the
/// custom-role sweep (plan 586 task 3937) that picks any non-built-in
/// `roles.<name>` / `role_vendors.<name>` present in the config file.
/// Purely a side effect on `eff`: the oracle returns nothing from this
/// block either, and none of these keys is a `config` struct field.
void resolve_models_routing_roles(const env_view& env, const toml_map& file_map, const toml_map& def_map, effective_map& eff) {
  // Model tier maps. No env override and no per-association override in
  // v1, so `pick_model_tier_candidates` takes neither.
  for (const auto vendor : vendors) {
    for (const auto tier : tiers) {
      pick_model_tier_candidates(std::format("models.{}.{}", vendor, tier), file_map, def_map, eff);
    }
  }

  // Work-type routing map (plan 899 D4/D7/D9/D10/D11): a plain candidate
  // model id string, never a list index (D10). A work type absent from
  // both layers is simply not recorded — callers treat a routing-map miss
  // as "fall back to the tier default".
  for (const auto vendor : vendors) {
    for (const auto tier : tiers) {
      for (const auto work_type : work_types) {
        static_cast<void>(
            pick_str(std::format("routing.{}.{}.{}", vendor, tier, work_type), "", env, std::nullopt, file_map, def_map, eff));
      }
    }
  }

  // Role→tier (plan 540), scalar-only. `role_vendors.*` are override-only
  // (no embedded default → the resolver falls back to `[defaults].vendor`);
  // they are picked so an operator-set value resolves.
  for (const auto role : builtin_roles) {
    static_cast<void>(pick_str(std::format("roles.{}", role), "", env, std::nullopt, file_map, def_map, eff));
    static_cast<void>(pick_str(std::format("role_vendors.{}", role), "", env, std::nullopt, file_map, def_map, eff));
  }

  // User-defined custom roles (plan 586 task 3937): any `roles.<name>` /
  // `role_vendors.<name>` in the config file that is NOT a built-in gets
  // picked so `buildRouting` can enumerate it. File-only — no env
  // override, no embedded-default counterpart.
  for (const auto& [file_key, unused] : file_map) {
    std::string_view suffix;
    if (file_key.starts_with("roles.")) {
      suffix = std::string_view{file_key}.substr(std::string_view{"roles."}.size());
    } else if (file_key.starts_with("role_vendors.")) {
      suffix = std::string_view{file_key}.substr(std::string_view{"role_vendors."}.size());
    } else {
      continue;
    }
    if (suffix.empty() || std::ranges::contains(builtin_roles, suffix)) {
      continue;
    }
    static_cast<void>(pick_str(file_key, "", env, std::nullopt, file_map, def_map, eff));
  }
}

/// @brief Look up `associations.<slug>.<subkey>` in `file_map`. Only the
/// bare (unquoted) dotted form is checked — the zig oracle's second lookup
/// attempt (a quoted `associations."<slug>".<subkey>"` form) is dead code
/// there: its own table-header parser (`parseKeySegment`) always strips
/// quoting before joining segments with '.', so the quoted form never
/// actually appears as a stored key. `toml.cppm`'s flattener has the same
/// property (Glaze's object keys never retain source quoting) — see
/// toml.cppm's header comment.
auto assoc_str(const toml_map& file_map, std::optional<std::string_view> slug, std::string_view subkey)
    -> std::optional<std::string> {
  if (!slug.has_value()) {
    return std::nullopt;
  }
  const auto key = std::format("associations.{}.{}", *slug, subkey);
  if (auto it = file_map.find(key); it != file_map.end() && it->second.kind_ == toml_value::kind::string) {
    return it->second.string_;
  }
  return std::nullopt;
}

} // namespace

auto env_view::get(std::string_view name) const -> std::optional<std::string> {
  if (auto it = vars_.find(name); it != vars_.end()) {
    return it->second.empty() ? std::nullopt : std::optional<std::string>{it->second};
  }
  // Fall back to the real process environment ONLY for from_process()
  // views. empty() and the explicit-map constructor both leave
  // consult_process_env_ false, so a hermetic view stays hermetic on a
  // miss instead of silently reading the developer's real environment.
  if (!consult_process_env_) {
    return std::nullopt;
  }
  const char* raw = std::getenv(std::string(name).c_str()); // NOLINT(concurrency-mt-unsafe) — single-threaded CLI startup path.
  if (raw == nullptr || raw[0] == '\0') {
    return std::nullopt;
  }
  return std::string(raw);
}

auto resolve(std::optional<std::string_view> file_content, const env_view& env, std::optional<std::string_view> assoc_slug)
    -> std::expected<resolved, effective_error> {
  auto def_map = parse_toml(k_defaults_toml);
  if (!def_map) {
    return std::unexpected(effective_error::parse_failed);
  }

  toml_map file_map;
  if (file_content.has_value()) {
    auto parsed = parse_toml(*file_content);
    if (!parsed) {
      return std::unexpected(effective_error::parse_failed);
    }
    file_map = std::move(*parsed);
  }

  effective_map eff;

  const auto defaults_vendor = pick_str("defaults.vendor", "PLANAR_VENDOR", env, std::nullopt, file_map, *def_map, eff);
  const auto defaults_scope  = pick_str("defaults.scope", "PLANAR_SCOPE", env, std::nullopt, file_map, *def_map, eff);
  const auto workbench_root  = pick_str("workbench.root", "PLANAR_WORKBENCH_ROOT", env, std::nullopt, file_map, *def_map, eff);
  const auto templates_dir   = pick_str("templates.dir", "PLANAR_TEMPLATES_DIR", env, std::nullopt, file_map, *def_map, eff);
  const auto templates_set =
      pick_str("templates.default_set", "PLANAR_TEMPLATES_DEFAULT_SET", env, std::nullopt, file_map, *def_map, eff);

  // Models / routing / roles (task 6080) — same position in the sequence
  // the oracle walks them (effective.zig:515-608), between the templates
  // keys and `external.jira.base_url`.
  resolve_models_routing_roles(env, file_map, *def_map, eff);

  const auto jira_base_url  = pick_str("external.jira.base_url", "JIRA_BASE_URL", env, std::nullopt, file_map, *def_map, eff);
  const auto jira_user_env  = pick_str("external.jira.user_env", "", env, std::nullopt, file_map, *def_map, eff);
  const auto jira_token_env = pick_str("external.jira.token_env", "", env, std::nullopt, file_map, *def_map, eff);

  const auto assoc_jira_todo    = assoc_str(file_map, assoc_slug, "external.jira.status.todo");
  const auto assoc_jira_doing   = assoc_str(file_map, assoc_slug, "external.jira.status.doing");
  const auto assoc_jira_blocked = assoc_str(file_map, assoc_slug, "external.jira.status.blocked");
  const auto assoc_jira_done    = assoc_str(file_map, assoc_slug, "external.jira.status.done");

  const auto jira_status_todo    = pick_str("external.jira.status.todo", "", env, assoc_jira_todo, file_map, *def_map, eff);
  const auto jira_status_doing   = pick_str("external.jira.status.doing", "", env, assoc_jira_doing, file_map, *def_map, eff);
  const auto jira_status_blocked = pick_str("external.jira.status.blocked", "", env, assoc_jira_blocked, file_map, *def_map, eff);
  const auto jira_status_done    = pick_str("external.jira.status.done", "", env, assoc_jira_done, file_map, *def_map, eff);

  const auto gh_issues_auth =
      pick_str("external.github-issues.auth", "PLANAR_GITHUB_AUTH", env, std::nullopt, file_map, *def_map, eff);
  const auto gh_issues_token_env = pick_str("external.github-issues.token_env", "", env, std::nullopt, file_map, *def_map, eff);
  const auto gh_issues_status_todo =
      pick_str("external.github-issues.status.todo", "", env, std::nullopt, file_map, *def_map, eff);
  const auto gh_issues_status_doing =
      pick_str("external.github-issues.status.doing", "", env, std::nullopt, file_map, *def_map, eff);
  const auto gh_issues_status_done =
      pick_str("external.github-issues.status.done", "", env, std::nullopt, file_map, *def_map, eff);

  const auto gh_projects_parent_field_names = resolve_parent_field_names(env, file_map, *def_map, eff);

  const bool         introspection_cli_log        = pick_bool("introspection.cli_log", file_map, *def_map, eff, false);
  const std::int64_t introspection_retention_days = pick_int("introspection.retention_days", file_map, *def_map, eff, 90);
  const bool         claude_enabled  = pick_bool("introspection.transcripts.claude_enabled", file_map, *def_map, eff, true);
  const bool         codex_enabled   = pick_bool("introspection.transcripts.codex_enabled", file_map, *def_map, eff, true);
  const bool         copilot_enabled = pick_bool("introspection.transcripts.copilot_enabled", file_map, *def_map, eff, true);
  const auto claude_path  = pick_str("introspection.transcripts.claude_path", "", env, std::nullopt, file_map, *def_map, eff);
  const auto codex_path   = pick_str("introspection.transcripts.codex_path", "", env, std::nullopt, file_map, *def_map, eff);
  const auto copilot_path = pick_str("introspection.transcripts.copilot_path", "", env, std::nullopt, file_map, *def_map, eff);

  return resolved{
      .cfg =
          config{
              .defaults  = defaults_config{.vendor = defaults_vendor, .scope = defaults_scope},
              .workbench = workbench_config{.root = workbench_root},
              .templates = templates_config{.dir = templates_dir, .default_set = templates_set},
              .external =
                  external_config{
                      .jira =
                          external_jira{
                              .base_url  = jira_base_url,
                              .user_env  = jira_user_env,
                              .token_env = jira_token_env,
                              .status =
                                  jira_status{
                                      .todo    = jira_status_todo,
                                      .doing   = jira_status_doing,
                                      .blocked = jira_status_blocked,
                                      .done    = jira_status_done,
                                  },
                          },
                      .github_issues =
                          external_github_issues{
                              .auth      = gh_issues_auth,
                              .token_env = gh_issues_token_env,
                              .status =
                                  github_issues_status{
                                      .todo  = gh_issues_status_todo,
                                      .doing = gh_issues_status_doing,
                                      .done  = gh_issues_status_done,
                                  },
                          },
                      .github_projects = external_github_projects{.parent_field_names = gh_projects_parent_field_names},
                  },
              .introspection =
                  introspection_config{
                      .cli_log        = introspection_cli_log,
                      .retention_days = introspection_retention_days,
                      .transcripts =
                          transcripts_config{
                              .claude_enabled  = claude_enabled,
                              .claude_path     = claude_path,
                              .codex_enabled   = codex_enabled,
                              .codex_path      = codex_path,
                              .copilot_enabled = copilot_enabled,
                              .copilot_path    = copilot_path,
                          },
                  },
          },
      .effective = std::move(eff),
  };
}

auto sensitive_name(std::string_view name) -> bool {
  std::string lower(name);
  std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  constexpr std::array<std::string_view, 3> exact    = {"token", "password", "secret"};
  constexpr std::array<std::string_view, 4> suffixes = {"_token", "_password", "_secret", "_key"};

  if (std::ranges::any_of(exact, [&](auto e) { return lower == e; })) {
    return true;
  }
  return std::ranges::any_of(suffixes, [&](auto suf) { return lower.ends_with(suf); });
}

auto sorted_keys(const effective_map& eff) -> std::vector<std::string> {
  std::vector<std::string> keys;
  keys.reserve(eff.size());
  for (const auto& [k, v] : eff) {
    keys.push_back(k);
  }
  return keys; // std::map already iterates in ascending key order.
}

} // namespace planar::engine::config
