// @file queue.t.cpp
// @brief Unit tests for `planar.engine.hostqueue.queue` (plan 1080, task
// hq-enqueue). Covers the test-spec scenarios "Happy path -- enqueue records
// the submitter" and "Edge -- arguments with spaces and quotes survive", and
// the schema half of the task: main migration 00040 (plan 1089) creates
// `queue_entries` and `queue_history` with their CHECK constraints and the
// sequence floor, and rolls back cleanly.
//
// Every case opens its own scratch planar.db through `open_main_store_at`
// (scratch_store.hpp), built from the embedded MAIN chain, so
// nothing here reads the process environment or the real ~/.planar. Host
// identity, pid, start time and both clocks are plain values the test
// chooses (test-spec § Strategy: "controlled process identities and
// clocks").
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.engine.hostqueue;

#include "scratch_store.hpp"

namespace {

namespace hq = planar::engine::hostqueue;

/// @brief A unique scratch directory under the system temp directory,
/// removed (recursively, best-effort) when the guard goes out of scope.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_hostqueue_test_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_);
  }

  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;

  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
};

/// @brief A scratch planar.db at the head of the embedded main chain.
auto open_scratch_store(const scratch_dir& scratch) -> planar::db::connection {
  auto opened = open_main_store_at(scratch.path_ / "planar.db");
  REQUIRE(opened.has_value());
  return std::move(*opened);
}

/// @brief The first column of the first row of `sql`, as text.
auto scalar(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  return stmt->column_text(0);
}

/// @brief Whether `table` exists on `conn`.
auto has_table(planar::db::connection& conn, std::string_view table) -> bool {
  return scalar(conn, std::format("select count(*) from sqlite_master where type = 'table' and name = '{}'", table)) == "1";
}

/// @brief `sqlite_master` as one string, so a down/up roundtrip can be
/// compared structurally. Same normalisation as `src/lib/db/migrate.t.cpp`.
auto schema_dump(planar::db::connection& conn) -> std::string {
  auto stmt = conn.prepare("select type, name, tbl_name, replace(ifnull(sql, ''), '\"', '') "
                           "from sqlite_master order by type, name, tbl_name");
  REQUIRE(stmt.has_value());
  std::string out;
  while (stmt->step().value() == planar::db::step_result::row) {
    for (int column = 0; column < 4; ++column) {
      out += stmt->column_text(column);
      out += '\x1f';
    }
    out += '\x1e';
  }
  return out;
}

/// @brief A complete request with every optional set, so a read-back test
/// can compare every column.
auto full_request() -> hq::enqueue_request {
  return hq::enqueue_request{
      .host_id            = "boot-7f3a",
      .pid                = 4242,
      .pid_started        = 1'700'000'123,
      .cwd                = "/work/planar",
      .argv               = {"make", "test"},
      .label              = "unit gate",
      .vendor             = "claude",
      .role               = "coder",
      .claim_token        = "53f83462ecb9f6c6c90738a108db92fb",
      .log_path           = "/home/u/.planar/queue-logs/1.log",
      .enqueued_at        = 1'759'000'000'000,
      .refreshed_mono     = 90'000,
      .wait_deadline_mono = 3'690'000,
      .parent_seq         = std::nullopt,
  };
}

} // namespace

