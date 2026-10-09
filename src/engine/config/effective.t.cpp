// @file effective.t.cpp
// @brief Unit tests for `planar.engine.config.effective` (plan 996, task
// 6032). Exercises the four-layer precedence (env > per-association
// override > config file > embedded default) against the same shapes
// zig/src/engine/config/effective.zig's own test suite pins: defaults with
// no config file present, a config-file override, env-beats-file, the
// jira-status-only per-association override, and the introspection
// bool/int fields. The models/routing/roles surface (task 6080) has its own
// section at the bottom of this file.
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

import std;
import planar.engine.config.effective;
import planar.engine.config.toml;

using planar::engine::config::effective_error;
using planar::engine::config::env_view;
using planar::engine::config::parse_toml;
using planar::engine::config::provenance;
using planar::engine::config::resolve;
using planar::engine::config::sensitive_name;
using planar::engine::config::sorted_keys;
using planar::engine::config::tiers;
using planar::engine::config::validate_introspection;
using planar::engine::config::vendors;
using planar::engine::config::work_types;

namespace {

auto make_env(std::map<std::string, std::string, std::less<>> vars) -> env_view {
  return env_view{std::move(vars)};
}

} // namespace

TEST_CASE("resolve: defaults-only (no file, no env) — every key comes from the embedded default", "[effective]") {
  auto res = resolve(std::nullopt, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());

  CHECK(res->cfg.defaults.vendor == "claude");
  CHECK(res->cfg.defaults.scope == "global");
  CHECK(res->cfg.workbench.root == "~/.planar/workbench");
  CHECK(res->cfg.templates.dir == "~/.planar/templates");
  CHECK(res->cfg.templates.default_set == "default");
  CHECK(res->cfg.external.jira.user_env == "JIRA_USER");
  CHECK(res->cfg.external.jira.token_env == "JIRA_TOKEN");
  CHECK(res->cfg.external.jira.status.todo == "To Do");
  CHECK(res->cfg.external.jira.status.doing == "In Progress");
  CHECK(res->cfg.external.jira.status.blocked == "Blocked");
  CHECK(res->cfg.external.jira.status.done == "Done");
  CHECK(res->cfg.external.github_issues.auth == "gh-cli");
  CHECK(res->cfg.external.github_issues.token_env == "GITHUB_TOKEN");
  CHECK(res->cfg.external.github_issues.status.todo == "open");
  CHECK(res->cfg.external.github_issues.status.doing == "open");
  CHECK(res->cfg.external.github_issues.status.done == "closed");
  REQUIRE(res->cfg.external.github_projects.parent_field_names.size() == 3);
  CHECK(res->cfg.external.github_projects.parent_field_names[0] == "Parent");
  CHECK(res->cfg.introspection.cli_log == false);
  CHECK(res->cfg.introspection.retention_days == 90);
  CHECK(res->cfg.introspection.transcripts.claude_enabled == true);
  CHECK(res->cfg.introspection.transcripts.claude_path.empty());

  auto it = res->effective.find("defaults.vendor");
  REQUIRE(it != res->effective.end());
  CHECK(it->second.source_ == provenance::embedded_default);
  CHECK(it->second.env_var_name.empty());
}

TEST_CASE("resolve: config file overrides the default vendor", "[effective]") {
  constexpr std::string_view file = "[defaults]\nvendor = \"codex\"\n";
  auto                       res  = resolve(file, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());

  CHECK(res->cfg.defaults.vendor == "codex");
  auto it = res->effective.find("defaults.vendor");
  REQUIRE(it != res->effective.end());
  CHECK(it->second.source_ == provenance::config_file);
}

