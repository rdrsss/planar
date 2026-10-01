// queue_store.t.cpp: the queue's store open path as units (plan 1089, task
// qp-agent-queue-open; tech spec 656 § Store and open path; test spec 658
// "queue status opens planar.db read-only at the SQLite layer").
//
// What the cross-process cases in queue_run_store.t.cpp cannot see lives
// here: that a status handle returns SQLITE_READONLY on a write, that a
// refusal never creates the file, which of two failures wins when a database
// has both, and that the C++ log-directory function and the counter-reset
// helper's `log_directory()` agree on every row of the spec's stem table.
//
// ## Break-probes run against this file
//
//   - Turned `check_queue_schema` at its call site in `open_queue_store` into
//     a no-op -> `queue_store refuses an equal-version database with no queue
//     tables as foreign` and `... ahead ... incompatible` FAIL, and so do the
//     cross-process cases in queue_run_store.t.cpp.
//   - Swapped the handshake and the queue check -> `queue_store reports a
//     database that is behind and has lost its queue tables as behind` FAILS.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.engine.hostqueue;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.queue_store;

#include "parity_harness.hpp"
#include "queue_test_store.hpp"

namespace {

namespace qfix = planar::cmd::qfix;
namespace hq   = planar::engine::hostqueue;
using planar::cmd::agent::open_queue_store;
using planar::cmd::agent::store_access;

constexpr int k_sqlite_readonly = 8;

auto scratch(std::string_view tag) -> std::filesystem::path {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar_queue_store_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(root);
  return root;
}

struct remover {
  std::filesystem::path root;
  ~remover() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

auto env_of(const std::filesystem::path& db) -> planar::cmd::agent::env_lookup {
  return planar::cmd::agent::map_env({{"PLANAR_DB", db.string()}});
}

} // namespace

TEST_CASE("queue_store opens a head planar.db read-write and the handle can write", "[cmd][agent][queue][queue-store]") {
  remover const cleanup{scratch("rw")};
  auto const    db = cleanup.root / "planar.db";
  qfix::head_store(db);

  auto store = open_queue_store(env_of(db), store_access::read_write);
  REQUIRE(store.has_value());
  CHECK(store->path == db);
  CHECK_FALSE(store->conn.is_read_only());
  CHECK(store->conn.execute("insert into queue_schema (version, compat, description) values (9, 1, 'probe')").has_value());
}

TEST_CASE("queue_store's status open is read-only at the SQLite layer", "[cmd][agent][queue][queue-store]") {
  remover const cleanup{scratch("ro")};
  auto const    db = cleanup.root / "planar.db";
  qfix::head_store(db);

  auto store = open_queue_store(env_of(db), store_access::read_only);
  REQUIRE(store.has_value());
  CHECK(store->conn.is_read_only());
  auto const written = store->conn.execute("insert into queue_schema (version, compat, description) values (9, 1, 'probe')");
  REQUIRE_FALSE(written.has_value());
  CHECK((written.error().code_ & 0xff) == k_sqlite_readonly);
}

TEST_CASE("queue_store refuses a missing file and an unset path without creating anything", "[cmd][agent][queue][queue-store]") {
  remover const cleanup{scratch("missing")};
  auto const    db = cleanup.root / "nested" / "planar.db";

  for (auto const access : {store_access::read_write, store_access::read_only}) {
    auto const store = open_queue_store(env_of(db), access);
    REQUIRE_FALSE(store.has_value());
    CHECK(store.error().tag == "store_unreachable");
    CHECK(store.error().message.contains("planar init"));
    CHECK_FALSE(store.error().message.contains('\n'));
  }
  CHECK_FALSE(std::filesystem::exists(db));
  CHECK_FALSE(std::filesystem::exists(db.parent_path()));

  auto const unset = open_queue_store(planar::cmd::agent::map_env({}), store_access::read_write);
  REQUIRE_FALSE(unset.has_value());
  CHECK(unset.error().tag == "store_unreachable");
  CHECK(unset.error().message.contains("PLANAR_DB"));
  CHECK(unset.error().message.contains("HOME"));
}

TEST_CASE("queue_store refuses an equal-version database with no queue tables as foreign", "[cmd][agent][queue][queue-schema]") {
  remover const cleanup{scratch("foreign")};
  auto const    db = cleanup.root / "planar.db";
  qfix::foreign_store(db);

  for (auto const access : {store_access::read_write, store_access::read_only}) {
    auto const store = open_queue_store(env_of(db), access);
    REQUIRE_FALSE(store.has_value());
    CHECK(store.error().tag == hq::k_tag_queue_schema_foreign);
    CHECK(store.error().message.contains("queue_schema"));
  }
}

TEST_CASE("queue_store refuses an ahead database whose queue marker or columns do not admit this binary as incompatible",
          "[cmd][agent][queue][queue-schema]") {
  remover const cleanup{scratch("incompat")};
  auto const    marker = cleanup.root / "marker.db";
  auto const    drift  = cleanup.root / "drift.db";
  qfix::incompatible_ahead_store(marker);
  qfix::shape_drift_store(drift);

  for (auto const& db : {marker, drift}) {
    INFO(db.string());
    auto const store = open_queue_store(env_of(db), store_access::read_write);
    REQUIRE_FALSE(store.has_value());
    CHECK(store.error().tag == hq::k_tag_queue_schema_incompatible);
  }
  CHECK(open_queue_store(env_of(marker), store_access::read_only).error().message.contains("queue version 2"));
  CHECK(open_queue_store(env_of(drift), store_access::read_only).error().message.contains("queue_entries.child_pgid"));
}

TEST_CASE("queue_store admits a compatible ahead database", "[cmd][agent][queue][queue-schema]") {
  remover const cleanup{scratch("ahead")};
  auto const    db = cleanup.root / "planar.db";
  qfix::ahead_store(db);
  CHECK(open_queue_store(env_of(db), store_access::read_write).has_value());
  CHECK(open_queue_store(env_of(db), store_access::read_only).has_value());
}

TEST_CASE("queue_store reports a database that is behind and has lost its queue tables as behind, not foreign",
          "[cmd][agent][queue][queue-schema]") {
  // The version handshake runs first. A behind database may simply not have
  // the queue tables yet, and "foreign" would send the operator the wrong way.
  remover const cleanup{scratch("order")};
  auto const    db = cleanup.root / "planar.db";
  qfix::behind_store(db);
  qfix::exec(db, "drop table queue_schema");

  for (auto const access : {store_access::read_write, store_access::read_only}) {
    auto const store = open_queue_store(env_of(db), access);
    REQUIRE_FALSE(store.has_value());
    CHECK(store.error().tag == "schema_version_behind");
    CHECK(store.error().message.contains("run `planar init`"));
  }
}

TEST_CASE("queue_store does not call a file that is not a database behind", "[cmd][agent][queue][queue-store]") {
  remover const cleanup{scratch("garbage")};
  auto const    db = cleanup.root / "garbage.db";
  {
    std::ofstream out(db, std::ios::binary | std::ios::trunc);
    out << std::string(4096, 'x') << "this is not an SQLite database";
  }
  auto const store = open_queue_store(env_of(db), store_access::read_write);
  REQUIRE_FALSE(store.has_value());
  CHECK(store.error().tag == "store_unreachable");
}

// ---------------------------------------------------------------------------
// The log directory, and its agreement with the counter-reset helper
// ---------------------------------------------------------------------------

namespace {

struct stem_row {
  std::string name;
  std::string logs;
};

/// @brief Tech spec 656's table, plus the default and a nested directory.
auto stem_table() -> std::vector<stem_row> {
  return {{"planar.db", "queue-logs"},  {"other.db", "other.queue-logs"},  {"scratch.sqlite", "scratch.queue-logs"},
          {"a.b.db", "a.b.queue-logs"}, {".hidden", ".hidden.queue-logs"}, {"queuedb", "queuedb.queue-logs"}};
}

} // namespace

TEST_CASE("queue_log_directory follows the stem table in tech spec 656", "[cmd][agent][queue][queue-logs]") {
  auto const dir = std::filesystem::path{"/var/data/planar"};
  for (auto const& row : stem_table()) {
    INFO(row.name);
    CHECK(planar::cmd::agent::queue_log_directory(dir / row.name) == dir / row.logs);
  }
}

TEST_CASE("queue_log_directory agrees with scripts/queue-logs-after-reset.py's log_directory() on every row",
          "[cmd][agent][queue][queue-logs]") {
  namespace parity  = planar::cmd::parity;
  auto const dir    = std::filesystem::path{"/var/data/planar"};
  auto const script = std::filesystem::path{PLANAR_REPO_ROOT} / "scripts" / "queue-logs-after-reset.py";
  REQUIRE(std::filesystem::exists(script));

  // The helper is imported, not run: its `main` is guarded.
  std::string const program = "import importlib.util, sys\n"
                              "spec = importlib.util.spec_from_file_location('queue_logs_after_reset', sys.argv[1])\n"
                              "module = importlib.util.module_from_spec(spec)\n"
                              "spec.loader.exec_module(module)\n"
                              "for path in sys.argv[2:]:\n"
                              "    print(module.log_directory(path))\n";
  std::string       command = "python3 -c " + parity::shell_quote(program) + " " + parity::shell_quote(script.string());
  for (auto const& row : stem_table()) {
    command += " " + parity::shell_quote((dir / row.name).string());
  }
  auto* pipe = ::popen(command.c_str(), "r");
  REQUIRE(pipe != nullptr);
  std::string printed;
  char        buffer[512];
  while (auto const n = std::fread(buffer, 1, sizeof buffer, pipe)) {
    printed.append(buffer, n);
  }
  REQUIRE(::pclose(pipe) == 0);

  std::vector<std::string> from_python;
  std::istringstream       lines(printed);
  for (std::string line; std::getline(lines, line);) {
    from_python.push_back(line);
  }
  auto const table = stem_table();
  REQUIRE(from_python.size() == table.size());
  for (std::size_t i = 0; i < table.size(); ++i) {
    INFO(table[i].name);
    CHECK(from_python[i] == planar::cmd::agent::queue_log_directory(dir / table[i].name).string());
    CHECK(from_python[i] == (dir / table[i].logs).string());
  }
}
