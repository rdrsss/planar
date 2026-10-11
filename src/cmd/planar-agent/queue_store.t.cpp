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
using planar::cmd::agent::claim_standing_lookup;
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

TEST_CASE("bounded queue store opens and failed partial opens release their connection in process",
          "[cmd][agent][queue][bqw1123-deadlines]") {
  remover const cleanup{scratch("bounded")};
  auto const    db = cleanup.root / "planar.db";
  qfix::head_store(db);
  auto writer = open_queue_store(env_of(db), store_access::read_write);
  REQUIRE(writer.has_value());

  for (int repeat = 0; repeat < 24; ++repeat) {
    int  checks = 0;
    auto budget = [&]() -> std::expected<int, planar::cmd::agent::store_refusal> {
      ++checks;
      if ((repeat % 2) == 0 && checks == 4) {
        return std::unexpected(planar::cmd::agent::store_refusal{.tag = "timed_out", .message = "deadline expired"});
      }
      return 2;
    };
    auto opened = planar::cmd::agent::open_queue_store_for_wait(env_of(db), budget);
    if ((repeat % 2) == 0) {
      REQUIRE_FALSE(opened.has_value());
      CHECK(opened.error().tag == "timed_out");
    } else {
      REQUIRE(opened.has_value());
      CHECK(opened->conn.is_read_only());
      CHECK_FALSE(opened->conn.in_transaction());
    }
    CHECK(checks >= 4);
    CHECK(writer->conn.execute("update queue_schema set description = 'probe' where version = 1").has_value());
  }
  // No read snapshot or statement from a prior invocation may pin the WAL.
  auto checkpoint = writer->conn.prepare("pragma wal_checkpoint(truncate)");
  REQUIRE(checkpoint.has_value());
  auto row = checkpoint->step();
  REQUIRE(row.has_value());
  REQUIRE(*row == planar::db::step_result::row);
  CHECK(checkpoint->column_int64(0) == 0);
}