TEST_CASE("enqueue records the submitter and returns a sequence number above every earlier one",
          "[engine][hostqueue][hq-enqueue]") {
  // Test-spec "Happy path -- enqueue records the submitter".
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto const request = full_request();
  auto const first   = hq::enqueue(conn, request);
  REQUIRE(first.has_value());
  CHECK(*first >= 1);

  auto found = hq::find(conn, *first);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  auto const& entry = **found;
  CHECK(entry.seq == *first);
  CHECK(entry.state == hq::entry_state::waiting);
  CHECK(entry.host_id == request.host_id);
  CHECK(entry.pid == request.pid);
  CHECK(entry.pid_started == request.pid_started);
  CHECK(entry.cwd == request.cwd);
  CHECK(entry.argv == request.argv);
  CHECK(entry.label == request.label);
  CHECK(entry.vendor == request.vendor);
  CHECK(entry.role == request.role);
  CHECK(entry.claim_token == request.claim_token);
  CHECK(entry.log_path == request.log_path);
  CHECK(entry.enqueued_at == request.enqueued_at);
  CHECK(entry.refreshed_mono == request.refreshed_mono);
  CHECK(entry.wait_deadline_mono == request.wait_deadline_mono);
  // Nothing has started, stopped or been cancelled.
  CHECK_FALSE(entry.parent_seq.has_value());
  CHECK_FALSE(entry.child_pgid.has_value());
  CHECK_FALSE(entry.child_started.has_value());
  CHECK_FALSE(entry.started_at.has_value());
  CHECK_FALSE(entry.deadline_mono.has_value());
  CHECK_FALSE(entry.terminating_since_mono.has_value());
  CHECK_FALSE(entry.terminate_reason.has_value());
  CHECK_FALSE(entry.cancelled_by.has_value());

  // A second submitter, from another process on the same host, gets a
  // higher number; the order of arrival is the order of sequence numbers.
  auto second_request = request;
  second_request.pid  = 4243;
  second_request.argv = {"make", "fmt-check"};
  auto const second   = hq::enqueue(conn, second_request);
  REQUIRE(second.has_value());
  CHECK(*second > *first);

  auto listed = hq::list(conn);
  REQUIRE(listed.has_value());
  REQUIRE(listed->size() == 2);
  CHECK((*listed)[0].seq == *first);
  CHECK((*listed)[0].pid == 4242);
  CHECK((*listed)[1].seq == *second);
  CHECK((*listed)[1].pid == 4243);
  CHECK((*listed)[1].argv == std::vector<std::string>{"make", "fmt-check"});

  // Even after the earlier entries are gone, the next number is higher than
  // any that was ever issued: a sequence number is never reused.
  REQUIRE(conn.execute("delete from queue_entries;").has_value());
  auto const third = hq::enqueue(conn, request);
  REQUIRE(third.has_value());
  CHECK(*third > *second);
  auto const gone = hq::find(conn, *first);
  REQUIRE(gone.has_value());
  CHECK_FALSE(gone->has_value());
}

TEST_CASE("an argv with a space, a double quote, a newline and non-ASCII text is read back as the same list",
          "[engine][hostqueue][hq-enqueue]") {
  // Test-spec "Edge -- arguments with spaces and quotes survive".
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  std::vector<std::string> const argv{
      "ctest",
      "-R",
      "a test with spaces",
      "say \"hello\"",
      "line one\nline two",
      "caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x9A\x80",
      "",
      "back\\slash and 'single' quotes",
      "[not, json]",
  };
  auto request = full_request();
  request.argv = argv;

  auto const seq = hq::enqueue(conn, request);
  REQUIRE(seq.has_value());

  auto found = hq::find(conn, *seq);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  CHECK((*found)->argv == argv);
  CHECK((*found)->argv.size() == argv.size());

  // The column is a JSON array of strings, not a joined line: element
  // boundaries are the array's, so the stored text parses back to the same
  // count and the empty argument is still there.
  auto const stored  = scalar(conn, std::format("select argv from queue_entries where seq = {}", *seq));
  auto const decoded = hq::decode_argv(stored);
  REQUIRE(decoded.has_value());
  CHECK(*decoded == argv);
  CHECK(stored.front() == '[');
  CHECK(stored.back() == ']');
  CHECK(stored == hq::encode_argv(argv));

  // A store written by something else, holding text that is not a JSON
  // array of strings, is refused rather than misread.
  CHECK_FALSE(hq::decode_argv("make test").has_value());
  CHECK_FALSE(hq::decode_argv("[1, 2]").has_value());
  CHECK(hq::decode_argv("[\"only\"]").value() == std::vector<std::string>{"only"});
  CHECK(hq::decode_argv("[]").value().empty());
}

