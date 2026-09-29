// @file nested.t.cpp
// @brief Unit tests for `planar.engine.hostqueue.nested` (plan 1080, task
// hq-nested-entry). Covers the engine half of the test-spec scenarios that
// cite `task:hq-nested-entry`:
//
//   * Happy path -- a nested entry is recorded with its parent
//   * Edge -- nested entries do not take slots
//   * Edge -- a nested run inside a nested run
//
// plus decision 1191's rule that the slot marker is honoured only when it
// names a live running entry: a missing, waiting, dead or unjudgeable parent
// is refused with `queue_normally` and nothing is inserted, and a parent's
// liveness is judged through the injected process probe. The nested entry
// runs from its insertion, with a start time and a deadline the poll
// enforces like any other entry's.
//
// Every case opens its own scratch store in its own temp directory and
// drives process identity through a fake `process_probe` and time through a
// fake clock. Entries are seeded and read through the engine (`enqueue`,
// `poll`, `enqueue_nested`, `end_entry`, `find`, `list`, `find_history`). A
// running entry's child group has no engine writer yet (the `queue run` verb
// records it, task hq-queue-run-verb), so it is written with raw SQL where a
// case needs one.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

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
constexpr std::int64_t     k_mono0     = 7'000'000;
constexpr std::int64_t     k_wall0     = 1'759'100'000'000;

// @brief A unique scratch directory, removed when the guard leaves scope.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_hostqueue_nested_test_{}_{}_{}", ::getpid(),
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
  std::int64_t          mono = k_mono0;
  std::int64_t          wall = k_wall0;
  int                   monotonic_reads{0};
  std::function<void()> on_monotonic;

  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, pid_ns::error> override {
    ++monotonic_reads;
    if (on_monotonic) {
      on_monotonic();
    }
    return mono;
  }

  [[nodiscard]] auto wall_ms() -> std::int64_t override {
    return wall;
  }
};

// @brief A host the test describes: which pids exist (with their start
// times), which groups have members, and which ids fail every query.
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
auto request_for(std::int64_t pid, const fake_clock& clock) -> hq::enqueue_request {
  return hq::enqueue_request{
      .host_id        = std::string(k_host),
      .pid            = pid,
      .pid_started    = pid * 10,
      .cwd            = "/work/planar",
      .argv           = {"make", "test"},
      .label          = std::format("pid {}", pid),
      .enqueued_at    = clock.wall,
      .refreshed_mono = clock.mono,
  };
}

constexpr hq::nested_limits k_limits{.stale_after_ms = k_window, .run_limit_ms = k_run_limit};

auto enqueue_one(planar::db::connection& conn, const hq::enqueue_request& request) -> std::int64_t {
  auto seq = hq::enqueue(conn, request);
  REQUIRE(seq.has_value());
  return *seq;
}

// @brief Polls as the submitter of `seq` with one slot and requires a
// completed poll.
auto poll_as(planar::db::connection& conn, std::int64_t seq, fake_clock& clock, const fake_host& host) -> hq::poll_result {
  auto result = hq::poll(
      conn,
      hq::poll_request{
          .seq = seq, .host_id = std::string(k_host), .slots = 1, .stale_after_ms = k_window, .run_limit_ms = k_run_limit},
      clock, host.probe());
  if (!result) {
    FAIL(result.error().message);
  }
  REQUIRE(result->status == hq::poll_status::completed);
  return *result;
}

// @brief An entry that has polled its way to `running` for submitter `pid`.
auto running_entry(planar::db::connection& conn, std::int64_t pid, fake_clock& clock, const fake_host& host) -> std::int64_t {
  auto const seq = enqueue_one(conn, request_for(pid, clock));
  REQUIRE(poll_as(conn, seq, clock, host).running);
  return seq;
}

// @brief Inserts a nested entry for submitter `pid` under `parent` and
// returns the result, which must not be an error.
auto nest(planar::db::connection& conn, std::int64_t parent, std::int64_t pid, fake_clock& clock, const fake_host& host)
    -> hq::nested_result {
  auto result = hq::enqueue_nested(conn, parent, request_for(pid, clock), k_limits, clock, host.probe());
  if (!result) {
    FAIL(result.error().message);
  }
  return *result;
}

// @brief Inserts a nested entry that must be accepted, and returns its
// sequence number.
auto nest_ok(planar::db::connection& conn, std::int64_t parent, std::int64_t pid, fake_clock& clock, const fake_host& host)
    -> std::int64_t {
  auto const result = nest(conn, parent, pid, clock, host);
  REQUIRE(result.status == hq::nested_status::inserted);
  REQUIRE_FALSE(result.refusal.has_value());
  return result.seq;
}

