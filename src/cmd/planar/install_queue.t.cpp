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
// `queue_retire_live_oracle` (task qp-install-retire) is the only check that
// `scripts/install-lib/queue_retire.py`, the installer's fail-closed reader of
// the retired `agent.db`, agrees with the ENGINE on identity format, start-time
// units and parsing: it writes rows with the real `planar.process.identity`
// and asks the reader for its verdicts. The reader's own suite
// (`queue_retire_reader`) builds its rows with the reader's readers, which is
// self-consistent by design and cannot catch a wrong struct offset or `/proc`
// field index.
//
// The installer's prefix is a scratch directory under the arena, and the
// scratch `HOME` holds a canary `.planar/planar.db`. The seam is run through
// `run_pinned` with `PLANAR_DB` REMOVED from the environment, so the only way
// any binary reaches a database is the path the step itself passes; a step
// that forgot to pass it would reach the canary, and the canary is checked.
#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.process.identity;

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

/// @brief Fails the case, before any binary runs, when a fixture step failed.
/// @param what The step.
/// @param ok Whether it worked.
void must(std::string_view what, bool ok) {
  if (!ok) {
    throw std::runtime_error(std::format("install fixture: {} failed", what));
  }
}

/// @brief The installer's store reader in this checkout.
/// @return The path.
auto reader_py() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_TARGET_SOURCE_ROOT} / "scripts" / "install-lib" / "queue_retire.py";
}

/// @brief The queue tables of the retired agent store (agent migrations
/// 00002 and 00003), which `queue_retire.py` reads.
constexpr std::string_view k_agent_schema = R"(
create table agent_schema_migrations (version integer primary key, compat integer not null, description text not null);
create table queue_entries (
  seq integer primary key autoincrement,
  state text not null check(state in ('waiting', 'running')),
  host_id text not null, pid integer not null, pid_started integer not null,
  child_pgid integer, child_started integer, parent_seq integer, terminating_since_mono integer,
  terminate_reason text check(terminate_reason in ('timeout', 'cancelled')), cancelled_by text,
  cwd text not null, argv text not null, label text, vendor text, role text, claim_token text, log_path text,
  enqueued_at integer not null, started_at integer, refreshed_mono integer not null, deadline_mono integer,
  wait_deadline_mono integer, run_limit_ms integer, wait_limit_ms integer);
create table queue_history (
  seq integer primary key, outcome text not null, exit_code integer, signal integer, successor_seq integer,
  cancelled_by text, nested integer not null default 0, parent_seq integer, cwd text not null, argv text not null,
  label text, vendor text, role text, log_path text, enqueued_at integer not null, started_at integer,
  ended_at integer not null, waited_ms integer not null, ran_ms integer, run_limit_ms integer, wait_limit_ms integer);
)";

