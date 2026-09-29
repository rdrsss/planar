// @file liveness.t.cpp
// @brief Unit tests for `planar.engine.hostqueue.liveness` (plan 1080, task
// hq-liveness). Covers the liveness half of the test-spec scenarios that
// cite `task:hq-liveness`:
//
//   * Happy path -- a dead waiting submitter is reaped
//   * Edge -- a reused process id is not mistaken for the submitter
//   * Edge -- a running entry whose submitter is gone stays live while its
//     command runs
//   * Edge -- a running entry with a stopped submitter stays live while its
//     command runs
//   * Edge -- a reused process-group id is not mistaken for the command
//   * Error -- a running entry with no submitter and an empty group is reaped
//   * Edge -- an entry from another host identity is judged by freshness alone
//   * Error -- a live entry is never reaped
//   * Edge -- a killed submitter does not free the slot while its command runs
//   * Edge -- a killed waiter does not hold up the queue
//
// "Reaped" here is selection by `select_not_live`; deleting the entry and
// writing its history row belong to the poll transaction and hq-history.
//
// Most cases drive a fake `process_probe` whose processes and groups the
// test declares, so every branch is deterministic. The fake fails the test
// on any query about a process the test did not declare. The last cases
// use `system_process_probe()` against children this file forks and reaps
// itself, which proves the wiring to `planar.process.identity`.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

import std;
import planar.process.identity;
import planar.engine.hostqueue;

namespace {

namespace hq     = planar::engine::hostqueue;
namespace pid_ns = planar::process::identity;

constexpr std::string_view k_host   = "host-a";
constexpr std::int64_t     k_now    = 1'000'000;
constexpr std::int64_t     k_window = 30'000;

// @brief A host the test describes: which pids exist with which start
// times, and which groups have members. Every query is counted.
struct fake_host {
  std::map<std::int64_t, pid_ns::start_time> processes; // pid -> start time
  std::set<std::int64_t>                     groups_with_members;
  std::set<std::int64_t>                     failing; // ids whose every query fails
  int                                        calls = 0;

  [[nodiscard]] auto probe() -> hq::process_probe {
    return hq::process_probe{
        .process_exists = [this](std::int64_t pid) -> std::expected<bool, pid_ns::error> {
          ++calls;
          if (failing.contains(pid)) {
            return std::unexpected(pid_ns::error::query_failed);
          }
          return processes.contains(pid);
        },
        .process_start_time = [this](std::int64_t pid) -> std::expected<std::optional<pid_ns::start_time>, pid_ns::error> {
          ++calls;
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
          ++calls;
          if (failing.contains(pgid)) {
            return std::unexpected(pid_ns::error::query_failed);
          }
          return groups_with_members.contains(pgid);
        },
    };
  }
};

auto context(std::string_view host = k_host) -> hq::liveness_context {
  return hq::liveness_context{.host_id = std::string(host), .now_mono = k_now, .stale_after_ms = k_window};
}

// @brief A waiting entry owned by `pid` started at `started`, refreshed
// `age_ms` before `k_now`, recorded on `host`.
auto waiting(std::int64_t seq, std::int64_t pid, std::int64_t started, std::int64_t age_ms = 0, std::string_view host = k_host)
    -> hq::entry {
  hq::entry e;
  e.seq            = seq;
  e.state          = hq::entry_state::waiting;
  e.host_id        = std::string(host);
  e.pid            = pid;
  e.pid_started    = started;
  e.refreshed_mono = k_now - age_ms;
  return e;
}

// @brief A running entry: `waiting(...)` plus a child group and the start
// time of its leader.
auto running(std::int64_t seq, std::int64_t pid, std::int64_t started, std::int64_t pgid, std::int64_t leader_started,
             std::int64_t age_ms = 0, std::string_view host = k_host) -> hq::entry {
  hq::entry e     = waiting(seq, pid, started, age_ms, host);
  e.state         = hq::entry_state::running;
  e.child_pgid    = pgid;
  e.child_started = leader_started;
  return e;
}

auto judge(const hq::entry& e, fake_host& host, const hq::liveness_context& ctx = context()) -> hq::liveness {
  auto verdict = hq::judge_liveness(e, ctx, host.probe());
  REQUIRE(verdict.has_value());
  return *verdict;
}

auto select(std::span<hq::entry const> entries, fake_host& host, const hq::liveness_context& ctx = context())
    -> std::vector<std::int64_t> {
  return hq::select_not_live(entries, ctx, host.probe());
}

} // namespace

