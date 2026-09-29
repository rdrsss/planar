// @file runner.t.cpp
// @brief Tests for `planar.process.runner` (plan 1080, task 7006).
//
// The four scenarios in the host build and test queue test spec (artifact
// 649) that cite `task:hq-process-runner` are here, except "run_inherited
// is unchanged", which is process.t.cpp passing unmodified. The brief added
// the working directory, the non-blocking wait and the grandchild reached
// through the group.
//
// Hermetic under parallel ctest:
//
//   * Every child is a script this file wrote, started by the runner.
//     Children block on a FIFO this test owns, never on a sleep, and are
//     released by writing to it.
//   * Every child is reaped on every path. `child_guard` kills and reaps a
//     child the test did not see terminate; it signals the group only after
//     checking the child leads a group that is not this process's own, so a
//     broken runner that left the child in the test's group cannot make the
//     cleanup signal the test runner.
//   * Every wait is bounded. The non-blocking test releases its child from
//     a watchdog thread, so a `poll` that blocks returns late and fails
//     rather than hanging.
//
// The runner resets caught signal handlers in the child before it
// executes, so Catch2's SIGTERM handler, which a forked child inherits,
// cannot swallow the SIGTERM the grandchild test sends.

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import planar.process;
import planar.process.identity;
import planar.process.runner;

namespace {

namespace proc   = planar::process;
namespace runner = planar::process::runner;

constexpr std::string_view k_slot_var = "PLANAR_QUEUE_SLOT";
constexpr auto             k_bound    = std::chrono::seconds{10};

// @brief A scratch directory removed when the test leaves scope.
class scratch_dir {
public:
  scratch_dir() {
    auto const base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 64; ++attempt) {
      auto            candidate = base / std::format("planar-runner-t-{}-{}", ::getpid(), attempt);
      std::error_code ec;
      if (std::filesystem::create_directory(candidate, ec)) {
        _path = std::filesystem::canonical(candidate);
        return;
      }
    }
    throw std::runtime_error("could not create a scratch directory");
  }
  scratch_dir(const scratch_dir&)                    = delete;
  auto operator=(const scratch_dir&) -> scratch_dir& = delete;
  scratch_dir(scratch_dir&&)                         = delete;
  auto operator=(scratch_dir&&) -> scratch_dir&      = delete;
  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(_path, ec);
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return _path;
  }

private:
  std::filesystem::path _path;
};

// @brief Write `body` to `path` and make it executable.
void write_script(const std::filesystem::path& path, std::string_view body) {
  {
    std::ofstream out{path};
    out << body;
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_all);
}