TEST_CASE("resolve: env override beats a file override (full precedence chain)", "[effective]") {
  constexpr std::string_view file = "[defaults]\nvendor = \"codex\"\n";
  auto                       env  = make_env({{"PLANAR_VENDOR", "claude"}});
  auto                       res  = resolve(file, env, std::nullopt);
  REQUIRE(res.has_value());

  // env (claude) beats file (codex) beats default (claude, coincidentally
  // the same text as env here — the provenance assertion is what actually
  // proves which layer won).
  CHECK(res->cfg.defaults.vendor == "claude");
  auto it = res->effective.find("defaults.vendor");
  REQUIRE(it != res->effective.end());
  CHECK(it->second.source_ == provenance::env);
  CHECK(it->second.env_var_name == "PLANAR_VENDOR");
}

TEST_CASE("resolve: per-association override wins over file and default for jira status", "[effective]") {
  constexpr std::string_view file = "[external.jira.status]\n"
                                    "done = \"Shipped\"\n"
                                    "[associations.\"org:acme\"]\n"
                                    "[associations.\"org:acme\".external.jira.status]\n"
                                    "done = \"Closed\"\n";

  auto res = resolve(file, env_view::empty(), std::string_view{"org:acme"});
  REQUIRE(res.has_value());

  CHECK(res->cfg.external.jira.status.done == "Closed");
  auto it = res->effective.find("external.jira.status.done");
  REQUIRE(it != res->effective.end());
  CHECK(it->second.source_ == provenance::assoc_override);

  // A different (non-matching) association slug does NOT pick up the
  // override — falls through to the file value.
  auto other = resolve(file, env_view::empty(), std::string_view{"org:other"});
  REQUIRE(other.has_value());
  CHECK(other->cfg.external.jira.status.done == "Shipped");
}

