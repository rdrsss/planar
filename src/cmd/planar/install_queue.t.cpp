// @file install_queue.t.cpp
// @brief The installer's queue-upgrade steps driven against the REAL built
// binaries (plan 1089, task qp-install-migrate; tech spec 656, "Install and
// upgrade"; test spec 658, the install scenarios that name the post-build
// ctest case).
//
// `scripts/install-lib/queue-retire.sh` holds the steps install.sh runs after
// it installs the binaries. `scripts/install-manifest-test.sh` drives the same
// functions with stub binaries, before anything is built, which pins the
// probe table's decisions. What a stub cannot show is that the decisions hold
// for what the shipped binaries actually print and do: that a truncated
// `planar.db` really probes `schema_version_behind`, that `planar init` run the
// way the step runs it really migrates it from inside a linked worktree, and
// that an ahead database really is left alone. ctest runs only after the
// build, so the binaries here are always current.
//
// The installer's prefix is a scratch directory under the arena, and the
// scratch `HOME` holds a canary `.planar/planar.db`. The seam is run through
// `run_pinned` with `PLANAR_DB` REMOVED from the environment, so the only way
// any binary reaches a database is the path the step itself passes; a step
// that forgot to pass it would reach the canary, and the canary is checked.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;

#include "parity_harness.hpp"
#include "planar-agent/queue_test_store.hpp"

namespace {

namespace parity = ::planar::cmd::parity;
namespace qfix   = ::planar::cmd::qfix;

using parity::capture;

/// @brief Path to the built `planar` binary.
/// @return The path.
auto planar_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Path to the built `planar-agent` binary.
/// @return The path.
auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_AGENT_CPP_BIN};
}

/// @brief The installer's sourceable queue-upgrade functions in this checkout.
/// @return The path.
auto seam_lib() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_TARGET_SOURCE_ROOT} / "scripts" / "install-lib" / "queue-retire.sh";
}

/// @brief A scratch install: a prefix holding the built binaries and a
/// config with the `cli_log` hook pinned off, and a scratch `HOME` holding a
/// canary `.planar/planar.db`.
struct install_fixture {
  parity::arena         arena;  ///< Owns every path below.
  std::filesystem::path work;   ///< The arena's root.
  std::filesystem::path prefix; ///< The install prefix (`--prefix`).
  std::filesystem::path db;     ///< `prefix/planar.db`.
  std::filesystem::path config; ///< `prefix/config.toml`.
  std::filesystem::path canary; ///< `HOME/.planar/planar.db`, which nothing may touch.
};

/// @brief Fails the case, before any binary runs, when a fixture step failed.
/// @param what The step.
/// @param ok Whether it worked.
void must(std::string_view what, bool ok) {
  if (!ok) {
    throw std::runtime_error(std::format("install fixture: {} failed", what));
  }
}

/// @brief Builds the scratch install.
/// @param tag The arena tag.
/// @return The fixture.
auto make_install(std::string_view tag) -> install_fixture {
  install_fixture fx;
  fx.arena  = parity::make_arena(tag);
  fx.work   = fx.arena.cpp_root;
  fx.prefix = fx.work / "prefix";
  fx.db     = fx.prefix / "planar.db";
  fx.config = fx.prefix / "config.toml";
  fx.canary = fx.work / "fakehome" / ".planar" / "planar.db";

  std::error_code ec;
  std::filesystem::create_directories(fx.prefix / "bin", ec);
  must("create prefix/bin", !ec);
  std::filesystem::create_symlink(planar_bin(), fx.prefix / "bin" / "planar", ec);
  must("place planar", !ec);
  std::filesystem::create_symlink(agent_bin(), fx.prefix / "bin" / "planar-agent", ec);
  must("place planar-agent", !ec);
  {
    std::ofstream out(fx.config, std::ios::binary);
    out << "[introspection]\ncli_log = false\n";
    must("write config.toml", static_cast<bool>(out));
  }
  std::filesystem::create_directories(fx.canary.parent_path(), ec);
  must("create HOME/.planar", !ec);
  {
    std::ofstream out(fx.canary, std::ios::binary);
    out << "canary: not a database; the install must never open this\n";
    must("write the canary", static_cast<bool>(out));
  }
  return fx;
}

/// @brief A `planar.db` whose chain stops one migration short of head: a
/// truly truncated chain, so `init` then applies a real migration (test spec
/// 658, "Behind, for migration tests").
/// @param path The database file.
void truncated_store(const std::filesystem::path& path) {
  auto opened = planar::db::connection::open(path.string());
  must("open the truncated store", opened.has_value());
  auto const chain = planar::db::migrations();
  must("a chain longer than one migration", chain.size() > 1);
  auto applied = planar::db::apply_all(*opened, chain.first(chain.size() - 1));
  must("apply the chain minus its head", applied.has_value());
}

