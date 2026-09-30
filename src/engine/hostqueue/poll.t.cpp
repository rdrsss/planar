// @file poll.t.cpp
// @brief Unit tests for `planar.engine.hostqueue.poll` (plan 1080, task
// hq-poll-transaction). Covers the engine half of the test-spec scenarios
// that cite `task:hq-poll-transaction`:
//
//   * Empty -- the first entry in an empty queue has its turn at once
//   * Happy path -- entries are served in arrival order
//   * Edge -- two submitters cannot take the last slot
//   * Edge -- nested entries do not take slots
//   * Edge -- a busy store delays a poll and does not fail it
//   * Edge -- lowering the slot count stops nothing
//   * Edge -- a wall-clock step does not make an entry stale
//   * Edge -- a second submitter waits for the first
//
// plus the poll's own contract: a dead waiter is reaped with one `abandoned`
// history row in the poll's transaction, an overdue orphan is marked
// terminating and returned without being signalled, a running entry's start
// time and deadline are set once, an entry whose liveness query failed is
// returned and not reaped, and the monotonic clock is read only after the
// write lock is held (decision 1203).
//
// Every case opens its own scratch store in its own temp directory and
// drives process identity through a fake `process_probe` and time through a
// fake clock. Following the test spec's § Strategy, entries are seeded and
// read through the engine (`enqueue`, `poll`, `end_entry`, `find`, `list`,
// `find_history`). Two things have no engine operation yet and are written
// with raw SQL, each saying so where it happens: a running entry's child
// group (recorded by the `queue run` verb, task hq-queue-run-verb), and a
// trigger that makes a write fail, which is fault injection rather than
// seeding.
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
constexpr std::int64_t     k_mono0     = 5'000'000;
constexpr std::int64_t     k_wall0     = 1'759'000'000'000;

// @brief A unique scratch directory, removed when the guard leaves scope.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_hostqueue_poll_test_{}_{}_{}", ::getpid(),
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
// times), which groups have members, and which ids fail every query. It is
// read-only while polls run, so two threads may share it.
struct fake_host {
  std::map<std::int64_t, pid_ns::start_time> processes;
  std::set<std::int64_t>                     groups_with_members;
  std::set<std::int64_t>                     failing;

  [[nodiscard]] auto probe() const -> hq::process_probe {
    return hq::process_probe{
        .process_exists = [this](std::int64_t pid) -> std::expected<bool, pid_ns::error> {
          if (failing.contains(pid)) {
            return std::unexpected(pid_ns::error::query_failed);
          }
          return processes.contains(pid);
        },
        .process_start_time = [this](std::int64_t pid) -> std::expected<std::optional<pid_ns::start_time>, pid_ns::error> {
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
          if (failing.contains(pgid)) {
            return std::unexpected(pid_ns::error::query_failed);
          }
          return groups_with_members.contains(pgid);
        },
    };
  }

  // @brief Declares a live submitter `pid` whose start time is `pid * 10`.
  void add(std::int64_t pid) {
    processes[pid] = static_cast<pid_ns::start_time>(pid * 10);
  }
};

// @brief A request for a submitter `pid` on `k_host`, refreshed at `clock`'s
// current monotonic time.
auto request_for(std::int64_t pid, const fake_clock& clock, std::optional<std::int64_t> parent_seq = std::nullopt)
    -> hq::enqueue_request {
  return hq::enqueue_request{
      .host_id        = std::string(k_host),
      .pid            = pid,
      .pid_started    = pid * 10,
      .cwd            = "/work/planar",
      .argv           = {"make", "test"},
      .label          = std::format("pid {}", pid),
      .enqueued_at    = clock.wall.load(),
      .refreshed_mono = clock.mono.load(),
      .parent_seq     = parent_seq,
  };
}

auto enqueue_one(planar::db::connection& conn, const hq::enqueue_request& request) -> std::int64_t {
  auto seq = request.parent_seq ? hq::insert_nested_entry(conn, request) : hq::enqueue(conn, request);
  REQUIRE(seq.has_value());
  return *seq;
}

auto poll_request_for(std::int64_t seq, std::int64_t slots = 1) -> hq::poll_request {
  return hq::poll_request{
      .seq = seq, .host_id = std::string(k_host), .slots = slots, .stale_after_ms = k_window, .run_limit_ms = k_run_limit};
}

