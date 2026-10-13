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
import planar.engine.config.policy;
import planar.engine.config.toml;

using planar::engine::config::effective_error;
using planar::engine::config::env_view;
using planar::engine::config::parse_toml;
using planar::engine::config::provenance;
using planar::engine::config::resolve;
using planar::engine::config::sensitive_name;
using planar::engine::config::sorted_keys;
using planar::engine::config::tiers;
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

TEST_CASE("resolve: no execute.* key is recorded, from defaults, file or env", "[effective][execute]") {
  // The `[execute]` table and its engine selector were removed (plan 1142,
  // task 7459). Nothing in the resolved view may carry an `execute.` key,
  // even when the file and the old environment variable both name one.
  constexpr std::string_view file = "[execute]\nengine = \"centurion\"\n\n"
                                    "[execute.profiles.w]\nstate_dir = \"~/s\"\n";
  auto const                 env  = make_env({{"PLANAR_EXECUTE_ENGINE", "embedded"}});
  for (auto const& res : {resolve(std::nullopt, env_view::empty(), std::nullopt), resolve(file, env, std::nullopt)}) {
    REQUIRE(res.has_value());
    CHECK(std::ranges::none_of(res->effective, [](auto const& kv) { return kv.first.starts_with("execute"); }));
  }
}

