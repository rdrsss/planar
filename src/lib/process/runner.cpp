/// @file runner.cpp
/// @brief Implementation of `planar.process.runner`. The contract lives on
/// the interface; this unit is where the C calls are made and where every
/// errno is turned into an `error` before it can escape.
///
/// `start` forks once and hands the child two close-on-exec pipes:
///
///   * a **go** pipe the child reads before it does anything the caller can
///     observe. While the child is held there, the parent places it in its
///     group and reads its start time, so the start time is read from a
///     child that is alive and not yet reaped. Closing the write end
///     releases it; a failure on the parent's side kills it instead.
///   * a **report** pipe the child writes `{stage, errno}` to when changing
///     directory or `exec` fails. The write end closes on a successful
///     `exec`, so end of file with nothing read means the program is
///     running.
///
/// Between `fork` and `exec` the child makes only async-signal-safe calls,
/// on data prepared before the fork.

module;

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

module planar.process.runner;

import std;

/// @brief The process environment the child inherits. Declared rather than
/// included because no portable header exposes it consistently.
extern "C" char** environ; // NOLINT(readability-redundant-declaration)

namespace planar::process::runner {

namespace {

/// @brief What examining one candidate path found.
enum class candidate_verdict : std::uint8_t { executable, not_executable, absent };

/// @brief Examine `path` the way `execve` would judge it.
///
/// A path that does not exist, or whose prefix is not a directory, is
/// absent. One that exists but is not a regular file the caller may
/// execute, or that cannot be examined for another reason (a directory on
/// the way that may not be searched), is not executable.
auto examine(const std::string& path) -> candidate_verdict {
  struct ::stat info{};
  if (::stat(path.c_str(), &info) != 0) {
    return (errno == ENOENT || errno == ENOTDIR) ? candidate_verdict::absent : candidate_verdict::not_executable;
  }
  if (!S_ISREG(info.st_mode) || ::access(path.c_str(), X_OK) != 0) {
    return candidate_verdict::not_executable;
  }
  return candidate_verdict::executable;
}

/// @brief Where in the child a failure after the fork happened.
enum class child_stage : int { group = 1, directory = 2, exec = 3 };

/// @brief What the child writes to the report pipe when it cannot run the
/// program.
struct child_report {
  int stage = 0; ///< A `child_stage`.
  int err   = 0; ///< The errno the failing call left.
};

/// @brief Whether `id` is a positive value a `pid_t` holds without
/// narrowing. `waitpid` with 0 or a negative id would wait on children this
/// module did not start.
auto is_pid(std::int64_t id) -> bool {
  return id > 0 && id <= static_cast<std::int64_t>(std::numeric_limits<::pid_t>::max());
}

/// @brief Close both ends of a pipe that may be partly open.
void close_pair(std::array<int, 2>& fds) {
  for (int& fd : fds) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
}

/// @brief Open a pipe with both ends close-on-exec.
///
/// On Linux `pipe2(O_CLOEXEC)` makes this atomic. Elsewhere `pipe` followed
/// by `fcntl` leaves a window in which another thread's `fork` and `exec`
/// would carry the descriptors into its child; `start` therefore requires
/// that no other thread forks and execs concurrently.
auto open_pipe(std::array<int, 2>& fds) -> bool {
#if defined(__linux__)
  if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
    fds = {-1, -1};
    return false;
  }
  return true;
#else
  if (::pipe(fds.data()) != 0) {
    fds = {-1, -1};
    return false;
  }
  for (int const fd : fds) {
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
      close_pair(fds);
      return false;
    }
  }
  return true;
#endif
}

/// @brief Wait for `pid` to terminate, retrying an interrupted wait.
void reap(::pid_t pid) {
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
}

/// @brief Kill a child that was never released, and reap it.
void abandon(::pid_t pid) {
  ::kill(pid, SIGKILL);
  reap(pid);
}

/// @brief The error an `exec` failure maps to.
auto exec_error(int err) -> error {
  switch (err) {
  case ENOENT:
  case ENOTDIR:
    return error::not_found;
  case EACCES:
  case EPERM:
  case ENOEXEC:
  case EISDIR:
    return error::not_executable;
  default:
    return error::spawn_failed;
  }
}