// @brief Polls as the submitter of `seq` and requires a completed poll.
auto poll_as(planar::db::connection& conn, std::int64_t seq, fake_clock& clock, const fake_host& host, std::int64_t slots = 1)
    -> hq::poll_result {
  auto result = hq::poll(conn, poll_request_for(seq, slots), clock, host.probe());
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

auto state_of(planar::db::connection& conn, std::int64_t seq) -> hq::entry_state {
  return entry_of(conn, seq).state;
}

auto exists(planar::db::connection& conn, std::int64_t seq) -> bool {
  auto found = hq::find(conn, seq);
  REQUIRE(found.has_value());
  return found->has_value();
}

// @brief Ends `seq` as a command that exited 0 at `ended_at`.
void end_exited(planar::db::connection& conn, std::int64_t seq, std::int64_t ended_at) {
  auto ended =
      hq::end_entry(conn, seq, hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = ended_at});
  REQUIRE(ended.has_value());
  REQUIRE(*ended == hq::end_result::ended);
}

auto running_count(planar::db::connection& conn) -> std::size_t {
  auto all = hq::list(conn);
  REQUIRE(all.has_value());
  return static_cast<std::size_t>(
      std::ranges::count_if(*all, [](const hq::entry& e) { return e.state == hq::entry_state::running; }));
}

// @brief Records a child group on a running entry. No engine operation
// writes `child_pgid` yet (the `queue run` verb records it, task
// hq-queue-run-verb), so this is raw SQL.
void record_child_group(planar::db::connection& conn, std::int64_t seq, std::int64_t pgid,
                        std::optional<std::int64_t> leader_started = std::nullopt) {
  auto const started = leader_started ? std::to_string(*leader_started) : std::string("null");
  REQUIRE(
      conn.execute(std::format("update queue_entries set child_pgid = {}, child_started = {} where seq = {}", pgid, started, seq))
          .has_value());
}

} // namespace

TEST_CASE("poll: the first entry in an empty queue has its turn at once", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(100);

  auto const seq = enqueue_one(conn, request_for(100, clock));
  clock.mono += 250;
  clock.wall += 7;

  auto const result = poll_as(conn, seq, clock, host);
  CHECK(result.running);
  CHECK(result.started);
  CHECK_FALSE(result.entry_missing);
  CHECK(result.now_mono == k_mono0 + 250);
  CHECK(result.started_at == k_wall0 + 7);
  CHECK(result.deadline_mono == k_mono0 + 250 + k_run_limit);
  CHECK(result.reaped.empty());
  CHECK(result.terminating.empty());
  CHECK(result.liveness_errors.empty());

  auto const stored = entry_of(conn, seq);
  CHECK(stored.state == hq::entry_state::running);
  CHECK(stored.started_at == k_wall0 + 7);
  CHECK(stored.deadline_mono == k_mono0 + 250 + k_run_limit);
  CHECK(stored.refreshed_mono == k_mono0 + 250);
  CHECK_FALSE(stored.terminating_since_mono.has_value());
}

TEST_CASE("poll: entries are served in arrival order", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(101);
  host.add(102);
  host.add(103);

  auto const first  = enqueue_one(conn, request_for(101, clock));
  auto const second = enqueue_one(conn, request_for(102, clock));
  auto const third  = enqueue_one(conn, request_for(103, clock));

  // The later arrivals poll first: polling early does not jump the queue.
  CHECK_FALSE(poll_as(conn, third, clock, host).running);
  CHECK_FALSE(poll_as(conn, second, clock, host).running);
  CHECK(poll_as(conn, first, clock, host).running);
  CHECK(state_of(conn, second) == hq::entry_state::waiting);
  CHECK(state_of(conn, third) == hq::entry_state::waiting);

  end_exited(conn, first, k_wall0 + 10);

  CHECK_FALSE(poll_as(conn, third, clock, host).running);
  CHECK(poll_as(conn, second, clock, host).running);
  CHECK_FALSE(poll_as(conn, third, clock, host).running);
  CHECK(state_of(conn, third) == hq::entry_state::waiting);
  CHECK(running_count(conn) == 1);
}

