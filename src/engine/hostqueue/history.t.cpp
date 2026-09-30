// @file history.t.cpp
// @brief Unit tests for `planar.engine.hostqueue.history` (plan 1080, task
// hq-history). Covers the test-spec scenarios "Happy path -- every ended
// entry has exactly one history row", "Edge -- two processes ending the same
// entry write one row", "Empty -- history starts empty", "Edge -- old history
// and its log files are pruned at enqueue", and the history half of "Happy
// path -- a nested entry is recorded with its parent".
//
// Every case opens its own scratch store through `open_agent_db_at` in its
// own temp directory. Following the test spec's § Strategy, the scenarios
// seed and read the store only through the engine (`enqueue`, `poll`,
// `begin_terminate`, `end_entry`, `find_history`, `list_history`). The
// case covering `started_at` and an entry-recorded canceller takes them from
// the engine's own writers too: the poll transaction sets `started_at`, and a
// cancellation through `begin_terminate` records the canceller on the entry
// (task 7047 replaced the raw UPDATEs that stood in for both).
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.agentdb;
import planar.engine.hostqueue;
import planar.process.identity;

namespace {

namespace hq = planar::engine::hostqueue;

/// @brief A unique scratch directory under the system temp directory,
/// removed (recursively, best-effort) when the guard goes out of scope.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_hostqueue_history_test_{}_{}",
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

/// @brief A scratch agent store at the head of the embedded agent chain.
auto open_scratch_store(const scratch_dir& scratch) -> planar::db::connection {
  auto opened = planar::db::agent::open_agent_db_at(scratch.path_ / "agent.db");
  REQUIRE(opened.has_value());
  return std::move(*opened);
}

constexpr std::int64_t k_enqueued_at = 1'759'000'000'000;

/// @brief A waiting-entry request with every optional column set.
auto request_for(std::string label) -> hq::enqueue_request {
  return hq::enqueue_request{
      .host_id        = "boot-7f3a",
      .pid            = 4242,
      .pid_started    = 1'700'000'123,
      .cwd            = "/work/planar",
      .argv           = {"make", "test", "a b"},
      .label          = std::move(label),
      .vendor         = "claude",
      .role           = "coder",
      .claim_token    = "53f83462ecb9f6c6c90738a108db92fb",
      .log_path       = "/home/u/.planar/queue-logs/x.log",
      .enqueued_at    = k_enqueued_at,
      .refreshed_mono = 90'000,
  };
}

/// @brief Enqueues `request` and returns its sequence number.
auto enqueue_one(planar::db::connection& conn, const hq::enqueue_request& request) -> std::int64_t {
  auto seq = hq::enqueue(conn, request);
  REQUIRE(seq.has_value());
  return *seq;
}

/// @brief Ends `seq` and requires that this call ended it.
auto end_one(planar::db::connection& conn, std::int64_t seq, const hq::end_request& request) -> void {
  auto ended = hq::end_entry(conn, seq, request);
  if (!ended) {
    FAIL(ended.error().message);
  }
  REQUIRE(*ended == hq::end_result::ended);
}

/// @brief The history row of `seq`, which must exist.
auto history_of(planar::db::connection& conn, std::int64_t seq) -> hq::history_row {
  auto found = hq::find_history(conn, seq);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  return **found;
}

/// @brief A clock the test sets by hand; both readings are returned as set.
class hand_clock final : public planar::process::identity::clock {
public:
  std::int64_t mono = 1'000'000; ///< The monotonic reading, ms.
  std::int64_t wall = 0;         ///< The wall reading, ms since the epoch.

  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, planar::process::identity::error> override {
    return mono;
  }
  [[nodiscard]] auto wall_ms() -> std::int64_t override {
    return wall;
  }
};

/// @brief A probe that finds no process anywhere. The entries in these cases
/// are polled by their own submitter, which is never probed.
auto empty_probe() -> hq::process_probe {
  return hq::process_probe{
      .process_exists     = [](std::int64_t) -> std::expected<bool, planar::process::identity::error> { return false; },
      .process_start_time = [](std::int64_t)
          -> std::expected<std::optional<planar::process::identity::start_time>, planar::process::identity::error> {
        return std::optional<planar::process::identity::start_time>{};
      },
      .group_has_members = [](std::int64_t) -> std::expected<bool, planar::process::identity::error> { return false; },
  };
}

/// @brief Gives entry `seq` its turn, through the poll transaction, with the
/// wall clock at `at`; requires that the poll started it.
auto start_by_poll(planar::db::connection& conn, std::int64_t seq, hand_clock& clock, std::int64_t at) -> void {
  clock.wall  = at;
  auto polled = hq::poll(
      conn, hq::poll_request{.seq = seq, .host_id = "boot-7f3a", .slots = 1, .stale_after_ms = 60'000, .run_limit_ms = 60'000},
      clock, empty_probe());
  REQUIRE(polled.has_value());
  REQUIRE(polled->running);
}

/// @brief Writes `text` to `path`.
auto write_file(const std::filesystem::path& path, std::string_view text) -> void {
  std::ofstream out(path);
  out << text;
  REQUIRE(out.good());
}

} // namespace