TEST_CASE(
    "introspection_cli_log: unknown without a path, off for a missing file, the file's value otherwise, unknown when unparseable",
    "[effective]") {
  using planar::engine::config::introspection_cli_log;
  auto dir = std::filesystem::temp_directory_path() /
             std::format("planar_cli_log_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(dir);
  auto write = [&](std::string_view name, std::string_view body) {
    auto path = dir / name;
    std::ofstream(path) << body;
    return path;
  };
  CHECK_FALSE(introspection_cli_log(std::nullopt).has_value());
  CHECK(introspection_cli_log(dir / "absent.toml") == std::optional<bool>{false});
  CHECK(introspection_cli_log(write("on.toml", "[introspection]\ncli_log = true\n")) == std::optional<bool>{true});
  CHECK(introspection_cli_log(write("off.toml", "[introspection]\ncli_log = false\n")) == std::optional<bool>{false});
  CHECK_FALSE(introspection_cli_log(write("bad.toml", "[introspection\ncli_log = ")).has_value());
  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Policy (plan 1142, task mo-policy-config)
// ---------------------------------------------------------------------------

namespace {

using planar::engine::config::effective_policy;
using planar::engine::config::policy_canonical_json;
using planar::engine::config::policy_digest;
using planar::engine::config::policy_keys;
using planar::engine::config::policy_source_kind;
using planar::engine::config::policy_values;
using planar::engine::config::validate_policy;

auto parse_ok(std::string_view text) -> planar::engine::config::toml_map {
  auto parsed = parse_toml(text);
  REQUIRE(parsed.has_value());
  return *parsed;
}

auto policy_keys_named(const std::vector<planar::engine::config::policy_finding>& findings) -> std::vector<std::string> {
  std::vector<std::string> keys;
  for (const auto& finding : findings) {
    keys.push_back(finding.key);
  }
  return keys;
}

} // namespace

TEST_CASE("policy: no file means every key is its embedded default", "[effective][policy]") {
  auto policy = effective_policy({}, std::nullopt);
  REQUIRE(policy.has_value());
  CHECK(policy->values == policy_values{});
  CHECK_FALSE(policy->values.review_required);
  CHECK_FALSE(policy->values.task_gate.has_value());
  CHECK(policy->values.confirm_before.empty());
  for (const auto key : policy_keys) {
    INFO(key);
    REQUIRE(policy->sources.contains(std::string(key)));
    CHECK(policy->sources.at(std::string(key)).kind == policy_source_kind::embedded_default);
  }
}

TEST_CASE("policy: the struct defaults equal the [policy] table in defaults.toml", "[effective][policy]") {
  const auto from_defaults = parse_ok(planar::engine::config::defaults_toml());
  auto       policy        = effective_policy(from_defaults, std::nullopt);
  REQUIRE(policy.has_value());
  CHECK(policy->values == policy_values{});
  // Every key but the unset task_gate is spelled out in the shipped defaults.
  for (const auto key : policy_keys) {
    INFO(key);
    if (key == "task_gate") {
      CHECK_FALSE(from_defaults.contains("policy.task_gate"));
    } else {
      CHECK(from_defaults.contains(std::format("policy.{}", key)));
    }
  }
}

TEST_CASE("policy: the top-level table supplies values and names its section", "[effective][policy]") {
  const auto file   = parse_ok(R"([policy]
review_required = true
task_gate = ["make", "test"]
confirm_before = ["task done", "plan closeout"]
use_queue = true
)");
  auto       policy = effective_policy(file, std::nullopt);
  REQUIRE(policy.has_value());
  CHECK(policy->values.review_required);
  REQUIRE(policy->values.task_gate.has_value());
  CHECK(*policy->values.task_gate == std::vector<std::string>{"make", "test"});
  CHECK(policy->values.confirm_before == std::vector<std::string>{"task done", "plan closeout"});
  CHECK(policy->values.use_queue);
  CHECK(policy->sources.at("review_required").kind == policy_source_kind::top_level);
  CHECK(policy->sources.at("review_required").section == "policy");
  CHECK(policy->sources.at("use_queue").kind == policy_source_kind::top_level);
  CHECK(policy->sources.at("require_scenario_coverage").kind == policy_source_kind::embedded_default);
}

TEST_CASE("policy: association beats top level beats default, key by key", "[effective][policy]") {
  const auto file = parse_ok(R"([policy]
review_required = true
use_queue = true
[associations."org:acme".policy]
review_required = false
require_model = true
)");
  auto       acme = effective_policy(file, "org:acme");
  REQUIRE(acme.has_value());
  CHECK_FALSE(acme->values.review_required);
  CHECK(acme->sources.at("review_required").kind == policy_source_kind::association);
  CHECK(acme->sources.at("review_required").section == "associations.\"org:acme\".policy");
  CHECK(acme->values.require_model);
  CHECK(acme->values.use_queue);
  CHECK(acme->sources.at("use_queue").kind == policy_source_kind::top_level);
  CHECK(acme->sources.at("require_scenario_coverage").kind == policy_source_kind::embedded_default);

  // Another association, and no association, see only the top level.
  for (const std::optional<std::string_view> scope :
       {std::optional<std::string_view>{"org:other"}, std::optional<std::string_view>{}}) {
    auto other = effective_policy(file, scope);
    REQUIRE(other.has_value());
    CHECK(other->values.review_required);
    CHECK_FALSE(other->values.require_model);
    CHECK(other->sources.at("review_required").kind == policy_source_kind::top_level);
  }
}

TEST_CASE("policy: wrong types are refused naming the key and the expected type", "[effective][policy]") {
  const auto file     = parse_ok(R"([policy]
review_required = "yes"
task_gate = "make test"
use_queue = 1
confirm_before = ["Task Done"]
[associations."org:acme".policy]
require_model = ["x"]
)");
  const auto findings = validate_policy(file);
  CHECK(policy_keys_named(findings) == std::vector<std::string>{"associations.org:acme.policy.require_model",
                                                                "policy.confirm_before", "policy.review_required",
                                                                "policy.task_gate", "policy.use_queue"});
  for (const auto& finding : findings) {
    INFO(finding.key << ": " << finding.message);
    CHECK(finding.message.starts_with("must be "));
  }
  auto policy = effective_policy(file, "org:acme");
  REQUIRE_FALSE(policy.has_value());
  CHECK(policy.error().size() == 5);
}

TEST_CASE("policy: an empty task_gate argv and an empty argv element are refused", "[effective][policy]") {
  CHECK(validate_policy(parse_ok("[policy]\ntask_gate = []\n")).size() == 1);
  CHECK(validate_policy(parse_ok("[policy]\ntask_gate = [\"make\", \"\"]\n")).size() == 1);
  CHECK(validate_policy(parse_ok("[policy]\nuse_queue = true\ntask_gate = [\"make\"]\n")).empty());
}

TEST_CASE("policy: workflow-shaped and unknown keys are refused by name", "[effective][policy]") {
  const auto file     = parse_ok(R"([policy]
after_review = "dispatch"
then = "merge"
on_failure = "retry"
retry = 3
steps = ["a"]
review_required = true
[associations."org:acme".policy]
bogus = true
)");
  const auto findings = validate_policy(file);
  CHECK(policy_keys_named(findings) == std::vector<std::string>{"associations.org:acme.policy.bogus", "policy.after_review",
                                                                "policy.on_failure", "policy.retry", "policy.steps",
                                                                "policy.then"});
  for (const auto& finding : findings) {
    INFO(finding.key);
    CHECK(finding.message.contains("unknown policy key"));
    CHECK(finding.message.contains("facts Planar can check at one verb"));
  }
  CHECK_FALSE(effective_policy(file, std::nullopt).has_value());
}

TEST_CASE("policy: a nested table under policy is named, and policy itself must be a table", "[effective][policy]") {
  CHECK(policy_keys_named(validate_policy(parse_ok("[policy.retry]\ncount = 2\n"))) ==
        std::vector<std::string>{"policy.retry.count"});
  CHECK(policy_keys_named(validate_policy(parse_ok("policy = true\n"))) == std::vector<std::string>{"policy"});
}

TEST_CASE("policy: a task_gate needs use_queue = true in the same effective scope", "[effective][policy][gate]") {
  SECTION("a top-level gate without use_queue is refused, naming the key, the scope and the fix") {
    auto const file     = parse_ok("[policy]\ntask_gate = [\"make\", \"test\"]\n");
    auto const findings = validate_policy(file);
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].key == "policy.task_gate");
    CHECK(
        findings[0].message ==
        "gate evidence is a queued run, so use_queue = true is required in the same scope (add use_queue = true under [policy])");
    CHECK_FALSE(effective_policy(file, std::nullopt).has_value());
    CHECK_FALSE(effective_policy(file, "acme").has_value());
  }
  SECTION("use_queue = false beside the gate is refused; true is accepted") {
    CHECK(validate_policy(parse_ok("[policy]\nuse_queue = false\ntask_gate = [\"make\"]\n")).size() == 1);
    CHECK(validate_policy(parse_ok("[policy]\nuse_queue = true\ntask_gate = [\"make\"]\n")).empty());
  }
  SECTION("an association that inherits the gate and turns use_queue off is refused by name") {
    auto const file     = parse_ok(R"([policy]
use_queue = true
task_gate = ["make", "test"]
[associations."acme".policy]
use_queue = false
)");
    auto const findings = validate_policy(file);
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].key == "associations.acme.policy.task_gate");
    CHECK(findings[0].message.contains("under [associations.\"acme\".policy]"));
    CHECK_FALSE(effective_policy(file, "acme").has_value());
    CHECK_FALSE(effective_policy(file, std::nullopt).has_value());
  }
  SECTION("an association with its own gate inherits use_queue from the top level") {
    auto const file = parse_ok(R"([policy]
use_queue = true
[associations."acme".policy]
task_gate = ["make", "test"]
)");
    CHECK(validate_policy(file).empty());
    auto const acme = effective_policy(file, "acme");
    REQUIRE(acme.has_value());
    CHECK(acme->values.use_queue);
    CHECK(acme->values.task_gate.has_value());
  }
  SECTION("an association with its own gate and no use_queue anywhere is refused") {
    auto const findings = validate_policy(parse_ok("[associations.\"acme\".policy]\ntask_gate = [\"make\"]\n"));
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].key == "associations.acme.policy.task_gate");
  }
  SECTION("an association that sets use_queue = true beside the top-level gate is accepted for itself") {
    auto const file = parse_ok(R"([policy]
use_queue = true
task_gate = ["make"]
[associations."acme".policy]
use_queue = true
[associations."beta".policy]
review_required = true
)");
    CHECK(validate_policy(file).empty());
  }
  SECTION("a mistyped use_queue counts as unset: the type finding and the gate finding are both reported") {
    auto const findings = validate_policy(parse_ok("[policy]\nuse_queue = \"yes\"\ntask_gate = [\"make\"]\n"));
    CHECK(policy_keys_named(findings) == std::vector<std::string>{"policy.task_gate", "policy.use_queue"});
  }
  SECTION("resolve_policy_lenient still resolves a gate without use_queue, for display") {
    auto const policy = resolve_policy_lenient(parse_ok("[policy]\ntask_gate = [\"make\"]\n"), std::nullopt);
    CHECK(policy.values.task_gate.has_value());
    CHECK_FALSE(policy.values.use_queue);
  }
}