/// @brief Runs one function of the seam with the fixture's prefix.
///
/// `PLANAR_DB` is removed from the environment and `PLANAR_HOME` reaches the
/// seam as an unexported shell variable, the way `install.sh --prefix` sets
/// it, so the binaries see neither.
/// @param fx The fixture.
/// @param fn The function to call.
/// @param tag A capture discriminator.
/// @return What the call printed and its status.
auto run_seam(const install_fixture& fx, std::string_view fn, std::string_view tag) -> capture {
  auto env = parity::pinned_env(fx.work);
  for (auto& var : env) {
    if (var.name == "PLANAR_DB") {
      var.unset = true;
    }
    if (var.name == "PLANAR_HOME") {
      var.value = fx.prefix.string();
    }
  }
  std::string const script = R"(set -eEuo pipefail; p="$PLANAR_HOME"; unset PLANAR_HOME; PLANAR_HOME="$p"; source "$1"; "$2")";
  std::vector<std::string> const args{"-c", script, "bash", seam_lib().string(), std::string{fn}};
  return parity::run_pinned("bash", args, fx.work, tag, env);
}

/// @brief Every path under `root`, relative to it, sorted, leaving out the
/// SQLite sidecars of `planar.db` (a connection may leave them in WAL mode;
/// they are the database's own files, not new ones).
/// @param root The directory.
/// @return The listing, one path per line.
auto listing(const std::filesystem::path& root) -> std::string {
  std::vector<std::string> paths;
  std::error_code          ec;
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    auto const rel = it->path().lexically_relative(root).string();
    if (rel == "planar.db-wal" || rel == "planar.db-shm") {
      continue;
    }
    paths.push_back(rel);
  }
  std::ranges::sort(paths);
  std::string out;
  for (auto const& p : paths) {
    out += p + "\n";
  }
  return out;
}

/// @brief The schema version and every table's row count, as one string, so
/// "nothing changed" is one comparison.
/// @param path The database file.
/// @return `versions|table=count,...`.
auto db_state(const std::filesystem::path& path) -> std::string {
  auto opened = planar::db::connection::open_read_only(path.string());
  must("open the store read-only", opened.has_value());
  auto tables = opened->prepare("select name from sqlite_master where type = 'table' order by name");
  must("list the tables", tables.has_value());
  std::vector<std::string> names;
  for (;;) {
    auto stepped = tables->step();
    must("step the table list", stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    names.push_back(tables->column_text(0));
  }
  std::string out = qfix::applied_versions(path) + "|";
  for (auto const& name : names) {
    auto count = opened->prepare(std::format("select count(*) from \"{}\"", name));
    must("count a table", count.has_value());
    auto stepped = count->step();
    must("step a count", stepped.has_value() && *stepped == planar::db::step_result::row);
    out += std::format("{}={},", name, count->column_int64(0));
  }
  return out;
}

/// @brief Turns the arena's `proj` directory, the cwd every pinned run starts
/// in, into a linked git worktree of a scratch repository.
/// @param work The arena root.
void make_proj_a_linked_worktree(const std::filesystem::path& work) {
  auto const line =
      std::format("cd {} && env -i PATH=\"$PATH\" HOME={} GIT_CONFIG_NOSYSTEM=1 sh -c 'git init -q repo && git -C repo -c "
                  "user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false commit -q --allow-empty -m init && "
                  "git -C repo worktree add -q ../proj' >/dev/null 2>&1",
                  parity::shell_quote(work.string()), parity::shell_quote((work / "fakehome").string()));
  must("create the linked worktree", std::system(line.c_str()) == 0);
  must("proj is a linked worktree", std::filesystem::is_regular_file(work / "proj" / ".git"));
}

/// @brief The position of `needle` in `hay`, failing the case when absent.
/// @param hay The text.
/// @param needle What to find.
/// @return Its offset.
auto position_of(const std::string& hay, std::string_view needle) -> std::size_t {
  auto const at = hay.find(needle);
  INFO("looking for: " << needle << "\nin:\n" << hay);
  REQUIRE(at != std::string::npos);
  return at;
}

/// @brief The canary's bytes and modification time, for "untouched".
/// @param path The canary.
/// @return Both, as one comparable string.
auto canary_state(const std::filesystem::path& path) -> std::string {
  std::error_code ec;
  auto const      when = std::filesystem::last_write_time(path, ec);
  return std::format("{}|{}", qfix::file_bytes(path), when.time_since_epoch().count());
}

} // namespace