TEST_CASE("liveness: a dead waiting submitter is reaped and the live one behind it is not", "[engine][hostqueue][liveness]") {
  fake_host host;
  host.processes = {{200, 7}};

  std::array const entries{waiting(1, 100, 5), waiting(2, 200, 7)};

  auto const dead = judge(entries[0], host);
  CHECK_FALSE(dead.live);
  CHECK(dead.submitter_gone());
  CHECK(select(entries, host) == std::vector<std::int64_t>{1});
}

TEST_CASE("liveness: a reused process id is not mistaken for the submitter", "[engine][hostqueue][liveness]") {
  fake_host host;
  host.processes = {{100, 99}}; // pid 100 exists, but it is a later process

  std::array const entries{waiting(1, 100, 5)};

  auto const verdict = judge(entries[0], host);
  CHECK_FALSE(verdict.live);
  CHECK(verdict.submitter_gone());
  CHECK(select(entries, host) == std::vector<std::int64_t>{1});
}

TEST_CASE("liveness: a live entry is never reaped", "[engine][hostqueue][liveness]") {
  fake_host host;
  host.processes           = {{100, 5}, {300, 11}};
  host.groups_with_members = {300};

  std::array const entries{waiting(1, 100, 5), running(2, 100, 5, 300, 11)};

  auto const w = judge(entries[0], host);
  CHECK(w.live);
  CHECK(w.submitter_live);
  CHECK_FALSE(w.submitter_gone());
  CHECK(w.group == hq::group_verdict::not_checked);

  auto const r = judge(entries[1], host);
  CHECK(r.live);
  CHECK(r.submitter_live);
  CHECK(r.group == hq::group_verdict::has_members);

  CHECK(select(entries, host).empty());
}

TEST_CASE("liveness: a running entry with a live submitter and an empty group is live", "[engine][hostqueue][liveness]") {
  // The submitter is between recording its child group and removing its
  // entry: the waiting test alone keeps the entry live.
  fake_host host;
  host.processes = {{100, 5}};

  std::array const entries{running(1, 100, 5, 300, 11)};
  auto const       verdict = judge(entries[0], host);
  CHECK(verdict.live);
  CHECK(verdict.group == hq::group_verdict::empty);
  CHECK(select(entries, host).empty());
}

TEST_CASE("liveness: a waiting entry refreshed outside the staleness window is not live", "[engine][hostqueue][liveness]") {
  fake_host host;
  host.processes = {{100, 5}};

  // Inside and at the edge of the window: fresh.
  CHECK(judge(waiting(1, 100, 5, 0), host).live);
  CHECK(judge(waiting(1, 100, 5, k_window - 1), host).live);
  CHECK(judge(waiting(1, 100, 5, k_window), host).live);

  // One millisecond past it: stale, although the process exists and matches.
  auto const stale = judge(waiting(1, 100, 5, k_window + 1), host);
  CHECK_FALSE(stale.live);
  CHECK(stale.submitter_gone());

  std::array const entries{waiting(1, 100, 5, k_window + 1), waiting(2, 100, 5, k_window)};
  CHECK(select(entries, host) == std::vector<std::int64_t>{1});
}

TEST_CASE("liveness: freshness is a window on both sides of now", "[engine][hostqueue][liveness]") {
  auto const ctx = context();

  CHECK(hq::is_fresh(k_now, ctx));
  CHECK(hq::is_fresh(k_now - k_window, ctx));
  CHECK_FALSE(hq::is_fresh(k_now - k_window - 1, ctx));

  // A refresh committed after the checker read its clock is fresh.
  CHECK(hq::is_fresh(k_now + 1, ctx));
  CHECK(hq::is_fresh(k_now + k_window, ctx));

  // A refresh far ahead of now comes from a clock this checker does not
  // share (a boot whose monotonic clock ran longer): stale.
  CHECK_FALSE(hq::is_fresh(k_now + k_window + 1, ctx));
  CHECK_FALSE(hq::is_fresh(std::numeric_limits<std::int64_t>::max(), ctx));
  CHECK_FALSE(hq::is_fresh(std::numeric_limits<std::int64_t>::min(), ctx));
}

