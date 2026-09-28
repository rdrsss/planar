// @file profile.t.cpp
// @brief Execution-profile resolution (plan 1033 M0, task 6494; tech-spec
// D10). Pure: config entries and the environment are injected; the only
// real filesystem use is the symlink-identity case.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine_execute;
import planar.cmd.planar_execute.profile;

namespace {

using planar::cmd::execute::resolve_profile;
using planar::engine::execute::config_entry;

/// @brief A config-file entry, as `planar config show --json` reports one.
auto entry(std::string key, std::string value) -> config_entry {
  return config_entry{.key = std::move(key), .value = std::move(value), .provenance = "config file"};
}

/// @brief An environment with only `vars` set.
auto env_of(std::map<std::string, std::string> vars) -> planar::cmd::execute::env_lookup {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    auto const it = vars.find(std::string{name});
    return it == vars.end() ? std::nullopt : std::optional{it->second};
  };
}

/// @brief A scratch directory, removed on scope exit.
struct scratch {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               std::format("planar_profile_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  scratch() {
    std::filesystem::create_directories(root / "real");
    std::filesystem::create_directories(root / "code");
    std::filesystem::create_directory_symlink(root / "real", root / "link");
  }
  scratch(scratch const&)                    = delete;
  auto operator=(scratch const&) -> scratch& = delete;
  ~scratch() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  /// @brief The canonical form of `root / rel`.
  [[nodiscard]] auto canon(std::string_view rel) const -> std::string {
    return std::filesystem::weakly_canonical(root / std::string{rel}).string();
  }
};

} // namespace

TEST_CASE("an unconfigured default profile is exactly the defaults", "[cmd][execute][profile][6494]") {
  scratch    s;
  int        path_reads = 0;
  auto const got        = resolve_profile({}, "default", env_of({{"HOME", s.root.string()}}), [&] {
    ++path_reads;
    return std::string{"/cfg"};
  });
  REQUIRE(got.has_value());
  CHECK(got->name == "default");
  CHECK_FALSE(got->configured);
  CHECK(got->state_dir == s.canon(".planar/execute/default"));
  CHECK(got->planar_db == s.canon(".planar/planar.db"));
  CHECK(got->allowed_roots.empty());
  CHECK(got->idle_grace_seconds == 300);
  CHECK_FALSE(got->command_policy.has_value());
  CHECK_FALSE(got->bundle.has_value());
  CHECK(got->providers.empty());
  // The config path is only fetched to name the file in a refusal.
  CHECK(path_reads == 0);

  // PLANAR_HOME and PLANAR_DB move the defaults, exactly as they move planar's.
  auto const moved = resolve_profile(
      {}, "default", env_of({{"HOME", "/nope"}, {"PLANAR_HOME", s.root.string()}, {"PLANAR_DB", (s.root / "x.db").string()}}),
      [] { return std::string{}; });
  REQUIRE(moved.has_value());
  CHECK(moved->state_dir == s.canon("execute/default"));
  CHECK(moved->planar_db == s.canon("x.db"));
}

TEST_CASE("a path and a symlink to it are the same profile identity; tilde roots are canonicalised",
          "[cmd][execute][profile][6494]") {
  scratch                         s;
  std::vector<config_entry> const entries{
      entry("execute.profiles.a.state_dir", (s.root / "real").string()),
      entry("execute.profiles.a.allowed_roots", R"(["~/code","~/code/../code/"])"),
      entry("execute.profiles.a.idle_grace_seconds", "120"),
      entry("execute.profiles.a.providers.claude.command", "claude"),
      entry("execute.profiles.a.bundle", "~/bundle.tar"),
      entry("execute.profiles.b.state_dir", "~/link"),
  };
  auto const env = env_of({{"HOME", s.root.string()}});
  auto const a   = resolve_profile(entries, "a", env, [] { return std::string{"/cfg"}; });
  auto const b   = resolve_profile(entries, "b", env, [] { return std::string{"/cfg"}; });
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a->configured);
  CHECK(a->state_dir == b->state_dir);
  CHECK(a->state_dir == s.canon("real"));
  CHECK(a->allowed_roots == std::vector<std::string>{s.canon("code"), s.canon("code")});
  CHECK(a->idle_grace_seconds == 120);
  CHECK(a->bundle == s.canon("bundle.tar"));
  CHECK(a->providers.at("claude").at("command") == "claude");
  CHECK(b->idle_grace_seconds == 300);
}

TEST_CASE("profile resolution refuses unknown keys, missing names and malformed values, naming the file",
          "[cmd][execute][profile][6494]") {
  auto const env  = env_of({{"HOME", "/h"}});
  auto const path = [] { return std::string{"/cfg/config.toml"}; };

  auto const unknown = resolve_profile(std::vector{entry("execute.profiles.x.state_dirr", "s")}, "default", env, path);
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error() == "unknown key 'execute.profiles.x.state_dirr' in /cfg/config.toml");

  auto const bare_provider = resolve_profile(std::vector{entry("execute.profiles.x.providers", "s")}, "x", env, path);
  REQUIRE_FALSE(bare_provider.has_value());
  CHECK(bare_provider.error() == "unknown key 'execute.profiles.x.providers' in /cfg/config.toml");

  auto const missing = resolve_profile(std::vector{entry("execute.profiles.x.state_dir", "/s")}, "y", env, path);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == "profile 'y' is not configured in /cfg/config.toml");

  auto const not_array = resolve_profile(std::vector{entry("execute.profiles.x.allowed_roots", "/a")}, "x", env, path);
  REQUIRE_FALSE(not_array.has_value());
  CHECK(not_array.error() == "execute.profiles.x.allowed_roots must be an array of paths in /cfg/config.toml");

  for (auto const bad : {"0", "-5", "12s", ""}) {
    INFO(bad);
    auto const grace = resolve_profile(std::vector{entry("execute.profiles.x.idle_grace_seconds", bad)}, "x", env, path);
    REQUIRE_FALSE(grace.has_value());
    CHECK(grace.error() ==
          std::format("execute.profiles.x.idle_grace_seconds must be a positive integer in /cfg/config.toml, got: {}", bad));
  }

  // A malformed OTHER profile is still reported when `default` is asked for.
  auto const elsewhere = resolve_profile(std::vector{entry("execute.profiles.z.idle_grace_seconds", "x")}, "default", env, path);
  REQUIRE_FALSE(elsewhere.has_value());

  auto const bad_name = resolve_profile({}, "a.b", env, path);
  REQUIRE_FALSE(bad_name.has_value());
  CHECK(bad_name.error() == "profile name 'a.b' must be letters, digits, '_' or '-'");
}