TEST_CASE("poll: two submitters cannot take the last slot", "[engine][hostqueue][hq-poll-transaction]") {
  // Two connections, each driven by its own thread, poll their own waiting
  // entries at once, with one slot. Each round starts both at one latch.
  scratch_dir scratch;
  auto        setup = open_store(scratch);
  auto        a     = open_store(scratch);
  auto        b     = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(201);
  host.add(202);

  constexpr int k_rounds = 200;
  int           a_won    = 0;
  int           b_won    = 0;
  for (int round = 0; round < k_rounds; ++round) {
    INFO("round " << round);
    auto const seq_a = enqueue_one(setup, request_for(201, clock));
    auto const seq_b = enqueue_one(setup, request_for(202, clock));

    std::latch                                      start{2};
    std::expected<hq::poll_result, hq::queue_error> result_a{std::unexpect};
    std::expected<hq::poll_result, hq::queue_error> result_b{std::unexpect};
    {
      std::jthread ta([&] {
        start.arrive_and_wait();
        result_a = hq::poll(a, poll_request_for(seq_a), clock, host.probe());
      });
      std::jthread tb([&] {
        start.arrive_and_wait();
        result_b = hq::poll(b, poll_request_for(seq_b), clock, host.probe());
      });
    }
    if (!result_a) {
      FAIL(result_a.error().message);
    }
    if (!result_b) {
      FAIL(result_b.error().message);
    }
    REQUIRE(result_a->status == hq::poll_status::completed);
    REQUIRE(result_b->status == hq::poll_status::completed);
    REQUIRE(running_count(setup) == 1);
    // The earlier arrival holds the turn whichever thread polled first.
    CHECK(result_a->running);
    CHECK_FALSE(result_b->running);
    a_won += result_a->started ? 1 : 0;
    b_won += result_b->started ? 1 : 0;

    // A second poll from the loser, now that the slot is taken, still waits.
    auto again = hq::poll(b, poll_request_for(seq_b), clock, host.probe());
    REQUIRE(again.has_value());
    CHECK_FALSE(again->running);
    REQUIRE(running_count(setup) == 1);

    end_exited(setup, seq_a, k_wall0);
    end_exited(setup, seq_b, k_wall0);
  }
  CHECK(a_won == k_rounds);
  CHECK(b_won == 0);
}

TEST_CASE("poll: two submitters racing for the one slot left never both start", "[engine][hostqueue][hq-poll-transaction]") {
  // Two slots, one taken by a running entry. The two racers are second and
  // third: only the second may start. Repeated, with the racers' threads
  // released together.
  scratch_dir scratch;
  auto        setup = open_store(scratch);
  auto        a     = open_store(scratch);
  auto        b     = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(210);
  host.add(211);
  host.add(212);

  auto const holder = enqueue_one(setup, request_for(210, clock));
  REQUIRE(poll_as(setup, holder, clock, host, 2).running);

  constexpr int k_rounds = 200;
  for (int round = 0; round < k_rounds; ++round) {
    INFO("round " << round);
    auto const                                      seq_a = enqueue_one(setup, request_for(211, clock));
    auto const                                      seq_b = enqueue_one(setup, request_for(212, clock));
    std::latch                                      start{2};
    std::expected<hq::poll_result, hq::queue_error> result_a{std::unexpect};
    std::expected<hq::poll_result, hq::queue_error> result_b{std::unexpect};
    {
      std::jthread tb([&] {
        start.arrive_and_wait();
        result_b = hq::poll(b, poll_request_for(seq_b, 2), clock, host.probe());
      });
      std::jthread ta([&] {
        start.arrive_and_wait();
        result_a = hq::poll(a, poll_request_for(seq_a, 2), clock, host.probe());
      });
    }
    REQUIRE(result_a.has_value());
    REQUIRE(result_b.has_value());
    REQUIRE(running_count(setup) == 2);
    CHECK(result_a->running);
    CHECK_FALSE(result_b->running);
    end_exited(setup, seq_a, k_wall0);
    end_exited(setup, seq_b, k_wall0);
  }
}

TEST_CASE("poll: nested entries do not take slots", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(301);
  host.add(302);
  host.add(303);

  auto const parent = enqueue_one(conn, request_for(301, clock));
  REQUIRE(poll_as(conn, parent, clock, host).running);
  auto const nested = enqueue_one(conn, request_for(302, clock, parent));
  REQUIRE(state_of(conn, nested) == hq::entry_state::running);
  auto const waiter = enqueue_one(conn, request_for(303, clock));

  CHECK_FALSE(poll_as(conn, waiter, clock, host).running);

  // The nested entry polls too; it is already running and stays so.
  auto const nested_poll = poll_as(conn, nested, clock, host);
  CHECK(nested_poll.running);
  CHECK_FALSE(nested_poll.started);

  end_exited(conn, parent, k_wall0 + 1);
  REQUIRE(state_of(conn, nested) == hq::entry_state::running);

  // The nested entry is still running and sits ahead of the waiter by
  // sequence number, but it is outside the slot count.
  auto const started = poll_as(conn, waiter, clock, host);
  CHECK(started.running);
  CHECK(started.started);
  CHECK(running_count(conn) == 2);
}