TEST_CASE("liveness: a running entry whose submitter is gone stays live while its command runs",
          "[engine][hostqueue][liveness]") {
  fake_host host;
  host.processes           = {{300, 11}}; // the submitter (100) is gone; the group leader lives
  host.groups_with_members = {300};

  std::array const entries{running(1, 100, 5, 300, 11), waiting(2, 100, 5)};

  auto const verdict = judge(entries[0], host);
  CHECK(verdict.live);
  CHECK(verdict.submitter_gone());
  CHECK(verdict.group == hq::group_verdict::has_members);
  CHECK(hq::submitter_gone(entries[0], context(), host.probe()) == true);

  // The running entry survives; only the waiting entry of the same dead
  // submitter is reaped.
  CHECK(select(entries, host) == std::vector<std::int64_t>{2});
}

TEST_CASE("liveness: a group whose leader has died is live while another member remains", "[engine][hostqueue][liveness]") {
  fake_host host;
  host.groups_with_members = {300}; // no process 300; the group still has a member

  auto const verdict = judge(running(1, 100, 5, 300, 11), host);
  CHECK(verdict.live);
  CHECK(verdict.group == hq::group_verdict::has_members);
}

TEST_CASE("liveness: a running entry with a stopped submitter stays live while its command runs",
          "[engine][hostqueue][liveness]") {
  fake_host host;
  host.processes           = {{100, 5}, {300, 11}}; // the submitter exists and matches, but stopped refreshing
  host.groups_with_members = {300};

  std::array const entries{running(1, 100, 5, 300, 11, k_window + 1)};

  auto const verdict = judge(entries[0], host);
  CHECK(verdict.live);
  CHECK(verdict.submitter_gone());
  CHECK(verdict.group == hq::group_verdict::has_members);
  CHECK(select(entries, host).empty());
}

TEST_CASE("liveness: a running entry with no submitter and an empty group is reaped", "[engine][hostqueue][liveness]") {
  fake_host host;

  std::array const entries{running(1, 100, 5, 300, 11)};

  auto const verdict = judge(entries[0], host);
  CHECK_FALSE(verdict.live);
  CHECK(verdict.submitter_gone());
  CHECK(verdict.group == hq::group_verdict::empty);
  CHECK(select(entries, host) == std::vector<std::int64_t>{1});
}

TEST_CASE("liveness: a running entry that has recorded no child group is judged by its submitter",
          "[engine][hostqueue][liveness]") {
  fake_host host;
  host.processes = {{100, 5}};

  hq::entry started = waiting(1, 100, 5);
  started.state     = hq::entry_state::running;
  auto const alive  = judge(started, host);
  CHECK(alive.live);
  CHECK(alive.group == hq::group_verdict::not_checked);

  hq::entry orphan = waiting(2, 400, 5);
  orphan.state     = hq::entry_state::running;
  auto const gone  = judge(orphan, host);
  CHECK_FALSE(gone.live);
  CHECK(gone.group == hq::group_verdict::not_checked);
}

TEST_CASE("liveness: a reused process-group id is not mistaken for the command", "[engine][hostqueue][liveness]") {
  fake_host host;
  // The submitter is gone. A process with the group's id exists with a
  // different start time, and its group has members.
  host.processes           = {{300, 42}};
  host.groups_with_members = {300};

  std::array const entries{running(1, 100, 5, 300, 11)};

  auto const verdict = judge(entries[0], host);
  CHECK_FALSE(verdict.live);
  CHECK(verdict.submitter_gone());
  CHECK(verdict.group == hq::group_verdict::reused);
  CHECK(select(entries, host) == std::vector<std::int64_t>{1});
}

TEST_CASE("liveness: a reused process-group id is reported even while the submitter is live", "[engine][hostqueue][liveness]") {
  // A caller that would signal the group must learn it is reused whatever
  // the submitter's state.
  fake_host host;
  host.processes           = {{100, 5}, {300, 42}};
  host.groups_with_members = {300};

  auto const verdict = judge(running(1, 100, 5, 300, 11), host);
  CHECK(verdict.live);
  CHECK(verdict.group == hq::group_verdict::reused);
}