TEST_CASE("a request with a parent sequence number is stored running with its parent", "[engine][hostqueue][hq-enqueue]") {
  // Only the storage half of a nested run: task hq-nested-entry owns the
  // rest. An entry with no optionals stores SQL NULL, not empty text.
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto const parent = hq::enqueue(conn, full_request());
  REQUIRE(parent.has_value());

  hq::enqueue_request nested{
      .host_id        = "boot-7f3a",
      .pid            = 5000,
      .pid_started    = 1'700'000'999,
      .cwd            = "/work/planar/sub",
      .argv           = {"true"},
      .enqueued_at    = 1'759'000'001'000,
      .refreshed_mono = 91'000,
      .parent_seq     = *parent,
  };
  auto const child = hq::insert_nested_entry(conn, nested);
  REQUIRE(child.has_value());
  CHECK(*child > *parent);

  auto found = hq::find(conn, *child);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  CHECK((*found)->state == hq::entry_state::running);
  CHECK((*found)->parent_seq == *parent);
  CHECK_FALSE((*found)->label.has_value());
  CHECK_FALSE((*found)->vendor.has_value());
  CHECK_FALSE((*found)->role.has_value());
  CHECK_FALSE((*found)->claim_token.has_value());
  CHECK_FALSE((*found)->log_path.has_value());
  CHECK_FALSE((*found)->wait_deadline_mono.has_value());
  CHECK(scalar(conn, std::format("select count(*) from queue_entries where seq = {} and label is null and vendor is null "
                                 "and role is null and claim_token is null and log_path is null and wait_deadline_mono is null",
                                 *child)) == "1");
  CHECK(hq::to_string(hq::entry_state::running) == "running");
  CHECK(hq::to_string(hq::entry_state::waiting) == "waiting");

  // The parent is untouched by the nested insert.
  auto const parent_entry = hq::find(conn, *parent);
  REQUIRE(parent_entry.has_value());
  CHECK((*parent_entry)->state == hq::entry_state::waiting);
  CHECK_FALSE((*parent_entry)->parent_seq.has_value());
}

TEST_CASE("main migration 00040 creates the queue tables with their CHECK constraints and the sequence floor",
          "[engine][hostqueue][migrations][hq-enqueue][qp-separability]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  REQUIRE(planar::db::current_version(conn).value() >= 40);
  CHECK(has_table(conn, "queue_entries"));
  CHECK(has_table(conn, "queue_history"));
  CHECK(has_table(conn, "queue_schema"));
  CHECK_FALSE(has_table(conn, "agent_schema_migrations"));
  CHECK(scalar(conn, "select compat from queue_schema where version = 1") == "1");
  CHECK(scalar(conn, "select count(*) from sqlite_master where type = 'index' and tbl_name = 'queue_entries' "
                     "and name = 'idx_queue_entries_state'") == "1");
  CHECK(scalar(conn, "select count(*) from sqlite_master where type = 'index' and tbl_name = 'queue_history' "
                     "and name = 'idx_queue_history_ended_at'") == "1");

  // A row the engine would never write is refused by the schema itself.
  auto const insert_entry = [&](std::string_view state, std::string_view terminate_reason) {
    return conn.execute(std::format("insert into queue_entries (state, host_id, pid, pid_started, cwd, argv, enqueued_at, "
                                    "refreshed_mono, terminate_reason) values ('{}', 'h', 1, 1, '/', '[]', 0, 0, {});",
                                    state, terminate_reason));
  };
  CHECK(insert_entry("waiting", "null").has_value());
  CHECK(insert_entry("running", "'timeout'").has_value());
  CHECK(insert_entry("running", "'cancelled'").has_value());
  auto const bad_state = insert_entry("finished", "null");
  REQUIRE_FALSE(bad_state.has_value());
  CHECK((bad_state.error().code_ & 0xff) == 19); // SQLITE_CONSTRAINT
  auto const bad_reason = insert_entry("running", "'bored'");
  REQUIRE_FALSE(bad_reason.has_value());
  CHECK((bad_reason.error().code_ & 0xff) == 19);
  CHECK(scalar(conn, "select count(*) from queue_entries") == "3");

  auto const insert_history = [&](std::string_view outcome) {
    return conn.execute(std::format("insert into queue_history (seq, outcome, cwd, argv, enqueued_at, ended_at, waited_ms) "
                                    "values ((select ifnull(max(seq), 0) + 1 from queue_history), '{}', '/', '[]', 0, 0, 0);",
                                    outcome));
  };
  for (auto const outcome : {"exited", "signaled", "timeout", "cancelled", "wait_timeout", "not_started", "abandoned"}) {
    INFO("outcome " << outcome);
    CHECK(insert_history(outcome).has_value());
  }
  auto const bad_outcome = insert_history("vanished");
  REQUIRE_FALSE(bad_outcome.has_value());
  CHECK((bad_outcome.error().code_ & 0xff) == 19);
  CHECK(scalar(conn, "select count(*) from queue_history") == "7");
  CHECK(scalar(conn, "select count(*) from queue_history where nested = 0") == "7");

  // The AUTOINCREMENT counter starts at the floor: the first entry the engine
  // enqueues continues from 1,000,001, above every retired agent.db number.
  scratch_dir floor_scratch;
  auto        floor_conn = open_scratch_store(floor_scratch);
  CHECK(hq::enqueue(floor_conn, full_request()).value() == 1'000'001);
}