TEST_CASE("bounded store read stops promptly when interrupted under an exclusive lock",
          "[cmd][agent][queue][bqw1123-deadlines]") {
  remover const cleanup{scratch("interrupted")};
  auto const    db = cleanup.root / "planar.db";
  qfix::head_store(db);
  auto writer = open_queue_store(env_of(db), store_access::read_write);
  REQUIRE(writer.has_value());
  REQUIRE(writer->conn.execute("pragma journal_mode = delete").has_value());
  REQUIRE(writer->conn.execute("begin exclusive").has_value());

  int  checks = 0;
  auto budget = [&]() -> std::expected<int, planar::cmd::agent::store_refusal> {
    if (++checks >= 5) {
      return std::unexpected(planar::cmd::agent::store_refusal{.tag = "interrupted", .message = "observer signaled"});
    }
    return 10'000;
  };
  auto const start   = std::chrono::steady_clock::now();
  auto       blocked = planar::cmd::agent::open_queue_store_for_wait(env_of(db), budget);
  auto const elapsed = std::chrono::steady_clock::now() - start;
  REQUIRE_FALSE(blocked.has_value());
  CHECK(blocked.error().tag == "interrupted");
  CHECK(elapsed < std::chrono::seconds{1});
  REQUIRE(writer->conn.execute("rollback").has_value());

  auto resumed = planar::cmd::agent::open_queue_store_for_wait(
      env_of(db), []() -> std::expected<int, planar::cmd::agent::store_refusal> { return 1'000; });
  REQUIRE(resumed.has_value());
  CHECK(resumed->conn.is_read_only());
  CHECK_FALSE(resumed->conn.in_transaction());
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
  CHECK(open_queue_store(env_of(marker), store_access::read_only)
            .error()
            .message.contains(std::format("queue version {}", hq::k_queue_schema_version + 1)));
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

// ---------------------------------------------------------------------------
// The claim-standing lookup the history retention prune uses (plan 1142, task
// mo-queue-claim-evidence).
// ---------------------------------------------------------------------------

namespace {

/// @brief Runs `sql` on `conn`, failing the case when it does not.
void run_sql(planar::db::connection& conn, std::string_view sql) {
  auto ran = conn.execute(sql);
  qfix::must(std::string{sql}, ran.has_value(), ran ? "" : ran.error().message_);
}

/// @brief Seeds an active plan (1), a task on it (1), a plan-less task (2), a
/// session, and one claim per entity kind. Tokens are `task-claim`,
/// `plan-claim`, `step-claim` and `no-plan-claim`.
void seed_claims(planar::db::connection& conn) {
  run_sql(conn, "insert into plans (scope_kind, title, slug, status) values ('global', 'p', 'standing-plan', 'active')");
  run_sql(conn, "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 1, 't', 'todo', 100)");
  run_sql(conn, "insert into tasks (scope_kind, title, status, priority) values ('global', 'orphan', 'todo', 100)");
  run_sql(conn, "insert into plan_steps (plan_id, ordinal, body) values (1, 1, 'step')");
  run_sql(conn, "insert into sessions (vendor) values ('claude')");
  for (auto const& [token, kind, id] : std::vector<std::tuple<std::string, std::string, int>>{
           {"task-claim", "task", 1}, {"plan-claim", "plan", 1}, {"step-claim", "plan_step", 1}, {"no-plan-claim", "task", 2}}) {
    run_sql(conn, std::format("insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, vendor, "
                              "lease_expires_at) values ('{}', 1, '{}', {}, 'claude', '2026-10-01T10:00:00.000Z')",
                              token, kind, id));
  }
}

} // namespace

TEST_CASE("claim_standing_lookup reports the plan state and when the claim ended",
          "[cmd][agent][queue][queue-store][mo-queue-claim-evidence]") {
  remover const guard{scratch("standing")};
  auto const    db   = guard.root / "planar.db";
  auto          conn = qfix::open_store(db);
  REQUIRE(conn.has_value());
  seed_claims(*conn);
  auto const lookup = claim_standing_lookup(*conn);
  using state       = hq::claim_standing::plan_state;

  // An active claim's end is its lease expiry.
  constexpr std::int64_t k_lease_ms = 1'790'848'800'000; // 2026-10-01T10:00:00Z
  for (std::string const token : {"task-claim", "plan-claim", "step-claim"}) {
    INFO(token);
    auto const standing = lookup(token);
    CHECK(standing.plan == state::open);
    CHECK(standing.claim_ended_ms == std::optional<std::int64_t>{k_lease_ms});
  }

  // A released claim's end is its release time, which wins over the lease.
  run_sql(*conn, "update agent_work_claims set status = 'completed', released_at = '2026-10-02T11:00:00.500Z' "
                 "where claim_token = 'task-claim'");
  CHECK(lookup("task-claim").claim_ended_ms == std::optional<std::int64_t>{1'790'938'800'000});

  // Every closed status closes the plan; draft and paused are open.
  for (auto const& [status, expected] : std::vector<std::pair<std::string, state>>{
           {"draft", state::open}, {"paused", state::open}, {"done", state::closed}, {"abandoned", state::closed}}) {
    INFO(status);
    run_sql(*conn, std::format("update plans set status = '{}' where id = 1", status));
    CHECK(lookup("task-claim").plan == expected);
    CHECK(lookup("plan-claim").plan == expected);
    CHECK(lookup("step-claim").plan == expected);
  }

  // A claim with no plan, and a token no claim has, are unknown (the end time
  // of the first is still known).
  CHECK(lookup("no-plan-claim").plan == state::unknown);
  CHECK(lookup("no-plan-claim").claim_ended_ms.has_value());
  auto const missing = lookup("no-such-claim");
  CHECK(missing.plan == state::unknown);
  CHECK_FALSE(missing.claim_ended_ms.has_value());
}

TEST_CASE("claim_standing_lookup fails safe when the claim tables cannot be read",
          "[cmd][agent][queue][queue-store][mo-queue-claim-evidence]") {
  remover const guard{scratch("standing_failsafe")};
  auto          conn = qfix::open_store(guard.root / "planar.db");
  REQUIRE(conn.has_value());
  seed_claims(*conn);
  auto const lookup = claim_standing_lookup(*conn);
  REQUIRE(lookup("task-claim").plan == hq::claim_standing::plan_state::open);

  // The lookup answers unknown (not open, not closed, no end time) when a
  // table or column it reads is gone: the retention prune then applies only
  // its 90-day cap.
  run_sql(*conn, "drop table plan_steps");
  CHECK(lookup("task-claim").plan == hq::claim_standing::plan_state::unknown);
  CHECK_FALSE(lookup("task-claim").claim_ended_ms.has_value());
}