// @brief Write `body` to `path` with no execute permission for anyone.
void write_plain(const std::filesystem::path& path, std::string_view body) {
  {
    std::ofstream out{path};
    out << body;
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

// @brief Read a whole file, or an empty string when it is not there.
auto slurp(const std::filesystem::path& path) -> std::string {
  std::ifstream in{path};
  if (!in) {
    return {};
  }
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// @brief An `env_lookup` over an explicit map. Nothing here reads the real
// process environment.
auto map_env(std::map<std::string, std::string, std::less<>> vars) -> proc::env_lookup {
  return [vars = std::move(vars)](std::string_view key) -> std::optional<std::string> {
    auto const hit = vars.find(key);
    if (hit == vars.end()) {
      return std::nullopt;
    }
    return hit->second;
  };
}

// @brief A FIFO a child blocks on until the test releases it.
//
// The test holds both a read end and a write end for the gate's lifetime,
// both close-on-exec. The write end means a child's `read x < fifo` opens
// at once whenever it gets there; the read end means a release written
// before the child opened the FIFO stays buffered instead of raising
// SIGPIPE here. `release(n)` writes n lines, one per `read` waiting on it.
class fifo_gate {
public:
  explicit fifo_gate(std::filesystem::path path) : _path(std::move(path)) {
    if (::mkfifo(_path.c_str(), 0600) != 0) {
      throw std::runtime_error("mkfifo failed");
    }
    _keeper = ::open(_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    _writer = ::open(_path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (_keeper < 0 || _writer < 0) {
      throw std::runtime_error("opening the fifo failed");
    }
  }
  fifo_gate(const fifo_gate&)                    = delete;
  auto operator=(const fifo_gate&) -> fifo_gate& = delete;
  fifo_gate(fifo_gate&&)                         = delete;
  auto operator=(fifo_gate&&) -> fifo_gate&      = delete;
  ~fifo_gate() {
    ::close(_writer);
    ::close(_keeper);
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return _path;
  }

  void release(int readers = 1) {
    std::scoped_lock const lock{_mutex};
    for (int i = 0; i < readers; ++i) {
      while (::write(_writer, "go\n", 3) < 0 && errno == EINTR) {
      }
    }
  }

private:
  std::filesystem::path _path;
  std::mutex            _mutex;
  int                   _keeper = -1;
  int                   _writer = -1;
};

// @brief Owns a started child until the test has seen it terminate.
//
// `wait_for` polls with a bound. A child the test never saw terminate is
// killed and reaped when the guard leaves scope.
class child_guard {
public:
  explicit child_guard(runner::child started) : _child(started) {
  }
  child_guard(const child_guard&)                    = delete;
  auto operator=(const child_guard&) -> child_guard& = delete;
  child_guard(child_guard&&)                         = delete;
  auto operator=(child_guard&&) -> child_guard&      = delete;
  ~child_guard() {
    if (_reaped || _child.pid <= 0) {
      return;
    }
    auto const pid = static_cast<::pid_t>(_child.pid);
    // Signal the group only when the child leads a group that is not this
    // process's own; otherwise signal the child alone.
    if (::getpgid(pid) == pid && pid != ::getpgrp()) {
      ::kill(-pid, SIGKILL);
    } else {
      ::kill(pid, SIGKILL);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }

  [[nodiscard]] auto get() const -> const runner::child& {
    return _child;
  }

  // @brief Poll until the child terminates or `limit` passes.
  // @return The terminal status, or unset on timeout or a poll error.
  auto wait_for(std::chrono::milliseconds limit) -> std::optional<runner::status> {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
      auto const got = runner::poll(_child);
      if (!got.has_value()) {
        // The runner could not wait on it, which means it is not waitable:
        // never signal that pid again.
        _reaped = true;
        return std::nullopt;
      }
      if (got->kind != runner::state::running) {
        _reaped = true;
        return *got;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return std::nullopt;
  }

private:
  runner::child _child;
  bool          _reaped = false;
};

// @brief Start options with the queue's variable set to `value`.
auto slot_options(std::string value) -> runner::start_options {
  return {.working_directory = std::nullopt, .env_name = std::string{k_slot_var}, .env_value = std::move(value)};
}

// @brief Whether this process has any child left to reap.
auto has_unreaped_child() -> bool {
  int status = 0;
  return ::waitpid(-1, &status, WNOHANG) >= 0;
}

} // namespace

TEST_CASE("runner: reports a child's exit status, in its own group, with the added variable", "[lib][process][runner]") {
  for (int const code : {0, 1, 42}) {
    CAPTURE(code);
    scratch_dir const scratch;
    fifo_gate         gate{scratch.path() / "gate"};
    auto const        witness = scratch.path() / "witness";
    auto const        script  = scratch.path() / "child.sh";
    write_script(script, std::format("#!/bin/sh\n"
                                     "printf '%s\\n%s\\n' \"${}\" \"$PATH\" > '{}'\n"
                                     "read x < '{}'\n"
                                     "exit {}\n",
                                     k_slot_var, witness.string(), gate.path().string(), code));

    std::vector<std::string> const argv{script.string()};
    auto const                     slot    = std::format("slot-{}", code);
    auto                           started = runner::start(map_env({}), argv, slot_options(slot));
    REQUIRE(started.has_value());
    child_guard guard{*started};

    // The child is blocked on the gate, so it is alive while its group is
    // read. Its group is its own, and not this process's.
    auto const pid = static_cast<::pid_t>(guard.get().pid);
    CHECK(guard.get().pgid == guard.get().pid);
    CHECK(::getpgid(pid) == pid);
    CHECK(::getpgid(pid) != ::getpgrp());

    // The start time names this child.
    auto const start_time = planar::process::identity::process_start_time(guard.get().pid);
    REQUIRE(start_time.has_value());
    REQUIRE(start_time->has_value());
    CHECK(**start_time == guard.get().started);

    gate.release();
    auto const ended = guard.wait_for(k_bound);
    REQUIRE(ended.has_value());
    CHECK(ended->kind == runner::state::exited);
    CHECK(ended->code == code);

    // The added variable arrived; an inherited one passed through.
    auto const recorded = slurp(witness);
    REQUIRE_FALSE(recorded.empty());
    auto const* inherited_path = std::getenv("PATH");
    REQUIRE(inherited_path != nullptr);
    CHECK(recorded == std::format("{}\n{}\n", slot, inherited_path));
  }
}

TEST_CASE("runner: the added variable replaces an inherited one of the same name", "[lib][process][runner]") {
  constexpr const char* k_name = "PLANAR_RUNNER_T_7006_OVERRIDE";
  REQUIRE(::setenv(k_name, "inherited", 1) == 0);
  struct unset_on_exit {
    unset_on_exit()                                        = default;
    unset_on_exit(const unset_on_exit&)                    = delete;
    auto operator=(const unset_on_exit&) -> unset_on_exit& = delete;
    unset_on_exit(unset_on_exit&&)                         = delete;
    auto operator=(unset_on_exit&&) -> unset_on_exit&      = delete;
    ~unset_on_exit() {
      ::unsetenv(k_name);
    }
  } const restore;

  scratch_dir const scratch;
  auto const        witness = scratch.path() / "witness";
  auto const        script  = scratch.path() / "child.sh";
  write_script(script, std::format("#!/bin/sh\n"
                                   "env | grep -c '^{}=' > '{}'\n"
                                   "printf '%s\\n' \"${}\" >> '{}'\n",
                                   k_name, witness.string(), k_name, witness.string()));

  std::vector<std::string> const argv{script.string()};
  runner::start_options const    options{.working_directory = std::nullopt, .env_name = k_name, .env_value = "added"};
  auto                           started = runner::start(map_env({}), argv, options);
  REQUIRE(started.has_value());
  child_guard guard{*started};

  auto const ended = guard.wait_for(k_bound);
  REQUIRE(ended.has_value());
  CHECK(ended->kind == runner::state::exited);
  CHECK(ended->code == 0);
  CHECK(slurp(witness) == "1\nadded\n");

  // A shell drops duplicate entries when it re-exports its environment, so
  // the raw environment is read by `env` itself, with no shell between it
  // and the runner. Its stdout is this process's, inherited: it is pointed
  // at a file for the duration, which also shows the stream is inherited
  // rather than captured. A second entry of the same name would be seen by
  // `getenv` in a C program first, so exactly one entry must arrive.
  auto const raw_witness = scratch.path() / "raw-env";
  int const  file        = ::open(raw_witness.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  REQUIRE(file >= 0);
  std::cout.flush();
  int const saved = ::dup(STDOUT_FILENO);
  REQUIRE(saved >= 0);
  REQUIRE(::dup2(file, STDOUT_FILENO) == STDOUT_FILENO);
  std::vector<std::string> const env_argv{"env"};
  auto                           raw = runner::start(map_env({{"PATH", "/usr/bin:/bin"}}), env_argv, options);
  ::dup2(saved, STDOUT_FILENO);
  ::close(saved);
  ::close(file);
  REQUIRE(raw.has_value());
  child_guard raw_guard{*raw};
  auto const  raw_end = raw_guard.wait_for(k_bound);
  REQUIRE(raw_end.has_value());
  CHECK(raw_end->kind == runner::state::exited);
  CHECK(raw_end->code == 0);

  auto const listing = slurp(raw_witness);
  REQUIRE_FALSE(listing.empty());
  std::vector<std::string> matches;
  for (auto const line : std::views::split(listing, '\n')) {
    std::string_view const entry{line.begin(), line.end()};
    if (entry.starts_with(std::format("{}=", k_name))) {
      matches.emplace_back(entry);
    }
  }
  CHECK(matches == std::vector<std::string>{std::format("{}=added", k_name)});
}

TEST_CASE("runner: a child killed by SIGKILL is reported as signalled with 9, not as an exit status", "[lib][process][runner]") {
  scratch_dir const scratch;
  auto const        script = scratch.path() / "child.sh";
  write_script(script, "#!/bin/sh\nkill -KILL $$\nexit 3\n");

  std::vector<std::string> const argv{script.string()};
  auto                           started = runner::start(map_env({}), argv, slot_options("1"));
  REQUIRE(started.has_value());
  child_guard guard{*started};

  auto const ended = guard.wait_for(k_bound);
  REQUIRE(ended.has_value());
  CHECK(ended->kind == runner::state::signalled);
  CHECK(ended->code == SIGKILL);
  CHECK(ended->code == 9);

  // The converse: a child that exits 137 by itself is an exit status, not
  // a signal.
  auto const exits = scratch.path() / "exits.sh";
  write_script(exits, "#!/bin/sh\nexit 137\n");
  std::vector<std::string> const exits_argv{exits.string()};
  auto                           second = runner::start(map_env({}), exits_argv, slot_options("2"));
  REQUIRE(second.has_value());
  child_guard second_guard{*second};
  auto const  second_end = second_guard.wait_for(k_bound);
  REQUIRE(second_end.has_value());
  CHECK(second_end->kind == runner::state::exited);
  CHECK(second_end->code == 137);
}

TEST_CASE("runner: a missing program and an unexecutable file are told apart", "[lib][process][runner]") {
  scratch_dir const scratch;
  auto const        first  = scratch.path() / "first";
  auto const        second = scratch.path() / "second";
  std::filesystem::create_directory(first);
  std::filesystem::create_directory(second);
  write_plain(first / "plain", "#!/bin/sh\nexit 0\n");
  write_plain(first / "tool", "#!/bin/sh\nexit 0\n");
  write_script(second / "tool", "#!/bin/sh\nexit 0\n");
  std::filesystem::create_directory(first / "dironly");

  auto const env = map_env({{"PATH", std::format("{}::{}", first.string(), second.string())}});

  SECTION("a bare name that matches nothing is not found") {
    auto const got = runner::resolve(env, "planar-no-such-program-7006");
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == runner::error::not_found);
  }
  SECTION("a path that names nothing is not found") {
    auto const got = runner::resolve(env, (scratch.path() / "absent").string());
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == runner::error::not_found);
  }
  SECTION("a bare name with no PATH is not found") {
    auto const got = runner::resolve(map_env({}), "tool");
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == runner::error::not_found);
  }
  SECTION("a path to a file without execute permission is not executable") {
    auto const got = runner::resolve(env, (first / "plain").string());
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == runner::error::not_executable);
  }
  SECTION("a bare name that matches only an unexecutable file is not executable") {
    auto const got = runner::resolve(env, "plain");
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == runner::error::not_executable);
  }
  SECTION("a directory is not executable, by path or by name") {
    auto const by_path = runner::resolve(env, (first / "dironly").string());
    REQUIRE_FALSE(by_path.has_value());
    CHECK(by_path.error() == runner::error::not_executable);
    auto const by_name = runner::resolve(env, "dironly");
    REQUIRE_FALSE(by_name.has_value());
    CHECK(by_name.error() == runner::error::not_executable);
  }
  SECTION("an unexecutable match earlier on PATH does not hide an executable one later") {
    auto const got = runner::resolve(env, "tool");
    REQUIRE(got.has_value());
    CHECK(*got == (second / "tool").string());
  }
  SECTION("an empty name is an empty command") {
    auto const got = runner::resolve(env, "");
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == runner::error::empty_command);
  }
  SECTION("start refuses both before forking") {
    std::vector<std::string> const missing{(scratch.path() / "absent").string()};
    auto const                     not_found = runner::start(env, missing, slot_options("1"));
    REQUIRE_FALSE(not_found.has_value());
    CHECK(not_found.error() == runner::error::not_found);

    std::vector<std::string> const plain{(first / "plain").string()};
    auto const                     not_executable = runner::start(env, plain, slot_options("1"));
    REQUIRE_FALSE(not_executable.has_value());
    CHECK(not_executable.error() == runner::error::not_executable);

    std::vector<std::string> const empty;
    auto const                     empty_command = runner::start(env, empty, slot_options("1"));
    REQUIRE_FALSE(empty_command.has_value());
    CHECK(empty_command.error() == runner::error::empty_command);

    CHECK_FALSE(has_unreaped_child());
  }
}

TEST_CASE("runner: an exec that fails after the fork is reported and the child is reaped", "[lib][process][runner]") {
  // Executable by permission but not by format: no `#!` line and not a
  // binary, so `resolve` accepts it and `execve` refuses it with ENOEXEC.
  scratch_dir const scratch;
  auto const        garbage = scratch.path() / "garbage";
  write_script(garbage, "this is not a program\n");

  std::vector<std::string> const argv{garbage.string()};
  auto const                     got = runner::start(map_env({}), argv, slot_options("1"));
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error() == runner::error::not_executable);
  CHECK_FALSE(has_unreaped_child());
}