/// @brief Write the child's failure to the report pipe and exit. Runs
/// between `fork` and `exec`, so only async-signal-safe calls are made.
[[noreturn]] void child_fail(int report_fd, child_stage stage, int err) {
  child_report const report{.stage = static_cast<int>(stage), .err = err};
  while (::write(report_fd, &report, sizeof report) < 0 && errno == EINTR) {
  }
  ::_exit(127);
}

/// @brief Reset every caught signal to its default, then unblock every
/// signal, so the child acts on a signal the way the program it becomes
/// would. Ignored signals stay ignored, as `exec` itself would leave them.
/// `exec` already resets caught handlers; the mask is what it would not
/// clear, so a caller that blocked a signal would otherwise hand the
/// program a signal it cannot receive. Async-signal-safe.
void reset_signals() {
  for (int sig = 1; sig < NSIG; ++sig) {
    struct ::sigaction current{};
    if (::sigaction(sig, nullptr, &current) != 0) {
      continue;
    }
    if (current.sa_handler == SIG_IGN || current.sa_handler == SIG_DFL) {
      continue;
    }
    struct ::sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    ::sigaction(sig, &fallback, nullptr);
  }
  ::sigset_t none{};
  sigemptyset(&none);
  ::sigprocmask(SIG_SETMASK, &none, nullptr);
}

} // namespace

auto resolve(const env_lookup& env, std::string_view program) -> std::expected<std::string, error> {
  if (program.empty()) {
    return std::unexpected{error::empty_command};
  }

  if (program.contains('/')) {
    std::string direct{program};
    switch (examine(direct)) {
    case candidate_verdict::executable:
      return direct;
    case candidate_verdict::not_executable:
      return std::unexpected{error::not_executable};
    case candidate_verdict::absent:
      break;
    }
    return std::unexpected{error::not_found};
  }

  auto const path = env ? env("PATH") : std::nullopt;
  if (!path.has_value() || path->empty()) {
    return std::unexpected{error::not_found};
  }
  // As `execvp` does, a match that cannot be executed does not end the
  // search, but is remembered so that no executable match anywhere reads
  // as "not executable" rather than "not found".
  bool saw_unexecutable = false;
  for (auto const part : std::views::split(*path, ':')) {
    std::string_view const dir{part.begin(), part.end()};
    if (dir.empty()) {
      continue;
    }
    auto candidate = std::format("{}/{}", dir, program);
    switch (examine(candidate)) {
    case candidate_verdict::executable:
      return candidate;
    case candidate_verdict::not_executable:
      saw_unexecutable = true;
      break;
    case candidate_verdict::absent:
      break;
    }
  }
  return std::unexpected{saw_unexecutable ? error::not_executable : error::not_found};
}