// @brief The entry `seq`, which must exist.
auto entry_of(planar::db::connection& conn, std::int64_t seq) -> hq::entry {
  auto found = hq::find(conn, seq);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  return **found;
}

auto all_entries(planar::db::connection& conn) -> std::vector<hq::entry> {
  auto all = hq::list(conn);
  REQUIRE(all.has_value());
  return *all;
}

auto running_count(planar::db::connection& conn) -> std::size_t {
  auto const all = all_entries(conn);
  return static_cast<std::size_t>(
      std::ranges::count_if(all, [](const hq::entry& e) { return e.state == hq::entry_state::running; }));
}

// @brief Ends `seq` as a command that exited 0 at `ended_at`.
void end_exited(planar::db::connection& conn, std::int64_t seq, std::int64_t ended_at) {
  auto ended =
      hq::end_entry(conn, seq, hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = ended_at});
  REQUIRE(ended.has_value());
  REQUIRE(*ended == hq::end_result::ended);
}

// @brief The history row of `seq`, which must exist.
auto history_of(planar::db::connection& conn, std::int64_t seq) -> hq::history_row {
  auto row = hq::find_history(conn, seq);
  REQUIRE(row.has_value());
  REQUIRE(row->has_value());
  return **row;
}

// @brief Records a child group on a running entry (raw SQL; see the file
// comment).
void record_child_group(planar::db::connection& conn, std::int64_t seq, std::int64_t pgid) {
  REQUIRE(conn.execute(std::format("update queue_entries set child_pgid = {} where seq = {}", pgid, seq)).has_value());
}

// @brief Requires a refusal with `reason` and that the store is unchanged.
void require_refused(planar::db::connection& conn, std::int64_t parent, std::int64_t pid, fake_clock& clock,
                     const fake_host& host, hq::nested_refusal reason) {
  auto const before = all_entries(conn);
  auto const result = nest(conn, parent, pid, clock, host);
  CHECK(result.status == hq::nested_status::queue_normally);
  REQUIRE(result.refusal.has_value());
  CHECK(*result.refusal == reason);
  CHECK_FALSE(result.started_at.has_value());
  CHECK_FALSE(result.deadline_mono.has_value());
  auto const after = all_entries(conn);
  REQUIRE(after.size() == before.size());
  for (std::size_t i = 0; i < after.size(); ++i) {
    CHECK(after[i].seq == before[i].seq);
    CHECK(after[i].pid != pid);
  }
}

} // namespace

TEST_CASE("nested: a nested entry is recorded with its parent", "[engine][hostqueue][hq-nested-entry]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(101);
  host.add(102);

  auto const parent = running_entry(conn, 101, clock, host);
  clock.mono += 400;
  clock.wall += 9;

  auto const result = nest(conn, parent, 102, clock, host);
  REQUIRE(result.status == hq::nested_status::inserted);
  CHECK_FALSE(result.refusal.has_value());
  CHECK(result.seq > parent);
  CHECK(result.now_mono == k_mono0 + 400);
  CHECK(result.started_at == k_wall0 + 9);
  CHECK(result.deadline_mono == k_mono0 + 400 + k_run_limit);

  auto const stored = entry_of(conn, result.seq);
  CHECK(stored.state == hq::entry_state::running);
  CHECK(stored.parent_seq == parent);
  CHECK(stored.pid == 102);
  CHECK(stored.started_at == k_wall0 + 9);
  CHECK(stored.deadline_mono == k_mono0 + 400 + k_run_limit);
  CHECK(stored.refreshed_mono == k_mono0 + 400);
  CHECK(stored.argv == std::vector<std::string>{"make", "test"});
  // The parent is untouched.
  CHECK_FALSE(entry_of(conn, parent).parent_seq.has_value());

  clock.wall += 50;
  end_exited(conn, result.seq, clock.wall);
  auto const nested_row = history_of(conn, result.seq);
  CHECK(nested_row.nested);
  CHECK(nested_row.parent_seq == parent);
  CHECK(nested_row.started_at == k_wall0 + 9);
  CHECK(nested_row.ran_ms == 50);

  end_exited(conn, parent, clock.wall);
  auto const parent_row = history_of(conn, parent);
  CHECK_FALSE(parent_row.nested);
  CHECK_FALSE(parent_row.parent_seq.has_value());
}

