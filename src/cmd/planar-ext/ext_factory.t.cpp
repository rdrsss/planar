// @file ext_factory.t.cpp
// @brief The adapter FACTORY behind `planar ext test`, tested directly
// (plan 996, task 6258).
//
// ## Why this is its own translation unit
//
// The same reason `init.t.cpp` is, and its header states the rule: this file
// imports `planar.cmd.planar_ext.handlers.ext_adapter_factory`, which opens
// `namespace planar::cmd::ext::handlers`, while the dispatch helper in
// `ext_leaves.t.cpp` calls the FUNCTION `planar::cmd::ext::handlers(*tree)`. With
// both in scope the qualified name is ambiguous and the TU does not compile.
// The split is not cosmetic: it is the one arrangement in which the
// end-to-end leaf cases and these direct ones can both exist.
//
// ## WHY THE FACTORY IS TESTED DIRECTLY AND NOT ONLY THROUGH THE VERB
//
// Two of its six refusals are not reachable through any CLI path.
// `ext register jira` REQUIRES `--auth-env` and `ext register github`
// selects `gh-cli` when it is omitted, so no register verb can write an
// `oauth-stored` row, and neither writes a `gitlab-issues` or `linear` one
// — though the `external_systems` CHECK constraint permits all three. Those
// arms are reachable only on a row written some other way, and the oracle
// serves them, so they are pinned here against rows built in memory.
//
// The `gh-cli` arms are here for a different reason: driving them through
// the verb would make the result depend on whether a `gh` binary happens to
// be installed on the machine running ctest. Injecting a `token_command`
// makes all four outcomes deterministic.
//
// ## THE CENTRAL CASE: THE TWO CREDENTIAL SOURCES DISAGREE ABOUT EMPTY
//
// `token-env` with the variable set to `""` SUCCEEDS — exit 0, adapter
// wired, bearer token empty. `gh-cli` with empty output REFUSES. The two are
// asserted together in one case on purpose. Read apart, each looks like a
// defect and the obvious fix (reject empty everywhere, or accept it
// everywhere) is a behaviour change this port is not entitled to make.
//
// ## THERE IS NO PRECEDENCE TO TEST, AND THAT IS ITSELF THE FINDING
//
// Task 6258 predicted "env var vs config file vs stored row, which wins".
// None of that exists: `auth_method` is a DISCRIMINANT and exactly one
// source is consulted per row. There is no config-file credential source and
// no stored-token column in `external_systems` at all. The case named for
// this asserts the NEGATIVE — a `gh-cli` row ignores a set environment
// variable, and a `token-env` row never shells `gh` — because a port that
// grew a fallback chain would pass every other case in this file.
//
// Everything here was captured from `zig/zig-out/bin/planar` across a
// 22-case differential covering present / absent / empty for every
// credential source, both render modes, both unsupported kinds, a NULL
// `base_url`, and the ordering case below. It matched byte for byte on
// stdout, stderr AND exit code.
//
// ## A RECORDED BREAK-PROBE SURVIVOR, AND WHY IT IS NOT PAPERED OVER
//
// Mutating `spawn_capture`'s `if (rc != 0)` early return to `if (false)`
// SURVIVES the case below, and the reason is worth knowing before someone
// "fixes" the test:
//
//   posix_spawnp on this platform reports a missing program through its
//   RETURN VALUE (measured: rc=2/ENOENT, and `pid` is left 0) rather than
//   by forking a child that fails exec. With the early return removed,
//   execution falls through to `waitpid(0, ...)`, which returns -1/ECHILD
//   when the process has no children — and THAT path also yields
//   `spawned == false`. The mutant is coincidentally equivalent on the one
//   observable this function exposes.
//
// So the early return has no output-visible contract of its own here, and
// no assertion over `spawn_capture`'s public result can kill that mutant.
// It is kept regardless, because it is not decorative: `waitpid(0, ...)`
// waits on ANY child in the caller's process group, so in a process that
// does have other children — this very test binary, once it spawns the
// shims above — the fallthrough would reap an unrelated child and return
// its exit status as `gh`'s. That is a real defect the mutant introduces
// and this test cannot see.
//
// Recorded rather than concealed: writing an assertion that appears to
// cover the line (a timing check, or one that reaches into internals)
// would be the false-positive test this milestone keeps catching.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.cmd.planar_ext.handlers.ext_adapter_factory;
import planar.engine.external;