TEST_CASE("runner: the child runs in the given working directory", "[lib][process][runner]") {
  scratch_dir const scratch;
  auto const        work    = scratch.path() / "work dir";
  auto const        witness = scratch.path() / "witness";
  auto const        script  = scratch.path() / "child.sh";
  std::filesystem::create_directory(work);
  write_script(script, std::format("#!/bin/sh\npwd -P > '{}'\n", witness.string()));

  std::vector<std::string> const argv{script.string()};
  auto                           options = slot_options("1");
  options.working_directory              = work;
  auto started                           = runner::start(map_env({}), argv, options);
  REQUIRE(started.has_value());
  child_guard guard{*started};

  auto const ended = guard.wait_for(k_bound);
  REQUIRE(ended.has_value());
  CHECK(ended->kind == runner::state::exited);
  CHECK(ended->code == 0);
  CHECK(slurp(witness) == std::format("{}\n", std::filesystem::canonical(work).string()));

  // A directory that does not exist is the module's error, and no child is
  // left behind.
  auto missing_dir              = slot_options("1");
  missing_dir.working_directory = scratch.path() / "absent";
  auto const refused            = runner::start(map_env({}), argv, missing_dir);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == runner::error::working_directory_failed);
  CHECK_FALSE(has_unreaped_child());
}

