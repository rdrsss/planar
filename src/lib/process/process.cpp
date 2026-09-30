/// @file process.cpp
/// @brief Implementation of `planar.process`. See the module interface for
/// why the two shapes are not interchangeable and why the
/// spawn-failure/non-zero-exit distinction is load-bearing.

module;

#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#endif
#if defined(__linux__)
#include <dirent.h>
#include <sys/syscall.h>
#endif

module planar.process;

import std;

/// @brief The process environment, for `posix_spawnp`. Declared rather than
/// included because no portable header exposes it consistently.
extern "C" char** environ; // NOLINT(readability-redundant-declaration)

namespace planar::process {

auto resolve_program(const env_lookup& env, std::string_view program) -> std::optional<std::string> {
  if (program.empty()) {
    return std::nullopt;
  }
  auto const executable = [](const std::string& candidate) { return ::access(candidate.c_str(), X_OK) == 0; };

  if (program.contains('/')) {
    std::string direct{program};
    return executable(direct) ? std::optional{direct} : std::nullopt;
  }

  auto const path = env("PATH");
  if (!path.has_value() || path->empty()) {
    return std::nullopt;
  }
  for (auto const part : std::views::split(*path, ':')) {
    std::string_view const dir{part.begin(), part.end()};
    if (dir.empty()) {
      continue;
    }
    auto candidate = std::format("{}/{}", dir, program);
    if (executable(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

auto run_inherited(const env_lookup& env, std::span<const std::string> argv) -> std::optional<int> {
  if (argv.empty()) {
    return std::nullopt;
  }
  auto const resolved = resolve_program(env, argv[0]);
  if (!resolved.has_value()) {
    return std::nullopt;
  }

  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  raw.push_back(const_cast<char*>(resolved->c_str()));
  for (auto const& arg : argv.subspan(1)) {
    raw.push_back(const_cast<char*>(arg.c_str()));
  }
  raw.push_back(nullptr);

  // stdout is flushed before the fork: the child INHERITS this process's
  // descriptors, so anything still sitting in this process's buffers would
  // otherwise be duplicated into the child and written twice.
  std::cout.flush();
  std::cerr.flush();

  pid_t const pid = ::fork();
  if (pid < 0) {
    return std::nullopt;
  }
  if (pid == 0) {
    // Child. `execv` is async-signal-safe; nothing else happens here.
    // stdin/stdout/stderr are left alone, which is the whole point — a
    // full-screen editor, and a workflow streaming its result JSON, both
    // need the real terminal.
    ::execv(raw[0], raw.data());
    ::_exit(127);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return std::nullopt;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  // Killed by a signal. The Zig originals map every non-`.exited`
  // termination to 1 — `editflow` reads this as "abort without writing" and
  // `workflow run` propagates it as the verb's own exit code.
  return 1;
}

auto capture(std::string_view program, std::span<const std::string_view> args) -> capture_result {
  std::array<int, 2> fds{-1, -1};
  if (::pipe(fds.data()) != 0) {
    return {};
  }
  int const read_fd  = fds[0];
  int const write_fd = fds[1];

  // argv must be NUL-terminated C strings that outlive the spawn call, so
  // the views are copied into owned storage first.
  std::vector<std::string> owned;
  owned.reserve(args.size() + 1);
  owned.emplace_back(program);
  for (auto const& arg : args) {
    owned.emplace_back(arg);
  }
  std::vector<char*> argv;
  argv.reserve(owned.size() + 1);
  for (auto& entry : owned) {
    argv.push_back(entry.data());
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions{};
  if (::posix_spawn_file_actions_init(&actions) != 0) {
    ::close(read_fd);
    ::close(write_fd);
    return {};
  }
  // stdout -> the pipe; stderr -> /dev/null. The oracle captures the
  // child's stderr into a buffer it frees, so a failing `gh` must not
  // reach the operator's terminal.
  ::posix_spawn_file_actions_addclose(&actions, read_fd);
  ::posix_spawn_file_actions_adddup2(&actions, write_fd, STDOUT_FILENO);
  ::posix_spawn_file_actions_addclose(&actions, write_fd);
  ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  ::pid_t   pid = 0;
  int const rc  = ::posix_spawnp(&pid, owned.front().c_str(), &actions, nullptr, argv.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  ::close(write_fd);

  if (rc != 0) {
    // ENOENT and friends land HERE rather than as an exit status, which is
    // the whole reason this is posix_spawnp and not popen. Measured on this
    // platform: a missing program gives rc=2/ENOENT and leaves `pid` at 0.
    //
    // DO NOT remove this early return on the grounds that the code below
    // "would return the same thing anyway". It does, for the wrong reason —
    // `waitpid(0, ...)` with no children returns ECHILD, which also maps to
    // `spawned = false` — and that equivalence holds ONLY while the process
    // has no other children. In a process that does (this binary's own test
    // suite, once it spawns anything else), `waitpid(0, ...)` waits on ANY
    // child in the caller's process group and would report an unrelated
    // child's exit status as this command's. A break-probe against this
    // line SURVIVES for exactly that reason; see process.t.cpp's header.
    ::close(read_fd);
    return {};
  }

  std::string            output;
  std::array<char, 4096> buffer{};
  for (;;) {
    auto const got = ::read(read_fd, buffer.data(), buffer.size());
    if (got > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(got));
      continue;
    }
    if (got < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  ::close(read_fd);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return {};
    }
  }

  int exit_code = 1;
  if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
  }
  return {.spawned = true, .exit_code = exit_code, .output = std::move(output)};
}

auto own_thread_count() -> std::optional<std::size_t> {
#if defined(__APPLE__)
  proc_taskinfo info{};
  auto const    got = ::proc_pidinfo(::getpid(), PROC_PIDTASKINFO, 0, &info, static_cast<int>(sizeof info));
  if (got != static_cast<int>(sizeof info) || info.pti_threadnum < 1) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(info.pti_threadnum);
#elif defined(__linux__)
  std::ifstream in{"/proc/self/status"};
  std::string   line;
  while (std::getline(in, line)) {
    constexpr std::string_view key = "Threads:";
    if (!line.starts_with(key)) {
      continue;
    }
    auto const start = line.find_first_of("0123456789", key.size());
    if (start == std::string::npos) {
      return std::nullopt;
    }
    std::size_t value = 0;
    if (std::from_chars(line.data() + start, line.data() + line.size(), value).ec != std::errc{} || value < 1) {
      return std::nullopt;
    }
    return value;
  }
  return std::nullopt;
#else
  return std::nullopt;
#endif
}

namespace {

/// @brief Closes every number from 3 up to the open-file limit but `keep`.
/// The last resort, used only when the open descriptors cannot be listed.
void close_by_limit(int keep) {
  auto const limit = ::sysconf(_SC_OPEN_MAX);
  for (long fd = 3; fd < (limit < 0 ? 65536 : limit); ++fd) {
    if (fd != keep) {
      ::close(static_cast<int>(fd));
    }
  }
}

} // namespace

void close_descriptors_except(int keep) {
#if defined(__APPLE__)
  // The list is a snapshot taken before any close; closing a number in it
  // cannot open another, and the buffer is a vector, which opens nothing.
  auto bytes = ::proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
  if (bytes <= 0) {
    close_by_limit(keep);
    return;
  }
  std::vector<proc_fdinfo> table;
  int                      got      = 0;
  bool                     complete = false;
  for (int attempt = 0; attempt < 8; ++attempt) {
    table.resize(static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo) + 32);
    auto const capacity = static_cast<int>(table.size() * sizeof(proc_fdinfo));
    got                 = ::proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0, table.data(), capacity);
    if (got <= 0) {
      close_by_limit(keep);
      return;
    }
    if (got < capacity) {
      complete = true;
      break;
    }
    bytes = capacity * 2; // Filled to the brim: there may be more.
  }
  if (!complete) {
    // The last read was still full, so the table may be missing descriptors
    // (the process keeps opening them faster than the list can be read).
    // Closing a partial list would leave the rest open; fall back to the
    // exhaustive sweep instead.
    close_by_limit(keep);
    return;
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(got) / sizeof(proc_fdinfo); ++i) {
    auto const fd = table[i].proc_fd;
    if (fd >= 3 && fd != keep) {
      ::close(fd);
    }
  }
#elif defined(__linux__)
#if defined(SYS_close_range)
  constexpr unsigned k_last = std::numeric_limits<unsigned>::max();
  bool               closed = true;
  if (keep > 3) {
    closed = ::syscall(SYS_close_range, 3U, static_cast<unsigned>(keep - 1), 0U) == 0;
  }
  if (closed) {
    auto const first = static_cast<unsigned>(keep >= 3 ? keep + 1 : 3);
    closed           = ::syscall(SYS_close_range, first, k_last, 0U) == 0;
  }
  if (closed) {
    return;
  }
#endif
  // No close_range (an old kernel, or a filter that refuses it): list
  // /proc/self/fd, then close what was listed. The directory's own descriptor
  // is in the list and is closed with the rest.
  std::vector<int> found;
  if (auto* dir = ::opendir("/proc/self/fd")) {
    while (auto const* item = ::readdir(dir)) {
      int        value = -1;
      auto const name  = std::string_view{item->d_name};
      if (std::from_chars(name.data(), name.data() + name.size(), value).ec == std::errc{} && value >= 3 && value != keep) {
        found.push_back(value);
      }
    }
    ::closedir(dir);
    for (auto const fd : found) {
      ::close(fd);
    }
    return;
  }
  close_by_limit(keep);
#else
  close_by_limit(keep);
#endif
}

} // namespace planar::process