TEST_CASE("liveness: an entry from another host identity is judged by freshness alone", "[engine][hostqueue][liveness]") {
  fake_host host; // no process exists here at all

  std::array const entries{
      waiting(1, 100, 5, 0, "host-b"),
      waiting(2, 100, 5, k_window + 1, "host-b"),
      running(3, 100, 5, 300, 11, 0, "host-b"),
      running(4, 100, 5, 300, 11, k_window + 1, "host-b"),
  };

  auto const fresh = judge(entries[0], host);
  CHECK(fresh.live);
  CHECK_FALSE(fresh.submitter_gone());

  auto const stale = judge(entries[1], host);
  CHECK_FALSE(stale.live);
  CHECK(stale.submitter_gone());

  auto const fresh_running = judge(entries[2], host);
  CHECK(fresh_running.live);
  CHECK(fresh_running.group == hq::group_verdict::not_checked);

  auto const stale_running = judge(entries[3], host);
  CHECK_FALSE(stale_running.live);
  CHECK(stale_running.group == hq::group_verdict::not_checked);

  CHECK(select(entries, host) == std::vector<std::int64_t>{2, 4});
  CHECK(host.calls == 0);
}

TEST_CASE("liveness: another host's pid is never tested even when it names a live process here",
          "[engine][hostqueue][liveness]") {
  // A pid here that matches the recorded one must not rescue a stale entry
  // from another host, nor a group here keep it running.
  fake_host host;
  host.processes           = {{100, 5}, {300, 11}};
  host.groups_with_members = {300};

  std::array const entries{running(1, 100, 5, 300, 11, k_window + 1, "host-b")};
  CHECK_FALSE(judge(entries[0], host).live);
  CHECK(select(entries, host) == std::vector<std::int64_t>{1});
  CHECK(host.calls == 0);
}

TEST_CASE("liveness: an unknown host identity on either side is judged by freshness alone", "[engine][hostqueue][liveness]") {
  fake_host host;
  host.processes = {{100, 5}};

  auto const unknown = std::string(pid_ns::k_unknown_host_identity);

  // Checker unknown, entry known; entry unknown, checker known; both unknown.
  for (auto const& [checker, recorded] : std::array{std::pair{unknown, std::string(k_host)},
                                                    std::pair{std::string(k_host), unknown}, std::pair{unknown, unknown}}) {
    // A dead pid does not reap a fresh entry...
    CHECK(judge(waiting(1, 999, 5, 0, recorded), host, context(checker)).live);
    // ...and a live, matching pid does not keep a stale one.
    CHECK_FALSE(judge(waiting(1, 100, 5, k_window + 1, recorded), host, context(checker)).live);
  }
  CHECK(host.calls == 0);
}

TEST_CASE("liveness: a failed process query is an error and never reaps", "[engine][hostqueue][liveness]") {
  fake_host host;
  host.failing = {100, 300};

  std::array const entries{waiting(1, 100, 5), running(2, 777, 5, 300, 11)};

  auto const waiting_verdict = hq::judge_liveness(entries[0], context(), host.probe());
  REQUIRE_FALSE(waiting_verdict.has_value());
  CHECK(waiting_verdict.error() == pid_ns::error::query_failed);

  auto const running_verdict = hq::judge_liveness(entries[1], context(), host.probe());
  REQUIRE_FALSE(running_verdict.has_value());
  CHECK(running_verdict.error() == pid_ns::error::query_failed);

  CHECK_FALSE(hq::submitter_gone(entries[0], context(), host.probe()).has_value());
  CHECK(select(entries, host).empty());
}

TEST_CASE("liveness: a killed submitter does not free the slot, and a killed waiter does not hold up the queue",
          "[engine][hostqueue][liveness]") {
  // Entry 1 runs; its submitter (100) was killed but its command's group
  // (300) runs on. Entry 2's submitter (200) was killed while waiting.
  // Entry 3's submitter (400) is live and waiting.
  fake_host host;
  host.processes           = {{300, 11}, {400, 13}};
  host.groups_with_members = {300};

  std::array const entries{running(1, 100, 5, 300, 11), waiting(2, 200, 7), waiting(3, 400, 13)};

  // While the command runs, only the killed waiter is reaped.
  CHECK(select(entries, host) == std::vector<std::int64_t>{2});

  // Once the command has ended, its entry is reaped too, and entry 3 is the
  // only one left.
  host.processes.erase(300);
  host.groups_with_members.clear();
  CHECK(select(entries, host) == std::vector<std::int64_t>{1, 2});
}