TEST_CASE("install_queue_probe_migrate", "[cmd][install][queue]") {
  SECTION("a behind prefix planar.db is migrated from a linked worktree, and a second run changes nothing") {
    auto fx = make_install("iq_behind");
    truncated_store(fx.db);
    make_proj_a_linked_worktree(fx.work);
    auto const head = qfix::head_version();
    REQUIRE_FALSE(qfix::applied_versions(fx.db).ends_with(std::to_string(head)));
    auto const config_before = qfix::file_bytes(fx.config);
    auto const canary_before = canary_state(fx.canary);
    auto const prefix_before = listing(fx.prefix);
    auto const home_before   = listing(fx.work / "fakehome" / ".planar");

    auto const first = run_seam(fx, "queue_probe_migrate", "first");
    INFO("stdout:\n" << first.out << "\nstderr:\n" << first.err);
    REQUIRE(first.code == 0);
    // The first probe answered behind, the second not_found, in that order.
    auto const behind = position_of(first.out, "queue store probe: behind (exit 125, tag schema_version_behind)");
    auto const usable = position_of(first.out, "queue store probe: usable (exit 1, tag not_found)");
    CHECK(behind < usable);
    CHECK(qfix::applied_versions(fx.db).ends_with(std::format(",{}", head)));
    // Nothing new under the prefix or the scratch HOME's .planar, no config
    // created or modified, and the canary untouched.
    CHECK(qfix::file_bytes(fx.config) == config_before);
    CHECK(listing(fx.prefix) == prefix_before);
    CHECK(listing(fx.work / "fakehome" / ".planar") == home_before);
    CHECK(canary_state(fx.canary) == canary_before);

    // The migrated database serves the queue.
    auto env = parity::pinned_env(fx.work);
    for (auto& var : env) {
      if (var.name == "PLANAR_DB") {
        var.value = fx.db.string();
      }
    }
    std::vector<std::string> const run_true{"queue", "run", "--", "true"};
    auto const                     ran = parity::run_pinned(agent_bin(), run_true, fx.work, "run_true", env);
    INFO("queue run stderr:\n" << ran.err);
    REQUIRE(ran.code == 0);

    // A second run of the step finds the database usable, migrates nothing
    // and changes no table.
    auto const state_before = db_state(fx.db);
    auto const second       = run_seam(fx, "queue_probe_migrate", "second");
    INFO("second stdout:\n" << second.out << "\nsecond stderr:\n" << second.err);
    REQUIRE(second.code == 0);
    CHECK(second.out.find("migrating") == std::string::npos);
    CHECK(second.out.find("queue store probe: usable") != std::string::npos);
    CHECK(db_state(fx.db) == state_before);
    CHECK(qfix::file_bytes(fx.config) == config_before);
    CHECK(canary_state(fx.canary) == canary_before);
  }

  SECTION("an ahead planar.db is never migrated or refused: compatible, incompatible and foreign") {
    struct ahead_case {
      std::string_view tag;
      void (*build)(const std::filesystem::path&);
      std::string_view verdict;
      std::vector<std::string_view> warning; ///< Phrases stderr must carry; empty for none.
    };
    std::array const cases{
        ahead_case{.tag = "iq_ahead", .build = qfix::ahead_store, .verdict = "queue store probe: usable (exit 1, tag not_found)", .warning = {}},
        ahead_case{.tag     = "iq_incompat",
                   .build   = qfix::incompatible_ahead_store,
                   .verdict = "queue store probe: incompatible (exit 125, tag queue_schema_incompatible)",
                   .warning = {"ahead of this build", "until a newer build is installed"}},
        ahead_case{.tag     = "iq_foreign",
                   .build   = qfix::foreign_store,
                   .verdict = "queue store probe: foreign (exit 125, tag queue_schema_foreign)",
                   .warning = {"same number, foreign migration", "a newer build alone will not fix it"}},
    };
    for (auto const& one : cases) {
      auto fx = make_install(one.tag);
      one.build(fx.db);
      auto const versions_before = qfix::applied_versions(fx.db);
      auto const got             = run_seam(fx, "queue_probe_migrate", "ahead");
      INFO(one.tag << " stdout:\n" << got.out << "\nstderr:\n" << got.err);
      CHECK(got.code == 0);
      CHECK(got.out.find(one.verdict) != std::string::npos);
      CHECK(got.out.find("migrating") == std::string::npos);
      CHECK(qfix::applied_versions(fx.db) == versions_before);
      for (auto const phrase : one.warning) {
        CHECK(got.err.find(phrase) != std::string::npos);
      }
      if (one.warning.empty()) {
        CHECK(got.err.empty());
      }
    }
  }

  SECTION("a probe that finds seq 1 as residue still counts as usable") {
    auto fx = make_install("iq_seq1");
    qfix::head_store(fx.db);
    qfix::exec(fx.db, "insert into queue_history (seq, outcome, exit_code, cwd, argv, enqueued_at, ended_at, waited_ms) "
                      "values (1, 'exited', 0, '/', '[\"true\"]', 1, 2, 0)");
    auto const versions_before = qfix::applied_versions(fx.db);
    auto const got             = run_seam(fx, "queue_probe_migrate", "seq1");
    INFO("stdout:\n" << got.out << "\nstderr:\n" << got.err);
    CHECK(got.code == 0);
    CHECK(got.out.find("queue store probe: usable (exit 0 with a status object)") != std::string::npos);
    CHECK(got.out.find("migrating") == std::string::npos);
    CHECK(qfix::applied_versions(fx.db) == versions_before);
  }
}