TEST_CASE("main migration 00040 rolls back and re-applies without losing schema",
          "[engine][hostqueue][migrations][hq-enqueue][qp-separability]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto const chain = planar::db::migrations();
  auto const found = std::ranges::find_if(chain, [](const planar::db::migration_record& m) { return m.version_ == 40; });
  REQUIRE(found != chain.end());

  auto const before = schema_dump(conn);
  // A down runs against a database at its own version, so every migration
  // above 00040 is rolled back first, newest first, then 00040 itself.
  for (auto i = chain.size(); i > 0 && chain[i - 1].version_ >= 40; --i) {
    REQUIRE(conn.execute(chain[i - 1].down_sql_).has_value());
  }
  CHECK_FALSE(has_table(conn, "queue_entries"));
  CHECK_FALSE(has_table(conn, "queue_history"));
  CHECK_FALSE(has_table(conn, "queue_schema"));
  CHECK(has_table(conn, "schema_migrations"));
  CHECK(planar::db::current_version(conn).value() == 39);

  REQUIRE(planar::db::apply_contiguous(conn, chain).has_value());
  CHECK(schema_dump(conn) == before);
  CHECK(planar::db::current_version(conn).value() == planar::db::embedded_max(chain));

  // Usable again through the engine after the roundtrip.
  auto const seq = hq::enqueue(conn, full_request());
  REQUIRE(seq.has_value());
  CHECK(hq::find(conn, *seq).value().has_value());
}