TEST_CASE("nested: nested entries do not take slots", "[engine][hostqueue][hq-nested-entry]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(201);
  host.add(202);
  host.add(203);

  auto const parent = running_entry(conn, 201, clock, host);
  auto const nested = nest_ok(conn, parent, 202, clock, host);
  auto const waiter = enqueue_one(conn, request_for(203, clock));

  // One slot, held by the parent: the waiter does not start.
  CHECK_FALSE(poll_as(conn, waiter, clock, host).running);
  CHECK(entry_of(conn, waiter).state == hq::entry_state::waiting);

  end_exited(conn, parent, k_wall0 + 1);
  REQUIRE(entry_of(conn, nested).state == hq::entry_state::running);

  // The nested entry is still running and is ahead of the waiter by sequence
  // number, but it is outside the slot count and arrival order.
  auto const started = poll_as(conn, waiter, clock, host);
  CHECK(started.running);
  CHECK(started.started);
  CHECK(running_count(conn) == 2);
}

TEST_CASE("nested: a nested run inside a nested run", "[engine][hostqueue][hq-nested-entry]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(301);
  host.add(302);
  host.add(303);
  host.add(304);

  auto const outer  = running_entry(conn, 301, clock, host);
  auto const middle = nest_ok(conn, outer, 302, clock, host);
  auto const inner  = nest_ok(conn, middle, 303, clock, host);
  CHECK(entry_of(conn, middle).parent_seq == outer);
  CHECK(entry_of(conn, inner).parent_seq == middle);
  CHECK(entry_of(conn, inner).state == hq::entry_state::running);

  // Three running entries and one slot: a waiter still counts only the outer.
  auto const waiter = enqueue_one(conn, request_for(304, clock));
  CHECK_FALSE(poll_as(conn, waiter, clock, host).running);

  end_exited(conn, inner, k_wall0 + 3);
  end_exited(conn, middle, k_wall0 + 4);
  end_exited(conn, outer, k_wall0 + 5);

  auto const outer_row  = history_of(conn, outer);
  auto const middle_row = history_of(conn, middle);
  auto const inner_row  = history_of(conn, inner);
  CHECK_FALSE(outer_row.nested);
  CHECK(middle_row.nested);
  CHECK(middle_row.parent_seq == outer);
  CHECK(inner_row.nested);
  CHECK(inner_row.parent_seq == middle);

  CHECK(poll_as(conn, waiter, clock, host).started);
}