TEST_CASE("policy: no environment variable changes a value or its source", "[effective][policy][env]") {
  const std::string_view content = "[policy]\nreview_required = true\n";
  const auto             hostile = make_env({{"PLANAR_REVIEW_REQUIRED", "false"},
                                             {"PLANAR_POLICY_REVIEW_REQUIRED", "false"},
                                             {"PLANAR_POLICY_USE_QUEUE", "true"},
                                             {"PLANAR_USE_QUEUE", "true"},
                                             {"PLANAR_TASK_GATE", "true"},
                                             {"REVIEW_REQUIRED", "false"},
                                             {"policy.review_required", "false"}});
  auto                   quiet   = resolve(content, env_view::empty(), std::nullopt);
  auto                   noisy   = resolve(content, hostile, std::nullopt);
  REQUIRE(quiet.has_value());
  REQUIRE(noisy.has_value());
  for (const auto key : policy_keys) {
    const auto name = std::format("policy.{}", key);
    INFO(name);
    REQUIRE(noisy->effective.contains(name));
    CHECK(noisy->effective.at(name).value == quiet->effective.at(name).value);
    CHECK(noisy->effective.at(name).source_ == quiet->effective.at(name).source_);
    CHECK(noisy->effective.at(name).env_var_name.empty());
  }
  CHECK(noisy->effective.at("policy.review_required").value == "true");
  CHECK(noisy->effective.at("policy.review_required").source_ == provenance::config_file);
  CHECK(noisy->effective.at("policy.use_queue").value == "false");
}

