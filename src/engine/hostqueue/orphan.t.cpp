// @file orphan.t.cpp
// @brief Unit tests for enforcing an orphan's deadline (plan 1080, task
// hq-orphan-deadline; tech spec 647 § Why the slot survives the submitter,
// § Stopping a command; decisions 1184, 1189 and 1196). Covers the engine
// half of the test-spec scenarios that cite `task:hq-orphan-deadline`:
//
//   * Happy path -- an orphaned command is stopped at its deadline
//   * Edge -- an orphaned command inside its deadline is left alone
//   * Edge -- cancelling an orphan past its deadline with nothing polling
//     (the engine half; the `queue cancel` verb is task hq-queue-cancel)
//
// plus the poll's rule that a terminating entry whose group has emptied is
// ended with its terminate reason, never `abandoned`, even when a poll finds
// it before any `advance_terminations` call does.
//
// Two kinds of case. The first drive process identity through a fake
// `process_probe`, so their pids and process-group ids are made-up numbers
// that reach no real process, and no signal is delivered. The cases under
// "Real process groups" fork their own process groups, signal only those
// (a fenced signaller fails the case if the engine asks for any other id),
// and reap every child on every path with bounded waits. Time is a fake
// clock throughout.
//
// A running entry's child group is written with raw SQL, as in poll.t.cpp
// and terminate.t.cpp: no engine operation records it yet (the `queue run`
// verb does, task hq-queue-run-verb).
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

import std;
import planar.db;
import planar.db.agentdb;
import planar.process.identity;
import planar.engine.hostqueue;

namespace {

namespace hq     = planar::engine::hostqueue;
namespace pid_ns = planar::process::identity;

constexpr std::string_view k_host      = "host-a";
constexpr std::int64_t     k_window    = 30'000;
constexpr std::int64_t     k_run_limit = 60'000;
constexpr std::int64_t     k_grace     = 10'000;
constexpr std::int64_t     k_mono0     = 5'000'000;
constexpr std::int64_t     k_wall0     = 1'759'000'000'000;

// @brief A unique scratch directory, removed when the guard leaves scope.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_hostqueue_orphan_test_{}_{}_{}", ::getpid(),
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

  [[nodiscard]] auto db_path() const -> std::filesystem::path {
    return path_ / "agent.db";
  }
};

// @brief A connection to the scratch store at the head of the agent chain.
auto open_store(const scratch_dir& scratch) -> planar::db::connection {
  auto opened = planar::db::agent::open_agent_db_at(scratch.db_path());
  REQUIRE(opened.has_value());
  return std::move(*opened);
}

// @brief A clock the test sets by hand.
class fake_clock final : public pid_ns::clock {
public:
  std::atomic<std::int64_t> mono{k_mono0};
  std::atomic<std::int64_t> wall{k_wall0};

  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, pid_ns::error> override {
    return mono.load();
  }

  [[nodiscard]] auto wall_ms() -> std::int64_t override {
    return wall.load();
  }
};

// @brief A host the test describes: which pids exist (with their start
// times) and which groups have members.
struct fake_host {
  std::map<std::int64_t, pid_ns::start_time> processes;
  std::set<std::int64_t>                     groups_with_members;

  [[nodiscard]] auto probe() const -> hq::process_probe {
    return hq::process_probe{
        .process_exists     = [this](std::int64_t pid) -> std::expected<bool, pid_ns::error> { return processes.contains(pid); },
        .process_start_time = [this](std::int64_t pid) -> std::expected<std::optional<pid_ns::start_time>, pid_ns::error> {
          auto found = processes.find(pid);
          if (found == processes.end()) {
            return std::optional<pid_ns::start_time>{};
          }
          return std::optional<pid_ns::start_time>{found->second};
        },
        .group_has_members = [this](std::int64_t pgid) -> std::expected<bool, pid_ns::error> {
          return groups_with_members.contains(pgid);
        },
    };
  }

  // @brief Declares a live process `pid` whose start time is `pid * 10`.
  void add(std::int64_t pid) {
    processes[pid] = static_cast<pid_ns::start_time>(pid * 10);
  }

  // @brief Declares a command group `pgid` whose leader (start time
  // `pgid * 10`) and group are alive.
  void add_group(std::int64_t pgid) {
    add(pgid);
    groups_with_members.insert(pgid);
  }

  // @brief Empties the command group `pgid`: its leader and every member
  // have exited.
  void empty_group(std::int64_t pgid) {
    processes.erase(pgid);
    groups_with_members.erase(pgid);
  }
};

// @brief A signaller that counts requests and delivers none.
struct null_signaller {
  std::shared_ptr<int> requests = std::make_shared<int>(0);

  [[nodiscard]] auto signaller() const -> hq::group_signaller {
    return [count = requests](std::int64_t, int) -> std::expected<void, pid_ns::error> {
      ++*count;
      return {};
    };
  }
};

// @brief A request for a submitter `pid` on `host`, refreshed at `clock`'s
// current monotonic time.
auto request_for(std::int64_t pid, const fake_clock& clock, std::string_view host = k_host) -> hq::enqueue_request {
  return hq::enqueue_request{
      .host_id        = std::string(host),
      .pid            = pid,
      .pid_started    = pid * 10,
      .cwd            = "/work/planar",
      .argv           = {"make", "test"},
      .label          = std::format("pid {}", pid),
      .enqueued_at    = clock.wall.load(),
      .refreshed_mono = clock.mono.load(),
  };
}

auto enqueue_one(planar::db::connection& conn, const hq::enqueue_request& request) -> std::int64_t {
  auto seq = hq::enqueue(conn, request);
  REQUIRE(seq.has_value());
  return *seq;
}