auto start(const env_lookup& env, std::span<const std::string> argv, const start_options& options)
    -> std::expected<child, error> {
  if (argv.empty() || argv[0].empty()) {
    return std::unexpected{error::empty_command};
  }
  if (options.env_name.empty() || options.env_name.contains('=')) {
    return std::unexpected{error::invalid_environment};
  }
  auto const resolved = resolve(env, argv[0]);
  if (!resolved.has_value()) {
    return std::unexpected{resolved.error()};
  }

  // Everything the child touches is built before the fork.
  std::vector<char*> raw_argv;
  raw_argv.reserve(argv.size() + 1);
  for (auto const& arg : argv) {
    raw_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  raw_argv.push_back(nullptr);

  auto const         prefix = std::format("{}=", options.env_name);
  std::string        added  = prefix + options.env_value;
  std::vector<char*> raw_env;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    if (!std::string_view{*entry}.starts_with(prefix)) {
      raw_env.push_back(*entry);
    }
  }
  raw_env.push_back(added.data());
  raw_env.push_back(nullptr);

  std::optional<std::string> const directory =
      options.working_directory.has_value() ? std::optional{options.working_directory->string()} : std::nullopt;

  std::array<int, 2> go{-1, -1};
  std::array<int, 2> report{-1, -1};
  if (!open_pipe(go)) {
    return std::unexpected{error::spawn_failed};
  }
  if (!open_pipe(report)) {
    close_pair(go);
    return std::unexpected{error::spawn_failed};
  }

  // The child inherits this process's standard streams, so anything still
  // buffered here would otherwise be written twice.
  std::cout.flush();
  std::cerr.flush();

  // Block every signal across the fork so the caller's handlers cannot run
  // in the child, in the caller's group, before `reset_signals`. The parent
  // restores its own mask straight after.
  ::sigset_t all{};
  ::sigset_t caller_mask{};
  sigfillset(&all);
  ::pthread_sigmask(SIG_SETMASK, &all, &caller_mask);
  ::pid_t const pid        = ::fork();
  int const     fork_errno = errno;
  if (pid != 0) {
    ::pthread_sigmask(SIG_SETMASK, &caller_mask, nullptr);
  }
  errno = fork_errno;
  if (pid < 0) {
    close_pair(go);
    close_pair(report);
    return std::unexpected{error::spawn_failed};
  }
  if (pid == 0) {
    ::close(go[1]);
    ::close(report[0]);
    // Handlers first, so nothing the caller installed runs once the mask
    // opens; the mask is still the all-blocked one the parent set.
    reset_signals();
    if (::setpgid(0, 0) != 0) {
      child_fail(report[1], child_stage::group, errno);
    }
    char byte = 0;
    while (::read(go[0], &byte, 1) < 0 && errno == EINTR) {
    }
    ::close(go[0]);
    if (directory.has_value() && ::chdir(directory->c_str()) != 0) {
      child_fail(report[1], child_stage::directory, errno);
    }
    ::execve(resolved->c_str(), raw_argv.data(), raw_env.data());
    child_fail(report[1], child_stage::exec, errno);
  }

  ::close(go[0]);
  ::close(report[1]);

  // The parent places the child too, so no caller ever sees it in this
  // process's group, whichever of the two calls runs first.
  if (::setpgid(pid, pid) != 0 && ::getpgid(pid) != pid) {
    ::close(go[1]);
    ::close(report[0]);
    abandon(pid);
    return std::unexpected{error::spawn_failed};
  }

  auto const started = identity::process_start_time(pid);
  if (!started.has_value() || !started->has_value()) {
    ::close(go[1]);
    ::close(report[0]);
    abandon(pid);
    return std::unexpected{error::start_time_failed};
  }

  // Release the child.
  ::close(go[1]);

  child_report failure{};
  std::size_t  got = 0;
  while (got < sizeof failure) {
    auto const n = ::read(report[0], reinterpret_cast<char*>(&failure) + got, sizeof failure - got);
    if (n > 0) {
      got += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  ::close(report[0]);

  if (got == 0) {
    return child{.pid = pid, .pgid = pid, .started = **started};
  }

  // The child wrote its failure and is exiting.
  reap(pid);
  if (got != sizeof failure) {
    return std::unexpected{error::spawn_failed};
  }
  switch (static_cast<child_stage>(failure.stage)) {
  case child_stage::directory:
    return std::unexpected{error::working_directory_failed};
  case child_stage::exec:
    return std::unexpected{exec_error(failure.err)};
  case child_stage::group:
    break;
  }
  return std::unexpected{error::spawn_failed};
}

auto poll(const child& target) -> std::expected<status, error> {
  if (!is_pid(target.pid)) {
    return std::unexpected{error::wait_failed};
  }
  auto const pid = static_cast<::pid_t>(target.pid);
  int        raw = 0;
  for (;;) {
    auto const rc = ::waitpid(pid, &raw, WNOHANG);
    if (rc == 0) {
      return status{.kind = state::running, .code = 0};
    }
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected{error::wait_failed};
    }
    break;
  }
  if (WIFEXITED(raw)) {
    return status{.kind = state::exited, .code = WEXITSTATUS(raw)};
  }
  if (WIFSIGNALED(raw)) {
    return status{.kind = state::signalled, .code = WTERMSIG(raw)};
  }
  // Stopped and continued children are reported only with WUNTRACED or
  // WCONTINUED, which this call does not pass.
  return status{.kind = state::running, .code = 0};
}

auto signal(const child& target, int sig) -> std::expected<void, error> {
  auto const sent = identity::signal_group(target.pgid, sig);
  if (sent.has_value()) {
    return {};
  }
  switch (sent.error()) {
  case identity::error::no_such_process:
    return std::unexpected{error::no_such_process};
  case identity::error::not_permitted:
    return std::unexpected{error::not_permitted};
  case identity::error::invalid_signal:
    return std::unexpected{error::invalid_signal};
  case identity::error::query_failed:
  case identity::error::clock_failed:
    break;
  }
  return std::unexpected{error::signal_failed};
}

} // namespace planar::process::runner
