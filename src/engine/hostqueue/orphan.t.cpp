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
import planar.db.migrate;
import planar.process.identity;
import planar.engine.hostqueue;

#include "scratch_store.hpp"

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
    return path_ / "planar.db";
  }
};

// @brief A connection to the scratch planar.db at the head of the main chain.
auto open_store(const scratch_dir& scratch) -> planar::db::connection {
  auto opened = open_main_store_at(scratch.db_path());
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
    REQUIRE(later.stopped.size() == 1);
    CHECK(later.stopped[0].seq == orphan);
    CHECK(later.stopped[0].outcome == hq::history_outcome::timeout);
    CHECK(later.reaped.empty());
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
    REQUIRE(later.stopped.size() == 1);
    CHECK(later.stopped[0].seq == orphan);
    CHECK(later.stopped[0].outcome == hq::history_outcome::cancelled);
    CHECK(later.reaped.empty());
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
  CHECK(polled.stopped.empty());
  CHECK(history_of(conn, orphan).outcome == hq::history_outcome::abandoned);
  CHECK(polled.started);
}

TEST_CASE("orphan: a cancelled orphan with no readable canceller does not block the poll",
          "[engine][hostqueue][hq-orphan-deadline]") {
  // `begin_terminate` always records the canceller of a cancellation; a row
  // written some other way may not. `end_entry` refuses a cancelled end with
  // no canceller, so the poll must not ask for one, or every poll would fail
  // for as long as the row stood. It is reaped as abandoned instead.
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(941);
  host.add(942);
  host.add_group(9'410);

  auto const orphan = enqueue_one(conn, request_for(941, clock));
  REQUIRE(poll(conn, poll_request_for(orphan), clock, host.probe()).running);
  record_child_group(conn, orphan, 9'410, 9'410 * 10);
  auto const waiter = enqueue_one(conn, request_for(942, clock));
  host.processes.erase(941);
  host.empty_group(9'410);

  SECTION("no canceller") {
    // Raw SQL: no engine operation writes this row.
    REQUIRE(conn.execute(std::format("update queue_entries set terminating_since_mono = {}, terminate_reason = 'cancelled', "
                                     "cancelled_by = null where seq = {}",
                                     k_mono0, orphan))
                .has_value());
  }
  SECTION("a canceller that is not an object") {
    REQUIRE(conn.execute(std::format("update queue_entries set terminating_since_mono = {}, terminate_reason = 'cancelled', "
                                     "cancelled_by = 'not json' where seq = {}",
                                     k_mono0, orphan))
                .has_value());
  }

  auto const polled = poll(conn, poll_request_for(waiter), clock, host.probe());
  CHECK(polled.reaped == std::vector<std::int64_t>{orphan});
  CHECK(polled.stopped.empty());
  CHECK(history_of(conn, orphan).outcome == hq::history_outcome::abandoned);
  CHECK(polled.started);
}

namespace {

// @brief One signal the engine asked for, and what the store looked like at
// that moment.
struct signal_record {
  std::int64_t                pgid           = 0;
  int                         sig            = 0;
  bool                        in_transaction = false; // The polling connection had a transaction open.
  bool                        lock_free      = false; // Another connection could take the write lock.
  std::optional<std::int64_t> marker_seen;            // The marker another connection read for `seq`.
};

// @brief A signaller that records every request and delivers none, checking
// at each request, through a second connection with no busy timeout, that
// the write lock is free and the marker already committed.
struct recorder {
  planar::db::connection*    poller   = nullptr;
  planar::db::connection*    observer = nullptr;
  std::int64_t               seq      = 0;
  std::vector<signal_record> records;

  [[nodiscard]] auto signaller() -> hq::group_signaller {
    return [this](std::int64_t pgid, int sig) -> std::expected<void, pid_ns::error> {
      signal_record rec{.pgid = pgid, .sig = sig};
      rec.in_transaction = poller->in_transaction();
      auto const begun   = observer->execute("begin immediate;");
      rec.lock_free      = begun.has_value();
      auto found         = hq::find(*observer, seq);
      if (found && found->has_value()) {
        rec.marker_seen = (*found)->terminating_since_mono;
      }
      if (begun) {
        REQUIRE(observer->execute("rollback;").has_value());
      }
      records.push_back(rec);
      return {};
    };
  }
};

auto stop_request_for(std::int64_t seq, std::string_view host = k_host, std::int64_t grace = k_grace) -> hq::poll_stop_request {
  return hq::poll_stop_request{.poll = poll_request_for(seq, host), .grace_ms = grace};
}

// @brief Runs `poll_and_stop` and requires it, its poll and its advance to
// succeed.
auto poll_and_stop(planar::db::connection& conn, const hq::poll_stop_request& request, fake_clock& clock,
                   const hq::process_probe& probe, const hq::group_signaller& signaller) -> hq::poll_stop_result {
  auto result = hq::poll_and_stop(conn, request, clock, probe, signaller);
  if (!result) {
    FAIL(result.error().message);
  }
  REQUIRE(result->poll.status == hq::poll_status::completed);
  if (result->advance_error) {
    FAIL(result->advance_error->message);
  }
  return *result;
}

} // namespace

TEST_CASE("orphan: poll_and_stop signals what its poll marked only after the poll commits, then advances",
          "[engine][hostqueue][hq-orphan-deadline]") {
  scratch_dir scratch;
  auto        conn     = open_store(scratch);
  auto        observer = open_store(scratch);
  REQUIRE(observer.execute("pragma busy_timeout = 0;").has_value());
  fake_clock clock;
  fake_host  host;
  host.add(921);
  host.add(922);
  host.add_group(9'300);

  auto const orphan = enqueue_one(conn, request_for(921, clock));
  REQUIRE(poll(conn, poll_request_for(orphan), clock, host.probe()).running);
  record_child_group(conn, orphan, 9'300, 9'300 * 10);
  auto const waiter = enqueue_one(conn, request_for(922, clock));
  host.processes.erase(921);
  recorder rec{.poller = &conn, .observer = &observer, .seq = orphan};

  // Past the deadline: the poll marks the orphan, and SIGTERM follows the
  // commit. Terminating for no time at all, it is not killed.
  clock.mono           = k_mono0 + k_run_limit + 1;
  auto const marked_at = clock.mono.load();
  auto const first     = poll_and_stop(conn, stop_request_for(waiter), clock, host.probe(), rec.signaller());
  REQUIRE(first.poll.terminating.size() == 1);
  REQUIRE(first.sigterms.size() == 1);
  CHECK(first.sigterms[0].seq == orphan);
  CHECK(first.sigterms[0].signal == hq::stop_signal::term);
  CHECK(first.sigterms[0].outcome == hq::signal_outcome::sent);
  REQUIRE(rec.records.size() == 1);
  CHECK(rec.records[0].pgid == 9'300);
  CHECK(rec.records[0].sig == SIGTERM);
  CHECK_FALSE(rec.records[0].in_transaction);
  CHECK(rec.records[0].lock_free);
  CHECK(rec.records[0].marker_seen == marked_at);
  CHECK(first.advanced.kills.empty());
  CHECK(first.advanced.ended.empty());
  CHECK_FALSE(first.poll.running);

  // The group ignores SIGTERM (the recorder delivers nothing). Past the grace
  // period the same call's advance sends SIGKILL, also outside the lock; the
  // orphan keeps its slot.
  clock.mono += k_grace + 1;
  auto const second = poll_and_stop(conn, stop_request_for(waiter), clock, host.probe(), rec.signaller());
  CHECK(second.poll.terminating.empty());
  CHECK(second.sigterms.empty());
  REQUIRE(second.advanced.kills.size() == 1);
  CHECK(second.advanced.kills[0].outcome == hq::signal_outcome::sent);
  REQUIRE(rec.records.size() == 2);
  CHECK(rec.records[1].sig == SIGKILL);
  CHECK_FALSE(rec.records[1].in_transaction);
  CHECK(rec.records[1].lock_free);
  CHECK_FALSE(second.poll.running);
  CHECK(exists(conn, orphan));

  // The group empties: the next call's poll ends the orphan as timed out and
  // gives the waiter its turn; its advance finds nothing left to end.
  host.empty_group(9'300);
  auto const third = poll_and_stop(conn, stop_request_for(waiter), clock, host.probe(), rec.signaller());
  REQUIRE(third.poll.stopped.size() == 1);
  CHECK(third.poll.stopped[0].outcome == hq::history_outcome::timeout);
  CHECK(third.poll.started);
  CHECK(third.advanced.ended.empty());
  CHECK(history_of(conn, orphan).outcome == hq::history_outcome::timeout);
  CHECK(rec.records.size() == 2);
  CHECK_FALSE(conn.in_transaction());
}

TEST_CASE("orphan: poll_and_stop does nothing for a bad grace period or a skipped poll",
          "[engine][hostqueue][hq-orphan-deadline]") {
  scratch_dir scratch;
  auto        conn  = open_store(scratch);
  auto        other = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(931);
  host.add(932);
  host.add_group(9'310);

  auto const orphan = enqueue_one(conn, request_for(931, clock));
  REQUIRE(poll(conn, poll_request_for(orphan), clock, host.probe()).running);
  record_child_group(conn, orphan, 9'310, 9'310 * 10);
  auto const waiter = enqueue_one(conn, request_for(932, clock));
  host.processes.erase(931);
  null_signaller signals;
  clock.mono = k_mono0 + k_run_limit + 1;

  SECTION("a negative grace period is refused before the poll runs") {
    auto const refused = hq::poll_and_stop(conn, stop_request_for(waiter, k_host, -1), clock, host.probe(), signals.signaller());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == hq::queue_error_kind::invalid_request);
    CHECK(entry_of(conn, waiter).refreshed_mono == k_mono0);
    CHECK_FALSE(entry_of(conn, orphan).terminating_since_mono.has_value());
  }

  SECTION("a poll skipped on a busy store signals and advances nothing") {
    // Marked by an earlier poll and now past the grace period, so an advance
    // would send SIGKILL.
    REQUIRE(poll(conn, poll_request_for(waiter), clock, host.probe()).terminating.size() == 1);
    clock.mono += k_grace + 1;
    REQUIRE(conn.execute("pragma busy_timeout = 0;").has_value());
    REQUIRE(other.execute("begin immediate;").has_value());
    auto const skipped = hq::poll_and_stop(conn, stop_request_for(waiter), clock, host.probe(), signals.signaller());
    REQUIRE(other.execute("rollback;").has_value());
    REQUIRE(skipped.has_value());
    CHECK(skipped->poll.status == hq::poll_status::skipped);
    CHECK(skipped->sigterms.empty());
    CHECK(skipped->advanced.kills.empty());
    CHECK(skipped->advanced.ended.empty());
  }

  CHECK(*signals.requests == 0);
}

// ---------------------------------------------------------------------------
// Real process groups
// ---------------------------------------------------------------------------

namespace {

// @brief Reads `count` readiness bytes from `fd`; false when the pipe closed
// first (a child died before it was ready).
auto read_ready(int fd, int count) -> bool {
  for (int got = 0; got < count;) {
    char            byte = 0;
    ::ssize_t const n    = ::read(fd, &byte, 1);
    if (n == 1) {
      ++got;
    } else if (n == 0 || errno != EINTR) {
      return false;
    }
  }
  return true;
}

// @brief A process group this test creates: a leader and a helper it forks
// (the leftover helper of decision 1196), both blocked reading a release
// pipe. With `ignore_term` both ignore SIGTERM. The constructor returns once
// both have written their readiness byte, so the SIGTERM disposition is in
// place. The destructor releases and reaps on every path, with bounded
// waits. Adapted from terminate.t.cpp.
class command_group {
public:
  explicit command_group(bool ignore_term) {
    std::array<int, 2> ready{};
    std::array<int, 2> release{};
    if (::pipe(ready.data()) != 0 || ::pipe(release.data()) != 0) {
      throw std::runtime_error("pipe failed");
    }
    ::pid_t const forked = ::fork();
    if (forked < 0) {
      throw std::runtime_error("fork failed");
    }
    if (forked == 0) {
      ::setpgid(0, 0);
      // The fork inherits the test runner's handler, which would catch
      // SIGTERM, report it as a failure of the forked copy and exit 0; the
      // command must instead die of SIGTERM or ignore it.
      ::signal(SIGTERM, ignore_term ? SIG_IGN : SIG_DFL);
      ::close(ready[0]);
      ::close(release[1]);
      if (::fork() == 0) {
        // The helper stays in the leader's group and inherits its SIGTERM
        // disposition.
        char const hb = 'h';
        (void)::write(ready[1], &hb, 1);
        ::close(ready[1]);
        char byte = 0;
        while (::read(release[0], &byte, 1) < 0 && errno == EINTR) {
        }
        ::_exit(0);
      }
      char const lb = 'l';
      (void)::write(ready[1], &lb, 1);
      ::close(ready[1]);
      char byte = 0;
      while (::read(release[0], &byte, 1) < 0 && errno == EINTR) {
      }
      ::_exit(0);
    }
    ::setpgid(forked, forked);
    ::close(ready[1]);
    ::close(release[0]);
    _pid          = forked;
    _release      = release[1];
    bool const ok = read_ready(ready[0], 2);
    ::close(ready[0]);
    if (!ok) {
      finish();
      throw std::runtime_error("child group did not become ready");
    }
  }
  command_group(const command_group&)                    = delete;
  auto operator=(const command_group&) -> command_group& = delete;
  command_group(command_group&&)                         = delete;
  auto operator=(command_group&&) -> command_group&      = delete;
  ~command_group() {
    finish();
  }

  [[nodiscard]] auto pgid() const -> std::int64_t {
    return _pid;
  }

  // @brief Waits up to ten seconds for the leader to end and reaps it.
  // @return Its wait status, or `std::nullopt` when it is still running.
  auto reap_leader() -> std::optional<int> {
    if (_reaped) {
      return _status;
    }
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      int           status = 0;
      ::pid_t const got    = ::waitpid(_pid, &status, WNOHANG);
      if (got == _pid) {
        _reaped = true;
        _status = status;
        return status;
      }
      if (got < 0 && errno != EINTR) {
        return std::nullopt;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
  }

  // @brief Closes the release pipe, so every member exits of its own
  // accord, and reaps the leader.
  // @return The leader's wait status, or `std::nullopt` when it did not end.
  auto release_and_reap() -> std::optional<int> {
    if (_release >= 0) {
      ::close(_release);
      _release = -1;
    }
    return reap_leader();
  }

  // @brief Waits up to ten seconds for the group to have no member.
  [[nodiscard]] auto wait_group_empty() const -> bool {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      auto const members = pid_ns::group_has_members(_pid);
      if (members && !*members) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

private:
  // @brief Releases the members, then reaps the leader (killing it by pid if
  // it outlives the bound; it is this test's own unreaped child, so its id
  // cannot have been reused) and waits for the helper to leave the group.
  void finish() {
    if (_pid > 0 && !_reaped && !release_and_reap()) {
      ::kill(_pid, SIGKILL);
      int status = 0;
      while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
      }
      _reaped = true;
    }
    if (_release >= 0) {
      ::close(_release);
      _release = -1;
    }
    if (_pid > 0) {
      (void)wait_group_empty();
    }
  }

  ::pid_t            _pid     = 0;
  int                _release = -1;
  bool               _reaped  = false;
  std::optional<int> _status;
};

// @brief The pid of a child that has exited and been reaped: a submitter
// that is gone.
auto reaped_pid() -> std::int64_t {
  ::pid_t const forked = ::fork();
  if (forked < 0) {
    throw std::runtime_error("fork failed");
  }
  if (forked == 0) {
    ::_exit(0);
  }
  int status = 0;
  while (::waitpid(forked, &status, 0) < 0 && errno == EINTR) {
  }
  return forked;
}

// @brief The system signaller, fenced to the one group this test created:
// any other id is refused and counted, and never reaches `kill`.
struct fenced_signaller {
  std::int64_t              allowed = 0;
  std::vector<std::int64_t> refused;
  std::vector<int>          delivered;

  [[nodiscard]] auto signaller() -> hq::group_signaller {
    return [this](std::int64_t pgid, int sig) -> std::expected<void, pid_ns::error> {
      if (pgid != allowed || allowed <= 1) {
        refused.push_back(pgid);
        return std::unexpected(pid_ns::error::not_permitted);
      }
      delivered.push_back(sig);
      return pid_ns::signal_group(pgid, sig);
    };
  }
};

// @brief The queue as the real host sees it: an orphaned running entry for
// a command group this test created (its submitter a pid that has exited),
// and a live waiter behind it whose submitter is this test process.
struct orphan_fixture {
  std::string  host;
  std::int64_t orphan   = 0;
  std::int64_t waiter   = 0;
  std::int64_t deadline = 0; ///< The orphan's `deadline_mono`.

  orphan_fixture(planar::db::connection& conn, fake_clock& clock, const command_group& command)
      : host(pid_ns::host_identity(pid_ns::native_identity_source())) {
    REQUIRE(host != pid_ns::k_unknown_host_identity);
    auto const probe = hq::system_process_probe();

    auto orphan_request        = request_for(reaped_pid(), clock, host);
    orphan_request.pid_started = 1;
    orphan                     = enqueue_one(conn, orphan_request);
    // The poll's own entry is never judged, so the orphan's turn is taken
    // by polling as it.
    auto const started = poll(conn, poll_request_for(orphan, host), clock, probe);
    REQUIRE(started.running);
    REQUIRE(started.deadline_mono.has_value());
    deadline                  = *started.deadline_mono;
    auto const leader_started = pid_ns::process_start_time(command.pgid()).value().value();
    record_child_group(conn, orphan, command.pgid(), static_cast<std::int64_t>(leader_started));

    auto self_request        = request_for(static_cast<std::int64_t>(::getpid()), clock, host);
    self_request.pid_started = static_cast<std::int64_t>(pid_ns::process_start_time(::getpid()).value().value());
    waiter                   = enqueue_one(conn, self_request);
  }
};

} // namespace

TEST_CASE("orphan: an orphaned command that honours SIGTERM is stopped at its deadline, and only then does the poller start",
          "[engine][hostqueue][hq-orphan-deadline]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  auto const  probe = hq::system_process_probe();

  command_group    command(/*ignore_term=*/false);
  fenced_signaller fence{.allowed = command.pgid()};
  orphan_fixture   queue(conn, clock, command);

  // Past the deadline, the waiter's poll marks the orphan terminating with
  // reason timeout and returns it. The poll itself signals nothing, and the
  // orphan keeps its slot.
  clock.mono        = queue.deadline + 1;
  auto const marked = poll(conn, poll_request_for(queue.waiter, queue.host), clock, probe);
  REQUIRE(marked.terminating.size() == 1);
  CHECK(marked.terminating[0].seq == queue.orphan);
  CHECK(marked.terminating[0].terminate_reason == "timeout");
  CHECK(marked.reaped.empty());
  CHECK_FALSE(marked.running);
  CHECK(entry_of(conn, queue.orphan).terminating_since_mono == queue.deadline + 1);
  CHECK(pid_ns::group_has_members(command.pgid()).value());
  CHECK(fence.delivered.empty());

  // The poll's caller, with the poll committed, sends SIGTERM through the
  // guarded path. Leader and helper honour it.
  auto const term = hq::signal_child_group(marked.terminating[0], hq::stop_signal::term, queue.host, probe, fence.signaller());
  CHECK(term.outcome == hq::signal_outcome::sent);
  auto const status = command.reap_leader();
  REQUIRE(status.has_value());
  CHECK(WIFSIGNALED(*status));
  CHECK(WTERMSIG(*status) == SIGTERM);
  REQUIRE(command.wait_group_empty());

  // A later poll, with no advance in between, ends the orphan as timed out,
  // and in that poll the waiter starts.
  clock.mono += 1;
  auto const later = poll(conn, poll_request_for(queue.waiter, queue.host), clock, probe);
  REQUIRE(later.stopped.size() == 1);
  CHECK(later.stopped[0].seq == queue.orphan);
  CHECK(later.stopped[0].outcome == hq::history_outcome::timeout);
  CHECK(later.reaped.empty());
  CHECK_FALSE(exists(conn, queue.orphan));
  CHECK(history_of(conn, queue.orphan).outcome == hq::history_outcome::timeout);
  CHECK(later.started);

  CHECK(fence.delivered == std::vector<int>{SIGTERM});
  CHECK(fence.refused.empty());
}

TEST_CASE("orphan: an orphaned command that ignores SIGTERM is killed after the grace period by the polling submitter",
          "[engine][hostqueue][hq-orphan-deadline]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  auto const  probe = hq::system_process_probe();

  command_group    command(/*ignore_term=*/true);
  fenced_signaller fence{.allowed = command.pgid()};
  orphan_fixture   queue(conn, clock, command);
  auto const       request = stop_request_for(queue.waiter, queue.host);

  // Past the deadline, one call marks the orphan and sends SIGTERM after the
  // poll commits; the group ignores it.
  clock.mono        = queue.deadline + 1;
  auto const marked = poll_and_stop(conn, request, clock, probe, fence.signaller());
  REQUIRE(marked.poll.terminating.size() == 1);
  REQUIRE(marked.sigterms.size() == 1);
  CHECK(marked.sigterms[0].outcome == hq::signal_outcome::sent);
  CHECK(marked.advanced.kills.empty());
  CHECK_FALSE(marked.poll.running);
  CHECK(fence.delivered == std::vector<int>{SIGTERM});

  // Up to the end of the grace period the group keeps its members, nothing
  // kills it, and the waiter does not start.
  clock.mono      = queue.deadline + 1 + k_grace;
  auto const held = poll_and_stop(conn, request, clock, probe, fence.signaller());
  CHECK(held.advanced.kills.empty());
  CHECK(held.poll.stopped.empty());
  CHECK_FALSE(held.poll.running);
  CHECK(exists(conn, queue.orphan));
  CHECK(pid_ns::group_has_members(command.pgid()).value());

  // Past it, the next call's advance sends SIGKILL; the poll ran before the
  // kill, so the waiter still has not started.
  clock.mono += 1;
  auto const killed = poll_and_stop(conn, request, clock, probe, fence.signaller());
  REQUIRE(killed.advanced.kills.size() == 1);
  CHECK(killed.advanced.kills[0].outcome == hq::signal_outcome::sent);
  CHECK_FALSE(killed.poll.running);
  CHECK(fence.delivered == std::vector<int>{SIGTERM, SIGKILL});

  auto const status = command.reap_leader();
  REQUIRE(status.has_value());
  CHECK(WIFSIGNALED(*status));
  CHECK(WTERMSIG(*status) == SIGKILL);
  REQUIRE(command.wait_group_empty());

  // With the group, helper included, empty, the next call removes the
  // orphan as timed out, and only then does the waiter start.
  auto const turn = poll_and_stop(conn, request, clock, probe, fence.signaller());
  CHECK(turn.poll.started);
  CHECK_FALSE(exists(conn, queue.orphan));
  CHECK(history_of(conn, queue.orphan).outcome == hq::history_outcome::timeout);
  CHECK(hq::list_history(conn).value().size() == 1);

  CHECK(fence.delivered == std::vector<int>{SIGTERM, SIGKILL});
  CHECK(fence.refused.empty());
}

TEST_CASE("orphan: an orphaned command inside its deadline is left alone", "[engine][hostqueue][hq-orphan-deadline]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  auto const  probe = hq::system_process_probe();

  command_group    command(/*ignore_term=*/false);
  fenced_signaller fence{.allowed = command.pgid()};
  orphan_fixture   queue(conn, clock, command);
  auto const       request = stop_request_for(queue.waiter, queue.host);

  // Well inside the deadline, one short of it, and exactly at it: never past.
  for (std::int64_t const at : {queue.deadline - k_run_limit / 2, queue.deadline - 1, queue.deadline}) {
    clock.mono        = at;
    auto const polled = poll_and_stop(conn, request, clock, probe, fence.signaller());
    CHECK(polled.poll.terminating.empty());
    CHECK(polled.sigterms.empty());
    CHECK(polled.poll.reaped.empty());
    CHECK(polled.poll.stopped.empty());
    CHECK(polled.advanced.kills.empty());
    CHECK_FALSE(polled.poll.running);
    CHECK_FALSE(entry_of(conn, queue.orphan).terminating_since_mono.has_value());
  }

  CHECK(fence.delivered.empty());
  CHECK(fence.refused.empty());
  // Never signalled: released, the leader exits 0 of its own accord.
  auto const status = command.release_and_reap();
  REQUIRE(status.has_value());
  CHECK(WIFEXITED(*status));
  CHECK(WEXITSTATUS(*status) == 0);
}

TEST_CASE("orphan: cancelling an orphan past its deadline with nothing polling empties its group and ends it as cancelled",
          "[engine][hostqueue][hq-orphan-deadline]") {
  // Engine half of the scenario shared with task hq-queue-cancel: the
  // submitter is gone, the command blocks past its deadline, and no other
  // submitter polls, so nothing has marked it. The cancel's two steps alone
  // stop it.
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  auto const  probe = hq::system_process_probe();

  command_group    command(/*ignore_term=*/true);
  fenced_signaller fence{.allowed = command.pgid()};
  orphan_fixture   queue(conn, clock, command);

  clock.mono = queue.deadline + k_run_limit;
  CHECK_FALSE(entry_of(conn, queue.orphan).terminating_since_mono.has_value());
  CHECK(pid_ns::group_has_members(command.pgid()).value());

  hq::canceller const who{.vendor = "claude", .role = "operator", .pid = static_cast<std::int64_t>(::getpid())};
  auto const          begun = hq::begin_terminate(
      conn,
      hq::begin_terminate_request{
          .seq = queue.orphan, .reason = hq::stop_reason::cancelled, .cancelled_by = who, .host_id = queue.host},
      clock, probe, fence.signaller());
  REQUIRE(begun.has_value());
  REQUIRE(begun->status == hq::begin_status::marked);
  CHECK(begun->sigterm->outcome == hq::signal_outcome::sent);

  // `queue cancel` advances its own entry until the group is empty.
  auto const advance_own = [&] {
    auto result =
        hq::advance_terminations(conn, hq::advance_request{.host_id = queue.host, .grace_ms = k_grace, .seq = queue.orphan},
                                 clock, probe, fence.signaller());
    REQUIRE(result.has_value());
    return *result;
  };
  clock.mono += k_grace + 1;
  auto const killed = advance_own();
  REQUIRE(killed.kills.size() == 1);
  CHECK(killed.kills[0].outcome == hq::signal_outcome::sent);
  auto const status = command.reap_leader();
  REQUIRE(status.has_value());
  CHECK(WTERMSIG(*status) == SIGKILL);
  REQUIRE(command.wait_group_empty());

  auto const ended = advance_own();
  REQUIRE(ended.ended.size() == 1);
  CHECK(ended.ended[0].outcome == hq::history_outcome::cancelled);
  CHECK_FALSE(exists(conn, queue.orphan));
  auto const row = history_of(conn, queue.orphan);
  CHECK(row.outcome == hq::history_outcome::cancelled);
  CHECK(row.cancelled_by == who);
  CHECK(fence.delivered == std::vector<int>{SIGTERM, SIGKILL});
  CHECK(fence.refused.empty());
}