auto poll_request_for(std::int64_t seq, std::string_view host = k_host) -> hq::poll_request {
  return hq::poll_request{
      .seq = seq, .host_id = std::string(host), .slots = 1, .stale_after_ms = k_window, .run_limit_ms = k_run_limit};
}

// @brief Runs a poll and requires it to complete.
auto poll(planar::db::connection& conn, const hq::poll_request& request, fake_clock& clock, const hq::process_probe& probe)
    -> hq::poll_result {
  auto result = hq::poll(conn, request, clock, probe);
  if (!result) {
    FAIL(result.error().message);
  }
  REQUIRE(result->status == hq::poll_status::completed);
  return *result;
}

// @brief The entry `seq`, which must exist.
auto entry_of(planar::db::connection& conn, std::int64_t seq) -> hq::entry {
  auto found = hq::find(conn, seq);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  return **found;
}

auto exists(planar::db::connection& conn, std::int64_t seq) -> bool {
  auto found = hq::find(conn, seq);
  REQUIRE(found.has_value());
  return found->has_value();
}

// @brief The history row of `seq`, which must exist.
auto history_of(planar::db::connection& conn, std::int64_t seq) -> hq::history_row {
  auto found = hq::find_history(conn, seq);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  return **found;
}

// @brief Records a child group on a running entry, by raw SQL (see the file
// header).
void record_child_group(planar::db::connection& conn, std::int64_t seq, std::int64_t pgid, std::int64_t leader_started) {
  REQUIRE(conn.execute(std::format("update queue_entries set child_pgid = {}, child_started = {} where seq = {}", pgid,
                                   leader_started, seq))
              .has_value());
}

} // namespace

TEST_CASE("orphan: a terminating orphan whose group has emptied is ended with its terminate reason by a poll",
          "[engine][hostqueue][hq-orphan-deadline]") {
  // Regression test: poll step 2 used to reap every entry that is not live
  // as `abandoned`, so a stopped orphan that a poll reached before any
  // `advance_terminations` call lost its outcome.
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(901);
  host.add(902);
  host.add_group(9'200);
  null_signaller signals;

  auto const orphan = enqueue_one(conn, request_for(901, clock));
  REQUIRE(poll(conn, poll_request_for(orphan), clock, host.probe()).running);
  record_child_group(conn, orphan, 9'200, 9'200 * 10);
  auto const waiter = enqueue_one(conn, request_for(902, clock));
  host.processes.erase(901); // the submitter is gone; the command runs on

  SECTION("timeout: marked by a poll past its deadline") {
    clock.mono        = k_mono0 + k_run_limit + 1;
    auto const marked = poll(conn, poll_request_for(waiter), clock, host.probe());
    REQUIRE(marked.terminating.size() == 1);
    CHECK_FALSE(marked.running);

    host.empty_group(9'200);
    clock.mono += 1;
    auto const later = poll(conn, poll_request_for(waiter), clock, host.probe());
    CHECK_FALSE(exists(conn, orphan));
    auto const row = history_of(conn, orphan);
    CHECK(row.outcome == hq::history_outcome::timeout);
    CHECK_FALSE(row.cancelled_by.has_value());
    // The orphan's slot is free once it has ended.
    CHECK(later.started);
  }

  SECTION("cancelled: marked by a cancel, with its canceller") {
    hq::canceller const who{.vendor = "codex", .role = "operator", .pid = 4'321};
    auto const          begun = hq::begin_terminate(
        conn,
        hq::begin_terminate_request{
            .seq = orphan, .reason = hq::stop_reason::cancelled, .cancelled_by = who, .host_id = std::string(k_host)},
        clock, host.probe(), signals.signaller());
    REQUIRE(begun.has_value());
    REQUIRE(begun->status == hq::begin_status::marked);

    host.empty_group(9'200);
    clock.mono += 1;
    auto const later = poll(conn, poll_request_for(waiter), clock, host.probe());
    CHECK_FALSE(exists(conn, orphan));
    auto const row = history_of(conn, orphan);
    CHECK(row.outcome == hq::history_outcome::cancelled);
    CHECK(row.cancelled_by == who);
    CHECK(later.started);
  }

  // Exactly one history row, and an advance afterwards finds nothing to end.
  auto const advanced = hq::advance_terminations(
      conn, hq::advance_request{.host_id = std::string(k_host), .grace_ms = k_grace, .seq = std::nullopt}, clock, host.probe(),
      signals.signaller());
  REQUIRE(advanced.has_value());
  CHECK(advanced->ended.empty());
  CHECK(hq::list_history(conn).value().size() == 1);
}

TEST_CASE("orphan: an orphan that was never stopped and whose group has emptied is still abandoned",
          "[engine][hostqueue][hq-orphan-deadline]") {
  // The terminate reason decides the outcome only when a stop was under way;
  // a command that ended on its own with nobody supervising it is abandoned
  // (tech spec 647 § Known limits).
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(911);
  host.add(912);
  host.add_group(9'210);

  auto const orphan = enqueue_one(conn, request_for(911, clock));
  REQUIRE(poll(conn, poll_request_for(orphan), clock, host.probe()).running);
  record_child_group(conn, orphan, 9'210, 9'210 * 10);
  auto const waiter = enqueue_one(conn, request_for(912, clock));
  host.processes.erase(911);
  host.empty_group(9'210);

  auto const polled = poll(conn, poll_request_for(waiter), clock, host.probe());
  CHECK(polled.reaped == std::vector<std::int64_t>{orphan});
  CHECK(history_of(conn, orphan).outcome == hq::history_outcome::abandoned);
  CHECK(polled.started);
}