TEST_CASE("resolve: parent_field_names — file array overrides the embedded default array", "[effective]") {
  constexpr std::string_view file = "[external.github-projects]\nparent_field_names = [\"Epic\"]\n";
  auto                       res  = resolve(file, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  REQUIRE(res->cfg.external.github_projects.parent_field_names.size() == 1);
  CHECK(res->cfg.external.github_projects.parent_field_names[0] == "Epic");
}

TEST_CASE("resolve: parent_field_names env override is comma-separated and trimmed", "[effective]") {
  auto env = make_env({{"PLANAR_GITHUB_PROJECTS_PARENT_FIELDS", "Epic,  Feature "}});
  auto res = resolve(std::nullopt, env, std::nullopt);
  REQUIRE(res.has_value());
  REQUIRE(res->cfg.external.github_projects.parent_field_names.size() == 2);
  CHECK(res->cfg.external.github_projects.parent_field_names[0] == "Epic");
  CHECK(res->cfg.external.github_projects.parent_field_names[1] == "Feature");

  auto it = res->effective.find("external.github-projects.parent_field_names");
  REQUIRE(it != res->effective.end());
  CHECK(it->second.source_ == provenance::env);
}

TEST_CASE("resolve: introspection — defaults are cli_log=false, retention_days=90", "[effective]") {
  auto res = resolve(std::nullopt, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  CHECK(res->cfg.introspection.cli_log == false);
  CHECK(res->cfg.introspection.retention_days == 90);
  CHECK(res->effective.at("introspection.cli_log").source_ == provenance::embedded_default);
}

TEST_CASE("resolve: introspection — config file overrides cli_log and retention_days", "[effective]") {
  constexpr std::string_view file = "[introspection]\ncli_log = true\nretention_days = 30\n";
  auto                       res  = resolve(file, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  CHECK(res->cfg.introspection.cli_log == true);
  CHECK(res->cfg.introspection.retention_days == 30);
  CHECK(res->effective.at("introspection.cli_log").source_ == provenance::config_file);
  CHECK(res->effective.at("introspection.cli_log").value == "true");
}

TEST_CASE("resolve: introspection — transcript_scan_bytes defaults to 64 MiB and a file value overrides it", "[effective]") {
  auto const unset = resolve(std::nullopt, env_view{}, std::nullopt);
  REQUIRE(unset.has_value());
  CHECK(unset->cfg.introspection.transcript_scan_bytes == 67'108'864);
  CHECK(unset->effective.at("introspection.transcript_scan_bytes").value == "67108864");
  CHECK(unset->effective.at("introspection.transcript_scan_bytes").source_ == provenance::embedded_default);

  constexpr std::string_view file = "[introspection]\ntranscript_scan_bytes = 1048576\n";
  auto const                 set  = resolve(file, env_view{}, std::nullopt);
  REQUIRE(set.has_value());
  CHECK(set->cfg.introspection.transcript_scan_bytes == 1'048'576);
  CHECK(set->effective.at("introspection.transcript_scan_bytes").source_ == provenance::config_file);
}

TEST_CASE("validate_introspection: transcript_scan_bytes must be an integer above 0", "[effective]") {
  auto const findings = [](std::string_view doc) {
    auto const parsed = parse_toml(doc);
    REQUIRE(parsed.has_value());
    return validate_introspection(*parsed);
  };
  CHECK(findings("").empty());
  CHECK(findings("[introspection]\ncli_log = true\n").empty());
  CHECK(findings("[introspection]\ntranscript_scan_bytes = 1\n").empty());
  CHECK(findings("[introspection]\ntranscript_scan_bytes = 67108864\n").empty());
  for (std::string_view const value : {"0", "-1", "\"64MiB\"", "true"}) {
    INFO(value);
    auto const refused = findings(std::format("[introspection]\ntranscript_scan_bytes = {}\n", value));
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].key == "introspection.transcript_scan_bytes");
    CHECK_FALSE(refused[0].message.empty());
  }
}

TEST_CASE("resolve: introspection transcripts — independent per-adapter enable/path overrides", "[effective]") {
  constexpr std::string_view file = "[introspection.transcripts]\n"
                                    "claude_enabled = false\n"
                                    "codex_path = \"/safe/codex\"\n";
  auto                       res  = resolve(file, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  CHECK(res->cfg.introspection.transcripts.claude_enabled == false);
  CHECK(res->cfg.introspection.transcripts.codex_enabled == true);
  CHECK(res->cfg.introspection.transcripts.codex_path == "/safe/codex");
}

TEST_CASE("resolve: an empty config-file document falls through entirely to defaults", "[effective]") {
  auto res = resolve(std::string_view{""}, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  CHECK(res->cfg.defaults.vendor == "claude");
  CHECK(res->effective.at("defaults.vendor").source_ == provenance::embedded_default);
}

TEST_CASE("resolve: a malformed config file surfaces effective_error::parse_failed", "[effective]") {
  auto res = resolve(std::string_view{"vendor = \"unclosed\n"}, env_view::empty(), std::nullopt);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == effective_error::parse_failed);
}

TEST_CASE("env_view::empty() ignores the real process environment (F1 hermeticity regression)", "[effective]") {
  // Export a var that resolve() would otherwise honor (PLANAR_VENDOR
  // feeds defaults.vendor via pick_str's env_name), then prove
  // env_view::empty() — and therefore resolve() called with it — never
  // observes it. Before the F1 fix, env_view::get() fell through to
  // std::getenv on every vars_ miss, so this poisoned var would win and
  // the CHECK below would fail.
  REQUIRE(::setenv("PLANAR_VENDOR", "poisoned-by-test", /*overwrite=*/1) == 0);
  struct restore_env {
    ~restore_env() {
      ::unsetenv("PLANAR_VENDOR");
    }
  } cleanup;

  CHECK(env_view::empty().get("PLANAR_VENDOR") == std::nullopt);

  auto res = resolve(std::nullopt, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  CHECK(res->cfg.defaults.vendor == "claude"); // embedded default, NOT "poisoned-by-test"
  CHECK(res->effective.at("defaults.vendor").source_ == provenance::embedded_default);
}

TEST_CASE("env_view — explicit map constructor also ignores the real process environment", "[effective]") {
  // The map-backed constructor (make_env / the explicit env_view{map}
  // ctor) must stay hermetic too, independent of empty(): a map miss must
  // return nullopt, not fall through to std::getenv.
  REQUIRE(::setenv("PLANAR_SCOPE", "poisoned-by-test", /*overwrite=*/1) == 0);
  struct restore_env {
    ~restore_env() {
      ::unsetenv("PLANAR_SCOPE");
    }
  } cleanup;

  auto env = make_env({{"PLANAR_VENDOR", "codex"}}); // PLANAR_SCOPE deliberately absent from the map
  CHECK(env.get("PLANAR_SCOPE") == std::nullopt);
  CHECK(env.get("PLANAR_VENDOR") == "codex");
}

TEST_CASE("sensitive_name: exact and suffix matches, case-insensitive", "[effective]") {
  CHECK(sensitive_name("token") == true);
  CHECK(sensitive_name("TOKEN") == true);
  CHECK(sensitive_name("api_token") == true);
  CHECK(sensitive_name("jira_password") == true);
  CHECK(sensitive_name("token_env") == false); // "token_env" is neither an exact match nor a listed suffix.
  CHECK(sensitive_name("vendor") == false);
}

TEST_CASE("sorted_keys: ascending lexicographic order", "[effective]") {
  auto res = resolve(std::nullopt, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  auto keys = sorted_keys(res->effective);
  REQUIRE(keys.size() > 1);
  CHECK(std::ranges::is_sorted(keys));
}

// ---------------------------------------------------------------------------
// Models / routing / roles (task 6080). Every expectation below was captured
// from `zig/zig-out/bin/planar config show --effective` in a pinned scratch
// arena, not derived by reading effective.zig. `defaults.toml` ships NO
// embedded default for any of these keys (plan 950 removed the model
// catalog), so the whole surface is operator-supplied — which is exactly why
// dropping the walk was not the no-op it looked like.
// ---------------------------------------------------------------------------

TEST_CASE("resolve: no models/routing/roles key is recorded without a config file", "[effective][models]") {
  auto res = resolve(std::nullopt, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  for (const auto& key : sorted_keys(res->effective)) {
    CHECK_FALSE(key.starts_with("models."));
    CHECK_FALSE(key.starts_with("routing."));
    CHECK_FALSE(key.starts_with("roles."));
    CHECK_FALSE(key.starts_with("role_vendors."));
  }
}

TEST_CASE("resolve: a scalar model tier resolves to a one-element candidate list", "[effective][models]") {
  auto res = resolve(std::string_view{"[models.claude]\nmedium = \"claude-sonnet-5\"\n"}, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  const auto& entry = res->effective.at("models.claude.medium");
  CHECK(entry.value == "claude-sonnet-5");
  CHECK(entry.source_ == provenance::config_file);
  REQUIRE(entry.candidates.size() == 1);
  CHECK(entry.candidates[0] == entry.value); // The candidates[0] == value invariant.
}

TEST_CASE("resolve: an array model tier resolves to the ordered candidate list", "[effective][models]") {
  auto res = resolve(std::string_view{"[models.codex]\nlarge = [\"gpt-5.5\", \"gpt-5.3-codex-spark\"]\n"}, env_view::empty(),
                     std::nullopt);
  REQUIRE(res.has_value());
  const auto& entry = res->effective.at("models.codex.large");
  REQUIRE(entry.candidates.size() == 2);
  CHECK(entry.candidates[0] == "gpt-5.5");
  CHECK(entry.candidates[1] == "gpt-5.3-codex-spark");
  CHECK(entry.value == entry.candidates[0]); // list[0] is the tier default (D5).
}

TEST_CASE("resolve: an empty model tier array records nothing", "[effective][models]") {
  auto res = resolve(std::string_view{"[models.codex]\nlarge = []\n"}, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  CHECK_FALSE(res->effective.contains("models.codex.large"));
}

TEST_CASE("resolve: routing keys resolve per work type and absent ones record nothing", "[effective][models]") {
  auto res = resolve(std::string_view{"[routing.claude.medium]\nengine = \"claude-haiku-4-5\"\ncli = \"claude-sonnet-5\"\n"},
                     env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  CHECK(res->effective.at("routing.claude.medium.engine").value == "claude-haiku-4-5");
  CHECK(res->effective.at("routing.claude.medium.engine").source_ == provenance::config_file);
  CHECK(res->effective.at("routing.claude.medium.cli").value == "claude-sonnet-5");
  // A work type present in neither the file nor the defaults is a miss, not
  // an empty entry — callers read a miss as "fall back to the tier default".
  CHECK_FALSE(res->effective.contains("routing.claude.medium.schema"));
  CHECK_FALSE(res->effective.contains("routing.gemini.small.feature"));
}

TEST_CASE("resolve: every canonical vendor/tier/work-type combination is walked", "[effective][models]") {
  // Build a config that sets one routing key for every (vendor, tier,
  // work_type) triple; all of them must come back. This is the check that
  // would have caught the dropped walk.
  std::string file;
  for (const auto vendor : vendors) {
    for (const auto tier : tiers) {
      file += std::format("[routing.{}.{}]\n", vendor, tier);
      for (const auto work_type : work_types) {
        file += std::format("{} = \"m-{}-{}-{}\"\n", work_type, vendor, tier, work_type);
      }
    }
  }
  auto res = resolve(std::string_view{file}, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  for (const auto vendor : vendors) {
    for (const auto tier : tiers) {
      for (const auto work_type : work_types) {
        const auto key = std::format("routing.{}.{}.{}", vendor, tier, work_type);
        REQUIRE(res->effective.contains(key));
        CHECK(res->effective.at(key).value == std::format("m-{}-{}-{}", vendor, tier, work_type));
      }
    }
  }
}

TEST_CASE("resolve: built-in roles and role_vendors resolve from the config file", "[effective][models]") {
  auto res = resolve(std::string_view{"[roles]\ncoder = \"medium\"\nreviewer = \"large\"\n"
                                      "[role_vendors]\ncoder = \"codex\"\n"},
                     env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  CHECK(res->effective.at("roles.coder").value == "medium");
  CHECK(res->effective.at("roles.reviewer").value == "large");
  CHECK(res->effective.at("role_vendors.coder").value == "codex");
  // Built-ins the file does not mention have no embedded default either, so
  // they are absent rather than empty.
  CHECK_FALSE(res->effective.contains("roles.test-coder"));
  CHECK_FALSE(res->effective.contains("role_vendors.reviewer"));
}

TEST_CASE("resolve: a custom (non-built-in) role is picked up too", "[effective][models]") {
  // plan 586 task 3937: any roles.<name> / role_vendors.<name> in the file
  // that is not one of the four built-ins still lands in the effective map.
  auto res = resolve(std::string_view{"[roles]\nmy-custom-role = \"large\"\n"
                                      "[role_vendors]\nmy-custom-role = \"claude\"\n"},
                     env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  CHECK(res->effective.at("roles.my-custom-role").value == "large");
  CHECK(res->effective.at("role_vendors.my-custom-role").value == "claude");
}

TEST_CASE("resolve: models/routing/roles take no env override", "[effective][models]") {
  // The oracle passes env_name = null for every key in this block; an env
  // var of the "same shape" must not leak in.
  const env_view env{{{"PLANAR_VENDOR", "codex"}, {"MODELS_CLAUDE_MEDIUM", "nope"}, {"ROLES_CODER", "nope"}}};
  auto res = resolve(std::string_view{"[models.claude]\nmedium = \"claude-sonnet-5\"\n[roles]\ncoder = \"medium\"\n"}, env,
                     std::nullopt);
  REQUIRE(res.has_value());
  CHECK(res->effective.at("models.claude.medium").value == "claude-sonnet-5");
  CHECK(res->effective.at("models.claude.medium").source_ == provenance::config_file);
  CHECK(res->effective.at("roles.coder").value == "medium");
  CHECK(res->effective.at("roles.coder").source_ == provenance::config_file);
}

TEST_CASE("resolve: candidates is empty for every non-model-tier key", "[effective][models]") {
  auto res = resolve(std::string_view{"[models.claude]\nmedium = [\"a\", \"b\"]\n[routing.claude.medium]\nengine = \"a\"\n"},
                     env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  for (const auto& key : sorted_keys(res->effective)) {
    if (key.starts_with("models.")) {
      continue;
    }
    INFO("key = " << key);
    CHECK(res->effective.at(key).candidates.empty());
  }
}

TEST_CASE("resolve: execute.engine defaults to embedded, and file then env override it", "[effective][6485]") {
  // `planar-execute` reads this key out of `planar config show --json` (plan
  // 1033 task 6485); the provenance is what its `profile show` reports.
  auto const def = resolve(std::nullopt, env_view::empty(), std::nullopt);
  REQUIRE(def.has_value());
  auto it = def->effective.find("execute.engine");
  REQUIRE(it != def->effective.end());
  CHECK(it->second.value == "embedded");
  CHECK(it->second.source_ == provenance::embedded_default);

  constexpr std::string_view file      = "[execute]\nengine = \"centurion\"\n";
  auto const                 from_file = resolve(file, env_view::empty(), std::nullopt);
  REQUIRE(from_file.has_value());
  it = from_file->effective.find("execute.engine");
  REQUIRE(it != from_file->effective.end());
  CHECK(it->second.value == "centurion");
  CHECK(it->second.source_ == provenance::config_file);

  auto const from_env = resolve(file, make_env({{"PLANAR_EXECUTE_ENGINE", "embedded"}}), std::nullopt);
  REQUIRE(from_env.has_value());
  it = from_env->effective.find("execute.engine");
  REQUIRE(it != from_env->effective.end());
  CHECK(it->second.value == "embedded");
  CHECK(it->second.source_ == provenance::env);
  CHECK(it->second.env_var_name == "PLANAR_EXECUTE_ENGINE");
}

TEST_CASE("resolve: execute.profiles.* keys from the file are surfaced verbatim, arrays as JSON", "[effective][6494]") {
  // `planar-execute` reads its execution profiles out of this view (plan
  // 1033 task 6494). An array is JSON text, not the `, `-joined form, so a
  // path holding a comma survives; an unknown key is surfaced for the
  // consumer to refuse, not dropped here.
  constexpr std::string_view file = "[execute.profiles.w]\n"
                                    "state_dir = \"~/s\"\n"
                                    "allowed_roots = [\"~/a\", \"/b,c\"]\n"
                                    "idle_grace_seconds = 120\n"
                                    "typo = true\n"
                                    "[execute.profiles.w.providers.claude]\n"
                                    "command = \"claude\"\n";
  auto const                 res  = resolve(file, env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  auto const value = [&](std::string_view key) {
    auto const it = res->effective.find(key);
    REQUIRE(it != res->effective.end());
    CHECK(it->second.source_ == provenance::config_file);
    return it->second.value;
  };
  CHECK(value("execute.profiles.w.state_dir") == "~/s");
  CHECK(value("execute.profiles.w.allowed_roots") == R"(["~/a","/b,c"])");
  CHECK(value("execute.profiles.w.idle_grace_seconds") == "120");
  CHECK(value("execute.profiles.w.typo") == "true");
  CHECK(value("execute.profiles.w.providers.claude.command") == "claude");
  // No file, no profile keys: the defaults are planar-execute's, not planar's.
  auto const bare = resolve(std::nullopt, env_view::empty(), std::nullopt);
  REQUIRE(bare.has_value());
  CHECK(std::ranges::none_of(bare->effective, [](auto const& kv) { return kv.first.starts_with("execute.profiles."); }));
}