TEST_CASE("every ended entry has exactly one history row with its outcome, and no entry remains",
          "[engine][hostqueue][hq-history]") {
  // Test-spec "Happy path -- every ended entry has exactly one history row".
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  hq::canceller const canceller{.vendor = "codex", .role = "orchestrator", .pid = 777};
  struct ending {
    std::string_view label;
    hq::end_request  request;
  };
  std::vector<ending> const endings{
      {"exited", {.outcome = hq::history_outcome::exited, .exit_code = 3, .ended_at = k_enqueued_at + 1'000}},
      {"signaled", {.outcome = hq::history_outcome::signaled, .signal = 9, .ended_at = k_enqueued_at + 2'000}},
      {"timeout", {.outcome = hq::history_outcome::timeout, .ended_at = k_enqueued_at + 3'000}},
      {"cancelled", {.outcome = hq::history_outcome::cancelled, .cancelled_by = canceller, .ended_at = k_enqueued_at + 4'000}},
      {"wait_timeout", {.outcome = hq::history_outcome::wait_timeout, .ended_at = k_enqueued_at + 5'000}},
      {"not_started", {.outcome = hq::history_outcome::not_started, .exit_code = 127, .ended_at = k_enqueued_at + 6'000}},
      {"abandoned", {.outcome = hq::history_outcome::abandoned, .ended_at = k_enqueued_at + 7'000}},
  };

  std::vector<std::int64_t> seqs;
  for (auto const& e : endings) {
    seqs.push_back(enqueue_one(conn, request_for(std::string{e.label})));
  }
  for (std::size_t i = 0; i < endings.size(); ++i) {
    end_one(conn, seqs[i], endings[i].request);
  }

  auto const remaining = hq::list(conn);
  REQUIRE(remaining.has_value());
  CHECK(remaining->empty());

  auto const rows = hq::list_history(conn);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == endings.size());

  for (std::size_t i = 0; i < endings.size(); ++i) {
    auto const& want = endings[i].request;
    auto const  row  = history_of(conn, seqs[i]);
    INFO("outcome " << endings[i].label);
    CHECK(row.seq == seqs[i]);
    CHECK(row.outcome == want.outcome);
    CHECK(hq::to_string(row.outcome) == endings[i].label);
    CHECK(row.exit_code == want.exit_code);
    CHECK(row.signal == want.signal);
    CHECK(row.cancelled_by == want.cancelled_by);
    CHECK_FALSE(row.successor_seq.has_value());
    CHECK_FALSE(row.nested);
    CHECK_FALSE(row.parent_seq.has_value());
    // The command columns are copied from the entry.
    CHECK(row.cwd == "/work/planar");
    CHECK(row.argv == std::vector<std::string>{"make", "test", "a b"});
    CHECK(row.label == std::string{endings[i].label});
    CHECK(row.vendor == "claude");
    CHECK(row.role == "coder");
    CHECK(row.log_path == "/home/u/.planar/queue-logs/x.log");
    CHECK(row.enqueued_at == k_enqueued_at);
    CHECK(row.ended_at == want.ended_at);
    // None of these entries started: the whole life was waiting.
    CHECK_FALSE(row.started_at.has_value());
    CHECK(row.waited_ms == want.ended_at - k_enqueued_at);
    CHECK_FALSE(row.ran_ms.has_value());
    // The list is ordered by ended_at, which rises with i here.
    CHECK((*rows)[i].seq == seqs[i]);
  }

  // Ending an entry a second time finds it gone and writes nothing more.
  auto const again = hq::end_entry(conn, seqs[0], endings[1].request);
  REQUIRE(again.has_value());
  CHECK(*again == hq::end_result::already_gone);
  CHECK(history_of(conn, seqs[0]).outcome == hq::history_outcome::exited);
  CHECK(hq::list_history(conn).value().size() == endings.size());

  // A number that was never issued is gone too, and writes no row.
  auto const never = hq::end_entry(conn, seqs.back() + 100, endings[2].request);
  REQUIRE(never.has_value());
  CHECK(*never == hq::end_result::already_gone);
  CHECK_FALSE(hq::find_history(conn, seqs.back() + 100).value().has_value());
  CHECK(hq::list_history(conn).value().size() == endings.size());
}

TEST_CASE("two connections ending the same entry at once write exactly one history row", "[engine][hostqueue][hq-history]") {
  // Test-spec "Edge -- two processes ending the same entry write one row".
  // Two connections to one store, each driven by its own thread, race to end
  // the same entry; each round starts both at the same latch.
  scratch_dir scratch;
  auto        setup = open_scratch_store(scratch);
  auto        first = planar::db::agent::open_agent_db_at(scratch.path_ / "agent.db");
  auto        other = planar::db::agent::open_agent_db_at(scratch.path_ / "agent.db");
  REQUIRE(first.has_value());
  REQUIRE(other.has_value());

  constexpr int k_rounds = 200;
  int           first_won{0};
  int           other_won{0};
  for (int round = 0; round < k_rounds; ++round) {
    auto const seq = enqueue_one(setup, request_for("race"));

    std::latch                                     start{2};
    std::expected<hq::end_result, hq::queue_error> first_result{std::unexpect};
    std::expected<hq::end_result, hq::queue_error> other_result{std::unexpect};
    hq::end_request const exited{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = k_enqueued_at + 10};
    hq::end_request const cancelled{.outcome      = hq::history_outcome::cancelled,
                                    .cancelled_by = hq::canceller{.vendor = "claude", .role = "coder", .pid = 1},
                                    .ended_at     = k_enqueued_at + 20};
    {
      std::jthread a([&] {
        start.arrive_and_wait();
        first_result = hq::end_entry(*first, seq, exited);
      });
      std::jthread b([&] {
        start.arrive_and_wait();
        other_result = hq::end_entry(*other, seq, cancelled);
      });
    }

    INFO("round " << round);
    if (!first_result) {
      FAIL(first_result.error().message);
    }
    if (!other_result) {
      FAIL(other_result.error().message);
    }
    // Exactly one of the two ended it; the other reports it already gone.
    auto const first_ended = *first_result == hq::end_result::ended;
    auto const other_ended = *other_result == hq::end_result::ended;
    REQUIRE(first_ended != other_ended);
    CHECK(*(first_ended ? other_result : first_result) == hq::end_result::already_gone);
    first_won += first_ended ? 1 : 0;
    other_won += other_ended ? 1 : 0;

    // One row, written by the winner.
    auto const row = history_of(setup, seq);
    CHECK(row.outcome == (first_ended ? hq::history_outcome::exited : hq::history_outcome::cancelled));
    CHECK_FALSE(hq::find(setup, seq).value().has_value());
  }
  CHECK(first_won + other_won == k_rounds);
  CHECK(hq::list_history(setup).value().size() == static_cast<std::size_t>(k_rounds));
  CHECK(hq::list(setup).value().empty());
}

TEST_CASE("a history insert that fails leaves the entry in place and writes no history row", "[engine][hostqueue][hq-history]") {
  // Ending an entry deletes it and writes its history row in one transaction:
  // when the insert fails, the delete must roll back with it.
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto const seq = enqueue_one(conn, request_for("doomed"));
  // Fault injection inside a unit test, not scenario seeding, so the
  // test spec's "never raw SQL" rule for scenarios does not apply.
  REQUIRE(conn.execute("create temp trigger fail_history_insert before insert on queue_history "
                       "begin select raise(abort, 'injected'); end;")
              .has_value());

  auto const ended =
      hq::end_entry(conn, seq, {.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = k_enqueued_at + 10});
  REQUIRE_FALSE(ended.has_value());
  CHECK(ended.error().kind == hq::queue_error_kind::query_failed);

  CHECK(hq::find(conn, seq).value().has_value());
  CHECK_FALSE(hq::find_history(conn, seq).value().has_value());
  CHECK_FALSE(conn.in_transaction());
}

TEST_CASE("a new store has no history rows", "[engine][hostqueue][hq-history]") {
  // Test-spec "Empty -- history starts empty".
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto const rows = hq::list_history(conn);
  REQUIRE(rows.has_value());
  CHECK(rows->empty());
  auto const since = hq::list_history(conn, 0);
  REQUIRE(since.has_value());
  CHECK(since->empty());
  auto const one = hq::find_history(conn, 1);
  REQUIRE(one.has_value());
  CHECK_FALSE(one->has_value());
}

TEST_CASE("an ended nested entry is recorded as nested with its parent's sequence number", "[engine][hostqueue][hq-history]") {
  // History half of test-spec "Happy path -- a nested entry is recorded with
  // its parent".
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto const parent            = enqueue_one(conn, request_for("parent"));
  auto       nested_req        = request_for("nested");
  nested_req.parent_seq        = parent;
  auto const            nested = hq::insert_nested_entry(conn, nested_req).value();
  hq::end_request const ok     = {.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = k_enqueued_at + 50};

  end_one(conn, nested, ok);
  auto const nested_row = history_of(conn, nested);
  CHECK(nested_row.nested);
  CHECK(nested_row.parent_seq == parent);

  end_one(conn, parent, ok);
  auto const parent_row = history_of(conn, parent);
  CHECK_FALSE(parent_row.nested);
  CHECK_FALSE(parent_row.parent_seq.has_value());
}

TEST_CASE("old history rows and their log files are pruned at enqueue; newer ones remain", "[engine][hostqueue][hq-history]") {
  // Test-spec "Edge -- old history and its log files are pruned at enqueue",
  // with a retention of one day and "now" supplied as the new entry's
  // enqueued_at.
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  constexpr std::int64_t k_now = k_enqueued_at + 40 * hq::k_ms_per_day;
  auto const             log   = [&](std::string_view name) { return scratch.path_ / name; };

  struct seeded {
    std::string                 name;
    std::int64_t                ended_at;
    bool                        write_log;
    std::optional<std::int64_t> seq;
  };
  std::vector<seeded> rows{
      {"older.log", k_now - hq::k_ms_per_day - 1, true, {}},         // one ms past the retention: pruned
      {"much-older.log", k_now - 30 * hq::k_ms_per_day, true, {}},   // far past it: pruned
      {"missing.log", k_now - 2 * hq::k_ms_per_day, false, {}},      // pruned; its log is already gone
      {"boundary.log", k_now - hq::k_ms_per_day, true, {}},          // exactly one day old: kept
      {"newer.log", k_now - hq::k_ms_per_day + 3'600'000, true, {}}, // 23 hours old: kept
  };
  for (auto& row : rows) {
    auto request     = request_for(row.name);
    request.log_path = log(row.name).string();
    if (row.write_log) {
      write_file(log(row.name), "output\n");
    }
    row.seq = enqueue_one(conn, request);
    end_one(conn, *row.seq, {.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = row.ended_at});
  }
  // A row with no log file at all is pruned too.
  auto no_log_req     = request_for("no-log");
  no_log_req.log_path = std::nullopt;
  auto const no_log   = enqueue_one(conn, no_log_req);
  end_one(conn, no_log, {.outcome = hq::history_outcome::timeout, .ended_at = k_now - 3 * hq::k_ms_per_day});
  REQUIRE(hq::list_history(conn).value().size() == rows.size() + 1);

  auto next         = request_for("next");
  next.enqueued_at  = k_now;
  auto const result = hq::enqueue(conn, next, 1);
  if (!result) {
    FAIL(result.error().message);
  }
  CHECK(result->pruned.rows_deleted == 4);
  CHECK(result->pruned.log_failures.empty());

  // The new entry was enqueued.
  auto const entry = hq::find(conn, result->seq);
  REQUIRE(entry.has_value());
  REQUIRE(entry->has_value());
  CHECK((*entry)->label == "next");

  for (auto const& row : rows) {
    auto const kept = row.ended_at >= k_now - hq::k_ms_per_day;
    INFO("row " << row.name);
    CHECK(hq::find_history(conn, *row.seq).value().has_value() == kept);
    CHECK(std::filesystem::exists(log(row.name)) == (kept && row.write_log));
  }
  CHECK_FALSE(hq::find_history(conn, no_log).value().has_value());
  CHECK(hq::list_history(conn).value().size() == 2);

  // A second enqueue at the same time prunes nothing more.
  auto const again = hq::enqueue(conn, next, 1);
  REQUIRE(again.has_value());
  CHECK(again->pruned.rows_deleted == 0);
  CHECK(again->seq > result->seq);
}

TEST_CASE("a log file that cannot be removed is reported and the entry is still enqueued", "[engine][hostqueue][hq-history]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  // A non-empty directory where the log file should be: removing it fails.
  auto const stuck = scratch.path_ / "stuck.log";
  std::filesystem::create_directories(stuck);
  write_file(stuck / "inner", "x");
  auto const gone = scratch.path_ / "gone.log";
  write_file(gone, "x");

  constexpr std::int64_t k_now = k_enqueued_at + 10 * hq::k_ms_per_day;
  for (auto const& path : {stuck, gone}) {
    auto request     = request_for("old");
    request.log_path = path.string();
    auto const seq   = enqueue_one(conn, request);
    end_one(conn, seq, {.outcome = hq::history_outcome::abandoned, .ended_at = k_now - 5 * hq::k_ms_per_day});
  }

  auto next         = request_for("next");
  next.enqueued_at  = k_now;
  auto const result = hq::enqueue(conn, next, 1);
  if (!result) {
    FAIL(result.error().message);
  }
  CHECK(result->pruned.rows_deleted == 2);
  REQUIRE(result->pruned.log_failures.size() == 1);
  CHECK(result->pruned.log_failures[0].path == stuck.string());
  CHECK_FALSE(result->pruned.log_failures[0].message.empty());
  CHECK_FALSE(std::filesystem::exists(gone));
  CHECK(std::filesystem::exists(stuck));
  CHECK(hq::find(conn, result->seq).value().has_value());
  CHECK(hq::list_history(conn).value().empty());
}

TEST_CASE("an enqueue whose insert fails prunes no history rows and removes no log files", "[engine][hostqueue][hq-history]") {
  // queue.cppm: after a SQLite failure neither the prune nor the insert has
  // happened, so the expired rows and their logs must survive.
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  constexpr std::int64_t             k_now = k_enqueued_at + 10 * hq::k_ms_per_day;
  std::vector<std::int64_t>          seqs;
  std::vector<std::filesystem::path> logs;
  for (std::string_view name : {"expired-a.log", "expired-b.log"}) {
    auto const path = scratch.path_ / name;
    write_file(path, "output\n");
    auto request     = request_for(std::string{name});
    request.log_path = path.string();
    auto const seq   = enqueue_one(conn, request);
    end_one(conn, seq, {.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = k_now - 5 * hq::k_ms_per_day});
    seqs.push_back(seq);
    logs.push_back(path);
  }
  REQUIRE(hq::list(conn).value().empty());

  // Fault injection inside a unit test, not scenario seeding, so the
  // test spec's "never raw SQL" rule for scenarios does not apply.
  REQUIRE(conn.execute("create temp trigger fail_entry_insert before insert on queue_entries "
                       "begin select raise(abort, 'injected'); end;")
              .has_value());

  auto next         = request_for("next");
  next.enqueued_at  = k_now;
  auto const result = hq::enqueue(conn, next, 1);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().kind == hq::queue_error_kind::query_failed);
  CHECK_FALSE(conn.in_transaction());

  CHECK(hq::list(conn).value().empty());
  CHECK(hq::list_history(conn).value().size() == seqs.size());
  for (std::size_t i = 0; i < seqs.size(); ++i) {
    INFO("row " << logs[i].filename().string());
    CHECK(hq::find_history(conn, seqs[i]).value().has_value());
    CHECK(std::filesystem::exists(logs[i]));
  }
}

TEST_CASE("a negative retention is refused and nothing is enqueued or pruned", "[engine][hostqueue][hq-history]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto const seq = enqueue_one(conn, request_for("old"));
  end_one(conn, seq, {.outcome = hq::history_outcome::wait_timeout, .ended_at = k_enqueued_at});

  auto next         = request_for("next");
  next.enqueued_at  = k_enqueued_at + 100 * hq::k_ms_per_day;
  auto const result = hq::enqueue(conn, next, -1);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().kind == hq::queue_error_kind::invalid_request);
  CHECK(hq::list(conn).value().empty());
  CHECK(hq::find_history(conn, seq).value().has_value());

  // Zero keeps nothing older than now.
  auto const zero = hq::enqueue(conn, next, 0);
  REQUIRE(zero.has_value());
  CHECK(zero->pruned.rows_deleted == 1);
}

TEST_CASE("the canceller is stored as a vendor, role and pid object and read back unchanged", "[engine][hostqueue][hq-history]") {
  hq::canceller const full{.vendor = "claude", .role = "orchestrator", .pid = 31337};
  hq::canceller const bare{.pid = 12};
  CHECK(hq::encode_canceller(full) == R"({"vendor":"claude","role":"orchestrator","pid":31337})");
  CHECK(hq::encode_canceller(bare) == R"({"vendor":null,"role":null,"pid":12})");
  CHECK(hq::decode_canceller(hq::encode_canceller(full)).value() == full);
  CHECK(hq::decode_canceller(hq::encode_canceller(bare)).value() == bare);
  hq::canceller const quoted{.vendor = "we\"ird", .role = "r\nole", .pid = 1};
  CHECK(hq::decode_canceller(hq::encode_canceller(quoted)).value() == quoted);

  for (auto const bad :
       {R"(not json)", R"([1])", R"({"vendor":"v","role":"r"})", R"({"pid":"12"})", R"({"vendor":3,"role":null,"pid":1})"}) {
    INFO("text " << bad);
    auto const decoded = hq::decode_canceller(bad);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().kind == hq::queue_error_kind::malformed_canceller);
  }

  // Through the store: the cancelled row carries the same three values.
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);
  auto const  seq  = enqueue_one(conn, request_for("cancel me"));
  end_one(conn, seq, {.outcome = hq::history_outcome::cancelled, .cancelled_by = full, .ended_at = k_enqueued_at + 5});
  CHECK(history_of(conn, seq).cancelled_by == full);
}

TEST_CASE("a successor is recorded on an existing history row, and a missing row is reported",
          "[engine][hostqueue][hq-history]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto const reaped = enqueue_one(conn, request_for("reaped"));
  end_one(conn, reaped, {.outcome = hq::history_outcome::abandoned, .ended_at = k_enqueued_at + 5});
  auto const successor = enqueue_one(conn, request_for("again"));

  auto const recorded = hq::record_successor(conn, reaped, successor);
  REQUIRE(recorded.has_value());
  CHECK(*recorded == hq::successor_result::recorded);
  CHECK(history_of(conn, reaped).successor_seq == successor);

  auto const missing = hq::record_successor(conn, successor + 50, successor);
  REQUIRE(missing.has_value());
  CHECK(*missing == hq::successor_result::no_such_entry);
  CHECK_FALSE(hq::find_history(conn, successor + 50).value().has_value());

  // A successor known at the end is stored with the row.
  auto const with = enqueue_one(conn, request_for("with successor"));
  end_one(conn, with, {.outcome = hq::history_outcome::abandoned, .ended_at = k_enqueued_at + 6, .successor_seq = successor});
  CHECK(history_of(conn, with).successor_seq == successor);
}

TEST_CASE("a request whose fields do not fit its outcome is refused and the entry stays", "[engine][hostqueue][hq-history]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);
  auto const  seq  = enqueue_one(conn, request_for("stays"));

  std::vector<std::pair<std::string_view, hq::end_request>> const bad{
      {"exited without code", {.outcome = hq::history_outcome::exited}},
      {"exited with signal", {.outcome = hq::history_outcome::exited, .exit_code = 0, .signal = 9}},
      {"signaled without signal", {.outcome = hq::history_outcome::signaled}},
      {"signaled with code", {.outcome = hq::history_outcome::signaled, .exit_code = 1, .signal = 9}},
      {"not_started with 1", {.outcome = hq::history_outcome::not_started, .exit_code = 1}},
      {"not_started without code", {.outcome = hq::history_outcome::not_started}},
      {"timeout with code", {.outcome = hq::history_outcome::timeout, .exit_code = 0}},
      {"abandoned with canceller", {.outcome = hq::history_outcome::abandoned, .cancelled_by = hq::canceller{.pid = 1}}},
      // No canceller in the request and none recorded on the entry: refused
      // after the delete, which must be rolled back.
      {"cancelled without canceller", {.outcome = hq::history_outcome::cancelled}},
  };
  for (auto const& [name, request] : bad) {
    INFO(name);
    auto const result = hq::end_entry(conn, seq, request);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == hq::queue_error_kind::invalid_request);
    CHECK(hq::find(conn, seq).value().has_value());
    CHECK_FALSE(hq::find_history(conn, seq).value().has_value());
    CHECK_FALSE(conn.in_transaction());
  }
  end_one(conn, seq, {.outcome = hq::history_outcome::not_started, .exit_code = 126, .ended_at = k_enqueued_at});
  CHECK(history_of(conn, seq).exit_code == 126);
}

TEST_CASE("waited and ran times come from the entry's start, and a cancel recorded on the entry names the canceller",
          "[engine][hostqueue][hq-history]") {
  // The start is taken by the poll transaction and the canceller by a
  // cancellation through `begin_terminate`, the engine's own writers of those
  // columns; everything the case asserts is read through the engine.
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);
  hand_clock  clock;

  auto const started = enqueue_one(conn, request_for("started"));
  start_by_poll(conn, started, clock, k_enqueued_at + 4'000);
  end_one(conn, started, {.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = k_enqueued_at + 10'000});
  auto const row = history_of(conn, started);
  CHECK(row.started_at == k_enqueued_at + 4'000);
  CHECK(row.waited_ms == 4'000);
  CHECK(row.ran_ms == 6'000);

  // A wall clock that stepped backwards gives zero, never a negative time.
  auto const stepped = enqueue_one(conn, request_for("stepped"));
  start_by_poll(conn, stepped, clock, k_enqueued_at + 4'000);
  end_one(conn, stepped, {.outcome = hq::history_outcome::signaled, .signal = 15, .ended_at = k_enqueued_at - 1});
  auto const back = history_of(conn, stepped);
  CHECK(back.waited_ms == 4'000);
  CHECK(back.ran_ms == 0);

  // The canceller is recorded on the running entry by the cancellation, and
  // the entry's own end (no canceller in the request) carries it to the row.
  hq::canceller const who{.vendor = "gemini", .role = "reviewer", .pid = 99};
  auto const          cancelled = enqueue_one(conn, request_for("cancelled"));
  start_by_poll(conn, cancelled, clock, k_enqueued_at + 1);
  std::vector<int> signals;
  auto const       begun = hq::begin_terminate(
      conn,
      hq::begin_terminate_request{
          .seq = cancelled, .reason = hq::stop_reason::cancelled, .cancelled_by = who, .host_id = "boot-7f3a"},
      clock, empty_probe(), [&signals](std::int64_t, int sig) -> std::expected<void, planar::process::identity::error> {
        signals.push_back(sig);
        return {};
      });
  REQUIRE(begun.has_value());
  REQUIRE(begun->status == hq::begin_status::marked);
  CHECK(signals.empty()); // no child group is recorded, so nothing is signalled
  REQUIRE(begun->stored.has_value());
  CHECK(begun->stored->cancelled_by == hq::encode_canceller(who));
  end_one(conn, cancelled, {.outcome = hq::history_outcome::cancelled, .ended_at = k_enqueued_at + 1});
  CHECK(history_of(conn, cancelled).cancelled_by == who);
}

TEST_CASE("history lists by end time with an inclusive lower bound", "[engine][hostqueue][hq-history]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  // Ended out of sequence order: the list follows ended_at.
  auto const a = enqueue_one(conn, request_for("a"));
  auto const b = enqueue_one(conn, request_for("b"));
  auto const c = enqueue_one(conn, request_for("c"));
  end_one(conn, a, {.outcome = hq::history_outcome::timeout, .ended_at = 300});
  end_one(conn, b, {.outcome = hq::history_outcome::timeout, .ended_at = 100});
  end_one(conn, c, {.outcome = hq::history_outcome::timeout, .ended_at = 200});

  auto const all = hq::list_history(conn).value();
  REQUIRE(all.size() == 3);
  CHECK(all[0].seq == b);
  CHECK(all[1].seq == c);
  CHECK(all[2].seq == a);

  auto const since = hq::list_history(conn, 200).value();
  REQUIRE(since.size() == 2);
  CHECK(since[0].seq == c);
  CHECK(since[1].seq == a);
  CHECK(hq::list_history(conn, 301).value().empty());
}

TEST_CASE("every outcome's stored text parses back to the same outcome", "[engine][hostqueue][hq-history]") {
  for (auto const outcome :
       {hq::history_outcome::exited, hq::history_outcome::signaled, hq::history_outcome::timeout, hq::history_outcome::cancelled,
        hq::history_outcome::wait_timeout, hq::history_outcome::not_started, hq::history_outcome::abandoned}) {
    CHECK(hq::parse_history_outcome(hq::to_string(outcome)) == outcome);
  }
  CHECK_FALSE(hq::parse_history_outcome("vanished").has_value());
}

TEST_CASE("an enqueue whose commit fails keeps the expired rows and their log files", "[engine][hostqueue][hq-history]") {
  // queue.cppm: the log files of pruned rows are removed only after the
  // transaction commits. The insert and the delete both succeed here; the
  // COMMIT itself is refused, so the delete is rolled back and the files must
  // still exist. Moving `remove_log_files` before the commit passes every other
  // case in this file and fails this one.
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  constexpr std::int64_t             k_now = k_enqueued_at + 10 * hq::k_ms_per_day;
  std::vector<std::int64_t>          seqs;
  std::vector<std::filesystem::path> logs;
  for (std::string_view name : {"expired-a.log", "expired-b.log"}) {
    auto const path = scratch.path_ / name;
    write_file(path, "output\n");
    auto request     = request_for(std::string{name});
    request.log_path = path.string();
    auto const seq   = enqueue_one(conn, request);
    end_one(conn, seq, {.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = k_now - 5 * hq::k_ms_per_day});
    seqs.push_back(seq);
    logs.push_back(path);
  }

  // Fault injection inside a unit test, not scenario seeding. A deferred
  // foreign key on a temp table is checked at COMMIT, not at the statement:
  // the trigger inserts a child row whose parent does not exist, so every
  // statement of the enqueue succeeds and the commit is what fails.
  REQUIRE(conn.execute("create temp table fk_parent (id integer primary key)").has_value());
  REQUIRE(conn.execute("create temp table fk_child (parent_id integer references fk_parent (id) deferrable initially deferred)")
              .has_value());
  REQUIRE(conn.execute("create temp trigger fail_at_commit after insert on queue_entries "
                       "begin insert into fk_child (parent_id) values (999); end;")
              .has_value());

  auto next         = request_for("next");
  next.enqueued_at  = k_now;
  auto const result = hq::enqueue(conn, next, 1);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().kind == hq::queue_error_kind::query_failed);
  CHECK(result.error().message.find("commit enqueue") != std::string::npos);
  CHECK_FALSE(conn.in_transaction());

  // Nothing was enqueued, and nothing was pruned: rows and files both remain.
  CHECK(hq::list(conn).value().empty());
  CHECK(hq::list_history(conn).value().size() == seqs.size());
  for (std::size_t i = 0; i < seqs.size(); ++i) {
    INFO("row " << logs[i].filename().string());
    CHECK(hq::find_history(conn, seqs[i]).value().has_value());
    CHECK(std::filesystem::exists(logs[i]));
  }
}

TEST_CASE("rejoin inserts a new entry behind the arrivals and names it as the abandoned row's successor",
          "[engine][hostqueue][hq-missing-entry]") {
  scratch_dir scratch;
  auto        conn    = open_scratch_store(scratch);
  auto const  old_seq = enqueue_one(conn, request_for("first"));
  auto const  arrival = enqueue_one(conn, request_for("arrival"));
  end_one(conn, old_seq, hq::end_request{.outcome = hq::history_outcome::abandoned, .ended_at = k_enqueued_at + 5});

  auto const got = hq::rejoin(conn, old_seq, request_for("first"));
  REQUIRE(got.has_value());
  CHECK(got->status == hq::rejoin_status::rejoined);
  CHECK(got->seq > arrival);
  CHECK_FALSE(got->outcome.has_value());
  CHECK(history_of(conn, old_seq).successor_seq == got->seq);
  auto const entries = hq::list(conn).value();
  REQUIRE(entries.size() == 2);
  CHECK(entries.back().seq == got->seq);
  CHECK(entries.back().state == hq::entry_state::waiting);
  CHECK(entries.back().label == "first");
}

TEST_CASE("rejoin writes nothing for a row that did not end abandoned or is not there", "[engine][hostqueue][hq-missing-entry]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto const gone = hq::rejoin(conn, 77, request_for("none"));
  REQUIRE(gone.has_value());
  CHECK(gone->status == hq::rejoin_status::no_history);

  auto const cancelled = enqueue_one(conn, request_for("cancelled"));
  end_one(conn, cancelled,
          hq::end_request{.outcome      = hq::history_outcome::cancelled,
                          .cancelled_by = hq::canceller{.vendor = "claude", .role = "operator", .pid = 9},
                          .ended_at     = k_enqueued_at + 5});
  auto const refused = hq::rejoin(conn, cancelled, request_for("cancelled"));
  REQUIRE(refused.has_value());
  CHECK(refused->status == hq::rejoin_status::not_abandoned);
  CHECK(refused->outcome == hq::history_outcome::cancelled);

  CHECK(hq::list(conn).value().empty());
  CHECK_FALSE(history_of(conn, cancelled).successor_seq.has_value());
}

TEST_CASE("a rejoin whose insert fails leaves the old row without a successor", "[engine][hostqueue][hq-missing-entry]") {
  scratch_dir scratch;
  auto        conn    = open_scratch_store(scratch);
  auto const  old_seq = enqueue_one(conn, request_for("first"));
  end_one(conn, old_seq, hq::end_request{.outcome = hq::history_outcome::abandoned, .ended_at = k_enqueued_at + 5});
  REQUIRE(conn.execute("create trigger refuse_insert before insert on queue_entries begin select raise(abort, 'refused'); end;")
              .has_value());

  auto const got = hq::rejoin(conn, old_seq, request_for("first"));
  CHECK_FALSE(got.has_value());
  CHECK_FALSE(history_of(conn, old_seq).successor_seq.has_value());
  CHECK(hq::list(conn).value().empty());
}

TEST_CASE("the history row copies the run and wait limits the entry recorded",
          "[engine][hostqueue][hq-history][hq-queue-limit-columns]") {
  // Task hq-queue-limit-columns: `queue status` answers an ended entry from
  // its history row, so ending an entry carries its limits across. The run
  // limit is set by the poll that starts the entry, which is how this case
  // gets one; an entry that never started has none to copy.
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);

  auto ran_request               = request_for("ran");
  ran_request.wait_deadline_mono = ran_request.refreshed_mono + 600'000;
  ran_request.wait_limit_ms      = 600'000;
  auto const ran                 = enqueue_one(conn, ran_request);

  auto gave_up_request               = request_for("gave-up");
  gave_up_request.wait_deadline_mono = gave_up_request.refreshed_mono + 120'000;
  gave_up_request.wait_limit_ms      = 120'000;
  auto const gave_up                 = enqueue_one(conn, gave_up_request);
  end_one(conn, gave_up, hq::end_request{.outcome = hq::history_outcome::wait_timeout, .ended_at = k_enqueued_at + 5});

  planar::process::identity::system_clock clock;
  auto const                              polled = hq::poll(
      conn, hq::poll_request{.seq = ran, .host_id = "boot-7f3a", .slots = 1, .stale_after_ms = 60'000, .run_limit_ms = 300'000},
      clock, hq::system_process_probe());
  REQUIRE(polled.has_value());
  REQUIRE(polled->started);
  end_one(conn, ran, hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = k_enqueued_at + 9});

  auto const ran_row = history_of(conn, ran);
  CHECK(ran_row.run_limit_ms == 300'000);
  CHECK(ran_row.wait_limit_ms == 600'000);
  auto const gave_up_row = history_of(conn, gave_up);
  CHECK_FALSE(gave_up_row.run_limit_ms.has_value());
  CHECK(gave_up_row.wait_limit_ms == 120'000);
}

TEST_CASE("rejoin keeps the wait limit of the request it re-enqueues",
          "[engine][hostqueue][hq-missing-entry][hq-queue-limit-columns]") {
  // The submitter passes its original request (queue run's `make_request`),
  // so the rejoined entry reports the same wait limit as the reaped one.
  scratch_dir scratch;
  auto        conn           = open_scratch_store(scratch);
  auto        request        = request_for("first");
  request.wait_deadline_mono = request.refreshed_mono + 600'000;
  request.wait_limit_ms      = 600'000;
  auto const old_seq         = enqueue_one(conn, request);
  end_one(conn, old_seq, hq::end_request{.outcome = hq::history_outcome::abandoned, .ended_at = k_enqueued_at + 5});
  CHECK(history_of(conn, old_seq).wait_limit_ms == 600'000);

  auto const got = hq::rejoin(conn, old_seq, request);
  REQUIRE(got.has_value());
  REQUIRE(got->status == hq::rejoin_status::rejoined);
  auto const found = hq::find(conn, got->seq);
  REQUIRE((found.has_value() && found->has_value()));
  CHECK((*found)->wait_limit_ms == 600'000);
  CHECK((*found)->wait_deadline_mono == request.refreshed_mono + 600'000);
  CHECK_FALSE((*found)->run_limit_ms.has_value());
}
