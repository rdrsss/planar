// @file identity.t.cpp
// @brief Tests for `planar.process.identity` (plan 1080, task 6998).
//
// Every scenario here is one of the five in the host build and test queue
// test spec (artifact 649) that cites `task:hq-process-identity`, plus the
// clock and group-signal checks the brief added. The tests are hermetic
// under parallel ctest: every process they examine is one they forked,
// every group they test is one they created, and everything they start is
// reaped before the test returns. No test asserts on a pid it did not
// produce, and nothing is signalled that this file did not start.
//
// The errno-to-verdict mapping is tested as a pure function so the `EPERM`
// arm holds when the suite runs as root, where no process is off limits.

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import planar.process.identity;

namespace {

namespace pid_ns = planar::process::identity;

// @brief A child running `/bin/sleep`, killed and reaped when the holder
// leaves scope or on `reap()`.
//
// By default the child leads its own process group. Given a group id, it
// joins that group instead, which is how a test builds a group whose leader
// can die while another member lives on.
class sleeping_child {
public:
  explicit sleeping_child(::pid_t join_group = 0) {
    // An exec barrier. The forked child inherits Catch2's fatal-condition
    // handler for SIGTERM, so a signal that lands before `execl` has
    // replaced the image runs that handler instead of terminating the
    // child. The write end is close-on-exec: the parent's read returns EOF
    // only once the exec has happened (or the child has died), so nothing
    // a test sends can reach the pre-exec image.
    std::array<int, 2> fds{};
    if (::pipe(fds.data()) != 0) {
      throw std::runtime_error("pipe failed");
    }
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    ::pid_t const forked = ::fork();
    if (forked < 0) {
      throw std::runtime_error("fork failed");
    }
    if (forked == 0) {
      ::close(fds[0]);
      ::setpgid(0, join_group);
      ::execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
      ::_exit(127);
    }
    ::close(fds[1]);
    // Both sides set the group so the parent never observes the child in
    // the parent's group, whichever runs first. Once the child has exec'd
    // the parent's call fails with EACCES, by which time the child's own
    // call has already placed it.
    ::setpgid(forked, join_group == 0 ? forked : join_group);
    _pid = forked;

    char byte = 0;
    while (::read(fds[0], &byte, 1) < 0 && errno == EINTR) {
    }
    ::close(fds[0]);
  }
  sleeping_child(const sleeping_child&)                    = delete;
  auto operator=(const sleeping_child&) -> sleeping_child& = delete;
  sleeping_child(sleeping_child&&)                         = delete;
  auto operator=(sleeping_child&&) -> sleeping_child&      = delete;
  ~sleeping_child() {
    reap();
  }

  [[nodiscard]] auto pid() const -> std::int64_t {
    return _pid;
  }

  // @brief Kill the child and wait for it, so its pid and group are gone
  // when this returns.
  void reap() {
    if (_pid <= 0) {
      return;
    }
    ::kill(_pid, SIGKILL);
    int status = 0;
    while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
    }
    _pid = 0;
  }

  // @brief Drop the pid without signalling or waiting, for a child the test
  // reaped by hand.
  void forget() {
    _pid = 0;
  }

private:
  ::pid_t _pid = 0;
};

// @brief What watching a group whose members the test has all reaped found.
enum class drain_verdict {
  emptied,         // No process is in the group any more.
  still_populated, // The group kept a member for the whole bound, and no process holds its id.
  pid_reused,      // Some other process now holds the group's id, so the group is not this test's any more.
};