TEST_CASE("runner: poll reports running without blocking while the child waits on a pipe", "[lib][process][runner]") {
  scratch_dir const scratch;
  fifo_gate         gate{scratch.path() / "gate"};
  auto const        script = scratch.path() / "child.sh";
  write_script(script, std::format("#!/bin/sh\nread x < '{}'\nexit 3\n", gate.path().string()));

  std::vector<std::string> const argv{script.string()};
  auto                           started = runner::start(map_env({}), argv, slot_options("1"));
  REQUIRE(started.has_value());
  child_guard guard{*started};

  // A watchdog releases the child after three seconds, so a `poll` that
  // blocks returns late and fails the timing check instead of hanging.
  std::jthread const watchdog{[&gate](const std::stop_token& stop) {
    std::mutex                  mutex;
    std::condition_variable_any wake;
    std::unique_lock            lock{mutex};
    if (!wake.wait_for(lock, stop, std::chrono::seconds{3}, [] { return false; })) {
      if (!stop.stop_requested()) {
        gate.release();
      }
    }
  }};

  auto const before  = std::chrono::steady_clock::now();
  auto const first   = runner::poll(guard.get());
  auto const elapsed = std::chrono::steady_clock::now() - before;
  REQUIRE(first.has_value());
  CHECK(first->kind == runner::state::running);
  CHECK(first->code == 0);
  CHECK(elapsed < std::chrono::seconds{1});

  gate.release();
  auto const ended = guard.wait_for(k_bound);
  REQUIRE(ended.has_value());
  CHECK(ended->kind == runner::state::exited);
  CHECK(ended->code == 3);

  // Reaped by the poll that reported it: a further poll is an error.
  auto const again = runner::poll(guard.get());
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error() == runner::error::wait_failed);
}

