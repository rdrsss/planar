// @file terminate.t.cpp
// @brief Unit tests for `planar.engine.hostqueue.terminate` (plan 1080, task
// hq-terminate). Covers the engine half of the test-spec scenarios that cite
// `task:hq-terminate`:
//
//   * Happy path -- stopping never removes an entry whose group has members
//   * Edge -- a later poll by another process sends the kill
//   * Error -- no signal is sent while the write lock is held
//   * Edge -- a reused process-group id is not mistaken for the command
//   * Edge -- a command that ignores SIGTERM is killed after the grace period
//
// plus the module's own contract: the marker is recorded once, with its
// reason and canceller; the grace comparison is strict; an entry ends with
// its terminate reason, never `abandoned`; groups 0, 1 and -1, another
// host's groups and unverifiable groups are never signalled.
//
// Two kinds of case. Most drive process identity through a fake
// `process_probe` and record signals with a recording `group_signaller` that
// never delivers anything, so their process-group ids are made-up numbers
// that reach no real process. The cases under "Real process groups" fork
// their own process groups, signal only those (a guard fails the case if the
// engine asks for any other id), and reap every child on every path with
// bounded waits. Time is a fake clock throughout: the grace period is
// measured against it, so order is asserted, not duration.
//
// A running entry's child group is written with raw SQL, as in poll.t.cpp:
// no engine operation records it yet (the `queue run` verb does, task
// hq-queue-run-verb).
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
      : path_(std::filesystem::temp_directory_path() / std::format("planar_hostqueue_terminate_test_{}_{}_{}", ::getpid(),
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

// @brief A clock the test sets by hand. `on_monotonic`, when set, runs at
// every monotonic read, before the value is returned.
class fake_clock final : public pid_ns::clock {
public:
  std::atomic<std::int64_t> mono{k_mono0};
  std::atomic<std::int64_t> wall{k_wall0};
  std::atomic<int>          monotonic_reads{0};
  std::function<void()>     on_monotonic;

  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, pid_ns::error> override {
    ++monotonic_reads;
    if (on_monotonic) {
      on_monotonic();
    }
    return mono.load();
  }

  [[nodiscard]] auto wall_ms() -> std::int64_t override {
    return wall.load();
  }
};

// @brief A host the test describes: which pids exist (with their start
// times), which groups have members, and which ids fail every query. Every
// query is counted, so a case can prove no id was queried at all.
struct fake_host {
  std::map<std::int64_t, pid_ns::start_time> processes;
  std::set<std::int64_t>                     groups_with_members;
  std::set<std::int64_t>                     failing;
  std::shared_ptr<int>                       queries = std::make_shared<int>(0);

  [[nodiscard]] auto probe() const -> hq::process_probe {
    return hq::process_probe{
        .process_exists = [this](std::int64_t pid) -> std::expected<bool, pid_ns::error> {
          ++*queries;
          if (failing.contains(pid)) {
            return std::unexpected(pid_ns::error::query_failed);
          }
          return processes.contains(pid);
        },
        .process_start_time = [this](std::int64_t pid) -> std::expected<std::optional<pid_ns::start_time>, pid_ns::error> {
          ++*queries;
          if (failing.contains(pid)) {
            return std::unexpected(pid_ns::error::query_failed);
          }
          auto found = processes.find(pid);
          if (found == processes.end()) {
            return std::optional<pid_ns::start_time>{};
          }
          return std::optional<pid_ns::start_time>{found->second};
        },
        .group_has_members = [this](std::int64_t pgid) -> std::expected<bool, pid_ns::error> {
          ++*queries;
          if (failing.contains(pgid)) {
            return std::unexpected(pid_ns::error::query_failed);
          }
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
};

// @brief One signal the engine asked for, and what the store looked like at
// that moment.
struct signal_record {
  std::int64_t                pgid           = 0;
  int                         sig            = 0;
  bool                        in_transaction = false; // The stopping connection had a transaction open.
  bool                        lock_free      = false; // Another connection could take the write lock.
  std::optional<std::int64_t> marker_seen;            // The marker another connection read for `seq`.
};

// @brief A signaller that records every request and delivers none. At each
// request it checks, through a second connection with no busy timeout,
// that the write lock is free and that the marker is already committed.
struct recorder {
  planar::db::connection*    stopper  = nullptr;
  planar::db::connection*    observer = nullptr;
  std::int64_t               seq      = 0;
  std::vector<signal_record> records;

  [[nodiscard]] auto signaller() -> hq::group_signaller {
    return [this](std::int64_t pgid, int sig) -> std::expected<void, pid_ns::error> {
      signal_record rec{.pgid = pgid, .sig = sig};
      if (stopper != nullptr) {
        rec.in_transaction = stopper->in_transaction();
      }
      if (observer != nullptr) {
        auto const begun = observer->execute("begin immediate;");
        rec.lock_free    = begun.has_value();
        auto found       = hq::find(*observer, seq);
        if (found && found->has_value()) {
          rec.marker_seen = (*found)->terminating_since_mono;
        }
        if (begun) {
          REQUIRE(observer->execute("rollback;").has_value());
        }
      }
      records.push_back(rec);
      return {};
    };
  }

  [[nodiscard]] auto count(int sig) const -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(records, [&](const signal_record& r) { return r.sig == sig; }));
  }
};

// @brief A request for a submitter `pid` on `k_host`, refreshed at `clock`'s
// current monotonic time.
auto request_for(std::int64_t pid, const fake_clock& clock) -> hq::enqueue_request {
  return hq::enqueue_request{
      .host_id        = std::string(k_host),
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

auto poll_request_for(std::int64_t seq, std::int64_t slots = 1) -> hq::poll_request {
  return hq::poll_request{
      .seq = seq, .host_id = std::string(k_host), .slots = slots, .stale_after_ms = k_window, .run_limit_ms = k_run_limit};
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

// @brief Records a child group on a running entry, by raw SQL (see the file
// header).
void record_child_group(planar::db::connection& conn, std::int64_t seq, std::int64_t pgid,
                        std::optional<std::int64_t> leader_started) {
  auto const started = leader_started ? std::to_string(*leader_started) : std::string("null");
  REQUIRE(
      conn.execute(std::format("update queue_entries set child_pgid = {}, child_started = {} where seq = {}", pgid, started, seq))
          .has_value());
}

// @brief Enqueues submitter `pid`, gives it its turn (it polls as itself, so
// the probe is not consulted for it; the slot count is large enough that
// earlier running entries do not hold it back), and records `pgid` as its command's
// group with leader start time `pgid * 10`, matching `fake_host::add_group`.
auto running_entry(planar::db::connection& conn, std::int64_t pid, fake_clock& clock, const hq::process_probe& probe,
                   std::int64_t pgid) -> std::int64_t {
  auto const seq     = enqueue_one(conn, request_for(pid, clock));
  auto const started = hq::poll(conn, poll_request_for(seq, 64), clock, probe);
  REQUIRE(started.has_value());
  REQUIRE(started->running);
  record_child_group(conn, seq, pgid, pgid * 10);
  return seq;
}

auto timeout_request(std::int64_t seq) -> hq::begin_terminate_request {
  return hq::begin_terminate_request{
      .seq = seq, .reason = hq::stop_reason::timeout, .cancelled_by = std::nullopt, .host_id = std::string(k_host)};
}

auto cancel_request(std::int64_t seq, const hq::canceller& who) -> hq::begin_terminate_request {
  return hq::begin_terminate_request{
      .seq = seq, .reason = hq::stop_reason::cancelled, .cancelled_by = who, .host_id = std::string(k_host)};
}

auto advance_request_for(std::int64_t grace = k_grace) -> hq::advance_request {
  return hq::advance_request{.host_id = std::string(k_host), .grace_ms = grace, .seq = std::nullopt};
}

// @brief Runs `begin_terminate` and requires it to succeed.
auto begin(planar::db::connection& conn, const hq::begin_terminate_request& request, fake_clock& clock,
           const hq::process_probe& probe, const hq::group_signaller& signaller) -> hq::begin_result {
  auto result = hq::begin_terminate(conn, request, clock, probe, signaller);
  if (!result) {
    FAIL(result.error().message);
  }
  return *result;
}

// @brief Runs `advance_terminations` and requires it to succeed.
auto advance(planar::db::connection& conn, const hq::advance_request& request, fake_clock& clock, const hq::process_probe& probe,
             const hq::group_signaller& signaller) -> hq::advance_result {
  auto result = hq::advance_terminations(conn, request, clock, probe, signaller);
  if (!result) {
    FAIL(result.error().message);
  }
  return *result;
}

auto sent_kills(const hq::advance_result& result) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(result.kills, [](const hq::signal_attempt& a) { return a.outcome == hq::signal_outcome::sent; }));
}

} // namespace

TEST_CASE("terminate: no signal is sent while the write lock is held", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn     = open_store(scratch);
  auto        observer = open_store(scratch);
  REQUIRE(observer.execute("pragma busy_timeout = 0;").has_value());
  fake_clock clock;
  fake_host  host;
  host.add_group(9'100);

  auto const seq = running_entry(conn, 901, clock, host.probe(), 9'100);
  recorder   rec{.stopper = &conn, .observer = &observer, .seq = seq};

  clock.mono += 1'000;
  auto const marked_at = clock.mono.load();
  auto const begun     = begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller());
  REQUIRE(begun.status == hq::begin_status::marked);
  REQUIRE(begun.sigterm.has_value());
  CHECK(begun.sigterm->outcome == hq::signal_outcome::sent);
  REQUIRE(rec.records.size() == 1);
  CHECK(rec.records[0].pgid == 9'100);
  CHECK(rec.records[0].sig == SIGTERM);
  // The SIGTERM follows the commit: the stopping connection holds no
  // transaction, another connection can take the write lock, and it already
  // reads the committed marker.
  CHECK_FALSE(rec.records[0].in_transaction);
  CHECK(rec.records[0].lock_free);
  CHECK(rec.records[0].marker_seen == marked_at);

  clock.mono += k_grace + 1;
  auto const killed = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
  CHECK(sent_kills(killed) == 1);
  REQUIRE(rec.records.size() == 2);
  CHECK(rec.records[1].sig == SIGKILL);
  CHECK_FALSE(rec.records[1].in_transaction);
  CHECK(rec.records[1].lock_free);

  // The group empties; the entry is ended in its own transaction and no
  // further signal is asked for.
  host.groups_with_members.erase(9'100);
  host.processes.erase(9'100);
  auto const ended = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
  CHECK(ended.kills.empty());
  REQUIRE(ended.ended.size() == 1);
  CHECK(rec.records.size() == 2);
  CHECK_FALSE(conn.in_transaction());
}

TEST_CASE("terminate: the marker records the reason, the canceller and the time read under the write lock",
          "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn  = open_store(scratch);
  auto        other = open_store(scratch);
  REQUIRE(other.execute("pragma busy_timeout = 0;").has_value());
  fake_clock clock;
  fake_host  host;
  host.add_group(9'200);
  auto const seq = running_entry(conn, 902, clock, host.probe(), 9'200);
  recorder   rec;

  clock.mono += 2'000;
  auto const          reads_before = clock.monotonic_reads.load();
  std::optional<bool> other_was_locked_out;
  clock.on_monotonic = [&] {
    auto const attempt   = other.execute("begin immediate;");
    other_was_locked_out = !attempt.has_value() && planar::db::is_busy(attempt.error());
    if (attempt) {
      REQUIRE(other.execute("rollback;").has_value());
    }
  };

  hq::canceller const who{.vendor = "claude", .role = "operator", .pid = 4'242};
  auto const          begun = begin(conn, cancel_request(seq, who), clock, host.probe(), rec.signaller());
  clock.on_monotonic        = nullptr;

  CHECK(clock.monotonic_reads.load() == reads_before + 1);
  REQUIRE(other_was_locked_out.has_value());
  CHECK(*other_was_locked_out);

  REQUIRE(begun.status == hq::begin_status::marked);
  REQUIRE(begun.stored.has_value());
  auto const stored = entry_of(conn, seq);
  CHECK(stored.terminating_since_mono == k_mono0 + 2'000);
  CHECK(stored.terminate_reason == "cancelled");
  REQUIRE(stored.cancelled_by.has_value());
  CHECK(hq::decode_canceller(*stored.cancelled_by).value() == who);
  CHECK(begun.stored->terminating_since_mono == stored.terminating_since_mono);
  // A terminating entry keeps its state and its slot.
  CHECK(stored.state == hq::entry_state::running);
  CHECK(hq::to_string(hq::stop_reason::timeout) == "timeout");
  CHECK(hq::to_string(hq::stop_reason::cancelled) == "cancelled");
}

TEST_CASE("terminate: stopping an entry that is already terminating changes nothing and sends nothing",
          "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'300);
  auto const seq = running_entry(conn, 903, clock, host.probe(), 9'300);
  recorder   rec;

  auto const first = begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller());
  REQUIRE(first.status == hq::begin_status::marked);
  REQUIRE(rec.records.size() == 1);

  clock.mono += 5'000;
  hq::canceller const who{.vendor = std::nullopt, .role = std::nullopt, .pid = 77};
  auto const          second = begin(conn, cancel_request(seq, who), clock, host.probe(), rec.signaller());
  CHECK(second.status == hq::begin_status::already_terminating);
  CHECK_FALSE(second.sigterm.has_value());
  CHECK(rec.records.size() == 1);

  auto const stored = entry_of(conn, seq);
  CHECK(stored.terminating_since_mono == k_mono0);
  CHECK(stored.terminate_reason == "timeout");
  CHECK_FALSE(stored.cancelled_by.has_value());
}

TEST_CASE("terminate: a waiting or missing entry is not stopped", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'400);
  auto const running = running_entry(conn, 904, clock, host.probe(), 9'400);
  auto const waiting = enqueue_one(conn, request_for(905, clock));
  recorder   rec;

  auto const not_running = begin(conn, timeout_request(waiting), clock, host.probe(), rec.signaller());
  CHECK(not_running.status == hq::begin_status::not_running);
  CHECK_FALSE(not_running.sigterm.has_value());
  CHECK_FALSE(entry_of(conn, waiting).terminating_since_mono.has_value());

  auto const missing = begin(conn, timeout_request(running + 100), clock, host.probe(), rec.signaller());
  CHECK(missing.status == hq::begin_status::missing);
  CHECK_FALSE(missing.stored.has_value());

  CHECK(rec.records.empty());
  CHECK_FALSE(entry_of(conn, running).terminating_since_mono.has_value());
}

TEST_CASE("terminate: a request that does not fit changes nothing", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'500);
  auto const seq = running_entry(conn, 906, clock, host.probe(), 9'500);
  recorder   rec;

  auto cancelled_without_canceller    = timeout_request(seq);
  cancelled_without_canceller.reason  = hq::stop_reason::cancelled;
  auto timeout_with_canceller         = timeout_request(seq);
  timeout_with_canceller.cancelled_by = hq::canceller{.vendor = std::nullopt, .role = std::nullopt, .pid = 1'234};
  for (auto const& request : {cancelled_without_canceller, timeout_with_canceller}) {
    auto const refused = hq::begin_terminate(conn, request, clock, host.probe(), rec.signaller());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == hq::queue_error_kind::invalid_request);
  }

  auto const negative = hq::advance_terminations(conn, advance_request_for(-1), clock, host.probe(), rec.signaller());
  REQUIRE_FALSE(negative.has_value());
  CHECK(negative.error().kind == hq::queue_error_kind::invalid_request);

  {
    auto txn = conn.begin_transaction();
    REQUIRE(txn.has_value());
    auto const in_begin = hq::begin_terminate(conn, timeout_request(seq), clock, host.probe(), rec.signaller());
    REQUIRE_FALSE(in_begin.has_value());
    CHECK(in_begin.error().kind == hq::queue_error_kind::invalid_request);
    auto const in_advance = hq::advance_terminations(conn, advance_request_for(), clock, host.probe(), rec.signaller());
    REQUIRE_FALSE(in_advance.has_value());
    CHECK(in_advance.error().kind == hq::queue_error_kind::invalid_request);
  }

  CHECK(rec.records.empty());
  CHECK_FALSE(entry_of(conn, seq).terminating_since_mono.has_value());
}

TEST_CASE("terminate: the kill is sent only once the grace period has passed", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'600);
  auto const seq = running_entry(conn, 907, clock, host.probe(), 9'600);
  recorder   rec;
  REQUIRE(begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller()).status == hq::begin_status::marked);
  REQUIRE(rec.records.size() == 1);

  // Exactly at the grace period: terminating for no longer than it.
  clock.mono    = k_mono0 + k_grace;
  auto const at = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
  CHECK(at.now_mono == k_mono0 + k_grace);
  CHECK(sent_kills(at) == 0);
  CHECK(rec.count(SIGKILL) == 0);

  clock.mono      = k_mono0 + k_grace + 1;
  auto const past = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
  REQUIRE(past.kills.size() == 1);
  CHECK(past.kills[0].seq == seq);
  CHECK(past.kills[0].signal == hq::stop_signal::kill);
  CHECK(past.kills[0].outcome == hq::signal_outcome::sent);
  REQUIRE(rec.count(SIGKILL) == 1);
  CHECK(rec.records.back().pgid == 9'600);
  CHECK(exists(conn, seq));
}

TEST_CASE("terminate: stopping never removes an entry whose group has members", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'700);
  host.add(909);
  auto const seq    = running_entry(conn, 908, clock, host.probe(), 9'700);
  auto const waiter = enqueue_one(conn, request_for(909, clock));
  recorder   rec;
  REQUIRE(begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller()).status == hq::begin_status::marked);

  // Its submitter is gone and the group ignores every signal (the recorder
  // delivers none): however often any process advances or polls, the entry
  // stays, carries its marker and keeps its slot.
  for (int round = 1; round <= 5; ++round) {
    clock.mono += k_grace;
    auto const advanced = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
    CHECK(advanced.ended.empty());
    auto const polled = hq::poll(conn, poll_request_for(waiter), clock, host.probe());
    REQUIRE(polled.has_value());
    CHECK(polled->reaped.empty());
    CHECK_FALSE(polled->running);

    auto const stored = entry_of(conn, seq);
    CHECK(stored.state == hq::entry_state::running);
    CHECK(stored.terminating_since_mono == k_mono0);
    CHECK(stored.terminate_reason == "timeout");
  }
  CHECK(rec.count(SIGKILL) >= 1);
  CHECK_FALSE(hq::find_history(conn, seq).value().has_value());
}

TEST_CASE("terminate: an empty group ends the entry with its terminate reason, not abandoned",
          "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'800);
  auto const seq = running_entry(conn, 910, clock, host.probe(), 9'800);
  recorder   rec;

  SECTION("timeout") {
    REQUIRE(begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller()).status == hq::begin_status::marked);
    host.groups_with_members.erase(9'800);
    host.processes.erase(9'800);
    // Inside the grace period: an empty group ends the entry at once.
    clock.mono += 1;
    auto const result = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
    REQUIRE(result.ended.size() == 1);
    CHECK(result.ended[0].seq == seq);
    CHECK(result.ended[0].outcome == hq::history_outcome::timeout);
    CHECK_FALSE(exists(conn, seq));
    auto const row = hq::find_history(conn, seq).value();
    REQUIRE(row.has_value());
    CHECK(row->outcome == hq::history_outcome::timeout);
    CHECK_FALSE(row->cancelled_by.has_value());
  }

  SECTION("cancelled, with the canceller") {
    hq::canceller const who{.vendor = "codex", .role = "coder", .pid = 5'151};
    REQUIRE(begin(conn, cancel_request(seq, who), clock, host.probe(), rec.signaller()).status == hq::begin_status::marked);
    host.groups_with_members.erase(9'800);
    auto const result = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
    REQUIRE(result.ended.size() == 1);
    CHECK(result.ended[0].outcome == hq::history_outcome::cancelled);
    auto const row = hq::find_history(conn, seq).value();
    REQUIRE(row.has_value());
    CHECK(row->outcome == hq::history_outcome::cancelled);
    CHECK(row->cancelled_by == who);
  }

  // Ending again finds nothing and writes nothing.
  auto const again = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
  CHECK(again.ended.empty());
  CHECK(hq::list_history(conn).value().size() == 1);
}

TEST_CASE("terminate: a reused process-group id is never signalled and counts as empty", "[engine][hostqueue][hq-terminate]") {
  // A process with the group's id exists with another start time, and the
  // fake says that group has members: signalling it would hit a stranger.
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'900);
  auto const seq        = running_entry(conn, 911, clock, host.probe(), 9'900);
  host.processes[9'900] = static_cast<pid_ns::start_time>(123'456); // the id now names someone else
  recorder rec;

  auto const begun = begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller());
  REQUIRE(begun.status == hq::begin_status::marked);
  REQUIRE(begun.sigterm.has_value());
  CHECK(begun.sigterm->outcome == hq::signal_outcome::reused);

  clock.mono += k_grace + 1;
  auto const result = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
  CHECK(sent_kills(result) == 0);
  CHECK(rec.records.empty());
  // Its stop was already under way, so it ends with its terminate reason.
  REQUIRE(result.ended.size() == 1);
  CHECK(result.ended[0].outcome == hq::history_outcome::timeout);
  CHECK(hq::find_history(conn, seq).value()->outcome == hq::history_outcome::timeout);
}

TEST_CASE("terminate: process groups 0, 1 and -1 are never signalled", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  recorder    rec;

  for (std::int64_t const pgid : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1}}) {
    // The fake claims every one of them is a live, matching group.
    host.add_group(pgid);
    auto const seq   = running_entry(conn, 920 + pgid, clock, host.probe(), pgid);
    auto const begun = begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller());
    REQUIRE(begun.status == hq::begin_status::marked);
    REQUIRE(begun.sigterm.has_value());
    CHECK(begun.sigterm->outcome == hq::signal_outcome::no_group);
  }
  clock.mono += k_grace + 1;
  auto const result = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
  CHECK(sent_kills(result) == 0);
  CHECK(rec.records.empty());
}

TEST_CASE("terminate: an unverifiable or foreign group is not signalled", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'010);
  host.add_group(9'020);
  recorder rec;

  SECTION("no recorded leader start time") {
    auto const seq = running_entry(conn, 930, clock, host.probe(), 9'010);
    record_child_group(conn, seq, 9'010, std::nullopt);
    auto const begun = begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller());
    REQUIRE(begun.sigterm.has_value());
    CHECK(begun.sigterm->outcome == hq::signal_outcome::no_group);
    clock.mono += k_grace + 1;
    CHECK(sent_kills(advance(conn, advance_request_for(), clock, host.probe(), rec.signaller())) == 0);
  }

  SECTION("another host identity: no id is even queried") {
    auto request       = request_for(931, clock);
    request.host_id    = "host-b";
    auto const seq     = enqueue_one(conn, request);
    auto const started = hq::poll(conn, poll_request_for(seq), clock, host.probe());
    REQUIRE(started.has_value());
    record_child_group(conn, seq, 9'020, 9'020 * 10);
    *host.queries    = 0;
    auto const begun = begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller());
    REQUIRE(begun.status == hq::begin_status::marked);
    CHECK(begun.sigterm->outcome == hq::signal_outcome::other_host);
    clock.mono += k_grace + 1;
    host.groups_with_members.clear();
    auto const result = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
    CHECK(result.ended.empty());
    CHECK(exists(conn, seq));
    CHECK(*host.queries == 0);
  }

  SECTION("the unknown host identity") {
    auto const seq     = running_entry(conn, 932, clock, host.probe(), 9'020);
    auto       request = timeout_request(seq);
    request.host_id    = std::string(pid_ns::k_unknown_host_identity);
    auto const begun   = begin(conn, request, clock, host.probe(), rec.signaller());
    CHECK(begun.sigterm->outcome == hq::signal_outcome::other_host);
  }

  CHECK(rec.records.empty());
}

TEST_CASE("terminate: a failed group query neither signals nor ends the entry", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'030);
  auto const seq = running_entry(conn, 940, clock, host.probe(), 9'030);
  recorder   rec;
  host.failing.insert(9'030);

  auto const begun = begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller());
  REQUIRE(begun.status == hq::begin_status::marked);
  CHECK(begun.sigterm->outcome == hq::signal_outcome::failed);
  CHECK(begun.sigterm->error == pid_ns::error::query_failed);

  clock.mono += k_grace + 1;
  auto const result = advance(conn, advance_request_for(), clock, host.probe(), rec.signaller());
  CHECK(result.ended.empty());
  CHECK(result.kills.empty());
  REQUIRE(result.failures.size() == 1);
  CHECK(result.failures[0].seq == seq);
  CHECK(exists(conn, seq));
  CHECK(rec.records.empty());
}

TEST_CASE("terminate: advancing one entry leaves the others alone", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'040);
  host.add_group(9'050);
  auto const first  = running_entry(conn, 950, clock, host.probe(), 9'040);
  auto const second = running_entry(conn, 951, clock, host.probe(), 9'050);
  recorder   rec;
  REQUIRE(begin(conn, timeout_request(first), clock, host.probe(), rec.signaller()).status == hq::begin_status::marked);
  REQUIRE(begin(conn, timeout_request(second), clock, host.probe(), rec.signaller()).status == hq::begin_status::marked);

  clock.mono += k_grace + 1;
  auto only    = advance_request_for();
  only.seq     = second;
  auto const r = advance(conn, only, clock, host.probe(), rec.signaller());
  REQUIRE(r.kills.size() == 1);
  CHECK(r.kills[0].seq == second);
  REQUIRE(rec.count(SIGKILL) == 1);
  CHECK(rec.records.back().pgid == 9'050);
}

TEST_CASE("terminate: the entries a poll marks are signalled through the guarded path after it commits",
          "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'060);
  host.add(961);
  auto const orphan = running_entry(conn, 960, clock, host.probe(), 9'060);
  auto const waiter = enqueue_one(conn, request_for(961, clock));
  recorder   rec{.stopper = &conn};

  clock.mono += k_run_limit + 1;
  auto const polled = hq::poll(conn, poll_request_for(waiter), clock, host.probe());
  REQUIRE(polled.has_value());
  REQUIRE(polled->terminating.size() == 1);
  CHECK(rec.records.empty());

  auto const attempt =
      hq::signal_child_group(polled->terminating[0], hq::stop_signal::term, k_host, host.probe(), rec.signaller());
  CHECK(attempt.seq == orphan);
  CHECK(attempt.outcome == hq::signal_outcome::sent);
  REQUIRE(rec.records.size() == 1);
  CHECK(rec.records[0].sig == SIGTERM);
  CHECK(rec.records[0].pgid == 9'060);
  CHECK_FALSE(rec.records[0].in_transaction);

  host.groups_with_members.erase(9'060);
  auto const empty = hq::signal_child_group(polled->terminating[0], hq::stop_signal::kill, k_host, host.probe(), rec.signaller());
  CHECK(empty.outcome == hq::signal_outcome::group_empty);
  CHECK(rec.records.size() == 1);
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

// @brief A process group this test creates: a leader and, optionally, a
// helper it forks, both blocked reading a release pipe. With `ignore_term`
// both ignore SIGTERM. The constructor returns once every member has
// written its readiness byte, so the SIGTERM disposition is in place.
// The destructor releases and reaps on every path, with bounded waits.
class command_group {
public:
  command_group(bool ignore_term, bool with_helper) {
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
      // The fork inherits the test runner's SIGTERM handler, which would
      // catch the signal, report it as a failure of the forked copy and exit
      // 0. The leader must instead ignore SIGTERM or die of it, so the
      // disposition is set explicitly both ways (as orphan.t.cpp does).
      ::signal(SIGTERM, ignore_term ? SIG_IGN : SIG_DFL);
      ::close(ready[0]);
      ::close(release[1]);
      if (with_helper && ::fork() == 0) {
        // The helper stays in the leader's group and inherits its
        // SIGTERM disposition.
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
    bool const ok = read_ready(ready[0], with_helper ? 2 : 1);
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
    if (_pid <= 0) {
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
  // @brief Closes the release pipe, so every member exits, then reaps the
  // leader (killing it by pid if it outlives the bound; it is this test's
  // own unreaped child, so its id cannot have been reused) and waits for the
  // helper to leave the group.
  void finish() {
    if (_release >= 0) {
      ::close(_release);
      _release = -1;
    }
    if (_pid > 0 && !_reaped) {
      if (!reap_leader()) {
        ::kill(_pid, SIGKILL);
        int status = 0;
        while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
        }
      }
      _reaped = true;
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

// @brief A running entry for a command group this test created, whose
// submitter has exited.
auto orphaned_entry(planar::db::connection& conn, fake_clock& clock, const command_group& command) -> std::int64_t {
  auto request        = request_for(reaped_pid(), clock);
  request.host_id     = pid_ns::host_identity(pid_ns::native_identity_source());
  request.pid_started = 1;
  auto const seq      = enqueue_one(conn, request);
  auto       poll     = poll_request_for(seq);
  poll.host_id        = request.host_id;
  auto const started  = hq::poll(conn, poll, clock, hq::system_process_probe());
  REQUIRE(started.has_value());
  REQUIRE(started->running);
  auto const leader_started = pid_ns::process_start_time(command.pgid()).value().value();
  record_child_group(conn, seq, command.pgid(), static_cast<std::int64_t>(leader_started));
  return seq;
}

} // namespace

TEST_CASE("terminate: a command that ignores SIGTERM is killed by a later poll of another process after the grace period",
          "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  fake_clock  clock;
  auto const  probe = hq::system_process_probe();
  auto const  host  = pid_ns::host_identity(pid_ns::native_identity_source());
  REQUIRE(host != pid_ns::k_unknown_host_identity);

  command_group    command(/*ignore_term=*/true, /*with_helper=*/true);
  fenced_signaller fence{.allowed = command.pgid()};

  auto       conn = open_store(scratch);
  auto const seq  = orphaned_entry(conn, clock, command);

  // Process A marks the entry and sends SIGTERM, then goes away.
  {
    auto stopper     = open_store(scratch);
    auto request     = timeout_request(seq);
    request.host_id  = host;
    auto const begun = begin(stopper, request, clock, probe, fence.signaller());
    REQUIRE(begun.status == hq::begin_status::marked);
    CHECK(begun.sigterm->outcome == hq::signal_outcome::sent);
  }
  CHECK(fence.delivered == std::vector<int>{SIGTERM});

  // A live waiter: this test process, whose entry keeps the queue polling.
  auto self_request        = request_for(static_cast<std::int64_t>(::getpid()), clock);
  self_request.host_id     = host;
  self_request.pid_started = static_cast<std::int64_t>(pid_ns::process_start_time(::getpid()).value().value());
  auto const waiter        = enqueue_one(conn, self_request);
  auto       waiter_poll   = poll_request_for(waiter);
  waiter_poll.host_id      = host;

  auto advance_by_b    = advance_request_for(k_grace);
  advance_by_b.host_id = host;

  // The group ignored SIGTERM. Up to the end of the grace period, another
  // process's polls neither kill it nor remove its entry, and the entry keeps
  // its slot.
  clock.mono += k_grace;
  auto const early = advance(conn, advance_by_b, clock, probe, fence.signaller());
  CHECK(sent_kills(early) == 0);
  CHECK(early.ended.empty());
  auto const polled = hq::poll(conn, waiter_poll, clock, probe);
  REQUIRE(polled.has_value());
  CHECK(polled->reaped.empty());
  CHECK_FALSE(polled->running);
  CHECK(pid_ns::group_has_members(command.pgid()).value());
  CHECK(entry_of(conn, seq).terminate_reason == "timeout");

  // Past the grace period the next advance sends SIGKILL to the group.
  clock.mono += 1;
  auto const killed = advance(conn, advance_by_b, clock, probe, fence.signaller());
  CHECK(sent_kills(killed) == 1);
  CHECK(fence.delivered == std::vector<int>{SIGTERM, SIGKILL});
  CHECK(exists(conn, seq));

  // The leader died of SIGKILL, not SIGTERM; the helper went with it.
  auto const status = command.reap_leader();
  REQUIRE(status.has_value());
  CHECK(WIFSIGNALED(*status));
  CHECK(WTERMSIG(*status) == SIGKILL);
  REQUIRE(command.wait_group_empty());

  // With the group empty, the next advance ends the entry as timed out, and
  // only then does the waiter get its turn.
  auto const ended = advance(conn, advance_by_b, clock, probe, fence.signaller());
  REQUIRE(ended.ended.size() == 1);
  CHECK(ended.ended[0].outcome == hq::history_outcome::timeout);
  CHECK_FALSE(exists(conn, seq));
  CHECK(hq::find_history(conn, seq).value()->outcome == hq::history_outcome::timeout);
  auto const turn = hq::poll(conn, waiter_poll, clock, probe);
  REQUIRE(turn.has_value());
  CHECK(turn->started);

  CHECK(fence.refused.empty());
}

TEST_CASE("terminate: a cancelled command that honours SIGTERM ends as cancelled without a kill",
          "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  fake_clock  clock;
  auto const  probe = hq::system_process_probe();
  auto const  host  = pid_ns::host_identity(pid_ns::native_identity_source());

  command_group    command(/*ignore_term=*/false, /*with_helper=*/false);
  fenced_signaller fence{.allowed = command.pgid()};
  auto             conn = open_store(scratch);
  auto const       seq  = orphaned_entry(conn, clock, command);

  hq::canceller const who{.vendor = "claude", .role = "operator", .pid = static_cast<std::int64_t>(::getpid())};
  auto                request = cancel_request(seq, who);
  request.host_id             = host;
  auto const begun            = begin(conn, request, clock, probe, fence.signaller());
  REQUIRE(begun.status == hq::begin_status::marked);
  CHECK(begun.sigterm->outcome == hq::signal_outcome::sent);

  auto const status = command.reap_leader();
  REQUIRE(status.has_value());
  CHECK(WIFSIGNALED(*status));
  CHECK(WTERMSIG(*status) == SIGTERM);
  REQUIRE(command.wait_group_empty());

  auto only        = advance_request_for(k_grace);
  only.host_id     = host;
  only.seq         = seq;
  auto const ended = advance(conn, only, clock, probe, fence.signaller());
  CHECK(ended.kills.empty());
  REQUIRE(ended.ended.size() == 1);
  CHECK(ended.ended[0].outcome == hq::history_outcome::cancelled);
  auto const row = hq::find_history(conn, seq).value();
  REQUIRE(row.has_value());
  CHECK(row->cancelled_by == who);
  CHECK(fence.delivered == std::vector<int>{SIGTERM});
  CHECK(fence.refused.empty());
}

// ---------------------------------------------------------------------------
// Task 7055 (reviewer caveat C2 of task 7003): one entry's failed end must
// not abort the advance and drop the kills it already sent.
// ---------------------------------------------------------------------------

TEST_CASE("terminate: one entry that cannot be ended does not stop the advance", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'610); // Still alive: needs the SIGKILL.
  host.add_group(9'620); // Empty by the time of the advance, but its history insert fails.
  host.add_group(9'630); // Empty: ends normally, after the failing one.
  auto const killed = running_entry(conn, 961, clock, host.probe(), 9'610);
  auto const broken = running_entry(conn, 962, clock, host.probe(), 9'620);
  auto const fine   = running_entry(conn, 963, clock, host.probe(), 9'630);
  recorder   rec;
  for (auto const seq : {killed, broken, fine}) {
    REQUIRE(begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller()).status == hq::begin_status::marked);
  }
  host.groups_with_members.erase(9'620);
  host.groups_with_members.erase(9'630);
  REQUIRE(conn.execute(std::format("create trigger refuse_history before insert on queue_history when new.seq = {} "
                                   "begin select raise(abort, 'history refused'); end;",
                                   broken))
              .has_value());
  clock.mono += k_grace + 1;

  auto const result = hq::advance_terminations(conn, advance_request_for(), clock, host.probe(), rec.signaller());
  REQUIRE(result.has_value());
  // The kill sent before the failure is still reported.
  REQUIRE(result->kills.size() == 1);
  CHECK(result->kills[0].seq == killed);
  CHECK(rec.count(SIGKILL) == 1);
  // The entry after the failing one was still examined and ended.
  REQUIRE(result->ended.size() == 1);
  CHECK(result->ended[0].seq == fine);
  CHECK_FALSE(exists(conn, fine));
  // The failing entry is left in place for a later call, and named.
  CHECK(exists(conn, broken));
  REQUIRE(result->end_failures.size() == 1);
  CHECK(result->end_failures[0].seq == broken);
  CHECK(result->end_failures[0].error.message.contains("history refused"));
}

TEST_CASE("terminate: a cancelled entry with no readable canceller ends as abandoned", "[engine][hostqueue][hq-terminate]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'710);
  host.add_group(9'720);
  auto const missing   = running_entry(conn, 971, clock, host.probe(), 9'710);
  auto const malformed = running_entry(conn, 972, clock, host.probe(), 9'720);
  recorder   rec;
  for (auto const seq : {missing, malformed}) {
    REQUIRE(begin(conn, timeout_request(seq), clock, host.probe(), rec.signaller()).status == hq::begin_status::marked);
  }
  REQUIRE(conn.execute(std::format("update queue_entries set terminate_reason = 'cancelled', cancelled_by = null where seq = {}",
                                   missing))
              .has_value());
  REQUIRE(conn.execute(std::format("update queue_entries set terminate_reason = 'cancelled', cancelled_by = 'not json' "
                                   "where seq = {}",
                                   malformed))
              .has_value());
  host.groups_with_members.clear();

  auto const result = hq::advance_terminations(conn, advance_request_for(), clock, host.probe(), rec.signaller());
  REQUIRE(result.has_value());
  REQUIRE(result->ended.size() == 2);
  for (auto const& ended : result->ended) {
    CHECK(ended.outcome == hq::history_outcome::abandoned);
  }
  for (auto const seq : {missing, malformed}) {
    CHECK_FALSE(exists(conn, seq));
    auto const row = hq::find_history(conn, seq).value();
    REQUIRE(row.has_value());
    CHECK(row->outcome == hq::history_outcome::abandoned);
    CHECK_FALSE(row->cancelled_by.has_value());
  }
}

TEST_CASE("terminate: cancel_waiting removes a waiting entry with its canceller and refuses one that has started",
          "[engine][hostqueue][hq-queue-cancel]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'300);

  hq::canceller const who{.vendor = "claude", .role = "reviewer", .pid = 4'242};

  // A waiting entry is removed and leaves exactly one cancelled row.
  auto const waiting = enqueue_one(conn, request_for(930, clock));
  auto const removed = hq::cancel_waiting(conn, waiting, who, clock.wall.load());
  REQUIRE(removed.has_value());
  CHECK(removed->status == hq::cancel_waiting_status::removed);
  CHECK_FALSE(exists(conn, waiting));
  auto const row = hq::find_history(conn, waiting);
  REQUIRE(row.has_value());
  REQUIRE(row->has_value());
  CHECK((*row)->outcome == hq::history_outcome::cancelled);
  CHECK((*row)->cancelled_by == who);
  CHECK((*row)->ended_at == clock.wall.load());
  CHECK_FALSE(conn.in_transaction());

  // A second call finds nothing, writes nothing.
  auto const again = hq::cancel_waiting(conn, waiting, who, clock.wall.load());
  REQUIRE(again.has_value());
  CHECK(again->status == hq::cancel_waiting_status::missing);
  CHECK(hq::list_history(conn).value().size() == 1);

  // A number never issued is missing too.
  CHECK(hq::cancel_waiting(conn, waiting + 50, who, clock.wall.load()).value().status == hq::cancel_waiting_status::missing);

  // An entry that has started is left exactly as it was.
  auto const running = running_entry(conn, 931, clock, host.probe(), 9'300);
  auto const refused = hq::cancel_waiting(conn, running, who, clock.wall.load());
  REQUIRE(refused.has_value());
  CHECK(refused->status == hq::cancel_waiting_status::not_waiting);
  REQUIRE(refused->stored.has_value());
  CHECK(refused->stored->state == hq::entry_state::running);
  CHECK(exists(conn, running));
  CHECK_FALSE(entry_of(conn, running).terminating_since_mono.has_value());
  CHECK(hq::list_history(conn).value().size() == 1);

  // The engine's own cancellation of a running entry records the same
  // canceller on the entry, and its end carries it to the history row.
  recorder   rec;
  auto const begun = begin(conn, cancel_request(running, who), clock, host.probe(), rec.signaller());
  REQUIRE(begun.status == hq::begin_status::marked);
  CHECK(entry_of(conn, running).cancelled_by == hq::encode_canceller(who));
}

TEST_CASE("terminate: cancel_waiting refuses a connection that is already in a transaction",
          "[engine][hostqueue][hq-queue-cancel]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  auto const  seq = enqueue_one(conn, request_for(940, clock));
  REQUIRE(conn.execute("begin;").has_value());
  auto const refused = hq::cancel_waiting(conn, seq, hq::canceller{.pid = 1}, clock.wall.load());
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().kind == hq::queue_error_kind::invalid_request);
  REQUIRE(conn.execute("rollback;").has_value());
  CHECK(exists(conn, seq));
}

// ---------------------------------------------------------------------------
// Task 7072: EPERM on a group that holds only exited, unreaped processes
// ---------------------------------------------------------------------------

namespace {

// @brief A signaller that refuses every signal the way the kernel refuses a group it may not signal.
auto refusing_signaller(std::shared_ptr<int> calls) -> hq::group_signaller {
  return [calls](std::int64_t, int) -> std::expected<void, pid_ns::error> {
    ++*calls;
    return std::unexpected(pid_ns::error::not_permitted);
  };
}

} // namespace

TEST_CASE("terminate: EPERM on a group that only holds zombies is an empty group, not a failed signal",
          "[engine][hostqueue][hq-eperm-zombie-group]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'300);
  auto const seq = running_entry(conn, 931, clock, host.probe(), 9'300);
  auto const e   = entry_of(conn, seq);

  auto       probe = host.probe();
  auto const calls = std::make_shared<int>(0);

  SECTION("the probe verified that every member is a zombie") {
    probe.group_only_zombies = [](std::int64_t pgid) -> std::expected<bool, pid_ns::error> { return pgid == 9'300; };
    auto const attempt       = hq::signal_child_group(e, hq::stop_signal::kill, k_host, probe, refusing_signaller(calls));
    CHECK(*calls == 1); // the signal was still tried; only its refusal is reinterpreted
    CHECK(attempt.outcome == hq::signal_outcome::group_empty);
    CHECK_FALSE(attempt.error.has_value());
  }
  SECTION("the probe says a member is live: the refusal is real") {
    probe.group_only_zombies = [](std::int64_t) -> std::expected<bool, pid_ns::error> { return false; };
    auto const attempt       = hq::signal_child_group(e, hq::stop_signal::kill, k_host, probe, refusing_signaller(calls));
    CHECK(attempt.outcome == hq::signal_outcome::failed);
    REQUIRE(attempt.error.has_value());
    CHECK(*attempt.error == pid_ns::error::not_permitted);
  }
  SECTION("the probe cannot tell: the refusal is real") {
    probe.group_only_zombies = [](std::int64_t) -> std::expected<bool, pid_ns::error> {
      return std::unexpected(pid_ns::error::query_failed);
    };
    auto const attempt = hq::signal_child_group(e, hq::stop_signal::kill, k_host, probe, refusing_signaller(calls));
    CHECK(attempt.outcome == hq::signal_outcome::failed);
    REQUIRE(attempt.error.has_value());
    CHECK(*attempt.error == pid_ns::error::not_permitted);
  }
  SECTION("the probe has no such question (a fake that leaves it unset): the refusal is real") {
    REQUIRE_FALSE(static_cast<bool>(probe.group_only_zombies));
    auto const attempt = hq::signal_child_group(e, hq::stop_signal::kill, k_host, probe, refusing_signaller(calls));
    CHECK(attempt.outcome == hq::signal_outcome::failed);
  }
  SECTION("a failure that is not a refusal is never reinterpreted, whatever the probe says") {
    probe.group_only_zombies = [](std::int64_t) -> std::expected<bool, pid_ns::error> { return true; };
    auto const attempt       = hq::signal_child_group(
        e, hq::stop_signal::kill, k_host, probe,
        [](std::int64_t, int) -> std::expected<void, pid_ns::error> { return std::unexpected(pid_ns::error::query_failed); });
    CHECK(attempt.outcome == hq::signal_outcome::failed);
  }
}

TEST_CASE("terminate: the kill an advance sends to a group of zombies is not reported as failed",
          "[engine][hostqueue][hq-eperm-zombie-group]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add_group(9'310);
  auto const seq = running_entry(conn, 932, clock, host.probe(), 9'310);

  auto probe               = host.probe();
  probe.group_only_zombies = [](std::int64_t) -> std::expected<bool, pid_ns::error> { return true; };
  auto const calls         = std::make_shared<int>(0);
  auto const signaller     = refusing_signaller(calls);

  clock.mono += 1'000;
  REQUIRE(begin(conn, timeout_request(seq), clock, probe, signaller).status == hq::begin_status::marked);
  clock.mono += k_grace + 1;
  auto const advanced = advance(conn, advance_request_for(), clock, probe, signaller);
  REQUIRE(advanced.kills.size() == 1);
  CHECK(advanced.kills.front().outcome != hq::signal_outcome::failed);
  CHECK_FALSE(advanced.kills.front().error.has_value());
}

#if defined(__APPLE__)
TEST_CASE("terminate: on macOS a real group whose leader exited unreaped is empty enough, not a failed signal",
          "[engine][hostqueue][hq-eperm-zombie-group]") {
  // The EPERM comes from the kernel here, not from a fake signaller.
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;

  // The leader waits for a release byte, so its start time can be read while it
  // lives (the kernel will not tell it once the process is a zombie), and then
  // exits without anyone reaping it.
  std::array<int, 2> release{};
  REQUIRE(::pipe(release.data()) == 0);
  ::pid_t const forked = ::fork();
  REQUIRE(forked >= 0);
  if (forked == 0) {
    ::setpgid(0, 0);
    ::close(release[1]);
    char byte = 0;
    while (::read(release[0], &byte, 1) < 0 && errno == EINTR) {
    }
    ::_exit(0);
  }
  ::setpgid(forked, forked);
  ::close(release[0]);
  auto const pgid = static_cast<std::int64_t>(forked);

  auto const probe   = hq::system_process_probe();
  auto const leader  = probe.process_start_time(pgid);
  auto const seq     = enqueue_one(conn, request_for(::getpid(), clock));
  auto const started = hq::poll(conn, poll_request_for(seq, 64), clock, probe);
  REQUIRE(started.has_value());
  bool const have_start = leader.has_value() && leader->has_value();
  if (!have_start) {
    char const go = 'g';
    (void)::write(release[1], &go, 1);
    ::close(release[1]);
    int status = 0;
    while (::waitpid(forked, &status, 0) < 0 && errno == EINTR) {
    }
  }
  REQUIRE(have_start);
  record_child_group(conn, seq, pgid, static_cast<std::int64_t>(**leader));
  auto const e = entry_of(conn, seq);
  {
    char const go = 'g';
    (void)::write(release[1], &go, 1);
    ::close(release[1]);
  }

  // Wait, bounded, for the exit to become a zombie the kernel refuses to signal.
  hq::signal_attempt attempt{};
  auto const         deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    attempt = hq::signal_child_group(e, hq::stop_signal::kill, k_host, probe, hq::system_group_signaller());
    if (attempt.outcome != hq::signal_outcome::sent) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);
  bool const zombies = pid_ns::group_only_zombies(pgid).value_or(false);
  bool const members = pid_ns::group_has_members(pgid).value_or(false);
  int        status  = 0;
  while (::waitpid(forked, &status, 0) < 0 && errno == EINTR) {
  }
  INFO("outcome " << static_cast<int>(attempt.outcome) << " zombies " << zombies << " members " << members);
  CHECK(attempt.outcome == hq::signal_outcome::group_empty);
  CHECK_FALSE(attempt.error.has_value());
}
#endif