// @brief Watches `pgid`, whose every member this test has reaped, until it is
// empty, for at most a bound.
//
// Two things make a single `group_has_members` call right after the reap an
// unreliable verdict on a loaded host (measured: 1 failure in 304 runs at -j16
// beside busy loops and fork churn). The kernel may still list the group for a
// moment after `waitpid` returns, which a bounded wait absorbs. And the id of
// a reaped leader is free for any other process to take; if one becomes a
// group leader, `kill(-pgid, 0)` succeeds for that unrelated group for as long
// as it lives. That cannot be waited out, so it is detected (the id then names
// a live process, and this test reaped its own) and reported, so the caller
// can rerun the scenario with a fresh group instead of judging a group that is
// not its own. What the production function returns is asserted unchanged.
auto watch_group_drain(std::int64_t pgid) -> drain_verdict {
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (true) {
    auto const members = pid_ns::group_has_members(pgid);
    REQUIRE(members.has_value());
    if (!*members) {
      return drain_verdict::emptied;
    }
    auto const holder = pid_ns::process_exists(pgid);
    REQUIRE(holder.has_value());
    if (*holder) {
      return drain_verdict::pid_reused;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return drain_verdict::still_populated;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

// @brief Runs `scenario` until it does not report that its group id was reused
// by another process, at most a few times. `scenario` returns true for reuse.
template <class Scenario> void with_fresh_group(Scenario&& scenario) {
  constexpr int k_attempts = 5;
  for (int attempt = 0; attempt < k_attempts; ++attempt) {
    if (!scenario()) {
      return;
    }
  }
  FAIL("the group id was taken by another process in every one of " << k_attempts << " attempts");
}

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

// @brief A clock a test sets by hand.
class fake_clock final : public pid_ns::clock {
public:
  std::int64_t monotonic = 0;
  std::int64_t wall      = 0;

  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, pid_ns::error> override {
    return monotonic;
  }
  [[nodiscard]] auto wall_ms() -> std::int64_t override {
    return wall;
  }
};

} // namespace

TEST_CASE("process_start_time: the current process's start time is stable and a later child's differs",
          "[lib][process][identity]") {
  auto const self = static_cast<std::int64_t>(::getpid());

  auto const first = pid_ns::process_start_time(self);
  REQUIRE(first.has_value());
  REQUIRE(first->has_value());

  auto const second = pid_ns::process_start_time(self);
  REQUIRE(second.has_value());
  REQUIRE(second->has_value());
  CHECK(**first == **second);

  // On Linux the value is in clock ticks (10 ms), so a child forked within
  // the same tick as this process started would share its value. This
  // process has been running for longer than that by the time a test case
  // executes, and the pause makes that true on every host.
  std::this_thread::sleep_for(std::chrono::milliseconds{25});
  sleeping_child const child;
  auto const           child_start = pid_ns::process_start_time(child.pid());
  REQUIRE(child_start.has_value());
  REQUIRE(child_start->has_value());
  CHECK(**child_start != **first);
}

TEST_CASE("process_start_time and process_exists: a process that does not exist reports absence, not an error",
          "[lib][process][identity]") {
  auto const gone = reaped_pid();

  auto const start = pid_ns::process_start_time(gone);
  REQUIRE(start.has_value());
  CHECK_FALSE(start->has_value());

  auto const exists = pid_ns::process_exists(gone);
  REQUIRE(exists.has_value());
  CHECK_FALSE(*exists);
}

TEST_CASE("process_exists: a live child exists", "[lib][process][identity]") {
  sleeping_child const child;
  auto const           exists = pid_ns::process_exists(child.pid());
  REQUIRE(exists.has_value());
  CHECK(*exists);
}

TEST_CASE("exists_from_errno: a process that cannot be signalled still exists", "[lib][process][identity]") {
  auto const permitted = pid_ns::exists_from_errno(0);
  REQUIRE(permitted.has_value());
  CHECK(*permitted);

  auto const refused = pid_ns::exists_from_errno(EPERM);
  REQUIRE(refused.has_value());
  CHECK(*refused);

  auto const absent = pid_ns::exists_from_errno(ESRCH);
  REQUIRE(absent.has_value());
  CHECK_FALSE(*absent);

  auto const unknown = pid_ns::exists_from_errno(EINVAL);
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error() == pid_ns::error::query_failed);
}

TEST_CASE("group_has_members: a process group is a member of itself until it is empty", "[lib][process][identity]") {
  with_fresh_group([] {
    sleeping_child child;
    auto const     pgid = child.pid();

    auto const populated = pid_ns::group_has_members(pgid);
    REQUIRE(populated.has_value());
    CHECK(*populated);

    child.reap();

    auto const verdict = watch_group_drain(pgid);
    if (verdict == drain_verdict::pid_reused) {
      return true;
    }
    CHECK(verdict == drain_verdict::emptied);
    return false;
  });
}

TEST_CASE("group_has_members and signal_group: a group outlives its dead leader until its last member is signalled",
          "[lib][process][identity]") {
  // An orphaned helper whose leader has died is the case the engine uses
  // the group form for. The leader is reaped first, so a call that targeted
  // the pid `pgid` instead of the group `-pgid` would find nothing.
  with_fresh_group([] {
    sleeping_child leader;
    auto const     pgid = leader.pid();
    sleeping_child member{static_cast<::pid_t>(pgid)};
    REQUIRE(::getpgid(static_cast<::pid_t>(member.pid())) == static_cast<::pid_t>(pgid));

    leader.reap();
    REQUIRE_FALSE(*pid_ns::process_exists(pgid));

    auto const orphaned = pid_ns::group_has_members(pgid);
    REQUIRE(orphaned.has_value());
    CHECK(*orphaned);

    auto const sent = pid_ns::signal_group(pgid, SIGTERM);
    REQUIRE(sent.has_value());

    int status = 0;
    while (::waitpid(static_cast<::pid_t>(member.pid()), &status, 0) < 0 && errno == EINTR) {
    }
    member.forget();
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGTERM);

    auto const verdict = watch_group_drain(pgid);
    if (verdict == drain_verdict::pid_reused) {
      return true;
    }
    CHECK(verdict == drain_verdict::emptied);
    return false;
  });
}

