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

// @brief A child running `/bin/sleep` as the leader of its own process
// group, killed and reaped when the holder leaves scope or on `reap()`.
class sleeping_child {
public:
  sleeping_child() {
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
      ::setpgid(0, 0);
      ::execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
      ::_exit(127);
    }
    ::close(fds[1]);
    // Both sides set the group so the parent never observes the child in
    // the parent's group, whichever runs first.
    ::setpgid(forked, forked);
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
  sleeping_child child;
  auto const     pgid = child.pid();

  auto const populated = pid_ns::group_has_members(pgid);
  REQUIRE(populated.has_value());
  CHECK(*populated);

  child.reap();

  auto const emptied = pid_ns::group_has_members(pgid);
  REQUIRE(emptied.has_value());
  CHECK_FALSE(*emptied);
}

TEST_CASE("signal_group: a signal reaches every member of a group this test created", "[lib][process][identity]") {
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
  auto const empty = pid_ns::group_has_members(pgid);
  REQUIRE(empty.has_value());
  CHECK_FALSE(*empty);
}

TEST_CASE("signal_group: a group id that names no group is the module's error, not a crash", "[lib][process][identity]") {
  auto const gone = reaped_pid();
  auto const sent = pid_ns::signal_group(gone, SIGTERM);
  REQUIRE_FALSE(sent.has_value());
  CHECK(sent.error() == pid_ns::error::no_such_process);
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