namespace {

using planar::cmd::ext::handlers::adapter_kind;
using planar::cmd::ext::handlers::build_adapter;
using planar::cmd::ext::handlers::default_transport_factory;
using planar::cmd::ext::handlers::factory_deps;
using planar::cmd::ext::handlers::factory_error;
using planar::cmd::ext::handlers::spawn_capture;
using planar::cmd::ext::handlers::token_command_result;

namespace system_ns = planar::engine::external::system;

/// @brief A row built in memory, including the shapes no register verb can
/// write.
/// @param kind The system kind.
/// @param auth How credentials are obtained.
/// @param auth_ref The env var name, or `default`.
/// @return The row.
auto row(system_ns::system_kind kind, system_ns::auth_method auth, std::string auth_ref) -> system_ns::external_system {
  system_ns::external_system sys;
  sys.id       = 1;
  sys.kind     = kind;
  sys.slug     = "demo";
  sys.base_url = "https://example.invalid";
  sys.auth     = auth;
  sys.auth_ref = std::move(auth_ref);
  return sys;
}

/// @brief Deps whose environment is a fixed map and whose `gh` is canned.
/// @param vars The environment.
/// @param gh What the credential subprocess should report.
/// @return The dependency set.
auto deps_with(std::map<std::string, std::string, std::less<>> vars, token_command_result gh) -> factory_deps {
  return factory_deps{
      .env = [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        auto const found = vars.find(name);
        if (found == vars.end()) {
          return std::nullopt;
        }
        return found->second;
      },
      .gh   = [gh = std::move(gh)] { return gh; },
      .wire = default_transport_factory(),
  };
}

/// @brief Write an executable `/bin/sh` script and return its absolute path.
///
/// Absolute so `spawn_capture` reaches it with no PATH mutation — the test
/// process's own environment is never touched.
/// @param dir The directory to write into.
/// @param name The file name.
/// @param body The script body, after the shebang.
/// @return The absolute path.
auto write_shim(const std::filesystem::path& dir, std::string_view name, std::string_view body) -> std::string {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  auto const    path = dir / name;
  std::ofstream file(path);
  file << "#!/bin/sh\n" << body;
  file.close();
  std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
  return path.string();
}

/// @brief A unique scratch directory for shim scripts.
/// @param tag A discriminator.
/// @return The path.
auto shim_dir(std::string_view tag) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         std::format("planar_shim_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
}

} // namespace

TEST_CASE("an EMPTY token-env variable SUCCEEDS while empty gh-cli output REFUSES", "[cmd][ext][factory][empty]") {
  auto const env_row = row(system_ns::system_kind::jira, system_ns::auth_method::token_env, "DEMO_TOKEN");
  auto const empty_env =
      build_adapter(env_row, deps_with({{"DEMO_TOKEN", ""}}, {.spawned = true, .exit_code = 0, .output = "unused"}));
  REQUIRE(empty_env.has_value());
  CHECK((*empty_env)->kind() == adapter_kind::jira);

  auto const cli_row   = row(system_ns::system_kind::github_issues, system_ns::auth_method::gh_cli, "default");
  auto const empty_cli = build_adapter(cli_row, deps_with({}, {.spawned = true, .exit_code = 0, .output = ""}));
  REQUIRE_FALSE(empty_cli.has_value());
  CHECK(empty_cli.error() == factory_error::gh_cli_empty_token);
}

TEST_CASE("an ABSENT token-env variable is the only token-env refusal", "[cmd][ext][factory][auth]") {
  auto const sys     = row(system_ns::system_kind::jira, system_ns::auth_method::token_env, "DEMO_TOKEN");
  auto const missing = build_adapter(sys, deps_with({{"OTHER", "x"}}, {}));
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == factory_error::token_env_var_missing);

  auto const present = build_adapter(sys, deps_with({{"DEMO_TOKEN", "tok-abc"}}, {}));
  CHECK(present.has_value());
}

TEST_CASE("auth_method is a discriminant: neither source falls back to the other", "[cmd][ext][factory][precedence]") {
  // A gh-cli row IGNORES a perfectly good environment variable...
  auto const cli_row = row(system_ns::system_kind::github_issues, system_ns::auth_method::gh_cli, "DEMO_TOKEN");
  auto const ignored =
      build_adapter(cli_row, deps_with({{"DEMO_TOKEN", "from-env"}}, {.spawned = false, .exit_code = 0, .output = ""}));
  REQUIRE_FALSE(ignored.has_value());
  CHECK(ignored.error() == factory_error::gh_cli_not_found);

  // ...and a token-env row never consults `gh`, even when `gh` would answer.
  auto const env_row     = row(system_ns::system_kind::jira, system_ns::auth_method::token_env, "DEMO_TOKEN");
  auto const not_shelled = build_adapter(env_row, deps_with({}, {.spawned = true, .exit_code = 0, .output = "from-gh"}));
  REQUIRE_FALSE(not_shelled.has_value());
  CHECK(not_shelled.error() == factory_error::token_env_var_missing);
}

TEST_CASE("the gh-cli arm separates not-found, non-zero and empty", "[cmd][ext][factory][ghcli]") {
  auto const sys = row(system_ns::system_kind::github_issues, system_ns::auth_method::gh_cli, "default");

  auto const not_found = build_adapter(sys, deps_with({}, {.spawned = false, .exit_code = 0, .output = ""}));
  REQUIRE_FALSE(not_found.has_value());
  CHECK(not_found.error() == factory_error::gh_cli_not_found);

  // Exit 127 with a REAL spawn is `failed`, not `not_found`. This is the
  // case a popen-based runner gets wrong: it cannot tell 127-because-missing
  // from 127-from-the-program.
  auto const failed = build_adapter(sys, deps_with({}, {.spawned = true, .exit_code = 127, .output = ""}));
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error() == factory_error::gh_cli_failed);

  auto const whitespace = build_adapter(sys, deps_with({}, {.spawned = true, .exit_code = 0, .output = " \t\r\n"}));
  REQUIRE_FALSE(whitespace.has_value());
  CHECK(whitespace.error() == factory_error::gh_cli_empty_token);

  auto const padded = build_adapter(sys, deps_with({}, {.spawned = true, .exit_code = 0, .output = "  tok  \n"}));
  CHECK(padded.has_value());
}

