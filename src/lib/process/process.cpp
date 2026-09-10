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

} // namespace planar::process