TEST_CASE("signal_group: a signal reaches every member of a group this test created", "[lib][process][identity]") {
  with_fresh_group([] {
    sleeping_child child;
    auto const     pgid = child.pid();

    auto const sent = pid_ns::signal_group(pgid, SIGTERM);
    REQUIRE(sent.has_value());

    int status = 0;
    while (::waitpid(static_cast<::pid_t>(pgid), &status, 0) < 0 && errno == EINTR) {
    }
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGTERM);

    // Reaped by hand above, so the holder has nothing left to wait on.
    child.forget();
    auto const verdict = watch_group_drain(pgid);
    if (verdict == drain_verdict::pid_reused) {
      return true;
    }
    CHECK(verdict == drain_verdict::emptied);
    return false;
  });
}

TEST_CASE("signal_group: a group id that names no group is the module's error, not a crash", "[lib][process][identity]") {
  // Signal 0 sends nothing, so even if another test's group has since
  // taken this id, it is not disturbed.
  auto const gone = reaped_pid();
  auto const sent = pid_ns::signal_group(gone, 0);
  REQUIRE_FALSE(sent.has_value());
  CHECK(sent.error() == pid_ns::error::no_such_process);
}

TEST_CASE("identity: ids that name the caller's own group or every process are refused before any kill",
          "[lib][process][identity]") {
  // `kill(0, ...)` targets the caller's own group and `kill(-1, ...)` every
  // process the user may signal. Only signal 0 is used here, so a missing
  // guard is detected by `kill` succeeding and nothing is ever delivered.
  auto const own_group = pid_ns::signal_group(0, 0);
  REQUIRE_FALSE(own_group.has_value());
  CHECK(own_group.error() == pid_ns::error::no_such_process);

  auto const everything = pid_ns::signal_group(1, 0);
  REQUIRE_FALSE(everything.has_value());
  CHECK(everything.error() == pid_ns::error::no_such_process);

  auto const negative_group = pid_ns::signal_group(-5, 0);
  REQUIRE_FALSE(negative_group.has_value());
  CHECK(negative_group.error() == pid_ns::error::no_such_process);

  auto const init_group = pid_ns::group_has_members(1);
  REQUIRE(init_group.has_value());
  CHECK_FALSE(*init_group);

  auto const zero_group = pid_ns::group_has_members(0);
  REQUIRE(zero_group.has_value());
  CHECK_FALSE(*zero_group);

  for (std::int64_t const pid : {std::int64_t{-1}, std::int64_t{0}}) {
    CAPTURE(pid);
    auto const exists = pid_ns::process_exists(pid);
    REQUIRE(exists.has_value());
    CHECK_FALSE(*exists);

    auto const start = pid_ns::process_start_time(pid);
    REQUIRE(start.has_value());
    CHECK_FALSE(start->has_value());
  }
}

