/// @file profile.cpp
/// @brief Implementation of `planar.cmd.planar_execute.profile`.

module;

#include <glaze/glaze.hpp>

module planar.cmd.planar_execute.profile;

import std;
import planar.engine_execute;

namespace planar::cmd::execute {

// Named, not anonymous: Glaze's reflection needs the types to have linkage.
namespace wire {

/// @brief `profile_json`'s document. Member order is key order.
struct profile_document {
  std::string                                               name;                   ///< Profile name.
  bool                                                      configured = false;     ///< Set in the config file.
  std::string                                               state_dir;              ///< Canonical state dir.
  std::string                                               planar_db;              ///< Canonical database.
  std::vector<std::string>                                  allowed_roots;          ///< Canonical roots.
  std::int64_t                                              idle_grace_seconds = 0; ///< Idle grace.
  std::optional<std::string>                                command_policy;         ///< Policy path or null.
  std::optional<std::string>                                bundle;                 ///< Bundle path or null.
  std::map<std::string, std::map<std::string, std::string>> providers;              ///< Provider config.
};

} // namespace wire

namespace {

constexpr std::string_view k_prefix = "execute.profiles.";

/// @brief The scalar keys a profile may set (providers are checked apart).
constexpr std::array<std::string_view, 6> k_keys{
    "state_dir", "planar_db", "allowed_roots", "idle_grace_seconds", "command_policy", "bundle",
};

/// @brief `~` / `~/…` expanded, made absolute, and canonicalised as far as
/// the path exists.
auto canonical_path(std::string_view raw, const env_lookup& env, std::string_view what)
    -> std::expected<std::string, std::string> {
  std::filesystem::path path{std::string{raw}};
  if (raw == "~" || raw.starts_with("~/")) {
    auto const home = env("HOME");
    if (!home.has_value()) {
      return std::unexpected{std::format("{} '{}' needs $HOME to expand '~', and HOME is unset", what, raw)};
    }
    path = std::filesystem::path{*home} / std::string{raw.substr(raw == "~" ? 1 : 2)};
  }
  std::error_code ec;
  auto            absolute = std::filesystem::absolute(path, ec);
  if (ec) {
    return std::unexpected{std::format("{} '{}' cannot be made absolute", what, raw)};
  }
  auto canonical = std::filesystem::weakly_canonical(absolute, ec);
  if (ec) {
    return std::unexpected{std::format("{} '{}' cannot be canonicalised", what, raw)};
  }
  auto text = canonical.string();
  // `weakly_canonical` keeps a trailing separator from `~/`; identity must not.
  while (text.size() > 1 && text.ends_with('/')) {
    text.pop_back();
  }
  return text;
}

/// @brief A profile name is one TOML key segment of `[A-Za-z0-9_-]`, so the
/// dotted keys `planar config show` flattens it into split unambiguously.
auto valid_name(std::string_view name) -> bool {
  return !name.empty() && std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

} // namespace

auto resolve_profile(std::span<const engine::execute::config_entry> entries, std::string_view name, const env_lookup& env,
                     const std::function<std::string()>& config_path) -> std::expected<profile, std::string> {
  if (!valid_name(name)) {
    return std::unexpected{std::format("profile name '{}' must be letters, digits, '_' or '-'", name)};
  }

  // Group every configured key by profile, validating as we go.
  std::map<std::string, std::map<std::string, std::string>, std::less<>> by_profile;
  for (auto const& entry : entries) {
    if (!entry.key.starts_with(k_prefix)) {
      continue;
    }
    auto const rest = std::string_view{entry.key}.substr(k_prefix.size());
    auto const dot  = rest.find('.');
    auto const who  = rest.substr(0, dot);
    auto const key  = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
    bool const known =
        std::ranges::contains(k_keys, key) ||
        (key.starts_with("providers.") && key.find('.', 10) != std::string_view::npos && !key.ends_with('.') && key[10] != '.');
    if (!valid_name(who) || !known) {
      return std::unexpected{std::format("unknown key '{}' in {}", entry.key, config_path())};
    }
    by_profile[std::string{who}][std::string{key}] = entry.value;
  }

  auto const found = by_profile.find(name);
  if (found == by_profile.end() && name != "default") {
    return std::unexpected{std::format("profile '{}' is not configured in {}", name, config_path())};
  }

  // Every configured profile is resolved, so a malformed value anywhere in
  // the file is reported now; the requested one is returned.
  auto const             home = [&]() -> std::optional<std::string> { return env("HOME"); };
  std::optional<profile> wanted;
  auto                   names = std::views::keys(by_profile) | std::ranges::to<std::vector<std::string>>();
  if (!by_profile.contains(name)) {
    names.emplace_back(name); // `default`, unconfigured
  }
  for (auto const& each : names) {
    std::map<std::string, std::string> const empty;
    auto const                               it = by_profile.find(each);
    auto const&                              kv = it == by_profile.end() ? empty : it->second;
    auto const where = [&](std::string_view key) { return std::format("execute.profiles.{}.{}", each, key); };

    profile out{.name = each, .configured = it != by_profile.end()};

    std::string state_raw;
    if (auto v = kv.find("state_dir"); v != kv.end()) {
      state_raw = v->second;
    } else if (auto ph = env("PLANAR_HOME"); ph.has_value()) {
      state_raw = std::format("{}/execute/{}", *ph, each);
    } else if (home().has_value()) {
      state_raw = std::format("{}/.planar/execute/{}", *home(), each);
    } else {
      return std::unexpected{std::format("{}: no state_dir set and neither PLANAR_HOME nor HOME is set", where("state_dir"))};
    }
    auto state = canonical_path(state_raw, env, where("state_dir"));
    if (!state) {
      return std::unexpected{state.error()};
    }
    out.state_dir = std::move(*state);

    std::string db_raw;
    if (auto v = kv.find("planar_db"); v != kv.end()) {
      db_raw = v->second;
    } else if (auto pdb = env("PLANAR_DB"); pdb.has_value()) {
      db_raw = *pdb;
    } else if (home().has_value()) {
      db_raw = std::format("{}/.planar/planar.db", *home());
    } else {
      return std::unexpected{std::format("{}: no planar_db set and neither PLANAR_DB nor HOME is set", where("planar_db"))};
    }
    auto db = canonical_path(db_raw, env, where("planar_db"));
    if (!db) {
      return std::unexpected{db.error()};
    }
    out.planar_db = std::move(*db);

    if (auto v = kv.find("allowed_roots"); v != kv.end()) {
      std::vector<std::string> raw_roots;
      if (!v->second.starts_with('[') || glz::read_json(raw_roots, v->second)) {
        return std::unexpected{std::format("{} must be an array of paths in {}", where("allowed_roots"), config_path())};
      }
      for (auto const& raw : raw_roots) {
        auto root = canonical_path(raw, env, where("allowed_roots"));
        if (!root) {
          return std::unexpected{root.error()};
        }
        out.allowed_roots.push_back(std::move(*root));
      }
    }

    if (auto v = kv.find("idle_grace_seconds"); v != kv.end()) {
      std::int64_t parsed  = 0;
      auto const [ptr, ec] = std::from_chars(v->second.data(), v->second.data() + v->second.size(), parsed);
      if (ec != std::errc{} || ptr != v->second.data() + v->second.size() || parsed <= 0) {
        return std::unexpected{
            std::format("{} must be a positive integer in {}, got: {}", where("idle_grace_seconds"), config_path(), v->second)};
      }
      out.idle_grace_seconds = parsed;
    }

    for (auto const key : {std::string_view{"command_policy"}, std::string_view{"bundle"}}) {
      if (auto v = kv.find(std::string{key}); v != kv.end()) {
        auto path = canonical_path(v->second, env, where(key));
        if (!path) {
          return std::unexpected{path.error()};
        }
        (key == "bundle" ? out.bundle : out.command_policy) = std::move(*path);
      }
    }

    for (auto const& [key, value] : kv) {
      if (!key.starts_with("providers.")) {
        continue;
      }
      auto const tail                                          = std::string_view{key}.substr(10);
      auto const dot                                           = tail.find('.');
      auto const vendor                                        = std::string{tail.substr(0, dot)};
      out.providers[vendor][std::string{tail.substr(dot + 1)}] = value;
    }

    if (each == name) {
      wanted = std::move(out);
    }
  }
  return std::move(*wanted);
}

auto profile_json(const profile& value) -> std::string {
  wire::profile_document const doc{
      .name               = value.name,
      .configured         = value.configured,
      .state_dir          = value.state_dir,
      .planar_db          = value.planar_db,
      .allowed_roots      = value.allowed_roots,
      .idle_grace_seconds = value.idle_grace_seconds,
      .command_policy     = value.command_policy,
      .bundle             = value.bundle,
      .providers          = value.providers,
  };
  std::string out;
  // `skip_null_members = false`: an unset path is `null`, never an absent key.
  if (glz::write<glz::opts{.skip_null_members = false}>(doc, out)) {
    return "{}";
  }
  return out;
}

auto profile_text(const profile& value) -> std::string {
  std::string out = std::format("profile: {}\nconfigured: {}\nstate_dir: {}\nplanar_db: {}\n", value.name,
                                value.configured ? "true" : "false", value.state_dir, value.planar_db);
  if (value.allowed_roots.empty()) {
    out += "allowed_roots: -\n";
  }
  for (auto const& root : value.allowed_roots) {
    out += std::format("allowed_root: {}\n", root);
  }
  out += std::format("idle_grace_seconds: {}\ncommand_policy: {}\nbundle: {}\n", value.idle_grace_seconds,
                     value.command_policy.value_or("-"), value.bundle.value_or("-"));
  if (value.providers.empty()) {
    out += "providers: -\n";
  }
  for (auto const& [vendor, keys] : value.providers) {
    for (auto const& [key, setting] : keys) {
      out += std::format("provider.{}.{}: {}\n", vendor, key, setting);
    }
  }
  return out;
}

} // namespace planar::cmd::execute
