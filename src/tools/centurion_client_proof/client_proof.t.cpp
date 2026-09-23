/// @file client_proof.t.cpp
/// @brief Toolchain proof for Centurion's client under Planar's build (plan
///        1033 M1, task 6497; tech-spec D9).
///
/// The claim: `centurion::client` — a C++23 module library — compiles and
/// links inside Planar's C++26 build under Planar's pinned LLVM, and the
/// resulting binary talks to a stock, INSTALLED `centuriond` over its
/// owner-only Unix socket. That is the whole toolchain proof; no library-only
/// runtime is compiled into Planar.
///
/// The case starts the daemon named by `PLANAR_CENTURIOND` (the Makefile's
/// `centurion-client-proof` target installs one and exports it) under a
/// scratch HOME, waits for its socket, and completes readiness the way
/// Centurion's consumer guide prescribes: `probe_bundle_capability`, a bounded
/// `ListWorkflowBundles`, because the Unix listener routes no health check.
/// A missing or non-executable daemon FAILS the case; it never skips.

import std;
import centurion.client;

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

/// @brief A scratch HOME short enough for a Unix socket path, removed on exit.
///
/// macOS caps `sun_path` at 104 bytes, and the socket lands at
/// `<home>/.centurion/runtime/centuriond-v1.sock`; `$TMPDIR` on macOS is
/// already ~50 bytes, so the root is `/tmp`, not `temp_directory_path()`.
struct scratch_home {
  std::filesystem::path path;

  scratch_home() {
    std::string pattern = "/tmp/pcp.XXXXXX";
    char*       made    = ::mkdtemp(pattern.data());
    REQUIRE(made != nullptr);
    path = made;
  }
  scratch_home(const scratch_home&)                    = delete;
  auto operator=(const scratch_home&) -> scratch_home& = delete;
  ~scratch_home() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

/// @brief A daemon child that is terminated and reaped when the case ends.
struct daemon_process {
  pid_t pid = -1;

  daemon_process(const std::filesystem::path& bin, const std::filesystem::path& home, const std::filesystem::path& log) {
    std::vector<std::string> env_storage;
    for (char** e = environ; *e != nullptr; ++e) {
      std::string_view entry{*e};
      if (!entry.starts_with("HOME=")) {
        env_storage.emplace_back(entry);
      }
    }
    env_storage.push_back("HOME=" + home.string());

    // NOT --no-listen: that flag disables the whole transport phase, the
    // owner-only Unix listener included, so the daemon comes up healthy and
    // never creates a socket. The loopback TCP listener comes with it, so it
    // is pinned to an unusual port rather than the default, where it could
    // collide with a centuriond the operator is actually running.
    std::vector<std::string> arg_storage{bin.string(), "--database", (home / "state" / "centurion.db").string(), "--listen-port",
                                         "47993"};
    std::vector<char*>       argv;
    for (auto& a : arg_storage) {
      argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (auto& e : env_storage) {
      envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    REQUIRE(::posix_spawn_file_actions_init(&actions) == 0);
    std::string const log_path = log.string();
    REQUIRE(::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600) ==
            0);
    REQUIRE(::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO) == 0);
    int const rc = ::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&actions);
    REQUIRE(rc == 0);
  }
  daemon_process(const daemon_process&)                    = delete;
  auto operator=(const daemon_process&) -> daemon_process& = delete;

  /// @brief Whether the child has already exited (reaping it if so).
  [[nodiscard]] auto exited() -> bool {
    if (pid <= 0) {
      return true;
    }
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      pid = -1;
      return true;
    }
    return false;
  }

  ~daemon_process() {
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      int status = 0;
      ::waitpid(pid, &status, 0);
    }
  }
};

auto read_file(const std::filesystem::path& p) -> std::string {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

/// @brief Link the client under Planar's toolchain, start the installed
///        centuriond, and complete readiness over its Unix socket.
TEST_CASE("centurion::client reaches readiness against the installed centuriond", "[centurion][toolchain]") {
  char const* configured = std::getenv("PLANAR_CENTURIOND");
  INFO("PLANAR_CENTURIOND must name an installed centuriond (make centurion-client-proof sets it)");
  REQUIRE(configured != nullptr);
  std::filesystem::path const bin{configured};
  REQUIRE(::access(bin.c_str(), X_OK) == 0);

  scratch_home home;
  std::filesystem::create_directories(home.path / "state");
  auto const     log    = home.path / "centuriond.log";
  auto const     socket = home.path / ".centurion" / "runtime" / "centuriond-v1.sock";
  daemon_process daemon(bin, home.path, log);

  // Wait for the listener's socket, failing fast if the daemon dies first.
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
  while (!std::filesystem::exists(socket)) {
    if (daemon.exited()) {
      INFO("centuriond log:\n" << read_file(log));
      FAIL("centuriond exited before creating its Unix socket");
    }
    if (std::chrono::steady_clock::now() > deadline) {
      INFO("centuriond log:\n" << read_file(log));
      FAIL("centuriond created no Unix socket within 60s");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }

  centurion::client::endpoint const target{.target_   = std::format("unix:{}", socket.string()),
                                           .deadline_ = std::chrono::seconds{5}};
  // The socket file can exist a moment before the listener accepts; retry a
  // transport failure for a bounded time, never a completed refusal.
  std::expected<centurion::client::capability_readiness, centurion::client::error> readiness =
      centurion::client::probe_bundle_capability(target);
  while (!readiness && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    readiness = centurion::client::probe_bundle_capability(target);
  }
  INFO("centuriond log:\n" << read_file(log));
  INFO("probe error: " << (readiness ? std::string{} : readiness.error().message_));
  REQUIRE(readiness.has_value());
  CHECK(*readiness == centurion::client::capability_readiness::ready);
  REQUIRE_FALSE(daemon.exited());
}