TEST_CASE("identity: an id beyond the platform's pid range is absent, not a truncated alias", "[lib][process][identity]") {
  // 2^32 plus a live id truncates to that live id in a 32-bit pid_t. The
  // module must read it as a process that cannot exist. Only signal 0 is
  // sent, so a truncating implementation disturbs nothing.
  constexpr std::int64_t k_wrap = std::int64_t{1} << 32;
  sleeping_child const   child;
  auto const             alias = k_wrap + child.pid();

  auto const exists = pid_ns::process_exists(alias);
  REQUIRE(exists.has_value());
  CHECK_FALSE(*exists);

  auto const start = pid_ns::process_start_time(alias);
  REQUIRE(start.has_value());
  CHECK_FALSE(start->has_value());

  auto const members = pid_ns::group_has_members(alias);
  REQUIRE(members.has_value());
  CHECK_FALSE(*members);

  auto const sent = pid_ns::signal_group(alias, 0);
  REQUIRE_FALSE(sent.has_value());
  CHECK(sent.error() == pid_ns::error::no_such_process);

  // The largest id a pid_t holds is still in range, and names no process.
  auto const largest = pid_ns::process_exists(std::numeric_limits<std::int32_t>::max());
  REQUIRE(largest.has_value());
  CHECK_FALSE(*largest);
}