TEST_CASE("poll: a busy store delays a poll and does not fail it", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn  = open_store(scratch);
  auto        other = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(401);

  auto const seq    = enqueue_one(conn, request_for(401, clock));
  auto const before = entry_of(conn, seq);
  // A short busy timeout keeps the case quick; the rule is the same at any
  // length.
  REQUIRE(conn.execute("pragma busy_timeout = 50;").has_value());

  REQUIRE(other.execute("begin immediate;").has_value());
  clock.mono += 1'000;
  auto const skipped = hq::poll(conn, poll_request_for(seq), clock, host.probe());
  REQUIRE(skipped.has_value());
  CHECK(skipped->status == hq::poll_status::skipped);
  CHECK_FALSE(skipped->running);
  CHECK_FALSE(skipped->started);
  CHECK_FALSE(conn.in_transaction());
  REQUIRE(other.execute("rollback;").has_value());

  // The skipped poll changed nothing.
  auto const after_skip = entry_of(conn, seq);
  CHECK(after_skip.state == hq::entry_state::waiting);
  CHECK(after_skip.refreshed_mono == before.refreshed_mono);
  CHECK_FALSE(after_skip.started_at.has_value());

  auto const next = poll_as(conn, seq, clock, host);
  CHECK(next.running);
  CHECK(entry_of(conn, seq).refreshed_mono == k_mono0 + 1'000);
}

TEST_CASE("poll: lowering the slot count stops nothing", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(501);
  host.add(502);
  host.add(503);

  auto const first  = enqueue_one(conn, request_for(501, clock));
  auto const second = enqueue_one(conn, request_for(502, clock));
  REQUIRE(poll_as(conn, first, clock, host, 2).running);
  REQUIRE(poll_as(conn, second, clock, host, 2).running);
  auto const second_deadline = entry_of(conn, second).deadline_mono;
  auto const third           = enqueue_one(conn, request_for(503, clock));

  // The slot count drops to one. Both running entries poll with it and stay
  // running; the third does not start.
  clock.mono += 100;
  CHECK(poll_as(conn, first, clock, host, 1).running);
  auto const still = poll_as(conn, second, clock, host, 1);
  CHECK(still.running);
  CHECK(still.deadline_mono == second_deadline);
  CHECK_FALSE(poll_as(conn, third, clock, host, 1).running);
  CHECK(running_count(conn) == 2);

  end_exited(conn, first, k_wall0 + 1);
  CHECK_FALSE(poll_as(conn, third, clock, host, 1).running);

  end_exited(conn, second, k_wall0 + 2);
  CHECK(poll_as(conn, third, clock, host, 1).running);
}

TEST_CASE("poll: a wall-clock step does not make an entry stale", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(601);
  host.add(602);

  auto const holder = enqueue_one(conn, request_for(601, clock));
  REQUIRE(poll_as(conn, holder, clock, host).running);
  auto const waiter = enqueue_one(conn, request_for(602, clock));

  // The wall clock jumps an hour; the monotonic clock advances one second.
  clock.wall += 3'600'000;
  clock.mono += 1'000;
  auto const by_waiter = poll_as(conn, waiter, clock, host);
  CHECK(by_waiter.reaped.empty());
  CHECK_FALSE(by_waiter.running);
  CHECK(exists(conn, holder));
  CHECK(state_of(conn, holder) == hq::entry_state::running);

  // Contrast: only the monotonic clock ages an entry. The holder has not
  // refreshed since it started, and once a window passes on the monotonic
  // clock with the wall clock held still it is no longer live.
  clock.mono += k_window;
  auto const later = poll_as(conn, waiter, clock, host);
  CHECK(later.reaped == std::vector<std::int64_t>{holder});
  CHECK(later.running);
}

TEST_CASE("poll: a second submitter waits for the first", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(701);
  host.add(702);

  auto const first = enqueue_one(conn, request_for(701, clock));
  REQUIRE(poll_as(conn, first, clock, host).running);
  auto const second = enqueue_one(conn, request_for(702, clock));

  for (int i = 0; i < 5; ++i) {
    clock.mono += 1'000;
    clock.wall += 1'000;
    CHECK(poll_as(conn, first, clock, host).running);
    CHECK_FALSE(poll_as(conn, second, clock, host).running);
    CHECK(state_of(conn, second) == hq::entry_state::waiting);
  }

  clock.wall += 1'000;
  auto const first_ended_at = clock.wall.load();
  end_exited(conn, first, first_ended_at);
  clock.wall += 1'000;
  clock.mono += 1'000;
  REQUIRE(poll_as(conn, second, clock, host).running);
  end_exited(conn, second, clock.wall.load() + 1);

  auto const first_row  = hq::find_history(conn, first).value().value();
  auto const second_row = hq::find_history(conn, second).value().value();
  REQUIRE(second_row.started_at.has_value());
  CHECK(*second_row.started_at >= first_row.ended_at);
}

TEST_CASE("poll: a dead waiting submitter is reaped with one abandoned history row", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(802);

  // The first entry's submitter (pid 801) does not exist.
  auto const dead = enqueue_one(conn, request_for(801, clock));
  auto const live = enqueue_one(conn, request_for(802, clock));
  clock.wall += 55;

  auto const result = poll_as(conn, live, clock, host);
  CHECK(result.reaped == std::vector<std::int64_t>{dead});
  CHECK_FALSE(exists(conn, dead));
  // With the dead entry gone, the live one is first and takes the slot.
  CHECK(result.running);

  auto const rows = hq::list_history(conn).value();
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].seq == dead);
  CHECK(rows[0].outcome == hq::history_outcome::abandoned);
  CHECK(rows[0].ended_at == k_wall0 + 55);
  CHECK_FALSE(rows[0].started_at.has_value());

  // A later poll finds nothing more to reap and writes no second row.
  CHECK(poll_as(conn, live, clock, host).reaped.empty());
  CHECK(hq::list_history(conn).value().size() == 1);
}