/// @brief Creates an idle retired agent store at `path`: the queue tables,
/// no entries, an old maximum sequence number of 57.
/// @param path The store.
void idle_agent_store(const std::filesystem::path& path) {
  auto opened = planar::db::connection::open(path.string());
  must("open the agent store", opened.has_value());
  must("create the agent tables", opened->execute(k_agent_schema).has_value());
  must("seed the old range",
       opened
           ->execute("insert into queue_history (seq, outcome, cwd, argv, enqueued_at, ended_at, waited_ms) "
                     "values (57, 'exited', '/', '[]', 1, 2, 0); "
                     "insert into sqlite_sequence (name, seq) values ('queue_entries', 57);")
           .has_value());
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
/// @param arg The function's argument, when it takes one.
/// @return What the call printed and its status.
auto run_seam(const install_fixture& fx, std::string_view fn, std::string_view tag, std::string_view arg = {}) -> capture {
  auto env = parity::pinned_env(fx.work);
  for (auto& var : env) {
    if (var.name == "PLANAR_DB") {
      var.unset = true;
    }
    if (var.name == "PLANAR_HOME") {
      var.value = fx.prefix.string();
    }
  }
  std::string const script =
      R"(set -eEuo pipefail; p="$PLANAR_HOME"; unset PLANAR_HOME; PLANAR_HOME="$p"; source "$1"; shift; "$@")";
  std::vector<std::string> args{"-c", script, "bash", seam_lib().string(), std::string{fn}};
  if (!arg.empty()) {
    args.emplace_back(arg);
  }
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
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
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
      std::string_view              verdict;
      std::vector<std::string_view> warning; ///< Phrases stderr must carry; empty for none.
    };
    std::array const cases{
        ahead_case{.tag     = "iq_ahead",
                   .build   = qfix::ahead_store,
                   .verdict = "queue store probe: usable (exit 1, tag not_found)",
                   .warning = {}},
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

      // ...and an idle agent.db beside it is still retired.
      idle_agent_store(fx.prefix / "agent.db");
      auto const recheck = run_seam(fx, "queue_live_guard", "recheck", "re-check");
      INFO("re-check stderr:\n" << recheck.err);
      CHECK(recheck.code == 0);
      auto const retired = run_seam(fx, "queue_retire_store", "retire");
      INFO("retire stderr:\n" << retired.err);
      CHECK(retired.code == 0);
      CHECK_FALSE(std::filesystem::exists(fx.prefix / "agent.db"));
      CHECK(std::filesystem::exists(fx.db));
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

namespace {

/// @brief A child this case started, killed and reaped when the case ends
/// however it ends. Only ever signals its own pid, or the group it made.
class child {
public:
  /// @brief Starts `/bin/sleep 300`, in a process group of its own when
  /// `own_group` is set.
  /// @param own_group Whether the child leads a new process group.
  explicit child(bool own_group) {
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    if (own_group) {
      posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
      posix_spawnattr_setpgroup(&attr, 0);
    }
    std::array<char*, 3> argv{const_cast<char*>("sleep"), const_cast<char*>("300"), nullptr};
    ::pid_t              pid = 0;
    int const            rc  = ::posix_spawn(&pid, "/bin/sleep", nullptr, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    must("spawn a sleeping child", rc == 0 && pid > 1);
    _pid   = pid;
    _group = own_group;
  }
  child(const child&)                    = delete;
  auto operator=(const child&) -> child& = delete;
  child(child&&)                         = delete;
  auto operator=(child&&) -> child&      = delete;
  ~child() {
    reap();
  }

  /// @brief The child's pid (and, for a group child, its pgid).
  /// @return The pid.
  [[nodiscard]] auto pid() const -> std::int64_t {
    return _pid;
  }

  /// @brief Kills the child (and its group, when it leads one) and reaps it.
  void reap() {
    if (_pid <= 1) {
      return;
    }
    if (_group) {
      ::kill(-_pid, SIGKILL);
    } else {
      ::kill(_pid, SIGKILL);
    }
    int status = 0;
    ::waitpid(_pid, &status, 0);
    _pid = 0;
  }

private:
  ::pid_t _pid   = 0;
  bool    _group = false;
};

/// @brief The pid of a child that has already exited and been reaped.
/// @return The pid.
auto reaped_pid() -> std::int64_t {
  std::array<char*, 2> argv{const_cast<char*>("true"), nullptr};
  ::pid_t              pid = 0;
  must("spawn true", ::posix_spawn(&pid, "/usr/bin/true", nullptr, nullptr, argv.data(), environ) == 0);
  int status = 0;
  ::waitpid(pid, &status, 0);
  return pid;
}

/// @brief The engine's start time for `pid`, read through the real
/// `planar.process.identity`.
/// @param pid A live child of this case.
/// @return The start time.
auto engine_start_time(std::int64_t pid) -> std::int64_t {
  for (int attempt = 0; attempt < 200; ++attempt) {
    auto const got = planar::process::identity::process_start_time(pid);
    if (got && got->has_value()) {
      return static_cast<std::int64_t>(**got);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error(std::format("no engine start time for child {}", pid));
}

/// @brief One `queue_entries` row, as the engine would write it.
struct oracle_row {
  std::int64_t                seq;           ///< The sequence number.
  std::string_view            state;         ///< `waiting` or `running`.
  std::string                 host_id;       ///< The submitter's host identity.
  std::int64_t                pid;           ///< The submitter's pid.
  std::int64_t                pid_started;   ///< The submitter's start time.
  std::optional<std::int64_t> child_pgid;    ///< The child group, for a running row.
  std::optional<std::int64_t> child_started; ///< The group leader's start time.
};

/// @brief Writes `rows` into a fresh retired-store file at `path`.
/// @param path The store.
/// @param rows The rows.
void write_oracle_store(const std::filesystem::path& path, std::span<const oracle_row> rows) {
  auto opened = planar::db::connection::open(path.string());
  must("open the oracle store", opened.has_value());
  must("create the agent tables", opened->execute(k_agent_schema).has_value());
  auto stmt =
      opened->prepare("insert into queue_entries (seq, state, host_id, pid, pid_started, child_pgid, child_started, cwd, "
                      "argv, enqueued_at, refreshed_mono) values (?, ?, ?, ?, ?, ?, ?, '/', '[\"sleep\",\"300\"]', 1, 1)");
  must("prepare the row insert", stmt.has_value());
  for (auto const& row : rows) {
    must("reset the row insert", stmt->reset().has_value());
    bool ok            = stmt->bind_int64(1, row.seq).has_value() && stmt->bind_text(2, row.state).has_value() &&
                         stmt->bind_text(3, row.host_id).has_value() && stmt->bind_int64(4, row.pid).has_value() &&
                         stmt->bind_int64(5, row.pid_started).has_value();
    ok                 = ok && (row.child_pgid ? stmt->bind_int64(6, *row.child_pgid) : stmt->bind_null(6)).has_value();
    ok                 = ok && (row.child_started ? stmt->bind_int64(7, *row.child_started) : stmt->bind_null(7)).has_value();
    auto const stepped = stmt->step();
    must("insert an oracle row", ok && stepped.has_value());
  }
}

/// @brief Runs `queue_retire.py live` on `store`.
/// @param work The arena root.
/// @param store The store.
/// @param tag A capture discriminator.
/// @return Its output and exit status.
auto reader_live(const std::filesystem::path& work, const std::filesystem::path& store, std::string_view tag) -> capture {
  std::vector<std::string> const args{reader_py().string(), "live", store.string()};
  return parity::run_pinned(PLANAR_PYTHON3, args, work, tag);
}

/// @brief Whether `out` lists row `seq` with verdict `kind` (`blocking` or `dead`).
/// @param out The reader's output.
/// @param kind The verdict word.
/// @param seq The row.
/// @return Whether the line is there.
auto lists(const std::string& out, std::string_view kind, std::int64_t seq) -> bool {
  return out.find(std::format("\n{} seq={} ", kind, seq)) != std::string::npos ||
         out.starts_with(std::format("{} seq={} ", kind, seq));
}

} // namespace

TEST_CASE("queue_retire_live_oracle", "[cmd][install][queue]") {
  namespace identity    = planar::process::identity;
  auto const      arena = parity::make_arena("qr_oracle");
  auto const      work  = arena.cpp_root;
  auto const      store = work / "oracle" / "agent.db";
  std::error_code ec;
  std::filesystem::create_directories(store.parent_path(), ec);

  auto const host = identity::host_identity(identity::native_identity_source());
  REQUIRE(host != identity::k_unknown_host_identity);

  child      submitter{false};
  child      group{true};
  child      reused{false};
  auto const dead = reaped_pid();

  std::array const rows{
      // 1: a live waiting submitter with its TRUE start time. A reader that
      // computes a different start time (a wrong ctypes offset, a wrong /proc
      // field index) calls it dead.
      oracle_row{.seq           = 1,
                 .state         = "waiting",
                 .host_id       = host,
                 .pid           = submitter.pid(),
                 .pid_started   = engine_start_time(submitter.pid()),
                 .child_pgid    = {},
                 .child_started = {}},
      // 2: a running entry whose submitter is gone but whose child group,
      // started in its own group, is live with the engine's start time.
      oracle_row{.seq           = 2,
                 .state         = "running",
                 .host_id       = host,
                 .pid           = dead,
                 .pid_started   = 1,
                 .child_pgid    = group.pid(),
                 .child_started = engine_start_time(group.pid())},
      // 3: a live pid whose stored start time is the truth + 1: a reused pid.
      // A reader with no usable start time (a wrong struct size) cannot
      // prove it different and blocks.
      oracle_row{.seq           = 3,
                 .state         = "waiting",
                 .host_id       = host,
                 .pid           = reused.pid(),
                 .pid_started   = engine_start_time(reused.pid()) + 1,
                 .child_pgid    = {},
                 .child_started = {}},
  };
  write_oracle_store(store, rows);

  auto const first = reader_live(work, store, "first");
  INFO("first:\n" << first.out << first.err);
  CHECK(first.code == 3);
  CHECK(lists(first.out, "blocking", 1));
  CHECK(lists(first.out, "blocking", 2));
  CHECK(lists(first.out, "dead", 3));

  submitter.reap();
  auto const second = reader_live(work, store, "second");
  INFO("after the submitter is reaped:\n" << second.out << second.err);
  CHECK(second.code == 3);
  CHECK(lists(second.out, "dead", 1));
  CHECK(lists(second.out, "blocking", 2));
  CHECK(lists(second.out, "dead", 3));

  group.reap();
  auto const third = reader_live(work, store, "third");
  INFO("after the group is reaped:\n" << third.out << third.err);
  CHECK(third.code == 0);
  CHECK(lists(third.out, "dead", 1));
  CHECK(lists(third.out, "dead", 2));
  CHECK(lists(third.out, "dead", 3));
}

TEST_CASE("queue_retire_ignores_the_new_queue", "[cmd][install][queue]") {
  // A live new-binary run in planar.db never blocks retiring agent.db, and
  // its log (numbered above the floor) survives the retire.
  auto fx = make_install("qr_newqueue");
  qfix::head_store(fx.db);
  idle_agent_store(fx.prefix / "agent.db");

  auto env = parity::pinned_env(fx.work);
  for (auto& var : env) {
    if (var.name == "PLANAR_DB") {
      var.value = fx.db.string();
    }
  }
  std::vector<std::string> const detach{"queue", "run", "--detach", "--", "sleep", "2"};
  auto const                     ticket = parity::run_pinned(agent_bin(), detach, fx.work, "detach", env);
  INFO("detach:\n" << ticket.out << ticket.err);
  REQUIRE(ticket.code == 0);
  auto const seq = ticket.out.substr(0, ticket.out.find('\n'));
  REQUIRE(std::stoll(seq) > qfix::k_seq_floor);
  auto const log = fx.prefix / "queue-logs" / std::format("{}.log", seq);

  auto const recheck = run_seam(fx, "queue_live_guard", "recheck", "re-check");
  INFO("re-check:\n" << recheck.out << recheck.err);
  CHECK(recheck.code == 0);
  auto const retired = run_seam(fx, "queue_retire_store", "retire");
  INFO("retire:\n" << retired.out << retired.err);
  CHECK(retired.code == 0);
  CHECK_FALSE(std::filesystem::exists(fx.prefix / "agent.db"));

  // The new run completes normally and keeps its log.
  std::string state;
  for (int attempt = 0; attempt < 400; ++attempt) {
    std::vector<std::string> const status{"queue", "status", seq, "--json"};
    auto const                     got = parity::run_pinned(agent_bin(), status, fx.work, "status", env);
    if (got.out.find(R"("state":"ended")") != std::string::npos) {
      state = got.out;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  INFO("final status: " << state);
  REQUIRE_FALSE(state.empty());
  CHECK(state.find(R"("outcome":"exited")") != std::string::npos);
  CHECK(state.find(R"("exit_code":0)") != std::string::npos);
  CHECK(std::filesystem::exists(log));
}
