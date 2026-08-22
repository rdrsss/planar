// @file effective.t.cpp
// @brief Unit tests for `planar.engine.config.effective` (plan 996, task
// 6032). Exercises the four-layer precedence (env > per-association
// override > config file > embedded default) against the same shapes
// zig/src/engine/config/effective.zig's own test suite pins: defaults with
// no config file present, a config-file override, env-beats-file, the
// jira-status-only per-association override, and the introspection
// bool/int fields. See effective.cppm's header comment for the deliberate
// models/routing/roles scope cut (no embedded default ships for those
// keys any more, and they belong to a separate engine/ops task).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.config.effective;

using planar::engine::config::effective_error;
using planar::engine::config::env_view;
using planar::engine::config::provenance;
using planar::engine::config::resolve;
using planar::engine::config::sensitive_name;
using planar::engine::config::sorted_keys;

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