TEST_CASE("poll: the reap, the marking and the turn commit together or not at all", "[engine][hostqueue][hq-poll-transaction]") {
  // Fault injection: a trigger makes the turn's state change fail. The reap
  // and the terminating mark that came before it in the same poll must roll
  // back with it.
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(851);
  host.add(853);
  host.groups_with_members = {9'100};

  // An orphan: running, past its deadline, submitter 851 about to vanish,
  // command group still has members.
  auto const orphan = enqueue_one(conn, request_for(851, clock));
  REQUIRE(poll_as(conn, orphan, clock, host, 2).running);
  record_child_group(conn, orphan, 9'100);
  host.processes.erase(851);

  auto const dead   = enqueue_one(conn, request_for(852, clock)); // pid 852 never existed
  auto const waiter = enqueue_one(conn, request_for(853, clock));
  clock.mono += k_run_limit + 1;
  // The waiter keeps itself fresh; the poll refreshes it anyway.

  REQUIRE(conn.execute("create temp trigger fail_turn before update of state on queue_entries "
                       "begin select raise(abort, 'injected turn failure'); end;")
              .has_value());
  auto const failed = hq::poll(conn, poll_request_for(waiter, 2), clock, host.probe());
  REQUIRE_FALSE(failed.has_value());
  CHECK_FALSE(conn.in_transaction());

  CHECK(exists(conn, dead));
  CHECK_FALSE(hq::find_history(conn, dead).value().has_value());
  CHECK_FALSE(entry_of(conn, orphan).terminating_since_mono.has_value());
  CHECK(state_of(conn, waiter) == hq::entry_state::waiting);
  CHECK(entry_of(conn, waiter).refreshed_mono == k_mono0);

  REQUIRE(conn.execute("drop trigger fail_turn;").has_value());
  auto const ok = poll_as(conn, waiter, clock, host, 2);
  CHECK(ok.reaped == std::vector<std::int64_t>{dead});
  REQUIRE(ok.terminating.size() == 1);
  CHECK(ok.terminating[0].seq == orphan);
  CHECK(ok.running);
}

TEST_CASE("poll: an overdue running entry whose submitter is gone is marked terminating and returned",
          "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(901);
  host.add(902);
  host.groups_with_members = {9'200};

  auto const orphan = enqueue_one(conn, request_for(901, clock));
  auto const first  = poll_as(conn, orphan, clock, host);
  REQUIRE(first.running);
  REQUIRE(first.deadline_mono == k_mono0 + k_run_limit);
  record_child_group(conn, orphan, 9'200);
  auto const waiter = enqueue_one(conn, request_for(902, clock));

  SECTION("not before the deadline has passed") {
    host.processes.erase(901);
    clock.mono    = k_mono0 + k_run_limit; // exactly at the deadline: not past it
    auto const at = poll_as(conn, waiter, clock, host);
    CHECK(at.terminating.empty());
    CHECK(at.reaped.empty());
    CHECK_FALSE(entry_of(conn, orphan).terminating_since_mono.has_value());
    CHECK_FALSE(at.running);
  }

  SECTION("not while its submitter is live") {
    clock.mono = k_mono0 + k_run_limit + 1;
    // The submitter refreshes its own entry, so it is live.
    REQUIRE(poll_as(conn, orphan, clock, host).running);
    auto const by_waiter = poll_as(conn, waiter, clock, host);
    CHECK(by_waiter.terminating.empty());
    CHECK_FALSE(entry_of(conn, orphan).terminating_since_mono.has_value());
  }

  SECTION("past the deadline with the submitter gone") {
    host.processes.erase(901);
    clock.mono      = k_mono0 + k_run_limit + 1;
    auto const past = poll_as(conn, waiter, clock, host);
    CHECK(past.reaped.empty()); // its command still runs, so it is live
    REQUIRE(past.terminating.size() == 1);
    CHECK(past.terminating[0].seq == orphan);
    CHECK(past.terminating[0].child_pgid == 9'200);
    CHECK(past.terminating[0].terminating_since_mono == k_mono0 + k_run_limit + 1);
    CHECK(past.terminating[0].terminate_reason == "timeout");

    auto const stored = entry_of(conn, orphan);
    CHECK(stored.state == hq::entry_state::running);
    CHECK(stored.terminating_since_mono == k_mono0 + k_run_limit + 1);
    CHECK(stored.terminate_reason == "timeout");
    // A terminating entry keeps its slot.
    CHECK_FALSE(past.running);

    // Already terminating: a later poll neither re-marks nor returns it.
    clock.mono += 5'000;
    auto const later = poll_as(conn, waiter, clock, host);
    CHECK(later.terminating.empty());
    CHECK(entry_of(conn, orphan).terminating_since_mono == k_mono0 + k_run_limit + 1);
    // Regression test: a terminating entry keeps its slot on later polls too.
    CHECK_FALSE(later.running);
  }
}

namespace {

// @brief A child that blocks reading a pipe until released, leading its own
// process group; reaped on every path. Copied from liveness.t.cpp.
class blocked_child {
public:
  blocked_child() {
    std::array<int, 2> fds{};
    if (::pipe(fds.data()) != 0) {
      throw std::runtime_error("pipe failed");
    }
    ::pid_t const forked = ::fork();
    if (forked < 0) {
      throw std::runtime_error("fork failed");
    }
    if (forked == 0) {
      ::setpgid(0, 0);
      ::close(fds[1]);
      char byte = 0;
      while (::read(fds[0], &byte, 1) < 0 && errno == EINTR) {
      }
      ::_exit(0);
    }
    ::setpgid(forked, forked);
    ::close(fds[0]);
    _pid     = forked;
    _release = fds[1];
  }
  blocked_child(const blocked_child&)                    = delete;
  auto operator=(const blocked_child&) -> blocked_child& = delete;
  blocked_child(blocked_child&&)                         = delete;
  auto operator=(blocked_child&&) -> blocked_child&      = delete;
  ~blocked_child() {
    (void)release();
  }

  [[nodiscard]] auto pid() const -> std::int64_t {
    return _pid;
  }

  // @brief Let the child exit and wait for it, for at most ten seconds.
  // A child still present at the deadline is killed and counts as unclean,
  // so a regression that stops the child fails the test instead of hanging
  // the suite.
  // @return Whether it exited normally with status 0: it was not killed by
  // a signal before it was released.
  auto release() -> bool {
    if (_release >= 0) {
      ::close(_release);
      _release = -1;
    }
    bool clean = false;
    if (_pid > 0) {
      int        status   = 0;
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      bool       reaped   = false;
      bool       gone     = false; // not our child any more: nothing to kill
      while (std::chrono::steady_clock::now() < deadline) {
        ::pid_t const got = ::waitpid(_pid, &status, WNOHANG);
        if (got == _pid) {
          reaped = true;
          break;
        }
        if (got < 0 && errno != EINTR) {
          gone = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (reaped) {
        clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      } else if (!gone) {
        ::kill(_pid, SIGKILL);
        while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
        }
      }
      _pid = 0;
    }
    return clean;
  }

private:
  ::pid_t _pid     = 0;
  int     _release = -1;
};

// @brief The pid of a child that has exited and been reaped.
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

} // namespace

TEST_CASE("poll: marking a real orphan terminating sends it no signal", "[engine][hostqueue][hq-poll-transaction]") {
  // Real processes through the system probe: the orphan's command is a
  // process group this test created, and its submitter is a pid that has
  // exited. After the poll marks it, the command must still be running.
  scratch_dir scratch;
  auto        conn  = open_store(scratch);
  auto const  probe = hq::system_process_probe();
  fake_clock  clock;

  blocked_child command;
  auto const    leader_started = pid_ns::process_start_time(command.pid()).value().value();
  auto const    self           = static_cast<std::int64_t>(::getpid());
  auto const    self_started   = pid_ns::process_start_time(self).value().value();

  auto orphan_request        = request_for(reaped_pid(), clock);
  orphan_request.pid_started = 1;
  auto const orphan          = enqueue_one(conn, orphan_request);
  // Starting the orphan's turn needs its submitter live; the poll's own
  // entry is never judged, so it polls as itself here.
  auto const started = hq::poll(conn, poll_request_for(orphan), clock, probe);
  REQUIRE(started.has_value());
  REQUIRE(started->running);
  record_child_group(conn, orphan, command.pid(), static_cast<std::int64_t>(leader_started));

  auto self_request        = request_for(self, clock);
  self_request.pid_started = static_cast<std::int64_t>(self_started);
  auto const caller        = enqueue_one(conn, self_request);

  clock.mono += k_run_limit + 1;
  auto const result = hq::poll(conn, poll_request_for(caller), clock, probe);
  REQUIRE(result.has_value());
  REQUIRE(result->status == hq::poll_status::completed);
  REQUIRE(result->terminating.size() == 1);
  CHECK(result->terminating[0].seq == orphan);
  CHECK(result->reaped.empty());

  // Had the poll signalled the group, the child would have died of it
  // rather than exiting 0 when released.
  CHECK(pid_ns::group_has_members(command.pid()).value());
  CHECK(command.release());
}

TEST_CASE("poll: a running entry's start time and deadline are set once", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(1'001);

  auto const seq   = enqueue_one(conn, request_for(1'001, clock));
  auto const first = poll_as(conn, seq, clock, host);
  REQUIRE(first.started);
  auto const started_at = entry_of(conn, seq).started_at;
  auto const deadline   = entry_of(conn, seq).deadline_mono;
  REQUIRE(started_at == k_wall0);
  REQUIRE(deadline == k_mono0 + k_run_limit);

  for (int i = 1; i <= 3; ++i) {
    clock.mono += 10'000;
    clock.wall += 10'000;
    auto const again = poll_as(conn, seq, clock, host);
    CHECK(again.running);
    CHECK_FALSE(again.started);
    CHECK(again.started_at == started_at);
    CHECK(again.deadline_mono == deadline);
    auto const stored = entry_of(conn, seq);
    CHECK(stored.started_at == started_at);
    CHECK(stored.deadline_mono == deadline);
    CHECK(stored.refreshed_mono == k_mono0 + i * 10'000);
  }
}

TEST_CASE("poll: an entry whose liveness query failed is returned, not reaped, and still holds its place",
          "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(1'102);
  host.failing = {1'101};

  // The first entry's submitter cannot be queried. It is not refreshed, so
  // were it judged it would be reaped as stale.
  auto const unknown = enqueue_one(conn, request_for(1'101, clock));
  auto const waiter  = enqueue_one(conn, request_for(1'102, clock));
  clock.mono += k_window + 1;

  auto const result = poll_as(conn, waiter, clock, host);
  REQUIRE(result.liveness_errors.size() == 1);
  CHECK(result.liveness_errors[0].seq == unknown);
  CHECK(result.liveness_errors[0].error == pid_ns::error::query_failed);
  CHECK(result.reaped.empty());
  CHECK(exists(conn, unknown));
  CHECK_FALSE(hq::find_history(conn, unknown).value().has_value());
  // It still counts toward the turn: the waiter behind it does not start.
  CHECK_FALSE(result.running);
}

TEST_CASE("poll: an overdue orphan whose liveness query failed is not marked", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(1'201);
  host.add(1'202);

  auto const orphan = enqueue_one(conn, request_for(1'201, clock));
  REQUIRE(poll_as(conn, orphan, clock, host).running);
  record_child_group(conn, orphan, 9'300);
  host.processes.erase(1'201);
  host.failing      = {9'300};
  auto const waiter = enqueue_one(conn, request_for(1'202, clock));

  clock.mono += k_run_limit + 1;
  auto const result = poll_as(conn, waiter, clock, host);
  REQUIRE(result.liveness_errors.size() == 1);
  CHECK(result.liveness_errors[0].seq == orphan);
  CHECK(result.terminating.empty());
  CHECK(result.reaped.empty());
  CHECK_FALSE(entry_of(conn, orphan).terminating_since_mono.has_value());
}

TEST_CASE("poll: the monotonic clock is read once, after the write lock is held", "[engine][hostqueue][hq-poll-transaction]") {
  // Decision 1203: at the moment the poll reads its monotonic clock, another
  // connection must already be unable to take the write lock.
  scratch_dir scratch;
  auto        conn  = open_store(scratch);
  auto        other = open_store(scratch);
  REQUIRE(other.execute("pragma busy_timeout = 0;").has_value());
  fake_clock clock;
  fake_host  host;
  host.add(1'301);
  auto const seq = enqueue_one(conn, request_for(1'301, clock));

  std::optional<bool> other_was_locked_out;
  clock.on_monotonic = [&] {
    auto const attempt   = other.execute("begin immediate;");
    other_was_locked_out = !attempt.has_value() && planar::db::is_busy(attempt.error());
    if (attempt) {
      REQUIRE(other.execute("rollback;").has_value());
    }
  };

  auto const result = poll_as(conn, seq, clock, host);
  CHECK(clock.monotonic_reads == 1);
  REQUIRE(other_was_locked_out.has_value());
  CHECK(*other_was_locked_out);
  CHECK(result.running);
}

TEST_CASE("poll: a missing own entry is reported and the rest of the poll still runs",
          "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;

  auto const dead   = enqueue_one(conn, request_for(1'401, clock)); // pid 1401 does not exist
  auto const result = poll_as(conn, dead + 100, clock, host);
  CHECK(result.entry_missing);
  CHECK_FALSE(result.running);
  CHECK_FALSE(result.started);
  CHECK(result.reaped == std::vector<std::int64_t>{dead});
  CHECK(hq::list(conn).value().empty());
}

TEST_CASE("poll: an invalid request changes nothing", "[engine][hostqueue][hq-poll-transaction]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(1'501);
  auto const seq = enqueue_one(conn, request_for(1'501, clock));

  auto negative_window           = poll_request_for(seq);
  negative_window.stale_after_ms = -1;
  auto negative_limit            = poll_request_for(seq);
  negative_limit.run_limit_ms    = -1;
  for (auto const& request : {poll_request_for(seq, -1), negative_window, negative_limit}) {
    auto const refused = hq::poll(conn, request, clock, host.probe());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == hq::queue_error_kind::invalid_request);
  }

  {
    auto txn = conn.begin_transaction();
    REQUIRE(txn.has_value());
    auto const refused = hq::poll(conn, poll_request_for(seq), clock, host.probe());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == hq::queue_error_kind::invalid_request);
  }
  CHECK(clock.monotonic_reads == 0);
  CHECK(state_of(conn, seq) == hq::entry_state::waiting);
}

TEST_CASE("poll: the start records the run limit its deadline was computed from, once",
          "[engine][hostqueue][hq-poll-transaction][hq-queue-limit-columns]") {
  // Task hq-queue-limit-columns: `queue status` reports `run_limit_ms` from
  // the entry, so the poll that starts an entry records the limit in the same
  // statement that sets the deadline, and later polls leave both alone.
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(1'001);
  host.add(1'002);

  auto const seq    = enqueue_one(conn, request_for(1'001, clock));
  auto const behind = enqueue_one(conn, request_for(1'002, clock));
  CHECK_FALSE(entry_of(conn, seq).run_limit_ms.has_value()); // waiting: not yet in force

  auto const first = poll_as(conn, seq, clock, host);
  REQUIRE(first.started);
  auto const stored = entry_of(conn, seq);
  CHECK(stored.run_limit_ms == k_run_limit);
  CHECK(stored.deadline_mono == k_mono0 + k_run_limit);

  // A later poll by the same submitter carrying another limit changes neither.
  clock.mono += 1'000;
  auto other         = poll_request_for(seq, 1);
  other.run_limit_ms = 7'000;
  auto const again   = hq::poll(conn, other, clock, host.probe());
  REQUIRE(again.has_value());
  CHECK(entry_of(conn, seq).run_limit_ms == k_run_limit);
  CHECK(entry_of(conn, seq).deadline_mono == k_mono0 + k_run_limit);

  // The entry behind it has no turn, so nothing is recorded on it.
  auto waiting         = poll_request_for(behind, 1);
  waiting.run_limit_ms = 7'000;
  REQUIRE(hq::poll(conn, waiting, clock, host.probe()).has_value());
  CHECK(entry_of(conn, behind).state == hq::entry_state::waiting);
  CHECK_FALSE(entry_of(conn, behind).run_limit_ms.has_value());
}