TEST_CASE("runner: signalling the group reaches a grandchild in the same group", "[lib][process][runner]") {
  scratch_dir const scratch;
  fifo_gate         gate{scratch.path() / "gate"};
  auto const        witness = scratch.path() / "grandchild";
  auto const        script  = scratch.path() / "child.sh";
  write_script(script, std::format("#!/bin/sh\n"
                                   "read x < '{0}' &\n"
                                   "echo $! > '{1}.tmp'\n"
                                   "mv '{1}.tmp' '{1}'\n"
                                   "read y < '{0}'\n",
                                   gate.path().string(), witness.string()));

  std::vector<std::string> const argv{script.string()};
  auto                           started = runner::start(map_env({}), argv, slot_options("1"));
  REQUIRE(started.has_value());
  child_guard guard{*started};
  auto const  pgid = guard.get().pgid;

  // Never signal a group this test cannot prove is the child's own.
  REQUIRE(::getpgid(static_cast<::pid_t>(guard.get().pid)) == static_cast<::pid_t>(pgid));
  REQUIRE(pgid != ::getpgrp());

  auto const deadline = std::chrono::steady_clock::now() + k_bound;
  while (!std::filesystem::exists(witness) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  auto const recorded = slurp(witness);
  REQUIRE_FALSE(recorded.empty());
  auto const grandchild = std::stoll(recorded);
  REQUIRE(grandchild != guard.get().pid);
  REQUIRE(::getpgid(static_cast<::pid_t>(grandchild)) == static_cast<::pid_t>(pgid));

  auto const sent = runner::signal(guard.get(), SIGTERM);
  REQUIRE(sent.has_value());

  auto const ended = guard.wait_for(k_bound);
  REQUIRE(ended.has_value());
  CHECK(ended->kind == runner::state::signalled);
  CHECK(ended->code == SIGTERM);

  // The grandchild is not this process's child, so it is reaped by
  // whichever process adopted it; the group empties once it has died.
  auto emptied = false;
  while (std::chrono::steady_clock::now() < deadline) {
    auto const members = planar::process::identity::group_has_members(pgid);
    REQUIRE(members.has_value());
    if (!*members) {
      emptied = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK(emptied);
  if (!emptied) {
    // Let the survivor read its line and exit.
    gate.release(2);
  }

  // The group is gone, so a further signal names no process. Signal 0
  // delivers nothing even if the id were reused.
  if (emptied) {
    auto const late = runner::signal(guard.get(), 0);
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error() == runner::error::no_such_process);
  }
}

TEST_CASE("runner: an added variable name that cannot be set is refused before forking", "[lib][process][runner]") {
  scratch_dir const scratch;
  auto const        script = scratch.path() / "child.sh";
  write_script(script, "#!/bin/sh\nexit 0\n");
  std::vector<std::string> const argv{script.string()};

  for (std::string const name : {"", "A=B"}) {
    CAPTURE(name);
    runner::start_options const options{.working_directory = std::nullopt, .env_name = name, .env_value = "x"};
    auto const                  got = runner::start(map_env({}), argv, options);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == runner::error::invalid_environment);
  }
  CHECK_FALSE(has_unreaped_child());
}