namespace {

// @brief A child that blocks reading a pipe until `release()` (or the
// holder leaving scope) closes the write end, then exits; it is reaped on
// every path. It leads its own process group.
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
    release();
  }

  [[nodiscard]] auto pid() const -> std::int64_t {
    return _pid;
  }

  // @brief Let the child exit and wait for it, so its pid and group are
  // gone when this returns.
  void release() {
    if (_release >= 0) {
      ::close(_release);
      _release = -1;
    }
    if (_pid > 0) {
      int status = 0;
      while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
      }
      _pid = 0;
    }
  }

private:
  ::pid_t _pid     = 0;
  int     _release = -1;
};

// @brief Fork a child that exits at once, reap it, and return its pid,
// which then names no process.
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

auto real_start_time(std::int64_t pid) -> std::int64_t {
  auto started = pid_ns::process_start_time(pid);
  REQUIRE(started.has_value());
  REQUIRE(started->has_value());
  return static_cast<std::int64_t>(**started);
}

} // namespace

TEST_CASE("liveness: the system probe judges a submitter this test started", "[engine][hostqueue][liveness]") {
  auto const probe = hq::system_process_probe();
  auto const ctx   = context();

  blocked_child submitter;
  auto const    started = real_start_time(submitter.pid());

  auto live = hq::judge_liveness(waiting(1, submitter.pid(), started), ctx, probe);
  REQUIRE(live.has_value());
  CHECK(live->live);

  auto reused = hq::judge_liveness(waiting(1, submitter.pid(), started + 1), ctx, probe);
  REQUIRE(reused.has_value());
  CHECK_FALSE(reused->live);

  auto const pid = submitter.pid();
  submitter.release();
  auto dead = hq::judge_liveness(waiting(1, pid, started), ctx, probe);
  REQUIRE(dead.has_value());
  CHECK_FALSE(dead->live);
}

TEST_CASE("liveness: the system probe keeps a running entry live while a group this test created has a member",
          "[engine][hostqueue][liveness]") {
  auto const probe = hq::system_process_probe();
  auto const ctx   = context();

  auto const gone_submitter = reaped_pid();

  blocked_child command;
  auto const    pgid    = command.pid();
  auto const    started = real_start_time(pgid);

  std::array const entries{running(1, gone_submitter, 5, pgid, started)};
  auto             verdict = hq::judge_liveness(entries[0], ctx, probe);
  REQUIRE(verdict.has_value());
  CHECK(verdict->live);
  CHECK(verdict->submitter_gone());
  CHECK(verdict->group == hq::group_verdict::has_members);
  CHECK(hq::select_not_live(entries, ctx, probe).empty());

  // The group's leader is alive with a different recorded start time: the
  // group id counts as reused.
  auto reused = hq::judge_liveness(running(1, gone_submitter, 5, pgid, started + 1), ctx, probe);
  REQUIRE(reused.has_value());
  CHECK_FALSE(reused->live);
  CHECK(reused->group == hq::group_verdict::reused);

  command.release();
  auto ended = hq::judge_liveness(entries[0], ctx, probe);
  REQUIRE(ended.has_value());
  CHECK_FALSE(ended->live);
  CHECK(ended->group == hq::group_verdict::empty);
  CHECK(hq::select_not_live(entries, ctx, probe) == std::vector<std::int64_t>{1});
}

TEST_CASE("liveness: the system probe counts a process the checker may not signal as existing", "[engine][hostqueue][liveness]") {
  // Process 1 always exists. A checker that is not root may not signal it,
  // so `kill(1, 0)` fails with EPERM, which must still read as existing.
  // Only its existence is queried; nothing is signalled. (Its start time is
  // not asserted: macOS refuses `proc_pidinfo` on another user's process.)
  auto const probe  = hq::system_process_probe();
  auto const exists = probe.process_exists(1);
  REQUIRE(exists.has_value());
  CHECK(*exists);

  // A process that is gone reads as absent through the same probe.
  auto const gone = probe.process_exists(reaped_pid());
  REQUIRE(gone.has_value());
  CHECK_FALSE(*gone);
}