TEST_CASE("a planar.db ahead of the binary with the queue marker unchanged accepts an enqueue and an end",
          "[engine][hostqueue][hq-enqueue][qp-separability]") {
  // A newer binary added a migration and left the queue marker's compat
  // alone, so this binary may enqueue and remove an entry in the shared file.
  // The queue compatibility check itself is schema.t.cpp's; this proves the
  // engine's reads and writes do not depend on `schema_migrations` at all.
  scratch_dir scratch;
  auto const  store       = scratch.path_ / "planar.db";
  auto const  binary_head = planar::db::embedded_max();
  {
    auto seeded = open_main_store_at(store);
    REQUIRE(seeded.has_value());
    REQUIRE(seeded
                ->execute(std::format("insert into schema_migrations (version, description) "
                                      "values ({}, 'seeded ahead by the test');"
                                      "insert into queue_schema (version, compat, description) "
                                      "values (2, 1, 'seeded ahead by the test')",
                                      binary_head + 1))
                .has_value());
  }

  auto opened = planar::db::connection::open(store.string());
  REQUIRE(opened.has_value());
  auto& conn = *opened;
  REQUIRE(planar::db::current_version(conn).value() == binary_head + 1);

  auto const seq = hq::enqueue(conn, full_request());
  REQUIRE(seq.has_value());
  auto const found = hq::find(conn, *seq);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  CHECK((*found)->argv == full_request().argv);

  auto const ended = hq::end_entry(conn, *seq, {.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = 2'000});
  REQUIRE(ended.has_value());
  CHECK(*ended == hq::end_result::ended);
  CHECK(hq::list(conn).value().empty());
  auto const row = hq::find_history(conn, *seq);
  REQUIRE(row.has_value());
  REQUIRE(row->has_value());
  CHECK((*row)->outcome == hq::history_outcome::exited);
  // The newer database's own version row is untouched.
  CHECK(planar::db::current_version(conn).value() == binary_head + 1);
}

TEST_CASE("set_log_path names the output file of an entry and refuses a missing one", "[engine][hostqueue][hq-detach]") {
  scratch_dir scratch;
  auto        conn  = open_scratch_store(scratch);
  auto const  seq   = hq::enqueue(conn, full_request()).value();
  auto const  other = hq::enqueue(conn, full_request()).value();

  auto const set = hq::set_log_path(conn, seq, "/logs/1.log");
  REQUIRE(set.has_value());
  CHECK(*set);
  CHECK(hq::find(conn, seq).value()->log_path == "/logs/1.log");
  CHECK(hq::find(conn, other).value()->log_path == full_request().log_path); // Only the named entry moved.

  auto const missing = hq::set_log_path(conn, 9'999, "/logs/9999.log");
  REQUIRE(missing.has_value());
  CHECK_FALSE(*missing);
}

TEST_CASE("discard_entry removes an entry and writes no history row", "[engine][hostqueue][hq-detach]") {
  scratch_dir scratch;
  auto        conn  = open_scratch_store(scratch);
  auto const  seq   = hq::enqueue(conn, full_request()).value();
  auto const  other = hq::enqueue(conn, full_request()).value();

  auto const gone = hq::discard_entry(conn, seq);
  REQUIRE(gone.has_value());
  CHECK(*gone);
  CHECK_FALSE(hq::find(conn, seq).value().has_value());
  CHECK(hq::find(conn, other).value().has_value());
  CHECK(hq::list_history(conn).value().empty());

  auto const again = hq::discard_entry(conn, seq);
  REQUIRE(again.has_value());
  CHECK_FALSE(*again);
}

TEST_CASE("enqueue records the wait limit it is given, and none when there is none",
          "[engine][hostqueue][hq-enqueue][hq-queue-limit-columns]") {
  // Task hq-queue-limit-columns: `queue status` reports `wait_limit_ms`, so
  // enqueue stores the limit next to the deadline computed from it. The run
  // limit is recorded at the start, so a fresh entry has none.
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto limited               = full_request();
  limited.wait_deadline_mono = limited.refreshed_mono + 600'000;
  limited.wait_limit_ms      = 600'000;
  auto const with_limit      = hq::enqueue(conn, limited);
  REQUIRE(with_limit.has_value());

  auto unlimited               = full_request();
  unlimited.wait_deadline_mono = std::nullopt;
  unlimited.wait_limit_ms      = std::nullopt;
  auto const without           = hq::enqueue(conn, unlimited);
  REQUIRE(without.has_value());

  auto const first = hq::find(conn, *with_limit);
  REQUIRE((first.has_value() && first->has_value()));
  CHECK((*first)->wait_limit_ms == 600'000);
  CHECK((*first)->wait_deadline_mono == limited.refreshed_mono + 600'000);
  CHECK_FALSE((*first)->run_limit_ms.has_value());

  auto const second = hq::find(conn, *without);
  REQUIRE((second.has_value() && second->has_value()));
  CHECK_FALSE((*second)->wait_limit_ms.has_value());
  CHECK_FALSE((*second)->run_limit_ms.has_value());

  // `list` reads the same columns as `find`.
  auto const all = hq::list(conn);
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 2);
  CHECK(all->front().wait_limit_ms == 600'000);
  CHECK_FALSE(all->back().wait_limit_ms.has_value());
}