TEST_CASE("oauth-stored and the two unsupported kinds each refuse", "[cmd][ext][factory][unsupported]") {
  auto const oauth = row(system_ns::system_kind::jira, system_ns::auth_method::oauth_stored, "default");
  auto const built = build_adapter(oauth, deps_with({{"default", "x"}}, {}));
  REQUIRE_FALSE(built.has_value());
  CHECK(built.error() == factory_error::unsupported_auth_method);

  for (auto const kind : {system_ns::system_kind::gitlab_issues, system_ns::system_kind::linear}) {
    auto const sys = row(kind, system_ns::auth_method::token_env, "DEMO_TOKEN");
    auto const out = build_adapter(sys, deps_with({{"DEMO_TOKEN", "tok"}}, {}));
    INFO("kind: " << system_ns::system_kind_to_text(kind));
    REQUIRE_FALSE(out.has_value());
    CHECK(out.error() == factory_error::unsupported_system_kind);
  }
}

TEST_CASE("the credential resolves BEFORE the kind is checked", "[cmd][ext][factory][ordering]") {
  // A row that fails both ways. The missing variable wins — oracle-captured,
  // and the reverse order is equally plausible from the signatures alone.
  auto const sys = row(system_ns::system_kind::linear, system_ns::auth_method::token_env, "NEVER_SET");
  auto const out = build_adapter(sys, deps_with({}, {}));
  REQUIRE_FALSE(out.has_value());
  CHECK(out.error() == factory_error::token_env_var_missing);
}

TEST_CASE("a github row builds a github adapter, not a jira one", "[cmd][ext][factory][kind]") {
  // The kind switch is the half of the factory that is invisible in every
  // refusal case above: all six of those return before it runs.
  auto const sys = row(system_ns::system_kind::github_issues, system_ns::auth_method::token_env, "GH_TOKEN");
  auto const out = build_adapter(sys, deps_with({{"GH_TOKEN", "tok"}}, {}));
  REQUIRE(out.has_value());
  CHECK((*out)->kind() == adapter_kind::github);
}

TEST_CASE("a NULL base_url builds an adapter rather than refusing", "[cmd][ext][factory][nullurl]") {
  auto sys       = row(system_ns::system_kind::jira, system_ns::auth_method::token_env, "DEMO_TOKEN");
  sys.base_url   = std::nullopt;
  auto const out = build_adapter(sys, deps_with({{"DEMO_TOKEN", "tok"}}, {}));
  CHECK(out.has_value());
}

TEST_CASE("spawn_capture runs a REAL subprocess and separates its three outcomes", "[cmd][ext][spawn]") {
  // The factory cases above inject a `token_command` and therefore bypass
  // every line of `spawn_capture`. This one does not: real fork, real exec,
  // real pipe, real wait — against shim scripts invoked by ABSOLUTE path, so
  // the test process's own PATH is never mutated.
  auto const dir = shim_dir("spawn");

  auto const ok  = write_shim(dir, "ok.sh", "echo gh-token-xyz\n");
  auto const ran = spawn_capture(ok, {});
  CHECK(ran.spawned);
  CHECK(ran.exit_code == 0);
  CHECK(ran.output == "gh-token-xyz\n");

  auto const bad    = write_shim(dir, "bad.sh", "echo 'not logged in' >&2\nexit 3\n");
  auto const failed = spawn_capture(bad, {});
  CHECK(failed.spawned);
  CHECK(failed.exit_code == 3);
  // The child's stderr went to /dev/null — not into the captured stdout, and
  // not to the test runner's terminal.
  CHECK(failed.output.empty());

  // THE DISTINCTION popen CANNOT MAKE, asserted as a PAIR: a program that
  // does not exist is `spawned == false`, while a program that exists and
  // exits 127 is `spawned == true, exit_code == 127`. Under `popen` both
  // arrive as exit 127 and the factory cannot tell `gh binary not on PATH`
  // from `run gh auth login`.
  auto const missing = spawn_capture((dir / "no-such-program").string(), {});
  CHECK_FALSE(missing.spawned);

  auto const shell_style = write_shim(dir, "e127.sh", "exit 127\n");
  auto const as_127      = spawn_capture(shell_style, {});
  CHECK(as_127.spawned);
  CHECK(as_127.exit_code == 127);

  // Arguments really are passed through, so the `auth token` the real runner
  // supplies is not silently dropped.
  auto const                                echo = write_shim(dir, "args.sh", "echo \"$1-$2\"\n");
  constexpr std::array<std::string_view, 2> k_args{"auth", "token"};
  auto const                                passed = spawn_capture(echo, k_args);
  CHECK(passed.output == "auth-token\n");

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}