TEST_CASE("signal_group: an invalid signal number is the module's error", "[lib][process][identity]") {
  sleeping_child const child;
  auto const           sent = pid_ns::signal_group(child.pid(), 100'000);
  REQUIRE_FALSE(sent.has_value());
  CHECK(sent.error() == pid_ns::error::invalid_signal);
}

TEST_CASE("host_identity: an unreadable source is the literal unknown", "[lib][process][identity]") {
  pid_ns::identity_source const unreadable = []() -> std::optional<std::string> { return std::nullopt; };
  CHECK(pid_ns::host_identity(unreadable) == "unknown");
  CHECK(pid_ns::host_identity(unreadable) == pid_ns::k_unknown_host_identity);

  pid_ns::identity_source const blank = []() -> std::optional<std::string> { return std::string{}; };
  CHECK(pid_ns::host_identity(blank) == "unknown");

  auto const real = pid_ns::host_identity(pid_ns::native_identity_source());
  CHECK(real != "unknown");
  CHECK_FALSE(real.empty());
  CHECK(pid_ns::host_identity(unreadable) != real);

  // A real read is stable within one boot.
  CHECK(pid_ns::host_identity(pid_ns::native_identity_source()) == real);
}

TEST_CASE("host_identity: a readable source is passed through verbatim", "[lib][process][identity]") {
  pid_ns::identity_source const fixed = []() -> std::optional<std::string> { return "boot-7:pid:[42]"; };
  CHECK(pid_ns::host_identity(fixed) == "boot-7:pid:[42]");
}

TEST_CASE("system_clock: the monotonic and wall clocks tick forward", "[lib][process][identity]") {
  pid_ns::system_clock clock;

  auto const before = clock.monotonic_ms();
  REQUIRE(before.has_value());
  auto const wall_before = clock.wall_ms();

  std::this_thread::sleep_for(std::chrono::milliseconds{15});

  auto const after = clock.monotonic_ms();
  REQUIRE(after.has_value());
  CHECK(*after > *before);
  CHECK(*after - *before >= 10);

  auto const wall_after = clock.wall_ms();
  CHECK(wall_after >= wall_before);
  CHECK(wall_after - wall_before >= 10);

  // Wall time is Unix-epoch milliseconds, so it is far past 2020-01-01.
  CHECK(wall_after > 1'577'836'800'000);
  auto const chrono_now =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  CHECK(std::abs(chrono_now - wall_after) < 5'000);

  // The monotonic clock counts from boot, not from the epoch: a host would
  // have to have been up for decades for it to reach half of wall time. A
  // monotonic value read from the wall clock fails this.
  CHECK(*after < wall_after / 2);
}

TEST_CASE("clock: a fake clock can be driven through the interface", "[lib][process][identity]") {
  fake_clock fake;
  fake.monotonic = 1'000;
  fake.wall      = 1'700'000'000'000;

  pid_ns::clock& as_interface = fake;
  auto const     first        = as_interface.monotonic_ms();
  REQUIRE(first.has_value());
  CHECK(*first == 1'000);
  CHECK(as_interface.wall_ms() == 1'700'000'000'000);

  fake.monotonic += 250;
  auto const second = as_interface.monotonic_ms();
  REQUIRE(second.has_value());
  CHECK(*second == 1'250);
  CHECK(as_interface.wall_ms() == 1'700'000'000'000);
}

// ---------------------------------------------------------------------------
// Task 7072: a group of exited-but-unreaped processes
// ---------------------------------------------------------------------------

TEST_CASE("group_only_zombies: a live group, an empty group and an id no group can have are not all zombies",
          "[lib][process][identity][hq-eperm-zombie-group]") {
  {
    sleeping_child child;
    auto const     live = pid_ns::group_only_zombies(child.pid());
    REQUIRE(live.has_value());
    CHECK_FALSE(*live);
  }
  for (std::int64_t const id : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-7}}) {
    INFO("id " << id);
    auto const verdict = pid_ns::group_only_zombies(id);
    REQUIRE(verdict.has_value());
    CHECK_FALSE(*verdict);
  }
}

#if defined(__APPLE__)
TEST_CASE("group_only_zombies: a group whose only member is an exited leader nobody reaped is all zombies, and macOS refuses to "
          "signal it",
          "[lib][process][identity][hq-eperm-zombie-group]") {
  // macOS answers kill(-pgid, sig) with EPERM for such a group, signal 0
  // included, so `group_has_members` says "populated" and `signal_group` says
  // "not permitted" for a group that holds nothing that can run. This case
  // pins that premise, and the verification that tells it from a real refusal.
  ::pid_t const forked = ::fork();
  REQUIRE(forked >= 0);
  if (forked == 0) {
    ::setpgid(0, 0);
    ::_exit(0);
  }
  ::setpgid(forked, forked);
  auto const pgid = static_cast<std::int64_t>(forked);

  // The kernel takes a moment to turn the exit into a zombie; wait for it, bounded.
  bool       zombies  = false;
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    auto const verdict = pid_ns::group_only_zombies(pgid);
    REQUIRE(verdict.has_value());
    if (*verdict) {
      zombies = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(zombies);

  auto const refused = pid_ns::signal_group(pgid, SIGKILL);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == pid_ns::error::not_permitted);

  int status = 0;
  while (::waitpid(forked, &status, 0) < 0 && errno == EINTR) {
  }
  // Reaped: the group is empty, which is not "only zombies".
  auto const after = pid_ns::group_only_zombies(pgid);
  REQUIRE(after.has_value());
  CHECK_FALSE(*after);
}

TEST_CASE("group_only_zombies: a zombie leader does not make a group with a live member all zombies",
          "[lib][process][identity][hq-eperm-zombie-group]") {
  std::array<int, 2> fds{};
  REQUIRE(::pipe(fds.data()) == 0);
  ::pid_t const leader = ::fork();
  REQUIRE(leader >= 0);
  if (leader == 0) {
    ::setpgid(0, 0);
    ::close(fds[0]);
    if (::fork() == 0) {
      // The member: stays in the leader's group until released by the pipe closing.
      char byte = 0;
      (void)::write(fds[1], &byte, 1);
      ::close(fds[1]);
      ::sleep(30);
      ::_exit(0);
    }
    ::_exit(0);
  }
  ::setpgid(leader, leader);
  ::close(fds[1]);
  char byte = 0;
  while (::read(fds[0], &byte, 1) < 0 && errno == EINTR) {
  }
  ::close(fds[0]);
  auto const pgid = static_cast<std::int64_t>(leader);
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the leader exit into zombie

  auto const verdict = pid_ns::group_only_zombies(pgid);
  // Every member this test made is signalled and reaped before any assertion can end the case.
  ::kill(-leader, SIGKILL);
  int status = 0;
  while (::waitpid(leader, &status, 0) < 0 && errno == EINTR) {
  }
  REQUIRE(verdict.has_value());
  CHECK_FALSE(*verdict);
}
#endif