TEST_CASE("resolve: records a policy row per key with its provenance and section", "[effective][policy]") {
  const std::string_view content = R"([policy]
review_required = true
task_gate = ["make", "test"]
[associations."org:acme".policy]
review_required = false
)";
  auto                   top     = resolve(content, env_view::empty(), std::nullopt);
  REQUIRE(top.has_value());
  CHECK(top->effective.at("policy.review_required").value == "true");
  CHECK(top->effective.at("policy.review_required").source_ == provenance::config_file);
  CHECK(top->effective.at("policy.review_required").section == "policy");
  CHECK(top->effective.at("policy.task_gate").value == "[\"make\",\"test\"]");
  CHECK(top->effective.at("policy.confirm_before").value == "[]");
  CHECK(top->effective.at("policy.confirm_before").source_ == provenance::embedded_default);
  CHECK(top->effective.at("policy.confirm_before").section.empty());

  auto acme = resolve(content, env_view::empty(), "org:acme");
  REQUIRE(acme.has_value());
  CHECK(acme->effective.at("policy.review_required").value == "false");
  CHECK(acme->effective.at("policy.review_required").source_ == provenance::assoc_override);
  CHECK(acme->effective.at("policy.review_required").section == "associations.\"org:acme\".policy");

  auto none = resolve(std::nullopt, env_view::empty(), std::nullopt);
  REQUIRE(none.has_value());
  CHECK(none->effective.at("policy.task_gate").value == "(unset)");
  CHECK(none->effective.at("policy.task_gate").source_ == provenance::embedded_default);
}

TEST_CASE("resolve: a mistyped policy value falls through to the default instead of failing", "[effective][policy]") {
  auto res = resolve("[policy]\nreview_required = \"yes\"\n", env_view::empty(), std::nullopt);
  REQUIRE(res.has_value());
  CHECK(res->effective.at("policy.review_required").value == "false");
  CHECK(res->effective.at("policy.review_required").source_ == provenance::embedded_default);
}

TEST_CASE("policy_digest: stable, lowercase hex, and sensitive to every key and to argv order", "[effective][policy][digest]") {
  const policy_values base{};
  const auto          digest = policy_digest(base);
  CHECK(digest.size() == 64);
  CHECK(std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
  CHECK(policy_digest(policy_values{}) == digest);
  // Pinned: the canonical form is part of the contract a claim records.
  CHECK(
      policy_canonical_json(base) ==
      R"({"confirm_before":[],"require_model":false,"require_scenario_coverage":false,"review_required":false,"task_gate":null,"use_queue":false})");

  std::vector<policy_values> variants(8);
  variants[0].review_required           = true;
  variants[1].task_gate                 = std::vector<std::string>{"make", "test"};
  variants[2].require_model             = true;
  variants[3].confirm_before            = {"task done"};
  variants[4].require_scenario_coverage = true;
  variants[5].use_queue                 = true;
  variants[6].task_gate                 = std::vector<std::string>{"test", "make"};
  variants[7].task_gate                 = std::vector<std::string>{"make test"};
  std::set<std::string> seen{digest};
  for (const auto& variant : variants) {
    CHECK(seen.insert(policy_digest(variant)).second);
  }
  // Two different argv splits of one command line differ: the array is preserved.
  CHECK(policy_digest(variants[1]) != policy_digest(variants[7]));
  // An unset gate differs from any gate.
  CHECK(policy_digest(base) != policy_digest(variants[1]));
}

TEST_CASE("policy_digest: the same policy resolved from two files digests equal", "[effective][policy][digest]") {
  auto a = effective_policy(parse_ok("[policy]\nreview_required = true\n"), std::nullopt);
  auto b = effective_policy(parse_ok("# comment\n[policy]\nuse_queue = false\nreview_required = true\n"), std::nullopt);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(policy_digest(a->values) == policy_digest(b->values));
}