TEST_CASE("nested: a missing parent is refused and nothing is inserted", "[engine][hostqueue][hq-nested-entry]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(401);
  host.add(402);

  SECTION("a sequence number that never existed") {
    require_refused(conn, 9'999, 402, clock, host, hq::nested_refusal::parent_missing);
    CHECK(all_entries(conn).empty());
  }

  SECTION("a parent that has ended") {
    auto const parent = running_entry(conn, 401, clock, host);
    end_exited(conn, parent, k_wall0 + 1);
    require_refused(conn, parent, 402, clock, host, hq::nested_refusal::parent_missing);
    CHECK(all_entries(conn).empty());
  }
}

TEST_CASE("nested: a waiting parent is refused and nothing is inserted", "[engine][hostqueue][hq-nested-entry]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(501);
  host.add(502);
  host.add(503);

  auto const holder = running_entry(conn, 501, clock, host);
  auto const parent = enqueue_one(conn, request_for(502, clock));
  REQUIRE_FALSE(poll_as(conn, parent, clock, host).running);

  require_refused(conn, parent, 503, clock, host, hq::nested_refusal::parent_waiting);
  CHECK(entry_of(conn, holder).state == hq::entry_state::running);
  CHECK(entry_of(conn, parent).state == hq::entry_state::waiting);
}

TEST_CASE("nested: a running parent that is not live is refused and nothing is inserted",
          "[engine][hostqueue][hq-nested-entry]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(601);
  host.add(602);

  auto const parent = running_entry(conn, 601, clock, host);

  SECTION("its submitter is gone and it records no child group") {
    host.processes.erase(601);
    require_refused(conn, parent, 602, clock, host, hq::nested_refusal::parent_not_live);
  }

  SECTION("its submitter's pid was reused by another process") {
    host.processes[601] = 1;
    require_refused(conn, parent, 602, clock, host, hq::nested_refusal::parent_not_live);
  }

  SECTION("its entry is stale and its child group is empty") {
    record_child_group(conn, parent, 6'100);
    clock.mono += k_window + 1;
    require_refused(conn, parent, 602, clock, host, hq::nested_refusal::parent_not_live);
  }

  // The refusal judges the parent; reaping it is the poll's job.
  CHECK(entry_of(conn, parent).state == hq::entry_state::running);
}

TEST_CASE("nested: a running parent whose command outlives its submitter is accepted", "[engine][hostqueue][hq-nested-entry]") {
  // Liveness, not the stored state alone, decides: a running entry whose
  // submitter is gone is live while its child group has a member.
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(701);
  host.add(702);

  auto const parent = running_entry(conn, 701, clock, host);
  record_child_group(conn, parent, 7'100);
  host.processes.erase(701);
  host.groups_with_members.insert(7'100);
  clock.mono += k_window + 1;

  auto const nested = nest_ok(conn, parent, 702, clock, host);
  CHECK(entry_of(conn, nested).parent_seq == parent);
}

TEST_CASE("nested: a parent whose liveness query fails is refused and nothing is inserted",
          "[engine][hostqueue][hq-nested-entry]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(801);
  host.add(802);

  auto const parent = running_entry(conn, 801, clock, host);
  host.failing.insert(801);
  require_refused(conn, parent, 802, clock, host, hq::nested_refusal::parent_unjudged);
}

TEST_CASE("nested: a nested entry has a deadline the poll enforces", "[engine][hostqueue][hq-nested-entry]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(901);
  host.add(902);
  host.add(903);

  auto const parent = running_entry(conn, 901, clock, host);
  clock.mono += 1'000;
  auto const nested = nest_ok(conn, parent, 902, clock, host);
  REQUIRE(entry_of(conn, nested).deadline_mono == k_mono0 + 1'000 + k_run_limit);
  // The parent ends; its nested run outlives it.
  end_exited(conn, parent, k_wall0 + 1);

  // The nested submitter is killed while its command runs on.
  record_child_group(conn, nested, 9'200);
  host.groups_with_members.insert(9'200);
  host.processes.erase(902);
  auto const waiter = enqueue_one(conn, request_for(903, clock));

  clock.mono    = k_mono0 + 1'000 + k_run_limit;
  auto const at = poll_as(conn, waiter, clock, host);
  CHECK(at.terminating.empty());

  clock.mono      = k_mono0 + 1'000 + k_run_limit + 1;
  auto const past = poll_as(conn, waiter, clock, host);
  REQUIRE(past.terminating.size() == 1);
  CHECK(past.terminating[0].seq == nested);
  CHECK(past.terminating[0].terminate_reason == "timeout");
}

TEST_CASE("nested: the monotonic clock is read once, after the write lock is held", "[engine][hostqueue][hq-nested-entry]") {
  scratch_dir scratch;
  auto        conn  = open_store(scratch);
  auto        other = open_store(scratch);
  REQUIRE(other.execute("pragma busy_timeout = 0;").has_value());
  fake_clock clock;
  fake_host  host;
  host.add(1'001);
  host.add(1'002);
  auto const parent     = running_entry(conn, 1'001, clock, host);
  clock.monotonic_reads = 0;

  std::optional<bool> other_was_locked_out;
  clock.on_monotonic = [&] {
    auto const attempt   = other.execute("begin immediate;");
    other_was_locked_out = !attempt.has_value() && planar::db::is_busy(attempt.error());
    if (attempt) {
      REQUIRE(other.execute("rollback;").has_value());
    }
  };

  nest_ok(conn, parent, 1'002, clock, host);
  CHECK(clock.monotonic_reads == 1);
  REQUIRE(other_was_locked_out.has_value());
  CHECK(*other_was_locked_out);
}

TEST_CASE("nested: an invalid request changes nothing", "[engine][hostqueue][hq-nested-entry]") {
  scratch_dir scratch;
  auto        conn = open_store(scratch);
  fake_clock  clock;
  fake_host   host;
  host.add(1'101);
  host.add(1'102);
  auto const parent     = running_entry(conn, 1'101, clock, host);
  clock.monotonic_reads = 0;

  auto const refuse = [&](const hq::enqueue_request& request, const hq::nested_limits& limits) {
    auto const refused = hq::enqueue_nested(conn, parent, request, limits, clock, host.probe());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == hq::queue_error_kind::invalid_request);
  };

  refuse(request_for(1'102, clock), hq::nested_limits{.stale_after_ms = -1, .run_limit_ms = k_run_limit});
  refuse(request_for(1'102, clock), hq::nested_limits{.stale_after_ms = k_window, .run_limit_ms = -1});
  auto conflicting       = request_for(1'102, clock);
  conflicting.parent_seq = parent + 1;
  refuse(conflicting, k_limits);
  {
    auto txn = conn.begin_transaction();
    REQUIRE(txn.has_value());
    refuse(request_for(1'102, clock), k_limits);
  }

  CHECK(clock.monotonic_reads == 0);
  CHECK(all_entries(conn).size() == 1);

  // The same parent named on the request is not a conflict.
  auto agreeing       = request_for(1'102, clock);
  agreeing.parent_seq = parent;
  auto const accepted = hq::enqueue_nested(conn, parent, agreeing, k_limits, clock, host.probe());
  REQUIRE(accepted.has_value());
  CHECK(accepted->status == hq::nested_status::inserted);
}
